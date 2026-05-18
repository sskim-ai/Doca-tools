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

enum selector_type {
	SELECTOR_ANY = 0,
	SELECTOR_PCI,
	SELECTOR_IFACE,
	SELECTOR_IBDEV,
};

struct device_selector {
	enum selector_type type;
	const char *value;
};

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
open_doca_device(const struct device_selector *selector,
		 bool require_sta_cap,
		 struct doca_dev **dev)
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
		bool selector_match = false;

		result = doca_devinfo_get_pci_addr_str(dev_list[i], pci_addr);
		if (selector == NULL || selector->type == SELECTOR_ANY) {
			selector_match = true;
		} else if (selector->type == SELECTOR_PCI) {
			selector_match = (result == DOCA_SUCCESS) && (strcmp(selector->value, pci_addr) == 0);
		} else if (selector->type == SELECTOR_IFACE) {
			result = doca_devinfo_get_iface_name(dev_list[i], iface_name, sizeof(iface_name));
			selector_match = (result == DOCA_SUCCESS) && (strcmp(selector->value, iface_name) == 0);
		} else if (selector->type == SELECTOR_IBDEV) {
			result = doca_devinfo_get_ibdev_name(dev_list[i], ibdev_name, sizeof(ibdev_name));
			selector_match = (result == DOCA_SUCCESS) && (strcmp(selector->value, ibdev_name) == 0);
		}

		if (!selector_match)
			continue;

		if (require_sta_cap) {
			result = doca_sta_cap_is_supported(dev_list[i]);
			if (result != DOCA_SUCCESS)
				continue;
		}

		if (iface_name[0] == '\0')
			(void)doca_devinfo_get_iface_name(dev_list[i], iface_name, sizeof(iface_name));
		if (ibdev_name[0] == '\0')
			(void)doca_devinfo_get_ibdev_name(dev_list[i], ibdev_name, sizeof(ibdev_name));

		result = doca_dev_open(dev_list[i], dev);
		if (result == DOCA_SUCCESS) {
			printf("Selected DOCA device: pci=%s iface=%s ibdev=%s\n",
			       pci_addr[0] != '\0' ? pci_addr : "unknown",
			       iface_name[0] != '\0' ? iface_name : "unknown",
			       ibdev_name[0] != '\0' ? ibdev_name : "unknown");
		}
		(void)doca_devinfo_destroy_list(dev_list);
		return result;
	}

	(void)doca_devinfo_destroy_list(dev_list);
	return DOCA_ERROR_NOT_FOUND;
}

static struct device_selector
parse_selector(const char *value)
{
	struct device_selector selector = {
		.type = SELECTOR_ANY,
		.value = NULL,
	};

	if (value == NULL)
		return selector;

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
open_sta_devices(const struct device_selector *ctrl_selector,
		 const struct device_selector *net_selector,
		 struct app_state *app)
{
	doca_error_t result;

	result = open_doca_device(ctrl_selector, true, &app->ctrl_dev);
	if (result != DOCA_SUCCESS)
		return result;

	result = open_doca_device(net_selector, false, &app->net_dev);
	if (result != DOCA_SUCCESS)
		return result;

	return DOCA_SUCCESS;
}

static void
print_usage(const char *argv0)
{
	fprintf(stderr,
		"Usage: %s [control-pci-or-iface] [network-pci-or-iface]\n"
		"       %s --list\n"
		"Examples:\n"
		"  %s ens5008f0np0 endaf0pf0sf88\n"
		"  %s 0000:da:00.0 endaf0pf0sf88\n"
		"  %s mlx5_3 mlx5_2\n",
		argv0, argv0, argv0, argv0, argv0);
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
	struct device_selector ctrl_selector = { .type = SELECTOR_IFACE, .value = "ens5008f0np0" };
	struct device_selector net_selector = { .type = SELECTOR_IFACE, .value = "endaf0pf0sf88" };
	struct app_state app = {0};
	doca_error_t result;

	if (argc == 2 && strcmp(argv[1], "--list") == 0) {
		dump_doca_devices();
		return 0;
	}

	if (argc > 3) {
		print_usage(argv[0]);
		return 1;
	}

	if (argc >= 2)
		ctrl_selector = parse_selector(argv[1]);
	if (argc == 3)
		net_selector = parse_selector(argv[2]);

	result = open_sta_devices(&ctrl_selector, &net_selector, &app);
	if (result != DOCA_SUCCESS) {
		fprintf(stderr, "Failed to open required DOCA device(s): %s\n",
			doca_error_get_descr(result));
		dump_doca_devices();
		return 1;
	}

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
