#ifndef LIB_PRINT_H
#define LIB_PRINT_H

#include <stdint.h>
#include "../drivers/serial.h"

void print_str(serial_dev_t *dev, const char *s);
void print_hex64(serial_dev_t *dev, uint64_t value);
void print_hex32(serial_dev_t *dev, uint32_t value);
void print_uint64(serial_dev_t *dev, uint64_t value);
void print_int64(serial_dev_t *dev, int64_t value);
void print_hex8(serial_dev_t *dev, uint8_t value);

#endif
