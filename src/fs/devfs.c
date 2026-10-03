/*
 * VibeCagOS — devfs Implementation
 *
 * All device vnodes share the same fs_ops vtable (devfs_vfs_ops).
 * Device identity is stored in vnode->priv as a (uintptr_t) minor number.
 *
 * Ownership:
 *   devfs_init() allocates all vnodes statically.
 *   No dynamic allocation is used, so no freeing is required.
 *   vfs_vnode_put() is called by the VFS layer; devfs ignores it (static).
 *
 * Interrupt context:
 *   devfs_read/write are called from process context only (syscall path).
 *   They may not be called from IRQ handlers.
 *
 * Thread safety:
 *   Single-CPU design.  No locking needed.
 */

#include "devfs.h"
#include "vfs.h"
#include "../kernel/common.h"
#include "../kernel/kernel.h"  /* putchar, keyboard_getchar, serial_getchar */

/* =========================================================================
 * Static vnode pool
 * ========================================================================= */

/*
 * We need DEVFS_NUM_DEVICES + 1 vnodes:
 *   [0] = root directory vnode for /dev
 *   [1..N] = individual device vnodes
 */
#define DEVFS_POOL_SIZE (DEVFS_NUM_DEVICES + 1)

static struct vnode devfs_pool[DEVFS_POOL_SIZE];
static bool         devfs_initialized = false;

/* =========================================================================
 * PRNG state (xorshift32) for /dev/random
 * ========================================================================= */

static uint32_t prng_state = 0xDEADBEEFu;

static uint32_t prng_next(void) {
    prng_state ^= prng_state << 13;
    prng_state ^= prng_state >> 17;
    prng_state ^= prng_state << 5;
    return prng_state;
}

/* Mix in a time-based seed when first accessed */
static bool prng_seeded = false;

/* =========================================================================
 * Device metadata table
 * ========================================================================= */

typedef struct {
    const char *name;
    uint8_t     minor;
} devfs_entry_t;

static const devfs_entry_t devfs_entries[DEVFS_NUM_DEVICES] = {
    { "null",    DEV_NULL    },
    { "zero",    DEV_ZERO    },
    { "console", DEV_CONSOLE },
    { "random",  DEV_RANDOM  },
    { "tty",     DEV_TTY     },
};

/* =========================================================================
 * Forward declarations
 * ========================================================================= */

static struct vnode *devfs_lookup  (struct vnode *dir, const char *name);
static int           devfs_read    (struct vnode *vn,  void *buf, size_t len, uint32_t off);
static int           devfs_write   (struct vnode *vn,  const void *buf, size_t len, uint32_t off);
static int           devfs_readdir (struct vnode *dir, struct dirent *entries, int max, uint32_t *pos);
static int           devfs_stat    (struct vnode *vn,  struct vstat *st);

/* =========================================================================
 * VFS operations vtable
 * ========================================================================= */

const struct fs_ops devfs_vfs_ops = {
    .lookup  = devfs_lookup,
    .create  = NULL,   /* devices cannot be created by user */
    .mkdir   = NULL,
    .unlink  = NULL,
    .rmdir   = NULL,
    .read    = devfs_read,
    .write   = devfs_write,
    .readdir = devfs_readdir,
    .stat    = devfs_stat,
    .truncate = NULL,
    .rename  = NULL,
    .sync    = NULL,
};

/* =========================================================================
 * devfs_init
 * ========================================================================= */

void devfs_init(void) {
    if (devfs_initialized) return;

    /* Vnode 0: the /dev directory itself */
    struct vnode *root = &devfs_pool[0];
    root->ino    = 0;
    root->type   = VFS_TYPE_DIR;
    root->mode   = 0755;
    root->size   = 0;
    root->refcnt = 1;
    root->mount  = NULL;
    root->ops    = &devfs_vfs_ops;
    root->priv   = (void *)(uintptr_t)0xFF;  /* special: this is the dir */

    /* Vnodes 1..N: device nodes */
    for (int i = 0; i < DEVFS_NUM_DEVICES; i++) {
        struct vnode *vn = &devfs_pool[i + 1];
        vn->ino    = (uint32_t)(i + 1);
        vn->type   = VFS_TYPE_CHRDEV;
        vn->mode   = 0666;
        vn->size   = 0;
        vn->refcnt = 1;
        vn->mount  = NULL;
        vn->ops    = &devfs_vfs_ops;
        vn->priv   = (void *)(uintptr_t)devfs_entries[i].minor;
    }

    devfs_initialized = true;
}

/* =========================================================================
 * devfs_get_root
 * ========================================================================= */

struct vnode *devfs_get_root(void) {
    if (!devfs_initialized) return NULL;
    vfs_vnode_get(&devfs_pool[0]);
    return &devfs_pool[0];
}

/* =========================================================================
 * devfs_lookup — find a device node by name inside /dev
 * ========================================================================= */

static struct vnode *devfs_lookup(struct vnode *dir, const char *name) {
    /* Only the root dir supports lookup */
    if ((uintptr_t)dir->priv != 0xFF) return NULL;

    for (int i = 0; i < DEVFS_NUM_DEVICES; i++) {
        if (strcmp(devfs_entries[i].name, name) == 0) {
            struct vnode *vn = &devfs_pool[i + 1];
            vfs_vnode_get(vn);
            return vn;
        }
    }
    return NULL;
}

/* =========================================================================
 * devfs_read — read from a device
 * ========================================================================= */

static int devfs_read(struct vnode *vn, void *buf, size_t len, uint32_t off) {
    (void)off;  /* character devices ignore offset */

    if (len == 0) return 0;
    uint8_t minor = (uint8_t)(uintptr_t)vn->priv;
    uint8_t *dst  = (uint8_t *)buf;

    switch (minor) {
        case DEV_NULL:
            /* EOF immediately */
            return 0;

        case DEV_ZERO:
            memset(dst, 0, len);
            return (int)len;

        case DEV_CONSOLE:
        case DEV_TTY: {
            /* Blocking read one character at a time */
            int ch = -1;
            while (ch < 0) {
                ch = keyboard_getchar();
                if (ch < 0) ch = (int)serial_getchar();
                if (ch < 0) {
                    /* yield to avoid spinning — not callable from IRQ */
                    /* yield() is safe here (process context) */
                    yield();
                }
            }
            dst[0] = (uint8_t)ch;
            return 1;
        }

        case DEV_RANDOM: {
            if (!prng_seeded) {
                /* Mix in uptime for seed variety */
                prng_state ^= get_uptime_ms();
                prng_seeded = true;
            }
            for (size_t i = 0; i < len; ) {
                uint32_t r = prng_next();
                for (int b = 0; b < 4 && i < len; b++, i++) {
                    dst[i] = (uint8_t)(r & 0xFF);
                    r >>= 8;
                }
            }
            return (int)len;
        }

        default:
            return VFS_EINVAL;
    }
}

/* =========================================================================
 * devfs_write — write to a device
 * ========================================================================= */

static int devfs_write(struct vnode *vn, const void *buf, size_t len, uint32_t off) {
    (void)off;

    if (len == 0) return 0;
    uint8_t      minor = (uint8_t)(uintptr_t)vn->priv;
    const uint8_t *src = (const uint8_t *)buf;

    switch (minor) {
        case DEV_NULL:
        case DEV_ZERO:
        case DEV_RANDOM:
            /* Discard writes */
            return (int)len;

        case DEV_CONSOLE:
        case DEV_TTY:
            /* Echo to both serial and VGA */
            for (size_t i = 0; i < len; i++)
                putchar((char)src[i]);
            return (int)len;

        default:
            return VFS_EINVAL;
    }
}

/* =========================================================================
 * devfs_readdir — list /dev directory
 * ========================================================================= */

static int devfs_readdir(struct vnode *dir, struct dirent *entries, int max, uint32_t *pos) {
    (void)dir;
    int filled = 0;

    while (*pos < (uint32_t)DEVFS_NUM_DEVICES && filled < max) {
        uint32_t idx = *pos;
        entries[filled].ino  = idx + 1;
        entries[filled].type = VFS_TYPE_CHRDEV;
        strncpy(entries[filled].name, devfs_entries[idx].name, VFS_NAME_MAX);
        entries[filled].name[VFS_NAME_MAX] = '\0';
        filled++;
        (*pos)++;
    }
    return filled;
}

/* =========================================================================
 * devfs_stat — metadata for a device node
 * ========================================================================= */

static int devfs_stat(struct vnode *vn, struct vstat *st) {
    st->ino   = vn->ino;
    st->type  = vn->type;
    st->mode  = vn->mode;
    st->size  = 0;
    st->nlink = 1;
    return 0;
}
