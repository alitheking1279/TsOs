#ifndef _USERSPACE_SYSCALLS_H
#define _USERSPACE_SYSCALLS_H

#include <stdint.h>
#include <stddef.h>

long sys_write(int fd, const void *buf, size_t count);
long sys_read(int fd, void *buf, size_t count);
long sys_exit(int code);
long sys_fork(void);
long sys_getpid(void);
long sys_wait(int *status);
long sys_getppid(void);
long sys_brk(uint64_t addr);
long sys_mmap(uint64_t addr, uint64_t length, uint64_t prot, uint64_t flags);
long sys_munmap(uint64_t addr, uint64_t length);
long sys_execve(const void *elf_data, size_t elf_size);
long sys_open(const char *path, int flags);
long sys_close(int fd);
long sys_fstat(int fd, void *buf);
long sys_lseek(int fd, long offset, int whence);
long sys_unlink(const char *path);
long sys_getdents(int fd, void *buf, unsigned int count);
long sys_rename(const char *old, const char *new_name);
long sys_usleep(long usec);
long sys_gettimeofday(void *tv);
long sys_mkdir(const char *path, unsigned int mode);
long sys_rmdir(const char *path);
long sys_get_key(void);
long sys_vga_write(const char *buf, size_t len);
long sys_vga_clear(void);
long sys_vga_set_color(int fg, int bg);
long sys_beep(uint32_t freq, uint32_t duration_ms);
long sys_reboot(void);
long sys_vga_cursor_left(void);
long sys_vga_cursor_right(void);
long sys_vga_backspace(void);
long sys_vga_insert_char(char c);
long sys_vga_get_cursor(void);
long sys_vga_set_cursor(int x, int y);
long sys_vga_put_char(int x, int y, char c, int color);
long sys_vga_scroll(int lines);
long sys_vga_cursor_enable(int start, int end);
long sys_uptime(void);
long sys_sysinfo(void *info);
long sys_shutdown(void);

typedef struct {
    uint64_t mem_total;
    uint64_t mem_free;
    uint64_t mem_used;
    uint64_t mem_reserved;
    uint64_t task_count;
    uint64_t uptime_ticks;
} sysinfo_t;

#endif
