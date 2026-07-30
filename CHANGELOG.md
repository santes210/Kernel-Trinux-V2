# Changelog

Todos los cambios notables de Trinux se documentan en este archivo.

El formato está basado en [Keep a Changelog](https://keepachangelog.com/es-ES/1.0.0/),
y este proyecto adhiere a [Semantic Versioning](https://semver.org/lang/es/).

## [Unreleased] - 2026-07-29

### 🔒 Seguridad — DoS trivial vía `nice`/`renice` sin privilegios
- `process_set_priority()` (usada por `renice`, el comando `nice`, y el
  syscall `SYS_RENICE`) no verificaba credenciales antes de bajar la
  prioridad numérica de un proceso (es decir, subirle prioridad real de
  CPU). El propio comentario en el código decía *"we don't have proper
  credentials/uid plumbing yet, so any caller can change priority"*.
  Esto significaba que **cualquier usuario sin privilegios** podía correr
  `nice -20 exec algo` o `renice -20 <pid>` (incluso sobre PID 1) y
  monopolizar la CPU vía el scheduler MLFQ — una denegación de servicio
  trivial de un solo comando, sin necesitar exploit alguno.
- Ahora bajar la prioridad numérica (`prio < PRIO_DEFAULT`, i.e. pedir
  *más* CPU) requiere `uid == 0`, igual que el `nice`/`renice` reales de
  Unix. Subir tu propia prioridad numérica (ser más "amable", ceder CPU)
  sigue sin restricción para cualquier usuario.
- El bug existía en **dos rutas separadas** que había que arreglar por
  separado: `process_set_priority()` (usada por `renice` y por
  `SYS_RENICE` desde ring 3) y el mecanismo distinto de `nice <prio> <cmd>`
  en el shell (`process_set_next_priority()`), que no pasaba por el
  primer check en absoluto.

### 🐛 Fix crítico de build
- `user/coreutils/hdrs/reboot.h` estaba vacío (el binario embebido de
  `reboot` no se había regenerado tras el último merge), lo que rompía
  la compilación completa del kernel (`kernel/kernel.c` fallaba con
  `error: 'u_reboot' undeclared`). Este era el motivo real por el que
  `build.yml` y `ci.yml` llevaban 10 días en rojo. Regenerado.

### 🔒 Seguridad — permisos Unix no aplicados en varios syscalls
- `SYS_READFILE`, `SYS_FILE_OPEN`, `SYS_RENAME`, `SYS_OPENDIR` ahora
  llaman a `vfs_check_access()` antes de leer/listar. Antes, cualquier
  proceso sin privilegios podía leer `/etc/shadow` con `cat` o
  `readfile()`, listar directorios `0700` como `/root`, o pisar
  archivos ajenos con `writefile()`.
- `vfs_create()` ahora valida `ACC_WRITE` también al **sobrescribir**
  un archivo ya existente (antes solo lo validaba al crear uno nuevo).
- `/etc/shadow` se fuerza a permisos `0600` al escribirse (antes
  heredaba el `0644` por defecto de `ramfs`, dejándolo legible por
  cualquier usuario).
- Root conserva el bypass total de permisos, sin cambios de comportamiento
  para el usuario administrador.

### 📝 Documentación
- Corregida la sección "Solo hay 10 syscalls; faltan ~30" del README:
  estaba desactualizada — la mayoría de esos syscalls (directorios,
  stat, chmod/chown, fork/waitpid/kill, uid/passwd) ya estaban
  implementados desde hace varias fases. Se dejó una tabla con el
  estado real y lo que sigue faltando de verdad (`SYS_EXECVE` real,
  `SYS_MMAP`/`SYS_MUNMAP`, `SYS_DUP2`, `SYS_IOCTL`/termios).

## [0.3.2] - 2026-07-19

### 🛡️ Aislamiento de memoria avanzado
- **Page faults en ring 3** ahora matan el proceso con SIGSEGV en lugar de kernel panic
- El kernel continúa funcionando después de matar un proceso con fault
- Incluye process.h en vmm.c para acceder a process_t

### 🎯 Signal handlers en userspace
- Procesos pueden registrar handlers con `signal_(sig, handler)`
- Soporte para SIG_DFL (default) y SIG_IGN (ignorar)
- Nuevo syscall: SYS_SIGNAL (71)
- Array de handlers por proceso: sig_handlers[_NSIG]

### 👶 SIGCHLD - Notificación de hijos
- process_exit() envía SIGCHLD al padre automáticamente
- Permite implementar waitpid() no-bloqueante
- Limpieza eficiente de procesos zombie

### 📋 Señales POSIX expandidas
- 15 señales soportadas: SIGHUP, SIGINT, SIGQUIT, SIGILL, SIGTRAP, SIGABRT, SIGBUS, SIGFPE, SIGKILL, SIGSEGV, SIGPIPE, SIGALRM, SIGTERM, SIGCHLD
- SIGKILL no es capturable (como en POSIX real)
- SIGCHLD se ignora por defecto (como en POSIX)

### Archivos modificados
- `mm/vmm.c` - Page fault handler ring 3/ring 0
- `process/process.h` - Señales y handlers
- `process/process.c` - process_sigaction(), process_deliver_signal(), SIGCHLD
- `cpu/syscall.c` - SYS_SIGNAL handler
- `user/trinux.h` - signal_() wrapper y constantes

## [0.3.1] - 2026-01-18

### 🍴 Fork real + waitpid + SIGPIPE

#### `SYS_FORK` (69) — Creación de procesos
- Nuevo syscall que crea un proceso hijo con copia del address space del padre
- Usa la infraestructura existente de `process_fork()` en `mm/fork.c`
- Devuelve el PID del hijo al padre, -1 en error
- El hijo hereda `parent_pid`, `cwd`, y espacio de memoria copiado

#### `SYS_GETPPID` (70) — Obtener PID del padre
- Nuevo syscall que devuelve el PID del proceso padre
- `parent_pid` trackeado en cada `process_create()`

#### `SYS_WAITPID` (32) — Mejorado con blocking real
- El stub no-op fue reemplazado con `process_waitpid()` real
- Soporta `pid > 0` (esperar hijo específico) y `pid == -1` (cualquier hijo)
- Opción `WNOHANG` para polling no-bloqueante
- Bloquea con `schedule()` hasta que un hijo zombie esté disponible

#### SIGPIPE (13) — Escritura a pipe roto
- Cuando un proceso escribe a un pipe cuyo extremo de lectura está cerrado,
  recibe `SIGPIPE` (señal 13) que lo termina con exit code 141 (128+13)
- `SYS_FILE_WRITE` y `SYS_FILE_READ` ahora manejan fds de pipe correctamente
- `SYS_FILE_CLOSE` ahora cierra extremos de pipe correctamente

### Archivos modificados

| Archivo | Cambio |
|---|---|
| `user/trinux.h` | +SYS_FORK (69), +SYS_GETPPID (70), +WNOHANG, +wrappers fork_/waitpid_/getppid_, +WIFEXITED/WEXITSTATUS/WIFSIGNALED/WTERMSIG |
| `process/process.h` | +SIGPIPE (13), +parent_pid en process_t, +process_get_ppid(), +process_waitpid() |
| `process/process.c` | +parent_pid tracking en process_create(), +process_get_ppid(), +process_waitpid() |
| `cpu/syscall.c` | +SYS_FORK, +SYS_GETPPID handlers, +SIGPIPE en FILE_WRITE, +pipe_read en FILE_READ, +pipe_close en FILE_CLOSE, waitpid real |

---

## [0.3.0] - 2026-01-18

### 🛡️ Seguridad

#### Validación completa de punteros en syscalls (uaccess)

Se añadió validación `uaccess_ok()` y `UCHECK_STR()` a **todos** los syscalls
que reciben punteros de userland (35+ syscalls). Esto previene que programas
maliciosos de ring 3 puedan hacer que el kernel lea o escriba en su propia
memoria pasando punteros inválidos.

**Archivos modificados:**
- `cpu/syscall.c` — Validación añadida a todos los syscalls con punteros
- `cpu/uaccess.h` — Sin cambios (ya existía, ahora se usa exhaustivamente)

**Impacto:** Previene exploits de escalación de privilegios vía punteros
manipulados en syscalls.

### 🛑 Señales POSIX básicas

Se implementó un sistema de señales inspirado en POSIX con soporte para:
- `SIGHUP` (1), `SIGINT` (2), `SIGQUIT` (3), `SIGKILL` (9), `SIGTERM` (15)
- **Ctrl-C** ahora mata el proceso foreground correctamente
- `kill PID SIGTERM` funciona como esperado
- `kill PID 0` verifica existencia sin señalar

**Archivos modificados:**
- `process/process.h` — Añadidos campos `signal_pending` y `signaled` en `process_t`
- `process/process.c` — Implementación de `process_signal()` y `process_check_signal()`
- `drivers/keyboard.c` — Intercepta Ctrl-C en IRQ handler y señala proceso foreground
- `cpu/syscall.c` — `SYS_KILL` usa señales; check de señales al final del handler

**Ejemplo de uso:**
```sh
root@trinux:~# sleep 100
^C
[sleep] killed by signal 2
root@trinux:~#
```

### 📥 Redirección de stdin (`<`)

Se añadió soporte completo para redirección de entrada estándar:
- Tokenizer de shell reconoce `<` como operador
- `run_one()` parsea `< archivo` y establece `spawn_req.stdin_path`
- Kernel lee archivo en buffer de 4 KiB y activa override de teclado
- Driver de teclado consume del override antes del teclado real

**Archivos modificados:**
- `user/usersh/sh.c` — Tokenizer y dispatch con soporte para `<`
- `cpu/syscall.c` — `SYS_SPAWN_R` lee stdin y activa override
- `drivers/keyboard.c` — `keyboard_set_stdin_override()`/`clear_stdin_override()`
- `drivers/keyboard.h` — API pública de override
- `user/usersh/sh_elf.h` — Regenerado con cambios del shell

**Ejemplo de uso:**
```sh
root@trinux:~# echo "hola mundo" > /tmp/test.txt
root@trinux:~# cat < /tmp/test.txt
hola mundo
root@trinux:~# grep hola < /tmp/test.txt
hola mundo
```

### 🔧 Nuevos syscalls (ABI 3.2)

- `SYS_PIPE` (67) — Crea un pipe en RAM, retorna 2 fds (lectura/escritura)
- `SYS_PIPE_CLOSE` (68) — Cierra un extremo del pipe

**Archivos modificados:**
- `user/trinux.h` — Definiciones de syscalls 67-68 y wrappers `pipe_()`/`pipe_close_()`
- `cpu/syscall.c` — Handlers para `SYS_PIPE` y `SYS_PIPE_CLOSE`

**Detalles técnicos:**
Los pipes en RAM usan un ring buffer circular de 4 KiB (`fs/pipe.c`), soportan
hasta 16 pipes simultáneos, y el escritor bloquea si el buffer está lleno
(backpressure). El lector recibe EOF cuando el escritor cierra.

### 📊 Estadísticas actualizadas

- **Syscalls:** 10 → 68 (+58)
- **Líneas de código:** ~12,000 → ~12,500 (+500)
- **Syscalls con validación uaccess:** 4 → 39 (+35)

### 🔧 Mejoras internas

- `SYS_KILL` ahora distingue entre señales fatales (SIGKILL/SIGTERM) y otras
- `process_check_signal()` verifica señales pendientes antes de retornar a usermode
- Exit code de procesos señalados: `128 + número_de_señal` (convención Unix)
- Ctrl-C solo señala procesos con PID > 3 (no afecta init/kthreadd/mysh)

### 🐛 Correcciones

- **Arreglado:** Error de compilación por `SYS_PIPE`/`SYS_PIPE_CLOSE` no definidos
- **Arreglado:** `sh_elf.h` desactualizado respecto a `sh.c`
- **Arreglado:** Validación faltante en syscalls que aceptan punteros de usuario

### 📝 Documentación

- `README.md` — Sección completa de v0.3.0 con tabla de syscalls validados
- `README.md` — Actualización de estadísticas y áreas completadas
- `CHANGELOG.md` — Este archivo

### ⚠️ Breaking changes

Ninguno. Todos los cambios son backward-compatible.

### 🔄 Migración

No se requiere migración. Los cambios son transparentes para programas existentes.

**Nota:** El shell ELF (`sh_elf.h`) fue regenerado. Si tienes programas que
dependen del comportamiento exacto del shell anterior, verifica que sigan
funcionando correctamente.

---

## [0.2.4] - 2025-12-15

### Añadido
- Ring 3 real para shell y 64 coreutils
- Pipes en shell (`cmd1 | cmd2 | cmd3`)
- Editor de texto full-screen (`edit`/`nano`)
- Compilador C accesible (`tcc`)
- `top` interactivo
- 50+ syscalls para soporte de userland

### Cambiado
- Shell ahora corre en ring 3 (CPL=3) en lugar de ring 0
- ELF loader usa `usermode_save_and_enter` para ring 3 real

---

## [0.2.0] - 2025-11-01

### Añadido
- Sistema de archivos VFS con 6 backends (RAMFS, DISKFS, BLOCKFS, DEVFS, FAT16, path)
- Sistema multiusuario con permisos Unix
- Persistencia en disco
- Drivers: VGA, teclado, timer, RTC, serial, PCI, ATA, AHCI, xHCI, ACPI
- Scheduler MLFQ con prioridades
- 70+ comandos de shell
- Compilador C integrado (TCC)
- Ensamblador integrado (TASM)

---

## [0.1.0] - 2025-10-01

### Añadido
- Boot via Multiboot (GRUB)
- Modo protegido 32-bit
- Gestión de memoria con paging (identity-map 256 MiB)
- Manejo de interrupciones (IDT/ISR/IRQ)
- Shell básica en ring 0
- Drivers básicos: VGA texto, teclado PS/2, timer PIT

[0.3.0]: https://github.com/santes210/Kernel-Trinux-V2/compare/v0.2.4...v0.3.0
[0.2.4]: https://github.com/santes210/Kernel-Trinux-V2/compare/v0.2.0...v0.2.4
[0.2.0]: https://github.com/santes210/Kernel-Trinux-V2/compare/v0.1.0...v0.2.0
[0.1.0]: https://github.com/santes210/Kernel-Trinux-V2/releases/tag/v0.1.0

## [0.4.0] - 2026-07-19

### 🛡️ Mejoras Críticas de Estabilidad

#### Prevención de FD Leaks
- Agregado campo `owner_pid` a `kfd_t` para rastrear propietario de cada FD
- Función `fd_cleanup_process(pid)` cierra automáticamente todos los FDs al morir el proceso
- Limpieza automática de pipes asociados al proceso
- **Impacto**: Previene agotamiento de tabla global de FDs

#### Manejo de Procesos Huérfanos
- Reparenting automático: hijos de proceso muerto son adoptados por init (PID 1)
- init se encarga de hacer waitpid() y limpiar zombies
- **Impacto**: Previene acumulación de zombies eternos

#### Prevención de Acumulación de Zombies
- Auto-reaping cuando tabla de procesos está >75% llena
- Limpieza automática de zombies huérfanos
- Logging de advertencia al activar auto-reaping
- **Impacto**: Previene bloqueo por tabla de procesos llena

#### Validación de Syscalls
- Validación de rango al inicio de `syscall_handler()`
- Syscalls con número > 72 retornan -1 y logean advertencia
- **Impacto**: Previene dispatch a handlers inexistentes

#### Page Fault Handler Mejorado
- Page faults en ring 3 envían SIGSEGV al proceso y lo terminan
- Kernel continúa ejecutándose normalmente
- Solo page faults en ring 0 causan panic (bug real del kernel)
- **Impacto**: Sistema sobrevive crashes de procesos de usuario

### Archivos Modificados
- `cpu/syscall.c`: `owner_pid` en `kfd_t`, `fd_cleanup_process()`, validación syscall
- `process/process.c`: Reparenting, auto-reaping, fd cleanup en exit
- `mm/vmm.c`: Page fault handler distingue ring 0 vs ring 3
- `README.md`: Documentación completa de mejoras de robustez


---

## [0.5.1] - 2026-07-19

### 🛡️ Address Spaces por Proceso — Aislamiento de Memoria Real

#### Cambio fundamental: Page Directory por proceso + Kernel/User split

**Antes**: Un solo page directory global identity-mapped (1 GiB) con **todas las páginas marcadas `PAGE_USER`**. Cualquier proceso ring 3 podía leer/escribir memoria del kernel (código, datos, heap, `/etc/shadow` en RAM) conociendo la dirección virtual.

**Ahora**: Cada proceso tiene su propio `page_directory_t` (`p->page_dir`). Las páginas **0–80 MB** (BIOS + kernel code/data + kernel heap) son **supervisor-only (U/S=0)**; las páginas **80 MB–1 GB** son **user-accessible (U/S=1)**. El `context_switch()` cambia `CR3` en cada cambio de contexto.

### 📋 Resumen de cambios

| Componente | Antes | Ahora |
|---|---|---|
| Page directory | Global único | **Por proceso** (`process_create`/`process_exit`) |
| Kernel pages (0–0x05000000) | `PAGE_USER` = 1 | **`PAGE_USER` = 0** (supervisor-only) |
| User pages (≥0x05000000) | `PAGE_USER` = 1 | `PAGE_USER` = 1 |
| Context switch | Solo registros | **+ `mov cr3, new_pd`** |
| ELF load | Memoria global | **`vmm_map_page_in(proc->page_dir, ...)`** |
| `fork()` | Copia address space | **Duplica page directory (full-copy)** |
| Heap userland | Placeholder | **`SYS_BRK` (72) mapea en `proc->page_dir`** |

### 🔧 Archivos modificados

| Archivo | Cambio |
|---|---|
| `mm/vmm.c` | `vmm_init()`: kernel pages sin `PAGE_USER`; `vmm_create_address_space()`: shallow copy con split kernel/user |
| `mm/vmm.h` | `#define KERNEL_END_VIRT 0x05000000U` (80 MiB boundary) |
| `process/process.c` | `process_create()`: `vmm_create_address_space()`; `process_exit()`: `vmm_free_address_space()` |
| `process/switch.asm` | 3er arg `new_page_dir`; `mov cr3, ecx` antes de cargar contexto |
| `process/scheduler.c` | `schedule()` pasa `p->page_dir` al `context_switch()` |
| `kernel/elf.c` | `elf_exec_argv()`: crea proceso ANTES; usa `vmm_map_page_in(proc->page_dir, ...)` |
| `cpu/syscall.c` | Handler `SYS_BRK` (72); usa `KERNEL_END_VIRT` para validar rango |
| `user/trinux.h` | `#define SYS_BRK 72` + wrapper `brk_()` |

### ✅ Verificación del aislamiento

```sh
# Programa que intenta leer memoria del kernel (0x00100000 = kernel code)
cat > /root/leak.c <<'EOF'
int main() {
    volatile int *kernel_mem = (int *)0x00100000;
    int val = *kernel_mem;   // PAGE FAULT: U/S violation
    print_num(val);
    return 0;
}
EOF
tcc /root/leak.c && exec /root/leak
# *** PAGE FAULT en ring 3 (proceso X: leak) ***
#   addr=00100000  protection read
#   Terminando proceso con SIGSEGV (11)
# Kernel sigue vivo, shell responde normalmente
```

### ⚠️ Limitaciones actuales

1. **`fork()` = full-copy** del page directory (no COW aún).
2. **No hay `SYS_EXECVE` real** — `SYS_SPAWN` usa `setjmp/longjmp` + `elf_exec_argv()`.
3. **`brk()` tracking por proceso** es placeholder (`0x08100000` fijo).
4. **Kernel stacks** en identity-map global (requieren CR3 del kernel para acceso).

### 🎯 Próximos pasos (Fase 2)

1. **COW en `fork()`** — page fault handler para copy-on-write.
2. **`SYS_EXECVE`** — reemplazar address space del proceso actual.
3. **`brk` real por proceso** — campo `heap_brk` en `process_t`.
4. **Shell como proceso ring 3** — `kernel_main()` → `execve("/bin/mysh")`.

