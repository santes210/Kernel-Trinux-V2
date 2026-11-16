#!/usr/bin/env bash
# ============================================================================
# make-usb-image.sh  -  build ONE bootable disk image that ALSO persists files.
#
# Produces mykernel-usb.img : a raw disk image with
#   - a GRUB (BIOS/Legacy) bootloader + the kernel at the START
#   - a reserved persistence region at the END (used by the kernel's diskfs)
#
# v2 — tres cambios importantes:
#   1. El grub.cfg de arranque ANTES era un here-doc con <<EOF, es decir SIN
#      comillas. Eso expande `/boot/$KERNEL` a `/boot/mykernel.bin` (bien) pero
#      también se comía cualquier `$` del cfg. Ahora usa 'EOF' y la variable se
#      expande explícitamente.
#   2. `grub-mkrescue ... >/dev/null 2>&1` ocultaba el fallo: si el rescue no
#      se generaba, el `set -e` de bash NO lo detectaba (grub-mkrescue devuelve
#      0 aunque falle algún paso interno) y se producía una imagen SIN
#      bootloader → "grub rescue>".  Ahora se verifica que la imagen tenga
#      arranque GRUB y se aborta si no.
#   3. Se comprueba que `iso/boot/grub/grub.cfg` y `iso/boot/mykernel.bin`
#      existen DENTRO de la imagen, que es justo lo que grub rescue> no
#      encuentra.
#
# Usage:
#   ./make-usb-image.sh [SIZE_MB]      (default 512)
#
# Requirements: grub-mkrescue OR grub-install, xorriso, and the kernel built
# (make). Run as a normal user; uses a loopback-free approach via grub-mkrescue
# writing to an .img through hybrid MBR (isohybrid), then pads the tail.
# ============================================================================
set -euo pipefail

SIZE_MB="${1:-512}"
KERNEL="mykernel.bin"
OUT="mykernel-usb.img"
ISODIR="iso"

die() { echo "[✗] $*" >&2; exit 1; }

# ---------------------------------------------------------------- kernel ----
if [ ! -f "$KERNEL" ]; then
    echo "[*] $KERNEL no existe, construyendo..."
    make
fi
[ -f "$KERNEL" ] || die "no se pudo construir $KERNEL"

# ------------------------------------------------------- sanity del kernel ---
# Un kernel Multiboot v1 tiene que llevar el magic 0x1BADB002 en los primeros
# 8 KiB. Si no está, GRUB no lo reconocerá como Multiboot y el menú no sirve
# de nada (y además no verás el error hasta arrancar).  Avisamos aquí.
if ! LC_ALL=C grep -qa $'\x02\xb0\xad\x1b' "$KERNEL"; then
    echo "[!] AVISO: no encuentro el magic Multiboot (0x1BADB002) en $KERNEL."
    echo "    Si usaste un stub sin cabecera Multiboot, GRUB no lo cargará."
fi

# ------------------------------------------------------ herramientas grub ----
command -v grub-mkrescue >/dev/null 2>&1 || \
    die "falta grub-mkrescue (apt install grub-pc-bin grub-efi-ia32-bin xorriso mtools)"

# ------------------------------------------------------------ iso layout ----
rm -rf "$ISODIR"
mkdir -p "$ISODIR/boot/grub"
cp "$KERNEL" "$ISODIR/boot/$KERNEL"
[ -s "$ISODIR/boot/$KERNEL" ] || die "copia del kernel vacía"

# Comillas simples: NO expandir nada salvo lo que pongamos con ${...}.
cat > "$ISODIR/boot/grub/grub.cfg" <<'GRUBEOF'
set timeout=3
set default=0

menuentry "Trinux" {
  multiboot /boot/__KERNEL__
  boot
}

menuentry "Trinux (single-user / recovery)" {
  multiboot /boot/__KERNEL__ single
  boot
}
GRUBEOF

# Sustitución explícita y verificable.
sed -i "s/__KERNEL__/${KERNEL}/g" "$ISODIR/boot/grub/grub.cfg"
grep -q "multiboot /boot/${KERNEL}" "$ISODIR/boot/grub/grub.cfg" || \
    die "el grub.cfg no contiene la entrada multiboot esperada"

# ---------------------------------------------------------------- imagen ----
echo "[*] Construyendo imagen arrancable (GRUB + Multiboot)..."
# SIN redirección a /dev/null: si algo falla queremos verlo.
if ! grub-mkrescue -o "$OUT" "$ISODIR"; then
    die "grub-mkrescue falló (revisa el mensaje de arriba)"
fi
[ -s "$OUT" ] || die "grub-mkrescue no generó $OUT"

# ----------------------------------------------- verificar sector 0 (MBR) ----
# El MBR de GRUB (boot.img) empieza por una instrucción de salto y lleva la
# firma 0x55AA en el byte 510. Si no está, la BIOS no arranca: pantalla negra
# o "no bootable device".
SIG=$(dd if="$OUT" bs=1 skip=510 count=2 2>/dev/null | od -An -tx1 | tr -d ' \n')
[ "$SIG" = "55aa" ] || die "el sector 0 de $OUT no tiene firma 0x55AA (no boteará en BIOS)"

echo "[*] Imagen base: $OUT ($(stat -c%s "$OUT") bytes, MBR OK)."

# ------------------------------------------- verificar contenido interno ----
# Comprobar que el grub.cfg y el kernel están DENTRO de la imagen es lo que
# distingue "GRUB arranca y muestra el menú" de "grub rescue>".
# No se usa xorriso para esto: basta buscar las cadenas en el flujo del ISO
# (la entrada del cfg y el magic Multiboot del kernel están en claro).
if ! LC_ALL=C grep -qa "multiboot /boot/${KERNEL}" "$OUT"; then
    echo "[!] AVISO: no encuentro la entrada 'multiboot /boot/${KERNEL}' dentro de la imagen."
    echo "    Síntoma típico de esto: 'grub rescue>' o 'error: can't find command multiboot'."
fi
if ! LC_ALL=C grep -qa $'\x02\xb0\xad\x1b' "$OUT"; then
    echo "[!] AVISO: no encuentro el magic Multiboot del kernel dentro de la imagen."
fi

# ------------------------------------------------- zona de persistencia -----
# Crecer la imagen con truncate añade ceros al final. El ISO sigue intacto al
# principio y esa cola es el espacio que usa el diskfs del kernel.
CUR_BYTES=$(stat -c%s "$OUT")
TARGET_BYTES=$((SIZE_MB * 1024 * 1024))
if [ "$TARGET_BYTES" -gt "$CUR_BYTES" ]; then
    echo "[*] Extendiendo a ${SIZE_MB} MiB (zona de persistencia al final)..."
    if command -v truncate >/dev/null 2>&1; then
        truncate -s "${SIZE_MB}M" "$OUT"
    else
        dd if=/dev/zero of="$OUT" bs=1 count=0 seek="$TARGET_BYTES" 2>/dev/null
    fi
elif [ "$TARGET_BYTES" -lt "$CUR_BYTES" ]; then
    echo "[!] AVISO: la imagen base ya ocupa $((CUR_BYTES/1024/1024)) MiB,"
    echo "    más que los ${SIZE_MB} MiB pedidos. No se recorta."
fi

FINAL=$(stat -c%s "$OUT")
echo "[*] Hecho: $OUT  ($((FINAL/1024/1024)) MiB)"
echo
echo "Pruébala en QEMU (arranca Y persiste en el mismo fichero):"
echo "  qemu-system-i386 -drive file=$OUT,format=raw,if=ide -m 512M -serial stdio"
echo
echo "Grábala en USB (CUIDADO: sustituye sdX por tu unidad; la BORRA):"
echo "  sudo dd if=$OUT of=/dev/sdX bs=4M status=progress conv=fsync"
echo
echo "Si al arrancar sale 'grub rescue>', es que GRUB no encuentra su /boot/grub."
echo "Desde ahí:  ls   →  ls (hd0,msdos1)/boot/grub   para localizarlo."
