/*
 * VibeCagOS — DNS Resolver Client Implementation
 */

#include "dns.h"
#include "udp.h"
#include "byteorder.h"
#include "ethernet.h"
#include "kernel/kernel.h"
#include "kernel/klog.h"

struct dns_header {
    uint16_t id;
    uint16_t flags;
    uint16_t qdcount;
    uint16_t ancount;
    uint16_t nscount;
    uint16_t arcount;
} __attribute__((packed));

static volatile bool     dns_reply_received = false;
static volatile uint16_t dns_expect_id = 0;
static uint32_t          dns_resolved_ip = 0;

static void dns_udp_callback(uint32_t src_ip, uint16_t src_port,
                             const void *data, uint16_t len) {
    (void)src_ip;
    (void)src_port;

    if (len < sizeof(struct dns_header)) return;

    const struct dns_header *hdr = (const struct dns_header *)data;
    if (ntohs(hdr->id) != dns_expect_id) return;

    uint16_t ancount = ntohs(hdr->ancount);
    if (ancount == 0) return;

    const uint8_t *ptr = (const uint8_t *)data + sizeof(struct dns_header);
    const uint8_t *end = (const uint8_t *)data + len;

    /* Skip question section */
    while (ptr < end && *ptr != 0) {
        if ((*ptr & 0xC0) == 0xC0) { ptr += 2; break; }
        ptr += 1 + *ptr;
    }
    if (ptr < end && *ptr == 0) ptr++;
    ptr += 4; /* skip QTYPE and QCLASS */

    /* Parse answer records */
    for (int i = 0; i < ancount && ptr < end; i++) {
        /* Skip name (could be pointer 0xC0xx or label string) */
        if ((*ptr & 0xC0) == 0xC0) {
            ptr += 2;
        } else {
            while (ptr < end && *ptr != 0) {
                ptr += 1 + *ptr;
            }
            if (ptr < end) ptr++;
        }

        if (ptr + 10 > end) break;
        uint16_t type = (ptr[0] << 8) | ptr[1];
        /* uint16_t class = (ptr[2] << 8) | ptr[3]; */
        /* uint32_t ttl   = (ptr[4] << 24) | (ptr[5] << 16) | (ptr[6] << 8) | ptr[7]; */
        uint16_t rdlen = (ptr[8] << 8) | ptr[9];
        ptr += 10;

        if (type == 1 && rdlen == 4 && ptr + 4 <= end) {
            /* Type A record (IPv4 address) */
            dns_resolved_ip = ((uint32_t)ptr[0] << 24) |
                              ((uint32_t)ptr[1] << 16) |
                              ((uint32_t)ptr[2] << 8)  |
                              (uint32_t)ptr[3];
            dns_reply_received = true;
            return;
        }
        ptr += rdlen;
    }
}

int dns_resolve(const char *hostname, uint32_t *out_ip) {
    if (!hostname || !*hostname || !out_ip) return -1;

    uint8_t packet[256];
    memset(packet, 0, sizeof(packet));

    static uint16_t txid = 0x1337;
    dns_expect_id = ++txid;
    dns_reply_received = false;
    dns_resolved_ip = 0;

    struct dns_header *hdr = (struct dns_header *)packet;
    hdr->id      = htons(dns_expect_id);
    hdr->flags   = htons(0x0100); /* Standard query, recursion desired */
    hdr->qdcount = htons(1);

    /* Encode hostname into QNAME format: 3www6google3com0 */
    uint8_t *qname = packet + sizeof(struct dns_header);
    const char *src = hostname;

    while (*src) {
        const char *dot = strchr(src, '.');
        size_t label_len = dot ? (size_t)(dot - src) : strlen(src);
        if (label_len > 63) return -1;

        *qname++ = (uint8_t)label_len;
        memcpy(qname, src, label_len);
        qname += label_len;

        if (dot) src = dot + 1;
        else break;
    }
    *qname++ = 0; /* Null terminator */

    /* QTYPE = 1 (A), QCLASS = 1 (IN) */
    *qname++ = 0; *qname++ = 1;
    *qname++ = 0; *qname++ = 1;

    uint16_t packet_len = (uint16_t)(qname - packet);

    uint16_t local_port = 53000 + (txid % 1000);
    udp_listen(local_port, dns_udp_callback);

    /* Send query to DNS server */
    udp_send(DEFAULT_DNS_IP, local_port, 53, packet, packet_len);

    /* Poll network with timeout (up to 2 seconds = 2000 ms) */
    uint32_t start_ms = get_uptime_ms();
    while (!dns_reply_received && (get_uptime_ms() - start_ms) < 2000) {
        net_poll();
        io_wait();
    }

    udp_unlisten(local_port);

    if (dns_reply_received) {
        *out_ip = dns_resolved_ip;
        return 0;
    }

    return -1;
}
