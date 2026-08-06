#include "pcspk.h"
#include "pit.h"
#include "portio.h"
#include "../kernel/timer.h"
#include "../drivers/serial.h"
#include <stdint.h>

#define PCSPK_PORT   0x61
#define PCSPK_CMD    0xB6

static serial_dev_t *g_spk_serial = NULL;

/* Save/restore the interrupt flag instead of blindly re-enabling it.
 * An unconditional sti here turns interrupts on in the middle of boot
 * (pcspk_init runs before the scheduler is ready) and lets the timer
 * preempt the kernel before it has finished creating tasks. */
static uint64_t spk_save_if(void) {
    uint64_t rflags;
    asm volatile ("pushfq; pop %0" : "=r"(rflags));
    return rflags;
}

static void spk_restore_if(uint64_t rflags) {
    if (rflags & 0x200) asm volatile ("sti");
}

void pcspk_init(void *serial_dev) {
    g_spk_serial = (serial_dev_t *)serial_dev;
    pcspk_mute();
    if (g_spk_serial)
        serial_write_string(g_spk_serial, "[PCSPK] Initialized.\r\n");
}

void pcspk_beep(uint32_t freq_hz, uint32_t duration_ms) {
    if (freq_hz < 20 || freq_hz > 20000) return;

    uint32_t divisor = PIT_BASE_FREQ / freq_hz;
    uint64_t rflags = spk_save_if();

    /* Disable interrupts briefly to ensure PIT channel 2 and speaker
     * are programmed atomically — a timer IRQ mid-sequence could
     * corrupt the channel 2 divisor or port 0x61 state. */
    asm volatile ("cli");
    outb(PIT_COMMAND_PORT, PCSPK_CMD);
    outb(PIT_CHANNEL2_PORT, (uint8_t)(divisor & 0xFF));
    outb(PIT_CHANNEL2_PORT, (uint8_t)((divisor >> 8) & 0xFF));
    uint8_t tmp = inb(PCSPK_PORT);
    outb(PCSPK_PORT, tmp | 0x03);
    spk_restore_if(rflags);

    if (timer_is_initialized()) {
        uint64_t target = timer_get_ticks() + ((uint64_t)duration_ms * PIT_DEFAULT_FREQ / 1000);
        uint64_t rflags;
        asm volatile ("pushfq; pop %0" : "=r"(rflags));
        if (rflags & 0x200) {
            while (timer_get_ticks() < target) asm volatile ("hlt");
        } else {
            volatile uint32_t d = duration_ms * 10000;
            while (d--) asm volatile ("pause");
        }
    } else {
        volatile uint32_t d = duration_ms * 10000;
        while (d--) asm volatile ("pause");
    }

    pcspk_mute();
}

void pcspk_click(void) {
    pcspk_beep(1000, 50);
}

void pcspk_mute(void) {
    uint64_t rflags = spk_save_if();
    asm volatile ("cli");
    uint8_t tmp = inb(PCSPK_PORT);
    outb(PCSPK_PORT, tmp & ~0x03);
    spk_restore_if(rflags);
}
