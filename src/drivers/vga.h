/**
 * @file vga.h
 * @brief VGA text-mode driver interface (80x25, 16 colors).
 *
 * Writes directly to VGA text-mode memory at physical address 0xB8000.
 * Each character cell is 2 bytes: character byte + attribute byte.
 *
 * Attribute byte layout (Intel VGA spec):
 *   bits 7-4: Background color (0-15)
 *   bits 3-0: Foreground color (0-15)
 *
 * Color values:
 *   0 = Black       8 = Dark Gray
 *   1 = Blue        9 = Light Blue
 *   2 = Green       A = Light Green
 *   3 = Cyan        B = Light Cyan
 *   4 = Red         C = Light Red
 *   5 = Magenta     D = Light Magenta
 *   6 = Brown       E = Yellow
 *   7 = Light Gray  F = White
 *
 * Cursor is controlled via VGA registers at ports 0x3D4/0x3D5.
 *
 * References:
 *   IBM VGA Technical Reference
 *   OSDev wiki — VGA Text Mode
 */

#ifndef DRIVERS_VGA_H
#define DRIVERS_VGA_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* =========================================================================
 * Constants
 * ========================================================================= */

/** VGA text-mode memory physical address. */
#define VGA_TEXT_MEMORY      0xB8000

/** Screen dimensions. */
#define VGA_WIDTH           80
#define VGA_HEIGHT          25

/** VGA CRT controller address register port. */
#define VGA_CRT_ADDR_PORT   0x3D4

/** VGA CRT controller data register port. */
#define VGA_CRT_DATA_PORT   0x3D5

/* =========================================================================
 * Color Constants (foreground only — combine with vga_make_color)
 * ========================================================================= */

#define VGA_COLOR_BLACK          0
#define VGA_COLOR_BLUE           1
#define VGA_COLOR_GREEN          2
#define VGA_COLOR_CYAN           3
#define VGA_COLOR_RED            4
#define VGA_COLOR_MAGENTA        5
#define VGA_COLOR_BROWN          6
#define VGA_COLOR_LIGHT_GRAY     7
#define VGA_COLOR_DARK_GRAY      8
#define VGA_COLOR_LIGHT_BLUE     9
#define VGA_COLOR_LIGHT_GREEN    10
#define VGA_COLOR_LIGHT_CYAN     11
#define VGA_COLOR_LIGHT_RED      12
#define VGA_COLOR_LIGHT_MAGENTA  13
#define VGA_COLOR_YELLOW         14
#define VGA_COLOR_WHITE          15

/* =========================================================================
 * Public API
 * ========================================================================= */

/**
 * @brief Initialize the VGA text-mode driver.
 *
 * Maps VGA text memory into the kernel address space via the higher-half
 * direct mapping, clears the screen, and positions the cursor at (0,0).
 *
 * VGA text memory is at physical 0xB8000. After vmm_init() removes the
 * identity map, this address is accessed through the higher-half direct
 * mapping: kernel_virtual = physical + 0xFFFF800000000000.
 *
 * @note Must be called after vmm_init().
 */
void vga_init(void);

/**
 * @brief Clear the entire screen and reset cursor to (0,0).
 */
void vga_clear(void);

/**
 * @brief Write a single character at the given cell position.
 *
 * @param x      Column (0-79).
 * @param y      Row (0-24).
 * @param c      Character to display.
 * @param color  Attribute byte: (bg << 4) | fg.
 * @return The character that was previously in the cell.
 */
char vga_put_char(int x, int y, char c, uint8_t color);

/**
 * @brief Write a NUL-terminated string starting at the given position.
 *
 * Characters beyond the right edge wrap to the next line.
 * Characters beyond the bottom row are clipped.
 *
 * @param x      Starting column.
 * @param y      Starting row.
 * @param s      String to display.
 * @param color  Attribute byte.
 */
void vga_put_str(int x, int y, const char *s, uint8_t color);

/**
 * @brief Set the current default foreground and background colors.
 *
 * @param fg  Foreground color (0-15).
 * @param bg  Background color (0-15).
 */
void vga_set_color(uint8_t fg, uint8_t bg);

/**
 * @brief Get the current default color attribute.
 *
 * @return Current attribute byte: (bg << 4) | fg.
 */
uint8_t vga_get_color(void);

/**
 * @brief Scroll the screen up by the given number of lines.
 *
 * Moves existing content up and fills new bottom lines with blanks.
 *
 * @param lines  Number of lines to scroll (1-25).
 */
void vga_scroll(int lines);

/**
 * @brief Enable or disable the hardware cursor.
 *
 * @param start  Cursor scanline start (0-15). Pass 0 to disable.
 * @param end    Cursor scanline end (0-15). Pass 0 to disable.
 */
void vga_cursor_enable(uint8_t start, uint8_t end);

/**
 * @brief Set the hardware cursor position.
 *
 * @param x  Column (0-79).
 * @param y  Row (0-24).
 */
void vga_cursor_set(int x, int y);

/**
 * @brief Flush the shadow buffer to VGA memory.
 *
 * Copies the entire shadow buffer to physical VGA text memory.
 * Call after batch-updating cells to avoid flicker.
 */
void vga_flush(void);

/**
 * @brief Flush a single cell from shadow to VGA memory.
 *
 * Writes only the 2-byte cell at (x, y) to VGA memory.
 * Use this for interactive updates (typing, backspace) instead
 * of vga_flush() which copies all 4000 bytes.
 *
 * @param x  Column (0-79).
 * @param y  Row (0-24).
 */
void vga_flush_cell(int x, int y);

/**
 * @brief Build a VGA color attribute byte.
 *
 * @param fg  Foreground color (0-15).
 * @param bg  Background color (0-15).
 * @return Attribute byte: (bg << 4) | fg.
 */
uint8_t vga_make_color(uint8_t fg, uint8_t bg);

/**
 * @brief Read the character at a given cell position.
 *
 * @param x  Column (0-79).
 * @param y  Row (0-24).
 * @return Character at that cell, or 0 if out of bounds.
 */
char vga_read_char(int x, int y);

/**
 * @brief Fill a rectangular region with a character and color.
 */
void vga_fill_rect(int x, int y, int w, int h, char c, uint8_t color);

/**
 * @brief Draw a horizontal line of spaces.
 */
void vga_draw_hline(int x, int y, int len, uint8_t color);

/**
 * @brief Write a string centered horizontally on the given row.
 */
void vga_put_str_centered(int y, const char *s, uint8_t color);

/**
 * @brief Get current cursor X position.
 */
int vga_get_cursor_x(void);

/**
 * @brief Get current cursor Y position.
 */
int vga_get_cursor_y(void);

/**
 * @brief Enable blinking block cursor via timer callback.
 */
void vga_cursor_blink_enable(void);

/**
 * @brief Disable blinking cursor.
 */
void vga_cursor_blink_disable(void);

/**
 * @brief Enable CRT scanline overlay effect.
 */
void vga_scanline_enable(void);

/**
 * @brief Disable CRT scanline overlay effect.
 */
void vga_scanline_disable(void);

/**
 * @brief Timer tick handler for cursor blink and scanline effects.
 *        Called from timer_irq_handler.
 */
void vga_tick(void);

/**
 * @brief Enter deferred-flush (batch) mode.
 *
 * While batch mode is active, vga_flush() and vga_flush_cell() are
 * no-ops — all writes go to the shadow buffer only. This lets callers
 * accumulate many cell updates and flush them in one 4000-byte memcpy.
 */
void vga_batch_start(void);

/**
 * @brief Exit deferred-flush mode and flush the entire shadow buffer.
 *
 * If batch mode was not active, this is a no-op.
 */
void vga_batch_end(void);

#ifdef __cplusplus
}
#endif

#endif /* DRIVERS_VGA_H */
