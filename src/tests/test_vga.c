/**
 * @file test_vga.c
 * @brief VGA text-mode driver tests — init, put_char, color, scroll, cursor.
 *
 * Tests registered (10):
 *   vga_init_no_crash        — vga_init() completes without fault
 *   vga_put_char_in_bounds   — writing within 80x25 does not crash
 *   vga_put_char_out_bounds  — writing outside bounds is silently ignored
 *   vga_put_str_basic        — string rendering with newline handling
 *   vga_make_color_correct   — attribute byte encoding is correct
 *   vga_set_get_color        — set/get color round-trips
 *   vga_clear_no_crash       — vga_clear() completes without fault
 *   vga_scroll_no_crash      — vga_scroll(1) completes without fault
 *   vga_cursor_set_no_crash  — vga_cursor_set() completes without fault
 *   vga_flush_no_crash       — vga_flush() copies shadow to VGA memory
 */

#include "test.h"
#include "../drivers/vga.h"
#include <stdint.h>

static void test_vga_init_no_crash(serial_dev_t *dev) {
    /* vga_init() was called before tests. Just verify we can call
     * vga_clear (which touches the shadow buffer and flushes). */
    vga_clear();
    ASSERT_TRUE(dev, true);
}

static void test_vga_put_char_in_bounds(serial_dev_t *dev) {
    /* Writing to (0,0) should not crash. */
    vga_put_char(0, 0, 'A', vga_make_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK));
    vga_put_char(79, 24, 'Z', vga_make_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK));
    /* Writing to center. */
    vga_put_char(40, 12, 'X', vga_make_color(VGA_COLOR_GREEN, VGA_COLOR_BLACK));
    vga_flush();
    ASSERT_TRUE(dev, true);
}

static void test_vga_put_char_out_bounds(serial_dev_t *dev) {
    /* Out-of-bounds writes should be silently ignored (no crash). */
    vga_put_char(-1, 0, 'A', 0x07);
    vga_put_char(0, -1, 'A', 0x07);
    vga_put_char(80, 0, 'A', 0x07);
    vga_put_char(0, 25, 'A', 0x07);
    vga_put_char(999, 999, 'A', 0x07);
    ASSERT_TRUE(dev, true);
}

static void test_vga_put_str_basic(serial_dev_t *dev) {
    /* Simple string. */
    vga_put_str(0, 0, "Hello TsOs", vga_make_color(VGA_COLOR_CYAN, VGA_COLOR_BLACK));
    /* Newline handling. */
    vga_put_str(0, 1, "Line1\nLine2", vga_make_color(VGA_COLOR_CYAN, VGA_COLOR_BLACK));
    /* Carriage return. */
    vga_put_str(0, 2, "A\rB", vga_make_color(VGA_COLOR_CYAN, VGA_COLOR_BLACK));
    /* NULL string should not crash. */
    vga_put_str(0, 3, (const char *)0, 0x07);
    /* Empty string. */
    vga_put_str(0, 3, "", 0x07);
    vga_flush();
    ASSERT_TRUE(dev, true);
}

static void test_vga_make_color_correct(serial_dev_t *dev) {
    /* fg=15 (white), bg=0 (black) → 0x0F. */
    uint8_t c = vga_make_color(15, 0);
    ASSERT_EQ(dev, (int)c, 0x0F);

    /* fg=4 (red), bg=7 (light gray) → 0x74. */
    c = vga_make_color(4, 7);
    ASSERT_EQ(dev, (int)c, 0x74);

    /* fg=0, bg=0 → 0x00. */
    c = vga_make_color(0, 0);
    ASSERT_EQ(dev, (int)c, 0x00);

    /* fg=15, bg=15 → 0xFF. */
    c = vga_make_color(15, 15);
    ASSERT_EQ(dev, (int)c, (int)0xFF);
}

static void test_vga_set_get_color(serial_dev_t *dev) {
    vga_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLUE);
    uint8_t c = vga_get_color();
    /* fg=12 (light red), bg=1 (blue) → (1 << 4) | 12 = 0x1C. */
    ASSERT_EQ(dev, (int)c, 0x1C);

    /* Restore defaults. */
    vga_set_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
    c = vga_get_color();
    /* fg=11 (light cyan), bg=0 (black) → (0 << 4) | 11 = 0x0B. */
    ASSERT_EQ(dev, (int)c, 0x0B);
}

static void test_vga_clear_no_crash(serial_dev_t *dev) {
    /* Fill some cells first. */
    for (int x = 0; x < VGA_WIDTH; x++) {
        vga_put_char(x, 0, 'X', 0x07);
    }
    vga_flush();
    /* Clear should reset everything. */
    vga_clear();
    ASSERT_TRUE(dev, true);
}

static void test_vga_scroll_no_crash(serial_dev_t *dev) {
    vga_clear();
    /* Put some text on multiple lines. */
    vga_put_str(0, 0, "Line 0", 0x07);
    vga_put_str(0, 1, "Line 1", 0x07);
    vga_put_str(0, 2, "Line 2", 0x07);
    vga_flush();
    /* Scroll up by 1 line. */
    vga_scroll(1);
    vga_flush();
    /* Scroll by large amount (should clear). */
    vga_scroll(VGA_HEIGHT);
    vga_flush();
    ASSERT_TRUE(dev, true);
}

static void test_vga_cursor_set_no_crash(serial_dev_t *dev) {
    /* Cursor operations should not crash. */
    vga_cursor_enable(13, 15);
    vga_cursor_set(0, 0);
    vga_cursor_set(79, 24);
    vga_cursor_set(40, 12);
    /* Out-of-bounds should be clamped. */
    vga_cursor_set(-1, -1);
    vga_cursor_set(999, 999);
    /* Disable cursor. */
    vga_cursor_enable(0, 0);
    /* Restore visible cursor at origin. */
    vga_cursor_enable(13, 15);
    vga_cursor_set(0, 0);
    ASSERT_TRUE(dev, true);
}

static void test_vga_flush_no_crash(serial_dev_t *dev) {
    /* Fill shadow buffer with a pattern. */
    for (int i = 0; i < VGA_WIDTH * VGA_HEIGHT; i++) {
        vga_put_char(i % VGA_WIDTH, i / VGA_WIDTH,
                     'A' + (i % 26), 0x07);
    }
    /* Flush multiple times (should be idempotent). */
    vga_flush();
    vga_flush();
    vga_clear();
    ASSERT_TRUE(dev, true);
}

static void test_vga_read_char(serial_dev_t *dev) {
    vga_clear();
    vga_put_char(5, 3, 'R', vga_make_color(VGA_COLOR_RED, VGA_COLOR_BLACK));
    char c = vga_read_char(5, 3);
    ASSERT_EQ(dev, (int)c, (int)'R');
    char c2 = vga_read_char(0, 0);
    ASSERT_EQ(dev, (int)c2, (int)' ');
    char c3 = vga_read_char(-1, -1);
    ASSERT_EQ(dev, (int)c3, 0);
    vga_clear();
}

static void test_vga_tab_handling(serial_dev_t *dev) {
    vga_clear();
    vga_put_str(0, 0, "A\tB", vga_make_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK));
    char c0 = vga_read_char(0, 0);
    ASSERT_EQ(dev, (int)c0, (int)'A');
    char c8 = vga_read_char(8, 0);
    ASSERT_EQ(dev, (int)c8, (int)'B');
    vga_clear();
}

static void test_vga_fill_rect(serial_dev_t *dev) {
    vga_clear();
    vga_fill_rect(10, 5, 3, 2, 'X', vga_make_color(VGA_COLOR_GREEN, VGA_COLOR_BLACK));
    ASSERT_EQ(dev, (int)vga_read_char(10, 5), (int)'X');
    ASSERT_EQ(dev, (int)vga_read_char(12, 5), (int)'X');
    ASSERT_EQ(dev, (int)vga_read_char(10, 6), (int)'X');
    ASSERT_EQ(dev, (int)vga_read_char(13, 5), (int)' ');
    vga_clear();
}

static void test_vga_draw_hline(serial_dev_t *dev) {
    vga_clear();
    vga_draw_hline(5, 10, 4, vga_make_color(VGA_COLOR_CYAN, VGA_COLOR_BLACK));
    ASSERT_EQ(dev, (int)vga_read_char(5, 10), (int)' ');
    ASSERT_EQ(dev, (int)vga_read_char(8, 10), (int)' ');
    ASSERT_EQ(dev, (int)vga_read_char(9, 10), (int)' ');
    vga_clear();
}

static void test_vga_put_str_centered(serial_dev_t *dev) {
    vga_clear();
    vga_put_str_centered(12, "Hi", vga_make_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK));
    int y = vga_get_cursor_y();
    ASSERT_EQ(dev, y, 12);
    vga_clear();
}

static void test_vga_cursor_xy(serial_dev_t *dev) {
    vga_cursor_set(15, 10);
    ASSERT_EQ(dev, vga_get_cursor_x(), 15);
    ASSERT_EQ(dev, vga_get_cursor_y(), 10);
    vga_cursor_set(0, 0);
}

static void test_vga_blink_no_crash(serial_dev_t *dev) {
    vga_cursor_blink_enable();
    vga_cursor_blink_disable();
    vga_cursor_enable(13, 15);
    vga_cursor_set(0, 0);
    ASSERT_TRUE(dev, true);
}

static void test_vga_scanline_no_crash(serial_dev_t *dev) {
    vga_scanline_enable();
    vga_scanline_disable();
    vga_clear();
    ASSERT_TRUE(dev, true);
}

/* ---- Registration ---- */
void test_register_vga(void) {
    test_register("vga_init_no_crash",        test_vga_init_no_crash);
    test_register("vga_put_char_in_bounds",   test_vga_put_char_in_bounds);
    test_register("vga_put_char_out_bounds",  test_vga_put_char_out_bounds);
    test_register("vga_put_str_basic",        test_vga_put_str_basic);
    test_register("vga_make_color_correct",   test_vga_make_color_correct);
    test_register("vga_set_get_color",        test_vga_set_get_color);
    test_register("vga_clear_no_crash",       test_vga_clear_no_crash);
    test_register("vga_scroll_no_crash",      test_vga_scroll_no_crash);
    test_register("vga_cursor_set_no_crash",  test_vga_cursor_set_no_crash);
    test_register("vga_flush_no_crash",       test_vga_flush_no_crash);
    test_register("vga_read_char",            test_vga_read_char);
    test_register("vga_tab_handling",         test_vga_tab_handling);
    test_register("vga_fill_rect",            test_vga_fill_rect);
    test_register("vga_draw_hline",           test_vga_draw_hline);
    test_register("vga_put_str_centered",     test_vga_put_str_centered);
    test_register("vga_cursor_xy",            test_vga_cursor_xy);
    test_register("vga_blink_no_crash",       test_vga_blink_no_crash);
    test_register("vga_scanline_no_crash",    test_vga_scanline_no_crash);
}
