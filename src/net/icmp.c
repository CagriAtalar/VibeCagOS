#include "icmp.h"
#include "ipv4.h"
#include "ethernet.h"
#include "byteorder.h"
#include "kernel.h"

/* Forward declaration */
void printf(const char *fmt, ...);

/* State for tracking pending echo replies */
static volatile bool     ping_waiting;
static volatile bool     ping_got_reply;
static volatile uint16_t ping_expect_id;
static volatile uint16_t ping_expect_seq;

/* PIT (Programmable Interval Timer) polling for rough ms timing.
   PIT Channel 0 runs at 1193182 Hz by default (mode 3, divisor 65536).
   One full countdown ≈ 54.9 ms (~18.2 Hz).
   We latch and read the counter to estimate elapsed time. */

#define PIT_FREQ       1193182
#define PIT_CH0_PORT   0x40
#define PIT_CMD_PORT   0x43

/* Read current PIT Channel 0 count (16-bit countdown value) */
static uint16_t pit_read_count(void) {
    /* Latch command for Channel 0 */
    outb(PIT_CMD_PORT, 0x00);
    uint8_t lo = inb(PIT_CH0_PORT);
    uint8_t hi = inb(PIT_CH0_PORT);
    return ((uint16_t)hi << 8) | lo;
}

/* Get a rough "tick" value that increases with time.
   Since PIT counts DOWN, we invert it. */
static uint32_t pit_get_ticks(void) {
    return 0xFFFF - pit_read_count();
}

/* Estimate milliseconds between two tick readings.
   PIT freq = 1193182 Hz → 1 tick ≈ 0.838 µs
   ms = (ticks * 1000) / 1193182 ≈ ticks / 1193 */
static int pit_ticks_to_ms(uint32_t ticks) {
    if (ticks == 0) return 0;
    return (int)((ticks * 1000) / PIT_FREQ);
}

void icmp_handle_packet(uint32_t src_ip, const void *data, uint16_t len) {
    if (len < ICMP_HDR_LEN)
        return;

    const struct icmp_header *hdr = (const struct icmp_header *)data;

    /* Verify ICMP checksum */
    if (ipv4_checksum(data, len) != 0)
        return;  /* Bad checksum */

    (void)src_ip;

    switch (hdr->type) {
        case ICMP_ECHO_REQUEST:
            /* Reply to echo requests directed at us */
            {
                /* Build echo reply with same data */
                static uint8_t reply_buf[ICMP_HDR_LEN + IPV4_MAX_PAYLOAD];
                uint16_t data_len = len;
                if (data_len > sizeof(reply_buf))
                    data_len = sizeof(reply_buf);

                memcpy(reply_buf, data, data_len);

                struct icmp_header *reply = (struct icmp_header *)reply_buf;
                reply->type = ICMP_ECHO_REPLY;
                reply->code = 0;
                reply->checksum = 0;
                reply->checksum = ipv4_checksum(reply_buf, data_len);

                ipv4_send(src_ip, IP_PROTO_ICMP, reply_buf, data_len);
            }
            break;

        case ICMP_ECHO_REPLY:
            /* Check if this matches our pending ping */
            if (ping_waiting &&
                ntohs(hdr->id) == ping_expect_id &&
                ntohs(hdr->seq) == ping_expect_seq) {
                ping_got_reply = true;
            }
            break;

        default:
            break;
    }
}

int ping(uint32_t ip, int count, struct ping_result *result) {
    result->received = 0;
    result->sent = 0;
    result->min_ms = 99999;
    result->max_ms = 0;
    result->total_ms = 0;

    /* Use our process-specific ID */
    uint16_t id = 0xCA90;  /* CAgOS identifier */

    for (int seq = 0; seq < count; seq++) {
        /* Build ICMP echo request */
        uint8_t pkt[ICMP_HDR_LEN + PING_DATA_LEN];
        struct icmp_header *hdr = (struct icmp_header *)pkt;

        hdr->type = ICMP_ECHO_REQUEST;
        hdr->code = 0;
        hdr->checksum = 0;
        hdr->id  = htons(id);
        hdr->seq = htons((uint16_t)seq);

        /* Fill payload with pattern */
        for (int i = 0; i < PING_DATA_LEN; i++) {
            pkt[ICMP_HDR_LEN + i] = (uint8_t)(i & 0xFF);
        }

        /* Calculate ICMP checksum */
        hdr->checksum = ipv4_checksum(pkt, sizeof(pkt));

        /* Set up reply tracking */
        ping_expect_id = id;
        ping_expect_seq = (uint16_t)seq;
        ping_got_reply = false;
        ping_waiting = true;

        /* Record start time */
        uint32_t start_tick = pit_get_ticks();
        uint32_t wrap_count = 0;
        uint16_t last_raw = pit_read_count();

        /* Send the echo request */
        if (ipv4_send(ip, IP_PROTO_ICMP, pkt, sizeof(pkt)) < 0) {
            result->sent++;
            ping_waiting = false;
            continue;
        }
        result->sent++;

        /* Wait for reply — poll for up to ~2 seconds */
        bool got_it = false;
        for (int t = 0; t < 2000000; t++) {
            net_poll();

            if (ping_got_reply) {
                got_it = true;
                break;
            }

            /* Track PIT wraps for timing */
            uint16_t cur_raw = pit_read_count();
            if (cur_raw > last_raw) {
                wrap_count++;  /* Counter wrapped around */
            }
            last_raw = cur_raw;
        }

        ping_waiting = false;

        if (got_it) {
            uint32_t end_tick = pit_get_ticks();
            uint32_t elapsed_ticks;

            /* Handle tick wraparound */
            if (end_tick >= start_tick) {
                elapsed_ticks = end_tick - start_tick + (wrap_count * 0xFFFF);
            } else {
                elapsed_ticks = (0xFFFF - start_tick) + end_tick +
                                (wrap_count > 0 ? (wrap_count - 1) * 0xFFFF : 0);
            }

            int ms = pit_ticks_to_ms(elapsed_ticks);
            if (ms == 0) ms = 1;  /* At least 1ms if we got a reply */

            result->received++;
            result->total_ms += ms;
            if (ms < result->min_ms) result->min_ms = ms;
            if (ms > result->max_ms) result->max_ms = ms;

            printf("64 bytes from %d.%d.%d.%d: seq=%d time=%dms\n",
                   (ip >> 24) & 0xFF, (ip >> 16) & 0xFF,
                   (ip >> 8) & 0xFF, ip & 0xFF,
                   seq, ms);
        }
    }

    if (result->received == 0) {
        result->min_ms = 0;
    }

    return result->received > 0 ? 0 : -1;
}
