#include "kernel/spinlock.h"
#include "drivers/serial.h"

/* Intel SDM Vol.3 §8.1.2: On a uniprocessor, spinlocks degenerate to
   disable/enable interrupts to prevent deadlocks from interrupt preemption.
   On MP, we use xchg which is atomic across all cores.

   Intel SDM Vol.2A §XCHG: XCHG with a memory operand always asserts
   the LOCK# signal, even without the LOCK prefix. This provides a
   full memory barrier (sequential consistency).

   SMP safety: RFLAGS is returned to the caller (on the caller's stack)
   instead of stored in the lock struct.  This prevents the race where
   CPU B overwrites CPU A's saved_rflags while A still holds the lock.
   The caller must pass the saved rflags back on release. */

static inline uint32_t xchg(volatile uint32_t *addr, uint32_t val)
{
    asm volatile ("xchgl %0, %1"
                  : "+r"(val), "+m"(*addr)
                  :
                  : "memory", "cc");
    return val;
}

/** Save RFLAGS and disable interrupts.  Returns saved RFLAGS for restore. */
static inline uint64_t save_and_cli(void) {
    uint64_t rflags;
    asm volatile ("pushfq; pop %0; cli" : "=r"(rflags));
    return rflags;
}

/** Restore RFLAGS (including interrupt flag) from saved value. */
static inline void restore_rflags(uint64_t rflags) {
    asm volatile ("push %0; popfq" : : "r"(rflags) : "memory", "cc");
}

void spinlock_init(spinlock_t *lock)
{
    lock->locked = 0;
    lock->owner_file = 0;
    lock->owner_line = 0;
}

uint64_t spinlock_acquire(spinlock_t *lock, const char *file, int line)
{
    /* Intel SDM Vol.3 §8.1.1: CLI clears IF to disable maskable interrupts,
       preventing timer/interrupt handlers from preempting while we hold
       the lock.  We save RFLAGS first so spinlock_release can restore the
       caller's original interrupt state. */
    uint64_t rflags = save_and_cli();

    while (xchg(&lock->locked, 1) != 0) {
        /* Intel SDM Vol.2A §PAUSE: Hint that we are in a spin-wait loop.
           Avoids memory order violation penalty and reduces power on
           Hyperthreaded CPUs. */
        asm volatile ("pause");
    }

    lock->owner_file = file;
    lock->owner_line = line;

    /* Return saved rflags to caller's stack — not stored in the lock struct.
     * This is SMP-safe: each CPU gets its own copy on its own stack. */
    return rflags;
}

void spinlock_release(spinlock_t *lock, uint64_t rflags)
{
    lock->owner_file = 0;
    lock->owner_line = 0;

    /* Intel SDM §7.2: MOV with immediate to a non-stack memory location
       is not a full barrier on x86, but xchg already provided it on acquire.
       A plain store is sufficient for release semantics on x86 (TSO). */
    asm volatile ("movl $0, %0" : "=m"(lock->locked) :: "memory", "cc");

    /* Restore caller's interrupt state from the saved RFLAGS.
     * This preserves correct nesting: if interrupts were already disabled
     * when the outer lock was acquired, restoring RFLAGS keeps them disabled. */
    restore_rflags(rflags);
}

uint64_t spinlock_try_acquire(spinlock_t *lock, const char *file, int line)
{
    /* Save and disable interrupts for the same reason as spinlock_acquire. */
    uint64_t rflags = save_and_cli();

    if (xchg(&lock->locked, 1) == 0) {
        /* Successfully acquired — set owner info and return saved rflags. */
        lock->owner_file = file;
        lock->owner_line = line;
        return rflags;
    }

    /* Failed to acquire — restore caller's interrupt state and return 0. */
    restore_rflags(rflags);
    return 0;
}
