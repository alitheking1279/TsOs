/**
 * @file vfs.h
 * @brief Virtual Filesystem Switch — filesystem-agnostic path dispatch.
 *
 * The VFS provides a mount table and path-based dispatch layer that sits
 * between syscall handlers and filesystem implementations (ext2, future fs).
 *
 * Design:
 *   - Each registered filesystem provides a vfs_fs_ops_t vtable with
 *     path-based operations (open, stat, unlink, rmdir, rename, getdents).
 *   - vfs_mount() associates a mount-point path prefix with a filesystem.
 *   - vfs_resolve_path() performs longest-prefix matching to find the mount,
 *     then strips the prefix to produce a filesystem-relative path.
 *   - sys_open() goes through VFS; sys_read/write/close use the per-fd
 *     file_ops_t that was installed during open (already fs-agnostic).
 */

#ifndef FS_VFS_H
#define FS_VFS_H

#include <stdint.h>
#include <stdbool.h>
#include "../kernel/task.h"
#include "ext2.h"

#ifdef __cplusplus
extern "C" {
#endif

/* =========================================================================
 * Constants
 * ========================================================================= */

/** Maximum number of mount points. */
#define VFS_MAX_MOUNTS    4

/** Maximum mount-point path length. */
#define VFS_MAX_MP_LEN    64

/* =========================================================================
 * Filesystem Operations Vtable
 * ========================================================================= */

/**
 * @brief Per-filesystem operations vtable.
 *
 * All path arguments are filesystem-relative (mount prefix already stripped).
 * The open() callback must populate *ops and *data for the per-fd layer.
 */
typedef struct vfs_fs_ops {
    const char *name;   /**< Filesystem name (e.g., "ext2"). */

    /**
     * @brief Open a file relative to the mount root.
     *
     * @param rel_path  Filesystem-relative path.
     * @param flags     Open flags (O_RDONLY, O_CREAT, etc.).
     * @param ops       Output: per-fd file_ops_t to install.
     * @param data      Output: per-fd private data (caller frees via close).
     * @return 0 on success, -1 on error.
     */
    int (*open)(const char *rel_path, uint32_t flags,
                file_ops_t **ops, void **data);

    /**
     * @brief Get file status by path.
     * @param rel_path  Filesystem-relative path.
     * @param st        Output stat buffer.
     * @return 0 on success.
     */
    int (*stat)(const char *rel_path, ext2_stat_t *st);

    /**
     * @brief Remove a file by path.
     * @param rel_path  Filesystem-relative path.
     * @return 0 on success.
     */
    int (*unlink)(const char *rel_path);

    /**
     * @brief Remove an empty directory by path.
     * @param rel_path  Filesystem-relative path.
     * @return 0 on success.
     */
    int (*rmdir)(const char *rel_path);

    /**
     * @brief Rename/move a file.
     * @param old_path  Old filesystem-relative path.
     * @param new_path  New filesystem-relative path.
     * @return 0 on success.
     */
    int (*rename)(const char *old_path, const char *new_path);

    /**
     * @brief Create a directory.
     * @param rel_path  Filesystem-relative path.
     * @param mode      Permission bits.
     * @return 0 on success.
     */
    int (*mkdir)(const char *rel_path, uint32_t mode);

    /**
     * @brief Read directory entries.
     * @param rel_path  Filesystem-relative path (directory).
     * @param cookie    Byte offset (in/out).
     * @param buf       Output buffer.
     * @param count     Buffer size.
     * @return Bytes written, or 0 if no more entries.
     */
    int (*getdents)(const char *rel_path, uint64_t *cookie,
                    void *buf, uint32_t count);
} vfs_fs_ops_t;

/* =========================================================================
 * Mount Table Entry
 * ========================================================================= */

typedef struct vfs_mount {
    char            prefix[VFS_MAX_MP_LEN]; /**< Mount-point prefix (e.g., "/"). */
    vfs_fs_ops_t   *ops;                    /**< Filesystem operations vtable. */
    void           *data;                   /**< FS-specific private data. */
    bool            mounted;                /**< Slot in use. */
} vfs_mount_entry_t;

/* =========================================================================
 * Public API
 * ========================================================================= */

/**
 * @brief Initialize the VFS subsystem.
 *
 * Clears the mount table. Must be called before vfs_mount().
 */
void vfs_init(void);

/**
 * @brief Register a filesystem type with the VFS.
 *
 * Currently only one ext2 instance is supported. This registers the
 * ext2 vtable so vfs_mount() can reference it.
 *
 * @param ops  Filesystem operations vtable.
 */
void vfs_register_fs(vfs_fs_ops_t *ops);

/**
 * @brief Mount a filesystem at a given path prefix.
 *
 * The longest matching prefix wins during path resolution.
 * Mounting at "/" is the root filesystem.
 *
 * @param mount_point  Path prefix (e.g., "/").
 * @param ops          Filesystem operations (must be registered).
 * @param data         FS-specific private data.
 * @return 0 on success, -1 on error.
 */
int vfs_mount(const char *mount_point, vfs_fs_ops_t *ops, void *data);

/**
 * @brief Unmount a filesystem by mount-point prefix.
 *
 * @param mount_point  Path prefix to unmount.
 * @return 0 on success, -1 if not found.
 */
int vfs_unmount(const char *mount_point);

/**
 * @brief Resolve a path to its mount entry and compute the relative path.
 *
 * Performs longest-prefix matching against the mount table.
 *
 * @param path       Absolute path to resolve.
 * @param rel_path   Output: pointer into original path after the mount prefix.
 * @param mount_out  Output: the matching mount entry.
 * @return 0 on success (mount found), -1 if no mount matches.
 */
int vfs_resolve_path(const char *path, const char **rel_path,
                     vfs_mount_entry_t **mount_out);

/**
 * @brief Open a file through the VFS.
 *
 * Resolves the path, delegates to the filesystem's open(), and returns
 * a file descriptor on success.
 *
 * @param path    Absolute path.
 * @param flags   Open flags.
 * @return File descriptor (>= 0), or -1 on error.
 */
int vfs_open(const char *path, uint32_t flags);

/**
 * @brief Get file status through the VFS.
 *
 * @param path  Absolute path.
 * @param st    Output stat buffer.
 * @return 0 on success.
 */
int vfs_stat(const char *path, ext2_stat_t *st);

/**
 * @brief Unlink (remove) a file through the VFS.
 *
 * @param path  Absolute path.
 * @return 0 on success.
 */
int vfs_unlink(const char *path);

/**
 * @brief Remove an empty directory through the VFS.
 *
 * @param path  Absolute path.
 * @return 0 on success.
 */
int vfs_rmdir(const char *path);

/**
 * @brief Rename a file through the VFS.
 *
 * @param old_path  Old absolute path.
 * @param new_path  New absolute path.
 * @return 0 on success.
 */
int vfs_rename(const char *old_path, const char *new_path);

/**
 * @brief Create a directory through the VFS.
 *
 * @param path  Absolute path.
 * @param mode  Permission bits.
 * @return 0 on success.
 */
int vfs_mkdir(const char *path, uint32_t mode);

/**
 * @brief Read directory entries through the VFS.
 *
 * @param path    Absolute path (directory).
 * @param cookie  Byte offset (in/out).
 * @param buf     Output buffer.
 * @param count   Buffer size.
 * @return Bytes written, or 0 if no more entries.
 */
int vfs_getdents(const char *path, uint64_t *cookie,
                 void *buf, uint32_t count);

/**
 * @brief Get the global ext2 filesystem operations vtable.
 *
 * Returns the vtable registered for ext2. Used by syscall.c during init.
 *
 * @return Pointer to ext2 VFS operations.
 */
vfs_fs_ops_t *vfs_get_ext2_ops(void);

#ifdef __cplusplus
}
#endif

#endif /* FS_VFS_H */
