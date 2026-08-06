/**
 * @file vfs.c
 * @brief Virtual Filesystem Switch — mount table, path dispatch, and
 *        filesystem-agnostic wrappers for ext2.
 *
 * Provides longest-prefix mount-point matching and delegates path-based
 * operations to the appropriate filesystem driver.
 */

#include "vfs.h"
#include "../kernel/kheap.h"
#include "../drivers/serial.h"
#include "../lib/print.h"
#include <string.h>

extern serial_dev_t g_serial;

/* =========================================================================
 * Global State
 * ========================================================================= */

static vfs_mount_entry_t g_mounts[VFS_MAX_MOUNTS];
static vfs_fs_ops_t *g_ext2_ops = NULL;

/* =========================================================================
 * Helpers
 * ========================================================================= */

static int path_cmp_prefix(const char *path, const char *prefix) {
    while (*prefix) {
        if (*path != *prefix) return 0;
        path++;
        prefix++;
    }
    return 1;
}

/* =========================================================================
 * Core API
 * ========================================================================= */

void vfs_init(void) {
    memset(g_mounts, 0, sizeof(g_mounts));
    g_ext2_ops = NULL;
}

void vfs_register_fs(vfs_fs_ops_t *ops) {
    if (!ops) return;
    if (ops->name && strcmp(ops->name, "ext2") == 0) {
        g_ext2_ops = ops;
    }
}

int vfs_mount(const char *mount_point, vfs_fs_ops_t *ops, void *data) {
    if (!mount_point || !ops) return -1;

    /* Check for duplicate mount point. */
    for (int i = 0; i < VFS_MAX_MOUNTS; i++) {
        if (g_mounts[i].mounted &&
            strcmp(g_mounts[i].prefix, mount_point) == 0) {
            return -1; /* Already mounted here. */
        }
    }

    /* Find a free slot. */
    for (int i = 0; i < VFS_MAX_MOUNTS; i++) {
        if (!g_mounts[i].mounted) {
            int len = 0;
            while (mount_point[len] && len < VFS_MAX_MP_LEN - 1) {
                g_mounts[i].prefix[len] = mount_point[len];
                len++;
            }
            g_mounts[i].prefix[len] = '\0';
            g_mounts[i].ops    = ops;
            g_mounts[i].data   = data;
            g_mounts[i].mounted = true;
            return 0;
        }
    }

    return -1; /* No free mount slots. */
}

int vfs_unmount(const char *mount_point) {
    if (!mount_point) return -1;

    for (int i = 0; i < VFS_MAX_MOUNTS; i++) {
        if (g_mounts[i].mounted &&
            strcmp(g_mounts[i].prefix, mount_point) == 0) {
            g_mounts[i].mounted = false;
            g_mounts[i].ops    = NULL;
            g_mounts[i].data   = NULL;
            return 0;
        }
    }

    return -1; /* Not found. */
}

int vfs_resolve_path(const char *path, const char **rel_path,
                     vfs_mount_entry_t **mount_out) {
    if (!path || !rel_path || !mount_out) return -1;

    int best_idx = -1;
    int best_len = 0;

    /* Longest-prefix match. */
    for (int i = 0; i < VFS_MAX_MOUNTS; i++) {
        if (!g_mounts[i].mounted) continue;

        int plen = 0;
        while (g_mounts[i].prefix[plen]) plen++;

        /* Match: path must start with prefix. */
        if (!path_cmp_prefix(path, g_mounts[i].prefix)) continue;

        /* After the prefix, path must end, start with '/', or prefix ends with '/'. */
        if (path[plen] != '\0' && path[plen] != '/' &&
            g_mounts[i].prefix[plen - 1] != '/') continue;

        if (plen > best_len) {
            best_len = plen;
            best_idx = i;
        }
    }

    if (best_idx < 0) return -1;

    *mount_out = &g_mounts[best_idx];

    /* Compute relative path: skip the mount prefix. */
    const char *rp = path + best_len;
    /* Skip a single leading '/' if the prefix doesn't end with one. */
    if (*rp == '/' && best_len > 0 && g_mounts[best_idx].prefix[best_len - 1] != '/') {
        rp++;
    }
    /* Root path: if rp is empty, use "/". */
    if (*rp == '\0') rp = "/";

    *rel_path = rp;
    return 0;
}

/* =========================================================================
 * Path-Based Operations (VFS wrappers)
 * ========================================================================= */

int vfs_open(const char *path, uint32_t flags) {
    const char *rel = NULL;
    vfs_mount_entry_t *mnt = NULL;
    if (vfs_resolve_path(path, &rel, &mnt) < 0) return -1;
    if (!mnt->ops || !mnt->ops->open) return -1;

    file_ops_t *ops = NULL;
    void *data = NULL;
    int result = mnt->ops->open(rel, flags, &ops, &data);
    if (result < 0) return -1;

    /* Install into current task's fd table. */
    task_t *cur = task_get_current();
    if (!cur) return -1;

    int fd = -1;
    for (int f = 0; f < TASK_MAX_FDS; f++) {
        if (cur->fd_table[f].ops == NULL) { fd = f; break; }
    }
    if (fd < 0) return -1;

    cur->fd_table[fd].ops      = ops;
    cur->fd_table[fd].data      = data;
    cur->fd_table[fd].refcount  = 1;
    cur->fd_table[fd].flags     = flags;
    return fd;
}

int vfs_stat(const char *path, ext2_stat_t *st) {
    const char *rel = NULL;
    vfs_mount_entry_t *mnt = NULL;
    if (vfs_resolve_path(path, &rel, &mnt) < 0) return -1;
    if (!mnt->ops || !mnt->ops->stat) return -1;
    return mnt->ops->stat(rel, st);
}

int vfs_unlink(const char *path) {
    const char *rel = NULL;
    vfs_mount_entry_t *mnt = NULL;
    if (vfs_resolve_path(path, &rel, &mnt) < 0) return -1;
    if (!mnt->ops || !mnt->ops->unlink) return -1;
    return mnt->ops->unlink(rel);
}

int vfs_rmdir(const char *path) {
    const char *rel = NULL;
    vfs_mount_entry_t *mnt = NULL;
    if (vfs_resolve_path(path, &rel, &mnt) < 0) return -1;
    if (!mnt->ops || !mnt->ops->rmdir) return -1;
    return mnt->ops->rmdir(rel);
}

int vfs_rename(const char *old_path, const char *new_path) {
    const char *old_rel = NULL, *new_rel = NULL;
    vfs_mount_entry_t *old_mnt = NULL, *new_mnt = NULL;

    if (vfs_resolve_path(old_path, &old_rel, &old_mnt) < 0) return -1;
    if (vfs_resolve_path(new_path, &new_rel, &new_mnt) < 0) return -1;

    /* Both paths must be on the same mount. */
    if (old_mnt != new_mnt) return -1;

    if (!old_mnt->ops || !old_mnt->ops->rename) return -1;
    return old_mnt->ops->rename(old_rel, new_rel);
}

int vfs_mkdir(const char *path, uint32_t mode) {
    serial_write_string(&g_serial, "[VFS] mkdir path="); serial_write_string(&g_serial, path); serial_write_string(&g_serial, "\r\n");
    const char *rel = NULL;
    vfs_mount_entry_t *mnt = NULL;
    if (vfs_resolve_path(path, &rel, &mnt) < 0) {
        serial_write_string(&g_serial, "[VFS] mkdir: resolve_path FAILED\r\n");
        return -1;
    }
    serial_write_string(&g_serial, "[VFS] mkdir rel="); serial_write_string(&g_serial, rel); serial_write_string(&g_serial, "\r\n");
    if (!mnt->ops || !mnt->ops->mkdir) {
        serial_write_string(&g_serial, "[VFS] mkdir: no ops->mkdir\r\n");
        return -1;
    }
    int ret = mnt->ops->mkdir(rel, mode);
    serial_write_string(&g_serial, "[VFS] mkdir ret="); print_hex32(&g_serial, (uint32_t)ret); serial_write_string(&g_serial, "\r\n");
    return ret;
}

int vfs_getdents(const char *path, uint64_t *cookie,
                 void *buf, uint32_t count) {
    serial_write_string(&g_serial, "[VFS] getdents path="); serial_write_string(&g_serial, path); serial_write_string(&g_serial, " cookie=");
    print_hex64(&g_serial, *cookie); serial_write_string(&g_serial, "\r\n");
    const char *rel = NULL;
    vfs_mount_entry_t *mnt = NULL;
    if (vfs_resolve_path(path, &rel, &mnt) < 0) {
        serial_write_string(&g_serial, "[VFS] getdents: resolve_path FAILED\r\n");
        return -1;
    }
    serial_write_string(&g_serial, "[VFS] getdents rel="); serial_write_string(&g_serial, rel); serial_write_string(&g_serial, "\r\n");
    if (!mnt->ops || !mnt->ops->getdents) {
        serial_write_string(&g_serial, "[VFS] getdents: no ops->getdents\r\n");
        return -1;
    }
    int ret = mnt->ops->getdents(rel, cookie, buf, count);
    serial_write_string(&g_serial, "[VFS] getdents ret="); print_hex32(&g_serial, (uint32_t)ret); serial_write_string(&g_serial, "\r\n");
    return ret;
}

vfs_fs_ops_t *vfs_get_ext2_ops(void) {
    return g_ext2_ops;
}
