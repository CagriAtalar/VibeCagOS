/*
 * VibeCagOS — devfs: /dev Pseudo-Filesystem
 *
 * Provides character devices as VFS nodes under /dev.
 * Devices implemented:
 *
 *   /dev/null    — reads return 0 bytes, writes are discarded
 *   /dev/zero    — reads return infinite zero bytes, writes discarded
 *   /dev/console — reads/writes go to the kernel serial+VGA console
 *   /dev/random  — reads return pseudo-random bytes (LFSR-based)
 *   /dev/tty     — alias for /dev/console (convenience)
 *
 * Architecture:
 *   devfs registers itself with the VFS at /dev.
 *   Each device is a vnode with type VFS_TYPE_CHRDEV.
 *   The devfs_vfs_ops vtable dispatches to device-specific handlers.
 */

#pragma once
#include "vfs.h"

/* Device minor numbers */
#define DEV_NULL    0
#define DEV_ZERO    1
#define DEV_CONSOLE 2
#define DEV_RANDOM  3
#define DEV_TTY     4

/* Number of device nodes */
#define DEVFS_NUM_DEVICES 5

/*
 * devfs_init — prepare devfs internal structures.
 * Must be called before vfs_mount("/dev", ...).
 */
void devfs_init(void);

/*
 * devfs_get_root — return the root vnode for /dev.
 * Caller must call vfs_vnode_put() when done.
 */
struct vnode *devfs_get_root(void);

/* VFS operations table for devfs */
extern const struct fs_ops devfs_vfs_ops;
