#include "pci.h"
#include "kernel.h"

/* Global NIC device info */
struct pci_device nic_dev;

uint32_t pci_config_read32(uint8_t bus, uint8_t dev, uint8_t func,
                           uint8_t offset) {
    uint32_t addr = (1u << 31)            /* enable bit */
                  | ((uint32_t)bus << 16)
                  | ((uint32_t)dev << 11)
                  | ((uint32_t)func << 8)
                  | (offset & 0xFC);       /* align to dword */
    outl(PCI_CONFIG_ADDR, addr);
    return inl(PCI_CONFIG_DATA);
}

void pci_config_write32(uint8_t bus, uint8_t dev, uint8_t func,
                        uint8_t offset, uint32_t value) {
    uint32_t addr = (1u << 31)
                  | ((uint32_t)bus << 16)
                  | ((uint32_t)dev << 11)
                  | ((uint32_t)func << 8)
                  | (offset & 0xFC);
    outl(PCI_CONFIG_ADDR, addr);
    outl(PCI_CONFIG_DATA, value);
}

struct pci_device pci_find_device(uint16_t vendor_id, uint16_t device_id) {
    struct pci_device result;
    memset(&result, 0, sizeof(result));
    result.found = false;

    for (int bus = 0; bus < 256; bus++) {
        for (int dev = 0; dev < 32; dev++) {
            int max_func = 1;

            /* Check if device exists */
            uint32_t id_reg = pci_config_read32(bus, dev, 0, PCI_VENDOR_ID);
            uint16_t vid = id_reg & 0xFFFF;
            if (vid == 0xFFFF)
                continue;

            /* Check multifunction bit (header type bit 7) */
            uint32_t hdr = pci_config_read32(bus, dev, 0, PCI_HEADER_TYPE);
            if (((hdr >> 16) & 0x80) != 0)
                max_func = 8;

            for (int func = 0; func < max_func; func++) {
                id_reg = pci_config_read32(bus, dev, func, PCI_VENDOR_ID);
                vid = id_reg & 0xFFFF;
                uint16_t did = (id_reg >> 16) & 0xFFFF;

                if (vid == 0xFFFF)
                    continue;

                if (vid == vendor_id && did == device_id) {
                    result.bus = bus;
                    result.dev = dev;
                    result.func = func;
                    result.vendor_id = vid;
                    result.device_id = did;

                    /* Read BAR0 */
                    uint32_t bar0 = pci_config_read32(bus, dev, func,
                                                      PCI_BAR0);
                    if (bar0 & 1) {
                        /* I/O space: mask bit 0-1 */
                        result.io_base = bar0 & 0xFFFFFFFC;
                    } else {
                        /* Memory-mapped — store raw for now */
                        result.io_base = bar0 & 0xFFFFFFF0;
                    }

                    /* Read IRQ line */
                    uint32_t irq_reg = pci_config_read32(bus, dev, func,
                                                         PCI_IRQ_LINE);
                    result.irq_line = irq_reg & 0xFF;

                    /* Enable I/O space access + bus mastering */
                    uint32_t cmd = pci_config_read32(bus, dev, func,
                                                     PCI_COMMAND);
                    cmd |= PCI_CMD_IO_SPACE | PCI_CMD_BUS_MASTER;
                    pci_config_write32(bus, dev, func, PCI_COMMAND, cmd);

                    result.found = true;
                    return result;
                }
            }
        }
    }

    return result;
}

void pci_init(void) {
    printf("PCI: Scanning bus...\n");

    nic_dev = pci_find_device(RTL8139_VENDOR_ID, RTL8139_DEVICE_ID);

    if (nic_dev.found) {
        printf("PCI: Found RTL8139 at %d:%d.%d  IO=0x%x  IRQ=%d\n",
               nic_dev.bus, nic_dev.dev, nic_dev.func,
               nic_dev.io_base, nic_dev.irq_line);
    } else {
        printf("PCI: No NIC found\n");
    }
}
