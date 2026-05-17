#include <stdint.h>
#include <stdio.h>

#include <doca_dev.h>
#include <doca_error.h>

#define PCI_ADDR_STR_SIZE 32
#define IFACE_NAME_SIZE 64
#define IBDEV_NAME_SIZE 64

static void print_optional_string(const char *label, doca_error_t result, const char *value)
{
    if (result == DOCA_SUCCESS)
        printf("  %s: %s\n", label, value);
    else
        printf("  %s: unavailable (%s)\n", label, doca_error_get_descr(result));
}

int main(void)
{
    struct doca_devinfo **dev_list = NULL;
    uint32_t nb_devs = 0;
    doca_error_t result;

    result = doca_devinfo_create_list(&dev_list, &nb_devs);
    if (result != DOCA_SUCCESS) {
        fprintf(stderr, "Failed to enumerate DOCA devices: %s\n", doca_error_get_descr(result));
        return 1;
    }

    printf("Found %u DOCA device(s).\n", nb_devs);

    for (uint32_t i = 0; i < nb_devs; ++i) {
        char pci_addr[PCI_ADDR_STR_SIZE] = {0};
        char iface_name[IFACE_NAME_SIZE] = {0};
        char ibdev_name[IBDEV_NAME_SIZE] = {0};

        printf("Device %u:\n", i);
        print_optional_string(
            "PCI address",
            doca_devinfo_get_pci_addr_str(dev_list[i], pci_addr),
            pci_addr);
        print_optional_string(
            "Interface",
            doca_devinfo_get_iface_name(dev_list[i], iface_name, sizeof(iface_name)),
            iface_name);
        print_optional_string(
            "IB device",
            doca_devinfo_get_ibdev_name(dev_list[i], ibdev_name, sizeof(ibdev_name)),
            ibdev_name);
    }

    result = doca_devinfo_destroy_list(dev_list);
    if (result != DOCA_SUCCESS) {
        fprintf(stderr, "Failed to destroy DOCA device list: %s\n", doca_error_get_descr(result));
        return 1;
    }

    return 0;
}
