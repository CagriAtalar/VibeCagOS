#include "arp.h"
#include "ethernet.h"
#include "byteorder.h"
#include "netconfig.h"
#include "rtl8139.h"

/* Forward declaration */
void printf(const char *fmt, ...);

/* Static ARP cache — round-robin insertion */
static struct arp_entry arp_cache[ARP_CACHE_SIZE];
static int arp_cache_next = 0;  /* next slot for round-robin */

/* Add/update an entry in the ARP cache */
static void arp_cache_add(uint32_t ip, const uint8_t mac[6]) {
    /* First check if IP already cached */
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (arp_cache[i].valid && arp_cache[i].ip == ip) {
            memcpy(arp_cache[i].mac, mac, 6);
            return;
        }
    }

    /* Round-robin insert into next slot */
    arp_cache[arp_cache_next].ip = ip;
    memcpy(arp_cache[arp_cache_next].mac, mac, 6);
    arp_cache[arp_cache_next].valid = true;
    arp_cache_next = (arp_cache_next + 1) % ARP_CACHE_SIZE;
}

bool arp_cache_lookup(uint32_t ip, uint8_t mac_out[6]) {
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (arp_cache[i].valid && arp_cache[i].ip == ip) {
            memcpy(mac_out, arp_cache[i].mac, 6);
            return true;
        }
    }
    return false;
}

/* Send an ARP request for the given IP */
static void arp_send_request(uint32_t target_ip) {
    struct arp_packet pkt;
    uint8_t our_mac[6];

    nic_get_mac(our_mac);

    pkt.hw_type    = htons(ARP_HW_ETHER);
    pkt.proto_type = htons(ARP_PROTO_IPV4);
    pkt.hw_len     = 6;
    pkt.proto_len  = 4;
    pkt.opcode     = htons(ARP_OP_REQUEST);

    memcpy(pkt.sender_mac, our_mac, 6);
    pkt.sender_ip = htonl(NET_IP);

    memset(pkt.target_mac, 0, 6);
    pkt.target_ip = htonl(target_ip);

    eth_send(ETH_BROADCAST, ETH_TYPE_ARP, &pkt, sizeof(pkt));
}

/* Send an ARP reply to a specific MAC/IP */
static void arp_send_reply(const uint8_t dst_mac[6], uint32_t dst_ip) {
    struct arp_packet pkt;
    uint8_t our_mac[6];

    nic_get_mac(our_mac);

    pkt.hw_type    = htons(ARP_HW_ETHER);
    pkt.proto_type = htons(ARP_PROTO_IPV4);
    pkt.hw_len     = 6;
    pkt.proto_len  = 4;
    pkt.opcode     = htons(ARP_OP_REPLY);

    memcpy(pkt.sender_mac, our_mac, 6);
    pkt.sender_ip = htonl(NET_IP);

    memcpy(pkt.target_mac, dst_mac, 6);
    pkt.target_ip = htonl(dst_ip);

    eth_send(dst_mac, ETH_TYPE_ARP, &pkt, sizeof(pkt));
}

void arp_handle_packet(const void *data, uint16_t len) {
    if (len < sizeof(struct arp_packet))
        return;

    const struct arp_packet *pkt = (const struct arp_packet *)data;

    /* Only handle Ethernet + IPv4 */
    if (ntohs(pkt->hw_type) != ARP_HW_ETHER)
        return;
    if (ntohs(pkt->proto_type) != ARP_PROTO_IPV4)
        return;

    uint32_t sender_ip = ntohl(pkt->sender_ip);
    uint32_t target_ip = ntohl(pkt->target_ip);
    uint16_t opcode = ntohs(pkt->opcode);

    /* Always learn from any ARP traffic that has a valid sender */
    if (sender_ip != 0) {
        arp_cache_add(sender_ip, pkt->sender_mac);
    }

    if (opcode == ARP_OP_REQUEST && target_ip == NET_IP) {
        /* Someone is asking for our MAC — send unicast reply */
        arp_send_reply(pkt->sender_mac, sender_ip);
    }
    /* ARP_OP_REPLY is handled above (cache_add from sender) */
}

bool arp_resolve(uint32_t ip, uint8_t mac_out[6]) {
    /* Check cache first */
    if (arp_cache_lookup(ip, mac_out))
        return true;

    /* Send ARP request and poll for ~1 second.
       We use a simple busy-wait counter as timing reference.
       At ~O2 optimization with io_wait(), this is roughly 1s. */
    arp_send_request(ip);

    for (int attempt = 0; attempt < 500000; attempt++) {
        /* Poll NIC for any incoming packets */
        net_poll();

        /* Check if we got the reply */
        if (arp_cache_lookup(ip, mac_out))
            return true;

        /* Small delay to avoid hammering the NIC */
        for (volatile int d = 0; d < 10; d++)
            ;
    }

    return false;
}
