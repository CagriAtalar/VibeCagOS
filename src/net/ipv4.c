#include "ipv4.h"
#include "ethernet.h"
#include "arp.h"
#include "byteorder.h"
#include "netconfig.h"
#include "icmp.h"
#include "udp.h"

/* Forward declaration */
void printf(const char *fmt, ...);

/* Packet ID counter */
static uint16_t ip_id_counter = 1;

uint16_t ipv4_checksum(const void *data, uint16_t len) {
    const uint16_t *words = (const uint16_t *)data;
    uint32_t sum = 0;

    while (len > 1) {
        sum += *words++;
        len -= 2;
    }

    /* Handle odd byte */
    if (len == 1) {
        sum += *(const uint8_t *)words;
    }

    /* Fold 32-bit sum to 16 bits */
    while (sum >> 16) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }

    return (uint16_t)(~sum);
}

int ipv4_send(uint32_t dst_ip, uint8_t protocol,
              const void *payload, uint16_t payload_len) {
    /* Check payload size limit (no fragmentation) */
    if (payload_len > IPV4_MAX_PAYLOAD) {
        printf("IPv4: Payload too large (%d > %d)\n",
               payload_len, IPV4_MAX_PAYLOAD);
        return -1;
    }

    /* Build packet in a static buffer */
    static uint8_t ip_buf[IPV4_HDR_LEN + IPV4_MAX_PAYLOAD];

    struct ipv4_header *hdr = (struct ipv4_header *)ip_buf;
    memset(hdr, 0, IPV4_HDR_LEN);

    hdr->ver_ihl   = 0x45;     /* IPv4, 5 dwords (20 bytes) */
    hdr->tos       = 0;
    hdr->total_len = htons(IPV4_HDR_LEN + payload_len);
    hdr->id        = htons(ip_id_counter++);
    hdr->flags_frag = htons(0x4000);  /* Don't Fragment flag */
    hdr->ttl       = 64;
    hdr->protocol  = protocol;
    hdr->checksum  = 0;        /* Will be calculated below */
    hdr->src_ip    = htonl(NET_IP);
    hdr->dst_ip    = htonl(dst_ip);

    /* Calculate header checksum */
    hdr->checksum = ipv4_checksum(hdr, IPV4_HDR_LEN);

    /* Copy payload */
    if (payload_len > 0) {
        memcpy(ip_buf + IPV4_HDR_LEN, payload, payload_len);
    }

    /* Determine next-hop: same subnet → direct, else → gateway */
    uint32_t next_hop;
    if ((dst_ip & NET_NETMASK) == (NET_IP & NET_NETMASK)) {
        next_hop = dst_ip;
    } else {
        next_hop = NET_GATEWAY;
    }

    /* Resolve MAC via ARP */
    uint8_t dst_mac[6];
    if (!arp_resolve(next_hop, dst_mac)) {
        return -1;  /* ARP resolution failed */
    }

    /* Send via Ethernet */
    return eth_send(dst_mac, ETH_TYPE_IPV4,
                    ip_buf, IPV4_HDR_LEN + payload_len);
}

void ipv4_handle_packet(const void *data, uint16_t len) {
    if (len < IPV4_HDR_LEN)
        return;

    const struct ipv4_header *hdr = (const struct ipv4_header *)data;

    /* Basic validation */
    uint8_t version = (hdr->ver_ihl >> 4) & 0x0F;
    uint8_t ihl = hdr->ver_ihl & 0x0F;

    if (version != 4)
        return;

    if (ihl < 5)
        return;

    uint16_t hdr_len = (uint16_t)ihl * 4;
    uint16_t total_len = ntohs(hdr->total_len);

    if (total_len > len)
        return;

    /* Verify header checksum */
    if (ipv4_checksum(hdr, hdr_len) != 0)
        return;   /* Bad checksum — drop silently */

    /* Check destination: must be for us or broadcast */
    uint32_t dst = ntohl(hdr->dst_ip);
    if (dst != NET_IP && dst != 0xFFFFFFFF)
        return;

    uint32_t src_ip = ntohl(hdr->src_ip);
    const void *payload = (const uint8_t *)data + hdr_len;
    uint16_t payload_len = total_len - hdr_len;

    /* Dispatch based on protocol */
    switch (hdr->protocol) {
        case IP_PROTO_ICMP:
            icmp_handle_packet(src_ip, payload, payload_len);
            break;
        case IP_PROTO_UDP:
            udp_handle_packet(src_ip, payload, payload_len);
            break;
        default:
            /* TCP not yet implemented — drop */
            break;
    }
}
