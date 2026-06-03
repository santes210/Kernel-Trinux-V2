# ============================================================================
# mykernel - Makefile
#
# Uses the host gcc in 32-bit mode (-m32) when no i686-elf cross-compiler is
# available. If you have an i686-elf toolchain, override:
#     make CC=i686-elf-gcc LD=i686-elf-ld USE_CROSS=1
# ============================================================================

# ---- Toolchain ----
USE_CROSS ?= 0
ifeq ($(USE_CROSS),1)
  CC := i686-elf-gcc
  LD := i686-elf-ld
  ARCHFLAGS :=
else
  CC := gcc
  LD := ld
  ARCHFLAGS := -m32
  LDARCH := -m elf_i386
endif

ASM := nasm
QEMU := qemu-system-i386

# ---- Flags ----
CFLAGS  := $(ARCHFLAGS) -std=gnu11 -ffreestanding -O2 -Wall -Wextra \
           -nostdlib -fno-builtin -fno-stack-protector -fno-pic \
           -fno-asynchronous-unwind-tables -Iinclude -Ilib
ASFLAGS := -f elf32
LDFLAGS := $(LDARCH) -T linker.ld -nostdlib

# ---- Sources ----
C_SOURCES := $(wildcard boot/*.c kernel/*.c drivers/*.c cpu/*.c mm/*.c \
                        fs/*.c process/*.c shell/*.c auth/*.c lib/*.c \
                        user/*.c)
ASM_SOURCES := boot/boot.asm boot/kernel_entry.asm boot/gdt_flush.asm \
               cpu/idt_flush.asm cpu/isr_asm.asm cpu/irq_asm.asm \
               cpu/syscall_asm.asm process/switch.asm process/thread_exit.asm

OBJS := $(C_SOURCES:.c=.o) $(ASM_SOURCES:.asm=.o)

KERNEL := mykernel.bin
ISO    := mykernel.iso

# ---- Default ----
all: $(KERNEL)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

%.o: %.asm
	$(ASM) $(ASFLAGS) $< -o $@

$(KERNEL): $(OBJS) linker.ld
	$(LD) $(LDFLAGS) -o $@ $(OBJS)
	@echo "Built $(KERNEL)"

# ---- Disk image for persistence ----
# A raw 16 MiB disk QEMU exposes as the primary IDE master. The kernel's ATA
# driver reads/writes it; `sync` in the shell saves the filesystem here.
DISK    := disk.img
DISKMB  := 128

$(DISK):
	@echo "Creating $(DISKMB) MiB disk image $(DISK)"
	@dd if=/dev/zero of=$(DISK) bs=1M count=$(DISKMB) status=none

disk: $(DISK)

# ---- Run in QEMU (Multiboot) ----
# -drive ... if=ide attaches $(DISK) as the primary master so files persist.
run: $(KERNEL) $(DISK)
	$(QEMU) -kernel $(KERNEL) -m 512M -serial stdio \
		-drive file=$(DISK),format=raw,if=ide

run-curses: $(KERNEL) $(DISK)
	$(QEMU) -kernel $(KERNEL) -m 512M -display curses \
		-drive file=$(DISK),format=raw,if=ide

# ---- ISO (requires grub-mkrescue + xorriso) ----
iso: $(KERNEL)
	mkdir -p iso/boot/grub
	cp $(KERNEL) iso/boot/$(KERNEL)
	printf 'set timeout=0\nset default=0\nmenuentry "mykernel" {\n  multiboot /boot/$(KERNEL)\n  boot\n}\n' > iso/boot/grub/grub.cfg
	grub-mkrescue -o $(ISO) iso
	@echo "Built $(ISO)"

run-iso: iso $(DISK)
	$(QEMU) -cdrom $(ISO) -m 512M -serial stdio \
		-drive file=$(DISK),format=raw,if=ide

# ---- Cleanup ----
clean:
	rm -f $(OBJS) $(KERNEL) $(ISO)
	rm -rf iso

.PHONY: all run run-curses run-iso iso clean
