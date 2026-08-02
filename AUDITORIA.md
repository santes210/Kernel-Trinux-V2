# 🔍 Auditoría completa del código — Kernel Trinux V2

**Fecha**: 2026-07-30 · **Alcance**: todo el repositorio (~60.700 líneas, 272 archivos C/ASM/H)
**Resultado**: 12 correcciones aplicadas (v0.5.3) + hoja de ruta de lo que le falta al kernel.

---

## 1. Lo que el kernel YA tiene (inventario)

| Subsistema | Estado | Archivos |
|---|---|---|
| Boot Multiboot v1 + petición de framebuffer VBE | ✅ Completo | `boot/boot.asm`, `linker.ld` |
| GDT + TSS (soporte ring 3 real) | ✅ Completo | `boot/gdt.c`, `gdt_flush.asm` |
| IDT + ISRs (excepciones 0-31) + PIC/IRQs | ✅ Completo | `cpu/idt.c`, `isr.c`, `irq.c` |
| Syscalls int 0x80 con ABI 3.x (73 syscalls) | ✅ Completo | `cpu/syscall*.c`, `uaccess.h` |
| PMM (bitmap de frames) + VMM (paging 1 GiB identity) | ⚠️ Corregido en 0.5.3 | `mm/pmm.c`, `mm/vmm.c` |
| Kheap (first-fit + coalescing, aligned, realloc) | ✅ Bueno | `mm/kheap.c` |
| Procesos + scheduler MLFQ preemptivo con nice/quantum | ✅ Bueno (UP) | `process/*.c` |
| Señales POSIX básicas (SIGINT/TERM/KILL/CHLD/PIPE/SEGV) | ⚠️ Parcial | `process/process.c` |
| Ring 3 userspace (ELF loader, execve, brk, uaccess) | ⚠️ Corregido en 0.5.3 | `kernel/elf.c` |
| VFS + ramfs + persistencia en disco (sync) | ✅ Bueno | `fs/vfs.c`, `ramfs.c`, `diskfs.c`, `blockfs.c` |
| Drivers disco: IDE PIO + AHCI + xHCI (USB mass storage) | ✅ Completo | `drivers/ata.c`, `ahci.c`, `xhci.c` |
| FAT16 lectura/escritura (monta en `/fat`) | ⚠️ Solo root dir | `fs/fat16.c` |
| devfs (`/dev/zero,null,random,sda`) | ⚠️ Corregido en 0.5.3 | `fs/devfs.c` |
| Pipes in-memory (ring buffers) | ⚠️ Sin ownership | `fs/pipe.c` |
| Teclado/serial/VGA texto + framebuffer gráfico | ✅ Bueno | `drivers/keyboard.c`, `vga.c`, `fb.c` |
| Red: RTL8139 + ARP + ping (ICMP) | ⚠️ Básico | `drivers/rtl8139.c`, `net.c` |
| SMP: detección MADT… arranque de APs **sin invocar** | ❌ Código muerto | `cpu/smp*.c` |
| Multiusuario + SHA-256 + permisos Unix + sticky bit | ✅ Bueno | `auth/users.c`, `fs/vfs.c` |
| Shell ring 0 (80+ comandos) + shell ring 3 (`/bin/sh`) | ✅ Bueno | `shell/*.c`, `user/usersh/` |
| Editor fullscreen, `tcc` (compilador C), `tasm` (assembler) | ✅ Muy completo | `shell/editor.c`, `tcc.c`, `tasm.c` |
| 66 coreutils en ring 3 compilados a ELF | ✅ Excelente | `user/coreutils/` |
| RTC, batería ACPI EC, power off/reboot | ✅ Bueno | `drivers/rtc.c`, `acpi_*.c` |
| CI en GitHub Actions (build + check) | ✅ Configurado | `.github/workflows/` |

---

## 2. Bugs encontrados y CORREGIDOS en esta auditoría (v0.5.3)

### Críticos (seguridad / corrupción)

1. **`uaccess_ok()` permitía wrap-around de punteros** — `addr + len > USER_SPACE_END`
   desbordaba a un valor pequeño con entradas maliciosas, validando rangos que
   cruzan todo el mapa de memoria (kernel incluido). Un syscall como
   `writefile(path, 0x0F000000, 0xF1000000)` hubiera escrito en archivo el
   contenido del kernel. → comparación sin posibilidad de overflow.

2. **El ELF loader escribía sobre el kernel** — ni `elf_exec_argv_inner()` ni
   `elf_execve()` verificaban `p_vaddr >= región de usuario`. Un ELF con
   `PT_LOAD` en `0x00100000` sobrescribía el código del kernel en ring 0:
   ejecución de código arbitrario con privilegios totales desde userspace.
   Además, la tabla de program headers no se validaba contra el tamaño del
   archivo (OOB read en el heap). → Se rechazan segmentos `< 0x08000000` y
   phdrs fuera de rango, en AMBAS rutas (spawn y execve).

3. **El PMM regalaba los frames de los procesos de usuario** — la reserva era
   de 128 MiB, pero los ELF viven en 0x08048000 (128,3 MiB) y los stacks en
   ~0x0F000000: todo eso estaba marcado LIBRE. `pmm_alloc_frame()` (page
   directories, page tables de `vmm_map_page_in`, copias de `fork`) podía
   devolver un frame que era código o stack de un proceso vivo. → Reserva
   ampliada a 256 MiB; cap de entrega al GiB identity-mapeado (evita además
   frames no mapeados en máquinas con >1 GiB).

4. **Page directories/tables editables desde ring 3** — los frames dinámicos
   (≥128 MiB) caen en páginas identity con U/S=1: cualquier userspace podía
   escribir directamente sobre estructuras de paginación propias o ajenas.
   → `vmm_deprivilege_identity_page()` retira el alias de usuario en todos
   los frames usados para PD/PT y copias de fork.

5. **`fork()` era un botón de pánico** — el hijo se creaba con
   `context.eip = 0`; el scheduler lo elegía y `context_switch` saltaba a
   EIP=0 → page fault en ring 0 → kernel panic. Implementarlo de verdad
   requiere copiar el trap frame del int 0x80 y deep-copy de page tables
   (bosquejo en `mm/fork.c`). → `fork()` devuelve -1 (ENOSYS) de momento.

6. **Page fault en ring 3 colgaba el SO** — el handler marcaba `SIGSEGV`
   pendiente y retornaba: el `iret` re-ejecutaba la instrucción → fault →
   retorno → fault infinito (la señal solo se evaluaba al salir de syscalls,
   y el fault no venía de uno). → se mata al proceso al instante con
   `usermode_fault_kill(-14)`, coherente con el resto de excepciones.

### Altos (estabilidad)

7. **`net_ping()`: overflow del stack de kernel** — paquete de 74 bytes en
   buffer de 64 (+ enviaba 60). → buffer de 74 y envío completo.
8. **Trampoline SMP roto** — salto lejano a `0x8016` caía en medio de una
   instrucción (triple fault de APs), array de 256 con 258 bytes reales
   (`-Wexcess-initializer`: CR3 truncado), y `ap_main(uint32_t)` recibía un
   argumento que nadie empujaba. → offset 0x8015, array 0x102, y el AP lee
   su propio APIC ID del LAPIC (método estándar Intel).
9. **UAF open+unlink** — `vfs_delete()` liberaba nodos con fds/dir-handles
   abiertos. → política EBUSY (`fd_node_is_open()`).
10. **/dev se rompía tras el primer `sync`** — se persistían nodos sin
    handlers y al bootear se duplicaban; el muerto tapaba al vivo. Igual con
    `/fat` (congelaba la vista del FAT). → diskfs ya no persiste `/dev` ni
    `/fat`; devfs re-anima nodos existentes y crea `/dev` si falta.
11. **4 KiB de buffer en kstacks de 8 KiB** (`ramfs` diskfile_read/write)
    → buffers estáticos (driver no reentrante).
12. **Leaks**: block lists de nodos disk-backed en `wipe_children`
    (diskfs), PTEs remapeadas por fork que quedaban dangling al morir el
    hijo (→ `vmm_restore_user_identity()` en `process_exit`).

### Menores

- `KERNEL_VERSION` 0.2.0 → 0.5.3 (estaba desfasada 3 años de changelog).
- NULL check en `fat16_init` si falla leer el directorio raíz.
- Warnings: declaraciones implícitas (`tss_set_kernel_stack`, `schedule`),
  `status_col` sin inicializar, `dirty_batch`/label `ed_exit` sin uso.

---

## 3. Lo que le FALTA al kernel (hoja de ruta, por prioridad)

### P0 — Arquitectura (el cambio que más valor aporta)

1. **Espacios de direcciones por proceso de verdad.**
   Hoy TODOS los procesos comparten las mismas page tables (los page
   directories de proceso son shallow copies que apuntan a las mismas
   tablas 20-255). Consecuencias: el "aislamiento de memoria" del README
   es solo kernel-vs-usuario (U/S bit); entre procesos TODO es visible y
   escribible; por eso existen los hacks de backup/restore del padre
   (`elf_exec_argv` guarda 736 KB alrededor de cada spawn anidado) y los
   stacks por nivel de anidamiento (máx. ~4 shells anidadas).
   Camino: deep-copy (o creación vacía + populate) de las tablas de
   usuario en `vmm_create_address_space()`, mapear frames frescos para el
   ELF cargado, y cambiar CR3 al entrar a ring 3 (`usermode_save_and_enter`
   debería cargar `proc->page_dir`). Eso elimina de raíz: el backup/restore,
   el límite de anidamiento, y las colisiones de brk entre procesos.

2. **`fork()` real** (bosquejo incluido en `mm/fork.c`): copiar trap frame
   del int 0x80 al kstack del hijo, eax=0 en la copia, punto de reentrada
   asm que salte a la cola de retorno a ring 3 de `syscall_stub`, y requiere
   P0 nº1 (deep-copy de tablas) para no ser "threads disfrazados".

3. **Entrega de signal handlers en ring 3.** Hoy `process_sigaction`
   registra handlers, pero `process_deliver_signal` solo re-pone la señal
   pendiente: los handlers de usuario jamás se ejecutan (y SIGSEGV ahora
   mata siempre). Falta el trampoline que meta un frame falso en el user
   stack (handler EIP + sig + EIP de retorno) y la syscall `sigreturn`.

### P1 — Robustez y modelo de recursos

4. **fd table por proceso.** Los kfds son una tabla global con `owner_pid`
   solo para limpieza: cualquier proceso puede leer/escribir los fds de
   otro (y `fd_cleanup_process` cierra TODOS los pipes abiertos cuando
   muere cualquiera — rompe pipelines de otros procesos). Mover la tabla
   dentro de `process_t` y clonarla en fork/spawn.

5. **Refcount en vfs_node_t.** El EBUSY de 0.5.3 evita el UAF, pero lo
   correcto es que el nodo sobreviva al último `close()` (semántica POSIX
   de unlink). Con refcount se elimina la necesidad del fd checkeo global.

6. **context_switch con interrupciones inhibidas por diseño** funciona
   porque todo schedule() llega por compuertas de interrupción (IF=0), pero
   un `schedule()` futuro desde código con IF=1 tiene una ventana de carrera
   entre el cambio de CR3 y el de ESP. Envolver con cli/sti explícito.

7. **Concurrencia en VFS/ramfs**: no hay locks de ningún tipo; las listas
   de hijos (`children[]`) y el heap no son IRQ-safe. En UP con puntos de
   schedule controlados se puede vivir, pero cualquier syscall que haga
   `hlt`-yield con estructuras a medio modificar es frágil.

8. **Swap / demand paging / mmap**: hoy `SYS_BRK` mapea páginas físicas
   identidad por adelantado y jamás se liberan al encoger el brk; no hay
   copy-on-write, no hay archivos mapeados. Es el siguiente nivel real.

### P2 — Funcionalidad

9. **SMP: invocar `smp_boot_aps()`** (ahora con el trampoline arreglado)
   en `kernel_main` tras la detección, y después un scheduler con
   per-CPU run queues + locking (hoy el scheduler es estrictamente UP).
10. **FAT16: subdirectorios** (create/delete solo funcionan en raíz),
    nombres largos (LFN), y free-space check antes de escribir.
11. **Red**: solo ARP+ping. Siguiente paso natural: DHCP client, UDP
    sockets (echo), y luego TCP mínimo o un stack tipo lwIP-port.
12. **`fs/fat.c` está muerto** (driver FAT12/32 antiguo sin llamadas):
    borrarlo junto con `fs/fat.c.orig`, los `patch_*.patch`,
    `dd-patch.patch` y `FASE5_REGEN.sh` (cruft de fases viejas).
13. **`pipe()` sin ownership POSIX**: extremos compartidos globales, el
    SIGPIPE se auto-asigna sin chequeos de propietario. Integrar con P1-4.
14. **Máquinas de poca RAM (≤256 MiB)**: con la reserva de 0.5.3 el PMM no
    ofrece frames dinámicos; el kernel sigue funcionando (PD compartido)
    pero debería constar en README como requisito mínimo efectivo.
15. **Tests automatizados**: existe CI de compilación, pero ningún test
    que arranque QEMU headless y valide el prompt (se podría con
    `qemu -serial stdio` + expect en el workflow).

### P3 — Higiene

16. Quedan warnings cosméticos (-Wsign-compare, unused vars/funcs:
    `idle_task_entry`, `g_padre_backup*`, `htonl`, `rd64`, `g_in_usermode`).
    Idealmente: compilar con `-Werror` en CI cuando estén en cero.
17. `boot/mbr_boot.asm` y `boot/longmode.asm` no se enlazan (docs vivas).
    Mover a `docs/` para no confundir.
19. **El kernel NO zeroiza el BSS** (`boot/boot.asm`): funciona en QEMU
    de casualidad (RAM arranca a cero); en hardware real los estáticos
    sin init explícito son basura. COW/jump stack ya se protegen con
    memset propio (v0.6.1), pero lo correcto es zeroizar
    [bss_start, bss_end) en el entry antes de `kernel_main`.
20. Los objetos `.o` rastreados en git mezclados con los ignorados: elegir
    un solo criterio (recomendado: ignorar todos y compilar siempre).

---

## 4. Estado de CI/CD (diagnóstico extra)

- ✅ **`ci.yml` (compilación en PRs) funciona y pasa con esta rama**
  (35 s, `make clean && make` con nasm, artifact >100 KiB).
- ❌ **`build.yml` (build + ISO + USB + release) estaba roto desde que se
  introdujo** — todos los runs fallaban al instante con
  `Invalid workflow file: .github/workflows/build.yml#L121`
  ("workflow file issue", 0 s, sin jobs). **Causa real (confirmada con el
  mensaje de GitHub): el heredoc `GRUBEOF` del step "Generar ISO" estaba
  escrito a columna 0:**

  ```yaml
        run: |
          cat > iso/boot/grub/grub.cfg << 'GRUBEOF'
  set timeout=5        ← ¡columna 0! CIERRA el bloque escalar run: |
  ...                  ← YAML intenta parsear esto como nodo nuevo → error
  GRUBEOF
  ```

  En YAML, todo el contenido de un bloque `run: |` debe ir indentado más
  que la clave; al encontrarse `set timeout=5` a columna 0, el bloque
  termina y el parser muere exactamente en la **línea 121**. El segundo
  heredoc (`SUMEOF`) sí estaba indentado y por eso no fallaba.
  El `env: USB_SIZE: ${{ github.event.inputs.usb_size_mb || '512' }}`
  resultó ser **válido** (en eventos `push` el input es `null` y el
  `|| '512'` resuelve) — el diagnóstico inicial apuntaba ahí por error.

  **Corregido y validado localmente:** cuerpo y delimitador del heredoc
  indentados a la altura del bloque, verificado extrayendo los 12 bloques
  escalares del archivo y pasando cada `run:` por `bash -n`, más una
  simulación real del paso que genera un `grub.cfg` idéntico al esperado.
  De paso se eliminó `truncate` de la lista `apt-get install` (no existe
  como paquete en Ubuntu — viene en `coreutils` — y habría sido el
  siguiente fallo del workflow con "Unable to locate package truncate").
- **El push del fix lo tienes que hacer tú** (GitHub rechaza subidas de
  workflows desde la GitHub App de Arena: `refusing ... without
  \`workflows\` permission`). El fix listo para aplicar está en la raíz
  del repo como **`fix-build-yml.patch`**:

  ```bash
  git apply fix-build-yml.patch
  git commit -am "ci: fix build.yml heredoc GRUBEOF (error L121)"
  git push
  ```

  O desde la web de GitHub, editando `.github/workflows/build.yml`:
  1. Añade 10 espacios a cada línea del cuerpo del heredoc `GRUBEOF`
     (líneas 121-129 aprox.) y a la línea `GRUBEOF` que lo cierra, de
     modo que queden alineadas con el `cat > ... << 'GRUBEOF'`.
  2. Borra la línea `truncate \` de la lista `apt-get install`.

## 5. Notas de verificación

- Todo el código C compila sin errores con los flags del Makefile
  (`-Wall -Wextra`, gcc 12, `-m32`). Los asm no se pudieron ensamblar en
  el entorno de la auditoría (sin nasm), pero son 10 stubs estándar
  revisados a mano; CI (GitHub Actions, ubuntu-22.04 + nasm) valida el
  build final.
- No se pudo ejecutar QEMU en la auditoría: los fixes están pensados para
  ser conservadores (no cambian ABI ni layout de structs públicos), pero
  conviene bootar en QEMU/hardware antes de un release: `make run`.
