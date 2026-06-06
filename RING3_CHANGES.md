# Ring 3 — Cambios añadidos en esta tanda

## TL;DR
- Antes: `/bin/*.elf` se ejecutaban en **ring 0** (TODO sin resolver en `elf_exec`).
- Ahora: `/bin/*` se ejecutan en **ring 3 real** vía `usermode_save_and_enter`.
- La shell rutea automáticamente cualquier comando a `/bin/<nombre>` si existe.
- 27 comandos coreutils nuevos como ELFs userland.

## Cómo probar

```sh
# Compilar todo
bash user/coreutils/build.sh   # compila los 27 ELFs y genera user_bins.h
make                            # compila el kernel embebiéndolos

# Probar en QEMU (ISO mínima 21 MB)
qemu-system-i386 -cdrom trinux.iso -m 512M

# O genera la imagen USB completa de 512 MB con persistencia
bash make-usb-image.sh 512
qemu-system-i386 -drive file=mykernel-usb.img,format=raw,if=ide -m 512M
```

Login normal: `root / root` o `user / user`.

## Prueba de aislamiento

Hay un programa `ringtest` que intenta ejecutar `cli` (instrucción privilegiada):

```
root@trinux:~# ringtest
[ringtest] Soy un programa userland. PID=10
[ringtest] Intentando ejecutar 'cli' (instrucción privilegiada)...
[ringtest] Si ves 'isolation FAILED', estoy en ring 0.
[ringtest] Si en vez de eso el kernel me mata con #GP, estoy en ring 3.

*** CPU EXCEPTION ***
  General Protection Fault (interrupt 13)
  ...
  eip=080480d5 cs=001b  eflags=00010246 ds=0023
  (terminating ring-3 program; kernel continues)
root@trinux:~#
```

Las pistas: `cs=001b` (selector user code RPL=3), `ds=0023` (selector user data
RPL=3), el `cli` dispara `#GP` y el kernel mata el programa **sin morirse**.

## Archivos modificados

| Archivo | Cambio |
|---|---|
| `cpu/syscall.h` | Sourced from `user/trinux.h` — ABI única kernel↔user |
| `cpu/syscall.c` | +16 nuevos syscalls: `getcwd, chdir, opendir, readdir, closedir, stat, unlink, mkdir, rmdir, hostname, reboot, shutdown, kill, sync, getuid, getuser, rename, clear` |
| `user/trinux.h` | Re-escrito como ABI compartida + libc mini para ring 3 |
| `kernel/elf.c` | **Arreglado el TODO:** ahora usa `usermode_save_and_enter` (ring 3 real) en lugar de llamar el ELF como función en kernel mode |
| `kernel/elf.h` | +`elf_exec_argv()` para pasar argv al programa |
| `kernel/kernel.c` | Dropea automáticamente los 27 ELFs userland en `/bin/` al boot |
| `shell/commands.c` | Routing: si existe `/bin/<cmd>` lo ejecuta como ring 3 antes del built-in |

## Archivos nuevos

```
user/coreutils/
├── crt0.S              # startup ring-3 (llama main, hace SYS_EXIT)
├── user.ld             # linker script (entry 0x08048000)
├── build.sh            # compila los 27 .c → ELF → .h embebido
├── echo.c cat.c ls.c pwd.c whoami.c hostname_u.c uname.c uptime_u.c
├── clear.c mkdir_u.c rmdir_u.c rm_u.c touch_u.c wc_u.c head_u.c tail_u.c
├── grep_u.c sleep_u.c yes_u.c true_u.c false_u.c id_u.c
├── reboot_u.c shutdown_u.c sync_u.c kill_u.c
├── ringtest.c          # prueba aislamiento con `cli`
├── hdrs/*.h            # AUTO: cada ELF como blob xxd -i
└── user_bins.h         # AUTO: tabla {nombre,data,size} para el kernel
```

## Lo que NO está hecho (futuro)

- **Address space por proceso**: hoy todo el ring 3 comparte el identity map
  de 256 MiB del kernel. Un programa ring 3 puede *leer* memoria del kernel
  (no escribirla en páginas RO, pero la mayoría de mapeos son RW user).
  Para aislamiento de memoria real hay que crear un page directory por
  proceso, usar `copy_from_user`/`copy_to_user` para validar punteros en
  los syscalls, y `fork()`/`execve()`/`waitpid()`.
- **La shell sigue en ring 0**. El siguiente paso (Camino C en la
  conversación original) sería compilar `shell.c` también como ELF y
  arrancarlo como PID 1 (`/sbin/init` → `/bin/sh`). Pide ~20 syscalls más
  (`open/read/write/close` reales, `dup2`, `pipe`, `fork`, `execve`, `waitpid`).
- **Redirección `>` y pipes `|`**: hoy las maneja el shell capturando la VGA
  (`vga_capture_begin/end`). Los programas userland imprimen directo vía
  `SYS_WRITE` → `vga_putchar`, así que la captura no los ve. Hay que pasar
  un syscall de "fd a buffer".

## Syscalls disponibles desde ring 3 (`user/trinux.h`)

| # | Nombre | Resumen |
|---|---|---|
| 1 | `exit(rc)` | salir con código |
| 2 | `write(fd,buf,len)` | escribir a fd 1=stdout |
| 3 | `getpid()` | pid del proceso |
| 4 | `_syscall0(SYS_YIELD)` | ceder la CPU |
| 5 | `msleep(ms)` | dormir |
| 6 | `getchar()` | leer 1 tecla (bloquea) |
| 7 | `uptime()` | seg desde boot |
| 8 | `readfile(path,buf,max)` | leer archivo entero |
| 9 | `writefile(path,buf,len)` | crear/truncar/escribir |
| 10 | `getline_(buf,max)` | leer línea con eco |
| 11 | `getcwd(buf,max)` | dir actual |
| 12 | `chdir(path)` | cambiar dir |
| 13 | `opendir(path)` | abrir dir |
| 14 | `readdir(dh,&de)` | leer entrada |
| 15 | `closedir(dh)` | cerrar dir |
| 16 | `stat(path,&st)` | atributos |
| 17 | `unlink(path)` | borrar archivo |
| 18 | `mkdir(path)` | crear dir |
| 19 | `rmdir(path)` | borrar dir vacío |
| 20 | `hostname(buf,max)` | hostname |
| 21 | `sys_reboot()` | **ROOT** reinicia |
| 22 | `sys_shutdown()` | **ROOT** apaga |
| 23 | `sys_kill(pid,sig)` | **ROOT** matar |
| 24 | `sys_sync()` | flush disk |
| 25 | `getuid()` | uid actual |
| 26 | `getuser(buf,max)` | login name |
| 27 | `rename(src,dst)` | renombrar archivo |
| 30 | `clrscr()` | limpiar VGA |
