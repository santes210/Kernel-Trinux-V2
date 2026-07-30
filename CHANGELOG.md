# Changelog

Todos los cambios notables de Trinux se documentan en este archivo.

El formato está basado en [Keep a Changelog](https://keepachangelog.com/es-ES/1.0.0/),
y este proyecto adhiere a [Semantic Versioning](https://semver.org/lang/es/).

## [0.6.0] - 2026-07-30

### 🏛️ Address spaces REALES por proceso + fork() real (P0 de la auditoría)

El cambio arquitectónico más grande de Trinux: cada proceso ahora vive en
su propio espacio de direcciones, y `fork()` funciona de verdad.

#### Añadido
- **Aislamiento de memoria por proceso** (`mm/vmm.c`): cada page directory
  de proceso tiene **page tables PRIVADAS** para la región de usuario
  0x08000000-0x10000000 (tablas 32-63); kernel, physmap y MMIO se
  comparten con el PD del kernel. Un proceso ya NO puede leer ni escribir
  la memoria de otro usando su VA — solo la suya.
- **Physmap cerrado a ring 3**: las tablas 20-31 y 64-255 del identity-map
  pasan a supervisor-only. Antes CUALQUIER frame físico del sistema era
  accesible desde ring 3 usando su dirección física como VA (page tables
  de otros procesos, buffers de ramfs, etc.).
- **ELF loader sobre frames frescos** (`kernel/elf.c`): code/data/bss se
  cargan en frames PMM nuevos mapeados en las tablas privadas del proceso;
  el proceso entra a ring 3 **con su propio CR3**. El user stack siempre
  vive en VA `0x0F000000` pero en frames distintos por proceso.
- **Muere el hack del backup de 736 KiB** y los stacks por nivel de
  anidamiento (L0-L3): con aislamiento real, spawns anidados ilimitados
  (`sh` → `ls` → `sh` → ...) funcionan sin tocar la memoria del padre.
- **`fork()` real y asíncrono** (`mm/fork.c` + `cpu/syscall_asm.asm`):
  deep-copy de la región de usuario del padre (`vmm_copy_user_space`) +
  copia del trap frame del int 0x80 con `eax=0` en el kstack del hijo.
  El hijo entra al scheduler y su primer `context_switch` aterriza en
  `fork_child_trampoline`, que replica la cola de `syscall_stub` (iret a
  ring 3 justo después del `int 0x80`). fork() devuelve el PID al padre
  y 0 al hijo. La salida del hijo regresa directo al contexto del padre
  (`process_resume_parent_or_park`) sin tocar el jump buffer ajeno.
- **Jump stack con dueño** (`cpu/syscall.c`): la maquinaria setjmp/longjmp
  de los ELF pasa de un buffer global único a una pila de 8 niveles con
  pid dueño por nivel + CR3 guardado en el slot. Un hijo de fork (que
  nunca pasa por `usermode_save_and_enter`) ya no puede corromper el
  frame de salida de otro proceso al hacer SYS_EXIT/fault.
- **`SYS_BRK` con frames frescos**: el heap de cada proceso se mapea VA →
  frame PMM nuevo en sus tablas privadas (antes: identidad compartida).
- **`execve()` con reset de región** (`vmm_reset_user_region`): libera los
  frames viejos antes de cargar la imagen nueva (mismo PD, mismo PID).
- Higiene del repo: los 34 `.o` trackeados salen del índice (`*.o` ya
  estaba en `.gitignore`); se regeneran siempre con `make`.

#### Cambiado
- `vmm_create_address_space()` ahora crea tablas privadas 32-63 (+132 KiB
  por proceso) y hace snapshot de las PDEs de MMIO del kernel (256+).
- `vmm_free_address_space()` solo libera lo que el proceso posee en
  exclusiva (fix latente: antes podía devolver frames estáticos de MMIO
  al PMM) + `process_exit()` devuelve el CR3 al PD del kernel al morir.
- `vmm_map_page_in()` invalida la TLB cuando mapea sobre el PD activo.
- Límite del ELF loader unificado a 0x10000000 (era 1 GiB): coherente con
  `uaccess` y con el physmap cerrado.

#### Notas de diseño (v1)
- Sin COW todavía: fork copia físicamente cada página presente del padre.
- Modelo cooperativo: padre e hijo se alternan en syscalls
  (waitpid/yield/sleep), igual que el resto de Trinux.
- La fd-table sigue siendo global por diseño (los hijos la comparten).

## [0.5.3] - 2026-07-30

### 🛡️ Auditoría completa de código — 12 correcciones críticas/altas

Revisión línea por línea de todo el kernel (~60.700 líneas, 272 archivos).
Ver **AUDITORIA.md** para el análisis completo y la hoja de ruta.

#### Seguridad (críticas)
- **`uaccess_ok()` overflow** (`cpu/uaccess.h`): `addr + len > END` podía
  envolver y validar rangos que recorren TODA la memoria (kernel incluido)
  desde ring 3. Ahora `len > END - addr` (nunca desborda).
- **ELF loader sobre-escribía el kernel** (`kernel/elf.c`): `PT_LOAD` con
  `p_vaddr < 0x08000000` se copiaba en ring 0 sobre el propio kernel
  (ejecución arbitraria ring 0 desde un ELF, tanto por `SYS_SPAWN` como por
  `SYS_EXECVE`). Se rechaza todo segmento bajo `ELF_MIN_USER_VADDR` y se
  valida la tabla de PHDRs contra el tamaño del archivo (OOB read) en ambas
  rutas de carga.
- **PMM entregaba frames de usuario vivos** (`mm/pmm.c`): solo se
  reservaban 128 MiB; los frames del código/stack/heap de usuarios
  (0x08048000-0x0F100000) quedaban "libres" para page dirs/tables/fork —
  corrupción de procesos. Reserva ampliada a 256 MiB + cap de 1 GiB
  (límite del identity-map; evita frames inmappings en máquinas >1 GiB).
- **Page tables/dirs al alcance de ring 3** (`mm/vmm.c`): los frames
  dinámicos del PMM caen en páginas identity U/S=1 — un usuario podía
  editar las estructuras de paginación del kernel. Nueva
  `vmm_deprivilege_identity_page()` aplicada a PDs, PTs y frames de fork.
- **`fork()` paniqueaba el sistema** (`mm/fork.c`): el hijo se creaba con
  `context.eip = 0`; al ser elegido por el scheduler → EIP=0 → page fault
  en ring 0 → pánico. Ahora devuelve -1 de forma segura hasta implementarlo
  bien (bosquejo incluido en el código y en AUDITORIA.md).
- **Page fault en ring 3 = bucle infinito** (`mm/vmm.c`): solo se marcaba
  SIGSEGV pendiente y se retornaba; el `iret` re-ejecutaba la misma
  instrucción → fault infinito que colgaba el SO. Ahora termina el proceso
  vía `usermode_fault_kill(-14)`, coherente con otras excepciones.

#### Estabilidad (altas)
- **`net_ping()` stack overflow** (`drivers/net.c`): paquete ICMP de 74
  bytes construido sobre un buffer de 64 (overflow de 10 bytes, detectado
  por `-Warray-bounds`) y se enviaban solo 60 de los 74 bytes.
- **Trampoline SMP roto** (`cpu/smp_boot.c`): `jmp far` a 0x8016 en medio
  de una instrucción (triple fault de APs), array de 256 desbordado con
  258 reales (CR3 truncado), y `ap_main` recibía un arg cdecl que el
  trampoline nunca empujó — ahora cada AP lee su APIC ID del LAPIC.
- **Use-after-free por unlink** (`fs/vfs.c`, `cpu/syscall.c`): borrar un
  archivo/dir con fd o dir-handle abierto liberaba el nodo con punteros
  vivos. `vfs_delete()` devuelve "busy" (nueva `fd_node_is_open()`).
- **`/dev` moría tras el primer sync** (`fs/devfs.c`, `fs/diskfs.c`):
  diskfs persistía nodos sin handlers y se duplicaban al bootear — el
  nodo muerto tapaba al real. Ya no se persisten /dev ni /fat (esta última
  además congelaba una vista muerta del volumen FAT16) y `devfs_init`
  re-anima o recrea los nodos.
- **Buffers de 4 KiB en el kstack** (`fs/ramfs.c`): lecturas/escrituras
  disk-backed usaban la mitad de un kstack de 8 KiB. Ahora estáticos.
- **Leaks**: block lists disk-backed no se liberaban en `wipe_children`
  (diskfs); PTEs remapeadas por fork quedaban dangling al morir el hijo
  (nueva `vmm_restore_user_identity()` en `process_exit`).

#### Menores
- `KERNEL_VERSION` 0.2.0 → 0.5.3 (desfasada del CHANGELOG).
- `fat16_init` no chequeaba NULL si fallaba la lectura del directorio.
- Warnings: declaraciones implícitas (`tss_set_kernel_stack` en elf.c,
  `schedule` en process.c), `status_col` sin inicializar y
  `dirty_batch`/label `ed_exit` sin uso (editor.c), excess-elements
  (smp_boot.c).
- Nota: `fs/fat.c` (driver FAT viejo, sin uso: todo pasa por fat16.c)
  sigue compilándose pero muerto — ver AUDITORIA.md §3-P2.

## [0.5.2] - 2026-07-29

### 🚀 `SYS_EXECVE` real (execve() de verdad)
- Nuevo syscall `SYS_EXECVE` (73): reemplaza la imagen del proceso QUE
  LLAMA (mismo PID, mismo `page_dir`, mismos fds/cwd) con un ELF nuevo,
  en lugar de crear un proceso nuevo como hace `SYS_SPAWN`/`exec`.
- Implementado en dos pasadas en `kernel/elf.c:elf_execve()`: primero se
  valida el ELF completo (magic/arquitectura/cada segmento `PT_LOAD`
  dentro del 1 GiB identity-mapped y del archivo) sin tocar la memoria
  del proceso llamador; solo si todo pasa se empieza a sobrescribir. Si
  cualquier validación falla, el proceso original sigue corriendo
  intacto — exactamente la semántica de fallo de `execve(2)` en Unix.
- Valida el bit `ACC_EXEC` del binario destino (a diferencia de
  `SYS_SPAWN`, hoy solo alcanzable desde el shell de confianza).
- `cpu/syscall.c`: en éxito, el handler parchea `regs->eip`/`regs->useresp`
  del propio trap frame del `int 0x80` en curso, así que el `iret` que ya
  iba a ejecutar el retorno del syscall salta directo al programa nuevo
  sin necesitar ningún mecanismo de contexto adicional.
- Nuevo wrapper `execve_(path, argv)` en `user/trinux.h`.
- Programa de prueba: `exec /bin/execvetest` — compara su propio PID
  antes y después de hacer `execve_()` sobre sí mismo; el PID debe ser
  idéntico, demostrando que es el mismo proceso reemplazado in-place
  (y no uno nuevo).

### 🚀 `brk()` real por proceso
- `process_t` (`process/process.h`) gana dos campos: `heap_start` (break
  inicial, fijado al cargar el ELF) y `heap_brk` (break actual).
- `kernel/elf.c` calcula `heap_start` en ambos cargadores de ELF
  (`elf_exec_argv_inner()` y el nuevo `elf_execve()`) como el final del
  último segmento `PT_LOAD`, redondeado a página + una página de guarda.
- `SYS_BRK` en `cpu/syscall.c` ya no usa una dirección fija compartida
  (`0x08100000`) por todos los procesos — ahora lee/escribe
  `heap_start`/`heap_brk` del proceso actual, rechaza bajar de
  `heap_start`, y mapea solo las páginas nuevas necesarias al crecer.
- `mm/fork.c`: el hijo hereda `heap_start`/`heap_brk` del padre (las
  páginas de heap ya se copian físicamente en `vmm_copy_region()`).
- Programa de prueba: `exec /bin/brktest` — crece el heap 8 KiB,
  escribe/lee un patrón de bytes para confirmar que las páginas están
  de verdad mapeadas, y verifica que un intento de encoger por debajo de
  `heap_start` se rechaza sin tocar el break.

### 📝 Documentación
- README: sección "Limitaciones actuales" de v0.5.1 actualizada — ambos
  puntos (`execve()` real, `brk()` por proceso) que decían "no
  implementado" ahora están marcados como resueltos con referencia al
  código correspondiente.

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

