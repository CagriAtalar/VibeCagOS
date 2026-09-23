#include "ethernet.h"
#include "byteorder.h"
#include "arp.h"
#include "ipv4.h"
#include "rtl8139.h"

/* Forward declaration */
void printf(const char *fmt, ...);

const uint8_t ETH_BROADCAST[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

/* Scratch buffer for building outgoing frames */
static uint8_t eth_frame_buf[ETH_MAX_FRAME + 4];

/* Scratch buffer for receiving frames */
static uint8_t eth_recv_buf[ETH_MAX_FRAME + 4];

int eth_send(const uint8_t dst[6], uint16_t ethertype,
             const void *payload, uint16_t payload_len) {
    struct eth_header *hdr = (struct eth_header *)eth_frame_buf;
    uint8_t our_mac[6];

    nic_get_mac(our_mac);

    /* Build Ethernet header */
    memcpy(hdr->dst, dst, ETH_ADDR_LEN);
    memcpy(hdr->src, our_mac, ETH_ADDR_LEN);
    hdr->ethertype = htons(ethertype);

    /* Copy payload after header */
    uint16_t total = ETH_HDR_LEN + payload_len;
    if (payload_len > 0 && payload != NULL) {
        memcpy(eth_frame_buf + ETH_HDR_LEN, payload, payload_len);
    }

    /* Pad to minimum frame size (60 bytes) */
    if (total < ETH_MIN_FRAME) {
        memset(eth_frame_buf + total, 0, ETH_MIN_FRAME - total);
        total = ETH_MIN_FRAME;
    }

    return nic_send(eth_frame_buf, total);
}

void eth_recv_process(const void *frame, uint16_t len) {
    if (len < ETH_HDR_LEN)
        return;

    const struct eth_header *hdr = (const struct eth_header *)frame;
    uint16_t ethertype = ntohs(hdr->ethertype);
    const void *payload = (const uint8_t *)frame + ETH_HDR_LEN;
    uint16_t payload_len = len - ETH_HDR_LEN;

    switch (ethertype) {
        case ETH_TYPE_ARP:
            arp_handle_packet(payload, payload_len);
            break;
        case ETH_TYPE_IPV4:
            ipv4_handle_packet(payload, payload_len);
            break;
        default:
            /* Unknown ethertype — drop */
            break;
    }
}

void net_poll(void) {
    int len = nic_recv(eth_recv_buf, sizeof(eth_recv_buf));
    if (len > 0) {
        eth_recv_process(eth_recv_buf, (uint16_t)len);
    }
}
