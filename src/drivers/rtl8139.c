#include "rtl8139.h"
#include "pci.h"
#include "kernel.h"

/* Static buffers — identity-mapped, physical == virtual */
static uint8_t rx_buffer[RTL_RX_BUF_SIZE] __attribute__((aligned(4)));
static uint8_t tx_buffer[RTL_TX_BUF_SIZE] __attribute__((aligned(4)));

static uint16_t io_base;
static uint8_t  mac_addr[6];
static uint32_t rx_offset;       /* Current read offset in ring buffer */
static int      tx_cur;          /* Current TX descriptor (0-3 round robin) */
static bool     nic_present;

/* Forward declaration */
void printf(const char *fmt, ...);

int nic_init(void) {
    /* Check if PCI found the NIC */
    if (!nic_dev.found) {
        nic_present = false;
        return -1;
    }

    io_base = (uint16_t)nic_dev.io_base;
    nic_present = true;
    rx_offset = 0;
    tx_cur = 0;

    /* Power on */
    outb(io_base + RTL_CONFIG1, 0x00);

    /* Software reset */
    outb(io_base + RTL_CMD, RTL_CMD_RESET);

    /* Wait for reset to complete (bit clears), with timeout */
    int timeout = 100000;
    while ((inb(io_base + RTL_CMD) & RTL_CMD_RESET) && timeout > 0) {
        io_wait();
        timeout--;
    }
    if (timeout <= 0) {
        printf("RTL8139: Reset timeout\n");
        nic_present = false;
        return -1;
    }

    /* Read MAC address from IDR0-IDR5 */
    for (int i = 0; i < 6; i++) {
        mac_addr[i] = inb(io_base + RTL_IDR0 + i);
    }

    /* Set RX buffer physical address (identity-mapped) */
    outl(io_base + RTL_RBSTART, (uint32_t)rx_buffer);

    /* Disable all interrupts — we use polling only */
    outw(io_base + RTL_IMR, 0x0000);

    /* Clear any pending interrupt status */
    outw(io_base + RTL_ISR, 0xFFFF);

    /* Configure RX: accept physical match + broadcast, wrap mode */
    outl(io_base + RTL_RCR, RTL_RCR_APM | RTL_RCR_AB | RTL_RCR_WRAP);

    /* Enable RX and TX */
    outb(io_base + RTL_CMD, RTL_CMD_RE | RTL_CMD_TE);

    printf("RTL8139: MAC %d:%d:%d:%d:%d:%d  IO=0x%x  NIC ready\n",
           mac_addr[0], mac_addr[1], mac_addr[2],
           mac_addr[3], mac_addr[4], mac_addr[5],
           (unsigned)io_base);

    return 0;
}

int nic_send(const void *data, uint16_t len) {
    if (!nic_present || len == 0)
        return -1;

    /* Cap frame length */
    if (len > RTL_TX_BUF_SIZE)
        len = RTL_TX_BUF_SIZE;

    /* Copy data to TX buffer */
    memcpy(tx_buffer, data, len);

    /* Set TX start address for current descriptor */
    outl(io_base + RTL_TSAD0 + (tx_cur * 4), (uint32_t)tx_buffer);

    /* Write length to TX status register — clears OWN bit, starts DMA */
    /* Bits 12:0 = size, bit 13 (OWN) is cleared by writing */
    outl(io_base + RTL_TSD0 + (tx_cur * 4), (uint32_t)len);

    /* Poll for TOK (transmit OK) with timeout */
    int timeout = 2000000;
    while (timeout > 0) {
        uint32_t status = inl(io_base + RTL_TSD0 + (tx_cur * 4));
        if (status & RTL_TSD_TOK)
            break;
        io_wait();
        timeout--;
    }

    if (timeout <= 0) {
        printf("RTL8139: TX timeout\n");
        return -1;
    }

    /* Round-robin to next descriptor */
    tx_cur = (tx_cur + 1) % 4;

    return 0;
}

int nic_recv(void *buf, uint16_t max_len) {
    if (!nic_present)
        return -1;

    /* Check if buffer is empty (BUFE bit in CMD register) */
    uint8_t cmd = inb(io_base + RTL_CMD);
    if (cmd & RTL_CMD_BUFE)
        return 0;   /* No packet available */

    /* Clear any interrupt status bits */
    outw(io_base + RTL_ISR, 0xFFFF);

    /* Read packet header from ring buffer at current offset.
     * Header format: [uint16_t status] [uint16_t length] [payload...] */
    uint8_t *ring = rx_buffer + rx_offset;

    uint16_t status = *(uint16_t *)(ring);
    uint16_t pkt_len = *(uint16_t *)(ring + 2);

    /* Check receive OK */
    if (!(status & RTL_ROK)) {
        /* Bad packet — skip it */
        /* Advance past header + length, aligned to 4 */
        rx_offset = (rx_offset + pkt_len + 4 + 3) & ~3;
        rx_offset %= (RTL_RX_BUF_SIZE - 1500);
        outw(io_base + RTL_CAPR, (uint16_t)(rx_offset - 16));
        return -1;
    }

    /* pkt_len includes 4-byte CRC at the end */
    uint16_t data_len = pkt_len - 4;  /* strip CRC */

    /* Copy payload (skip 4-byte ring header) */
    uint16_t copy_len = data_len;
    if (copy_len > max_len)
        copy_len = max_len;

    memcpy(buf, ring + 4, copy_len);

    /* Advance rx_offset: header(4) + pkt_len, aligned to dword */
    rx_offset = (rx_offset + pkt_len + 4 + 3) & ~3;
    rx_offset %= (RTL_RX_BUF_SIZE - 1500);

    /* Update CAPR (must be offset - 16, per spec) */
    outw(io_base + RTL_CAPR, (uint16_t)(rx_offset - 16));

    return copy_len;
}

void nic_get_mac(uint8_t mac[6]) {
    memcpy(mac, mac_addr, 6);
}
