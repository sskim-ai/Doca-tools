#include "spdk_backend_qpair_export.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "spdk/env.h"
#include "spdk/nvme.h"

static int g_inited = 0;

struct backend_ctx {
    struct spdk_nvme_ctrlr *ctrlr;
    struct spdk_nvme_qpair *qpair;
    struct spdk_doca_queue_resources res;
};

/* ---------------- env init ---------------- */

int spdk_backend_init_env(const char *app_name, int shm_id)
{
    struct spdk_env_opts opts;

    if (g_inited)
        return 0;

    spdk_env_opts_init(&opts);
    opts.opts_size = sizeof(opts);
    opts.name = app_name;
    opts.shm_id = shm_id;
    /*
     * This tool always probes an explicit PCIe traddr instead of scanning all
     * PCI devices, so match spdk_nvme_identify's no_pci flow as closely as possible.
     */
    opts.no_pci = true;

    if (spdk_env_init(&opts) < 0)
        return -1;

    g_inited = 1;
    return 0;
}

/* ---------------- probe ---------------- */

struct probe_ctx {
    const char *bdf;
    struct spdk_nvme_ctrlr *ctrlr;
};

static bool probe_cb(void *cb_ctx,
    const struct spdk_nvme_transport_id *trid,
    struct spdk_nvme_ctrlr_opts *opts)
{
    struct probe_ctx *ctx = cb_ctx;
    return (strcmp(trid->traddr, ctx->bdf) == 0);
}

static void attach_cb(void *cb_ctx,
    const struct spdk_nvme_transport_id *trid,
    struct spdk_nvme_ctrlr *ctrlr,
    const struct spdk_nvme_ctrlr_opts *opts)
{
    struct probe_ctx *ctx = cb_ctx;
    ctx->ctrlr = ctrlr;
}

/* ---------------- main export ---------------- */

int spdk_backend_qpair_export(
    const char *bdf,
    struct spdk_doca_queue_resources *out,
    void **backend_ctx)
{
    struct spdk_nvme_transport_id trid;
    char trid_str[SPDK_NVMF_TRADDR_MAX_LEN + 32];
    struct probe_ctx pctx = {0};
    struct spdk_nvme_ctrlr *ctrlr;
    struct spdk_nvme_io_qpair_opts qopts;
    struct spdk_nvme_qpair *qpair;

    memset(&trid, 0, sizeof(trid));
    snprintf(trid_str, sizeof(trid_str), "trtype:PCIe traddr:%s", bdf);
    if (spdk_nvme_transport_id_parse(&trid, trid_str) != 0)
        return -1;

    pctx.bdf = bdf;

    if (spdk_nvme_probe(&trid, &pctx, probe_cb, attach_cb, NULL) != 0)
        return -2;

    if (!pctx.ctrlr)
        return -3;

    ctrlr = pctx.ctrlr;

    spdk_nvme_ctrlr_get_default_io_qpair_opts(ctrlr, &qopts, sizeof(qopts));

    qpair = spdk_nvme_ctrlr_alloc_io_qpair(ctrlr, &qopts, sizeof(qopts));
    if (!qpair)
        return -4;

    if (spdk_nvme_qpair_export_doca_resources(qpair, qopts.io_queue_size, out) != 0) {
        spdk_nvme_ctrlr_free_io_qpair(qpair);
        return -5;
    }

    struct backend_ctx *ctx = calloc(1, sizeof(*ctx));
    if (!ctx) {
        if (out->dummy_db_page_addr)
            spdk_free(out->dummy_db_page_addr);
        spdk_nvme_ctrlr_free_io_qpair(qpair);
        return -6;
    }

    ctx->ctrlr = ctrlr;
    ctx->qpair = qpair;
    memcpy(&ctx->res, out, sizeof(*out));

    *backend_ctx = ctx;
    return 0;
}

/* ---------------- cleanup ---------------- */

void spdk_backend_close(void *backend_ctx)
{
    struct backend_ctx *ctx = backend_ctx;

    if (!ctx)
        return;

    if (ctx->res.dummy_db_page_addr)
        spdk_free(ctx->res.dummy_db_page_addr);

    if (ctx->qpair)
        spdk_nvme_ctrlr_free_io_qpair(ctx->qpair);

    if (ctx->ctrlr)
        spdk_nvme_detach(ctx->ctrlr);

    free(ctx);
}
