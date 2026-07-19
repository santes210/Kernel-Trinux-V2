# Changelog

Todos los cambios notables de Trinux se documentan en este archivo.

El formato está basado en [Keep a Changelog](https://keepachangelog.com/es-ES/1.0.0/),
y este proyecto adhiere a [Semantic Versioning](https://semver.org/lang/es/).

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
