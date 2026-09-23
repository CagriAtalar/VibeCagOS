#pragma once
#include "common.h"

/* RTL8139 register offsets from I/O base */
#define RTL_IDR0        0x00    /* MAC address bytes 0-5 (6 bytes) */
#define RTL_MAR0        0x08    /* Multicast filter (8 bytes) */
#define RTL_TSD0        0x10    /* TX status descriptor 0 */
#define RTL_TSAD0       0x20    /* TX start address descriptor 0 */
#define RTL_RBSTART     0x30    /* RX buffer start address */
#define RTL_CMD         0x37    /* Command register */
#define RTL_CAPR        0x38    /* Current address of packet read */
#define RTL_CBR         0x3A    /* Current buffer address */
#define RTL_IMR         0x3C    /* Interrupt mask register */
#define RTL_ISR         0x3E    /* Interrupt status register */
#define RTL_TCR         0x40    /* TX config register */
#define RTL_RCR         0x44    /* RX config register */
#define RTL_CONFIG1     0x52    /* Configuration register 1 */

/* Command register bits */
#define RTL_CMD_RESET   0x10
#define RTL_CMD_RE      0x08    /* Receiver enable */
#define RTL_CMD_TE      0x04    /* Transmitter enable */
#define RTL_CMD_BUFE    0x01    /* Buffer empty */

/* TX status register bits */
#define RTL_TSD_OWN     (1 << 13)  /* DMA completed */
#define RTL_TSD_TOK     (1 << 15)  /* TX OK */

/* RX config bits */
#define RTL_RCR_AAP     (1 << 0)   /* Accept all packets (promiscuous) */
#define RTL_RCR_APM     (1 << 1)   /* Accept physical match */
#define RTL_RCR_AM      (1 << 2)   /* Accept multicast */
#define RTL_RCR_AB      (1 << 3)   /* Accept broadcast */
#define RTL_RCR_WRAP    (1 << 7)   /* Wrap around buffer */

/* RX packet header status bits */
#define RTL_ROK         (1 << 0)   /* Receive OK */

/* Buffer sizes */
#define RTL_RX_BUF_SIZE  (8192 + 16 + 1500)  /* 8K + 16 + extra */
#define RTL_TX_BUF_SIZE  1536

/* Initialize NIC using PCI-discovered I/O base. Returns 0 on success. */
int nic_init(void);

/* Send a raw Ethernet frame. Returns 0 on success, -1 on error/timeout. */
int nic_send(const void *data, uint16_t len);

/* Receive a raw Ethernet frame (non-blocking).
   Returns bytes copied (>0), 0 if no packet, -1 on error. */
int nic_recv(void *buf, uint16_t max_len);

/* Get MAC address (6 bytes) */
void nic_get_mac(uint8_t mac[6]);
