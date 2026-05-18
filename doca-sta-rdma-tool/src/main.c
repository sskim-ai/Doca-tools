#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <doca_ctx.h>
#include <doca_dev.h>
#include <doca_error.h>
#include <doca_pe.h>
#include <doca_sta.h>

struct config {
	const char *pf_dev;
	const char *sf_dev;
};

struct app_state {
	struct config cfg;
	struct doca_dev *pf_dev;
	struct doca_dev *sf_dev;
	struct doca_sta *sta;
	struct doca_ctx *main_ctx;
	struct doca_pe *main_pe;
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
	if (app->main_ctx != NULL) {
		(void)doca_ctx_stop(app->main_ctx);
		for (int i = 0; i < 5000; ++i)
			(void)doca_pe_progress(app->main_pe);
	}

	if (app->main_pe != NULL)
		(void)doca_pe_destroy(app->main_pe);

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
		"  %s --pf-dev <PCI_BDF> --sf-dev <PCI_BDF|IFACE|IBDEV>\n"
		"  %s --list\n",
		prog, prog);
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

		fprintf(stderr, "unknown or incomplete arg: %s\n", argv[i]);
		return -1;
	}

	if (cfg->pf_dev == NULL || cfg->sf_dev == NULL)
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

	parse_rc = parse_args(argc, argv, &app.cfg);
	if (parse_rc > 0)
		return 0;
	if (parse_rc < 0) {
		usage(argv[0]);
		return 1;
	}

	pf_selector = parse_selector(app.cfg.pf_dev);
	sf_selector = parse_selector(app.cfg.sf_dev);

	result = open_local_dev(&pf_selector, &app.pf_dev);
	if (result != DOCA_SUCCESS) {
		fprintf(stderr, "Failed to open PF device: %s\n", doca_error_get_descr(result));
		dump_doca_devices();
		return 1;
	}

	result = open_local_dev(&sf_selector, &app.sf_dev);
	if (result != DOCA_SUCCESS) {
		fprintf(stderr, "Failed to open SF device: %s\n", doca_error_get_descr(result));
		cleanup(&app);
		dump_doca_devices();
		return 1;
	}

	dump_devinfo("pf_dev", app.pf_dev);
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

	result = doca_sta_add_dev(app.sta, app.sf_dev);
	if (result != DOCA_SUCCESS) {
		fprintf(stderr, "doca_sta_add_dev failed: %s\n", doca_error_get_descr(result));
		cleanup(&app);
		return 1;
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

	result = doca_ctx_start(app.main_ctx);
	if (result != DOCA_SUCCESS) {
		fprintf(stderr, "doca_ctx_start failed immediately: %s\n", doca_error_get_descr(result));
		dump_ctx_state("main_ctx(after failed start)", app.main_ctx);
		progress_ctx_for_debug(app.main_pe, app.main_ctx, "main_ctx(post-fail)", 500);
		cleanup(&app);
		return 1;
	}

	dump_ctx_state("main_ctx(after start call)", app.main_ctx);
	progress_ctx_for_debug(app.main_pe, app.main_ctx, "main_ctx(post-start)", 1000);

	result = wait_until_running(app.main_pe, app.main_ctx, "main_ctx");
	if (result != DOCA_SUCCESS) {
		fprintf(stderr, "main ctx did not reach RUNNING: %s\n", doca_error_get_descr(result));
		cleanup(&app);
		return 1;
	}

	printf("[OK] main STA context is RUNNING\n");
	cleanup(&app);
	return 0;
}
