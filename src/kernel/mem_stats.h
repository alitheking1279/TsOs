#ifndef KERNEL_MEM_STATS_H
#define KERNEL_MEM_STATS_H

#include <stdint.h>
#include "../drivers/serial.h"

/* Memory statistics dump — unified view of PMM, VMM, kheap, slab usage. */

void mem_stats_init(void *serial_dev);
void mem_stats_dump(void);

#endif
