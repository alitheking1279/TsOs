/**
 * @file user_main.c
 * @brief Ring-3 program entry point (compiled into user_shell.elf).
 *
 * user_task_entry is the C entry point called from entry.asm's _start.
 * In the ring-3 shell build this drives shell_main(); the placeholder
 * body is used to prove the scheduler -> IRETQ -> syscall path first.
 */

#include <stdint.h>
#include "../libc/userspace/syscalls.h"

static void u64_to_dec(uint64_t v, char *buf) {
    if (v == 0) { buf[0] = '0'; buf[1] = '\0'; return; }
    char tmp[24]; int i = 0;
    while (v > 0) { tmp[i++] = '0' + (int)(v % 10); v /= 10; }
    for (int j = 0; j < i; j++) buf[j] = tmp[i - 1 - j];
    buf[i] = '\0';
}

static void vga_puts(const char *s) {
    long n = 0; while (s[n]) n++;
    sys_vga_write(s, (size_t)n);
}

void user_task_entry(void) {
    sys_vga_clear();
    sys_vga_set_color(0x0F, 0x00);

    vga_puts("TsOs: ring 3 online\r\n");

    sysinfo_t info;
    if (sys_sysinfo(&info) == 0) {
        char buf[32];
        vga_puts("  mem_total : ");
        u64_to_dec(info.mem_total / (1024 * 1024), buf);
        vga_puts(buf);
        vga_puts(" MB\r\n");
        vga_puts("  mem_free  : ");
        u64_to_dec(info.mem_free / (1024 * 1024), buf);
        vga_puts(buf);
        vga_puts(" MB\r\n");
        vga_puts("  tasks     : ");
        u64_to_dec(info.task_count, buf);
        vga_puts(buf);
        vga_puts("\r\n");
        vga_puts("  uptime    : ");
        u64_to_dec(info.uptime_ticks / 100, buf);
        vga_puts(buf);
        vga_puts(" s\r\n");
    } else {
        vga_puts("  sys_sysinfo failed\r\n");
    }

    for (;;) { __asm__ volatile ("nop"); }
}
