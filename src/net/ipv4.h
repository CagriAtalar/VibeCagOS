#pragma once
#include "common.h"

/* IP protocol numbers */
#define IP_PROTO_ICMP   1
#define IP_PROTO_TCP    6
#define IP_PROTO_UDP    17

/* IPv4 header (20 bytes minimum, no options) */
struct ipv4_header {
    uint8_t  ver_ihl;       /* Version (4 bits) + IHL (4 bits) */
    uint8_t  tos;           /* Type of service */
    uint16_t total_len;     /* Total length (network byte order) */
    uint16_t id;            /* Identification */
    uint16_t flags_frag;    /* Flags + Fragment offset */
    uint8_t  ttl;           /* Time to live */
    uint8_t  protocol;      /* Protocol (1=ICMP, 6=TCP, 17=UDP) */
    uint16_t checksum;      /* Header checksum */
    uint32_t src_ip;        /* Source IP (network byte order) */
    uint32_t dst_ip;        /* Destination IP (network byte order) */
} __attribute__((packed));

#define IPV4_HDR_LEN  20
#define IPV4_MAX_PAYLOAD 512    /* No fragmentation, hard cap */

/* Calculate IPv4 header checksum */
uint16_t ipv4_checksum(const void *data, uint16_t len);

/* Send an IPv4 packet. ip is in host byte order.
   Returns 0 on success, -1 on error. */
int ipv4_send(uint32_t dst_ip, uint8_t protocol,
              const void *payload, uint16_t payload_len);

/* Handle an incoming IPv4 packet (called from ethernet layer) */
void ipv4_handle_packet(const void *data, uint16_t len);
