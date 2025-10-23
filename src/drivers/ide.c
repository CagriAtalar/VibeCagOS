#include "ide.h"
#include "common.h"

// Forward declarations
void printf(const char *fmt, ...);
static inline uint8_t inb(uint16_t port);
static inline void outb(uint16_t port, uint8_t value);
static inline void io_wait(void);

static inline uint8_t inb(uint16_t port) {
    uint8_t value;
    __asm__ __volatile__("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static inline void outb(uint16_t port, uint8_t value) {
    __asm__ __volatile__("outb %0, %1" : : "a"(value), "Nd"(port));
}

static inline uint16_t inw(uint16_t port) {
    uint16_t value;
    __asm__ __volatile__("inw %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static inline void outw(uint16_t port, uint16_t value) {
    __asm__ __volatile__("outw %0, %1" : : "a"(value), "Nd"(port));
}

static inline void io_wait(void) {
    outb(0x80, 0);
}

static void ide_wait_bsy(void) {
    while (inb(IDE_PRIMARY_IO + IDE_REG_STATUS) & IDE_STATUS_BSY)
        ;
}

static void ide_wait_drq(void) {
    while (!(inb(IDE_PRIMARY_IO + IDE_REG_STATUS) & IDE_STATUS_DRQ))
        ;
}

void ide_init(void) {
    printf("IDE disk driver initialized\n");
    
    // Select drive 0
    outb(IDE_PRIMARY_IO + IDE_REG_DRIVE, 0xE0);
    io_wait();
    
    // Wait for drive to be ready
    ide_wait_bsy();
    
    // Check if drive exists
    uint8_t status = inb(IDE_PRIMARY_IO + IDE_REG_STATUS);
    if (status == 0 || status == 0xFF) {
        printf("Warning: No IDE drive detected\n");
        return;
    }
    
    printf("IDE drive detected and ready\n");
}

void ide_read_sector(uint32_t lba, void *buf) {
    uint16_t *ptr = (uint16_t *)buf;
    
    ide_wait_bsy();
    
    // Select drive 0 and set LBA mode
    outb(IDE_PRIMARY_IO + IDE_REG_DRIVE, 0xE0 | ((lba >> 24) & 0x0F));
    io_wait();
    
    // Set sector count
    outb(IDE_PRIMARY_IO + IDE_REG_SECTOR_CNT, 1);
    
    // Set LBA
    outb(IDE_PRIMARY_IO + IDE_REG_LBA_LOW, lba & 0xFF);
    outb(IDE_PRIMARY_IO + IDE_REG_LBA_MID, (lba >> 8) & 0xFF);
    outb(IDE_PRIMARY_IO + IDE_REG_LBA_HIGH, (lba >> 16) & 0xFF);
    
    // Send read command
    outb(IDE_PRIMARY_IO + IDE_REG_COMMAND, IDE_CMD_READ_SECTORS);
    
    // Wait for data
    ide_wait_drq();
    
    // Read 256 words (512 bytes)
    for (int i = 0; i < 256; i++) {
        ptr[i] = inw(IDE_PRIMARY_IO + IDE_REG_DATA);
    }
}

void ide_write_sector(uint32_t lba, const void *buf) {
    const uint16_t *ptr = (const uint16_t *)buf;
    
    ide_wait_bsy();
    
    // Select drive 0 and set LBA mode
    outb(IDE_PRIMARY_IO + IDE_REG_DRIVE, 0xE0 | ((lba >> 24) & 0x0F));
    io_wait();
    
    // Set sector count
    outb(IDE_PRIMARY_IO + IDE_REG_SECTOR_CNT, 1);
    
    // Set LBA
    outb(IDE_PRIMARY_IO + IDE_REG_LBA_LOW, lba & 0xFF);
    outb(IDE_PRIMARY_IO + IDE_REG_LBA_MID, (lba >> 8) & 0xFF);
    outb(IDE_PRIMARY_IO + IDE_REG_LBA_HIGH, (lba >> 16) & 0xFF);
    
    // Send write command
    outb(IDE_PRIMARY_IO + IDE_REG_COMMAND, IDE_CMD_WRITE_SECTORS);
    
    // Wait for ready
    ide_wait_drq();
    
    // Write 256 words (512 bytes)
    for (int i = 0; i < 256; i++) {
        outw(IDE_PRIMARY_IO + IDE_REG_DATA, ptr[i]);
    }
    
    // Flush cache
    ide_wait_bsy();
}

