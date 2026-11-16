#!/usr/bin/env bash
# ============================================================================
# make-usb-image.sh  -  imagen arrancable en BIOS *y* UEFI, con persistencia.
#
# Genera mykernel-usb.img, un medio con DOS caminos de arranque:
#
#   BIOS / CSM  ->  MBR de GRUB (i386-pc)  ->  menus de `boot/grub/grub.cfg`
#   UEFI        ->  ESP FAT con EFI/BOOT/BOOTX64.EFI (x86_64-efi) y BOOTIA32.EFI
#
# Ambas rutas cargan el mismo kernel Multiboot v1 de 32 bits.
#
# COMO
#   El GRUB EFI va EMBEBIDO como `grub-mkstandalone`, con el grub.cfg y los
#   modulos dentro del propio .EFI. Asi no depende de encontrar su $prefix en
#   el disco: el fallo clasico `grub rescue>` desaparece, porque no hay nada
#   que buscar. La ISO se construye con `xorriso`, que es el unico camino que
#   permite un ESP >= 34 MiB (el ESP por defecto de grub-mkrescue es de 10 MiB
#   y se queda corto).
#
# POR QUE NO `grub-mkrescue` A SECAS
#   1. En Debian, grub-mkrescue SIN `grub-efi-amd64-bin` genera una ISO solo
#      BIOS y no avisa. Sintoma: en una maquina UEFI pura, no arranca.
#   2. Su ESP por defecto (10 MiB) alberga mal el GRUB EFI completo.
#   3. Nadie comprobaba el resultado: se publicaba una imagen rota.
#
# Uso:
#   ./make-usb-image.sh [SIZE_MB]        (por defecto 512)
#
# Requisitos (Debian/Ubuntu):
#   sudo apt install grub-pc-bin grub-efi-amd64-bin grub-efi-ia32-bin \
#                    xorriso mtools dosfstools
# ============================================================================
set -euo pipefail

SIZE_MB="${1:-512}"
KERNEL="mykernel.bin"
# Nombre de la imagen. Se puede fijar con USB_OUT (lo usa el CI para que el
# fichero se llame trinux-usb-512.img desde el principio). Antes el workflow
# esperaba ese nombre pero el script escribia mykernel-usb.img y el `ls` del
# paso siguiente fallaba con "No such file or directory".
OUT="${USB_OUT:-mykernel-usb.img}"
ISODIR="iso"
BUILDDIR=".usb-build"

# Reserva de arranque del kernel: DEBE coincidir con BOOT_RESERVE_SECTORS de
# fs/blockfs.c y DISKFS_BOOT_RESERVE_SECTORS de fs/diskfs.c. El kernel no
# escribe por debajo de aqui, asi que todo lo que pongamos al principio
# (MBR + ESP + kernel) esta a salvo.
BOOT_RESERVE_BYTES=$((48 * 1024 * 1024))
# Tamano del ESP. GRUB EFI x86_64 completo + modulos ~34 MiB; 48 da aire.
ESP_BYTES=$((48 * 1024 * 1024))

die()  { echo "[✗] $*" >&2; exit 1; }
warn() { echo "[!] $*" >&2; }
info() { echo "[*] $*"; }

need() {
    command -v "$1" >/dev/null 2>&1 || die "falta '$1' — instala: $2"
}

# ---------------------------------------------------------------- kernel ----
if [ ! -f "$KERNEL" ]; then
    info "$KERNEL no existe, construyendo..."
    make
fi
[ -s "$KERNEL" ] || die "no se pudo construir $KERNEL"

# Multiboot v1 exige el magic 0x1BADB002 en los primeros 8 KiB. Sin el, GRUB
# no reconoce el fichero como kernel y el menu no sirve de nada.
if ! LC_ALL=C grep -qa $'\x02\xb0\xad\x1b' "$KERNEL"; then
    die "no encuentro el magic Multiboot (0x1BADB002) en $KERNEL.
    GRUB no lo reconocera como kernel Multiboot v1."
fi

# ----------------------------------------------------------- herramientas ----
need xorriso    "apt install xorriso"
need grub-mkstandalone "apt install grub-pc-bin"
need mkfs.vfat  "apt install dosfstools"
need mcopy      "apt install mtools"

GRUB_PC_DIR=/usr/lib/grub/i386-pc
GRUB_EFI64_DIR=/usr/lib/grub/x86_64-efi
[ -d "$GRUB_PC_DIR" ]    || die "no existe $GRUB_PC_DIR — instala grub-pc-bin"
[ -d "$GRUB_EFI64_DIR" ] || die "no existe $GRUB_EFI64_DIR — instala grub-efi-amd64-bin"

# Soporte UEFI de 32 bits: opcional (solo para maquinas EFI-IA32 antiguas,
# tipo algunos Atom/TabletPC). Avisamos pero no abortamos.
GRUB_EFI32_DIR=/usr/lib/grub/i386-efi
HAVE_EFI32=0
if [ -d "$GRUB_EFI32_DIR" ]; then
    HAVE_EFI32=1
else
    warn "sin $GRUB_EFI32_DIR (grub-efi-ia32-bin): la imagen no arrancara en UEFI-32."
    warn "  Para equipos modernos no hace falta, pero si quieres cubrirlo:"
    warn "  sudo apt install grub-efi-ia32-bin"
fi

# ------------------------------------------------------------- grub.cfg ----
# Un solo fichero para los dos caminos. Las entradas usan la ruta /boot/...
# porque el ISO expone su raiz como (cd0) y la ESP como su propio arbol.
rm -rf "$BUILDDIR" "$ISODIR"
mkdir -p "$BUILDDIR" "$ISODIR/boot/grub"

cat > "$BUILDDIR/grub.cfg" <<'GRUBEOF'
set timeout=5
set default=0
set gfxmode=auto
set gfxpayload=keep

# ---------------------------------------------------------------------------
# Localizar donde esta el kernel.
#
# Esto es CRITICO y es el fallo tipico de grub-mkstandalone: dentro del .EFI el
# `root` inicial es (memdisk), asi que `multiboot /boot/mykernel.bin` buscaria
# (memdisk)/boot/mykernel.bin y daria "error: file not found". Buscamos la
# particion que de verdad lo contiene: la ESP en UEFI, el ISO en BIOS.
# ---------------------------------------------------------------------------
insmod part_gpt
insmod part_msdos
insmod fat
insmod iso9660
insmod search_fs_file
search --no-floppy --file --set=root /boot/__KERNEL__

# Video: NO cargamos gfxterm a proposito. El menu se dibuja con la consola del
# firmware (texto por GOP), que siempre existe, y el framebuffer del KERNEL lo
# negocia GRUB a partir de la cabecera Multiboot (flags bit 2 = FB_VIDMODE),
# no del terminal de GRUB. Menos piezas = menos formas de fallar.
insmod all_video

menuentry "Trinux — Arranque normal" {
  multiboot /boot/__KERNEL__
  boot
}

menuentry "Trinux — Modo single-user (recovery)" {
  multiboot /boot/__KERNEL__ single
  boot
}

menuentry "Trinux — Shell ring-0 (debug)" {
  multiboot /boot/__KERNEL__ oldsh
  boot
}
GRUBEOF

sed -i "s/__KERNEL__/${KERNEL}/g" "$BUILDDIR/grub.cfg"
grep -q "multiboot /boot/${KERNEL}" "$BUILDDIR/grub.cfg" || \
    die "el grub.cfg no contiene la entrada multiboot esperada"
cp "$BUILDDIR/grub.cfg" "$ISODIR/boot/grub/grub.cfg"
cp "$KERNEL" "$ISODIR/boot/$KERNEL"

# ------------------------------------------------------ GRUB EFI embebido ----
# `grub-mkstandalone` mete el grub.cfg DENTRO del .EFI. El prefijo apunta a la
# ESP, y el cfg va embebido como (memdisk)/boot/grub/grub.cfg — con `insmod`
# de todo lo necesario para leer ISO9660/FAT y ejecutar multiboot.
info "Construyendo GRUB EFI x86_64 (standalone, con gfxterm y multiboot)..."
grub-mkstandalone \
    --format=x86_64-efi \
    --output="$BUILDDIR/BOOTX64.EFI" \
    --modules="part_gpt part_msdos fat iso9660 normal multiboot relocator \
               all_video efi_gop serial search search_fs_file \
               configfile echo test" \
    --locales="" --themes="" --fonts="" \
    "boot/grub/grub.cfg=$BUILDDIR/grub.cfg" \
    >/dev/null || die "grub-mkstandalone (x86_64-efi) fallo"
[ -s "$BUILDDIR/BOOTX64.EFI" ] || die "BOOTX64.EFI vacio"

if [ "$HAVE_EFI32" = 1 ]; then
    info "Construyendo GRUB EFI i386 (para UEFI de 32 bits, opcional)..."
    grub-mkstandalone \
        --format=i386-efi \
        --output="$BUILDDIR/BOOTIA32.EFI" \
        --modules="part_gpt part_msdos fat iso9660 normal multiboot relocator \
                   all_video efi_gop serial search search_fs_file \
                   configfile echo test" \
        --locales="" --themes="" --fonts="" \
        "boot/grub/grub.cfg=$BUILDDIR/grub.cfg" \
        >/dev/null || warn "grub-mkstandalone (i386-efi) fallo; sigo sin UEFI-32"
fi

# ------------------------------------------------------------ ESP (FAT) ----
# El firmware UEFI busca \EFI\BOOT\BOOTX64.EFI en una particion FAT. La
# creamos como fichero suelto; xorriso la injerta como particion y la marca
# como arrancable por EFI.
info "Creando ESP FAT de $((ESP_BYTES / 1024 / 1024)) MiB..."
dd if=/dev/zero of="$BUILDDIR/efiboot.img" bs=1M count=$((ESP_BYTES / 1024 / 1024)) status=none
mkfs.vfat -F 32 -n TRINUXESP "$BUILDDIR/efiboot.img" >/dev/null

export MTOOLS_SKIP_CHECK=1
mmd -i "$BUILDDIR/efiboot.img" ::/EFI ::/EFI/BOOT ::/boot
mcopy -i "$BUILDDIR/efiboot.img" "$BUILDDIR/BOOTX64.EFI" ::/EFI/BOOT/BOOTX64.EFI
if [ -f "$BUILDDIR/BOOTIA32.EFI" ]; then
    mcopy -i "$BUILDDIR/efiboot.img" "$BUILDDIR/BOOTIA32.EFI" ::/EFI/BOOT/BOOTIA32.EFI
fi
# Una copia del kernel y del cfg dentro de la ESP: asi el GRUB EFI tambien
# puede cargar desde ahi sin depender de leer el ISO9660.
mcopy -i "$BUILDDIR/efiboot.img" "$KERNEL" ::/boot/"$KERNEL"
mcopy -i "$BUILDDIR/efiboot.img" "$BUILDDIR/grub.cfg" ::/boot/grub.cfg

# ------------------------------------------------- imagen BIOS arrancable ----
# Camino BIOS: se construye un core.img de GRUB (i386-pc) y se le antepone
# cdboot.img, que es el cargador El Torito. Ese fichero es el que va en
# -eltorito-boot.
#
# ANTES ESTO ESTABA MAL: se llamaba a `grub-mkrescue` para "generar la parte
# BIOS" y luego xorriso apuntaba a un boot/grub/bios.img que NADIE creaba. Es
# decir, el comentario decia "extraemos boot.img + core.img" pero esa parte no
# estaba implementada -> xorriso fallaba siempre con "fallo al ensamblar".
CDBOOT="$GRUB_PC_DIR/cdboot.img"
[ -f "$CDBOOT" ] || die "no existe $CDBOOT — instala grub-pc-bin"

info "Generando el arranque BIOS (i386-pc: core.img + cdboot.img)..."
grub-mkimage \
    --format=i386-pc \
    --output="$BUILDDIR/core.img" \
    --prefix=/boot/grub \
    --compress=xz \
    biosdisk iso9660 part_msdos part_gpt fat normal multiboot relocator \
    search search_fs_file configfile echo test \
    >/dev/null 2>&1 || die "grub-mkimage (i386-pc) fallo"
[ -s "$BUILDDIR/core.img" ] || die "grub-mkimage no genero core.img"

# cdboot.img debe ir DELANTE: es el que la BIOS ejecuta en modo no-emulacion.
cat "$CDBOOT" "$BUILDDIR/core.img" > "$BUILDDIR/bios.img"
[ -s "$BUILDDIR/bios.img" ] || die "no se pudo montar bios.img"

# El El Torito lo referencia por ruta DENTRO del ISO, asi que tiene que estar
# en el arbol que se le pasa a xorriso.
cp "$BUILDDIR/bios.img" "$ISODIR/boot/grub/bios.img"

# ------------------------------------------------------------ ISO final ----
info "Ensamblando ISO hibrida (BIOS + UEFI) con ESP de $((ESP_BYTES/1024/1024)) MiB..."

# NOTA sobre --grub2-mbr: se usa boot_hybrid.img si existe y, si no,
# boot.img. Las dos son validas como MBR hibrido; los nombres cambian entre
# versiones de GRUB y no queremos depender de uno concreto.
HYBRID_MBR="$GRUB_PC_DIR/boot_hybrid.img"
[ -f "$HYBRID_MBR" ] || HYBRID_MBR="$GRUB_PC_DIR/boot.img"
[ -f "$HYBRID_MBR" ] || die "no encuentro boot_hybrid.img ni boot.img en $GRUB_PC_DIR"

# Sin >/dev/null: si xorriso falla, queremos ver POR QUE.
xorriso -as mkisofs \
    -iso-level 3 -full-iso9660-filenames -volid "TRINUX" \
    -eltorito-boot boot/grub/bios.img \
    -no-emul-boot -boot-load-size 4 -boot-info-table \
    -eltorito-alt-boot \
    -e --interval:appended_partition_2:all:: \
    -no-emul-boot \
    -isohybrid-gpt-basdat \
    -append_partition 2 0xef "$BUILDDIR/efiboot.img" \
    --grub2-mbr "$HYBRID_MBR" \
    -output "$OUT" \
    -graft-points \
      "$ISODIR" \
    || die "xorriso fallo al ensamblar la ISO (lee el mensaje de arriba)"

[ -s "$OUT" ] || die "xorriso no genero $OUT"

# ------------------------------------------------------- verificaciones ----
info "Verificando la imagen..."

# 1. Firma MBR: sin 0x55AA la BIOS no arranca el medio.
SIG=$(dd if="$OUT" bs=1 skip=510 count=2 2>/dev/null | od -An -tx1 | tr -d ' \n')
[ "$SIG" = "55aa" ] || die "el sector 0 no tiene firma 0x55AA (no arrancara en BIOS)"

# 2. El grub.cfg y el kernel tienen que estar DENTRO.
grep -qa "multiboot /boot/${KERNEL}" "$OUT" || \
    die "la imagen no contiene la entrada multiboot del grub.cfg"
grep -qa $'\x02\xb0\xad\x1b' "$OUT" || \
    die "la imagen no contiene el kernel con magic Multiboot"

# 3. El camino UEFI: firma PE ("MZ") en el .EFI y el nombre que busca el
#    firmware. Es LA comprobacion que faltaba y la causa de que en UEFI no
#    arrancara: sin BOOTX64.EFI no hay nada que ejecutar.
MZ=$(head -c2 "$BUILDDIR/BOOTX64.EFI" | od -An -tx1 | tr -d ' \n')
[ "$MZ" = "4d5a" ] || die "BOOTX64.EFI no tiene cabecera PE (MZ)"
grep -qa "EFI/BOOT/BOOTX64.EFI" "$OUT" || \
    grep -qa "BOOTX64" "$OUT" || \
    die "la imagen no contiene EFI/BOOT/BOOTX64.EFI — NO arrancara en UEFI"

echo "    BIOS : MBR 0x55AA + grub.cfg + kernel ......... OK"
echo "    UEFI : EFI/BOOT/BOOTX64.EFI (PE) + ESP FAT .... OK"
[ "$HAVE_EFI32" = 1 ] && echo "    UEFI32: EFI/BOOT/BOOTIA32.EFI ................. OK"

# Copia opcional del ISO SIN el relleno de persistencia. Asi el CI publica una
# ISO y una imagen USB generadas por el MISMO codigo, y no pueden divergir
# (que es como se colo antes una imagen que no arrancaba).
if [ -n "${ISO_OUT:-}" ]; then
    # OJO: la copia debe hacerse ANTES de extender con truncate, y ademas no
    # puede caer "dentro" de $OUT. Si ISO_OUT llega como ruta relativa (p.ej.
    # "trinux-usb.img") y OUT ya vale lo mismo, el cp daria "son el mismo
    # fichero" o, peor, el truncate posterior cortaria el ISO. Se compara la
    # ruta ya resuelta y se descarta la copia en ese caso (no es necesaria:
    # el .img empieza siendo exactamente el ISO).
    out_abs=$(cd "$(dirname "$OUT")" && pwd)/$(basename "$OUT")
    iso_abs=$(cd "$(dirname "$ISO_OUT")" && pwd)/$(basename "$ISO_OUT")
    if [ "$out_abs" = "$iso_abs" ]; then
        warn "ISO_OUT apunta al mismo fichero que la imagen; se omite la copia."
    else
        cp "$OUT" "$ISO_OUT"
        info "ISO copiada a $ISO_OUT ($(stat -c%s "$ISO_OUT") bytes)"
    fi
fi

# ---------------------------------------------- zona de persistencia -------
# Crecer la imagen con ceros al final NO rompe la ISO (que sigue al principio)
# y es el espacio que usa el diskfs del kernel. La reserva de 48 MiB del kernel
# deja libres la ESP y el kernel; el resto se usa para datos.
CUR_BYTES=$(stat -c%s "$OUT")
TARGET_BYTES=$((SIZE_MB * 1024 * 1024))
if [ "$TARGET_BYTES" -gt "$CUR_BYTES" ]; then
    info "Extendiendo a ${SIZE_MB} MiB (zona de persistencia al final)..."
    truncate -s "${SIZE_MB}M" "$OUT"
elif [ "$TARGET_BYTES" -lt "$CUR_BYTES" ]; then
    warn "la imagen base ya ocupa $((CUR_BYTES/1024/1024)) MiB; no se recorta a ${SIZE_MB}"
    SIZE_MB=$(( (CUR_BYTES + 1024*1024 - 1) / 1024 / 1024 ))
fi

# La persistencia necesita 48 (reserva) + 64 (cola diskfs) + datos.
MIN_FOR_PERSIST=$(( (BOOT_RESERVE_BYTES + 64*1024*1024) / 1024 / 1024 + 16 ))
if [ "$SIZE_MB" -lt "$MIN_FOR_PERSIST" ]; then
    warn "con ${SIZE_MB} MiB la persistencia queda desactivada (minimo ~${MIN_FOR_PERSIST} MiB)."
    warn "  El kernel arrancara igual, pero los ficheros se quedaran en RAM."
fi

rm -rf "$BUILDDIR"

FINAL=$(stat -c%s "$OUT")
echo
info "Hecho: $OUT  ($((FINAL/1024/1024)) MiB)"
echo
echo "Pruébala en QEMU:"
echo "  BIOS: qemu-system-i386  -drive file=$OUT,format=raw -m 512M -serial stdio"
echo "  UEFI: qemu-system-x86_64 -bios /usr/share/OVMF/OVMF_CODE.fd \\"
echo "          -drive file=$OUT,format=raw -m 512M -serial stdio"
echo
echo "Grábala en USB (CUIDADO: sustituye sdX; la BORRA):"
echo "  sudo dd if=$OUT of=/dev/sdX bs=4M status=progress conv=fsync && sync"
echo
echo "IMPORTANTE en UEFI: desactiva Secure Boot en la BIOS/UEFI."
echo "  El modulo 'multiboot' de GRUB no esta firmado, y con Secure Boot activo"
echo "  el firmware se niega a cargarlo (CVE-2015-5281). Si no lo desactivas,"
echo "  veras 'prohibited by secure boot policy' o una pantalla que vuelve al menu."
echo
echo "Si sale 'grub rescue>': GRUB arranco pero no encontro su /boot/grub."
echo "  Desde ahi:  ls   →  ls (hd0,msdos1)/boot/grub"
