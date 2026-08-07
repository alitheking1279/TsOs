/**
 * @file vga.c
 * @brief VGA text-mode driver implementation (80x25, 16 colors).
 *
 * Writes directly to VGA text-mode memory at physical 0xB8000.
 * Uses a shadow buffer for batch updates and flicker-free rendering.
 *
 * VGA text memory layout:
 *   Each cell = 2 bytes: [character, attribute]
 *   Attribute = (bg << 4) | fg
 *   Total: 80 * 25 * 2 = 4000 bytes
 *
 * Higher-half memory mapping:
 *   After vmm_init() removes the identity map, physical 0xB8000 is
 *   accessed via the higher-half direct mapping:
 *     kernel_virtual = physical + 0xFFFF800000000000
 *
 * References:
 *   IBM VGA Technical Reference
 *   OSDev wiki — VGA Text Mode
 */

#include "vga.h"
#include "portio.h"
#include "../kernel/vmm.h"
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#define VGA_DIRECT_OFFSET    0xFFFF800000000000ULL
#define VGA_DEFAULT_FG      VGA_COLOR_WHITE
#define VGA_DEFAULT_BG      VGA_COLOR_BLACK
#define VGA_CRT_CURSOR_HIGH 0x0E
#define VGA_CRT_CURSOR_LOW  0x0F

static uint16_t g_shadow[VGA_WIDTH * VGA_HEIGHT];
static uint8_t g_dirty[VGA_WIDTH * VGA_HEIGHT];   /* dirty-cell bitmap */
static uint8_t g_fg = VGA_DEFAULT_FG;
static uint8_t g_bg = VGA_DEFAULT_BG;
static volatile uint16_t *g_vga_mem = NULL;

static int g_cursor_x = 0;
static int g_cursor_y = 0;
static bool g_blink_enabled = false;
static bool g_blink_visible = true;
static bool g_scanline_enabled = false;
static bool g_batch_mode = false;

static inline uint16_t vga_entry(char c, uint8_t color) {
    return ((uint16_t)color << 8) | (uint8_t)c;
}

static inline void update_cursor(int x, int y) {
    uint16_t pos = (uint16_t)(y * VGA_WIDTH + x);
    outb(VGA_CRT_ADDR_PORT, VGA_CRT_CURSOR_HIGH);
    outb(VGA_CRT_DATA_PORT, (uint8_t)(pos >> 8));
    outb(VGA_CRT_ADDR_PORT, VGA_CRT_CURSOR_LOW);
    outb(VGA_CRT_DATA_PORT, (uint8_t)(pos & 0xFF));
}

void vga_init(void) {
    address_space_t *kern_as = vmm_get_kernel_address_space();
    vmm_map_page(kern_as,
                 VGA_DIRECT_OFFSET + VGA_TEXT_MEMORY,
                 VGA_TEXT_MEMORY,
                 VMM_FLAG_WRITE | VMM_FLAG_NOEXEC | VMM_FLAG_CACHE_DIS);
    g_vga_mem = (volatile uint16_t *)(VGA_DIRECT_OFFSET + VGA_TEXT_MEMORY);
    for (int i = 0; i < VGA_WIDTH * VGA_HEIGHT; i++) g_dirty[i] = 0;
    vga_clear();
}

void vga_clear(void) {
    uint16_t blank = vga_entry(' ', vga_make_color(g_fg, g_bg));
    for (int i = 0; i < VGA_WIDTH * VGA_HEIGHT; i++) {
        g_shadow[i] = blank;
        g_dirty[i] = 1;   /* mark all dirty so next flush writes everything */
    }
    g_cursor_x = 0;
    g_cursor_y = 0;
    vga_flush();
    update_cursor(0, 0);
}

char vga_put_char(int x, int y, char c, uint8_t color) {
    if (x < 0 || x >= VGA_WIDTH || y < 0 || y >= VGA_HEIGHT) return 0;
    int idx = y * VGA_WIDTH + x;
    uint16_t entry = vga_entry(c, color);
    uint16_t old = g_shadow[idx];
    if (g_shadow[idx] != entry) {
        g_shadow[idx] = entry;
        g_dirty[idx] = 1;   /* mark cell dirty for next flush */
    }
    return (char)(old & 0xFF);
}

void vga_put_str(int x, int y, const char *s, uint8_t color) {
    if (!s) return;
    int cx = x;
    int cy = y;
    while (*s) {
        if (cx >= VGA_WIDTH) { cx = 0; cy++; }
        if (cy >= VGA_HEIGHT) {
            vga_scroll(1);
            cy = VGA_HEIGHT - 1;
        }
        if (*s == '\n') { cx = 0; cy++; s++; continue; }
        if (*s == '\r') { cx = 0; s++; continue; }
        if (*s == '\t') { cx = (cx + 8) & ~7; s++; continue; }
        int idx = cy * VGA_WIDTH + cx;
        uint16_t entry = vga_entry(*s, color);
        if (g_shadow[idx] != entry) {
            g_shadow[idx] = entry;
            g_dirty[idx] = 1;
        }
        cx++;
        s++;
    }
    g_cursor_x = cx;
    g_cursor_y = cy;
    update_cursor(cx, cy);   /* sync hardware cursor — was missing, caused displaced cursor */
    vga_flush();
}

void vga_set_color(uint8_t fg, uint8_t bg) {
    g_fg = fg;
    g_bg = bg;
}

uint8_t vga_get_color(void) {
    return vga_make_color(g_fg, g_bg);
}

void vga_scroll(int lines) {
    if (lines <= 0) return;
    if (lines >= VGA_HEIGHT) {
        vga_clear();
        return;
    }

    int kept = (VGA_HEIGHT - lines) * VGA_WIDTH;
    memmove(g_shadow, &g_shadow[lines * VGA_WIDTH], kept * sizeof(uint16_t));
    memmove(g_dirty,  &g_dirty[lines * VGA_WIDTH],  kept);

    uint16_t blank = vga_entry(' ', vga_make_color(g_fg, g_bg));
    for (int i = kept; i < VGA_WIDTH * VGA_HEIGHT; i++) {
        g_shadow[i] = blank;
        g_dirty[i] = 1;
    }
}

void vga_cursor_enable(uint8_t start, uint8_t end) {
    outb(VGA_CRT_ADDR_PORT, 0x0A);
    uint8_t cur = inb(VGA_CRT_DATA_PORT);
    outb(VGA_CRT_DATA_PORT, (cur & 0xC0) | start);

    outb(VGA_CRT_ADDR_PORT, 0x0B);
    cur = inb(VGA_CRT_DATA_PORT);
    outb(VGA_CRT_DATA_PORT, (cur & 0xE0) | end);
}

void vga_cursor_set(int x, int y) {
    if (x < 0) x = 0;
    if (x >= VGA_WIDTH) x = VGA_WIDTH - 1;
    if (y < 0) y = 0;
    if (y >= VGA_HEIGHT) y = VGA_HEIGHT - 1;
    g_cursor_x = x;
    g_cursor_y = y;
    update_cursor(x, y);
}

void vga_flush(void) {
    if (!g_vga_mem || g_batch_mode) return;
    /* Dirty-cell optimized flush: only write cells that changed.
     * Falls back to full memcpy when >75% of cells are dirty (bulk redraws). */
    int dirty_count = 0;
    for (int i = 0; i < VGA_WIDTH * VGA_HEIGHT; i++) dirty_count += g_dirty[i];

    if (dirty_count >= (VGA_WIDTH * VGA_HEIGHT * 3 / 4)) {
        /* Bulk: single memcpy is faster than scattered writes when mostly dirty */
        for (int i = 0; i < VGA_WIDTH * VGA_HEIGHT; i++) g_vga_mem[i] = g_shadow[i];
    } else {
        /* Sparse: only write dirty cells */
        for (int i = 0; i < VGA_WIDTH * VGA_HEIGHT; i++) {
            if (g_dirty[i]) g_vga_mem[i] = g_shadow[i];
        }
    }
    /* Clear dirty bitmap after flush */
    for (int i = 0; i < VGA_WIDTH * VGA_HEIGHT; i++) g_dirty[i] = 0;
}

void vga_flush_cell(int x, int y) {
    if (!g_vga_mem || g_batch_mode) return;
    if (x < 0 || x >= VGA_WIDTH || y < 0 || y >= VGA_HEIGHT) return;
    int idx = y * VGA_WIDTH + x;
    g_vga_mem[idx] = g_shadow[idx];
    g_dirty[idx] = 0;
}

void vga_batch_start(void) {
    g_batch_mode = true;
}

void vga_batch_end(void) {
    g_batch_mode = false;
    if (g_vga_mem) {
        /* After batch: bulk-write all cells (full redraw context) */
        for (int i = 0; i < VGA_WIDTH * VGA_HEIGHT; i++) {
            g_vga_mem[i] = g_shadow[i];
            g_dirty[i] = 0;
        }
    }
}

uint8_t vga_make_color(uint8_t fg, uint8_t bg) {
    return (bg << 4) | fg;
}

char vga_read_char(int x, int y) {
    if (x < 0 || x >= VGA_WIDTH || y < 0 || y >= VGA_HEIGHT) return 0;
    return (char)(g_shadow[y * VGA_WIDTH + x] & 0xFF);
}

void vga_fill_rect(int x, int y, int w, int h, char c, uint8_t color) {
    for (int row = y; row < y + h; row++) {
        for (int col = x; col < x + w; col++) {
            vga_put_char(col, row, c, color);
        }
    }
}

void vga_draw_hline(int x, int y, int len, uint8_t color) {
    for (int i = 0; i < len; i++) {
        vga_put_char(x + i, y, ' ', color);
    }
}

void vga_put_str_centered(int y, const char *s, uint8_t color) {
    if (!s) return;
    int len = 0;
    while (s[len]) len++;
    int x = (VGA_WIDTH - len) / 2;
    if (x < 0) x = 0;
    vga_put_str(x, y, s, color);
}

int vga_get_cursor_x(void) { return g_cursor_x; }
int vga_get_cursor_y(void) { return g_cursor_y; }

void vga_cursor_blink_enable(void) {
    g_blink_enabled = true;
    g_blink_visible = true;
    vga_cursor_enable(13, 15);
}

void vga_cursor_blink_disable(void) {
    g_blink_enabled = false;
    g_blink_visible = false;
    vga_cursor_enable(0, 0);
}

void vga_scanline_enable(void)  { g_scanline_enabled = true; }
void vga_scanline_disable(void) { g_scanline_enabled = false; }

void vga_tick(void) {
    static uint32_t tick_count = 0;
    tick_count++;

    /* Cursor blink: toggle every 50 ticks (500ms at 100Hz). */
    if (g_blink_enabled && (tick_count % 50) == 0) {
        g_blink_visible = !g_blink_visible;
        if (g_blink_visible)
            vga_cursor_enable(13, 15);
        else
            vga_cursor_enable(0, 0);
    }

    /* Scanline overlay: every 3rd tick, toggle bg on even rows. */
    if (g_scanline_enabled && (tick_count % 3) == 0) {
        for (int y = 0; y < VGA_HEIGHT; y += 2) {
            for (int x = 0; x < VGA_WIDTH; x++) {
                int idx = y * VGA_WIDTH + x;
                uint16_t cell = g_shadow[idx];
                uint8_t bg = (cell >> 12) & 0x0F;
                if (bg == VGA_COLOR_BLACK) {
                    g_shadow[idx] = (cell & 0x0FFF) | (VGA_COLOR_DARK_GRAY << 12);
                } else if (bg == VGA_COLOR_DARK_GRAY) {
                    g_shadow[idx] = (cell & 0x0FFF) | (VGA_COLOR_BLACK << 12);
                }
            }
        }
        vga_flush();
    }
}
