# =============================================================================
# Makefile — TsOs bare-metal x86-64 OS
#
# Targets:
#   all      Build TsOs.iso (default)
#   test     Build then run under QEMU with serial output on stdout
#   lint     Re-compile all C sources with -Werror to catch warnings
#   clean    Remove all build artefacts
#   run      Build then launch QEMU with a graphical window + serial on stdout
# =============================================================================

CC      = gcc
CFLAGS  = -m64 -ffreestanding -fno-builtin -fno-stack-protector \
          -mno-red-zone -mgeneral-regs-only \
          -nostartfiles -nodefaultlibs -Wall -Wextra -Isrc -c \
          -O2

ASM     = nasm
ASMFLAGS = -f elf64

LD      = ld
LDFLAGS = -T linker.ld -m elf_x86_64

BUILD   = build

# -----------------------------------------------------------------------------
# Source lists
# Keep these in dependency order where possible (drivers before kernel before tests).
# -----------------------------------------------------------------------------

# Assembly sources — listed explicitly so we can control the output name
# and avoid collisions in the flat build dir.
ASM_SOURCES = src/boot/boot.asm \
              src/kernel/gdt_flush.asm \
              src/kernel/isr_stubs.asm \
              src/kernel/context_switch.asm \
              src/kernel/syscall_entry.asm

# C sources — pattern rules below match by source directory.
C_SOURCES   = src/kernel/main.c \
              src/kernel/gdt.c \
              src/kernel/pic.c \
              src/kernel/idt.c \
              src/kernel/isr.c \
              src/kernel/pmm.c \
              src/kernel/page_table.c \
              src/kernel/vmm.c \
              src/kernel/kheap.c \
              src/kernel/panic.c \
              src/kernel/spinlock.c \
              src/kernel/slab.c \
              src/kernel/pmm_buddy.c \
              src/kernel/mem_stats.c \
              src/drivers/pit.c \
              src/kernel/timer.c \
              src/kernel/task.c \
              src/kernel/scheduler.c \
              src/kernel/mlfq.c \
              src/kernel/syscall.c \
              src/kernel/elf.c \
              src/lib/string.c \
              src/lib/print.c \
              src/drivers/serial.c \
              src/user/user_main.c \
              src/tests/test.c \
              src/tests/test_serial.c \
              src/tests/test_boot.c \
              src/tests/test_gdt.c \
              src/tests/test_idt.c \
              src/tests/test_pmm.c \
              src/tests/test_vmm.c \
              src/tests/test_kheap.c \
              src/tests/test_slab.c \
              src/tests/test_spinlock.c \
              src/tests/test_panic.c \
              src/tests/test_pit.c \
              src/tests/test_timer.c \
              src/tests/test_task.c \
              src/tests/test_context_switch.c \
              src/tests/test_scheduler.c \
              src/tests/test_spinlock_rflags.c \
              src/tests/test_mlfq.c \
              src/tests/test_zombie.c \
              src/tests/test_syscall.c \
              src/tests/test_validation.c \
              src/tests/test_usermode.c \
              src/tests/test_elf.c

# -----------------------------------------------------------------------------
# Object lists
#
# gdt_flush.asm -> gdt_asm.o  (explicit, avoids collision with gdt.c -> gdt.o)
# isr_stubs.asm -> isr_stubs_asm.o  (explicit, avoids collision with isr.c -> isr.o)
# All other names derive 1:1 from their basename.
# -----------------------------------------------------------------------------
ASM_OBJECTS = $(BUILD)/boot.o \
              $(BUILD)/gdt_asm.o \
              $(BUILD)/isr_stubs_asm.o \
              $(BUILD)/context_switch_asm.o \
              $(BUILD)/syscall_entry_asm.o

C_OBJECTS   = $(BUILD)/main.o \
              $(BUILD)/gdt.o \
              $(BUILD)/pic.o \
              $(BUILD)/idt.o \
              $(BUILD)/isr.o \
              $(BUILD)/pmm.o \
              $(BUILD)/page_table.o \
              $(BUILD)/vmm.o \
              $(BUILD)/kheap.o \
              $(BUILD)/panic.o \
              $(BUILD)/spinlock.o \
              $(BUILD)/slab.o \
              $(BUILD)/pmm_buddy.o \
              $(BUILD)/mem_stats.o \
              $(BUILD)/pit.o \
              $(BUILD)/timer.o \
              $(BUILD)/task.o \
              $(BUILD)/scheduler.o \
              $(BUILD)/mlfq.o \
              $(BUILD)/syscall.o \
              $(BUILD)/string.o \
              $(BUILD)/print.o \
              $(BUILD)/serial.o \
              $(BUILD)/elf.o \
              $(BUILD)/user_main.o \
              $(BUILD)/test.o \
              $(BUILD)/test_serial.o \
              $(BUILD)/test_boot.o \
              $(BUILD)/test_gdt.o \
              $(BUILD)/test_idt.o \
              $(BUILD)/test_pmm.o \
              $(BUILD)/test_vmm.o \
              $(BUILD)/test_kheap.o \
              $(BUILD)/test_slab.o \
              $(BUILD)/test_spinlock.o \
              $(BUILD)/test_panic.o \
              $(BUILD)/test_pit.o \
              $(BUILD)/test_timer.o \
              $(BUILD)/test_task.o \
              $(BUILD)/test_context_switch.o \
              $(BUILD)/test_scheduler.o \
              $(BUILD)/test_spinlock_rflags.o \
              $(BUILD)/test_mlfq.o \
              $(BUILD)/test_zombie.o \
              $(BUILD)/test_syscall.o \
              $(BUILD)/test_validation.o \
              $(BUILD)/test_usermode.o \
              $(BUILD)/test_elf.o

OBJECTS = $(ASM_OBJECTS) $(C_OBJECTS)

# =============================================================================
# Primary targets
# =============================================================================

all: TsOs.iso

TsOs.iso: $(OBJECTS)
	@mkdir -p iso/boot/grub
	$(LD) $(LDFLAGS) -o iso/boot/kernel.bin $(OBJECTS)
	grub-mkrescue -o TsOs.iso iso

# Run under QEMU:
#   -serial stdio        COM1 -> host stdout (test output)
#   -serial null         COM2 -> /dev/null   (absorbs write_char_ok 'X' etc.)
#   -display none        headless (no VGA window)
#   -no-reboot           on triple-fault, exit instead of loop
test: TsOs.iso
	qemu-system-x86_64 -cdrom TsOs.iso \
	    -serial stdio -serial null \
	    -display none -no-reboot -accel tcg

run: TsOs.iso
	qemu-system-x86_64 -cdrom TsOs.iso -serial stdio -serial null

# =============================================================================
# Assembly build rules — explicit (not pattern) to control output filenames
# =============================================================================

$(BUILD)/boot.o: src/boot/boot.asm
	@mkdir -p $(BUILD)
	$(ASM) $(ASMFLAGS) $< -o $@

# gdt_flush.asm builds to gdt_asm.o so it doesn't collide with gdt.c -> gdt.o
$(BUILD)/gdt_asm.o: src/kernel/gdt_flush.asm
	@mkdir -p $(BUILD)
	$(ASM) $(ASMFLAGS) $< -o $@

# isr_stubs.asm builds to isr_stubs_asm.o so it doesn't collide with isr.c -> isr.o
$(BUILD)/isr_stubs_asm.o: src/kernel/isr_stubs.asm
	@mkdir -p $(BUILD)
	$(ASM) $(ASMFLAGS) $< -o $@

# context_switch.asm builds to context_switch_asm.o
$(BUILD)/context_switch_asm.o: src/kernel/context_switch.asm
	@mkdir -p $(BUILD)
	$(ASM) $(ASMFLAGS) $< -o $@

# syscall_entry.asm builds to syscall_entry_asm.o
$(BUILD)/syscall_entry_asm.o: src/kernel/syscall_entry.asm
	@mkdir -p $(BUILD)
	$(ASM) $(ASMFLAGS) $< -o $@

# =============================================================================
# C build rules — pattern rules per source directory
# =============================================================================

$(BUILD)/%.o: src/kernel/%.c
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $< -o $@

$(BUILD)/%.o: src/drivers/%.c
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $< -o $@

$(BUILD)/%.o: src/tests/%.c
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $< -o $@

$(BUILD)/%.o: src/lib/%.c
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $< -o $@

$(BUILD)/%.o: src/user/%.c
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $< -o $@

# =============================================================================
# Utility targets
# =============================================================================

# Compile all C sources with -Werror to surface warnings as errors.
lint:
	@for src in $(C_SOURCES); do \
		echo "Linting $$src"; \
		$(CC) $(CFLAGS) -Werror $$src -o /dev/null || exit 1; \
	done
	@echo "Lint passed."

# Debug build: no optimization, full symbols.
debug: CFLAGS += -O0 -g
debug: all

clean:
	rm -rf $(BUILD) iso/boot/kernel.bin TsOs.iso

.PHONY: all test run lint clean debug
