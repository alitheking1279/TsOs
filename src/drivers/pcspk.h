#ifndef DRIVERS_PCSPK_H
#define DRIVERS_PCSPK_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void pcspk_init(void *serial_dev);
void pcspk_beep(uint32_t freq_hz, uint32_t duration_ms);
void pcspk_click(void);
void pcspk_mute(void);

#ifdef __cplusplus
}
#endif

#endif /* DRIVERS_PCSPK_H */
