/*
 * VibeCagOS — procfs Implementation
 *
 * A pseudo-filesystem whose files are generated dynamically by callbacks.
 *
 * Data flow for reading /proc/uptime:
 *
 *   vfs_read(file at /proc/uptime)
 *       |
 *       v
 *   procfs_vfs_read(vnode)
 *       |
 *       v
 *   proc_entry[uptime].generate(buf)  -> writes "uptime: 42 sec\n"
 *       |
 *       v
 *   copy subset [off, off+len) to caller buffer
 *
 * Generation buffers:
 *   Each read regenerates the content into a static 2048-byte buffer.
 *   Offset slicing then serves the caller's requested range.
 *   This is acceptable for small proc files.
 */

#include "procfs.h"
#include "vfs.h"
#include "common.h"
#include "kernel/kmalloc.h"
#include "kernel/kernel.h"
#include "kernel/klog.h"
#include "kernel/pmm.h"
#include "drivers/pci.h"
#include "drivers/rtc.h"

/* =========================================================================
 * Forward declarations from kernel subsystems
 * ========================================================================= */

extern uint32_t get_uptime_ms(void);
extern uint32_t get_ticks(void);
extern struct process procs[];

/* Memory stats */
extern paddr_t next_paddr;
extern char __free_ram[], __free_ram_end[];

#define VIBECAGOS_VERSION "0.6.0"

/* =========================================================================
 * Content generation buffer
 * ========================================================================= */

#define PROC_BUF_SIZE 2048

/*
 * Simple printf-to-buffer helper.
 * Writes formatted text into buf[*pos], advances *pos.
 */
static char proc_tmp[64];

static void proc_append(char *buf, int *pos, const char *s) {
    while (*s && *pos < PROC_BUF_SIZE - 1) {
        buf[(*pos)++] = *s++;
    }
    buf[*pos] = '\0';
}

static void proc_append_u32(char *buf, int *pos, uint32_t v) {
    /* Simple integer-to-string */
    int len = 0;
    if (v == 0) {
        proc_tmp[len++] = '0';
    } else {
        while (v > 0) {
            proc_tmp[len++] = (char)('0' + v % 10);
            v /= 10;
        }
        /* Reverse */
        for (int a = 0, b = len - 1; a < b; a++, b--) {
            char t = proc_tmp[a]; proc_tmp[a] = proc_tmp[b]; proc_tmp[b] = t;
        }
    }
    proc_tmp[len] = '\0';
    proc_append(buf, pos, proc_tmp);
}

/* =========================================================================
 * Per-entry generation callbacks
 * ========================================================================= */

static int gen_version(char *buf) {
    int pos = 0;
    proc_append(buf, &pos, "VibeCagOS ");
    proc_append(buf, &pos, VIBECAGOS_VERSION);
    proc_append(buf, &pos, " i386 protected-mode kernel\n");
    proc_append(buf, &pos, "Build: " __DATE__ " " __TIME__ "\n");
    return pos;
}

static int gen_uptime(char *buf) {
    int pos = 0;
    uint32_t ms  = get_uptime_ms();
    uint32_t sec = ms / 1000;
    uint32_t min = sec / 60;
    uint32_t hr  = min / 60;
    min %= 60;
    sec %= 60;

    proc_append(buf, &pos, "uptime: ");
    proc_append_u32(buf, &pos, hr);
    proc_append(buf, &pos, "h ");
    proc_append_u32(buf, &pos, min);
    proc_append(buf, &pos, "m ");
    proc_append_u32(buf, &pos, sec);
    proc_append(buf, &pos, "s (");
    proc_append_u32(buf, &pos, ms);
    proc_append(buf, &pos, " ms)\n");
    proc_append(buf, &pos, "ticks: ");
    proc_append_u32(buf, &pos, get_ticks());
    proc_append(buf, &pos, "\n");
    return pos;
}

static int gen_meminfo(char *buf) {
    int pos = 0;
    uint32_t total = (uint32_t)(pmm_get_total_bytes() / 1024);
    uint32_t used  = (uint32_t)(pmm_get_used_bytes() / 1024);
    uint32_t free_b = (uint32_t)(pmm_get_free_bytes() / 1024);

    proc_append(buf, &pos, "MemTotal:    ");
    proc_append_u32(buf, &pos, total);
    proc_append(buf, &pos, " kB\n");

    proc_append(buf, &pos, "MemUsed:     ");
    proc_append_u32(buf, &pos, used);
    proc_append(buf, &pos, " kB\n");

    proc_append(buf, &pos, "MemFree:     ");
    proc_append_u32(buf, &pos, free_b);
    proc_append(buf, &pos, " kB\n");

    proc_append(buf, &pos, "FramesFree:  ");
    proc_append_u32(buf, &pos, (uint32_t)pmm_get_free_frames());
    proc_append(buf, &pos, "\n");

    struct kmalloc_stats ks;
    kmalloc_get_stats(&ks);
    proc_append(buf, &pos, "HeapAlloc:   ");
    proc_append_u32(buf, &pos, (uint32_t)(ks.allocated_bytes / 1024));
    proc_append(buf, &pos, " kB\n");

    proc_append(buf, &pos, "HeapFree:    ");
    proc_append_u32(buf, &pos, (uint32_t)(ks.free_bytes / 1024));
    proc_append(buf, &pos, " kB\n");
    return pos;
}

static int gen_cpuinfo(char *buf) {
    int pos = 0;
    proc_append(buf, &pos, "processor\t: 0\n");
    proc_append(buf, &pos, "vendor_id\t: VibeCagOS-x86\n");
    proc_append(buf, &pos, "cpu family\t: 6\n");
    proc_append(buf, &pos, "model name\t: QEMU i386 (Virtual)\n");
    proc_append(buf, &pos, "bogomips\t: 100.0\n");
    proc_append(buf, &pos, "flags\t\t: fpu pse pae mce cx8 apic sep mtrr\n");
    return pos;
}

static int gen_tasks(char *buf) {
    int pos = 0;
    /* indexed by PROC_* (kernel.h): UNUSED RUNNABLE RUNNING SLEEPING ZOMBIE BLOCKED CREATED */
    const char *state_names[] = {
        "UNUSED", "RUNNABLE", "RUNNING", "SLEEPING", "ZOMBIE", "BLOCKED", "CREATED"
    };

    proc_append(buf, &pos, "PID   PPID  STATE     NAME\n");
    proc_append(buf, &pos, "----  ----  --------  ----------------\n");

    for (int i = 0; i < PROCS_MAX; i++) {
        if (procs[i].state == PROC_UNUSED) continue;
        proc_append_u32(buf, &pos, (uint32_t)procs[i].pid);
        proc_append(buf, &pos, procs[i].pid < 10 ? "     " : procs[i].pid < 100 ? "    " : "   ");
        if (procs[i].ppid > 0) proc_append_u32(buf, &pos, (uint32_t)procs[i].ppid);
        else proc_append(buf, &pos, "-");
        proc_append(buf, &pos, procs[i].ppid > 9 ? "    " : "     ");
        int st = procs[i].state;
        if (st < 0 || st > 6) st = 0;
        proc_append(buf, &pos, state_names[st]);
        for (size_t k = strlen(state_names[st]); k < 10; k++) proc_append(buf, &pos, " ");
        proc_append(buf, &pos, procs[i].name);
        proc_append(buf, &pos, "\n");
    }
    return pos;
}

static void proc_append_hex(char *buf, int *pos, uint32_t v, int digits) {
    char t[9];
    for (int i = 0; i < digits; i++) t[i] = "0123456789abcdef"[(v >> (4 * (digits - 1 - i))) & 0xF];
    t[digits] = 0;
    proc_append(buf, pos, t);
}

static int gen_devices(char *buf) {
    int pos = 0;
    proc_append(buf, &pos,
        "Detected Devices:\n"
        "  [CHRDEV] /dev/null     - null device (read=EOF, write=discard)\n"
        "  [CHRDEV] /dev/zero     - zero byte source\n"
        "  [CHRDEV] /dev/console  - kernel serial+VGA console\n"
        "  [CHRDEV] /dev/random   - pseudo-random bytes (xorshift32)\n"
        "  [CHRDEV] /dev/tty      - alias for /dev/console\n"
        "  [BLKDEV] IDE disk 0    - ATA PIO mode (2 MB disk.img)\n"
        "  [NETDEV] RTL8139       - 10/100 Ethernet (QEMU virtnet)\n"
        "  [TIMER]  PIT 8254      - 100 Hz preemptive timer\n"
        "  [INPUT]  PS/2 keyboard - scancode set 1 + ring buffer\n");
    return pos;
}

static int gen_pci(char *buf) {
    int pos = 0;
    proc_append(buf, &pos, "PCI Devices:\n");
    if (nic_dev.found) {
        proc_append(buf, &pos, "  ");
        proc_append_hex(buf, &pos, nic_dev.bus, 2);  proc_append(buf, &pos, ":");
        proc_append_hex(buf, &pos, nic_dev.dev, 2);  proc_append(buf, &pos, ".");
        proc_append_hex(buf, &pos, nic_dev.func, 1); proc_append(buf, &pos, "  Vendor: 0x");
        proc_append_hex(buf, &pos, nic_dev.vendor_id, 4); proc_append(buf, &pos, " Device: 0x");
        proc_append_hex(buf, &pos, nic_dev.device_id, 4); proc_append(buf, &pos, "  IO: 0x");
        proc_append_hex(buf, &pos, nic_dev.io_base, 4);   proc_append(buf, &pos, "  IRQ: ");
        proc_append_u32(buf, &pos, nic_dev.irq_line);
        proc_append(buf, &pos, "  (Realtek RTL8139 Fast Ethernet)\n");
    } else {
        proc_append(buf, &pos, "  No PCI devices enumerated\n");
    }
    return pos;
}

static int gen_date(char *buf) {
    int pos = 0;
    char t[32];
    rtc_format_time(t, sizeof(t));
    proc_append(buf, &pos, t);
    proc_append(buf, &pos, " UTC\n");
    return pos;
}

static int gen_mounts(char *buf) {
    int pos = 0;
    proc_append(buf, &pos, "Filesystem  Mount point\n");
    proc_append(buf, &pos, "----------  -----------\n");
    proc_append(buf, &pos, "vibefs      /\n");
    proc_append(buf, &pos, "procfs      /proc\n");
    proc_append(buf, &pos, "devfs       /dev\n");
    return pos;
}

static int gen_dmesg(char *buf) {
    /* Read from the klog ring buffer */
    uint32_t n = klog_read(buf, PROC_BUF_SIZE - 1);
    buf[n] = '\0';
    return (int)n;
}

static int gen_net(char *buf) {
    int pos = 0;
    proc_append(buf, &pos, "Interface  IP               RX-pkts  TX-pkts\n");
    proc_append(buf, &pos, "---------  ---------------  -------  -------\n");
    proc_append(buf, &pos, "eth0       10.0.2.15        (live)   (live)\n");
    return pos;
}

/* =========================================================================
 * Entry table
 * ========================================================================= */

typedef int (*proc_gen_fn)(char *buf);

struct proc_entry {
    const char  *name;
    uint32_t     ino;    /* Fake inode number */
    proc_gen_fn  gen;
};

static const struct proc_entry proc_entries[] = {
    { "version",  100, gen_version  },
    { "uptime",   101, gen_uptime   },
    { "meminfo",  102, gen_meminfo  },
    { "cpuinfo",  103, gen_cpuinfo  },
    { "tasks",    104, gen_tasks    },
    { "mounts",   105, gen_mounts   },
    { "dmesg",    106, gen_dmesg    },
    { "net",      107, gen_net      },
    { "devices",  108, gen_devices  },
    { "pci",      109, gen_pci      },
    { "date",     110, gen_date     },
};

#define PROC_ENTRY_COUNT  (sizeof(proc_entries) / sizeof(proc_entries[0]))

#define PROC_ROOT_INO  99u

/* =========================================================================
 * VFS operations
 * ========================================================================= */

static struct vnode *procfs_root_vnode = NULL;

static struct vnode *procfs_make_file_vnode(uint32_t ino) {
    struct vnode *vn = vfs_vnode_alloc();
    if (!vn) return NULL;
    vn->ino  = ino;
    vn->type = VFS_TYPE_REG;
    vn->mode = VFS_PERM_DEFAULT_FILE & ~(VFS_PERM_OWNER_W | VFS_PERM_GROUP_W | VFS_PERM_OTHER_W);
    vn->size = 0;  /* Generated on read */
    vn->ops  = &procfs_vfs_ops;
    return vn;
}

static struct vnode *procfs_vfs_lookup(struct vnode *dir, const char *name) {
    (void)dir;
    for (size_t i = 0; i < PROC_ENTRY_COUNT; i++) {
        if (strcmp(proc_entries[i].name, name) == 0) {
            return procfs_make_file_vnode(proc_entries[i].ino);
        }
    }
    return NULL;
}

static int procfs_vfs_read(struct vnode *vn, void *buf, size_t len,
                            uint32_t off) {
    /* Find the entry by inode number */
    const struct proc_entry *entry = NULL;
    for (size_t i = 0; i < PROC_ENTRY_COUNT; i++) {
        if (proc_entries[i].ino == vn->ino) {
            entry = &proc_entries[i];
            break;
        }
    }
    if (!entry) return VFS_ENOENT;

    /* Generate content */
    static char content[PROC_BUF_SIZE];
    int total = entry->gen(content);

    if (off >= (uint32_t)total) return 0;

    size_t avail = (size_t)(total - (int)off);
    size_t to_copy = avail < len ? avail : len;
    memcpy(buf, content + off, to_copy);
    return (int)to_copy;
}

static int procfs_vfs_readdir(struct vnode *dir, struct dirent *entries,
                               int max, uint32_t *pos) {
    (void)dir;
    int count = 0;
    uint32_t start = *pos;

    /* Emit "." and ".." first */
    if (start == 0 && count < max) {
        entries[count].ino  = PROC_ROOT_INO;
        entries[count].type = VFS_TYPE_DIR;
        memcpy(entries[count].name, ".", 2);
        count++;
        start++;
    }
    if (start == 1 && count < max) {
        entries[count].ino  = PROC_ROOT_INO;
        entries[count].type = VFS_TYPE_DIR;
        memcpy(entries[count].name, "..", 3);
        count++;
        start++;
    }

    /* Emit actual proc entries */
    for (uint32_t i = start - 2; i < PROC_ENTRY_COUNT && count < max; i++) {
        entries[count].ino  = proc_entries[i].ino;
        entries[count].type = VFS_TYPE_REG;
        strncpy(entries[count].name, proc_entries[i].name, VFS_NAME_MAX);
        entries[count].name[VFS_NAME_MAX] = '\0';
        count++;
        start++;
    }

    *pos = start;
    return count;
}

static int procfs_vfs_stat(struct vnode *vn, struct vstat *st) {
    st->ino   = vn->ino;
    st->type  = vn->type;
    st->mode  = vn->mode;
    st->size  = vn->size;
    st->nlink = 1;
    return VFS_OK;
}

/* procfs is read-only */
static int procfs_vfs_create(struct vnode *d, const char *n, uint16_t m, struct vnode **o) {
    (void)d; (void)n; (void)m; (void)o; return VFS_EACCES;
}
static int procfs_vfs_mkdir(struct vnode *d, const char *n, uint16_t m, struct vnode **o) {
    (void)d; (void)n; (void)m; (void)o; return VFS_EACCES;
}
static int procfs_vfs_write(struct vnode *v, const void *b, size_t l, uint32_t o) {
    (void)v; (void)b; (void)l; (void)o; return VFS_EACCES;
}

const struct fs_ops procfs_vfs_ops = {
    .lookup  = procfs_vfs_lookup,
    .create  = procfs_vfs_create,
    .mkdir   = procfs_vfs_mkdir,
    .unlink  = NULL,
    .rmdir   = NULL,
    .read    = procfs_vfs_read,
    .write   = procfs_vfs_write,
    .readdir = procfs_vfs_readdir,
    .stat    = procfs_vfs_stat,
    .truncate= NULL,
    .rename  = NULL,
    .sync    = NULL,
};

/* =========================================================================
 * Public API
 * ========================================================================= */

void procfs_init(void) {
    struct vnode *root = vfs_vnode_alloc();
    if (!root) { PANIC("procfs_init: out of memory"); }
    root->ino  = PROC_ROOT_INO;
    root->type = VFS_TYPE_DIR;
    root->mode = VFS_PERM_DEFAULT_DIR;
    root->size = (uint32_t)PROC_ENTRY_COUNT;
    root->ops  = &procfs_vfs_ops;
    procfs_root_vnode = root;
}

struct vnode *procfs_get_root(void) {
    return procfs_root_vnode;
}
