#include "lib/print.h"

/* Unified print helpers — eliminates duplicate hex/decimal formatters
   across pmm.c, kheap.c, vmm.c, and main.c.  All output is serial-gated. */

static void write_hex_char(serial_dev_t *dev, char c)
{
    serial_write_char(dev, c);
}

void print_str(serial_dev_t *dev, const char *s)
{
    if (dev && s) serial_write_string(dev, s);
}

void print_hex8(serial_dev_t *dev, uint8_t value)
{
    static const char hex[] = "0123456789ABCDEF";
    if (!dev) return;
    serial_write_char(dev, hex[(value >> 4) & 0xF]);
    serial_write_char(dev, hex[value & 0xF]);
}

void print_hex32(serial_dev_t *dev, uint32_t value)
{
    static const char hex[] = "0123456789ABCDEF";
    if (!dev) return;
    serial_write_string(dev, "0x");
    for (int i = 28; i >= 0; i -= 4)
        serial_write_char(dev, hex[(value >> i) & 0xF]);
}

void print_hex64(serial_dev_t *dev, uint64_t value)
{
    static const char hex[] = "0123456789ABCDEF";
    if (!dev) return;
    serial_write_string(dev, "0x");
    for (int i = 60; i >= 0; i -= 4)
        serial_write_char(dev, hex[(value >> i) & 0xF]);
}

void print_uint64(serial_dev_t *dev, uint64_t value)
{
    if (!dev) return;
    if (value == 0) {
        serial_write_char(dev, '0');
        return;
    }

    char buf[21];
    int i = 20;
    buf[i] = '\0';

    while (value > 0 && i > 0) {
        buf[--i] = '0' + (value % 10);
        value /= 10;
    }

    serial_write_string(dev, &buf[i]);
}

void print_int64(serial_dev_t *dev, int64_t value)
{
    if (!dev) return;
    if (value < 0) {
        serial_write_char(dev, '-');
        /* Cast to uint64_t before negation: -(uint64_t)value where value
         * is negative uses two's complement wrapping, avoiding the UB of
         * negating INT64_MIN as a signed type. */
        print_uint64(dev, ~((uint64_t)value) + 1);
    } else {
        print_uint64(dev, (uint64_t)value);
    }
}
