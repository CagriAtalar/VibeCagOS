#pragma once
#include "vfs.h"

/*
 * VibeCagOS — procfs (pseudo-filesystem)
 *
 * Provides a virtual /proc directory exposing kernel information.
 * Files are generated on-demand by callback functions; no disk I/O.
 *
 * Entries:
 *   /proc/version    — kernel version string
 *   /proc/uptime     — system uptime in seconds
 *   /proc/meminfo    — physical memory statistics
 *   /proc/cpuinfo    — CPU information (vendor, frequency)
 *   /proc/tasks      — process list
 *   /proc/mounts     — mount table
 *
 * Architecture:
 *   procfs_mount() creates a single root vnode.
 *   lookup() matches filenames against a static table of entries.
 *   read() calls the entry's generate() callback to produce content.
 *   All content is generated fresh on each read.
 */

void procfs_init(void);

/* Returns the root vnode of the procfs (for use with vfs_mount). */
struct vnode *procfs_get_root(void);

/* procfs VFS operations vtable */
extern const struct fs_ops procfs_vfs_ops;
