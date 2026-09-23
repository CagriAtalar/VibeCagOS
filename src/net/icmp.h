#pragma once
#include "common.h"

/* ICMP message types */
#define ICMP_ECHO_REPLY     0
#define ICMP_ECHO_REQUEST   8

/* ICMP header */
struct icmp_header {
    uint8_t  type;
    uint8_t  code;
    uint16_t checksum;
    uint16_t id;            /* Identifier */
    uint16_t seq;           /* Sequence number */
} __attribute__((packed));

#define ICMP_HDR_LEN  8
#define PING_DATA_LEN 56    /* Standard ping payload */

/* Result of a single ping */
struct ping_result {
    int  received;      /* Number of replies received */
    int  sent;          /* Number of requests sent */
    int  min_ms;        /* Minimum RTT */
    int  max_ms;        /* Maximum RTT */
    int  total_ms;      /* Total RTT (for average) */
};

/* Send count echo requests to ip and collect results.
   ip is in host byte order. Returns 0 if at least one reply, -1 if none. */
int ping(uint32_t ip, int count, struct ping_result *result);

/* Handle incoming ICMP packet (called from IPv4 layer) */
void icmp_handle_packet(uint32_t src_ip, const void *data, uint16_t len);
