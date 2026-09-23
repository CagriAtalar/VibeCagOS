#pragma once
#include "common.h"

/* Ethernet frame header */
#define ETH_ADDR_LEN    6
#define ETH_HDR_LEN     14
#define ETH_MIN_FRAME   60     /* Without FCS (card adds 4-byte FCS) */
#define ETH_MAX_FRAME   1514   /* Without FCS */

/* EtherType values (network byte order aware — use htons) */
#define ETH_TYPE_IPV4   0x0800
#define ETH_TYPE_ARP    0x0806

struct eth_header {
    uint8_t  dst[ETH_ADDR_LEN];
    uint8_t  src[ETH_ADDR_LEN];
    uint16_t ethertype;             /* network byte order */
} __attribute__((packed));

/* Broadcast MAC */
extern const uint8_t ETH_BROADCAST[6];

/* Send an Ethernet frame with given payload.
   Handles padding to 60-byte minimum. */
int eth_send(const uint8_t dst[6], uint16_t ethertype,
             const void *payload, uint16_t payload_len);

/* Process a received raw frame from NIC.
   Dispatches to ARP / IPv4 handlers. */
void eth_recv_process(const void *frame, uint16_t len);

/* Poll NIC for incoming packets and process them */
void net_poll(void);
