#pragma once
#include "common.h"

/* PCI configuration space ports */
#define PCI_CONFIG_ADDR  0xCF8
#define PCI_CONFIG_DATA  0xCFC

/* PCI header offsets */
#define PCI_VENDOR_ID    0x00
#define PCI_DEVICE_ID    0x02
#define PCI_COMMAND      0x04
#define PCI_STATUS       0x06
#define PCI_HEADER_TYPE  0x0E
#define PCI_BAR0         0x10
#define PCI_IRQ_LINE     0x3C

/* PCI command register bits */
#define PCI_CMD_IO_SPACE     (1 << 0)
#define PCI_CMD_BUS_MASTER   (1 << 2)

/* RTL8139 identifiers */
#define RTL8139_VENDOR_ID  0x10EC
#define RTL8139_DEVICE_ID  0x8139

/* PCI device info returned by scan */
struct pci_device {
    uint8_t  bus;
    uint8_t  dev;
    uint8_t  func;
    uint16_t vendor_id;
    uint16_t device_id;
    uint32_t io_base;
    uint8_t  irq_line;
    bool     found;
};

/* Read 32-bit value from PCI config space */
uint32_t pci_config_read32(uint8_t bus, uint8_t dev, uint8_t func,
                           uint8_t offset);

/* Write 32-bit value to PCI config space */
void pci_config_write32(uint8_t bus, uint8_t dev, uint8_t func,
                        uint8_t offset, uint32_t value);

/* Scan PCI bus for a device by vendor+device ID */
struct pci_device pci_find_device(uint16_t vendor_id, uint16_t device_id);

/* Initialize PCI subsystem and scan for known devices */
void pci_init(void);

/* Global: detected NIC device info (valid if nic_dev.found == true) */
extern struct pci_device nic_dev;
