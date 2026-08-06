/**
 * @file syscalls.c
 * @brief Userspace SYSCALL/SYSRET wrappers — Linux-compatible ABI.
 *
 * Every userspace libc function that needs kernel services goes through here.
 * ABI: RAX=syscall#, RDI/RSI/RDX/R10/R8/R9=args, return in RAX.
 */

#include <stdint.h>
#include <stddef.h>

/* Syscall numbers (must match kernel/syscall.h) */
#define SYS_WRITE       0
#define SYS_READ        1
#define SYS_EXIT        2
#define SYS_FORK        3
#define SYS_GETPID      4
#define SYS_WAIT        5
#define SYS_GETPPID     6
#define SYS_BRK         12
#define SYS_MMAP        9
#define SYS_MUNMAP      11
#define SYS_EXECVE      59
#define SYS_OPEN        60
#define SYS_CLOSE       61
#define SYS_FSTAT       70
#define SYS_LSEEK       71
#define SYS_UNLINK      72
#define SYS_GETDENTS    73
#define SYS_RENAME      74
#define SYS_USLEEP      75
#define SYS_GETTIMEOFDAY 76
#define SYS_MKDIR       77
#define SYS_RMDIR       78
#define SYS_GET_KEY     79
#define SYS_VGA_WRITE       80
#define SYS_VGA_CLEAR       81
#define SYS_VGA_SET_COLOR   82
#define SYS_BEEP            83
#define SYS_REBOOT          84
#define SYS_VGA_CURSOR_LEFT 85
#define SYS_VGA_CURSOR_RIGHT 86
#define SYS_VGA_BACKSPACE   87
#define SYS_VGA_INSERT_CHAR 88
#define SYS_VGA_GET_CURSOR  89
#define SYS_VGA_SET_CURSOR  90
#define SYS_UPTIME          91
#define SYS_SYSINFO         92
#define SYS_SHUTDOWN        93
#define SYS_VGA_PUT_CHAR    94
#define SYS_VGA_SCROLL      95
#define SYS_VGA_CURSOR_ENABLE 96

/* =========================================================================
 * Raw syscall stubs — 1 through 6 arguments
 * ========================================================================= */

static inline long __syscall1(long num, long a1) {
    long ret;
    __asm__ volatile (
        "syscall"
        : "=a"(ret)
        : "a"(num), "D"(a1)
        : "rcx", "r11", "memory"
    );
    return ret;
}

static inline long __syscall2(long num, long a1, long a2) {
    long ret;
    __asm__ volatile (
        "syscall"
        : "=a"(ret)
        : "a"(num), "D"(a1), "S"(a2)
        : "rcx", "r11", "memory"
    );
    return ret;
}

static inline long __syscall3(long num, long a1, long a2, long a3) {
    long ret;
    __asm__ volatile (
        "syscall"
        : "=a"(ret)
        : "a"(num), "D"(a1), "S"(a2), "d"(a3)
        : "rcx", "r11", "memory"
    );
    return ret;
}

static inline long __syscall4(long num, long a1, long a2, long a3, long a4) {
    long ret;
    __asm__ volatile (
        "syscall"
        : "=a"(ret)
        : "a"(num), "D"(a1), "S"(a2), "d"(a3), "r"(a4)
        : "rcx", "r11", "memory"
    );
    return ret;
}

static inline long __syscall6(long num, long a1, long a2, long a3,
                               long a4, long a5, long a6) {
    long ret;
    register long r10 __asm__("r10") = a4;
    register long r8  __asm__("r8")  = a5;
    register long r9  __asm__("r9")  = a6;
    __asm__ volatile (
        "syscall"
        : "=a"(ret)
        : "a"(num), "D"(a1), "S"(a2), "d"(a3),
          "r"(r10), "r"(r8), "r"(r9)
        : "rcx", "r11", "memory"
    );
    return ret;
}

/* =========================================================================
 * POSIX syscall wrappers
 * ========================================================================= */

long sys_write(int fd, const void *buf, size_t count) {
    return __syscall3(SYS_WRITE, fd, (long)buf, (long)count);
}

long sys_read(int fd, void *buf, size_t count) {
    return __syscall3(SYS_READ, fd, (long)buf, (long)count);
}

long sys_exit(int code) {
    return __syscall1(SYS_EXIT, code);
}

long sys_fork(void) {
    return __syscall1(SYS_FORK, 0);
}

long sys_getpid(void) {
    return __syscall1(SYS_GETPID, 0);
}

long sys_wait(int *status) {
    return __syscall1(SYS_WAIT, (long)status);
}

long sys_getppid(void) {
    return __syscall1(SYS_GETPPID, 0);
}

long sys_brk(uint64_t addr) {
    return __syscall1(SYS_BRK, (long)addr);
}

long sys_mmap(uint64_t addr, uint64_t length, uint64_t prot, uint64_t flags) {
    return __syscall4(SYS_MMAP, (long)addr, (long)length,
                       (long)prot, (long)flags);
}

long sys_munmap(uint64_t addr, uint64_t length) {
    return __syscall2(SYS_MUNMAP, (long)addr, (long)length);
}

long sys_execve(const void *elf_data, size_t elf_size) {
    return __syscall2(SYS_EXECVE, (long)elf_data, (long)elf_size);
}

long sys_open(const char *path, int flags) {
    return __syscall2(SYS_OPEN, (long)path, (long)flags);
}

long sys_close(int fd) {
    return __syscall1(SYS_CLOSE, (long)fd);
}

long sys_fstat(int fd, void *buf) {
    return __syscall2(SYS_FSTAT, (long)fd, (long)buf);
}

long sys_lseek(int fd, long offset, int whence) {
    return __syscall3(SYS_LSEEK, (long)fd, offset, (long)whence);
}

long sys_unlink(const char *path) {
    return __syscall1(SYS_UNLINK, (long)path);
}

long sys_getdents(int fd, void *buf, unsigned int count) {
    return __syscall3(SYS_GETDENTS, (long)fd, (long)buf, (long)count);
}

long sys_rename(const char *old, const char *new_name) {
    return __syscall2(SYS_RENAME, (long)old, (long)new_name);
}

long sys_usleep(long usec) {
    return __syscall1(SYS_USLEEP, usec);
}

long sys_gettimeofday(void *tv) {
    return __syscall1(SYS_GETTIMEOFDAY, (long)tv);
}

long sys_mkdir(const char *path, unsigned int mode) {
    return __syscall2(SYS_MKDIR, (long)path, (long)mode);
}

long sys_rmdir(const char *path) {
    return __syscall1(SYS_RMDIR, (long)path);
}

long sys_get_key(void) {
    return __syscall1(SYS_GET_KEY, 0);
}

long sys_vga_write(const char *buf, size_t len) {
    return __syscall2(SYS_VGA_WRITE, (long)buf, (long)len);
}

long sys_vga_clear(void) {
    return __syscall1(SYS_VGA_CLEAR, 0);
}

long sys_vga_set_color(int fg, int bg) {
    return __syscall2(SYS_VGA_SET_COLOR, fg, bg);
}

long sys_beep(uint32_t freq, uint32_t duration_ms) {
    return __syscall2(SYS_BEEP, (long)freq, (long)duration_ms);
}

long sys_reboot(void) {
    return __syscall1(SYS_REBOOT, 0);
}

long sys_vga_cursor_left(void) {
    return __syscall1(SYS_VGA_CURSOR_LEFT, 0);
}

long sys_vga_cursor_right(void) {
    return __syscall1(SYS_VGA_CURSOR_RIGHT, 0);
}

long sys_vga_backspace(void) {
    return __syscall1(SYS_VGA_BACKSPACE, 0);
}

long sys_vga_insert_char(char c) {
    return __syscall1(SYS_VGA_INSERT_CHAR, c);
}

long sys_vga_get_cursor(void) {
    return __syscall1(SYS_VGA_GET_CURSOR, 0);
}

long sys_vga_set_cursor(int x, int y) {
    return __syscall2(SYS_VGA_SET_CURSOR, x, y);
}

long sys_vga_put_char(int x, int y, char c, int color) {
    return __syscall4(SYS_VGA_PUT_CHAR, x, y, c, color);
}

long sys_vga_scroll(int lines) {
    return __syscall1(SYS_VGA_SCROLL, lines);
}

long sys_vga_cursor_enable(int start, int end) {
    return __syscall2(SYS_VGA_CURSOR_ENABLE, start, end);
}

long sys_uptime(void) {
    return __syscall1(SYS_UPTIME, 0);
}

long sys_sysinfo(void *info) {
    return __syscall1(SYS_SYSINFO, (long)info);
}

long sys_shutdown(void) {
    return __syscall1(SYS_SHUTDOWN, 0);
}
