#pragma once
#include "common.h"

/* Static network configuration.
   Change these values here if adding DHCP later. */

#define NET_IP       0x0A00020F   /* 10.0.2.15  (host byte order) */
#define NET_GATEWAY  0x0A000202   /* 10.0.2.2   (host byte order) */
#define NET_NETMASK  0xFFFFFF00   /* 255.255.255.0 */

/* Helper to build IP from octets in host byte order */
#define MAKE_IP(a, b, c, d) \
    (((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) | \
     ((uint32_t)(c) << 8)  |  (uint32_t)(d))
