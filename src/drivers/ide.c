/*
 * VibeCagOS — IDE/ATA Disk Driver (PIO Mode)
 *
 * Implements 28-bit LBA PIO read/write for the primary IDE channel.
 * Uses the inb/outb/inw/outw helpers from kernel.h.
 */

#include "ide.h"
#include "kernel.h"
#include "common.h"

/* Port I/O helpers are in kernel.h (static inline) */

static void ide_wait_bsy(void) {
    while (inb(IDE_PRIMARY_IO + IDE_REG_STATUS) & IDE_STATUS_BSY)
        ;
}

static void ide_wait_drq(void) {
    while (!(inb(IDE_PRIMARY_IO + IDE_REG_STATUS) & IDE_STATUS_DRQ))
        ;
}

static bool ide_ready = false;

void ide_init(void) {
    printf("IDE: Initializing primary channel...\n");

    /* Select drive 0 */
    outb(IDE_PRIMARY_IO + IDE_REG_DRIVE, 0xE0);
    io_wait();
    ide_wait_bsy();

    uint8_t status = inb(IDE_PRIMARY_IO + IDE_REG_STATUS);
    if (status == 0 || status == 0xFF) {
        printf("IDE: No drive detected (status=0x%x)\n",
               (unsigned)status);
        ide_ready = false;
        return;
    }

    ide_ready = true;
    printf("IDE: Drive ready (status=0x%x)\n", (unsigned)status);
}

void ide_read_sector(uint32_t lba, void *buf) {
    uint16_t *ptr = (uint16_t *)buf;

    ide_wait_bsy();

    outb(IDE_PRIMARY_IO + IDE_REG_DRIVE, 0xE0 | ((lba >> 24) & 0x0F));
    io_wait();
    outb(IDE_PRIMARY_IO + IDE_REG_SECTOR_CNT, 1);
    outb(IDE_PRIMARY_IO + IDE_REG_LBA_LOW,  (uint8_t)(lba & 0xFF));
    outb(IDE_PRIMARY_IO + IDE_REG_LBA_MID,  (uint8_t)((lba >> 8) & 0xFF));
    outb(IDE_PRIMARY_IO + IDE_REG_LBA_HIGH, (uint8_t)((lba >> 16) & 0xFF));
    outb(IDE_PRIMARY_IO + IDE_REG_COMMAND, IDE_CMD_READ_SECTORS);

    ide_wait_drq();

    /* Read 256 words = 512 bytes */
    for (int i = 0; i < 256; i++)
        ptr[i] = inw(IDE_PRIMARY_IO + IDE_REG_DATA);
}

void ide_write_sector(uint32_t lba, const void *buf) {
    const uint16_t *ptr = (const uint16_t *)buf;

    ide_wait_bsy();

    outb(IDE_PRIMARY_IO + IDE_REG_DRIVE, 0xE0 | ((lba >> 24) & 0x0F));
    io_wait();
    outb(IDE_PRIMARY_IO + IDE_REG_SECTOR_CNT, 1);
    outb(IDE_PRIMARY_IO + IDE_REG_LBA_LOW,  (uint8_t)(lba & 0xFF));
    outb(IDE_PRIMARY_IO + IDE_REG_LBA_MID,  (uint8_t)((lba >> 8) & 0xFF));
    outb(IDE_PRIMARY_IO + IDE_REG_LBA_HIGH, (uint8_t)((lba >> 16) & 0xFF));
    outb(IDE_PRIMARY_IO + IDE_REG_COMMAND, IDE_CMD_WRITE_SECTORS);

    ide_wait_drq();

    /* Write 256 words = 512 bytes */
    for (int i = 0; i < 256; i++)
        outw(IDE_PRIMARY_IO + IDE_REG_DATA, ptr[i]);

    ide_wait_bsy();  /* Wait for write to complete */
}
