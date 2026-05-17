#ifndef SPDK_BACKEND_QPAIR_EXPORT_H
#define SPDK_BACKEND_QPAIR_EXPORT_H

#include "spdk_doca_queue_export.h"

#ifdef __cplusplus
extern "C" {
#endif

int spdk_backend_init_env(const char *app_name, int shm_id);

int spdk_backend_qpair_export(
    const char *bdf,
    struct spdk_doca_queue_resources *out,
    void **backend_ctx);

void spdk_backend_close(void *backend_ctx);

#ifdef __cplusplus
}
#endif

#endif