#include "shell.h"
#include "../drivers/vga.h"
#include "../drivers/ps2.h"
#include "../drivers/pcspk.h"
#include "../drivers/serial.h"
#include "../lib/print.h"
#include "../kernel/task.h"
#include "../kernel/timer.h"
#include "../kernel/pmm.h"
#include "../kernel/scheduler.h"
#include "../drivers/portio.h"
#include "../fs/vfs.h"
#include "../fs/ext2.h"
#include "../fs/bcache.h"
#include "../kernel/kheap.h"
#include <stdint.h>
#include <stddef.h>
#include <string.h>

extern serial_dev_t g_serial;

#define SHELL_BUF_SIZE  256
#define SHELL_MAX_ARGS  16
#define HISTORY_SIZE    32

/* Control characters (Ctrl+letter decoded by PS/2 driver). */
#define CTRL_A  0x01
#define CTRL_C  0x03
#define CTRL_E  0x05
#define CTRL_K  0x0B
#define CTRL_L  0x0C
#define CTRL_U  0x15
#define CTRL_W  0x17

/* CP437 box-drawing and block characters. */
#define BOX_TL     '\xC9'   /* ╔ */
#define BOX_TR     '\xBB'   /* ╗ */
#define BOX_BL     '\xC8'   /* ╚ */
#define BOX_BR     '\xBC'   /* ╝ */
#define BOX_H      '\xCD'   /* ═ */
#define BOX_V      '\xBA'   /* ║ */
#define BOX_ML     '\xCC'   /* ╠ */
#define BOX_MR     '\xB9'   /* ╣ */
#define BLOCK_FULL '\xDB'   /* █ */
#define BLOCK_LITE '\xB0'   /* ░ */
#define BLOCK_MED  '\xB1'   /* ▒ */
#define DCHEV_R    '\xBB'   /* » (reuse BOX_TR value — same glyph) */

static char g_line_buf[SHELL_BUF_SIZE];
static int  g_line_len = 0;
static int  g_line_pos = 0;
static int  g_prompt_col = 0;
static int  g_prompt_row = 0;

/* Command history. */
static char g_history[HISTORY_SIZE][SHELL_BUF_SIZE];
static int  g_history_count = 0;
static int  g_history_idx   = -1;
static int  g_history_saved_len = 0;

/* =========================================================================
 * Utility helpers
 * ========================================================================= */

static int shell_strlen(const char *s) {
    int n = 0; while (s[n]) n++; return n;
}

static int shell_strcmp(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

static void shell_strcpy(char *dst, const char *src) {
    while (*src) *dst++ = *src++; *dst = '\0';
}

static void shell_itoa(int val, char *buf) {
    int i = 0;
    if (val == 0) { buf[i++] = '0'; }
    else {
        char tmp[12]; int neg = 0;
        if (val < 0) { neg = 1; val = -val; }
        while (val > 0) { tmp[i] = '0' + (val % 10); val /= 10; i++; }
        if (neg) { tmp[i++] = '-'; }
        for (int j = 0; j < i; j++) buf[j] = tmp[i - 1 - j];
    }
    buf[i] = '\0';
}

static void shell_u64toa(uint64_t val, char *buf) {
    if (val == 0) { buf[0] = '0'; buf[1] = '\0'; return; }
    char tmp[24]; int i = 0;
    while (val > 0) { tmp[i++] = '0' + (int)(val % 10); val /= 10; }
    for (int j = 0; j < i; j++) buf[j] = tmp[i - 1 - j];
    buf[i] = '\0';
}

static void shell_fmt_bytes(uint64_t bytes, char *buf) {
    if (bytes >= 1024 * 1024) {
        shell_u64toa(bytes / (1024 * 1024), buf);
        int l = 0; while (buf[l]) l++;
        buf[l++] = ' '; buf[l++] = 'M'; buf[l++] = 'B'; buf[l] = '\0';
    } else if (bytes >= 1024) {
        shell_u64toa(bytes / 1024, buf);
        int l = 0; while (buf[l]) l++;
        buf[l++] = ' '; buf[l++] = 'K'; buf[l++] = 'B'; buf[l] = '\0';
    } else {
        shell_u64toa(bytes, buf);
        int l = 0; while (buf[l]) l++;
        buf[l++] = ' '; buf[l++] = 'B'; buf[l] = '\0';
    }
}

/* =========================================================================
 * Low-level VGA write helpers
 * ========================================================================= */

static void shell_print(const char *s) {
    int x = vga_get_cursor_x();
    int y = vga_get_cursor_y();
    vga_put_str(x, y, s, vga_get_color());
    vga_cursor_set(vga_get_cursor_x(), vga_get_cursor_y());
}

static void shell_vga_write(char c) {
    int x = vga_get_cursor_x();
    int y = vga_get_cursor_y();
    if (c == '\r') {
        vga_cursor_set(0, y);
    } else if (c == '\n') {
        x = 0; y++;
        if (y >= VGA_HEIGHT) { vga_scroll(1); vga_flush(); y = VGA_HEIGHT - 1; }
        vga_cursor_set(x, y);
    } else {
        vga_put_char(x, y, c, vga_get_color());
        vga_flush_cell(x, y);
        x++;
        if (x >= VGA_WIDTH) {
            x = 0; y++;
            if (y >= VGA_HEIGHT) { vga_scroll(1); vga_flush(); y = VGA_HEIGHT - 1; }
        }
        vga_cursor_set(x, y);
    }
}

static void shell_vga_cursor_left(void) {
    int x = vga_get_cursor_x(), y = vga_get_cursor_y();
    if (x > 0) vga_cursor_set(x - 1, y);
    else if (y > 0) vga_cursor_set(VGA_WIDTH - 1, y - 1);
}

static void shell_vga_cursor_right(void) {
    int x = vga_get_cursor_x(), y = vga_get_cursor_y();
    if (x < VGA_WIDTH - 1) vga_cursor_set(x + 1, y);
    else if (y < VGA_HEIGHT - 1) vga_cursor_set(0, y + 1);
}

static void shell_vga_backspace(void) {
    int x = vga_get_cursor_x(), y = vga_get_cursor_y();
    if (x == 0 && y == 0) return;
    if (x > 0) x--; else { x = VGA_WIDTH - 1; y--; }
    vga_put_char(x, y, ' ', vga_get_color());
    vga_flush_cell(x, y);
    vga_cursor_set(x, y);
}

static void shell_vga_insert_char(char c) {
    int x = vga_get_cursor_x(), y = vga_get_cursor_y();
    vga_put_char(x, y, c, vga_get_color());
    vga_flush_cell(x, y);
    x++;
    if (x >= VGA_WIDTH) {
        x = 0; y++;
        if (y >= VGA_HEIGHT) { vga_scroll(1); vga_flush(); y = VGA_HEIGHT - 1; }
    }
    vga_cursor_set(x, y);
}

/* =========================================================================
 * Drawing primitives
 * ========================================================================= */

static void shell_put_str_at(int x, int y, const char *s, uint8_t color) {
    while (*s && x < VGA_WIDTH) vga_put_char(x++, y, *s++, color);
}

static void shell_put_centered(int y, const char *s, uint8_t color) {
    int len = shell_strlen(s);
    int x   = (VGA_WIDTH - len) / 2;
    if (x < 0) x = 0;
    shell_put_str_at(x, y, s, color);
}

/* Draw a full-width horizontal border row with optional left/mid/right caps. */
static void shell_draw_hborder(int y, char lc, char mc, char rc, uint8_t color) {
    vga_put_char(0,  y, lc, color);
    for (int x = 1; x < 79; x++) vga_put_char(x, y, mc, color);
    vga_put_char(79, y, rc, color);
}

/* Draw side pillars ║ on row y (columns 0 and 79). */
static void shell_draw_sides(int y, uint8_t color) {
    vga_put_char(0,  y, BOX_V, color);
    vga_put_char(79, y, BOX_V, color);
}

/* =========================================================================
 * Logo letter bitmaps  (5 rows each)
 *
 *   T  = 8 cols    s  = 8 cols    O  = 10 cols   s  = 8 cols
 *   gaps: 3 × 2 spaces  →  total 40 cols, centered at col (80-40)/2 = 20
 * ========================================================================= */

static const char *LOGO_T[5] = {
    "\xDB\xDB\xDB\xDB\xDB\xDB\xDB\xDB",
    "   \xDB\xDB   ",
    "   \xDB\xDB   ",
    "   \xDB\xDB   ",
    "   \xDB\xDB   ",
};
static const char *LOGO_S[5] = {
    " \xDB\xDB\xDB\xDB\xDB\xDB\xDB",
    "\xDB\xDB      ",
    " \xDB\xDB\xDB\xDB\xDB\xDB ",
    "      \xDB\xDB",
    "\xDB\xDB\xDB\xDB\xDB\xDB\xDB ",
};
static const char *LOGO_O[5] = {
    " \xDB\xDB\xDB\xDB\xDB\xDB\xDB\xDB ",
    "\xDB\xDB      \xDB\xDB",
    "\xDB\xDB      \xDB\xDB",
    "\xDB\xDB      \xDB\xDB",
    " \xDB\xDB\xDB\xDB\xDB\xDB\xDB\xDB ",
};

static void logo_build_row(int row, char *buf) {
    int i = 0;
    for (const char *p = LOGO_T[row]; *p; p++) buf[i++] = *p;
    buf[i++] = ' '; buf[i++] = ' ';
    for (const char *p = LOGO_S[row]; *p; p++) buf[i++] = *p;
    buf[i++] = ' '; buf[i++] = ' ';
    for (const char *p = LOGO_O[row]; *p; p++) buf[i++] = *p;
    buf[i++] = ' '; buf[i++] = ' ';
    for (const char *p = LOGO_S[row]; *p; p++) buf[i++] = *p;
    buf[i] = '\0';
}

/* =========================================================================
 * Animated Boot Splash
 *
 * Full 25-row layout:
 *   Row  0   ╔══...══╗
 *   Row  1   ║       ║   (blank)
 *   Rows 2-6 ║  TsOs logo  ║
 *   Row  7   ║       ║   (blank)
 *   Row  8   ║  tagline    ║
 *   Row  9   ║  version    ║
 *   Row 10   ║       ║   (blank)
 *   Row 11   ╠══...══╣
 *   Row 12   ║  subsystems ║
 *   Row 13   ╠══...══╣
 *   Row 14   ║       ║   (blank)
 *   Row 15   ║  Booting... ║
 *   Row 16   ║  [bar]      ║
 *   Row 17   ║       ║   (blank)
 *   Row 18   ╚══...══╝
 *   Rows 19-24  (clear — not inside box)
 * ========================================================================= */

#define SPLASH_BAR_WIDTH   50   /* inner fill width of loading bar */
#define SPLASH_BAR_STEPS   25   /* number of animation steps       */
#define SPLASH_TICKS_TOTAL 300  /* ~3 seconds at 100 Hz            */

static void splash_draw_frame(void) {
    vga_batch_start();
    vga_clear();

    const uint8_t col_border  = vga_make_color(VGA_COLOR_LIGHT_CYAN,  VGA_COLOR_BLACK);
    const uint8_t col_logo    = vga_make_color(VGA_COLOR_WHITE,        VGA_COLOR_BLACK);
    const uint8_t col_tagline = vga_make_color(VGA_COLOR_LIGHT_GRAY,   VGA_COLOR_BLACK);
    const uint8_t col_dim     = vga_make_color(VGA_COLOR_DARK_GRAY,    VGA_COLOR_BLACK);
    const uint8_t col_yellow  = vga_make_color(VGA_COLOR_YELLOW,       VGA_COLOR_BLACK);

    /* Borders */
    shell_draw_hborder(0,  BOX_TL, BOX_H, BOX_TR, col_border);
    for (int y = 1; y <= 10; y++) shell_draw_sides(y, col_border);
    shell_draw_hborder(11, BOX_ML, BOX_H, BOX_MR, col_border);
    shell_draw_sides(12, col_border);
    shell_draw_hborder(13, BOX_ML, BOX_H, BOX_MR, col_border);
    for (int y = 14; y <= 17; y++) shell_draw_sides(y, col_border);
    shell_draw_hborder(18, BOX_BL, BOX_H, BOX_BR, col_border);

    /* Logo rows 2-6 */
    char logo_row_buf[48];
    for (int r = 0; r < 5; r++) {
        logo_build_row(r, logo_row_buf);
        int len = shell_strlen(logo_row_buf);
        int x   = 1 + (78 - len) / 2;
        shell_put_str_at(x, 2 + r, logo_row_buf, col_logo);
    }

    /* Tagline / version */
    shell_put_centered(8,  "Truly Simple Operating System",               col_tagline);
    shell_put_centered(9,  "v1.0  \xB3  x86-64  \xB3  ext2  \xB3  MLFQ Scheduler", col_dim);

    /* Subsystems row 12 */
    shell_put_centered(12, "Kernel  \xFA  PMM/Buddy/Slab  \xFA  VFS/ext2  \xFA  MLFQ@100Hz", col_dim);

    /* Boot label row 15 */
    shell_put_centered(15, "Booting TsOs...", col_yellow);

    /* Bar shell row 16 — drawn empty first */
    {
        /* "[" at col 14, "]" at col 14+1+50+1 = 66, label after */
        int bx = (VGA_WIDTH - (SPLASH_BAR_WIDTH + 2)) / 2;
        vga_put_char(bx, 16, '[', col_dim);
        for (int i = 0; i < SPLASH_BAR_WIDTH; i++)
            vga_put_char(bx + 1 + i, 16, BLOCK_LITE, col_dim);
        vga_put_char(bx + 1 + SPLASH_BAR_WIDTH, 16, ']', col_dim);
    }

    vga_batch_end();
}

static void splash_update_bar(int step) {
    /* step = 0..SPLASH_BAR_STEPS */
    const uint8_t col_bar     = vga_make_color(VGA_COLOR_LIGHT_CYAN,  VGA_COLOR_BLACK);
    const uint8_t col_dim     = vga_make_color(VGA_COLOR_DARK_GRAY,   VGA_COLOR_BLACK);
    const uint8_t col_pct     = vga_make_color(VGA_COLOR_WHITE,       VGA_COLOR_BLACK);

    int bx       = (VGA_WIDTH - (SPLASH_BAR_WIDTH + 2)) / 2;
    int filled   = step * SPLASH_BAR_WIDTH / SPLASH_BAR_STEPS;

    for (int i = 0; i < SPLASH_BAR_WIDTH; i++) {
        char c     = (i < filled) ? BLOCK_FULL : BLOCK_LITE;
        uint8_t cl = (i < filled) ? col_bar    : col_dim;
        vga_put_char(bx + 1 + i, 16, c, cl);
        vga_flush_cell(bx + 1 + i, 16);
    }

    /* Percentage label — "XXX%" — right of bar */
    int pct  = step * 100 / SPLASH_BAR_STEPS;
    char pct_buf[8];
    shell_itoa(pct, pct_buf);
    /* append '%' */
    int pl = shell_strlen(pct_buf);
    pct_buf[pl++] = '%'; pct_buf[pl++] = ' '; pct_buf[pl] = '\0';

    int px = bx + 1 + SPLASH_BAR_WIDTH + 2;
    /* clear 5 chars then write */
    for (int i = 0; i < 5; i++) {
        vga_put_char(px + i, 16, ' ', col_pct);
        vga_flush_cell(px + i, 16);
    }
    for (int i = 0; pct_buf[i]; i++) {
        vga_put_char(px + i, 16, pct_buf[i], col_pct);
        vga_flush_cell(px + i, 16);
    }
}

static void shell_draw_splash_animated(void) {
    splash_draw_frame();

    uint64_t start  = timer_get_ticks();
    uint64_t total  = SPLASH_TICKS_TOTAL;
    int      last_step = -1;

    while (1) {
        uint64_t elapsed = timer_get_ticks() - start;
        if (elapsed >= total) elapsed = total;

        int step = (int)(elapsed * SPLASH_BAR_STEPS / total);
        if (step > SPLASH_BAR_STEPS) step = SPLASH_BAR_STEPS;

        if (step != last_step) {
            splash_update_bar(step);
            last_step = step;
        }
        if (elapsed >= total) break;
    }

    /* Small pause at 100% then clear for clean shell. */
    uint64_t pause_start = timer_get_ticks();
    while (timer_get_ticks() - pause_start < 40) { /* ~0.4 s */ }

    vga_clear();
    vga_cursor_set(0, 0);
}

/* =========================================================================
 * Compact static banner — used by 'banner' command after boot.
 * 14 rows, leaves rows 14-24 clear for shell output.
 * ========================================================================= */

static void shell_draw_banner(void) {
    vga_batch_start();
    vga_clear();

    const uint8_t col_border  = vga_make_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
    const uint8_t col_logo    = vga_make_color(VGA_COLOR_WHITE,       VGA_COLOR_BLACK);
    const uint8_t col_tagline = vga_make_color(VGA_COLOR_LIGHT_GRAY,  VGA_COLOR_BLACK);
    const uint8_t col_dim     = vga_make_color(VGA_COLOR_DARK_GRAY,   VGA_COLOR_BLACK);

    shell_draw_hborder(0,  BOX_TL, BOX_H, BOX_TR, col_border);
    for (int y = 1; y <= 10; y++) shell_draw_sides(y, col_border);
    shell_draw_hborder(11, BOX_ML, BOX_H, BOX_MR, col_border);
    shell_draw_sides(12, col_border);
    shell_draw_hborder(13, BOX_BL, BOX_H, BOX_BR, col_border);

    char logo_row_buf[48];
    for (int r = 0; r < 5; r++) {
        logo_build_row(r, logo_row_buf);
        int len = shell_strlen(logo_row_buf);
        int x   = 1 + (78 - len) / 2;
        shell_put_str_at(x, 2 + r, logo_row_buf, col_logo);
    }

    shell_put_centered(8,  "Truly Simple Operating System",          col_tagline);
    shell_put_centered(9,  "v1.0  \xB3  x86-64  \xB3  ext2  \xB3  MLFQ",  col_dim);
    shell_put_centered(12, "Type 'help' \xB3 'help fs' \xB3 'help keys'", col_dim);

    vga_batch_end();
    vga_cursor_set(0, 15);
}

/* =========================================================================
 * Command history
 * ========================================================================= */

static void shell_redraw_input(void);

static void shell_history_store(const char *line) {
    if (line[0] == '\0') return;
    if (g_history_count > 0) {
        int last = (g_history_count - 1) % HISTORY_SIZE;
        int same = 1;
        for (int i = 0; g_history[last][i] || line[i]; i++) {
            if (g_history[last][i] != line[i]) { same = 0; break; }
        }
        if (same) return;
    }
    int slot = g_history_count % HISTORY_SIZE;
    shell_strcpy(g_history[slot], line);
    g_history_count++;
    g_history_idx = -1;
}

static void shell_history_prev(void) {
    if (g_history_count == 0) return;
    if (g_history_idx == -1) {
        g_history_saved_len = g_line_len;
        g_history[g_history_count % HISTORY_SIZE][0] = '\0';
        shell_strcpy(g_history[g_history_count % HISTORY_SIZE], g_line_buf);
        g_history_idx = (g_history_count - 1) % HISTORY_SIZE;
    } else if (g_history_idx != (g_history_count - 1) % HISTORY_SIZE ||
               g_history_count <= HISTORY_SIZE) {
        int slots = g_history_count < HISTORY_SIZE ? g_history_count : HISTORY_SIZE;
        int cur_visual = -1;
        for (int i = 0; i < slots; i++) {
            int idx = (g_history_count - 1 - i) % HISTORY_SIZE;
            if (idx == g_history_idx) { cur_visual = i; break; }
        }
        if (cur_visual >= 0 && cur_visual < slots - 1)
            g_history_idx = (g_history_count - 1 - (cur_visual + 1)) % HISTORY_SIZE;
    }
    shell_strcpy(g_line_buf, g_history[g_history_idx]);
    g_line_len = 0;
    while (g_line_buf[g_line_len]) g_line_len++;
    g_line_pos = g_line_len;
    shell_redraw_input();
}

static void shell_history_next(void) {
    if (g_history_idx == -1) return;
    int slots = g_history_count < HISTORY_SIZE ? g_history_count : HISTORY_SIZE;
    int cur_visual = -1;
    for (int i = 0; i < slots; i++) {
        int idx = (g_history_count - 1 - i) % HISTORY_SIZE;
        if (idx == g_history_idx) { cur_visual = i; break; }
    }
    if (cur_visual > 0) {
        g_history_idx = (g_history_count - 1 - (cur_visual - 1)) % HISTORY_SIZE;
        shell_strcpy(g_line_buf, g_history[g_history_idx]);
        g_line_len = 0;
        while (g_line_buf[g_line_len]) g_line_len++;
        g_line_pos = g_line_len;
    } else {
        g_history_idx = -1;
        shell_strcpy(g_line_buf, g_history[g_history_count % HISTORY_SIZE]);
        g_line_len = g_history_saved_len;
        g_line_buf[g_line_len] = '\0';
        g_line_pos = g_line_len;
    }
    shell_redraw_input();
}

static void shell_redraw_input(void) {
    int sx = g_prompt_col, sy = g_prompt_row;
    int cx = sx, cy = sy;

    for (int i = 0; i < g_line_len; i++) {
        if (cx >= VGA_WIDTH) { cx = 0; cy++; }
        if (cy >= VGA_HEIGHT) { vga_scroll(1); vga_flush(); cy = VGA_HEIGHT - 1; }
        vga_put_char(cx, cy, g_line_buf[i], vga_get_color());
        cx++;
    }
    if (cx < VGA_WIDTH && cy < VGA_HEIGHT)
        vga_put_char(cx, cy, ' ', vga_get_color());

    cx = sx; cy = sy;
    for (int i = 0; i <= g_line_len; i++) {
        if (cx >= VGA_WIDTH) { cx = 0; cy++; }
        if (cy >= VGA_HEIGHT) break;
        vga_flush_cell(cx, cy);
        cx++;
    }

    cx = sx; cy = sy;
    for (int i = 0; i < g_line_pos; i++) {
        cx++;
        if (cx >= VGA_WIDTH) { cx = 0; cy++; }
    }
    vga_cursor_set(cx, cy);
}

/* =========================================================================
 * Argument parser
 * ========================================================================= */

static void shell_parse_args(char *line, char *args[], int *argc) {
    *argc = 0;
    int in_word = 0;
    while (*line) {
        if (*line == ' ' || *line == '\t') { *line = 0; in_word = 0; }
        else {
            if (!in_word) { args[*argc] = line; (*argc)++; in_word = 1; }
        }
        line++;
    }
}

/* =========================================================================
 * Built-in: help  /  help fs  /  help keys
 * ========================================================================= */

static void cmd_help_general(void) {
    const uint8_t col_hdr = vga_make_color(VGA_COLOR_LIGHT_CYAN,  VGA_COLOR_BLACK);
    const uint8_t col_cmd = vga_make_color(VGA_COLOR_WHITE,        VGA_COLOR_BLACK);
    const uint8_t col_dim = vga_make_color(VGA_COLOR_DARK_GRAY,    VGA_COLOR_BLACK);

    (void)col_hdr; (void)col_cmd; (void)col_dim;

    shell_print("  General Commands                  'help fs'  'help keys' for more\r\n");
    shell_print("  \xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\r\n");
    shell_print("  help              This screen\r\n");
    shell_print("  ver               OS version string\r\n");
    shell_print("  sysinfo           Neofetch-style system info\r\n");
    shell_print("  mem               Physical memory stats\r\n");
    shell_print("  tasks             Running task list\r\n");
    shell_print("  uptime            System uptime\r\n");
    shell_print("  pid               Current process ID\r\n");
    shell_print("  echo [text]       Print to screen\r\n");
    shell_print("  color FG BG       Set colors (0-15)\r\n");
    shell_print("  beep FREQ DUR     PC speaker beep\r\n");
    shell_print("  clear             Clear screen\r\n");
    shell_print("  banner            Redraw header banner\r\n");
    shell_print("  reboot            Reboot system\r\n");
    shell_print("  exit              Exit shell\r\n");
}

static void cmd_help_fs(void) {
    shell_print("  Filesystem Commands\r\n");
    shell_print("  \xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\r\n");
    shell_print("  ls [path]         List directory\r\n");
    shell_print("  cat <path>        Print file contents\r\n");
    shell_print("  touch <path>      Create empty file\r\n");
    shell_print("  rm <path>         Remove file\r\n");
    shell_print("  mkdir <path>      Create directory\r\n");
    shell_print("  rmdir <path>      Remove empty directory\r\n");
    shell_print("  write <p> <txt>   Write text to file\r\n");
    shell_print("  mv <src> <dst>    Move / rename\r\n");
    shell_print("  cp <src> <dst>    Copy file\r\n");
    shell_print("  info              Filesystem stats\r\n");
}

static void cmd_help_keys(void) {
    shell_print("  Key Bindings\r\n");
    shell_print("  \xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\r\n");
    shell_print("  Up / Down         Browse history\r\n");
    shell_print("  Left / Right      Move cursor\r\n");
    shell_print("  Home / End        Jump to line start / end\r\n");
    shell_print("  Ctrl+A / Ctrl+E   Home / End\r\n");
    shell_print("  Ctrl+U            Clear line\r\n");
    shell_print("  Ctrl+K            Kill to end of line\r\n");
    shell_print("  Ctrl+W            Delete word backward\r\n");
    shell_print("  Ctrl+C            Cancel input\r\n");
    shell_print("  Ctrl+L            Clear screen\r\n");
    shell_print("  Delete            Delete char at cursor\r\n");
}

static void cmd_help(int argc, char *args[]) {
    if (argc >= 2 && shell_strcmp(args[1], "fs")   == 0) { cmd_help_fs();   return; }
    if (argc >= 2 && shell_strcmp(args[1], "keys") == 0) { cmd_help_keys(); return; }
    cmd_help_general();
}

/* =========================================================================
 * Built-in: ver
 * ========================================================================= */

static void cmd_ver(void) {
    shell_print("  TsOs v1.0  \xB3  x86-64 bare-metal\r\n");
    shell_print("  Arch    : x86-64 (long mode)\r\n");
    shell_print("  Memory  : PMM bitmap + buddy + slab\r\n");
    shell_print("  FS      : ext2 (read/write) via VFS\r\n");
    shell_print("  Sched   : MLFQ preemptive  100 Hz\r\n");
    shell_print("  Shell   : built-in interactive\r\n");
}

/* =========================================================================
 * Built-in: sysinfo
 * ========================================================================= */

static void cmd_sysinfo(void) {
    uint64_t ticks   = timer_get_ticks();
    uint64_t seconds = ticks / 100;
    uint64_t minutes = seconds / 60;
    uint64_t hours   = minutes / 60;
    seconds %= 60; minutes %= 60;

    pmm_stats_t ps;
    pmm_get_stats(&ps);
    uint64_t total_mb = (ps.total_frames * 4096ULL) / (1024*1024);
    uint64_t used_mb  = (ps.used_frames  * 4096ULL) / (1024*1024);
    uint64_t free_mb  = (ps.free_frames  * 4096ULL) / (1024*1024);
    uint64_t ntasks   = scheduler_get_task_count();
    char buf[32];

    shell_print("  TsOs v1.0\r\n");
    shell_print("  \xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\r\n");

    shell_print("  OS      : TsOs v1.0  (x86-64)\r\n");
    shell_print("  Kernel  : Monolithic  MLFQ scheduler\r\n");
    shell_print("  FS      : ext2 rev 0  read/write\r\n");
    shell_print("  Timer   : PIT IRQ0  100 Hz\r\n");
    shell_print("  Storage : ATA PIO  IDE\r\n");
    shell_print("  Display : VGA text  80x25\r\n");

    shell_print("  Uptime  : ");
    shell_itoa((int)hours,   buf); shell_print(buf); shell_print("h ");
    shell_itoa((int)minutes, buf); shell_print(buf); shell_print("m ");
    shell_itoa((int)seconds, buf); shell_print(buf); shell_print("s\r\n");

    shell_print("  RAM     : ");
    shell_u64toa(total_mb, buf); shell_print(buf); shell_print(" MB total   ");
    shell_u64toa(used_mb,  buf); shell_print(buf); shell_print(" MB used   ");
    shell_u64toa(free_mb,  buf); shell_print(buf); shell_print(" MB free\r\n");

    shell_print("  Tasks   : ");
    shell_u64toa(ntasks, buf); shell_print(buf); shell_print(" active\r\n");
}

/* =========================================================================
 * Built-in: mem
 * ========================================================================= */

static void cmd_mem(void) {
    pmm_stats_t ps;
    pmm_get_stats(&ps);
    uint64_t total_bytes = ps.total_frames * 4096ULL;
    uint64_t free_bytes  = ps.free_frames  * 4096ULL;
    uint64_t used_bytes  = ps.used_frames  * 4096ULL;
    char buf[32];

    shell_print("  Physical Memory\r\n");
    shell_print("  \xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\r\n");

    shell_print("  Total  : ");
    shell_u64toa(ps.total_frames, buf); shell_print(buf); shell_print(" frames  (");
    shell_fmt_bytes(total_bytes, buf);  shell_print(buf); shell_print(")\r\n");

    shell_print("  Free   : ");
    shell_u64toa(ps.free_frames, buf);  shell_print(buf); shell_print(" frames  (");
    shell_fmt_bytes(free_bytes,  buf);  shell_print(buf); shell_print(")\r\n");

    shell_print("  Used   : ");
    shell_u64toa(ps.used_frames, buf);  shell_print(buf); shell_print(" frames  (");
    shell_fmt_bytes(used_bytes,  buf);  shell_print(buf); shell_print(")\r\n");

    shell_print("  Rsvd   : ");
    shell_u64toa(ps.reserved_frames, buf); shell_print(buf); shell_print(" frames  (kernel + bitmap)\r\n");

    /* Usage bar using block chars █ and ░ */
    shell_print("  Usage  : [");
    uint64_t bar_used = (ps.total_frames > 0)
        ? (ps.used_frames * 40 / ps.total_frames) : 0;
    for (uint64_t i = 0; i < 40; i++) {
        char c[2]; c[1] = '\0';
        c[0] = (i < bar_used) ? BLOCK_FULL : BLOCK_LITE;
        shell_print(c);
    }
    shell_print("]\r\n");
}

/* =========================================================================
 * Built-in: tasks
 * ========================================================================= */

static const char *task_state_str(task_state_t state) {
    switch (state) {
        case TASK_STATE_CREATED:  return "NEW    ";
        case TASK_STATE_READY:    return "READY  ";
        case TASK_STATE_RUNNING:  return "RUN    ";
        case TASK_STATE_BLOCKED:  return "BLOCK  ";
        case TASK_STATE_SLEEPING: return "SLEEP  ";
        case TASK_STATE_ZOMBIE:   return "ZOMBIE ";
        case TASK_STATE_DEAD:     return "DEAD   ";
        default:                  return "?      ";
    }
}

static void cmd_tasks(void) {
    shell_print("  PID   STATE    LVL  NAME\r\n");
    shell_print("  \xC4\xC4\xC4\xC4  \xC4\xC4\xC4\xC4\xC4\xC4\xC4  \xC4\xC4\xC4  \xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\r\n");

    task_t *t = task_get_list_head();
    int count = 0;
    while (t) {
        char pid_buf[16], lvl_buf[4];
        shell_itoa((int)t->pid, pid_buf);

        shell_print("  ");
        shell_print(pid_buf);
        int plen = shell_strlen(pid_buf);
        for (int s = plen; s < 4; s++) shell_print(" ");

        shell_print("  ");
        shell_print(task_state_str(t->state));
        shell_print("  ");

        shell_itoa(t->mlfq_level, lvl_buf);
        shell_print(lvl_buf);
        for (int s = shell_strlen(lvl_buf); s < 3; s++) shell_print(" ");

        shell_print("  ");
        shell_print(t->name ? t->name : "(unnamed)");
        shell_print("\r\n");

        t = t->g_next;
        count++;
    }
    if (count == 0) {
        shell_print("  (no tasks)\r\n");
    } else {
        char cbuf[16];
        shell_print("  \xC4\xC4 ");
        shell_itoa(count, cbuf); shell_print(cbuf);
        shell_print(" task(s) \xC4\xC4\r\n");
    }
}

/* =========================================================================
 * Built-in: clear / banner / echo / uptime / pid / color / beep / reboot / exit
 * ========================================================================= */

static void cmd_clear(void) {
    vga_clear();
    vga_cursor_set(0, 0);
}

static void cmd_banner(void) {
    shell_draw_banner();
}

static void cmd_echo(int argc, char *args[]) {
    for (int i = 1; i < argc; i++) {
        if (i > 1) shell_print(" ");
        shell_print(args[i]);
    }
    shell_print("\r\n");
}

static void cmd_uptime(void) {
    uint64_t ticks   = timer_get_ticks();
    uint64_t seconds = ticks / 100;
    uint64_t minutes = seconds / 60;
    uint64_t hours   = minutes / 60;
    seconds %= 60; minutes %= 60;
    char buf[32];
    shell_print("Uptime: ");
    shell_itoa((int)hours,   buf); shell_print(buf); shell_print("h ");
    shell_itoa((int)minutes, buf); shell_print(buf); shell_print("m ");
    shell_itoa((int)seconds, buf); shell_print(buf); shell_print("s  (");
    shell_u64toa(ticks / 100, buf); shell_print(buf);
    shell_print("s total)\r\n");
}

static void cmd_pid(void) {
    task_t *cur = task_get_current();
    uint64_t pid = cur ? cur->pid : 0;
    char buf[16];
    shell_itoa((int)pid, buf);
    shell_print("PID: "); shell_print(buf); shell_print("\r\n");
}

static void cmd_color(int argc, char *args[]) {
    if (argc < 3) { shell_print("Usage: color FG BG  (0-15)\r\n"); return; }
    int fg = 0, bg = 0;
    for (int i = 0; args[1][i]; i++) fg = fg * 10 + (args[1][i] - '0');
    for (int i = 0; args[2][i]; i++) bg = bg * 10 + (args[2][i] - '0');
    if (fg < 0 || fg > 15 || bg < 0 || bg > 15) {
        shell_print("Colors must be 0-15\r\n"); return;
    }
    vga_set_color((uint8_t)fg, (uint8_t)bg);
}

static void cmd_beep(int argc, char *args[]) {
    if (argc < 3) { shell_print("Usage: beep FREQ_HZ DURATION_MS\r\n"); return; }
    uint32_t freq = 0, dur = 0;
    for (int i = 0; args[1][i]; i++) freq = freq * 10 + (args[1][i] - '0');
    for (int i = 0; args[2][i]; i++) dur  = dur  * 10 + (args[2][i] - '0');
    pcspk_beep(freq, dur);
}

static void cmd_reboot(void) {
    shell_print("Flushing buffers...\r\n");
    bcache_flush_all();
    uint8_t good = 0x02;
    while (good & 0x02) good = inb(0x64);
    outb(0x64, 0xFE);
    while (1) { asm volatile ("hlt"); }
}

static void cmd_exit(void) {
    shell_print("Flushing buffers...\r\n");
    bcache_flush_all();
    shell_print("Goodbye.\r\n");
    task_exit(0);
    while (1) { asm volatile ("hlt"); }
}

/* =========================================================================
 * Filesystem commands
 * ========================================================================= */

static void fs_abs_path(const char *in, char *out, int out_size) {
    if (in[0] == '/') {
        int i = 0;
        while (in[i] && i < out_size - 1) { out[i] = in[i]; i++; }
        out[i] = '\0';
    } else {
        out[0] = '/';
        int i = 0;
        while (in[i] && i < out_size - 2) { out[i + 1] = in[i]; i++; }
        out[i + 1] = '\0';
    }
}

static void cmd_ls(int argc, char *args[]) {
    const char *path = (argc > 1) ? args[1] : "/";
    char full[EXT2_MAX_PATH];
    fs_abs_path(path, full, EXT2_MAX_PATH);

    ext2_stat_t st;
    int stat_ret = vfs_stat(full, &st);
    if (stat_ret == 0 && !EXT2_S_ISDIR(st.mode)) {
        shell_print(full + 1); shell_print("\r\n"); return;
    }

    #define LS_NAME_W 24
    shell_print("File");
    int hdr_pad = LS_NAME_W - 4;
    for (int i = 0; i < hdr_pad; i++) shell_print(" ");
    shell_print("Size\r\n");

    uint64_t cookie = 0;
    uint8_t  dirent_buf[1024];
    int      total = 0;

    while (1) {
        int n = vfs_getdents(full, &cookie, dirent_buf, sizeof(dirent_buf));
        if (n <= 0) break;
        int offset = 0;
        while (offset < n) {
            ext2_dirent64_t *de = (ext2_dirent64_t *)(dirent_buf + offset);
            if (de->d_reclen == 0) break;
            offset += de->d_reclen;
            if (de->d_name[0] == '.') {
                if (de->d_name[1] == '\0' ||
                    (de->d_name[1] == '.' && de->d_name[2] == '\0')) continue;
            }

            int nlen = 0;
            while (nlen < 255 && de->d_name[nlen]) nlen++;

            char disp[260]; int k = 0;
            for (int i = 0; i < nlen; i++) disp[k++] = de->d_name[i];
            int is_dir = (de->d_type == EXT2_FT_DIR);
            if (is_dir) disp[k++] = '/';
            disp[k] = '\0';

            shell_print(disp);
            int pad = LS_NAME_W - k;
            if (pad < 1) pad = 1;
            for (int i = 0; i < pad; i++) shell_print(" ");

            if (is_dir) {
                shell_print("<DIR>\r\n");
            } else {
                ext2_stat_t fst;
                char sz[24];
                if (ext2_stat((uint32_t)de->d_ino, &fst) == EXT2_OK) {
                    shell_fmt_bytes(fst.size, sz);
                } else {
                    sz[0] = '?'; sz[1] = '\0';
                }
                shell_print(sz);
                shell_print("\r\n");
            }
            total++;
        }
    }
    if (total == 0) {
        shell_print("  (empty)\r\n");
    } else {
        char cbuf[16]; shell_print("  ");
        shell_itoa(total, cbuf); shell_print(cbuf); shell_print(" item(s)\r\n");
    }
    #undef LS_NAME_W
}

static void cmd_cat(int argc, char *args[]) {
    if (argc < 2) { shell_print("Usage: cat <path>\r\n"); return; }
    char full[EXT2_MAX_PATH];
    fs_abs_path(args[1], full, EXT2_MAX_PATH);
    ext2_stat_t st;
    if (vfs_stat(full, &st) != 0) {
        shell_print("cat: '"); shell_print(args[1]); shell_print("': not found\r\n"); return;
    }
    if (EXT2_S_ISDIR(st.mode)) {
        shell_print("cat: '"); shell_print(args[1]); shell_print("': is a directory\r\n"); return;
    }
    uint32_t ino = ext2_resolve_path(full);
    if (ino == 0) { shell_print("cat: cannot resolve path\r\n"); return; }
    uint8_t buf[512]; uint64_t remaining = st.size, offset = 0;
    while (remaining > 0) {
        uint64_t chunk = (remaining > sizeof(buf)) ? sizeof(buf) : remaining;
        int64_t n = ext2_read_file(ino, buf, offset, chunk);
        if (n <= 0) break;
        for (int64_t i = 0; i < n; i++) shell_vga_write((char)buf[i]);
        offset += (uint64_t)n; remaining -= (uint64_t)n;
    }
    if (offset > 0) shell_vga_write('\n');
}

static void cmd_touch(int argc, char *args[]) {
    if (argc < 2) { shell_print("Usage: touch <path>\r\n"); return; }
    char full[EXT2_MAX_PATH];
    fs_abs_path(args[1], full, EXT2_MAX_PATH);
    ext2_stat_t st;
    if (vfs_stat(full, &st) == 0) {
        shell_print("touch: '"); shell_print(args[1]); shell_print("': already exists\r\n"); return;
    }
    int fd = vfs_open(full, O_CREAT | O_WRONLY);
    if (fd < 0) {
        shell_print("touch: cannot create '"); shell_print(args[1]); shell_print("'\r\n"); return;
    }
    task_t *cur = task_get_current();
    if (cur && cur->fd_table[fd].ops && cur->fd_table[fd].ops->close)
        cur->fd_table[fd].ops->close(fd, cur->fd_table[fd].data);
    memset(&cur->fd_table[fd], 0, sizeof(file_t));
    shell_print("created '"); shell_print(args[1]); shell_print("'\r\n");
}

static void cmd_rm(int argc, char *args[]) {
    if (argc < 2) { shell_print("Usage: rm <path>\r\n"); return; }
    char full[EXT2_MAX_PATH];
    fs_abs_path(args[1], full, EXT2_MAX_PATH);
    if (vfs_unlink(full) != 0) {
        shell_print("rm: cannot remove '"); shell_print(args[1]); shell_print("'\r\n");
    } else {
        shell_print("removed '"); shell_print(args[1]); shell_print("'\r\n");
    }
}

static void cmd_mkdir(int argc, char *args[]) {
    if (argc < 2) { shell_print("Usage: mkdir <path>\r\n"); return; }
    char full[EXT2_MAX_PATH];
    fs_abs_path(args[1], full, EXT2_MAX_PATH);
    if (vfs_mkdir(full, 0755) != 0) {
        shell_print("mkdir: cannot create '"); shell_print(args[1]); shell_print("'\r\n");
    } else {
        shell_print("created '"); shell_print(args[1]); shell_print("'\r\n");
    }
}

static void cmd_rmdir_cmd(int argc, char *args[]) {
    if (argc < 2) { shell_print("Usage: rmdir <path>\r\n"); return; }
    char full[EXT2_MAX_PATH];
    fs_abs_path(args[1], full, EXT2_MAX_PATH);
    if (vfs_rmdir(full) != 0) {
        shell_print("rmdir: '"); shell_print(args[1]); shell_print("': not empty or not found\r\n");
    } else {
        shell_print("removed '"); shell_print(args[1]); shell_print("'\r\n");
    }
}

static void cmd_write_file(int argc, char *args[]) {
    if (argc < 3) { shell_print("Usage: write <path> <text>\r\n"); return; }
    char full[EXT2_MAX_PATH];
    fs_abs_path(args[1], full, EXT2_MAX_PATH);
    int fd = vfs_open(full, O_CREAT | O_WRONLY);
    if (fd < 0) { shell_print("write: cannot open '"); shell_print(args[1]); shell_print("'\r\n"); return; }
    task_t *cur = task_get_current();
    if (!cur || !cur->fd_table[fd].ops || !cur->fd_table[fd].ops->write) {
        shell_print("write: internal error\r\n"); return;
    }
    char text_buf[512]; int pos = 0;
    for (int i = 2; i < argc && pos < 510; i++) {
        if (i > 2) text_buf[pos++] = ' ';
        const char *s = args[i];
        while (*s && pos < 510) text_buf[pos++] = *s++;
    }
    text_buf[pos] = '\0';
    int64_t written = cur->fd_table[fd].ops->write(fd, text_buf, (uint64_t)pos, cur->fd_table[fd].data);
    if (cur->fd_table[fd].ops->close) cur->fd_table[fd].ops->close(fd, cur->fd_table[fd].data);
    memset(&cur->fd_table[fd], 0, sizeof(file_t));
    if (written < 0) {
        shell_print("write: failed\r\n");
    } else {
        char wbuf[16]; shell_itoa((int)written, wbuf);
        shell_print("wrote "); shell_print(wbuf); shell_print(" bytes to '");
        shell_print(args[1]); shell_print("'\r\n");
    }
}

static void cmd_mv(int argc, char *args[]) {
    if (argc < 3) { shell_print("Usage: mv <src> <dst>\r\n"); return; }
    char src[EXT2_MAX_PATH], dst[EXT2_MAX_PATH];
    fs_abs_path(args[1], src, EXT2_MAX_PATH);
    fs_abs_path(args[2], dst, EXT2_MAX_PATH);
    if (vfs_rename(src, dst) != 0) {
        shell_print("mv: cannot move '"); shell_print(args[1]);
        shell_print("' -> '"); shell_print(args[2]); shell_print("'\r\n");
    } else {
        shell_print("moved '"); shell_print(args[1]);
        shell_print("' -> '"); shell_print(args[2]); shell_print("'\r\n");
    }
}

static void cmd_cp(int argc, char *args[]) {
    if (argc < 3) { shell_print("Usage: cp <src> <dst>\r\n"); return; }
    char src[EXT2_MAX_PATH], dst[EXT2_MAX_PATH];
    fs_abs_path(args[1], src, EXT2_MAX_PATH);
    fs_abs_path(args[2], dst, EXT2_MAX_PATH);
    ext2_stat_t st;
    if (vfs_stat(src, &st) != 0) { shell_print("cp: source not found\r\n"); return; }
    if (EXT2_S_ISDIR(st.mode)) { shell_print("cp: source is a directory\r\n"); return; }
    uint32_t src_ino = ext2_resolve_path(src);
    if (src_ino == 0) { shell_print("cp: cannot resolve source\r\n"); return; }
    int dst_fd = vfs_open(dst, O_CREAT | O_WRONLY);
    if (dst_fd < 0) { shell_print("cp: cannot create destination\r\n"); return; }
    task_t *cur = task_get_current();
    if (!cur) { shell_print("cp: no current task\r\n"); return; }
    uint8_t buf[512]; uint64_t remaining = st.size, offset = 0; int ok = 1;
    while (remaining > 0) {
        uint64_t chunk = (remaining > sizeof(buf)) ? sizeof(buf) : remaining;
        int64_t n = ext2_read_file(src_ino, buf, offset, chunk);
        if (n <= 0) break;
        int64_t w = cur->fd_table[dst_fd].ops->write(dst_fd, buf, (uint64_t)n, cur->fd_table[dst_fd].data);
        if (w != n) { ok = 0; break; }
        offset += (uint64_t)n; remaining -= (uint64_t)n;
    }
    if (cur->fd_table[dst_fd].ops->close) cur->fd_table[dst_fd].ops->close(dst_fd, cur->fd_table[dst_fd].data);
    memset(&cur->fd_table[dst_fd], 0, sizeof(file_t));
    if (!ok) {
        shell_print("cp: write error\r\n");
    } else {
        char sbuf[16]; shell_fmt_bytes(offset, sbuf);
        shell_print("copied '"); shell_print(args[1]);
        shell_print("' -> '"); shell_print(args[2]);
        shell_print("'  ("); shell_print(sbuf); shell_print(")\r\n");
    }
}

static void cmd_info(void) {
    ext2_fs_t *fs = ext2_get_fs();
    if (!fs || !fs->mounted) { shell_print("Filesystem not mounted.\r\n"); return; }
    char buf[32];

    shell_print("  Filesystem\r\n");
    shell_print("  \xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\r\n");
    shell_print("  Type    : ext2 rev 0\r\n");
    shell_print("  Volume  : "); shell_print(fs->sb.s_volume_name); shell_print("\r\n");
    shell_print("  BlkSize : "); shell_itoa((int)fs->block_size, buf); shell_print(buf); shell_print(" B\r\n");
    shell_print("  Groups  : "); shell_itoa((int)fs->num_groups, buf); shell_print(buf); shell_print("\r\n");
    shell_print("  Blocks  : "); shell_itoa((int)fs->sb.s_blocks_count, buf); shell_print(buf); shell_print(" total   ");
    shell_itoa((int)fs->sb.s_free_blocks_count, buf); shell_print(buf); shell_print(" free\r\n");
    shell_print("  Inodes  : "); shell_itoa((int)fs->sb.s_inodes_count, buf); shell_print(buf); shell_print(" total   ");
    shell_itoa((int)fs->sb.s_free_inodes_count, buf); shell_print(buf); shell_print(" free\r\n");

    uint32_t used_blk = fs->sb.s_blocks_count - fs->sb.s_free_blocks_count;
    uint64_t bar_used = (fs->sb.s_blocks_count > 0)
        ? ((uint64_t)used_blk * 40 / fs->sb.s_blocks_count) : 0;
    shell_print("  Usage   : [");
    for (uint64_t i = 0; i < 40; i++) {
        char c[2]; c[1] = '\0';
        c[0] = (i < bar_used) ? BLOCK_FULL : BLOCK_LITE;
        shell_print(c);
    }
    shell_print("]\r\n");
}

/* =========================================================================
 * Shell dispatch
 * ========================================================================= */

static void shell_execute(char *line) {
    while (*line == ' ') line++;
    if (*line == 0) return;

    char *args[SHELL_MAX_ARGS];
    int   argc = 0;
    shell_parse_args(line, args, &argc);
    if (argc == 0) return;

    vga_batch_start();
    if      (shell_strcmp(args[0], "help")   == 0) cmd_help(argc, args);
    else if (shell_strcmp(args[0], "ver")    == 0) cmd_ver();
    else if (shell_strcmp(args[0], "sysinfo")== 0) cmd_sysinfo();
    else if (shell_strcmp(args[0], "mem")    == 0) cmd_mem();
    else if (shell_strcmp(args[0], "tasks")  == 0) cmd_tasks();
    else if (shell_strcmp(args[0], "clear")  == 0) cmd_clear();
    else if (shell_strcmp(args[0], "banner") == 0) cmd_banner();
    else if (shell_strcmp(args[0], "echo")   == 0) cmd_echo(argc, args);
    else if (shell_strcmp(args[0], "uptime") == 0) cmd_uptime();
    else if (shell_strcmp(args[0], "pid")    == 0) cmd_pid();
    else if (shell_strcmp(args[0], "color")  == 0) cmd_color(argc, args);
    else if (shell_strcmp(args[0], "beep")   == 0) cmd_beep(argc, args);
    else if (shell_strcmp(args[0], "reboot") == 0) cmd_reboot();
    else if (shell_strcmp(args[0], "exit")   == 0) cmd_exit();
    else if (shell_strcmp(args[0], "ls")     == 0) cmd_ls(argc, args);
    else if (shell_strcmp(args[0], "cat")    == 0) cmd_cat(argc, args);
    else if (shell_strcmp(args[0], "touch")  == 0) cmd_touch(argc, args);
    else if (shell_strcmp(args[0], "rm")     == 0) cmd_rm(argc, args);
    else if (shell_strcmp(args[0], "mkdir")  == 0) cmd_mkdir(argc, args);
    else if (shell_strcmp(args[0], "rmdir")  == 0) cmd_rmdir_cmd(argc, args);
    else if (shell_strcmp(args[0], "write")  == 0) cmd_write_file(argc, args);
    else if (shell_strcmp(args[0], "mv")     == 0) cmd_mv(argc, args);
    else if (shell_strcmp(args[0], "cp")     == 0) cmd_cp(argc, args);
    else if (shell_strcmp(args[0], "info")   == 0) cmd_info();
    else {
        shell_print("Unknown: '");
        shell_print(args[0]);
        shell_print("'  \xB3  type 'help'\r\n");
    }
    vga_batch_end();
}

/* =========================================================================
 * Main shell loop
 * ========================================================================= */

void shell_main(void) {
    serial_write_string(&g_serial, "[SHELL] shell_main entered.\r\n");

    /* Enable interrupts. */
    asm volatile ("sti");
    serial_write_string(&g_serial, "[SHELL] Interrupts enabled.\r\n");

    /* Enable hardware cursor (scanlines 13-15). */
    vga_cursor_enable(13, 15);
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);

    /* ---- Animated boot splash ---- */
    shell_draw_splash_animated();

    serial_write_string(&g_serial, "[SHELL] Splash done, entering main loop.\r\n");

    /* ---- Quick post-init OBF drain ---- */
    {
        uint64_t start = timer_get_ticks();
        while (timer_get_ticks() - start < 100) {
            if (inb(0x64) & 0x01) (void)inb(0x60);
        }
    }

    while (1) {
        /* Prompt: "TsOs » " */
        shell_print("TsOs> ");
        g_prompt_col = vga_get_cursor_x();
        g_prompt_row = vga_get_cursor_y();
        g_line_len   = 0;
        g_line_pos   = 0;
        g_history_idx = -1;

        /* ---- Periodic diagnostic dump (every 500 prompts) ---- */
        static int g_diag_counter = 0;
        g_diag_counter++;
        if (g_diag_counter % 500 == 0) {
            uint32_t irq_cnt = ps2_get_irq_count();
            uint32_t obf_cnt = ps2_get_obf_count();
            serial_write_string(&g_serial, "[DIAG] irq=");
            char buf[12]; int i = 0; uint32_t v = irq_cnt;
            if (v == 0) { buf[i++] = '0'; }
            else { char tmp[12]; int j = 0; while (v > 0) { tmp[j++] = '0' + (v % 10); v /= 10; } while (j > 0) buf[i++] = tmp[--j]; }
            buf[i] = '\0'; serial_write_string(&g_serial, buf);
            serial_write_string(&g_serial, " obf=");
            v = obf_cnt; i = 0;
            if (v == 0) { buf[i++] = '0'; }
            else { char tmp[12]; int j = 0; while (v > 0) { tmp[j++] = '0' + (v % 10); v /= 10; } while (j > 0) buf[i++] = tmp[--j]; }
            buf[i] = '\0'; serial_write_string(&g_serial, buf);
            serial_write_string(&g_serial, "\r\n");
        }

        while (1) {
            uint8_t key = ps2_get_key();

            if (key != 0) {
                serial_write_string(&g_serial, "[SHELL] key=0x");
                char hx[] = "00";
                static const char hexc[] = "0123456789ABCDEF";
                hx[0] = hexc[(key >> 4) & 0xF];
                hx[1] = hexc[key & 0xF];
                serial_write_string(&g_serial, hx);
                serial_write_string(&g_serial, "\r\n");
            }

            /* Enter */
            if (key == '\r') {
                g_line_buf[g_line_len] = 0;
                shell_history_store(g_line_buf);
                shell_vga_write('\r'); shell_vga_write('\n');
                shell_execute(g_line_buf);
                break;
            }

            /* Backspace */
            if (key == 0x08) {
                if (g_line_pos > 0) {
                    int midline = (g_line_pos < g_line_len);
                    for (int i = g_line_pos; i < g_line_len; i++)
                        g_line_buf[i - 1] = g_line_buf[i];
                    g_line_len--; g_line_pos--;
                    g_line_buf[g_line_len] = 0;
                    if (midline) shell_redraw_input();
                    else         shell_vga_backspace();
                }
                continue;
            }

            /* Arrow keys */
            if (key == PS2_KEY_LEFT) {
                if (g_line_pos > 0) { g_line_pos--; shell_vga_cursor_left(); } continue;
            }
            if (key == PS2_KEY_RIGHT) {
                if (g_line_pos < g_line_len) { g_line_pos++; shell_vga_cursor_right(); } continue;
            }
            if (key == PS2_KEY_UP)   { shell_history_prev(); continue; }
            if (key == PS2_KEY_DOWN) { shell_history_next(); continue; }

            /* Home / End */
            if (key == PS2_KEY_HOME || key == CTRL_A) {
                g_line_pos = 0; shell_redraw_input(); continue;
            }
            if (key == PS2_KEY_END || key == CTRL_E) {
                g_line_pos = g_line_len; shell_redraw_input(); continue;
            }

            /* Delete */
            if (key == PS2_KEY_DELETE) {
                if (g_line_pos < g_line_len) {
                    for (int i = g_line_pos; i < g_line_len - 1; i++)
                        g_line_buf[i] = g_line_buf[i + 1];
                    g_line_len--; g_line_buf[g_line_len] = 0;
                    shell_redraw_input();
                }
                continue;
            }

            /* Ctrl+C */
            if (key == CTRL_C) {
                shell_print("^C");
                shell_vga_write('\r'); shell_vga_write('\n');
                g_line_buf[0] = '\0'; g_line_len = 0; g_line_pos = 0;
                break;
            }

            /* Ctrl+U */
            if (key == CTRL_U) {
                g_line_len = 0; g_line_pos = 0; g_line_buf[0] = '\0';
                shell_redraw_input(); continue;
            }

            /* Ctrl+K */
            if (key == CTRL_K) {
                if (g_line_pos < g_line_len) {
                    g_line_len = g_line_pos; g_line_buf[g_line_len] = '\0';
                    shell_redraw_input();
                }
                continue;
            }

            /* Ctrl+W */
            if (key == CTRL_W) {
                if (g_line_pos > 0) {
                    int end = g_line_pos;
                    while (g_line_pos > 0 && g_line_buf[g_line_pos - 1] == ' ') g_line_pos--;
                    while (g_line_pos > 0 && g_line_buf[g_line_pos - 1] != ' ') g_line_pos--;
                    int removed = end - g_line_pos;
                    for (int i = g_line_pos; i + removed <= g_line_len; i++)
                        g_line_buf[i] = g_line_buf[i + removed];
                    g_line_len -= removed; g_line_buf[g_line_len] = '\0';
                    shell_redraw_input();
                }
                continue;
            }

            /* Ctrl+L — clear and redraw banner */
            if (key == CTRL_L) {
                shell_draw_banner();
                shell_print("TsOs> ");
                g_prompt_col = vga_get_cursor_x();
                g_prompt_row = vga_get_cursor_y();
                if (g_line_len > 0) shell_redraw_input();
                continue;
            }

            /* Esc */
            if (key == PS2_KEY_ESC) {
                shell_vga_write('\r'); shell_vga_write('\n');
                g_line_buf[0] = '\0'; g_line_len = 0; g_line_pos = 0;
                break;
            }

            /* Tab — 4 spaces */
            if (key == '\t') {
                for (int s = 0; s < 4 && g_line_len < SHELL_BUF_SIZE - 1; s++) {
                    int midline = (g_line_pos < g_line_len);
                    for (int i = g_line_len; i > g_line_pos; i--) g_line_buf[i] = g_line_buf[i - 1];
                    g_line_buf[g_line_pos] = ' ';
                    g_line_len++; g_line_pos++; g_line_buf[g_line_len] = 0;
                    if (midline) shell_redraw_input();
                    else         shell_vga_insert_char(' ');
                }
                continue;
            }

            if (key == 0) continue;

            /* Printable character */
            if (g_line_len < SHELL_BUF_SIZE - 1) {
                int midline = (g_line_pos < g_line_len);
                for (int i = g_line_len; i > g_line_pos; i--) g_line_buf[i] = g_line_buf[i - 1];
                g_line_buf[g_line_pos] = (char)key;
                g_line_len++; g_line_pos++; g_line_buf[g_line_len] = 0;
                if (midline) shell_redraw_input();
                else         shell_vga_insert_char((char)key);
            }
        }
    }
}
