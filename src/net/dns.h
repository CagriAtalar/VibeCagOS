/*
 * VibeCagOS — DNS (Domain Name System) Client
 *
 * Implements basic DNS A-record resolution via UDP port 53.
 */

#pragma once
#include "common.h"

/* Default DNS server in QEMU virtnet */
#define DEFAULT_DNS_IP  ((10u << 24) | (0u << 16) | (2u << 8) | 3u)  /* 10.0.2.3 */

/*
 * dns_resolve — resolve a domain name (e.g., "example.com") to an IPv4 address.
 * Returns 0 on success (with out_ip in host byte order), or negative on error.
 */
int dns_resolve(const char *hostname, uint32_t *out_ip);
