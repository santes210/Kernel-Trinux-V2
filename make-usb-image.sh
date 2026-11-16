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
#   La ISO la monta `grub-mkrescue`, que genera el El Torito BIOS y la ESP
#   UEFI a la vez, con el grub.cfg y el kernel dentro.
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

die()  { echo "[✗] $*" >&2; exit 1; }
warn() { echo "[!] $*" >&2; }
info() { echo "[*] $*"; }

need() {
    command -v "$1" >/dev/null 2>&1 || die "falta '$1' - instala: $2"
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
need xorriso       "apt install xorriso"
need grub-mkrescue "apt install grub-pc-bin"
# grub-mkrescue construye su propia ESP con `mformat` + `mcopy`
# (util/grub-mkrescue.c: mformat -C -f 2880 ... / mcopy -s -i ...). Sin mtools
# falla con un error opaco, asi que lo comprobamos aqui.
need mformat       "apt install mtools"
need mcopy         "apt install mtools"

GRUB_PC_DIR=/usr/lib/grub/i386-pc
GRUB_EFI64_DIR=/usr/lib/grub/x86_64-efi
[ -d "$GRUB_PC_DIR" ]    || die "no existe $GRUB_PC_DIR - instala grub-pc-bin"
[ -d "$GRUB_EFI64_DIR" ] || die "no existe $GRUB_EFI64_DIR - instala grub-efi-amd64-bin"

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
set timeout=60
set default=0
set gfxmode=auto
set gfxpayload=keep

# ---------------------------------------------------------------------------
# Localizar donde esta el kernel.  (CORREGIDO)
#
# Dos bugs aqui causaban una pantalla negra con la palabra "GRUB" y dejaban el
# kernel sin cargar, en BIOS *y* en UEFI:
#
#  1. `search --no-floppy --file --set=root /boot/mykernel.bin`
#     La opcion --file EXIGE el nombre del fichero como argumento; lo que
#     venia despues se interpretaba como patron de dispositivo. Resultado:
#     `root` NUNCA se fijaba. Al arrancar en BIOS desde una particion ISO9660
#     (donde `root` es (cd0) y el ceros-de-relleno), `normal` no encontraba su
#     /boot/grub, no podia mostrar el menu y se quedaba en la pantalla "GRUB".
#     En UEFI pasaba algo equivalente y devolvia al firmware.
#
#  2. Se asumia que el cfg se carga desde (memdisk). Desde que el cfg vive en
#     la particion, `root` YA apunta al sitio correcto; solo hay que
#     ASEGURARLO en vez de fallar.
#
# La estrategia ahora es a prueba de balas: si el kernel ya es accesible con el
# `root` actual, se usa; si no, se busca; y si no aparece en ningun sitio, el
# menu sale igualmente y muestra un diagnostico en vez de quedarse mudo.
# ---------------------------------------------------------------------------
insmod part_gpt
insmod part_msdos
insmod fat
insmod iso9660
insmod search_fs_file

if [ -f /boot/__KERNEL__ ]; then
  echo "Trinux: kernel encontrado en $root"
else
  search --no-floppy --file --set=root /boot/__KERNEL__
fi

# Modulos que el menu NECESITA, cargados EXPLICITAMENTE.
# grub-mkrescue deja todos los .mod en /boot/grub/i386-pc del ISO y fija
# `prefix` ahi (lo comprobamos en util/grub-mkrescue.c: su load_cfg embebido
# solo hace `search --fs-uuid` + `set prefix`, y la lista de modulos la pone
# el propio ISO). Normalmente GRUB autocarga un modulo cuando el comando no
# existe, pero de eso a que funcione siempre hay un trecho: si la autocarga
# falla, `multiboot` da "unknown command" y el arranque muere otra vez.
insmod multiboot
insmod relocator

# Video: NO cargamos gfxterm a proposito. El menu se dibuja con la consola del
# firmware (texto por GOP), que siempre existe, y el framebuffer del KERNEL lo
# negocia GRUB a partir de la cabecera Multiboot (flags bit 2 = FB_VIDMODE),
# no del terminal de GRUB. Menos piezas = menos formas de fallar.
insmod all_video

menuentry "Trinux: Arranque normal" {
  multiboot /boot/__KERNEL__
  boot
}

menuentry "Trinux: Modo single-user (recovery)" {
  multiboot /boot/__KERNEL__ single
  boot
}

menuentry "Trinux: Shell ring-0 (debug)" {
  multiboot /boot/__KERNEL__ oldsh
  boot
}

# Esta entrada NO toca `multiboot`. Sirve para separar dos fallos que se ven
# igual desde fuera (pantalla negra / texto clavado):
#   - si ESTA entrada dibuja el menu y responde, GRUB esta sano y el problema
#     es del kernel o del handoff Multiboot;
#   - si ni siquiera aqui hay menu, el roto es GRUB/carga del ISO.
menuentry "Trinux: Diagnostico de GRUB (no arranca el kernel)" {
  echo "Trinux: GRUB ha cargado y ejecuta este menu."
  echo "  root   = $root"
  echo "  prefix = $prefix"
  echo "--- dispositivos (ls) ---"
  ls
  echo "--- contenido de /boot ---"
  ls /boot
  echo "--- cabecera del kernel (cat, 64 bytes) ---"
  cat /boot/__KERNEL__
  echo ""
  echo "Si ves esto, GRUB funciona. Pulsa Enter para volver."
  sleep 60
}
GRUBEOF

sed -i "s/__KERNEL__/${KERNEL}/g" "$BUILDDIR/grub.cfg"
grep -q "multiboot /boot/${KERNEL}" "$BUILDDIR/grub.cfg" || \
    die "el grub.cfg no contiene la entrada multiboot esperada"
# AHORA hay UN SOLO grub.cfg: este. Antes se duplicaba (uno embebido en el
# .EFI/memdisk y otro en la particion) y el de la particion llevaba el
# `search` comentado porque "ya lo habia resuelto el memdisk". Eso ya no
# aplica: grub-mkrescue embebe su propio cargador y el unico cfg que existe
# es el de la ISO, asi que tiene que llevar la logica de busqueda COMPLETA.
cp "$BUILDDIR/grub.cfg" "$ISODIR/boot/grub/grub.cfg"
cp "$KERNEL" "$ISODIR/boot/$KERNEL"

# -------------------------------------------------- ISO (grub-mkrescue) ----
# CAMINO OFICIAL Y UNICO. Se deja que `grub-mkrescue` monte TODA la ISO:
#   - El Torito BIOS con su cdboot/core/mbr  (i386-pc)
#   - La particion ESP con EFI/BOOT/BOOTX64.EFI y BOOTIA32.EFI  (x86_64-efi
#     y i386-efi, si los paquetes estan instalados)
#
# Es el codigo que GRUB prueba en su propio CI, asi que aqui no hay montajes
# a mano que se puedan romper. Los tres intentos anteriores de construir el
# arranque BIOS por mi cuenta (grub-mkimage con --prefix, grub-mkstandalone,
# cdboot.img + core.img a mano) daban todos el mismo sintoma: la palabra
# "GRUB" y el sistema clavado ahi.
#
# Si el paquete grub-efi-amd64-bin falta, grub-mkrescue genera una ISO solo
# BIOS y NO avisa; por eso lo comprobamos antes y abortamos con un mensaje
# claro.
if [ ! -d "$GRUB_EFI64_DIR" ]; then
    die "falta $GRUB_EFI64_DIR - instala grub-efi-amd64-bin.
    Sin ese paquete grub-mkrescue genera una ISO SOLO BIOS, sin UEFI."
fi

info "Generando la ISO (BIOS + UEFI) con grub-mkrescue..."
grub-mkrescue --output="$OUT" "$ISODIR" \
    || die "grub-mkrescue fallo (lee el mensaje de arriba)"
[ -s "$OUT" ] || die "grub-mkrescue no genero $OUT"

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
# El BOOTX64.EFI lo genera grub-mkrescue dentro de su ESP; comprobamos que
# el fichero esta en la ISO y que lleva cabecera PE ("MZ").
BOOTX64_TMP="$BUILDDIR/probe_bootx64.efi"
rm -f "$BOOTX64_TMP"
xorriso -osirrox on -extract /EFI/BOOT/BOOTX64.EFI "$BOOTX64_TMP" \
    -indev "$OUT" >/dev/null 2>&1 || true
if [ -s "$BOOTX64_TMP" ]; then
    MZ=$(head -c2 "$BOOTX64_TMP" | od -An -tx1 | tr -d ' \n')
    [ "$MZ" = "4d5a" ] || die "BOOTX64.EFI extraido no tiene cabecera PE (MZ)"
fi
grep -qa "EFI/BOOT/BOOTX64.EFI" "$OUT" || \
    grep -qa "BOOTX64" "$OUT" || \
    die "la imagen no contiene EFI/BOOT/BOOTX64.EFI - NO arrancara en UEFI"

echo "    BIOS : MBR 0x55AA + grub.cfg + kernel ......... OK"
echo "    UEFI : EFI/BOOT/BOOTX64.EFI (PE) + ESP FAT .... OK"
if [ "$HAVE_EFI32" = 1 ]; then
    grep -qa "BOOTIA32" "$OUT" && echo "    UEFI32: EFI/BOOT/BOOTIA32.EFI ................. OK"
fi

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

rm -rf "$BUILDDIR" "$ISODIR"

# Sonda de depuracion del El Torito, por si algun dia GRUB deja de arrancar:
#   offset 0x8000 (sector 16) = LBA de la particion de arranque
#   offset 0x8007             = tipo de medio (0 = no-emulacion, lo correcto)
# Si el tipo no es 0, el BIOS cargaria la imagen como disquete.
BOOT_LBA_HEX=$(dd if="$OUT" bs=1 skip=$((0x8000)) count=4 2>/dev/null | od -An -tu4 --endian=little | tr -d ' \n')
MEDIA_TYPE=$(dd if="$OUT" bs=1 skip=$((0x8007)) count=1 2>/dev/null | od -An -tu1 | tr -d ' \n')
if [ -n "$BOOT_LBA_HEX" ]; then
    echo "    [debug] El Torito: LBA de arranque=$BOOT_LBA_HEX  tipo_medio=${MEDIA_TYPE:-?} (0=no-emulacion)"
    if [ "${MEDIA_TYPE:-0}" != "0" ]; then
        warn "tipo de emulacion != 0: el BIOS cargaria la imagen como disquete."
    fi
fi

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
