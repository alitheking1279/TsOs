/**
 * @file iretq_trampoline.c
 * @brief Kernel-mode trampoline that performs IRETQ to ring 3.
 *
 * When a user task is first scheduled, context_switch "returns" here
 * (kernel mode).  We load the IRETQ frame that was built on the
 * kernel stack and execute IRETQ to transfer to user mode.
 *
 * The IRETQ frame layout (5 qwords at current RSP):
 *   [RSP+0]  RIP  = user task entry address
 *   [RSP+8]  CS   = GDT_USER_CS_SEL (0x23)
 *   [RSP+16] RFLAGS = 0x202 (IF enabled)
 *   [RSP+24] RSP  = user stack top
 *   [RSP+32] SS   = GDT_USER_DS_SEL (0x1B)
 */

void user_task_iretq_trampoline(void) {
    __asm__ volatile (
        "iretq"
        : : : "memory"
    );
    /* Should never return. */
    while (1) { __asm__ volatile ("hlt"); }
}
