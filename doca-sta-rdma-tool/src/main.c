#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/mman.h>

#include <doca_ctx.h>
#include <doca_dev.h>
#include <doca_error.h>
#include <doca_log.h>
#include <doca_pe.h>
#include <doca_sta.h>
#include <doca_sta_be.h>
#include <doca_sta_io.h>
#include <doca_sta_io_non_offload.h>
#include <doca_sta_event.h>
#include <doca_sta_mem.h>
#include <doca_sta_subsystem.h>
#include <doca_sta_task.h>

struct config {
	const char *pf_dev;
	const char *sf_dev;
	uint16_t max_sta_io;
	uint32_t hold_seconds;
	bool skip_add_dev;
	bool start_io;
};

struct app_state {
	struct config cfg;
	struct doca_dev *pf_dev;
	struct doca_dev *sf_dev;
	struct doca_sta *sta;
	struct doca_sta_io *sta_io;
	struct doca_ctx *main_ctx;
	struct doca_ctx *io_ctx;
	struct doca_pe *main_pe;
	struct doca_log_backend *sdk_log_backend;
};

enum selector_type {
	SELECTOR_PCI = 0,
	SELECTOR_IFACE,
	SELECTOR_IBDEV,
};

struct device_selector {
	enum selector_type type;
	const char *value;
};


struct allocation_record {
	void *ptr;
	size_t size;
	struct allocation_record *next;
};

static struct allocation_record *g_allocations;
static int g_pagemap_fd = -1;
static size_t g_page_size;

static size_t
page_size_get(void)
{
	if (g_page_size == 0)
		g_page_size = (size_t)sysconf(_SC_PAGESIZE);
	return g_page_size;
}

static int
pagemap_open(void)
{
	if (g_pagemap_fd >= 0)
		return g_pagemap_fd;

	g_pagemap_fd = open("/proc/self/pagemap", O_RDONLY | O_CLOEXEC);
	return g_pagemap_fd;
}

static uint64_t
virt_to_phys_single_page(const void *addr)
{
	const size_t page_sz = page_size_get();
	const uint64_t virt = (uint64_t)(uintptr_t)addr;
	const uint64_t page_index = virt / page_sz;
	const uint64_t offset = virt % page_sz;
	const int fd = pagemap_open();
	uint64_t entry = 0;
	const off_t file_off = (off_t)(page_index * sizeof(uint64_t));
	ssize_t n;
	uint64_t pfn;

	if (fd < 0)
		return DOCA_STA_VTOPHYS_ERROR;

	n = pread(fd, &entry, sizeof(entry), file_off);
	if (n != (ssize_t)sizeof(entry))
		return DOCA_STA_VTOPHYS_ERROR;

	if (((entry >> 63U) & 0x1U) == 0)
		return DOCA_STA_VTOPHYS_ERROR;

	pfn = entry & ((1ULL << 55U) - 1ULL);
	if (pfn == 0)
		return DOCA_STA_VTOPHYS_ERROR;

	return (pfn * page_sz) + offset;
}

static size_t
normalize_alignment(size_t align)
{
	size_t a = align > sizeof(void *) ? align : sizeof(void *);
	size_t pow2 = 1;

	while (pow2 < a)
		pow2 <<= 1U;

	return pow2 > page_size_get() ? pow2 : page_size_get();
}

static void
touch_pages(void *buf, size_t size)
{
	volatile unsigned char *p = (volatile unsigned char *)buf;
	const size_t page_sz = page_size_get();

	for (size_t i = 0; i < size; i += page_sz)
		p[i] = 0;

	if (size > 0)
		p[size - 1] = 0;
}

static int
allocation_record_add(void *ptr, size_t size)
{
	struct allocation_record *rec = calloc(1, sizeof(*rec));

	if (rec == NULL)
		return -1;

	rec->ptr = ptr;
	rec->size = size;
	rec->next = g_allocations;
	g_allocations = rec;
	return 0;
}

static size_t
allocation_record_remove(void *ptr)
{
	struct allocation_record **cur = &g_allocations;

	while (*cur != NULL) {
		struct allocation_record *rec = *cur;

		if (rec->ptr == ptr) {
			size_t size = rec->size;
			*cur = rec->next;
			free(rec);
			return size;
		}

		cur = &rec->next;
	}

	return 0;
}

static void *
sta_zmalloc_cb(size_t size, size_t align, uint64_t *phys_addr)
{
	const size_t use_align = normalize_alignment(align);
	void *ptr = NULL;
	uint64_t phys;
	int rc;

	rc = posix_memalign(&ptr, use_align, size);
	if (rc != 0) {
		printf("[ALLOC] posix_memalign failed size=%zu align=%zu rc=%d (%s)\n",
		       size, use_align, rc, strerror(rc));
		return NULL;
	}

	memset(ptr, 0, size);
	touch_pages(ptr, size);

	if (mlock(ptr, size) != 0) {
		printf("[ALLOC] mlock failed size=%zu ptr=%p errno=%d (%s)\n",
		       size, ptr, errno, strerror(errno));
		free(ptr);
		return NULL;
	}

	phys = virt_to_phys_single_page(ptr);
	if (phys == DOCA_STA_VTOPHYS_ERROR) {
		printf("[ALLOC] first-page vtophys failed size=%zu ptr=%p\n", size, ptr);
		(void)munlock(ptr, size);
		free(ptr);
		return NULL;
	}

	if (allocation_record_add(ptr, size) != 0) {
		printf("[ALLOC] allocation record failed size=%zu ptr=%p\n", size, ptr);
		(void)munlock(ptr, size);
		free(ptr);
		return NULL;
	}

	if (phys_addr != NULL)
		*phys_addr = phys;

	printf("[ALLOC] ok size=%zu align=%zu ptr=%p first_phys=0x%lx\n",
	       size, use_align, ptr, (unsigned long)phys);
	return ptr;
}

static void
sta_free_cb(void *buf)
{
	size_t size;

	if (buf == NULL)
		return;

	size = allocation_record_remove(buf);
	if (size != 0)
		(void)munlock(buf, size);

	printf("[ALLOC] free ptr=%p size=%zu\n", buf, size);
	free(buf);
}

static uint64_t
sta_vtophys_cb(const void *buf, uint32_t size)
{
	const size_t page_sz = page_size_get();
	const uintptr_t start = (uintptr_t)buf;
	const uintptr_t first_page_virt = start & ~((uintptr_t)page_sz - 1U);
	const size_t offset_in_first_page = (size_t)(start - first_page_virt);
	const size_t total_span = offset_in_first_page + (size_t)size;
	const size_t num_pages = (total_span + page_sz - 1U) / page_sz;
	const uint64_t first_phys = virt_to_phys_single_page(buf);
	const uint64_t first_phys_page = first_phys & ~((uint64_t)page_sz - 1ULL);

	if (buf == NULL || size == 0 || first_phys == DOCA_STA_VTOPHYS_ERROR) {
		printf("[ALLOC] vtophys failed buf=%p size=%u\n", buf, size);
		return DOCA_STA_VTOPHYS_ERROR;
	}

	for (size_t i = 1; i < num_pages; ++i) {
		const void *page_ptr = (const void *)(first_page_virt + (i * page_sz));
		const uint64_t phys_i = virt_to_phys_single_page(page_ptr);
		const uint64_t phys_i_page = phys_i & ~((uint64_t)page_sz - 1ULL);

		if (phys_i == DOCA_STA_VTOPHYS_ERROR || phys_i_page != first_phys_page + (i * page_sz)) {
			printf("[ALLOC] vtophys non-contiguous/fail buf=%p size=%u page=%zu\n", buf, size, i);
			return DOCA_STA_VTOPHYS_ERROR;
		}
	}

	printf("[ALLOC] vtophys buf=%p size=%u phys=0x%lx\n", buf, size, (unsigned long)first_phys);
	return first_phys;
}

static void
on_sta_be_timeout(const struct doca_sta_event_be_timeout *event, union doca_data user_data)
{
	(void)event;
	(void)user_data;
	printf("[CB] STA backend timeout event\n");
}

static void
on_sta_eu_err(const struct doca_sta_event_eu_err *event, union doca_data user_data)
{
	bool fatal = false;

	(void)user_data;
	(void)doca_sta_event_eu_err_is_fatal_error(event, &fatal);
	printf("[CB] STA EU error event fatal=%s\n", fatal ? "true" : "false");
}

static void
on_sta_cqe_notify(const struct doca_sta_event_cqe_notify *event, union doca_data user_data)
{
	(void)event;
	(void)user_data;
	printf("[CB] STA CQE notify event\n");
}

static void
on_sta_io_transport_err(const struct doca_sta_event_transport_err *event, union doca_data user_data)
{
	uint32_t syndrome = 0;
	uint32_t vendor_syndrome = 0;
	uint32_t operation = 0;

	(void)user_data;
	(void)doca_sta_event_transport_err_get_operation(event, &operation);
	(void)doca_sta_event_transport_err_get_syndrome(event, &syndrome);
	(void)doca_sta_event_transport_err_get_vendor_syndrome(event, &vendor_syndrome);
	printf("[CB] STA IO transport error operation=%u syndrome=0x%x vendor_syndrome=0x%x\n",
	       operation, syndrome, vendor_syndrome);
}


static void
on_sta_task_complete(struct doca_sta_producer_task_send *task, union doca_data task_user_data)
{
	(void)task;
	(void)task_user_data;
	printf("[CB] STA producer task completed\n");
}

static void
on_sta_task_error(struct doca_sta_producer_task_send *task, union doca_data task_user_data)
{
	(void)task;
	(void)task_user_data;
	printf("[CB] STA producer task error\n");
}

static doca_error_t
register_sta_task_confs(struct doca_sta *sta)
{
	doca_error_t result;

	result = doca_sta_subsystem_task_rm_ns_set_conf(sta, on_sta_task_complete, on_sta_task_error);
	if (result != DOCA_SUCCESS)
		return result;

	return doca_sta_be_task_destroy_queue_set_conf(sta, on_sta_task_complete, on_sta_task_error);
}


static void
on_sta_io_non_offload(struct doca_sta_qp_handle *qp_handle,
			       union doca_data user_data,
			       const uint8_t *nvme_cmd,
			       uint8_t *payload,
			       uint32_t payload_len,
			       bool payload_valid,
			       union doca_data non_offload_user_data)
{
	(void)qp_handle;
	(void)user_data;
	(void)nvme_cmd;
	(void)payload;
	(void)non_offload_user_data;
	printf("[CB] STA IO non-offload command payload_len=%u payload_valid=%s\n",
	       payload_len, payload_valid ? "true" : "false");
}

static doca_error_t
register_sta_io_confs(struct doca_sta_io *sta_io)
{
	union doca_data user_data = {0};
	doca_error_t result;

	result = doca_sta_io_task_disconnect_set_conf(sta_io, on_sta_task_complete, on_sta_task_error);
	if (result != DOCA_SUCCESS)
		return result;

	result = doca_sta_io_event_transport_err_register_cb(sta_io, on_sta_io_transport_err, user_data);
	if (result != DOCA_SUCCESS)
		return result;

	result = doca_sta_io_non_offload_register_cb(sta_io, on_sta_io_non_offload, user_data);
	if (result != DOCA_SUCCESS)
		return result;

	result = doca_sta_io_task_non_offload_set_rdma_write_send_conf(sta_io,
									on_sta_task_complete,
									on_sta_task_error);
	if (result != DOCA_SUCCESS)
		return result;

	return doca_sta_io_task_non_offload_set_rdma_read_conf(sta_io,
								    on_sta_task_complete,
								    on_sta_task_error);
}
static doca_error_t
register_sta_event_callbacks(struct doca_sta *sta)
{
	union doca_data user_data = {0};
	doca_error_t result;

	result = doca_sta_event_be_timeout_register_cb(sta, on_sta_be_timeout, user_data);
	if (result != DOCA_SUCCESS)
		return result;

	result = doca_sta_event_eu_err_register_cb(sta, on_sta_eu_err, user_data);
	if (result != DOCA_SUCCESS)
		return result;

	return doca_sta_event_cqe_notify_register_cb(sta, on_sta_cqe_notify, user_data);
}

static const char *
ctx_state_to_str(enum doca_ctx_states state)
{
	switch (state) {
	case DOCA_CTX_STATE_IDLE:
		return "IDLE";
	case DOCA_CTX_STATE_STARTING:
		return "STARTING";
	case DOCA_CTX_STATE_RUNNING:
		return "RUNNING";
	case DOCA_CTX_STATE_STOPPING:
		return "STOPPING";
	default:
		return "UNKNOWN";
	}
}

static void
dump_doca_devices(void)
{
	struct doca_devinfo **dev_list = NULL;
	uint32_t nb_devs = 0;
	doca_error_t result;

	result = doca_devinfo_create_list(&dev_list, &nb_devs);
	if (result != DOCA_SUCCESS) {
		fprintf(stderr, "Failed to enumerate DOCA devices: %s\n", doca_error_get_descr(result));
		return;
	}

	printf("Available DOCA devices:\n");
	for (uint32_t i = 0; i < nb_devs; ++i) {
		char pci_addr[32] = {0};
		char iface_name[64] = {0};
		char ibdev_name[64] = {0};
		doca_error_t pci_result;
		doca_error_t iface_result;
		doca_error_t ibdev_result;
		doca_error_t sta_result;

		pci_result = doca_devinfo_get_pci_addr_str(dev_list[i], pci_addr);
		iface_result = doca_devinfo_get_iface_name(dev_list[i], iface_name, sizeof(iface_name));
		ibdev_result = doca_devinfo_get_ibdev_name(dev_list[i], ibdev_name, sizeof(ibdev_name));
		sta_result = doca_sta_cap_is_supported(dev_list[i]);

		printf("  - pci=%s iface=%s ibdev=%s sta_cap=%s\n",
		       pci_result == DOCA_SUCCESS ? pci_addr : "n/a",
		       iface_result == DOCA_SUCCESS ? iface_name : "n/a",
		       ibdev_result == DOCA_SUCCESS ? ibdev_name : "n/a",
		       sta_result == DOCA_SUCCESS ? "yes" : "no");
	}

	(void)doca_devinfo_destroy_list(dev_list);
}

static void
dump_devinfo(const char *tag, struct doca_dev *dev)
{
	const struct doca_devinfo *info = doca_dev_as_devinfo(dev);
	char pci_addr[32] = {0};
	char iface_name[64] = {0};
	char ibdev_name[64] = {0};

	if (info == NULL) {
		printf("[DBG] %s devinfo=null\n", tag);
		return;
	}

	if (doca_devinfo_get_pci_addr_str(info, pci_addr) != DOCA_SUCCESS)
		strcpy(pci_addr, "<n/a>");
	if (doca_devinfo_get_iface_name(info, iface_name, sizeof(iface_name)) != DOCA_SUCCESS)
		strcpy(iface_name, "<n/a>");
	if (doca_devinfo_get_ibdev_name(info, ibdev_name, sizeof(ibdev_name)) != DOCA_SUCCESS)
		strcpy(ibdev_name, "<n/a>");

	printf("[DBG] %s pci=%s iface=%s ibdev=%s\n", tag, pci_addr, iface_name, ibdev_name);
}

static void
dump_ctx_state(const char *tag, struct doca_ctx *ctx)
{
	enum doca_ctx_states state;
	doca_error_t result;

	result = doca_ctx_get_state(ctx, &state);
	if (result != DOCA_SUCCESS) {
		printf("[DBG] %s state query failed: %s\n", tag, doca_error_get_descr(result));
		return;
	}

	printf("[DBG] %s state=%s (%d)\n", tag, ctx_state_to_str(state), state);
}

static void
ctx_state_changed_cb(const union doca_data user_data,
		     struct doca_ctx *ctx,
		     enum doca_ctx_states prev_state,
		     enum doca_ctx_states next_state)
{
	(void)ctx;
	printf("[DBG] %s state changed: %s -> %s\n",
	       (const char *)user_data.ptr,
	       ctx_state_to_str(prev_state),
	       ctx_state_to_str(next_state));
}

static void
progress_ctx_for_debug(struct doca_pe *pe, struct doca_ctx *ctx, const char *tag, int loops)
{
	for (int i = 0; i < loops; ++i) {
		int progressed = doca_pe_progress(pe);

		if ((i % 100) == 0) {
			enum doca_ctx_states state;
			doca_error_t result = doca_ctx_get_state(ctx, &state);
			if (result == DOCA_SUCCESS) {
				printf("[DBG] %s loop=%d progressed=%d state=%s\n",
				       tag, i, progressed, ctx_state_to_str(state));
			} else {
				printf("[DBG] %s loop=%d progressed=%d state_query=%s\n",
				       tag, i, progressed, doca_error_get_descr(result));
			}
		}
	}
}


static void
print_u32_result(const char *name, doca_error_t result, uint32_t value)
{
	if (result == DOCA_SUCCESS)
		printf("[CAP] %s=%u\n", name, value);
	else
		printf("[CAP] %s=<%s>\n", name, doca_error_get_descr(result));
}

static void
print_u16_result(const char *name, doca_error_t result, uint16_t value)
{
	if (result == DOCA_SUCCESS)
		printf("[CAP] %s=%u\n", name, value);
	else
		printf("[CAP] %s=<%s>\n", name, doca_error_get_descr(result));
}

static void
dump_sta_caps(const char *stage, struct doca_sta *sta)
{
	uint16_t value16 = 0;
	uint32_t value32 = 0;

	printf("[CAP] ---- %s ----\n", stage);
	print_u32_result("max_devs", doca_sta_get_max_devs(&value32), value32);
	value32 = 0;
	print_u32_result("max_subsys", doca_sta_get_max_subsys(&value32), value32);
	value32 = 0;
	print_u32_result("max_ns_per_subsys", doca_sta_get_max_ns_per_subs(&value32), value32);
	value32 = 0;
	print_u32_result("max_qps", doca_sta_get_max_qps(&value32), value32);
	value32 = 0;
	print_u32_result("max_io_threads", doca_sta_get_max_io_threads(&value32), value32);
	value32 = 0;
	print_u32_result("max_io_size", doca_sta_get_max_io_size(&value32), value32);
	value32 = 0;
	print_u32_result("max_ioccsz", doca_sta_get_max_ioccsz(&value32), value32);
	value32 = 0;
	print_u32_result("max_iorcsz", doca_sta_get_max_iorcsz(&value32), value32);
	value32 = 0;
	print_u32_result("max_be", doca_sta_get_max_be(&value32), value32);
	value32 = 0;
	print_u32_result("max_qs_per_be", doca_sta_get_max_qs_per_be(&value32), value32);
	value32 = 0;
	print_u32_result("max_num_eus_available", doca_sta_get_max_num_eus_available(sta, &value32), value32);
	value32 = 0;
	print_u32_result("max_connected_qp_per_eu", doca_sta_get_max_num_connected_qp_per_eu(sta, &value32), value32);
	value32 = 0;
	print_u32_result("max_io_num_per_dev", doca_sta_get_max_io_num_per_dev(sta, &value32), value32);
	value32 = 0;
	print_u32_result("max_io_queue_size", doca_sta_get_max_io_queue_size(sta, &value32), value32);
	print_u16_result("configured_max_sta_io", doca_sta_get_max_sta_io(sta, &value16), value16);
}

static doca_error_t
wait_until_state(struct doca_pe *pe, struct doca_ctx *ctx, enum doca_ctx_states target, const char *name)
{
	for (int i = 0; i < 20000; ++i) {
		enum doca_ctx_states state;
		doca_error_t result = doca_ctx_get_state(ctx, &state);

		if (result != DOCA_SUCCESS)
			return result;

		if ((i % 100) == 0)
			printf("[DBG] %s wait_loop=%d state=%s target=%s\n",
			       name, i, ctx_state_to_str(state), ctx_state_to_str(target));

		if (state == target)
			return DOCA_SUCCESS;

		(void)doca_pe_progress(pe);
		usleep(1000);
	}

	return DOCA_ERROR_TIME_OUT;
}

static doca_error_t
wait_until_running(struct doca_pe *pe, struct doca_ctx *ctx, const char *name)
{
	for (int i = 0; i < 5000; ++i) {
		enum doca_ctx_states state;
		doca_error_t result = doca_ctx_get_state(ctx, &state);

		if (result != DOCA_SUCCESS)
			return result;

		if ((i % 100) == 0)
			printf("[DBG] %s wait_loop=%d state=%s\n", name, i, ctx_state_to_str(state));

		if (state == DOCA_CTX_STATE_RUNNING)
			return DOCA_SUCCESS;

		if (state == DOCA_CTX_STATE_IDLE || state == DOCA_CTX_STATE_STOPPING)
			return DOCA_ERROR_BAD_STATE;

		(void)doca_pe_progress(pe);
	}

	return DOCA_ERROR_TIME_OUT;
}

static struct device_selector
parse_selector(const char *value)
{
	struct device_selector selector;

	if (strchr(value, ':') != NULL) {
		selector.type = SELECTOR_PCI;
		selector.value = value;
		return selector;
	}

	if (strncmp(value, "mlx5_", 5) == 0) {
		selector.type = SELECTOR_IBDEV;
		selector.value = value;
		return selector;
	}

	selector.type = SELECTOR_IFACE;
	selector.value = value;
	return selector;
}

static doca_error_t
open_local_dev(const struct device_selector *selector, struct doca_dev **out)
{
	struct doca_devinfo **dev_list = NULL;
	uint32_t nb_devs = 0;
	doca_error_t result;

	result = doca_devinfo_create_list(&dev_list, &nb_devs);
	if (result != DOCA_SUCCESS)
		return result;

	for (uint32_t i = 0; i < nb_devs; ++i) {
		char pci_addr[32] = {0};
		char iface_name[64] = {0};
		char ibdev_name[64] = {0};
		bool match = false;

		(void)doca_devinfo_get_pci_addr_str(dev_list[i], pci_addr);
		(void)doca_devinfo_get_iface_name(dev_list[i], iface_name, sizeof(iface_name));
		(void)doca_devinfo_get_ibdev_name(dev_list[i], ibdev_name, sizeof(ibdev_name));

		if (selector->type == SELECTOR_PCI)
			match = (strcmp(selector->value, pci_addr) == 0);
		else if (selector->type == SELECTOR_IFACE)
			match = (strcmp(selector->value, iface_name) == 0);
		else if (selector->type == SELECTOR_IBDEV)
			match = (strcmp(selector->value, ibdev_name) == 0);

		if (!match)
			continue;

		result = doca_dev_open(dev_list[i], out);
		if (result == DOCA_SUCCESS) {
			printf("[OK] opened DOCA dev selector=%s pci=%s iface=%s ibdev=%s\n",
			       selector->value, pci_addr[0] != '\0' ? pci_addr : "<n/a>",
			       iface_name[0] != '\0' ? iface_name : "<n/a>",
			       ibdev_name[0] != '\0' ? ibdev_name : "<n/a>");
		}
		(void)doca_devinfo_destroy_list(dev_list);
		return result;
	}

	(void)doca_devinfo_destroy_list(dev_list);
	return DOCA_ERROR_NOT_FOUND;
}

static void
cleanup(struct app_state *app)
{
	if (app->io_ctx != NULL && app->main_pe != NULL) {
		doca_error_t result = doca_ctx_stop(app->io_ctx);
		if (result != DOCA_SUCCESS && result != DOCA_ERROR_IN_PROGRESS && result != DOCA_ERROR_BAD_STATE)
			fprintf(stderr, "[WARN] doca_ctx_stop(io) failed: %s\n", doca_error_get_descr(result));
		result = wait_until_state(app->main_pe, app->io_ctx, DOCA_CTX_STATE_IDLE, "io_ctx(stop)");
		if (result != DOCA_SUCCESS)
			fprintf(stderr, "[WARN] IO ctx did not return to IDLE before cleanup: %s\n", doca_error_get_descr(result));
	}

	if (app->main_ctx != NULL && app->main_pe != NULL) {
		doca_error_t result = doca_ctx_stop(app->main_ctx);
		if (result != DOCA_SUCCESS && result != DOCA_ERROR_IN_PROGRESS && result != DOCA_ERROR_BAD_STATE)
			fprintf(stderr, "[WARN] doca_ctx_stop(main) failed: %s\n", doca_error_get_descr(result));
		result = wait_until_state(app->main_pe, app->main_ctx, DOCA_CTX_STATE_IDLE, "main_ctx(stop)");
		if (result != DOCA_SUCCESS)
			fprintf(stderr, "[WARN] main ctx did not return to IDLE before cleanup: %s\n", doca_error_get_descr(result));
	}

	if (app->main_pe != NULL)
		(void)doca_pe_destroy(app->main_pe);

	if (app->sta_io != NULL)
		(void)doca_sta_io_destroy(app->sta_io);

	if (app->sta != NULL)
		(void)doca_sta_destroy(app->sta);

	if (app->sf_dev != NULL)
		(void)doca_dev_close(app->sf_dev);

	if (app->pf_dev != NULL)
		(void)doca_dev_close(app->pf_dev);
}

static void
usage(const char *prog)
{
	fprintf(stderr,
		"Usage:\n"
		"  %s --pf-dev <PCI_BDF|IBDEV|IFACE> --sf-dev <PCI_BDF|IFACE|IBDEV> [--max-sta-io N] [--hold-seconds N] [--start-io]\n"
		"  %s --pf-dev <PCI_BDF|IBDEV|IFACE> --skip-add-dev [--max-sta-io N] [--hold-seconds N] [--start-io]\n"
		"  %s --list\n",
		prog, prog, prog);
}

static int
parse_args(int argc, char **argv, struct config *cfg)
{
	for (int i = 1; i < argc; ++i) {
		if (strcmp(argv[i], "--list") == 0) {
			dump_doca_devices();
			return 1;
		}

		if (strcmp(argv[i], "--pf-dev") == 0 && i + 1 < argc) {
			cfg->pf_dev = argv[++i];
			continue;
		}

		if (strcmp(argv[i], "--sf-dev") == 0 && i + 1 < argc) {
			cfg->sf_dev = argv[++i];
			continue;
		}

		if (strcmp(argv[i], "--max-sta-io") == 0 && i + 1 < argc) {
			char *end = NULL;
			unsigned long value = strtoul(argv[++i], &end, 0);

			if (end == argv[i] || *end != '\0' || value == 0 || value > UINT16_MAX) {
				fprintf(stderr, "invalid --max-sta-io value: %s\n", argv[i]);
				return -1;
			}
			cfg->max_sta_io = (uint16_t)value;
			continue;
		}

		if (strcmp(argv[i], "--skip-add-dev") == 0) {
			cfg->skip_add_dev = true;
			continue;
		}

		if (strcmp(argv[i], "--hold-seconds") == 0 && i + 1 < argc) {
			char *end = NULL;
			unsigned long value = strtoul(argv[++i], &end, 0);

			if (end == argv[i] || *end != '\0' || value > UINT32_MAX) {
				fprintf(stderr, "invalid --hold-seconds value: %s\n", argv[i]);
				return -1;
			}
			cfg->hold_seconds = (uint32_t)value;
			continue;
		}

		if (strcmp(argv[i], "--start-io") == 0) {
			cfg->start_io = true;
			continue;
		}

		fprintf(stderr, "unknown or incomplete arg: %s\n", argv[i]);
		return -1;
	}

	if (cfg->max_sta_io == 0)
		cfg->max_sta_io = 1;

	if (cfg->pf_dev == NULL)
		return -1;

	if (!cfg->skip_add_dev && cfg->sf_dev == NULL)
		return -1;

	return 0;
}

int
main(int argc, char **argv)
{
	struct app_state app = {0};
	struct device_selector pf_selector;
	struct device_selector sf_selector;
	doca_error_t result;
	int parse_rc;

	result = doca_log_backend_create_standard();
	if (result != DOCA_SUCCESS)
		fprintf(stderr, "[WARN] doca_log_backend_create_standard failed: %s\n", doca_error_get_descr(result));

	result = doca_log_backend_create_with_file_sdk(stdout, &app.sdk_log_backend);
	if (result == DOCA_SUCCESS) {
		result = doca_log_backend_set_sdk_level(app.sdk_log_backend, DOCA_LOG_LEVEL_DEBUG);
		if (result != DOCA_SUCCESS)
			fprintf(stderr, "[WARN] doca_log_backend_set_sdk_level failed: %s\n", doca_error_get_descr(result));
	} else {
		fprintf(stderr, "[WARN] doca_log_backend_create_with_file_sdk failed: %s\n", doca_error_get_descr(result));
	}

	parse_rc = parse_args(argc, argv, &app.cfg);
	if (parse_rc > 0)
		return 0;
	if (parse_rc < 0) {
		usage(argv[0]);
		return 1;
	}

	pf_selector = parse_selector(app.cfg.pf_dev);
	if (!app.cfg.skip_add_dev)
		sf_selector = parse_selector(app.cfg.sf_dev);

	printf("[DBG] requested pf_dev=%s sf_dev=%s max_sta_io=%u hold_seconds=%u start_io=%s skip_add_dev=%s\n",
	       app.cfg.pf_dev,
	       app.cfg.sf_dev != NULL ? app.cfg.sf_dev : "<none>",
	       app.cfg.max_sta_io,
	       app.cfg.hold_seconds,
	       app.cfg.start_io ? "yes" : "no",
	       app.cfg.skip_add_dev ? "yes" : "no");

	result = open_local_dev(&pf_selector, &app.pf_dev);
	if (result != DOCA_SUCCESS) {
		fprintf(stderr, "Failed to open PF device: %s\n", doca_error_get_descr(result));
		dump_doca_devices();
		return 1;
	}

	if (!app.cfg.skip_add_dev) {
		result = open_local_dev(&sf_selector, &app.sf_dev);
		if (result != DOCA_SUCCESS) {
			fprintf(stderr, "Failed to open SF device: %s\n", doca_error_get_descr(result));
			cleanup(&app);
			dump_doca_devices();
			return 1;
		}
	}

	dump_devinfo("pf_dev", app.pf_dev);
	if (app.sf_dev != NULL)
		dump_devinfo("sf_dev", app.sf_dev);

	result = doca_pe_create(&app.main_pe);
	if (result != DOCA_SUCCESS) {
		fprintf(stderr, "doca_pe_create failed: %s\n", doca_error_get_descr(result));
		cleanup(&app);
		return 1;
	}

	result = doca_sta_create(app.pf_dev, &app.sta);
	if (result != DOCA_SUCCESS) {
		fprintf(stderr, "doca_sta_create failed: %s\n", doca_error_get_descr(result));
		cleanup(&app);
		return 1;
	}

	result = register_sta_event_callbacks(app.sta);
	if (result != DOCA_SUCCESS) {
		fprintf(stderr, "register STA event callbacks failed: %s\n", doca_error_get_descr(result));
		cleanup(&app);
		return 1;
	}
	printf("[OK] STA event callbacks registered\n");

	result = doca_sta_mem_allocator_register(app.sta, sta_zmalloc_cb, sta_free_cb, sta_vtophys_cb);
	if (result != DOCA_SUCCESS) {
		fprintf(stderr, "doca_sta_mem_allocator_register failed: %s\n", doca_error_get_descr(result));
		cleanup(&app);
		return 1;
	}
	printf("[OK] STA pinned memory allocator registered\n");

	result = register_sta_task_confs(app.sta);
	if (result != DOCA_SUCCESS) {
		fprintf(stderr, "register STA task confs failed: %s\n", doca_error_get_descr(result));
		cleanup(&app);
		return 1;
	}
	printf("[OK] STA task configurations registered\n");

	result = doca_sta_set_max_sta_io(app.sta, app.cfg.max_sta_io);
	if (result != DOCA_SUCCESS) {
		fprintf(stderr, "doca_sta_set_max_sta_io(%u) failed: %s\n",
			app.cfg.max_sta_io, doca_error_get_descr(result));
		cleanup(&app);
		return 1;
	}
	printf("[OK] doca_sta_set_max_sta_io(%u)\n", app.cfg.max_sta_io);
	dump_sta_caps("after set_max_sta_io / before add_dev", app.sta);

	if (!app.cfg.skip_add_dev) {
		result = doca_sta_add_dev(app.sta, app.sf_dev);
		if (result != DOCA_SUCCESS) {
			fprintf(stderr, "doca_sta_add_dev failed: %s\n", doca_error_get_descr(result));
			cleanup(&app);
			return 1;
		}
		printf("[OK] doca_sta_add_dev\n");
		dump_sta_caps("after add_dev", app.sta);
	} else {
		printf("[DBG] skipping doca_sta_add_dev for control-only diagnostic run\n");
	}

	app.main_ctx = doca_sta_as_ctx(app.sta);
	if (app.main_ctx == NULL) {
		fprintf(stderr, "doca_sta_as_ctx returned null\n");
		cleanup(&app);
		return 1;
	}

	result = doca_ctx_set_user_data(app.main_ctx, (union doca_data){ .ptr = "main_sta" });
	if (result != DOCA_SUCCESS) {
		fprintf(stderr, "doca_ctx_set_user_data failed: %s\n", doca_error_get_descr(result));
		cleanup(&app);
		return 1;
	}

	result = doca_ctx_set_state_changed_cb(app.main_ctx, ctx_state_changed_cb);
	if (result != DOCA_SUCCESS) {
		fprintf(stderr, "doca_ctx_set_state_changed_cb failed: %s\n", doca_error_get_descr(result));
		cleanup(&app);
		return 1;
	}

	dump_ctx_state("main_ctx(before connect)", app.main_ctx);

	result = doca_pe_connect_ctx(app.main_pe, app.main_ctx);
	if (result != DOCA_SUCCESS) {
		fprintf(stderr, "doca_pe_connect_ctx failed: %s\n", doca_error_get_descr(result));
		cleanup(&app);
		return 1;
	}

	dump_ctx_state("main_ctx(after connect)", app.main_ctx);
	dump_sta_caps("after pe_connect / before start", app.sta);

	result = doca_ctx_start(app.main_ctx);
	if (result != DOCA_SUCCESS && result != DOCA_ERROR_IN_PROGRESS) {
		fprintf(stderr, "doca_ctx_start failed immediately: %s\n", doca_error_get_descr(result));
		dump_ctx_state("main_ctx(after failed start)", app.main_ctx);
		progress_ctx_for_debug(app.main_pe, app.main_ctx, "main_ctx(post-fail)", 500);
		cleanup(&app);
		return 1;
	}
	if (result == DOCA_ERROR_IN_PROGRESS)
		printf("[OK] doca_ctx_start returned IN_PROGRESS; waiting for RUNNING\n");

	dump_ctx_state("main_ctx(after start call)", app.main_ctx);
	progress_ctx_for_debug(app.main_pe, app.main_ctx, "main_ctx(post-start)", 1000);

	result = wait_until_running(app.main_pe, app.main_ctx, "main_ctx");
	if (result != DOCA_SUCCESS) {
		fprintf(stderr, "main ctx did not reach RUNNING: %s\n", doca_error_get_descr(result));
		cleanup(&app);
		return 1;
	}

	printf("[OK] main STA context is RUNNING\n");

	if (app.cfg.start_io) {
		result = doca_sta_io_create(app.sta, &app.sta_io);
		if (result != DOCA_SUCCESS) {
			fprintf(stderr, "doca_sta_io_create failed: %s\n", doca_error_get_descr(result));
			cleanup(&app);
			return 1;
		}
		printf("[OK] doca_sta_io_create\n");

		result = register_sta_io_confs(app.sta_io);
		if (result != DOCA_SUCCESS) {
			fprintf(stderr, "register STA IO confs failed: %s\n", doca_error_get_descr(result));
			cleanup(&app);
			return 1;
		}
		printf("[OK] STA IO task/event configurations registered\n");

		app.io_ctx = doca_sta_io_as_ctx(app.sta_io);
		if (app.io_ctx == NULL) {
			fprintf(stderr, "doca_sta_io_as_ctx returned null\n");
			cleanup(&app);
			return 1;
		}

		result = doca_ctx_set_user_data(app.io_ctx, (union doca_data){ .ptr = "sta_io" });
		if (result != DOCA_SUCCESS) {
			fprintf(stderr, "doca_ctx_set_user_data(io) failed: %s\n", doca_error_get_descr(result));
			cleanup(&app);
			return 1;
		}

		result = doca_ctx_set_state_changed_cb(app.io_ctx, ctx_state_changed_cb);
		if (result != DOCA_SUCCESS) {
			fprintf(stderr, "doca_ctx_set_state_changed_cb(io) failed: %s\n", doca_error_get_descr(result));
			cleanup(&app);
			return 1;
		}

		result = doca_pe_connect_ctx(app.main_pe, app.io_ctx);
		if (result != DOCA_SUCCESS) {
			fprintf(stderr, "doca_pe_connect_ctx(io) failed: %s\n", doca_error_get_descr(result));
			cleanup(&app);
			return 1;
		}

		result = doca_ctx_start(app.io_ctx);
		if (result != DOCA_SUCCESS && result != DOCA_ERROR_IN_PROGRESS) {
			fprintf(stderr, "doca_ctx_start(io) failed immediately: %s\n", doca_error_get_descr(result));
			cleanup(&app);
			return 1;
		}
		if (result == DOCA_ERROR_IN_PROGRESS)
			printf("[OK] doca_ctx_start(io) returned IN_PROGRESS; waiting for RUNNING\n");

		result = wait_until_running(app.main_pe, app.io_ctx, "io_ctx");
		if (result != DOCA_SUCCESS) {
			fprintf(stderr, "IO ctx did not reach RUNNING: %s\n", doca_error_get_descr(result));
			cleanup(&app);
			return 1;
		}
		printf("[OK] STA IO context is RUNNING\n");
	}
	if (app.cfg.hold_seconds > 0) {
		printf("[INFO] holding RUNNING context for %u seconds\n", app.cfg.hold_seconds);
		for (uint32_t sec = 0; sec < app.cfg.hold_seconds; ++sec) {
			for (int i = 0; i < 1000; ++i) {
				(void)doca_pe_progress(app.main_pe);
				usleep(1000);
			}
		}
	}
	cleanup(&app);
	return 0;
}
