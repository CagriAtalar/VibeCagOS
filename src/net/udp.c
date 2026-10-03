/*
 * VibeCagOS — UDP (User Datagram Protocol) Stack Implementation
 */

#include "udp.h"
#include "ipv4.h"
#include "ethernet.h"
#include "byteorder.h"
#include "kernel/klog.h"
#include "kernel/common.h"

#define MAX_LISTENERS 8

struct udp_listener {
    uint16_t       port;
    udp_callback_t callback;
    bool           active;
};

static struct udp_listener listeners[MAX_LISTENERS];
static struct net_socket   sockets[NET_MAX_SOCKETS];
static uint16_t            next_ephemeral = 49152;

void udp_init(void) {
    memset(listeners, 0, sizeof(listeners));
    memset(sockets, 0, sizeof(sockets));
    KINFO("NET", "UDP protocol stack initialized");
}

int udp_listen(uint16_t port, udp_callback_t callback) {
    for (int i = 0; i < MAX_LISTENERS; i++) {
        if (!listeners[i].active) {
            listeners[i].port = port;
            listeners[i].callback = callback;
            listeners[i].active = true;
            return 0;
        }
    }
    return -1;
}

void udp_unlisten(uint16_t port) {
    for (int i = 0; i < MAX_LISTENERS; i++) {
        if (listeners[i].active && listeners[i].port == port) {
            listeners[i].active = false;
        }
    }
}

int udp_send(uint32_t dst_ip, uint16_t src_port, uint16_t dst_port,
             const void *payload, uint16_t len) {
    if (len > UDP_MAX_PAYLOAD) {
        return -1;
    }

    uint8_t packet[UDP_HDR_LEN + UDP_MAX_PAYLOAD];
    struct udp_header *hdr = (struct udp_header *)packet;

    hdr->src_port = htons(src_port);
    hdr->dst_port = htons(dst_port);
    hdr->length   = htons(UDP_HDR_LEN + len);
    hdr->checksum = 0; /* Optional in IPv4 */

    if (len > 0 && payload) {
        memcpy(packet + UDP_HDR_LEN, payload, len);
    }

    return ipv4_send(dst_ip, IP_PROTO_UDP, packet, UDP_HDR_LEN + len);
}

void udp_handle_packet(uint32_t src_ip, const void *data, uint16_t len) {
    if (len < UDP_HDR_LEN) return;

    const struct udp_header *hdr = (const struct udp_header *)data;
    uint16_t src_port = ntohs(hdr->src_port);
    uint16_t dst_port = ntohs(hdr->dst_port);
    uint16_t pkt_len  = ntohs(hdr->length);

    if (pkt_len < UDP_HDR_LEN || pkt_len > len) return;
    uint16_t data_len = pkt_len - UDP_HDR_LEN;
    const void *payload = (const uint8_t *)data + UDP_HDR_LEN;

    /* Notify listener callbacks */
    for (int i = 0; i < MAX_LISTENERS; i++) {
        if (listeners[i].active && listeners[i].port == dst_port) {
            if (listeners[i].callback) {
                listeners[i].callback(src_ip, src_port, payload, data_len);
            }
        }
    }

    /* Store into bound sockets */
    for (int i = 0; i < NET_MAX_SOCKETS; i++) {
        if (sockets[i].used && sockets[i].port == dst_port) {
            uint16_t copy_len = data_len;
            if (copy_len > sizeof(sockets[i].rx_buf)) {
                copy_len = sizeof(sockets[i].rx_buf);
            }
            memcpy(sockets[i].rx_buf, payload, copy_len);
            sockets[i].rx_len = copy_len;
            sockets[i].last_src_ip = src_ip;
            sockets[i].last_src_port = src_port;
            sockets[i].has_data = true;
        }
    }
}

int net_socket_open(uint16_t port) {
    for (int i = 0; i < NET_MAX_SOCKETS; i++) {
        if (!sockets[i].used) {
            sockets[i].used = true;
            sockets[i].port = (port != 0) ? port : next_ephemeral++;
            sockets[i].has_data = false;
            sockets[i].rx_len = 0;
            return i;
        }
    }
    return -1;
}

int net_socket_sendto(int sock, uint32_t dst_ip, uint16_t dst_port,
                       const void *data, uint16_t len) {
    if (sock < 0 || sock >= NET_MAX_SOCKETS || !sockets[sock].used) {
        return -1;
    }
    return udp_send(dst_ip, sockets[sock].port, dst_port, data, len);
}

int net_socket_recvfrom(int sock, void *buf, uint16_t max_len,
                         uint32_t *src_ip, uint16_t *src_port) {
    if (sock < 0 || sock >= NET_MAX_SOCKETS || !sockets[sock].used) {
        return -1;
    }

    /* Poll network to process pending frames */
    net_poll();

    if (!sockets[sock].has_data) {
        return 0;
    }

    uint16_t copy_len = sockets[sock].rx_len;
    if (copy_len > max_len) copy_len = max_len;

    memcpy(buf, sockets[sock].rx_buf, copy_len);
    if (src_ip) *src_ip = sockets[sock].last_src_ip;
    if (src_port) *src_port = sockets[sock].last_src_port;

    sockets[sock].has_data = false;
    return (int)copy_len;
}

void net_socket_close(int sock) {
    if (sock >= 0 && sock < NET_MAX_SOCKETS) {
        sockets[sock].used = false;
        sockets[sock].has_data = false;
    }
}
