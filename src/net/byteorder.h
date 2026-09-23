#pragma once
#include "common.h"

/* x86 is little-endian; network byte order is big-endian.
   These inline helpers perform byte-swapping. */

static inline uint16_t htons(uint16_t val) {
    return (val >> 8) | (val << 8);
}

static inline uint16_t ntohs(uint16_t val) {
    return htons(val);
}

static inline uint32_t htonl(uint32_t val) {
    return ((val >> 24) & 0x000000FF)
         | ((val >>  8) & 0x0000FF00)
         | ((val <<  8) & 0x00FF0000)
         | ((val << 24) & 0xFF000000);
}

static inline uint32_t ntohl(uint32_t val) {
    return htonl(val);
}
