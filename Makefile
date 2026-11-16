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
QEMU_SYS64 := qemu-system-x86_64

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
# CAMBIOS #1-#12: los nuevos .c ya los recoge el wildcard de arriba automáticamente.
# (lib/sha256.c, fs/pipe.c, mm/fork.c, drivers/rtl8139.c, drivers/net.c,
#  drivers/acpi_power.c, cpu/smp_boot.c)
ASM_SOURCES := boot/boot.asm boot/kernel_entry.asm boot/gdt_flush.asm \
               cpu/idt_flush.asm cpu/isr_asm.asm cpu/irq_asm.asm \
               cpu/syscall_asm.asm cpu/elf_jmp.asm \
               process/switch.asm process/thread_exit.asm
# longmode.asm es infraestructura 64-bit (no se enlaza en el build 32-bit actual)
# ASM_SOURCES += boot/longmode.asm

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
# Raw disk QEMU expone como maestro IDE primario. El driver ATA del kernel lo
# lee/escribe; `sync` guarda ahi el filesystem.
#
# 256 MiB y no 128: blockfs necesita 48 MiB de reserva de arranque + 64 MiB de
# cola para el snapshot de diskfs + espacio para datos. Con 128 MiB quedaban
# ~16 MiB utiles y `blockfs_init()` se desactivaba en silencio (los ficheros se
# quedaban en RAM), asi que la persistencia no se probaba nunca.
DISK    := disk.img
DISKMB  := 256

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
# Verifica de verdad que la imagen sirve: antes `grub-mkrescue` podía "tener
# éxito" y dejar una ISO sin grub.cfg utilizable, cuyo síntoma al arrancar es
# un `grub rescue>` en vez del menú.
iso: $(KERNEL)
	@# El kernel DEBE llevar la cabecera Multiboot v1 (magic 02 b0 ad 1b).
	@if ! LC_ALL=C grep -qa $$'\x02\xb0\xad\x1b' $(KERNEL); then \
	    echo "ERROR: $(KERNEL) no tiene el magic Multiboot (0x1BADB002)."; \
	    echo "       GRUB no lo reconocera como kernel Multiboot."; exit 1; fi
	mkdir -p iso/boot/grub
	cp $(KERNEL) iso/boot/$(KERNEL)
	printf 'set timeout=3\nset default=0\nmenuentry "Trinux" {\n  multiboot /boot/$(KERNEL)\n  boot\n}\nmenuentry "Trinux (single-user / recovery)" {\n  multiboot /boot/$(KERNEL) single\n  boot\n}\n' > iso/boot/grub/grub.cfg
	grep -q "multiboot /boot/$(KERNEL)" iso/boot/grub/grub.cfg
	grub-mkrescue -o $(ISO) iso
	@# Firma 0x55AA en el byte 510: sin ella la BIOS no arranca el medio.
	@sig=$$(dd if=$(ISO) bs=1 skip=510 count=2 2>/dev/null | od -An -tx1 | tr -d ' \n'); \
	 if [ "$$sig" != "55aa" ]; then \
	    echo "ERROR: $(ISO) no tiene firma de arranque 0x55AA en el MBR."; exit 1; fi
	@# Que el cfg y el kernel esten DENTRO de la imagen (lo que busca GRUB).
	@# Si GRUB arranca pero no los encuentra, el sintoma es `grub rescue>`.
	@grep -qa "multiboot /boot/$(KERNEL)" $(ISO) \
	   || { echo "ERROR: $(ISO) no contiene la entrada multiboot en /boot/grub/grub.cfg"; exit 1; }
	@grep -qa $$'\x02\xb0\xad\x1b' $(ISO) \
	   || { echo "ERROR: $(ISO) no contiene el kernel $(KERNEL) (sin magic Multiboot)"; exit 1; }
	@echo "Built $(ISO) ($(shell stat -c%s $(ISO) 2>/dev/null || echo '?') bytes)"

run-iso: iso $(DISK)
	$(QEMU) -cdrom $(ISO) -m 512M -serial stdio \
		-drive file=$(DISK),format=raw,if=ide

# ---- Probar la imagen USB en QEMU (BIOS + serie en la MISMA ventana) ----
# Con `-serial stdio` la consola serie se va a la terminal, asi que la ventana
# de QEMU se queda SIN VIDEO y parece colgada en "GRUB" aunque el kernel haya
# arrancado perfectamente. Con `-nographic` el BIOS, GRUB, la salida del
# kernel y el puerto serie comparten una sola consola: es la forma correcta
# de ver si GRUB dibuja su menu.
#   salir: Ctrl-A seguido de X
run-usb:
	@[ -f mykernel-usb.img ] || { echo "falta mykernel-usb.img; ejecuta 'make usb'"; exit 1; }
	$(QEMU) -nographic -drive file=mykernel-usb.img,format=raw,if=ide -m 512M

# Version UEFI (necesita OVMF instalado: apt install ovmf)
run-usb-uefi:
	@[ -f mykernel-usb.img ] || { echo "falta mykernel-usb.img; ejecuta 'make usb'"; exit 1; }
	@OVMF=$$(ls /usr/share/ovmf/OVMF.fd /usr/share/OVMF/OVMF_CODE.fd \
	              /usr/share/edk2/ovmf/OVMF_CODE.fd 2>/dev/null | head -1); \
	 [ -n "$$OVMF" ] || { echo "OVMF no encontrado: apt install ovmf"; exit 1; }; \
	 $(QEMU_SYS64) -nographic -bios "$$OVMF" \
		-drive file=mykernel-usb.img,format=raw,if=ide -m 512M

# ---- Imagen USB (verificada) ----
# No es un simple `cp` del ISO: el script comprueba firma MBR, grub.cfg y
# kernel dentro de la imagen, y aborta si algo falta. Un fallo silencioso
# aquí es exactamente lo que produce `grub rescue>` al arrancar el pendrive.
usb: $(KERNEL)
	bash make-usb-image.sh

# ---- Smoke test (lo que ejecuta el CI) ----
# Arranca el kernel en QEMU headless, captura COM1 y valida la secuencia de
# arranque. Ver scripts/smoke_test.sh y docs/github-actions/README.md.
smoke: $(KERNEL)
	bash scripts/smoke_test.sh

# ---- Auditoría estática (lo que ejecuta audit.yml) ----
audit:
	@python3 scripts/gen_embedded.py --check
	@echo "== syscalls declarados vs implementados =="
	@python3 -c "import re,sys;\
h=open('user/trinux.h').read();c=open('cpu/syscall.c').read();\
d={int(n):m for m,n in re.findall(r'#define\\s+(SYS_\\w+)\\s+(\\d+)',h)};\
i=set(re.findall(r'case\\s+(SYS_\\w+)\\s*:',c));\
m=[(d[n],n) for n in sorted(d) if d[n] not in i];\
print(f'{len(d)} declarados, {len(d)-len(m)} con handler');\
sys.exit(1 if m else 0)"
	@echo "== artefactos versionados =="
	@n=$$(git ls-files '*.o' '*.bin' '*.iso' '*.img' | wc -l); \
	  if [ "$$n" -ne 0 ]; then echo "$$n artefactos versionados"; exit 1; fi; \
	  echo "0 artefactos versionados"

# ---- Análisis estático de GCC (lo que ejecuta audit.yml) ----
analyze:
	@rc=0; \
	for f in boot/*.c kernel/*.c drivers/*.c cpu/*.c mm/*.c fs/*.c \
	         process/*.c shell/*.c auth/*.c lib/*.c user/*.c; do \
	  out=$$(gcc -m32 -std=gnu11 -ffreestanding -O1 -fanalyzer -Wall -Wextra \
	              -nostdlib -fno-builtin -fno-stack-protector -fno-pic \
	              -Iinclude -Ilib -c "$$f" -o /dev/null 2>&1 || true); \
	  bad=$$(printf '%s\n' "$$out" | grep -E '\[-Wanalyzer-|: error:' || true); \
	  if [ -n "$$bad" ]; then echo "❌ $$f"; printf '%s\n' "$$bad" | head -6; rc=1; fi; \
	done; \
	[ $$rc -eq 0 ] && echo "✅ -fanalyzer limpio"; exit $$rc

# ---- Cleanup ----
clean:
	rm -f $(OBJS) $(KERNEL) $(ISO)
	rm -rf iso

.PHONY: all run run-curses run-iso iso usb run-usb run-usb-uefi clean smoke audit analyze
