#pragma once
#include "common.h"

// Simple IDE disk driver
#define IDE_PRIMARY_IO 0x1F0
#define IDE_PRIMARY_CONTROL 0x3F6

// IDE registers
#define IDE_REG_DATA       0x00
#define IDE_REG_ERROR      0x01
#define IDE_REG_SECTOR_CNT 0x02
#define IDE_REG_LBA_LOW    0x03
#define IDE_REG_LBA_MID    0x04
#define IDE_REG_LBA_HIGH   0x05
#define IDE_REG_DRIVE      0x06
#define IDE_REG_STATUS     0x07
#define IDE_REG_COMMAND    0x07

// IDE commands
#define IDE_CMD_READ_SECTORS  0x20
#define IDE_CMD_WRITE_SECTORS 0x30

// Status bits
#define IDE_STATUS_BSY  0x80
#define IDE_STATUS_DRDY 0x40
#define IDE_STATUS_DRQ  0x08
#define IDE_STATUS_ERR  0x01

void ide_init(void);
void ide_read_sector(uint32_t lba, void *buf);
void ide_write_sector(uint32_t lba, const void *buf);

