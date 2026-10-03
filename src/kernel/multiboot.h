/*
 * VibeCagOS — Multiboot v1 Header & Information Structures
 */

#pragma once
#include "common.h"

#define MULTIBOOT_BOOTLOADER_MAGIC  0x2BADB002u

#define MULTIBOOT_INFO_MEMORY       (1u << 0)
#define MULTIBOOT_INFO_BOOTDEV      (1u << 1)
#define MULTIBOOT_INFO_CMDLINE      (1u << 2)
#define MULTIBOOT_INFO_MODS         (1u << 3)
#define MULTIBOOT_INFO_MEM_MAP      (1u << 6)
#define MULTIBOOT_INFO_BOOT_LOADER  (1u << 9)

struct multiboot_mmap_entry {
    uint32_t size;
    uint32_t addr_low;
    uint32_t addr_high;
    uint32_t len_low;
    uint32_t len_high;
    uint32_t type;  /* 1 = usable RAM */
} __attribute__((packed));

struct multiboot_info {
    uint32_t flags;
    uint32_t mem_lower;    /* KB */
    uint32_t mem_upper;    /* KB */
    uint32_t boot_device;
    uint32_t cmdline;      /* pointer to ASCII string */
    uint32_t mods_count;
    uint32_t mods_addr;
    uint32_t syms[4];
    uint32_t mmap_length;
    uint32_t mmap_addr;
    uint32_t drives_length;
    uint32_t drives_addr;
    uint32_t config_table;
    uint32_t boot_loader_name; /* pointer to ASCII string */
    uint32_t apm_table;
    uint32_t vbe_control_info;
    uint32_t vbe_mode_info;
    uint16_t vbe_mode;
    uint16_t vbe_interface_seg;
    uint16_t vbe_interface_off;
    uint16_t vbe_interface_len;
} __attribute__((packed));

extern struct multiboot_info *g_mb_info;
extern uint32_t               g_mb_magic;

void multiboot_dump_info(void);
