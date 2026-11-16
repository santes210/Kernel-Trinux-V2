#!/usr/bin/env bash
# ============================================================================
# scripts/smoke_test.sh — arranca mykernel.bin en QEMU y valida el boot.
#
# Motivación: el CI original solo comprobaba `stat -c%s mykernel.bin > 100 KiB`.
# Un kernel que enlazaba pero hacía panic al inicializar el scheduler (o que se
# quedaba colgado en `sti; hlt`) pasaba en verde.  Esto arranca el kernel de
# verdad, captura la salida de COM1 y exige que la secuencia de arranque
# llegue hasta el shell en ring 3.
#
# Uso:
#   bash scripts/smoke_test.sh                 # construye si hace falta
#   TIMEOUT=45 bash scripts/smoke_test.sh     # más margen en runners lentos
#   KERNEL=mykernel.bin bash scripts/smoke_test.sh
#
# Salida: boot.log (todo) y boot-serial.log (solo COM1).
# Código de salida: 0 si todas las comprobaciones pasan, 1 si alguna falla.
# ============================================================================
set -uo pipefail

KERNEL="${KERNEL:-mykernel.bin}"
TIMEOUT="${TIMEOUT:-30}"
LOG="boot.log"
SERIAL="boot-serial.log"

fail=0
note() { printf '%s\n' "$*"; }
ok()   { printf '  \033[32m[ OK ]\033[0m %s\n' "$*"; }
bad()  { printf '  \033[31m[FAIL]\033[0m %s\n' "$*"; fail=1; }

# ---- 1. Artefacto -----------------------------------------------------------
if [ ! -f "$KERNEL" ]; then
    note "== $KERNEL no existe: compilando =="
    make -j"$(nproc 2>/dev/null || echo 2)" || { bad "make falló"; exit 1; }
fi
[ -f "$KERNEL" ] || { bad "$KERNEL no existe tras make"; exit 1; }
ok "$KERNEL presente ($(( $(stat -c%s "$KERNEL") / 1024 )) KiB)"

# ---- 2. QEMU -----------------------------------------------------------------
if ! command -v qemu-system-i386 >/dev/null 2>&1; then
    note "qemu-system-i386 no está instalado."
    note "  Debian/Ubuntu: sudo apt-get install qemu-system-x86"
    note "  (el CI lo instala antes de llamar a este script)"
    note "Se omiten las comprobaciones de arranque."
    exit 0
fi

# ---- 3. Arranque ------------------------------------------------------------
# -kernel: el binario es ELF Multiboot, así que QEMU lo carga directamente.
# -display none + -serial file: sin ventana y con la salida a un archivo.
# -no-reboot: si el kernel hace triple fault, QEMU sale en vez de reiniciar.
note "== Arrancando $KERNEL en QEMU (timeout ${TIMEOUT}s) =="
timeout --foreground "$TIMEOUT" \
    qemu-system-i386 \
        -kernel "$KERNEL" \
        -m 512M \
        -display none \
        -serial "file:$SERIAL" \
        -no-reboot \
        -nic none \
        >"$LOG" 2>&1
rc=$?
# timeout devuelve 124 cuando el kernel sigue vivo al agotarse el tiempo: es lo
# esperado (el sistema queda esperando en el prompt del shell).
if [ "$rc" -ne 0 ] && [ "$rc" -ne 124 ]; then
    bad "QEMU salió con código $rc (¿triple fault?)"
    tail -30 "$LOG"
fi

# La salida del kernel va por COM1; si el archivo está vacío puede que el
# binario haya arrancado sin inicializar el serie, así que también se mira
# la salida estándar de QEMU.
OUT="$(cat "$SERIAL" 2>/dev/null)$(cat "$LOG" 2>/dev/null)"
[ -n "$OUT" ] || { bad "sin salida del kernel"; exit 1; }
printf '%s\n' "$OUT" > boot-full.log

# ---- 4. Comprobaciones ------------------------------------------------------
need() {
    if printf '%s' "$OUT" | grep -qF -- "$1"; then ok "$2"; else bad "$2 ( falta: '$1' )"; fi
}
need 'IDT installed'                 'IDT instalada'
need 'Paging enabled'                'paginación activa'
need 'Physical memory manager'       'PMM inicializado'
need 'Process table + scheduler'     'tabla de procesos + scheduler'
need 'Interrupts enabled'            'interrupciones habilitadas'
need 'RAM filesystem mounted'        'VFS montado'
need 'Built-in apps installed'       '/bin poblado'
need 'launching /bin/sh in RING 3'   'salto a ring 3 (/bin/sh)'
need 'jumping to ring 3 at'          'entrada real en el usuario'

# Anti-regresión: un panic o un page fault en ring 0 no deben aparecer NUNCA.
if printf '%s' "$OUT" | grep -qiE 'PANIC|PAGE FAULT EN KERNEL|triple fault'; then
    bad 'panic / page fault en el kernel'
    printf '%s\n' "$OUT" | grep -iE -A4 'PANIC|PAGE FAULT|triple fault' | head -20
else
    ok 'sin panics ni #PF en ring 0'
fi

# Cuenta de [ OK ] como sanity check de que se llegó al final del arranque.
n_ok=$(printf '%s' "$OUT" | grep -cF '[ OK ]')
if [ "$n_ok" -ge 15 ]; then
    ok "$n_ok subsistemas reportan [ OK ]"
else
    bad "solo $n_ok subsistemas [ OK ] (se esperaban >= 15)"
fi

echo
if [ "$fail" -ne 0 ]; then
    echo "SMOKE TEST: FALLÓ  (log completo en boot-full.log)"
    exit 1
fi
echo "SMOKE TEST: OK  (log completo en boot-full.log)"
