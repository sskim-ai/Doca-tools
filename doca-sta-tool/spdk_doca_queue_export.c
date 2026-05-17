#include "spdk_doca_queue_export.h"

#include <string.h>
#include "spdk/nvme.h"
#include "spdk/env.h"
#include "nvme_internal.h"
#include "nvme_pcie_internal.h"

int spdk_nvme_qpair_export_doca_resources(
    struct spdk_nvme_qpair *qpair,
    struct spdk_doca_queue_resources *out)
{
    struct nvme_pcie_qpair *pqpair;

    if (!qpair || !out)
        return -1;

    memset(out, 0, sizeof(*out));

    pqpair = nvme_pcie_qpair(qpair);
    if (!pqpair)
        return -2;

    out->sq_addr = pqpair->cmd;
    out->cq_addr = pqpair->cpl;

    out->sq_len = qpair->num_entries * sizeof(struct spdk_nvme_cmd);
    out->cq_len = qpair->num_entries * sizeof(struct spdk_nvme_cpl);

    out->dummy_db_page_len = 4096;
    out->dummy_db_page_addr =
        spdk_zmalloc(out->dummy_db_page_len,
                     4096,
                     NULL,
                     SPDK_ENV_SOCKET_ID_ANY,
                     SPDK_MALLOC_DMA);

    if (!out->dummy_db_page_addr)
        return -3;

    out->real_sq_db = pqpair->sq_tdbl;
    out->real_cq_db = pqpair->cq_hdbl;

    out->sq_db_offset = 0;
    out->cq_db_offset = 64;

    out->qid = qpair->id;
    out->num_entries = qpair->num_entries;

    return 0;
}