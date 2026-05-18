#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <thread>

#include <doca_error.h>
#include <doca_mmap.h>

#include "spdk_backend_qpair_export.h"

extern "C" void doorbell_polling_thread(
    volatile uint32_t *dummy_db,
    volatile uint32_t *real_db,
    const volatile int *stop_requested);

namespace {

int configure_mmap(struct doca_mmap **mmap, void *addr, size_t len, const char *label)
{
    doca_error_t result = doca_mmap_create(mmap);
    if (result != DOCA_SUCCESS) {
        std::fprintf(stderr, "Failed to create %s mmap: %s\n", label, doca_error_get_descr(result));
        return -1;
    }

    result = doca_mmap_set_memrange(*mmap, addr, len);
    if (result != DOCA_SUCCESS) {
        std::fprintf(stderr, "Failed to set %s mmap memrange: %s\n", label, doca_error_get_descr(result));
        doca_mmap_destroy(*mmap);
        *mmap = nullptr;
        return -1;
    }

    result = doca_mmap_start(*mmap);
    if (result != DOCA_SUCCESS) {
        std::fprintf(stderr, "Failed to start %s mmap: %s\n", label, doca_error_get_descr(result));
        doca_mmap_destroy(*mmap);
        *mmap = nullptr;
        return -1;
    }

    return 0;
}

} // namespace

int main(int argc, char **argv)
{
    struct spdk_doca_queue_resources qres = {};
    void *backend = nullptr;
    struct doca_mmap *mmap_sq = nullptr;
    struct doca_mmap *mmap_cq = nullptr;
    struct doca_mmap *mmap_db = nullptr;
    volatile int stop_requested = 0;
    const char *bdf = "0000:3b:00.0";
    int rc = 1;

    if (argc > 1)
        bdf = argv[1];

    if (spdk_backend_init_env("sta_host", -1) != 0) {
        std::fprintf(stderr, "Failed to initialize SPDK environment\n");
        return 1;
    }

    if (spdk_backend_qpair_export(bdf, &qres, &backend) != 0) {
        std::fprintf(stderr, "Failed to export SPDK qpair resources for BDF %s\n", bdf);
        return 1;
    }

    if (configure_mmap(&mmap_sq, qres.sq_addr, qres.sq_len, "SQ") != 0)
        goto cleanup;
    if (configure_mmap(&mmap_cq, qres.cq_addr, qres.cq_len, "CQ") != 0)
        goto cleanup;
    if (configure_mmap(&mmap_db, qres.dummy_db_page_addr, qres.dummy_db_page_len, "doorbell") != 0)
        goto cleanup;

    {
        std::thread polling_thread(
            doorbell_polling_thread,
            static_cast<volatile uint32_t *>(qres.dummy_db_page_addr),
            qres.real_sq_db,
            &stop_requested);

        std::puts("STA host integration initialized. Press Enter to stop.");
        (void)std::getchar();
        stop_requested = 1;
        polling_thread.join();
    }

    rc = 0;

cleanup:
    if (mmap_db != nullptr)
        doca_mmap_destroy(mmap_db);
    if (mmap_cq != nullptr)
        doca_mmap_destroy(mmap_cq);
    if (mmap_sq != nullptr)
        doca_mmap_destroy(mmap_sq);
    spdk_backend_close(backend);
    return rc;
}
