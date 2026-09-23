#pragma once
#include "common.h"

/* ARP hardware/protocol types */
#define ARP_HW_ETHER    1
#define ARP_PROTO_IPV4  0x0800

/* ARP opcodes */
#define ARP_OP_REQUEST  1
#define ARP_OP_REPLY    2

/* ARP packet for Ethernet + IPv4 */
struct arp_packet {
    uint16_t hw_type;           /* Hardware type (1 = Ethernet) */
    uint16_t proto_type;        /* Protocol type (0x0800 = IPv4) */
    uint8_t  hw_len;            /* Hardware address length (6) */
    uint8_t  proto_len;         /* Protocol address length (4) */
    uint16_t opcode;            /* 1 = request, 2 = reply */
    uint8_t  sender_mac[6];     /* Sender hardware address */
    uint32_t sender_ip;         /* Sender protocol address */
    uint8_t  target_mac[6];     /* Target hardware address */
    uint32_t target_ip;         /* Target protocol address */
} __attribute__((packed));

/* ARP cache entry */
struct arp_entry {
    uint32_t ip;
    uint8_t  mac[6];
    bool     valid;
};

/* Number of ARP cache entries */
#define ARP_CACHE_SIZE 4

/* Resolve IP to MAC address.
   Returns true if resolved (mac_out filled), false if ARP timed out. */
bool arp_resolve(uint32_t ip, uint8_t mac_out[6]);

/* Handle an incoming ARP packet (called from ethernet layer) */
void arp_handle_packet(const void *data, uint16_t len);

/* Lookup ARP cache without sending a request. Returns true if found. */
bool arp_cache_lookup(uint32_t ip, uint8_t mac_out[6]);
