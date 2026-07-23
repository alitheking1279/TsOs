#ifndef KERNEL_SPINLOCK_H
#define KERNEL_SPINLOCK_H

#include <stdint.h>

/* Intel SDM Vol.3 §7.2: The LOCK prefix ensures atomic read-modify-write.
   xchg is inherently locked and acts as a full memory barrier.
   spinlocks are the minimal synchronization primitive for uniprocessor and
   multiprocessor kernel code.

   Intel SDM Vol.3 §8.1.2: On a uniprocessor, a spinlock that does not
   disable interrupts will deadlock if the interrupt handler attempts to
   acquire the same lock.  Therefore, spinlock_acquire saves RFLAGS,
   disables interrupts, and spinlock_release restores the saved RFLAGS.
   This provides correct nesting: if the outer lock already disabled
   interrupts, the inner release will see IF=0 and leave it disabled.

   SMP safety: saved_rflags is returned to the caller (on the caller's
   stack) instead of stored in the lock struct.  This prevents the race
   where CPU B overwrites CPU A's saved_rflags while A still holds the lock. */

typedef struct {
    volatile uint32_t locked;
    const char *owner_file;
    int owner_line;
} spinlock_t;

#define SPINLOCK_INIT { .locked = 0, .owner_file = 0, .owner_line = 0 }

void     spinlock_init(spinlock_t *lock);
uint64_t spinlock_acquire(spinlock_t *lock, const char *file, int line);
void     spinlock_release(spinlock_t *lock, uint64_t rflags);
uint64_t spinlock_try_acquire(spinlock_t *lock, const char *file, int line);

#define spin_lock(lock)          spinlock_acquire(lock, __FILE__, __LINE__)
#define spin_unlock(lock, rflags) spinlock_release(lock, rflags)
#define spin_trylock(lock)       spinlock_try_acquire(lock, __FILE__, __LINE__)

#endif
