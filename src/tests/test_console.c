#include "../tests/test.h"
#include "../drivers/vga.h"
#include <stdint.h>

static void test_console_vga_put_char_advances(serial_dev_t *dev) {
    vga_init();
    int x0 = vga_get_cursor_x();
    int y0 = vga_get_cursor_y();
    vga_put_char(0, 0, 'A', vga_get_color());
    vga_cursor_set(x0 + 1, y0);
    int x1 = vga_get_cursor_x();
    ASSERT_EQ(dev, x1, x0 + 1);
}

static void test_console_vga_clear_resets_cursor(serial_dev_t *dev) {
    vga_init();
    vga_cursor_set(10, 5);
    vga_clear();
    ASSERT_EQ(dev, vga_get_cursor_x(), 0);
    ASSERT_EQ(dev, vga_get_cursor_y(), 0);
}

static void test_console_vga_color_roundtrip(serial_dev_t *dev) {
    vga_init();
    vga_set_color(VGA_COLOR_RED, VGA_COLOR_BLUE);
    uint8_t c = vga_get_color();
    uint8_t expected = vga_make_color(VGA_COLOR_RED, VGA_COLOR_BLUE);
    ASSERT_EQ(dev, c, expected);
}

static void test_console_vga_scroll_no_crash(serial_dev_t *dev) {
    vga_init();
    vga_scroll(1);
    ASSERT_TRUE(dev, 1);
}

static void test_console_vga_put_str_advances(serial_dev_t *dev) {
    vga_init();
    vga_put_str(0, 0, "Hello", vga_get_color());
    ASSERT_EQ(dev, vga_get_cursor_x(), 5);
    ASSERT_EQ(dev, vga_get_cursor_y(), 0);
}

static void test_console_vga_cursor_set_updates_internal(serial_dev_t *dev) {
    vga_init();
    vga_cursor_set(42, 13);
    ASSERT_EQ(dev, vga_get_cursor_x(), 42);
    ASSERT_EQ(dev, vga_get_cursor_y(), 13);
}

void test_register_console(void) {
    test_register("console_vga_put_char",        test_console_vga_put_char_advances);
    test_register("console_vga_clear_resets",    test_console_vga_clear_resets_cursor);
    test_register("console_vga_color_roundtrip", test_console_vga_color_roundtrip);
    test_register("console_vga_scroll",          test_console_vga_scroll_no_crash);
    test_register("console_vga_put_str",         test_console_vga_put_str_advances);
    test_register("console_vga_cursor_set",      test_console_vga_cursor_set_updates_internal);
}
