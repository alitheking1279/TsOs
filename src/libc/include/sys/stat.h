/**
 * @file sys/stat.h
 * @brief File status — freestanding libc.
 *
 * POSIX.1-2008 §4.5. File attributes and type tests.
 * struct stat layout matches ext2_stat_t for zero-copy kernel delivery.
 */

#ifndef _LIBC_SYS_STAT_H
#define _LIBC_SYS_STAT_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* --- File type bits (masked with S_IFMT) --- */
#define S_IFMT   0xF000
#define S_IFIFO  0x1000
#define S_IFCHR  0x2000
#define S_IFDIR  0x4000
#define S_IFBLK  0x6000
#define S_IFREG  0x8000
#define S_IFLNK  0xA000
#define S_IFSOCK 0xC000

/* --- Permission bits --- */
#define S_ISUID  0x0800
#define S_ISGID  0x0400
#define S_ISVTX  0x0200
#define S_IRUSR  0x0100
#define S_IWUSR  0x0080
#define S_IXUSR  0x0040
#define S_IRGRP  0x0020
#define S_IWGRP  0x0010
#define S_IXGRP  0x0008
#define S_IROTH  0x0004
#define S_IWOTH  0x0002
#define S_IXOTH  0x0001

/* --- File type test macros (POSIX.1-2008 §4.5.1) --- */
#define S_ISREG(m)  (((m) & S_IFMT) == S_IFREG)
#define S_ISDIR(m)  (((m) & S_IFMT) == S_IFDIR)
#define S_ISCHR(m)  (((m) & S_IFMT) == S_IFCHR)
#define S_ISBLK(m)  (((m) & S_IFMT) == S_IFBLK)
#define S_ISFIFO(m) (((m) & S_IFMT) == S_IFIFO)
#define S_ISSOCK(m) (((m) & S_IFMT) == S_IFSOCK)

/* --- struct stat — matches ext2_stat_t layout for zero-copy --- */
struct stat {
    uint32_t st_dev;      /* Device ID (unused, padding for POSIX layout) */
    uint32_t st_ino;      /* Inode number */
    uint32_t st_mode;     /* File type + permissions */
    uint32_t st_nlink;    /* Hard link count */
    uint32_t st_uid;      /* Owner user ID */
    uint32_t st_gid;      /* Owner group ID */
    uint32_t st_rdev;     /* Device type (unused) */
    uint32_t st_size;     /* File size in bytes */
    uint32_t st_blksize;  /* Preferred block size (unused) */
    uint32_t st_blocks;   /* Number of 512-byte blocks */
    uint32_t st_atime;    /* Last access time */
    uint32_t st_mtime;    /* Last modification time */
    uint32_t st_ctime;    /* Inode change time */
};

/* --- File system calls --- */
int fstat(int fd, struct stat *buf);
int stat(const char *restrict path, struct stat *buf);
int mkdir(const char *path, uint32_t mode);

#ifdef __cplusplus
}
#endif

#endif /* _LIBC_SYS_STAT_H */
