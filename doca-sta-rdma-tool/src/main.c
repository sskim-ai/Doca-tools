#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <doca_ctx.h>
#include <doca_dev.h>
#include <doca_error.h>
#include <doca_pe.h>
#include <doca_sta.h>
#include <doca_sta_io.h>

struct app_state {
	struct doca_pe *sta_pe;
	struct doca_pe *sta_io_pe;
	struct doca_dev *ctrl_dev;
	struct doca_dev *net_dev;
	struct doca_sta *sta;
	struct doca_sta_io *sta_io;
	bool sta_running;
	bool sta_io_running;
};

static void
ctx_state_changed_cb(const union doca_data user_data,
		     struct doca_ctx *ctx,
		     enum doca_ctx_states prev_state,
		     enum doca_ctx_states next_state)
{
	const char *label = user_data.ptr;

	(void)ctx;
	printf("[%s] ctx state changed: %d -> %d\n", label, prev_state, next_state);
}

static doca_error_t
wait_for_running(struct app_state *app)
{
	for (uint32_t i = 0; i < 10000; ++i) {
		enum doca_ctx_states sta_state = DOCA_CTX_STATE_IDLE;
		enum doca_ctx_states sta_io_state = DOCA_CTX_STATE_IDLE;
		doca_error_t result;

		result = doca_ctx_get_state(doca_sta_as_ctx(app->sta), &sta_state);
		if (result != DOCA_SUCCESS)
			return result;

		result = doca_ctx_get_state(doca_sta_io_as_ctx(app->sta_io), &sta_io_state);
		if (result != DOCA_SUCCESS)
			return result;

		app->sta_running = (sta_state == DOCA_CTX_STATE_RUNNING);
		app->sta_io_running = (sta_io_state == DOCA_CTX_STATE_RUNNING);

		if (app->sta_running && app->sta_io_running)
			return DOCA_SUCCESS;

		if (app->sta_pe != NULL)
			(void)doca_pe_progress(app->sta_pe);
		if (app->sta_io_pe != NULL)
			(void)doca_pe_progress(app->sta_io_pe);
	}

	return DOCA_ERROR_TIME_OUT;
}

static doca_error_t
open_sta_capable_device(const char *requested_pci, struct doca_dev **dev)
{
	struct doca_devinfo **dev_list = NULL;
	uint32_t nb_devs = 0;
	doca_error_t result;

	result = doca_devinfo_create_list(&dev_list, &nb_devs);
	if (result != DOCA_SUCCESS)
		return result;

	for (uint32_t i = 0; i < nb_devs; ++i) {
		char pci_addr[32] = {0};
		bool pci_match;

		result = doca_devinfo_get_pci_addr_str(dev_list[i], pci_addr);
		if (result != DOCA_SUCCESS)
			continue;

		pci_match = (requested_pci == NULL) || (strcmp(requested_pci, pci_addr) == 0);
		if (!pci_match)
			continue;

		result = doca_sta_cap_is_supported(dev_list[i]);
		if (result != DOCA_SUCCESS)
			continue;

		result = doca_dev_open(dev_list[i], dev);
		(void)doca_devinfo_destroy_list(dev_list);
		if (result == DOCA_SUCCESS)
			printf("Selected DOCA device: %s\n", pci_addr);
		return result;
	}

	(void)doca_devinfo_destroy_list(dev_list);
	return DOCA_ERROR_NOT_FOUND;
}

static void
cleanup(struct app_state *app)
{
	if (app->sta_io != NULL) {
		(void)doca_ctx_stop(doca_sta_io_as_ctx(app->sta_io));
		(void)doca_sta_io_destroy(app->sta_io);
	}

	if (app->sta != NULL) {
		(void)doca_ctx_stop(doca_sta_as_ctx(app->sta));
		(void)doca_sta_destroy(app->sta);
	}

	if (app->sta_io_pe != NULL)
		(void)doca_pe_destroy(app->sta_io_pe);

	if (app->sta_pe != NULL)
		(void)doca_pe_destroy(app->sta_pe);

	if (app->net_dev != NULL && app->net_dev != app->ctrl_dev)
		(void)doca_dev_close(app->net_dev);

	if (app->ctrl_dev != NULL)
		(void)doca_dev_close(app->ctrl_dev);
}

int
main(int argc, char **argv)
{
	const char *requested_pci = NULL;
	struct app_state app = {0};
	doca_error_t result;

	if (argc > 2) {
		fprintf(stderr, "Usage: %s [host-visible-doca-pci-bdf]\n", argv[0]);
		return 1;
	}

	if (argc == 2)
		requested_pci = argv[1];

	result = open_sta_capable_device(requested_pci, &app.ctrl_dev);
	if (result != DOCA_SUCCESS) {
		fprintf(stderr, "Failed to open STA-capable DOCA device: %s\n",
			doca_error_get_descr(result));
		return 1;
	}
	app.net_dev = app.ctrl_dev;

	result = doca_pe_create(&app.sta_pe);
	if (result != DOCA_SUCCESS) {
		fprintf(stderr, "Failed to create STA DOCA PE: %s\n", doca_error_get_descr(result));
		cleanup(&app);
		return 1;
	}

	result = doca_pe_create(&app.sta_io_pe);
	if (result != DOCA_SUCCESS) {
		fprintf(stderr, "Failed to create STA IO DOCA PE: %s\n", doca_error_get_descr(result));
		cleanup(&app);
		return 1;
	}

	result = doca_sta_create(app.ctrl_dev, &app.sta);
	if (result != DOCA_SUCCESS) {
		fprintf(stderr, "Failed to create DOCA STA context: %s\n", doca_error_get_descr(result));
		cleanup(&app);
		return 1;
	}

	result = doca_sta_add_dev(app.sta, app.net_dev);
	if (result != DOCA_SUCCESS) {
		fprintf(stderr, "Failed to add DOCA device to STA context: %s\n", doca_error_get_descr(result));
		cleanup(&app);
		return 1;
	}

	result = doca_sta_set_max_sta_io(app.sta, 1);
	if (result != DOCA_SUCCESS) {
		fprintf(stderr, "Failed to set max STA IO contexts: %s\n", doca_error_get_descr(result));
		cleanup(&app);
		return 1;
	}

	result = doca_ctx_set_user_data(doca_sta_as_ctx(app.sta), (union doca_data){ .ptr = "sta" });
	if (result != DOCA_SUCCESS) {
		fprintf(stderr, "Failed to set STA ctx user data: %s\n", doca_error_get_descr(result));
		cleanup(&app);
		return 1;
	}

	result = doca_pe_connect_ctx(app.sta_pe, doca_sta_as_ctx(app.sta));
	if (result != DOCA_SUCCESS) {
		fprintf(stderr, "Failed to connect STA ctx to PE: %s\n", doca_error_get_descr(result));
		cleanup(&app);
		return 1;
	}

	result = doca_ctx_set_state_changed_cb(doca_sta_as_ctx(app.sta), ctx_state_changed_cb);
	if (result != DOCA_SUCCESS) {
		fprintf(stderr, "Failed to set STA state callback: %s\n", doca_error_get_descr(result));
		cleanup(&app);
		return 1;
	}

	result = doca_ctx_start(doca_sta_as_ctx(app.sta));
	if (result != DOCA_SUCCESS && result != DOCA_ERROR_IN_PROGRESS) {
		fprintf(stderr, "Failed to start STA ctx: %s\n", doca_error_get_descr(result));
		cleanup(&app);
		return 1;
	}

	result = wait_for_running(&app);
	if (result != DOCA_SUCCESS || !app.sta_running) {
		fprintf(stderr, "STA main context did not reach RUNNING state: %s\n", doca_error_get_descr(result));
		cleanup(&app);
		return 1;
	}

	result = doca_sta_io_create(app.sta, &app.sta_io);
	if (result != DOCA_SUCCESS) {
		fprintf(stderr, "Failed to create DOCA STA IO context: %s\n", doca_error_get_descr(result));
		cleanup(&app);
		return 1;
	}

	result = doca_ctx_set_user_data(doca_sta_io_as_ctx(app.sta_io), (union doca_data){ .ptr = "sta_io" });
	if (result != DOCA_SUCCESS) {
		fprintf(stderr, "Failed to set STA IO ctx user data: %s\n", doca_error_get_descr(result));
		cleanup(&app);
		return 1;
	}

	result = doca_ctx_set_state_changed_cb(doca_sta_io_as_ctx(app.sta_io), ctx_state_changed_cb);
	if (result != DOCA_SUCCESS) {
		fprintf(stderr, "Failed to set STA IO state callback: %s\n", doca_error_get_descr(result));
		cleanup(&app);
		return 1;
	}

	result = doca_pe_connect_ctx(app.sta_io_pe, doca_sta_io_as_ctx(app.sta_io));
	if (result != DOCA_SUCCESS) {
		fprintf(stderr, "Failed to connect STA IO ctx to PE: %s\n", doca_error_get_descr(result));
		cleanup(&app);
		return 1;
	}

	result = doca_ctx_start(doca_sta_io_as_ctx(app.sta_io));
	if (result != DOCA_SUCCESS && result != DOCA_ERROR_IN_PROGRESS) {
		fprintf(stderr, "Failed to start STA IO ctx: %s\n", doca_error_get_descr(result));
		cleanup(&app);
		return 1;
	}

	result = wait_for_running(&app);
	if (result != DOCA_SUCCESS || !app.sta_io_running) {
		fprintf(stderr, "STA IO context did not reach RUNNING state: %s\n", doca_error_get_descr(result));
		cleanup(&app);
		return 1;
	}

	printf("STA host-side control and IO contexts are running.\n");
	printf("Next step: add remote subsystem / namespace / QP setup for RDMA path validation.\n");

	cleanup(&app);
	return 0;
}
