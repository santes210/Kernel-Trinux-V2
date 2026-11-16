#!/usr/bin/env bash
set -e
cd "$(dirname "$0")"
CC="${CC:-gcc}"

# xxd se usa para convertir cada ELF en un header .h (array de bytes).
# En esta máquina minimalista no existe, y antes el `xxd -i` fallaba en
# silencio dejando headers vacíos. Se usa un fallback en Python.
if command -v xxd >/dev/null 2>&1; then
    have_xxd=1
else
    have_xxd=0
fi
pybin() { python3 "$@"; }
xxd_i() {   # xxd_i <file> <varname>  -> escribe el .h por stdout
    local f="$1" n="$2"
    if [ "$have_xxd" = 1 ]; then
        xxd -i -n "$n" "$f"
    else
        pybin - "$f" "$n" <<'PYEOF'
import sys
path, name = sys.argv[1], sys.argv[2]
data = open(path, 'rb').read()
out = ["unsigned char %s[] = {" % name]
line = []
for i, b in enumerate(data):
    line.append("0x%02x" % b)
    if len(line) == 12:
        out.append("  " + ", ".join(line) + ",")
        line = []
if line:
    out.append("  " + ", ".join(line) + ",")
out.append("};")
out.append("unsigned int %s_len = %d;" % (name, len(data)))
print("\n".join(out))
PYEOF
    fi
}
CFLAGS="-m32 -ffreestanding -nostdlib -static -fno-pic -fno-pie -fno-stack-protector \
        -fno-asynchronous-unwind-tables -Os -Wall -Wno-unused-parameter -I.."
LD="ld -m elf_i386 -T user.ld -nostdlib --build-id=none"
OUT="bin"; HDRS="hdrs"
mkdir -p "$OUT" "$HDRS"
$CC $CFLAGS -c crt0.S -o "$OUT/crt0.o"

declare -A PROGS=(
  [echo]=echo.c [cat]=cat.c [ls]=ls.c [pwd]=pwd.c
  [whoami]=whoami.c [hostname]=hostname_u.c [uname]=uname.c
  [uptime]=uptime_u.c [clear]=clear.c
  [mkdir]=mkdir_u.c [rmdir]=rmdir_u.c [rm]=rm_u.c [touch]=touch_u.c
  [wc]=wc_u.c [head]=head_u.c [tail]=tail_u.c [grep]=grep_u.c
  [sleep]=sleep_u.c [yes]=yes_u.c [true]=true_u.c [false]=false_u.c
  [id]=id_u.c [reboot]=reboot_u.c [shutdown]=shutdown_u.c
  [sync]=sync_u.c [kill]=kill_u.c [ringtest]=ringtest.c
  [execvetest]=execvetest_u.c [brktest]=brktest_u.c
  [cp]=cp_u.c [mv]=mv_u.c [stat]=stat_u.c
  [chmod]=chmod_u.c [chown]=chown_u.c
  [date]=date_u.c [free]=free_u.c [df]=df_u.c
  [users]=users_u.c [groups]=groups_u.c [logout]=logout_u.c
  [su]=su_u.c [useradd]=useradd_u.c [passwd]=passwd_u.c
  [ps]=ps_u.c [renice]=renice_u.c [battery]=battery_u_cmd.c
  [tree]=tree_u.c [find]=find_u.c
  [sort]=sort_u.c [uniq]=uniq_u.c [cut]=cut_u.c [tee]=tee_u.c
  [seq]=seq_u.c [basename]=basename_u.c [dirname]=dirname_u.c
  [which]=which_u.c [env]=env_u.c
  [calc]=calc_u.c [hexdump]=hexdump_u.c [neofetch]=neofetch_u.c
  [write]=write_u.c [color]=color_u.c [halt]=halt_u.c
  [lscpu]=lscpu_u.c [screeninfo]=screeninfo_u.c [sysinfo]=sysinfo_u.c
  [top]=top_u.c [edit]=edit_u.c [tcc]=tcc_u.c \
  [cide]=cide_u.c [tar]=tar_u.c [ping]=ping_u.c
)
declare -A ALIASES=([cls]=clear [nano]=edit)

for name in "${!PROGS[@]}"; do
    src="${PROGS[$name]}"
    if ! $CC $CFLAGS -c "$src" -o "$OUT/${name}.o"; then
        echo "  ⚠️  $name: fallo al compilar $src" >&2
        continue
    fi
    if ! $LD "$OUT/crt0.o" "$OUT/${name}.o" -o "$OUT/${name}"; then
        echo "  ⚠️  $name: fallo al enlazar" >&2
        continue
    fi
    xxd_i "$OUT/${name}" "u_${name}" > "$HDRS/${name}.h"
done
BUILT=0
for name in "${!PROGS[@]}"; do
    [ -f "$OUT/${name}" ] && [ -f "$HDRS/${name}.h" ] && BUILT=$((BUILT+1))
done
echo "Compilados $BUILT de ${#PROGS[@]} programas"
if [ "$BUILT" -ne "${#PROGS[@]}" ]; then
    echo "⚠️  Ojo: algún programa no compiló (ver avisos arriba)." >&2
fi

{
  echo "/* AUTO-GENERATED. */"
  echo "#ifndef USER_BIN_TABLE_H"
  echo "#define USER_BIN_TABLE_H"
  for name in $(printf '%s\n' "${!PROGS[@]}" | sort); do
    [ -f "$HDRS/${name}.h" ] || continue
    echo "#include \"hdrs/${name}.h\""
  done
  echo "typedef struct { const char *name; unsigned char *data; unsigned int *size_p; } user_bin_t;"
  echo "static user_bin_t user_bins[] = {"
  for name in $(printf '%s\n' "${!PROGS[@]}" | sort); do
    [ -f "$HDRS/${name}.h" ] || continue
    echo "    { \"$name\", u_${name}, &u_${name}_len },"
  done
  for a in "${!ALIASES[@]}"; do tgt="${ALIASES[$a]}"; echo "    { \"$a\", u_${tgt}, &u_${tgt}_len },"; done
  echo "};"
  echo "#define user_bins_count (sizeof(user_bins)/sizeof(user_bins[0]))"
  echo "#endif"
} > user_bins.h

SH_SRC="../usersh/sh.c"
if [ -f "$SH_SRC" ]; then
    $CC $CFLAGS -c "$SH_SRC" -o "$OUT/sh.o" 2>/dev/null
    $LD "$OUT/crt0.o" "$OUT/sh.o" -o "$OUT/sh" 2>/dev/null
    xxd_i "$OUT/sh" "u_sh" > "../usersh/sh_elf.h"
    echo "Shell ring-3: $(stat -c%s "$OUT/sh") bytes"
fi
echo "Total: ${#PROGS[@]} binarios + ${#ALIASES[@]} alias"
