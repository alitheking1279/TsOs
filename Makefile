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

.DEFAULT_GOAL := all

CC      = gcc
CFLAGS  = -m64 -ffreestanding -fno-builtin -fno-stack-protector \
          -mno-red-zone -mgeneral-regs-only \
          -nostartfiles -nodefaultlibs -Wall -Wextra -Isrc/libc/include -Isrc -c \
          -O2 -D_FORTIFY_SOURCE=0

# Set SKIP_TESTS=1 to build without the test suite (faster boot, smaller binary).
ifdef SKIP_TESTS
CFLAGS += -DTSOS_SKIP_TESTS
endif

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
              src/drivers/ata.c \
              src/kernel/timer.c \
              src/kernel/task.c \
              src/kernel/scheduler.c \
              src/kernel/mlfq.c \
              src/kernel/syscall.c \
              src/kernel/elf.c \
              src/kernel/iretq_trampoline.c \
              src/fs/block_dev.c \
              src/fs/bcache.c \
              src/fs/ext2.c \
              src/fs/vfs.c \
              src/libc/common/string.c \
              src/libc/common/ctype.c \
              src/libc/common/stdlib.c \
              src/libc/common/vsnprintf.c \
              src/libc/kernel/errno.c \
              src/libc/kernel/stdio.c \
              src/libc/kernel/stdlib.c \
              src/libc/userspace/syscalls.c \
              src/libc/userspace/malloc.c \
              src/libc/userspace/stdlib.c \
              src/libc/userspace/posix.c \
              src/libc/userspace/stdio.c \
              src/lib/print.c \
              src/drivers/serial.c \
              src/drivers/ps2.c \
              src/drivers/vga.c \
              src/drivers/pcspk.c \
              src/drivers/pci.c \
              src/drivers/ac97.c \
              src/user/shell.c \
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
              src/tests/test_elf.c \
              src/tests/test_ata.c \
              src/tests/test_block_dev.c \
              src/tests/test_bcache.c \
              src/tests/test_ext2_struct.c \
              src/tests/test_ext2_inode_ops.c \
              src/tests/test_ext2_bitmap.c \
              src/tests/test_ext2_block_map.c \
              src/tests/test_ext2_fileio.c \
              src/tests/test_ext2_dir.c \
              src/tests/test_ext2_path.c \
              src/tests/test_ext2_syscall.c \
              src/tests/test_ext2_phase9.c \
              src/tests/test_vfs.c \
              src/tests/test_ps2.c \
              src/tests/test_vga.c \
              src/tests/test_pcspk.c \
              src/tests/test_pci.c \
              src/tests/test_ac97.c \
              src/tests/test_console.c

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
              $(BUILD)/ata.o \
              $(BUILD)/timer.o \
              $(BUILD)/task.o \
              $(BUILD)/scheduler.o \
              $(BUILD)/mlfq.o \
              $(BUILD)/syscall.o \
              $(BUILD)/string.o \
              $(BUILD)/ctype.o \
              $(BUILD)/stdlib.o \
              $(BUILD)/vsnprintf.o \
              $(BUILD)/kernel_stdlib.o \
              $(BUILD)/errno.o \
              $(BUILD)/stdio.o \
              $(BUILD)/user_syscalls.o \
              $(BUILD)/user_malloc.o \
              $(BUILD)/user_stdlib.o \
              $(BUILD)/user_posix.o \
              $(BUILD)/user_stdio.o \
              $(BUILD)/print.o \
              $(BUILD)/serial.o \
              $(BUILD)/ps2.o \
              $(BUILD)/vga.o \
              $(BUILD)/pcspk.o \
              $(BUILD)/pci.o \
              $(BUILD)/ac97.o \
              $(BUILD)/elf.o \
              $(BUILD)/iretq_trampoline.o \
              $(BUILD)/block_dev.o \
              $(BUILD)/bcache.o \
              $(BUILD)/ext2.o \
              $(BUILD)/shell.o \
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
              $(BUILD)/test_elf.o \
              $(BUILD)/test_ata.o \
              $(BUILD)/test_block_dev.o \
              $(BUILD)/test_bcache.o \
              $(BUILD)/test_ext2_struct.o \
              $(BUILD)/test_ext2_inode_ops.o \
              $(BUILD)/test_ext2_bitmap.o \
              $(BUILD)/test_ext2_block_map.o \
              $(BUILD)/test_ext2_fileio.o \
              $(BUILD)/test_ext2_dir.o \
              $(BUILD)/test_ext2_path.o \
              $(BUILD)/test_ext2_syscall.o \
    $(BUILD)/test_ext2_phase9.o \
    $(BUILD)/test_vfs.o \
    $(BUILD)/test_ps2.o \
    $(BUILD)/test_vga.o \
    $(BUILD)/test_pcspk.o \
    $(BUILD)/test_pci.o \
    $(BUILD)/test_ac97.o \
    $(BUILD)/test_console.o \
    $(BUILD)/vfs.o

OBJECTS = $(ASM_OBJECTS) $(C_OBJECTS)

# -----------------------------------------------------------------------------
# Ring-3 user program (user_shell.elf)
#
# Built as a standalone ELF linked at 0x400000, then embedded into the
# kernel image with `ld -r -b binary`.  Symbols:
#   _binary_user_shell_elf_start / _binary_user_shell_elf_end
# are referenced by main.c to spawn the shell via task_create_elf().
#
# Object names are prefixed with userapp_ to avoid collisions with the
# kernel's user_* libc objects in the flat build dir.
# -----------------------------------------------------------------------------
USER_ELF    = $(BUILD)/user_shell.elf
USER_OBJECTS = $(BUILD)/userapp_entry.o \
               $(BUILD)/userapp_ushell.o \
               $(BUILD)/userapp_syscalls.o \
               $(BUILD)/userapp_malloc.o \
               $(BUILD)/userapp_stdlib.o \
               $(BUILD)/userapp_posix.o \
               $(BUILD)/userapp_stdio.o \
               $(BUILD)/userapp_string.o \
               $(BUILD)/userapp_ctype.o \
               $(BUILD)/userapp_common_stdlib.o \
               $(BUILD)/userapp_vsnprintf.o \
               $(BUILD)/userapp_errno.o

$(BUILD)/userapp_entry.o: src/user/entry.asm
	@mkdir -p $(BUILD)
	$(ASM) $(ASMFLAGS) $< -o $@

$(BUILD)/userapp_ushell.o: src/user/ushell.c
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $< -o $@

$(BUILD)/userapp_syscalls.o: src/libc/userspace/syscalls.c
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $< -o $@

$(BUILD)/userapp_malloc.o: src/libc/userspace/malloc.c
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $< -o $@

$(BUILD)/userapp_stdlib.o: src/libc/userspace/stdlib.c
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $< -o $@

$(BUILD)/userapp_posix.o: src/libc/userspace/posix.c
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $< -o $@

$(BUILD)/userapp_stdio.o: src/libc/userspace/stdio.c
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $< -o $@

$(BUILD)/userapp_string.o: src/libc/common/string.c
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $< -o $@

$(BUILD)/userapp_ctype.o: src/libc/common/ctype.c
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $< -o $@

$(BUILD)/userapp_common_stdlib.o: src/libc/common/stdlib.c
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $< -o $@

$(BUILD)/userapp_vsnprintf.o: src/libc/common/vsnprintf.c
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $< -o $@

$(BUILD)/userapp_errno.o: src/libc/kernel/errno.c
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $< -o $@

$(USER_ELF): $(USER_OBJECTS)
	$(LD) -T src/user/user.ld -m elf_x86_64 -o $@ $(USER_OBJECTS)

# Embed the user ELF as a binary blob in the kernel image.  cd into the build
# dir so ld names the blob symbols from the bare filename:
#   _binary_user_shell_elf_start / _binary_user_shell_elf_end
$(BUILD)/user_shell_elf.o: $(USER_ELF)
	cd $(BUILD) && $(LD) -r -b binary user_shell.elf -o user_shell_elf.o

OBJECTS += $(BUILD)/user_shell_elf.o

# =============================================================================
# Primary targets
# =============================================================================

all: TsOs.iso

TsOs.iso: $(OBJECTS) iso/boot/grub/grub.cfg
	$(LD) $(LDFLAGS) -o iso/boot/kernel.bin $(OBJECTS)
	grub-mkrescue -o TsOs.iso iso

iso/boot/grub/grub.cfg:
	@mkdir -p iso/boot/grub
	@echo 'set timeout=0' > iso/boot/grub/grub.cfg
	@echo 'set default=0' >> iso/boot/grub/grub.cfg
	@echo '' >> iso/boot/grub/grub.cfg
	@echo 'menuentry "TsOs" {' >> iso/boot/grub/grub.cfg
	@echo '    multiboot2 /boot/kernel.bin' >> iso/boot/grub/grub.cfg
	@echo '    boot' >> iso/boot/grub/grub.cfg
	@echo '}' >> iso/boot/grub/grub.cfg

# Run under QEMU (with tests):
#   -serial stdio        COM1 -> host stdout (test output)
#   -serial null         COM2 -> /dev/null   (absorbs write_char_ok 'X' etc.)
#   -display none        headless (no VGA window)
#   -no-reboot           on triple-fault, exit instead of loop
#   -drive               secondary IDE drive for block device tests
test: TsOs.iso disk.img
	qemu-system-x86_64 -cdrom TsOs.iso \
	    -drive file=disk.img,format=raw,if=ide,index=0,media=disk \
	    -serial stdio -serial null \
	    -display none -no-reboot -accel tcg \
	    -audiodev sdl,id=snd0 -machine pcspk-audio=snd0

run: TsOs.iso disk.img
	qemu-system-x86_64 -cdrom TsOs.iso \
	    -drive file=disk.img,format=raw,if=ide,index=0,media=disk \
	    -serial stdio -serial null \
	    -audiodev sdl,id=snd0 -machine pcspk-audio=snd0

# Production build — skips tests for faster boot.
run-prod: CFLAGS += -DTSOS_SKIP_TESTS
run-prod: TsOs.iso disk.img
	qemu-system-x86_64 -cdrom TsOs.iso \
	    -drive file=disk.img,format=raw,if=ide,index=0,media=disk \
	    -serial stdio -serial null

# Create a 4 MiB ext2 filesystem image for block device tests.
# Only created when missing; preserves data across runs.
disk.img:
	@if [ ! -f disk.img ]; then dd if=/dev/zero of=disk.img bs=1M count=4 2>/dev/null && mkfs.ext2 -F -q -b 1024 disk.img 2>/dev/null; fi

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

$(BUILD)/%.o: src/libc/common/%.c
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $< -o $@

$(BUILD)/%.o: src/libc/kernel/%.c
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $< -o $@

# Userspace libc objects — named user_* to avoid collisions with kernel objects
$(BUILD)/user_syscalls.o: src/libc/userspace/syscalls.c
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $< -o $@

$(BUILD)/user_malloc.o: src/libc/userspace/malloc.c
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $< -o $@

$(BUILD)/user_stdlib.o: src/libc/userspace/stdlib.c
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $< -o $@

$(BUILD)/user_posix.o: src/libc/userspace/posix.c
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $< -o $@

$(BUILD)/user_stdio.o: src/libc/userspace/stdio.c
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $< -o $@

# kernel_stdlib.o — kernel-specific stdlib (abort, __assert_fail)
# Avoids collision with common/stdlib.o (pure functions)
$(BUILD)/kernel_stdlib.o: src/libc/kernel/stdlib.c
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $< -o $@

$(BUILD)/%.o: src/fs/%.c
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

.PHONY: all test run run-prod lint clean debug
