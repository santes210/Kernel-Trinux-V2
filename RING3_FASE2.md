# Trinux Ring 3 — Fase 2 (Shell completa en ring 3)

## TL;DR
- Antes (Fase 1): `/bin/*` corrían ring 3, pero el **shell vivía en ring 0**.
- Ahora (Fase 2): el shell ES `/bin/sh`, un ELF userland en **CPL=3 verdadero**.
- 15 syscalls nuevos para soportar fork/exec/io/login.

## Componentes nuevos

| Archivo | Qué hace |
|---|---|
| `user/usersh/sh.c` | El nuevo shell userland (229 líneas) |
| `user/usersh/sh_elf.h` | Generado: el ELF de `sh` como blob xxd |
| `cpu/syscall.c` (extendido) | 15 nuevos handlers de syscall |
| `kernel/elf.c` (extendido) | Snapshot/restore del padre cuando un ring-3 hace SPAWN |

## Cómo funciona la "fork sin fork"

Como no hay address space separado por proceso, el truco es:

1. El shell ring-3 hace `SYS_SPAWN("/bin/echo", argv)`.
2. El kernel respalda la región `0x08048000..0x08100000` (donde vive el shell)
   a un buffer en kheap.
3. Carga `/bin/echo` en la misma VA, salta a ring 3, ejecuta hasta `SYS_EXIT`.
4. Restaura la región del shell desde el backup.
5. Vuelve al shell con el `exit_code` del hijo.

Es como `posix_spawn()` de POSIX: el shell se "duerme" mientras el hijo corre,
sin necesidad de duplicar memoria con `fork`. Más simple y suficiente para
una shell single-threaded.

## Syscalls añadidos (ABI 3.0)

| # | Nombre | Para qué |
|---|---|---|
| 31 | SYS_SPAWN | lanzar ELF con argv, espera (sync) |
| 32 | SYS_WAITPID | (no-op hoy, SPAWN ya espera) |
| 33 | SYS_SPAWN_R | spawn con redirección a archivo |
| 34 | SYS_DUP2_FD | reservado |
| 35-39 | SYS_FILE_* | open/read/write/close/seek con fd |
| 40 | SYS_KEY_RAW | tecla cruda (flechas, etc.) |
| 41 | SYS_VGA_COLOR | cambiar color VGA |
| 42 | SYS_USERADD | ROOT: crear usuario |
| 43 | SYS_PASSWD | cambiar password |
| 44 | SYS_LISTPROC | tabla de procesos para `top` |
| 45 | SYS_LOGIN | login contra /etc/shadow |
| 46 | SYS_VFS_CAP | reservado |

## Built-ins del shell ring-3

Implementados **dentro** de sh.c (no spawn):
- `cd` (cambia cwd vía SYS_CHDIR)
- `pwd` (SYS_GETCWD)
- `exit`, `clear`, `help`, `history`

Todo lo demás se resuelve a `/bin/<cmd>` y se spawnea como ELF ring-3.

## Volver al shell viejo (debug)

Si pasas `oldsh` en el cmdline de GRUB, el kernel arranca el shell ring-0
de antes. Útil para comparar.

## Limitaciones conocidas (vs. Linux real)

- **Sin pipes `|`** todavía (sólo `>` y `>>`).
- **`<` no soportado** (el kernel ignora `stdin_path`).
- **Sin background `&`** (todo síncrono).
- **Anidamiento limitado**: un programa ring-3 puede spawnear otro
  (snapshot/restore funciona), pero no infinitamente — sólo 1 nivel
  de profundidad de hijos. Suficiente para shell→cmd.
- **Sin address space por proceso** (sigue siendo identity-map global).
- **Sin `fork()` real** — usamos posix_spawn semantics.
