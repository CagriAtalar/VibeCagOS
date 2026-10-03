/*
 * VibeCagOS — UDP (User Datagram Protocol) Stack
 *
 * Implements UDP packet construction, transmission, reception,
 * and a minimal socket-like API.
 */

#pragma once
#include "common.h"

#define UDP_HDR_LEN  8
#define UDP_MAX_PAYLOAD (512 - UDP_HDR_LEN)

struct udp_header {
    uint16_t src_port;
    uint16_t dst_port;
    uint16_t length;    /* header + data in bytes */
    uint16_t checksum;  /* 0 if unused */
} __attribute__((packed));

typedef void (*udp_callback_t)(uint32_t src_ip, uint16_t src_port,
                               const void *data, uint16_t len);

/* Initialize UDP subsystem */
void udp_init(void);

/* Send a raw UDP datagram */
int udp_send(uint32_t dst_ip, uint16_t src_port, uint16_t dst_port,
             const void *payload, uint16_t len);

/* Handle an incoming UDP packet from IPv4 */
void udp_handle_packet(uint32_t src_ip, const void *data, uint16_t len);

/* Register a callback for incoming datagrams on a specific local port */
int udp_listen(uint16_t port, udp_callback_t callback);

/* Unregister listener */
void udp_unlisten(uint16_t port);

/* Minimal socket API */
#define NET_MAX_SOCKETS 8

struct net_socket {
    bool     used;
    uint16_t port;
    uint32_t last_src_ip;
    uint16_t last_src_port;
    uint8_t  rx_buf[512];
    uint16_t rx_len;
    bool     has_data;
};

int  net_socket_open(uint16_t port);
int  net_socket_sendto(int sock, uint32_t dst_ip, uint16_t dst_port,
                       const void *data, uint16_t len);
int  net_socket_recvfrom(int sock, void *buf, uint16_t max_len,
                         uint32_t *src_ip, uint16_t *src_port);
void net_socket_close(int sock);
