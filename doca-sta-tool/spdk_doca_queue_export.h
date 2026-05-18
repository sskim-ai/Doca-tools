#ifndef SPDK_DOCA_QUEUE_EXPORT_H
#define SPDK_DOCA_QUEUE_EXPORT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct spdk_nvme_qpair;

/* SPDK → DOCA export result */
struct spdk_doca_queue_resources {
    void *sq_addr;
    size_t sq_len;

    void *cq_addr;
    size_t cq_len;

    /* fallback doorbell buffer (RAM) */
    void *dummy_db_page_addr;
    size_t dummy_db_page_len;

    /* real NVMe doorbell (SPDK/VFIO internal MMIO) */
    volatile uint32_t *real_sq_db;
    volatile uint32_t *real_cq_db;

    uint16_t sq_db_offset;
    uint16_t cq_db_offset;

    uint16_t qid;
    uint16_t num_entries;
};

int spdk_nvme_qpair_export_doca_resources(
    struct spdk_nvme_qpair *qpair,
    uint16_t queue_depth,
    struct spdk_doca_queue_resources *out
);

#ifdef __cplusplus
}
#endif

#endif
