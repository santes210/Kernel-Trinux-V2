#!/usr/bin/env bash
# ============================================================================
# make-usb-image.sh  -  build ONE bootable disk image that ALSO persists files.
#
# Produces mykernel-usb.img : a raw disk image with
#   - a GRUB (BIOS/Legacy) bootloader + the kernel at the START
#   - a reserved persistence region at the END (used by the kernel's diskfs)
#
# Flash it straight to a USB stick and boot a PC from it (BIOS / Legacy / CSM),
# or run it in QEMU. The same medium boots AND saves your files.
#
# Usage:
#   ./make-usb-image.sh [SIZE_MB]      (default 512)
#
# Requirements: grub-mkrescue OR grub-install, xorriso, and the kernel built
# (make). Run as a normal user; uses a loopback-free approach via grub-mkrescue
# writing to an .img through hybrid MBR (isohybrid), then pads the tail.
# ============================================================================
set -e

SIZE_MB="${1:-512}"
KERNEL="mykernel.bin"
OUT="mykernel-usb.img"

if [ ! -f "$KERNEL" ]; then
    echo "[*] $KERNEL not found, building..."
    make
fi

echo "[*] Building bootable ISO (GRUB + Multiboot kernel)..."
mkdir -p iso/boot/grub
cp "$KERNEL" iso/boot/"$KERNEL"
cat > iso/boot/grub/grub.cfg <<EOF
set timeout=3
set default=0

menuentry "Trinux" {
  multiboot /boot/$KERNEL
  boot
}

menuentry "Trinux (single-user / recovery)" {
  multiboot /boot/$KERNEL single
  boot
}
EOF
grub-mkrescue -o "$OUT" iso >/dev/null 2>&1
echo "[*] Base ISO written to $OUT (hybrid MBR, USB-bootable)."

# Grow the image to SIZE_MB so the kernel has room for its tail persistence
# region. The ISO part stays intact at the front; the rest is zero-filled.
CUR_BYTES=$(stat -c%s "$OUT")
TARGET_BYTES=$((SIZE_MB * 1024 * 1024))
if [ "$TARGET_BYTES" -gt "$CUR_BYTES" ]; then
    echo "[*] Growing image to exactly ${SIZE_MB} MiB (persistence room at end)..."
    # truncate extends the file with zeros to an exact size, keeping boot data.
    if command -v truncate >/dev/null 2>&1; then
        truncate -s "${SIZE_MB}M" "$OUT"
    else
        dd if=/dev/zero of="$OUT" bs=1 count=0 seek="$TARGET_BYTES" 2>/dev/null
    fi
fi

FINAL=$(stat -c%s "$OUT")
echo "[*] Done: $OUT  ($((FINAL/1024/1024)) MiB)"
echo
echo "Try it in QEMU (boots AND persists in the same file):"
echo "  qemu-system-i386 -drive file=$OUT,format=raw,if=ide -m 512M"
echo
echo "Flash to USB (CAREFUL: replace sdX with your stick; ERASES it):"
echo "  sudo dd if=$OUT of=/dev/sdX bs=4M status=progress conv=fsync"
