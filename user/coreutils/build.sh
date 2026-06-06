#!/usr/bin/env bash
# Compila todos los *.c en programas ELF32 ring-3, los embebe como .h
# con xxd -i para que el kernel los pueda dropear en /bin al boot.
set -e
cd "$(dirname "$0")"

CC="${CC:-gcc}"
CFLAGS="-m32 -ffreestanding -nostdlib -static -fno-pic -fno-pie -fno-stack-protector \
        -fno-asynchronous-unwind-tables -Os -Wall -Wno-unused-parameter -I.."
LD="ld -m elf_i386 -T user.ld -nostdlib --build-id=none"

OUT="bin"
HDRS="hdrs"
mkdir -p "$OUT" "$HDRS"

# crt0 una sola vez
$CC $CFLAGS -c crt0.S -o "$OUT/crt0.o"

# Mapeo: nombre-de-binario  archivo-fuente
declare -A PROGS=(
  [echo]=echo.c
  [cat]=cat.c
  [ls]=ls.c
  [pwd]=pwd.c
  [whoami]=whoami.c
  [hostname]=hostname_u.c
  [uname]=uname.c
  [uptime]=uptime_u.c
  [clear]=clear.c
  [mkdir]=mkdir_u.c
  [rmdir]=rmdir_u.c
  [rm]=rm_u.c
  [touch]=touch_u.c
  [wc]=wc_u.c
  [head]=head_u.c
  [tail]=tail_u.c
  [grep]=grep_u.c
  [sleep]=sleep_u.c
  [yes]=yes_u.c
  [true]=true_u.c
  [false]=false_u.c
  [id]=id_u.c
  [reboot]=reboot_u.c
  [shutdown]=shutdown_u.c
  [sync]=sync_u.c
  [kill]=kill_u.c
  [ringtest]=ringtest.c
)

for name in "${!PROGS[@]}"; do
    src="${PROGS[$name]}"
    $CC $CFLAGS -c "$src" -o "$OUT/$name.o"
    $LD "$OUT/crt0.o" "$OUT/$name.o" -o "$OUT/$name"
    # Genera el header embebido
    (cd "$OUT" && xxd -i -n "u_${name}" "$name") > "$HDRS/${name}.h"
    sz=$(stat -c%s "$OUT/$name")
    echo "  [OK] $name  ($sz bytes)"
done

# Genera el índice global con un array { nombre, ptr, size }
{
  echo "/* AUTO-GENERATED por user/coreutils/build.sh — no editar. */"
  echo "#ifndef USER_BIN_TABLE_H"
  echo "#define USER_BIN_TABLE_H"
  echo
  for name in "${!PROGS[@]}"; do
      echo "#include \"hdrs/${name}.h\""
  done
  echo
  echo "typedef struct { const char *name; unsigned char *data; unsigned int *size_p; } user_bin_t;"
  echo "static user_bin_t user_bins[] = {"
  for name in "${!PROGS[@]}"; do
      echo "    { \"$name\", u_${name}, &u_${name}_len },"
  done
  echo "};"
  echo "#define user_bins_count (sizeof(user_bins)/sizeof(user_bins[0]))"
  echo "#endif"
} > user_bins.h

echo
echo "Total: ${#PROGS[@]} programas en bin/, header generado en user_bins.h"
