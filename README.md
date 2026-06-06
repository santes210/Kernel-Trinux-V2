# Trinux

Un pequeño **sistema operativo estilo Unix/Linux** escrito completamente
desde cero en **C** y **ensamblador x86 (NASM)**. No usa ninguna librería
externa, ni código de Linux, ni de ningún otro SO — cada línea, desde el
bootloader hasta el último comando de la shell, fue escrita a mano.

Arranca vía **Multiboot** (GRUB / `qemu-system-i386 -kernel`), corre en
**modo protegido de 32 bits**, y te deja en una **shell interactiva**
completamente funcional con más de 66 comandos integrados, un sistema de
archivos jerárquico con persistencia en disco, gestión de memoria con
paging, manejo de interrupciones, drivers de teclado/timer/disco/batería,
dispositivos en `/dev`, sistema multiusuario con permisos Unix reales,
editor de texto a pantalla completa, pipes, redirección, grep, y mucho más.

**Funciona tanto en QEMU como en hardware real** — la imagen USB generada
bootea en **Legacy BIOS y UEFI**, y el kernel detecta automáticamente si
el disco es IDE, SATA (AHCI) o USB (xHCI), así que puedes correrlo en
laptops modernas como la HP Stream 14 donde la USB de boot es el único
medio de almacenamiento accesible.

---

## 🆕 Novedades v0.2.x — Ring 3 real (Fases 1-7)

> **Resumen ultra corto**: el shell y 64 comandos ahora corren en **ring 3 real** (CPL=3),
> con pipes, redirección, top interactivo, editor de texto, compilador C accesible,
> y prompt con colores. El kernel sólo se llama vía 50+ syscalls.

Esta sección documenta **TODO** lo añadido en las fases v0.2.1 → v0.2.4.
Para el diseño original ring-0 del sistema, ver la versión v0.2.0 más abajo.

### Fase 1 — Loader ELF a ring 3 + 27 coreutils

**Antes**: el `elf_exec()` del kernel cargaba binarios pero los ejecutaba con
`fn()` (llamada en kernel mode — CPL=0). El comentario decía
*"TODO: proper ring 3 execution with save/restore"*.

**Ahora**: `elf_exec()` arma un IRET frame con `cs=0x1B` (user code, RPL=3) y
`ss=0x23` (user data, RPL=3), salta a `usermode_save_and_enter()` que:
1. Guarda el ESP kernel del padre
2. Hace `iret` a la VA del ELF en ring 3
3. Cuando el ELF llama `SYS_EXIT`, `elf_jmp_longjmp()` vuelve al kernel
4. Restaura el ESP guardado

Los primeros 27 comandos coreutils nacen como ELFs `/bin/*` en CPL=3:
`ls cat echo pwd whoami hostname uname uptime clear mkdir rmdir rm touch wc head tail grep sleep yes true false id reboot shutdown sync kill ringtest`.

### Fase 2 — Shell completa en ring 3 (`/bin/sh`)

**Antes**: la shell (`shell/shell.c`) era código del kernel que el `kernel_main`
llamaba directamente como función. No era un proceso schedulable.

**Ahora**: hay un nuevo ELF userland `user/usersh/sh.c` que el kernel arranca
como **PID 5** en ring 3. La shell:
- Hace login interactivo vía `SYS_LOGIN` (contra `/etc/shadow` del kernel)
- Tiene prompt en color con `user@host:cwd`
- Tokeniza, ejecuta built-ins (`cd`, `exit`, `help`, `history`, `clear`)
- Resuelve cualquier otro comando a `/bin/<nombre>` vía `SYS_SPAWN`

**Modo legacy**: pasar `oldsh` en el cmdline de GRUB lanza el shell ring-0 viejo.

### Fase 3 — Resto de los 64 coreutils en ring 3

Se portan todos los comandos restantes (excepto `top`, `edit`, `tcc` que
necesitaban infra extra — ver Fase 5):
`cp mv stat chmod chown date free df users groups logout su useradd passwd ps`
`renice battery tree find sort uniq cut tee seq basename dirname which env`
`calc hexdump neofetch write color halt`

Cada uno requirió un syscall nuevo según lo que tocaba (chmod → `SYS_CHMOD`,
free → `SYS_MEMINFO`, ps → `SYS_LISTPROC`, etc).

### Fase 4 — Pipes + multi-stack por nivel de anidamiento

**Bug crítico encontrado**: cuando el shell ring-3 hacía `SPAWN` de un hijo,
el kernel cargaba el ELF hijo en la misma VA (`0x08048000`) y le daba un
user stack en `USER_STACK_TOP = 0x0F000000` — el **mismo** stack del shell.
Resultado: el stack del padre se corrompía y el shell crasheaba con `eip=3`.

**Fix**: cada nivel de spawn usa una VA de stack distinta:
- Nivel 0 (shell)     → `0x0F000000`
- Nivel 1 (cmd hijo)  → `0x0EE00000`
- Nivel 2 (sub-cmd)   → `0x0EC00000`
- Nivel 3             → `0x0EA00000`

Además, el código del padre se respalda en kheap antes del spawn y se restaura
al volver (porque el ELF hijo carga en `0x08048000`).

Con eso resuelto, los **pipes** se implementan en `dispatch()`:
```
cmd1 | cmd2 | cmd3
```
se ejecuta como tres SPAWN secuenciales, redirigiendo cada uno a un archivo
temporal `/tmp/.p0`, `/tmp/.p1`, etc., y pasando el archivo como último argv
del siguiente stage. Máximo **8 stages**. Los tmpfiles se borran al terminar.

### Fase 5 — `top`, `edit`, `tcc` con 3 syscalls extra

**Problema**: estos tres comandos requieren primitivas que no existían:
- `top` necesita leer teclas **sin bloquearse** (para el refresh periódico).
- `edit` necesita posicionar el cursor en cualquier (x,y) sin imprimir.
- `tcc` es un compilador grande del kernel (`shell/tcc.c`); portarlo a ring 3
  sería ~1500 líneas. Pragmático: hacer un wrapper userland que invoque al
  built-in del kernel vía syscall.

**Syscalls añadidos**:
| # | Nombre | Para qué |
|---|---|---|
| 61 | `SYS_KEY_POLL` | tecla pendiente o -1 (no bloquea) |
| 62 | `SYS_VGA_GOTO` | mover cursor de hardware a (x,y) |
| 63 | `SYS_VGA_CLEAR` | clear + reset cursor a (0,0) |
| 64 | `SYS_TCC_COMPILE` | compila `.c` (delega al built-in del kernel) |

**`top`**: redibuja cada 1 s, header con uptime/memoria/disco, tabla de 18
procesos, sale con `q`.

**`edit`**: editor full-screen, 23 filas de contenido + status bar arriba y
abajo. Cursor de hardware (`vga_goto`) en la posición exacta del carácter
(no debajo). Controles: flechas, `Ctrl-S` guardar, `Ctrl-X` guardar+salir,
`Ctrl-Q` quit sin guardar. Hasta 200 líneas × 80 columnas. Alias: `nano`.

**`tcc`**: wrapper userland (`user/coreutils/tcc_u.c`, ~30 líneas) que llama
`SYS_TCC_COMPILE`. El compilador en sí (`shell/tcc.c`) sigue siendo built-in
del kernel — corre en ring 0 mientras compila, pero el binario que produce
sí se ejecuta en ring 3 al invocarlo. Flujo completo:
```
edit /root/p.c   →   tcc /root/p.c   →   /root/p   (corre en ring 3)
```

### Fase 6 — Bugfixes (v0.2.3)

**Bug 1**: `cp` y `mv` no sobrescribían correctamente archivos destino existentes.
Causa: `vfs_create(path,...)` cuando el path existe devuelve el nodo
existente; el handler `SYS_WRITEFILE` hacía `n->size=0` pero los datos viejos
en disco/RAM podían dejar residuos en el archivo nuevo.
**Fix**: `cp_u.c` y `mv_u.c` hacen `unlink(dst)` antes de `writefile(dst, ...)`
para forzar un nodo virgen. También se aplicó la misma precaución a `tee` y
`write` y `edit` (en su save).

**Bug 2**: `su` no cambiaba el usuario "de verdad" (el prompt seguía mostrando
el viejo).
Causa doble:
- El syscall `SYS_SU` SÍ actualizaba `current_user` en el kernel, pero el
  shell ring-3 mantenía `g_user/g_is_root` en su BSS y nunca los refrescaba.
- `su_u.c` antiguo dependía de `is_root_user()` evaluado dentro del syscall
  para decidir si pedía password, lo cual era inconsistente.

**Fix**: nueva función `refresh_identity()` en `sh.c` que llama `getuser` +
`getuid` desde el kernel después de cualquier `su`/`login`/`logout`. Y `su`
ahora siempre pide password (root puede pasar Enter para bypass).

### Fase 7 — Colores del prompt y `ls` con tipos (v0.2.4)

**Problema reportado**: "los comandos se ven blancos, las carpetas no tienen color".

Causas:
1. El `show_prompt()` de `sh.c` terminaba con `vga_color(LIGHT_GREY, BLACK)`,
   así que lo que el usuario tipeaba salía gris (no verde como en el shell
   original ring-0).
2. `ls.c` no llamaba `vga_color` para nada — imprimía todo en el color que
   estuviera "vigente" (típicamente gris).

**Fix en `sh.c`**: el final de `show_prompt()` ahora pone
`vga_color(LIGHT_GREEN, BLACK)` — lo que escribes se ve **verde**. Y al final
de cada `dispatch()` se resetea a gris para que la salida del comando no se
contamine con colores que un comando anterior haya dejado.

**Fix en `ls.c`**: cada entry se pinta según tipo y permisos:
- 🔵 **Directorios** → `LIGHT_BLUE` (azul claro)
- 🟢 **Ejecutables** (cualquier bit `x` en perms) → `LIGHT_GREEN`
- 🟦 **Dispositivos** (`/dev/sda`, `/dev/null`, ...) → `LIGHT_CYAN`
- ⚪ **Archivos normales** → `LIGHT_GREY`

### Fase 8 — Modo texto 80×50 con fuente 8×8 (v0.2.5)

**Problema reportado**: las letras del modo texto VGA se veían demasiado
grandes. Caben solo 25 filas de 80 columnas.

**Fix**: el driver VGA ahora arranca en **80×50** con fuente 8×8 en vez de
80×25 con 8×16. Eso da el **doble de filas** con letras la mitad de altas.
Sin tocar paging, framebuffer ni nada — solo reconfigurar el adaptador VGA.

**Cómo funciona** (`drivers/vga.c` → `vga_set_8x8_mode()`):

1. **Cambiar al modo "carga de fuente"** programando el Sequencer (puerto
   `0x3C4`) y Graphics Controller (`0x3CE`) para mapear `0xA0000` al plano 2
   sin chain-4.
2. **Copiar 2 KB de fuente 8×8 embebida** a la RAM de fuentes del VGA. Cada
   glifo ocupa 32 bytes en el slot (aunque la fuente sea solo 8). La fila 7
   (última) se fuerza a `0` para dejar 1 scanline de **respiro** entre líneas.
3. **Restaurar el modo texto** (Map Mask = planos 0+1, GC apunta a `0xB8000`).
4. **Reprogramar el CRTC** (puerto `0x3D4`): Maximum Scan Line register
   `0x09` con bits 0-4 = 7 (cada carácter ocupa 8 scanlines, no 16). El
   panel sigue siendo 400 scanlines, así que `50 × 8 = 400` cabe exacto.

```c
void vga_init(void)
{
    vga_set_8x8_mode();   /* primero el modo VGA */
    cur_color = vga_entry_color(VGA_LIGHT_GREY, VGA_BLACK);
    vga_clear();
}
```

**Ajustes derivados**:
- `VGA_HEIGHT` en `drivers/vga.h` cambió de `25` a `50`.
- `top_u.c`: ahora dibuja hasta 42 procesos (antes 18) y limpia hasta fila 49.
- `edit_u.c`: `SCREEN_H = 50`, `CONTENT_H = 48` (más espacio para edición).

**Detalle sobre la fuente embebida**: usé la fuente IBM PC VGA 8×8 estándar
(dominio público) que viene en muchos kernels osdev. Cubre ASCII printable
(0x20-0x7E) y los símbolos de pseudo-gráficos (0x00-0x1F). Los caracteres no
ASCII (0x80+) salen como cajas vacías — los coreutils solo usan ASCII así
que no afecta nada.

**Compatibilidad**: funciona perfecto en QEMU. En hardware real puede verse
con fuentes ligeramente distintas si la BIOS sobrescribe la RAM de fuentes
después del boot, pero el modo VGA estándar lo respeta. Si quieres volver al
modo 80×25 original, comenta la llamada `vga_set_8x8_mode()` en `vga_init()`.

### Fase 9 — tcc puro en ring 3 + IDE con syntax highlighting (v0.2.6)

**Antes**: `/bin/tcc` era un wrapper userland (~30 líneas) que llamaba al
syscall `SYS_TCC_COMPILE`. El compilador real (`shell/tcc.c`, 1786 líneas)
corría en kernel mode (ring 0). El binario producido sí era ring 3, pero el
proceso de compilación no.

**Ahora**: hay **dos** compiladores:
- **`/bin/tcc`** — port completo del compilador a ring 3 puro. 23 KB. Es el
  mismo código C subset (int, char, if, while, for, structs, arrays, ++/--,
  break/continue, strings, etc.) que el built-in, pero **corre 100% en CPL=3**.
- **`/bin/tcck`** — el wrapper viejo, mantenido por compat (es ~10× más rápido
  arrancando porque no carga el ELF del compilador, solo llama un syscall).

**El port** (`user/tccu/tcc_main.c`, 1994 líneas):

1. Copia de `shell/tcc.c` sin tocar la lógica del compilador.
2. Añadí un **shim userland** de 167 líneas que reemplaza las 15 APIs del
   kernel que tcc.c usaba:
   - **`kmalloc`/`kfree`/`krealloc`** → bump allocator de 256 KB en BSS
     userland (suficiente para el text buffer + el output ELF; tcc nunca
     libera nada, así que no falta el free real)
   - **`kprintf("...%d %s %x...", a, b, c)`** → macro varargs casera que
     mete los args en un `long[]` y los pasa a un `kprintf_internal()`
     que parsea el formato
   - **`vfs_read/resolve/create/write`** → wrappers sobre `readfile()` y
     `writefile()` (los syscalls de fs que ya existían)
   - **`strcmp/strncmp/strcpy/strncpy/strlen/memcpy/memset`** → versiones
     locales (no están en `trinux.h`)
3. Reemplazado el `tcc_compile(src, out)` por un `main(argc, argv)` que
   parsea args y delega.
4. Como el código generado **ya emitía `int 0x80` directamente** (los
   built-ins del compilador eran inline syscalls), el binario producido
   funciona idéntico al producido por `tcck`.

**Verificación**:
```
$ tcc /root/slow.c
tcc: compiled 128 bytes code + 28 bytes data + 0 bytes bss
     -> /root/slow (240 file, 240 mem)
tcc[ring3]: success
$ /root/slow
slow: starting
(4 segundos)
slow: done
```

### Fase 10 — Code IDE `/bin/cide` con syntax highlighting (v0.2.6)

Nuevo programa: **`/bin/cide`** — editor estilo "VSCode-mini" para terminal.
Hereda de `/bin/edit` pero añade:

**Syntax highlighting** para archivos `.c`:
- 🔵 **Keywords** (`int char void if else while for do return break continue static const struct typedef enum sizeof unsigned signed long short`) en `LIGHT_BLUE`
- 🟦 **Identifiers** (nombres de funciones/variables) en `LIGHT_CYAN`
- 🟢 **Strings** `"..."` y chars `'.'` en `LIGHT_GREEN`
- 🟡 **Numbers** (decimal, hex `0x..`, float) en `YELLOW`
- ⬛ **Comments** `// ...` y `/* ... */` (incluso multilinea entre líneas) en `DARK_GREY`
- 🟣 **Preprocesador** (`#include`, `#define`, ...) en `LIGHT_MAGENTA`
- ⚪ **Punctuation** (`{ } ( ) ; ,`) en `WHITE`

**Atajos integrados**:
- **`Ctrl-B`** → guarda el archivo y **compila** con tcc (usa el syscall
  `SYS_TCC_COMPILE`, así el editor no se desmonta). Muestra "BUILD OK" o
  "BUILD: compilation failed" en la barra inferior.
- **`F5`** o **`Ctrl-R`** → corre el binario compilado (mismo nombre sin `.c`).
  Sale del modo full-screen, spawnea el binario en ring 3, espera una tecla, y
  vuelve al editor sin perder el buffer.
- **`F1`** o **`?`** → ayuda interactiva con la lista de atajos y la leyenda
  de colores.
- **`Ctrl-S`** / **`Ctrl-X`** / **`Ctrl-Q`** → como en edit (save / save+exit / quit).

**Detalles técnicos**:
- 200 líneas × 80 columnas máximo
- Status bar superior: `cide <archivo> [modified] [C mode]`
- Status bar inferior: atajos + posición (line/total, col)
- El lexer es **single-line** pero mantiene un flag `in_block_comment` entre
  líneas, así los `/* ... */` multilinea se pintan correctamente
- Cuando el archivo no termina en `.c`, el highlight se desactiva (modo
  "plain text") y se imprime sin colores

**Flujo típico de uso**:
```
$ cide /root/hello.c
   (editas con flechas + letras, ves los colores en tiempo real)
   Ctrl-B          → BUILD OK: compiled to /root/hello
   F5              → corre /root/hello, ves la salida
   Ctrl-X          → guarda y sale
$ /root/hello       (también puedes correrlo desde fuera)
```

### Fase 11 — Driver de pantalla dual VGA texto + VBE framebuffer (v0.2.7)

**Antes**: el kernel sólo soportaba modo texto VGA (`0xB8000`), con una
resolución fija de 80×25 (o 80×50 con la fuente 8×8 de la Fase 8). En
hardware moderno sin BIOS VGA legacy esto a veces ni funciona.

**Ahora**: hay **modo dual** automático:
- **Al arrancar**, si GRUB ofrece framebuffer (modo gráfico VBE), el kernel
  conmuta al driver `drivers/fb.c` que pinta texto pixel-por-pixel.
- **Si no hay framebuffer**, fallback automático al driver VGA texto clásico.
- **La API pública `vga_*` no cambia** — todos los programas (shell, ls,
  cide, tcc, edit, top, etc.) siguen funcionando idéntico. Solo cambia el
  backend silenciosamente.

**Resolución**: **auto** (lo que GRUB negocie con la BIOS/VBE). En QEMU es
1280×800×32; en hardware real puede ser 1024×768, 1920×1080, etc. — el
kernel se adapta a lo que le den.

#### Implementación

1. **Multiboot header** (`boot/boot.asm`): añadido bit `FB_VIDMODE` (0x4) +
   campos `mode_type, width, height, depth` puestos a 0 para que GRUB elija
   la mejor resolución disponible. El header pasa de 12 a 48 bytes.

2. **`kernel/multiboot.h`**: extendido el `multiboot_info_t` con los campos
   `framebuffer_addr/pitch/width/height/bpp/type` que GRUB rellena cuando
   activa modo gráfico (flag bit 12 = `0x1000` = `MULTIBOOT_FLAG_FB`).

3. **`drivers/fb.c`** + **`drivers/fb.h`** (nuevos, ~250 líneas):
   - `fb_init(magic, info)` parsea el framebuffer info, valida que sea
     RGB de 32 bpp, y mapea las páginas físicas del FB al identity-map
     del kernel via `vmm_map_page()` (porque el FB suele estar en `0xFD000000`,
     fuera de los primeros 256 MiB que ya están mapeados).
   - `fb_putchar(c)` renderiza el carácter pixel-por-pixel usando la fuente
     VGA 8×8 que ya estaba embebida en `drivers/vga.c` (ahora exportada como
     `extern const uint8_t vga_font_8x8[2048]`).
   - `fb_set_color(c)` traduce los 16 colores VGA a RGB888 con una paleta
     fija (`vga_palette[16]`).
   - `fb_scroll()` hace memcpy del framebuffer entero subiendo `CELL_H=8`
     píxels — lento (~30 ms para 1280×800) pero correcto.
   - Expone `fb_put_pixel(x,y,rgb)` y `fb_get_pixel(x,y)` para apps gráficas
     futuras (modo C de los syscalls).

4. **`drivers/vga.c`**: pequeña refactorización. Cada función pública (`vga_putchar`,
   `vga_clear`, `vga_scroll`, `vga_set_cursor`, `vga_set_color`) ahora hace:
   ```c
   if (g_use_fb) { fb_putchar(c); return; }
   /* ... código VGA texto original ... */
   ```
   Cero impacto cuando `g_use_fb=false` (modo texto sigue como siempre).

5. **`display_init(magic, info)`** (nueva función en `vga.c`): se llama
   desde `kernel_main` **después** de `vmm_init()` (porque `fb_init` necesita
   `vmm_map_page` para mapear el FB en addr alta). Si `fb_init` retorna true,
   pone `g_use_fb=true` y todas las llamadas posteriores van al FB. Si
   retorna false, el modo VGA texto sigue activo sin cambios.

#### Decisiones de diseño y limitaciones

- **Solo bpp=32 RGB**. Si GRUB nos da otro formato (8 bpp paletizado, 16 bpp
  RGB565, 24 bpp packed) caemos al fallback VGA texto. Cubrir todos los
  bpps duplicaría el código de `fb_putchar` para poco beneficio.
- **No hay double buffering**. Cada `putchar` pinta directo al FB visible.
  Funciona porque el scroll es la única operación realmente lenta.
- **`fb_scroll` es lento (~30 ms en 1280×800)**. Suficiente para uso normal
  (tipear, ver `ls`, etc.). Si compilamos algo grande con `tcc` y produce mucho
  output, se nota. Optimización futura: scroll por DMA o hacer scroll por
  ajuste de panel offset (CRTC).
- **No usamos VBE 2.0 / 3.0 directamente** — confiamos en lo que GRUB
  negocie con la BIOS. Eso significa que **en hardware sin BIOS VGA**
  (algunos laptops UEFI puros) puede no haber framebuffer disponible y se
  cae a modo texto automáticamente.

#### Capturas

`screenshots-fase11/trinux-fb.png` — modo gráfico 1280×800, se ve `[OK]
Framebuffer (VBE) graphics mode active` al boot, `neofetch` con ASCII
art coloreado, prompt verde, `ls /bin` con 65 binarios en 160 columnas.

#### Para tests/debug

Si quieres forzar modo texto VGA (saltarte el framebuffer):
- Edita `boot/boot.asm`, comenta `FB_VIDMODE` del `MBFLAGS`, recompila.
- O en GRUB añade `text` al cmdline (futuro: leerlo del kernel cmdline).

#### Lo que se queda para futuras fases

- **Apps gráficas reales** (no solo texto sobre FB): exponer `fb_put_pixel`
  via syscall y escribir un programa demo que dibuje líneas/círculos.
- **Doble buffer** para evitar tearing al hacer scroll de mucho texto.
- **Soporte para más bpps** (8/16/24).
- **Cursor parpadeante** estilo terminal real (hoy no parpadea en modo FB).

### Fase 12 — Detección SMP via ACPI MADT (v0.2.8) — *primer paso multicore*

**Contexto importante**: el usuario pidió "que use múltiples cores para no
calentar tanto". Le expliqué que **eso es un mito** — usar más cores
calienta más, no menos (especialmente bajo QEMU, donde cada vCPU emulada =
otro thread del host). Pero como sí quería correr Trinux en hardware real
con multicore, vamos por SMP en fases.

**Esta fase (1 de 5)** SOLO detecta los cores, no los arranca. Es lo
mínimo para verificar que el kernel ve los APs (Application Processors).

#### Implementación

1. **`cpu/smp.h`** + **`cpu/smp.c`** (nuevos, ~250 líneas):
   - `smp_detect()` parsea ACPI MADT siguiendo el flujo estándar:
     1. Busca firma `"RSD PTR "` en EBDA (0x80000-0x9FFFF) y BIOS ROM (0xE0000-0xFFFFF)
     2. Verifica checksum del RSDP
     3. Lee el RSDT cuya dirección está en `rsdp->rsdt_addr`
     4. Recorre las entradas del RSDT buscando una con firma `"APIC"` → eso es la MADT
     5. Recorre las entradas MADT: tipo 0 = LAPIC (un core), tipo 5 = LAPIC base override
   - Si **cualquier paso falla** (sin ACPI, sin LAPIC, checksum malo, etc.),
     devuelve 1 core (solo BSP) y el kernel sigue como single-core de siempre.
2. **Mapeo de páginas ACPI**: el RSDT en QEMU está típicamente en
   `0xBFFExxxx` (~3 GB), **fuera del identity-map de 256 MB** que el kernel
   tiene por defecto. Si no mapeamos esa página explícitamente, leerla causa
   #PF. Solución: `map_acpi_page()` que usa `vmm_map_page()` igual que hicimos
   con el framebuffer en la Fase 11.
3. **`kernel/kernel.c`**: llamada a `smp_detect()` después de `display_init()`,
   imprime `[OK] SMP: detected N CPU cores (LAPIC @ XXXX), BSP only running`.
4. **`SYS_SMP_INFO` (syscall #65)**: expone la lista de cores detectados a
   userland (n_cpus, online, lapic_base, apic_ids[], bsp_apic_id).
5. **`/bin/lscpu`** (nuevo coreutil ring 3): imprime tabla legible:
   ```
   $ lscpu
   Architecture:        i386 (x86 32-bit protected mode)
   CPU(s):              4
   On-line CPU(s):      0 (BSP only — APs detected but not started)
   BSP APIC ID:         0
   LAPIC base address:  0xfee00000

   Detected cores:
     #    APIC ID    Status
     0    0          RUNNING (BSP)
     1    1          halted (AP, awaiting wake-up)
     2    2          halted (AP, awaiting wake-up)
     3    3          halted (AP, awaiting wake-up)
   ```

#### Verificado en QEMU

```bash
qemu-system-i386 -smp 1    -> "SMP: single-core"
qemu-system-i386 -smp 2    -> "SMP: detected 2 CPU cores"
qemu-system-i386 -smp 4    -> "SMP: detected 4 CPU cores"
qemu-system-i386 -smp 8    -> "SMP: detected 8 CPU cores"  (= SMP_MAX_CPUS)
```

#### Lo que sigue (NO está en esta fase)

- **Fase 2 SMP**: trampolín AP — código que despierta los otros cores via
  IPI/SIPI sequence, los pasa de real mode → protected mode → C, y los
  pone en un loop `sti; hlt`.
- **Fase 3 SMP**: per-CPU data (cada core tiene su GDT/TSS/kernel stack).
- **Fase 4 SMP**: spinlocks reales + scheduler thread-safe (refactor masivo).
- **Fase 5 SMP**: distribución de procesos entre cores.

**Total estimado**: 4-5 tandas más para completar SMP funcional. Cada fase
entregable independientemente.

#### Honestidad sobre el calor

Reitero: **multicore no enfría tu CPU**. Si tu laptop se calienta corriendo
Trinux en QEMU, las soluciones reales son:
- `qemu-system-i386 -enable-kvm ...` → virtualización hardware (10× menos CPU host)
- Reducir `-m 512M` a `-m 128M`
- Compilar el kernel con `-Os` en vez de `-O2`

En hardware real con multicore SÍ tiene sentido distribuir trabajo entre
cores. Pero la *temperatura* depende del TDP del chip, no del número de
cores activos (uno saturado calienta similar a 4 al 25% cada uno).

### Fase 13 — Optimizaciones para hardware real (HP Stream 14) (v0.2.9)

**Contexto**: el usuario tiene una HP Stream 14 (Intel Atom/Celeron Bay Trail,
1366×768, eMMC, batería, BIOS legacy). Esta máquina es **el target ideal**
para Trinux: BIOS legacy completa, panel estándar 16:9, sin TPM/Secure Boot
complicados, Atom de bajo TDP que casi no se calienta.

Esta fase añade ajustes específicos para que Trinux corra **mejor en hardware
real**, no solo en QEMU.

#### Cambios

1. **Multiboot header pide 1366×768×32 explícito** (`boot/boot.asm`):
   ```nasm
   dd 0       ; mode_type = linear framebuffer
   dd 1366    ; width nativo del panel HP Stream 14
   dd 768     ; height
   dd 32      ; depth
   ```
   GRUB intenta dar exactamente esta resolución. Si no la tiene, da "lo mejor
   disponible" (no falla — fallback automático).

2. **Fix térmico crítico** (`kernel/kernel.c`):
   Había un `for (volatile int i = 0; i < 50000000; i++);` como delay tonto
   en el loop de reinicio del shell. **Ese busy-wait calentaba el CPU al 100%**
   durante el delay (en hardware real, no en QEMU). Reemplazado por `sleep(1000)`
   que usa el timer del PIT + `sti; hlt` → el CPU baja a low-power.

3. **`SYS_FB_INFO` (syscall #66)**: expone resolución, bpp, addr del framebuffer
   a userland. Útil para diagnóstico en hardware real (saber qué te dio GRUB).

4. **`/bin/screeninfo`** (nuevo): comando que imprime el modo de video con
   formato legible y un "Matched profile" que identifica resoluciones comunes:
   ```
   $ screeninfo
   Display mode:        GRAPHICS (VBE framebuffer)
   Text grid:           160 cols x 100 rows
   Resolution:          1280 x 800 pixels, 32 bpp
   Framebuffer addr:    0xfd000000
   Pitch:               5120 bytes/scanline
   Framebuffer size:    4000 KiB
   Matched profile:     QEMU default (1280x800)
   ```
   En tu HP dirá `Matched profile: HP Stream 14 (1366x768 nativo)`.

5. **`/bin/sysinfo`** (nuevo): dashboard ejecutivo con TODA la info del sistema
   en un comando. CPU + Display + Memory + Storage + Battery + Time + System,
   con colores. Pensado como **primer comando a correr al arrancar en hardware
   nuevo** para ver de un vistazo qué reconoció el kernel:
   ```
   ================================================
     Trinux System Info
   ================================================

   [CPU]
     Cores detected:  4
     Cores online:    1 (BSP only)
     BSP APIC ID:     0

   [Display]
     Mode:            graphics (VBE)
     Resolution:      1280 x 800 @ 32 bpp
     Text grid:       160 x 100

   [Memory]
     Total:           511 MiB
     Used:            256 MiB
     Free:            255 MiB

   [Storage]
     Disk:            present
     Total:           431 MiB
     Used:            194 MiB

   [Battery]
     No battery detected (AC-only / VM)
     (en tu HP: "Status: discharging, Charge: 87 %")

   [Time]
     Uptime:          3 seconds

   [System]
     Hostname:        trinux
     Current user:    root (uid=0)
     Shell PID:       6
   ================================================
   ```

#### Test en QEMU

Funciona en QEMU igual que antes — la única diferencia es que `screeninfo`
ahora dice `Matched profile: QEMU default (1280x800)` porque QEMU ignora
nuestro request de 1366×768 y da su default.

#### Lo que esperamos en tu HP Stream 14

- `[OK] Framebuffer (VBE) graphics mode active` en boot
- Resolución 1366×768 (o lo que tu BIOS negocie)
- `sysinfo` mostrará tu batería real, eMMC si el driver xHCI/AHCI lo detecta
- `lscpu` debería detectar 2 cores (Bay Trail dual-core / 4 cores quad)
- Sin calentamiento (el fix térmico ayuda incluso en tu chip de bajo TDP)

### Tabla resumen de las fases nuevas

| Aspecto | v0.2.0 (original) | v0.2.9 (actual) |
|---|---|---|
| Shell | Built-in kernel ring 0 | **`/bin/sh` ring 3** (PID 5) |
| Comandos | 66 built-ins kernel | **65 ELFs en `/bin/`** ring 3 + `tcc/tcck/cide/edit/top` |
| Syscalls | 10 | **64** |
| Aislamiento | Ninguno (todo ring 0) | **CPL=3 real** (cs=0x1B, GPF si toca IO/MSR) |
| Pipes | Sí (en shell ring 0) | Sí (en shell ring 3, vía tmpfiles) |
| Multi-stack | No | Sí (4 niveles, evita colisiones) |
| Prompt | Colores | Colores + input verde + ls con tipos |
| Modo texto VGA | 80×25 (fuente 8×16) | 80×50 (fuente 8×8) en fallback |
| **Modo gráfico** | No (solo VGA texto) | **VBE framebuffer auto** (1280×800 en QEMU, lo que el monitor soporte en real) |
| Compilador C | Built-in kernel | **`/bin/tcc` puro ring 3** (+ `/bin/tcck` wrapper) |
| IDE / editor | edit simple | **`/bin/cide`** con syntax highlight + ^B + F5 |
| **SMP** | No (single-core) | **Detecta hasta 8 cores via ACPI MADT** (`/bin/lscpu`); arranque AP es Fase 2 |
| **Hardware real** | Solo QEMU testeado | **Optimizado para HP Stream 14**: 1366×768 nativo, fix térmico, `/bin/screeninfo` y `/bin/sysinfo` para diagnóstico |

### Cómo verificar todo

```sh
# 1) Confirmar aislamiento ring 3
ringtest                        # intenta 'cli' → #GP, kernel mata el proceso

# 2) Pipes
ls /bin | wc                    # 1 65 422
cat /etc/passwd | grep root | wc

# 3) Redirección + cp/mv (bug 1 fix)
echo HOLA > /tmp/a
echo OLD > /tmp/b
cp /tmp/a /tmp/b
cat /tmp/b                      # → HOLA (no OLD)

# 4) su funciona en ambas direcciones (bug 2 fix)
whoami                          # root
su user                         # password: user
whoami                          # user
su root                         # password: root
whoami                          # root

# 5) top / edit / tcc
top                             # 'q' para salir
edit /root/hi.c                 # escribe código, Ctrl-X guarda y sale
tcc /root/hi.c                  # compila → /root/hi
/root/hi                        # ejecuta tu binario en ring 3

# 6) Colores
ls /                            # directorios en azul
ls /bin                         # ejecutables en verde
```

### Cómo está organizado el nuevo userland

```
user/
├── trinux.h               # ABI única: 64 syscalls + libc mini para ring 3
├── coreutils/
│   ├── build.sh           # compila los 64 .c → ELF32 → .h embebido
│   ├── crt0.S, user.ld    # startup + linker script para ring 3
│   ├── *.c                # 64 fuentes (cp, mv, top, edit, tcc, ls, ...)
│   ├── hdrs/*.h           # AUTO: cada ELF como blob xxd -i
│   └── user_bins.h        # AUTO: tabla {nombre, blob, size} para el kernel
├── usersh/
│   ├── sh.c               # shell ring-3 con pipes y refresh_identity
│   └── sh_elf.h           # AUTO: ELF del shell
└── userprog.c             # demo legacy (usertest, badboy) — sigue funcionando
```

Adicionalmente:
- **`FASE5_REGEN.sh`** (raíz del repo) — script anti-poda: si por alguna razón
  los `.c` de coreutils o `sh.c` se borraron (snapshot del workspace, limpieza),
  basta correr `bash FASE5_REGEN.sh` y todo se regenera.

### Cómo recompilar todo

```sh
cd Kernel-Trinux-V2
bash FASE5_REGEN.sh             # si faltan archivos en user/usersh/ o coreutils/
bash user/coreutils/build.sh    # 64 ELFs ring-3 + /bin/sh
make                             # kernel (embebe los ELFs)
bash make-usb-image.sh 512       # mykernel-usb.img de 512 MB
qemu-system-i386 -drive file=mykernel-usb.img,format=raw,if=ide -m 512M
```

### Volver al shell viejo (debug)

En el menú GRUB → `e` → en la línea `multiboot /boot/mykernel.bin` añadir `oldsh` →
`Ctrl-X`. Eso lanza el shell legacy ring-0 con sus 66 built-ins originales.

### Lo que aún NO está

- **Address spaces por proceso** (CR3 distinto por task con `copy_from_user`/
  `copy_to_user`). Hoy el ring 3 protege **privilegio** (no puede `cli`/`outb`/
  MSR), pero los procesos comparten el identity-map de 256 MiB del kernel.
  El "multi-stack por nivel" de Fase 4 resuelve el problema práctico de spawn
  anidado sin necesitar CR3 separados.
- **`fork()`/`execve()` reales con COW.** Usamos `posix_spawn` semantics
  (un syscall `SYS_SPAWN` que arranca y espera al hijo).
- **`tcc` puro en ring 3** — sigue siendo built-in del kernel; el `/bin/tcc`
  es un wrapper userland que invoca el built-in vía syscall.

Ver detalles técnicos:
- [`RING3_CHANGES.md`](RING3_CHANGES.md) — Fase 1 original
- [`RING3_FASE2.md`](RING3_FASE2.md) — Fase 2 original
- Esta sección — Fases 3-7 (todo lo posterior).

---

## 🆕 Novedades v0.2.1 — Ring 3 real (Fases 1-4)  (HISTÓRICO)

Esta versión migra el sistema de **kernel-mode-only** a un modelo
**ring 0 + ring 3** real, con shell y comandos corriendo en CPL=3.

### Lo que cambió

| Antes (v0.2.0)                       | Ahora (v0.2.1)                                  |
|---|---|
| Shell vivía en el kernel (ring 0)   | **`/bin/sh` es un ELF userland en ring 3 (PID 5)** |
| Built-ins en C dentro del kernel    | **61 binarios `/bin/*` + 3 aliases**, todos ELF ring 3 |
| 10 syscalls                          | **48 syscalls** (open/read/write/spawn/login/...) |
| Sin aislamiento de privilegio       | **CPL=3 real** (cs=0x1B): `cli`/`outb`/IO/MSR prohibidos |
| Sin pipes en ring 3                 | **Pipes `cmd1 \| cmd2 \| cmd3`** (hasta 8 stages) |
| 1 user stack global                 | **Stack por nivel de anidamiento** (L0..L3) |

### Comandos en `/bin/` ejecutándose en ring 3

```
ls cp mv rm rmdir mkdir touch cat stat chmod chown
head tail wc grep sort uniq cut tee write
pwd basename dirname which env find tree
date free df ps neofetch uptime hostname uname id whoami users groups
echo true false yes seq calc hexdump sleep clear cls color
login logout su useradd passwd
reboot shutdown halt sync kill renice battery
ringtest   ← prueba que CPL=3 es real (intenta `cli` → #GP, kernel mata el proceso)
```

### Verificar el aislamiento

```
root@trinux:~# ringtest
[ringtest] Soy un programa userland. PID=10
[ringtest] Intentando ejecutar 'cli' (instrucción privilegiada)...

*** CPU EXCEPTION ***
  General Protection Fault (interrupt 13)
  eip=080480d5 cs=001b  eflags=00010246 ds=0023
                ^^^^^^^                  ^^^^^^^
                CPL=3 user code segment   user data segment
  (terminating ring-3 program; kernel continues)
root@trinux:~#                ← el shell sobrevive
```

### Pipes funcionando

```
$ ls /bin | wc                          → 1 65 422
$ cat /etc/passwd | grep root           → root:x:0:0:root:/root:/bin/sh
$ seq 1 20 | wc                         → 20 20 51
$ cat /etc/passwd | head -n 1 | wc      → 1 1 30   (3 stages)
```

### Cómo está organizado el nuevo userland

```
user/
├── trinux.h             # ABI única: 48 syscalls + libc mini (ring-3)
├── coreutils/
│   ├── build.sh         # compila los 61 .c → ELF32 → .h embebido
│   ├── crt0.S, user.ld  # startup + linker script para ring 3
│   ├── *.c              # 62 fuentes (echo, cat, ls, ps, neofetch, ...)
│   ├── hdrs/*.h         # generado: cada ELF como blob xxd -i
│   └── user_bins.h      # generado: tabla {nombre, blob, size} para el kernel
└── usersh/
    ├── sh.c             # el shell completo ring-3 (login, prompt, pipes)
    └── sh_elf.h         # generado: blob del shell
```

### Recompilar

```sh
# 1. Compila los ELFs userland (necesita gcc-multilib, nasm, xxd)
bash user/coreutils/build.sh

# 2. Compila el kernel (embebe los ELFs en /bin/ al boot)
make

# 3. Genera la imagen USB persistente de 512 MiB
bash make-usb-image.sh 512

# 4. Corre en QEMU
qemu-system-i386 -drive file=mykernel-usb.img,format=raw,if=ide -m 512M
```

### Cómo volver al shell viejo (debug)

Si quieres comparar con el shell legacy en ring 0:

1. En el menú de GRUB, presiona `e` para editar la entrada
2. En la línea `multiboot /boot/mykernel.bin`, agrega `oldsh` al final
3. Presiona Ctrl-X para bootear

### Lo que aún NO está

- **Address spaces por proceso reales** (CR3 distinto por task con `copy_from_user`/`copy_to_user`).
  Hoy el ring 3 protege **privilegio** (no puede `cli`/`outb`/MSR), pero
  todos los procesos comparten el identity-map de 256 MiB del kernel.
  El "multi-stack por nivel" de Fase 4 resuelve el problema práctico de
  spawn anidado sin necesitar CR3 separados.
- `fork()` real con COW (usamos `posix_spawn` semantics: SPAWN + WAITPID).
- `top` interactivo y el editor `nano/edit` siguen siendo built-ins ring 0.

Ver detalles técnicos:
- [`RING3_CHANGES.md`](RING3_CHANGES.md) — Fase 1 (loader ELF a ring 3 + 27 coreutils)
- [`RING3_FASE2.md`](RING3_FASE2.md) — Fase 2 (shell entero a ring 3)
- Fases 3 y 4 (resto de coreutils + pipes + multi-stack) documentadas en esta sección.

---

```
user@trinux:/$ neofetch
      .--.        user@trinux
     |o_o |       OS:     Trinux 0.2.1 (ring 3 shell)
     |:_/ |       Kernel: x86 32-bit protected mode
    //   \ \      Uptime: 42 s
   (|     | )     Shell:  /bin/sh (PID=5, ring 3)
  /'\_   _/`\     Memory: 16/511 MiB
  \___)=(___/     Disk:   0/14336 MiB
```

---

## 🚀 Inicio rápido

### En Linux (QEMU)

```sh
# Instala las dependencias
sudo apt-get install build-essential gcc-multilib nasm qemu-system-x86

# Compila y corre
make          # compila mykernel.bin (~120 KB)
make run      # lo arranca en QEMU con disco de persistencia
```

Luego escribe `help` en el prompt para ver todos los comandos.  Login por
defecto: `user`/`user` o `root`/`root`.

> La compilación usa el `gcc -m32`, `nasm`, `ld` del sistema.  Si tienes
> una toolchain cruzada `i686-elf` puedes usar:  `make USE_CROSS=1`

### En hardware real (USB bootable) 💾

```sh
# Instala también los paquetes para generar la imagen UEFI
sudo apt-get install grub-pc-bin grub-efi-ia32-bin xorriso mtools

# Genera la imagen (ajusta el tamaño a tu USB)
./make-usb-image.sh 14336    # genera mykernel-usb.img de 14 GB

# Grábala a la USB (¡CUIDADO: verifica con lsblk antes!)
sudo dd if=mykernel-usb.img of=/dev/sdX bs=4M status=progress conv=fsync
```

La imagen generada es **híbrida**: contiene tanto MBR (Legacy BIOS) como
una partición ESP con GRUB EFI de 32 bits, así que bootea en:

| Tipo de PC | Funciona? |
|---|---|
| PC con BIOS Legacy / CSM | ✅ |
| PC con UEFI (sin Legacy) | ✅ |
| Mac Intel | ✅ (vía partición HFS+) |
| QEMU | ✅ |

Al bootear verás un menú GRUB con dos opciones:
- **Trinux** — boot normal (pide login)
- **Trinux (single-user / recovery)** — root directo sin contraseña

El kernel detecta automáticamente el controlador de disco:

```
[ata] no legacy IDE; probing PCI for AHCI...
[ata] no AHCI; probing PCI for xHCI (USB)...
[xhci] found at 0:14.0 BAR0=f7100000
[xhci] USB disk ready: 28311552 sectors (13824 MiB)
[ OK ] Disk detected via xHCI/USB (13824 MiB)
```

### Modo recovery / single-user (como en Linux real)

Si olvidaste la contraseña o no puedes entrar:

1. Reinicia la PC
2. En el menú GRUB (tienes 3 segundos), selecciona
   **"Trinux (single-user / recovery)"**
3. O hazlo manualmente como en Linux:  presiona `e` sobre la entrada
   "Trinux", busca la línea `multiboot /boot/mykernel.bin`, agrégale
   `single` al final, y presiona **Ctrl-X** para bootear
4. Entras directo a un shell de root sin contraseña
5. Usa `passwd root` y `passwd user` para resetear las passwords
6. Ejecuta `sync` para guardar los cambios al disco
7. Ejecuta `reboot` y bootea normal

Esto es exactamente el mismo mecanismo que usan Debian, Ubuntu, Arch y
todas las distribuciones de Linux reales.

### En Termux (Android) 📱

Si quieres correr Trinux en tu teléfono Android, mira **[TERMUX_QUICKSTART.md](TERMUX_QUICKSTART.md)**:

```sh
pkg install qemu-system-i386-headless

# Modo "clásico" — pantalla VGA emulada en curses
qemu-system-i386 -drive file=trinux.img,format=raw,if=ide -m 512M -display curses

# 🆕 Modo recomendado para pegar texto largo desde el portapapeles
qemu-system-i386 -drive file=trinux.img,format=raw,if=ide -m 512M \
                 -display none -serial mon:stdio
```

> 💡 El **modo serial** evita el cuello de botella de `-display curses`
> al inyectar scancodes PS/2 (la cola interna de QEMU descarta bytes en
> pastes muy grandes y deja Shift/CapsLock pegado).  Con `-serial
> mon:stdio` los bytes del clipboard van directamente al UART del guest,
> sin traducción a scancodes y sin pérdidas.  Ver Fase 16 más abajo.

---

### Requisitos de compilación

| Herramienta | Paquete Debian/Ubuntu | Para qué |
|---|---|---|
| GCC (32-bit) | `gcc` + `gcc-multilib` | Compilar el kernel en C |
| NASM | `nasm` | Ensamblar los `.asm` (boot, IDT, IRQ, etc.) |
| GNU ld | `binutils` | Enlazar el binario ELF con `linker.ld` |
| QEMU | `qemu-system-x86` | Emular y probar |
| GRUB (Legacy) | `grub-pc-bin` | Boot BIOS en la imagen USB |
| GRUB (EFI) | `grub-efi-ia32-bin` | Boot UEFI en la imagen USB |
| xorriso + mtools | `xorriso mtools` | Generar la ISO/imagen híbrida |

### Targets del Makefile

| Target | Qué hace |
|---|---|
| `make` | Compila + enlaza `mykernel.bin` |
| `make run` | Arranca en QEMU con disco IDE de 128 MiB |
| `make run-curses` | Igual pero con display tipo terminal (curses) |
| `make iso` | Genera `mykernel.iso` booteable con GRUB |
| `make run-iso` | Genera y arranca la ISO |
| `make clean` | Borra `.o`, binarios e ISO |
| `./make-usb-image.sh N` | Genera `mykernel-usb.img` de N MiB (Legacy+UEFI) |

---

## ✨ Lo que tiene implementado

### Fase 1 — Boot y modo protegido

- Header **Multiboot v1** (`boot/boot.asm`), kernel enlazado en la dirección
  física 1 MiB como dicta la especificación.
- Setup de **GDT** (`boot/gdt.c` + `boot/gdt_flush.asm`): 6 entradas —
  null, ring0 code/data, ring3 code/data, TSS.  La TSS se configura con
  `ss0:esp0` para que el CPU sepa qué stack de kernel usar cuando un
  programa en ring 3 hace un syscall o recibe una interrupción.
- **Parser de kernel command line**: lee la cmdline que GRUB pasa vía el
  struct multiboot info.  Reconoce `single`, `emergency`, `init=/bin/bash`
  e `init=/bin/sh` para activar el modo single-user (root sin contraseña).
- Menú GRUB con timeout de 3 segundos y opción de recovery integrada.
- Bootloader MBR clásico en modo real de 512 bytes incluido como referencia
  educativa (`boot/mbr_boot.asm`) — no se usa en producción pero demuestra
  cómo se activa A20, se carga la GDT y se entra a modo protegido a mano.

### Fase 2 — Driver VGA texto (`drivers/vga.*`)

- Acceso directo al buffer de video en `0xB8000` (modo texto 80×25).
- 16 colores de primer plano y 16 de fondo.
- Cursor por hardware (registros del controlador VGA CRTC).
- Scrolling automático cuando se llena la pantalla.
- Manejo de caracteres especiales: `\n` (nueva línea), `\t` (tab de 8
  espacios), `\b` (backspace con borrado visual), `\r` (retorno de carro).
- `vga_printf` formateado, `vga_print_color` para texto coloreado.
- Captura de output a buffer (para implementar pipes en la shell).

### Fase 3 — Interrupciones (`cpu/*`)

- **IDT** (Interrupt Descriptor Table) con 256 entradas.
- **ISR 0-31** (CPU exceptions): cada una con su mensaje descriptivo
  ("Division By Zero", "Page Fault", "General Protection Fault", etc.)
  y dump completo de registros (EAX-EDI, EIP, CS, EFLAGS, error code).
- **PIC 8259** remapeado de IRQ 0-15 a interrupciones 32-47 para no
  chocar con las CPU exceptions.
- Registro dinámico de handlers por IRQ, EOI (End Of Interrupt) correcto
  para master y slave PIC.
- Si un fault viene de ring 3 (código de usuario), el kernel mata solo
  ese programa sin crashear — aislamiento real.

### Fase 4 — Entrada (teclado y timer)

- **Teclado PS/2** (`drivers/keyboard.*`): scancode set 1 (el clásico de
  la IBM PC AT).  Decodifica Shift, Caps Lock, Ctrl, Alt, flechas,
  F1-F12, Home, End, Delete, Insert.  Buffer circular de 256 caracteres.
  `getchar` bloqueante (con HLT para no quemar CPU) y `trygetchar`
  no-bloqueante.
- **Timer PIT** (`drivers/timer.*`): Programmable Interval Timer configurado
  a 100 Hz.  Provee `sleep()` (en milisegundos), `uptime()` (segundos
  desde boot), `timer_get_ticks()`.  Cuenta idle ticks para el cálculo
  de CPU usage en tiempo real.

### Fase 5 — Memoria (`mm/*`)

- **PMM** (Physical Memory Manager, `mm/pmm.*`): bitmap de frames de 4 KiB.
  Detecta la memoria total vía la info de Multiboot.  `pmm_alloc_frame()`
  y `pmm_free_frame()` para asignar/liberar páginas físicas.

- **VMM** (Virtual Memory Manager, `mm/vmm.*`):
  - Identity-map de los primeros **256 MiB** con 64 page tables estáticas
    (cubre kernel + heap + buffers DMA).
  - Pool de **8 page tables extra** para mapear MMIO en regiones altas del
    espacio de direcciones de 4 GiB (necesario para AHCI ABAR en
    `0xFEBxxxxx` y xHCI BAR en rangos similares).
  - Handler de page fault que muestra dirección, tipo de acceso (read/write,
    user/kernel, present/not-present) y mata solo el proceso si viene de
    ring 3.
  - Soporte para crear/destruir espacios de direcciones separados (para
    futuros procesos con aislamiento de memoria).

- **Heap del kernel** (`mm/kheap.*`): arena estática de 32 MiB en `.bss`.
  Algoritmo first-fit con coalescing de bloques libres adyacentes.
  Magic numbers (`0xA110C8ED` / `0xF8EE8100`) para detectar corrupción.
  API: `kmalloc`, `kcalloc`, `krealloc`, `kfree`, `kmalloc_aligned`
  (alineación a página para buffers DMA), `kheap_stats`.

### Fase 6 — Sistema de archivos (`fs/*`)

- **VFS** (`fs/vfs.*`): capa de abstracción con callbacks por nodo
  (read/write/open/close/readdir/finddir).  Permisos Unix completos:
  - Bits `rwx` para owner, group y others (ej. `chmod 755`).
  - Owner UID/GID por archivo con `chown`.
  - `umask` para permisos por defecto de archivos/directorios nuevos.
  - **Sticky bit** (`chmod 1777`): en `/tmp`, cualquiera puede crear
    archivos pero solo el owner puede borrarlos (como en Unix real).
  - Root (UID 0) bypassa todos los checks de permisos.
  - Proveedor de credenciales dinámico (cambia según qué usuario está
    logueado).

- **RAM filesystem** (`fs/ramfs.*`): árbol inicial con la estructura
  clásica de Unix: `/bin`, `/etc`, `/home/user`, `/tmp` (1777), `/var/log`,
  `/dev`, `/mnt`, `/root` (0700).  Archivos iniciales: `/etc/hostname`,
  `/etc/motd`, `/home/user/readme.txt`.  Dos modos de almacenamiento por
  archivo: RAM (heap del kernel) o disco (bloques de 4 KiB vía blockfs).

- **Resolución de rutas** (`fs/path.*`): normalización completa con
  soporte para `.` (directorio actual), `..` (padre), rutas absolutas y
  relativas, `basename`, `dirname`, `path_join`.

- **Persistencia en disco** (`fs/diskfs.*`): serializa todo el árbol VFS
  (directorios + archivos + permisos + owners) como un snapshot binario
  al final del disco.  Formato propio `MKFS` con superblock + registros
  de nodos en preorder.  Se guarda con `sync` y se restaura automáticamente
  al boot siguiente.  Soporta discos de hasta **2 TiB** sin overflow
  (usa cálculos en MiB con `diskfs_total_mb()`).

- **Block storage** (`fs/blockfs.*`): cuando hay disco presente, el
  contenido de los archivos vive en **bloques de 4 KiB escritos
  directamente en el disco**, leídos y escritos bajo demanda.  Un bitmap
  rastrea la asignación de bloques.  Esto significa que los datos **no
  consumen RAM** y el almacenamiento total está limitado por el tamaño del
  disco, no por la memoria.  En un disco de 14 GB hay ~13.9 GB disponibles
  para archivos.

- **Dispositivos `/dev`** (`fs/devfs.*`):
  - `/dev/sda` — disco crudo (solo root, lectura/escritura byte a byte)
  - `/dev/zero` — devuelve infinitos ceros
  - `/dev/null` — descarta todo lo que se le escriba
  - `/dev/random` — genera bytes pseudoaleatorios
  Todos integrados con el VFS y con permisos correctos.

### Fase 7 — Procesos (`process/*`)

- PCB (Process Control Block) + tabla de hasta 64 procesos.
- Procesos iniciales estilo Unix al boot: PID 1 = `init`, PID 2 =
  `kthreadd`, PID 3 = `mysh` (la shell).
- Cada comando de la shell se registra como proceso temporal con su propio
  PID, visible en `ps` y `top`.
- Esqueleto de scheduler round-robin (`process/scheduler.*`) + rutina de
  context switch en ASM puro (`process/switch.asm`).  Corre
  cooperativamente por defecto para mantener la estabilidad; la
  infraestructura para preemption vía timer IRQ existe pero está
  deshabilitada.

### Fase 8 — Shell (`shell/*`)

- **Prompt** `user@host:/path$` con colores (verde para usuarios normales,
  rojo `#` para root, como en Unix real).
- **Editor de línea** completo: movimiento del cursor con ←/→, Home/End,
  backspace con redibujado, inserción en medio de la línea.
- **Historial** con ↑/↓ (circular, 50 entradas), sin duplicados consecutivos.
- **Tab completion**: completa nombres de comandos (primer palabra) y
  rutas de archivos/directorios (argumentos), con listado de múltiples
  candidatos y extensión por prefijo común.
- **Tokenizer** robusto: comillas dobles para preservar espacios
  (`echo "hola mundo"`), operadores `>` `>>` `<` como tokens separados
  aun sin espacios (`echo hola>archivo`).
- **Pipes** `|`: pipeline multi-etapa con buffers ping-pong de 8 KiB.
  La salida capturada de cada etapa se pasa como stdin a la siguiente.
- **Redirección** `>` (truncar), `>>` (append), `<` (input desde archivo).
- **Aliases**: `alias ll ls -la` expande automáticamente.
- **66+ comandos integrados** (ver tabla más abajo).
- Mensajes de error detallados estilo Unix (`Permission denied`,
  `No such file or directory`, `File exists`, `Is a directory`, `Operation
  not permitted`, etc.).

### Fase 9 — Librerías (`lib/*`)

- `lib/string.*`: set completo de funciones de cadena y memoria —
  `strlen`, `strcmp`, `strncmp`, `strcpy`, `strncpy`, `strcat`, `strncat`,
  `strchr`, `strrchr`, `strstr`, `strtok`, `memcpy`, `memset`, `memcmp`,
  `memmove`, `atoi`, `isdigit`, `isalpha`, `isspace`, `toupper`,
  `tolower`.
- `lib/printf.*`: `kprintf` y `snprintf`/`vsnprintf` con formato completo
  — `%d`, `%u`, `%x`, `%s`, `%c`, `%p`, `%%`, ancho mínimo, padding con
  ceros, alineación izquierda.
- `drivers/rtc.*`: lectura del reloj en tiempo real (CMOS RTC) — año, mes,
  día, hora, minuto, segundo.  Usado por el comando `date`.
- `drivers/serial.*`: salida por COM1 (38400 baud) para debug.  Toda la
  salida del kernel también va al serial, útil para depurar en QEMU con
  `-serial stdio`.

### Fase 10 — Userspace ring 3 + syscalls (`cpu/syscall.*`, `user/*`)

- **TSS** configurada con `ss0:esp0` para transiciones ring3 → ring0.
- **Gate de syscall** `int 0x80` con DPL 3 (accesible desde userspace).
- **`enter_usermode`**: baja a ring 3 con su propio stack de usuario y
  segmentos de código/datos con RPL 3.
- **7 syscalls** implementadas: `SYS_EXIT`, `SYS_WRITE`, `SYS_GETPID`,
  `SYS_YIELD`, `SYS_SLEEP`, `SYS_GETC`, `SYS_UPTIME`.
- **Mini "libc" en ring 3** (`user/userprog.c`): funciones `u_write`,
  `u_write_u` que solo usan `int 0x80`, cero acceso directo a hardware.
- **Aislamiento demostrado**: `usertest bad` intenta ejecutar `cli` en
  ring 3 → el CPU genera un #GP (General Protection Fault) → el kernel
  mata solo ese programa y sigue corriendo.  La shell no crashea.
- Comando `usertest` para la demo amable, `usertest bad` para la demo
  de aislamiento.

### Fase 11 — FAT16 (`fs/fat16.*`, `fs/fat.*`)

- Lectura de particiones FAT16 desde el disco ATA.
- Parser de la tabla de particiones MBR.
- Lectura del BPB (BIOS Parameter Block) y las tablas FAT.
- Navegación de directorios y lectura de archivos FAT16.
- Integración con el VFS para montar particiones FAT16 como subárboles.

### Fase 12 — PCI + AHCI/SATA (discos internos en hardware real)

- **Enumeración PCI** (`drivers/pci.*`): escaneo completo de los 256
  buses × 32 devices × 8 funciones del bus PCI usando el mecanismo 1
  (puertos `0xCF8`/`0xCFC`).  Detecta dispositivos por clase/subclase/
  progIF.  Habilita bus-mastering (bit 2 del registro Command) para DMA.

- **Driver AHCI** (`drivers/ahci.*`): inicialización completa del HBA
  (Host Bus Adapter) según la especificación AHCI 1.0:
  - Mapeo MMIO del ABAR (BAR5 del PCI config space) usando el VMM
    extendido (puede mapear cualquier dirección de 4 GiB, no solo los
    primeros 256 MiB).
  - **BIOS/OS Handoff** (CAP2.BOH): toma control del controlador desde
    el BIOS de forma limpia.
  - Detección de puertos con dispositivos SATA presentes (SSTS.DET +
    SSTS.IPM).
  - **Port rebase**: asigna buffers DMA alineados para Command List
    (1 KiB), Received FIS (256 B) y Command Table (con PRDT).
  - **IDENTIFY DEVICE** para obtener el número total de sectores.
  - Lectura/escritura con **READ/WRITE DMA EXT** (48-bit LBA, soporta
    discos de hasta 128 PiB teóricos).
  - Bounce buffers vía `kmalloc_aligned` para garantizar que los buffers
    DMA estén en la región identity-mapped.

- **Driver de disco unificado** (`drivers/ata.c`): `ata_init()` intenta
  tres backends en cascada:  IDE PIO → AHCI → xHCI.  La API pública
  (`ata_read_sectors`, `ata_write_sectors`, `ata_present`,
  `ata_total_sectors`) no cambia — diskfs, blockfs, devfs, `dd` y `sync`
  funcionan idéntico sin importar qué backend está activo.

### Fase 13 — xHCI / USB 3.0 Mass Storage (persistencia en USB) 🆕

El driver xHCI permite que **la misma USB desde la que booteas sea tu disco
de persistencia**, incluso en laptops modernas donde el único controlador
de almacenamiento accesible desde el kernel es el xHCI (USB 3.0).

- **xHCI Host Controller** (`drivers/xhci.*`): implementación completa
  del stack necesario para hablar con un dispositivo USB Mass Storage:
  - Búsqueda en PCI por clase `0C:03:30` (Serial Bus / USB / xHCI).
  - Mapeo MMIO de 64 KiB para los registros del controlador (Capability,
    Operational, Runtime, Doorbell).
  - Reset y re-inicialización del controlador (`USBCMD.HCRST`).
  - Asignación del **DCBAA** (Device Context Base Address Array) con
    soporte para scratchpad buffers (requeridos por algunos controladores).
  - **Command Ring** (32 TRBs) para enviar comandos al controlador.
  - **Event Ring** (64 TRBs) + Event Ring Segment Table + Interrupter
    para recibir respuestas del controlador vía polling.
  - Detección de puertos con dispositivos conectados y **port reset**.
  - **Enable Slot** + **Address Device** para asignar una dirección USB
    al dispositivo.
  - **Control transfers** en el endpoint 0 para leer Device Descriptor,
    Configuration Descriptor, detectar interfaces y endpoints.
  - **Configure Endpoint** para activar los endpoints Bulk IN/OUT del
    dispositivo de almacenamiento.

- **USB Mass Storage Bulk-Only Transport (BOT)**: protocolo estándar para
  hablar con memorias USB, discos externos, etc.:
  - Envío de **CBW** (Command Block Wrapper) con comandos SCSI vía
    Bulk OUT.
  - Transferencia de datos vía Bulk IN (lectura) o Bulk OUT (escritura).
  - Recepción de **CSW** (Command Status Wrapper) vía Bulk IN para
    verificar el resultado.

- **Comandos SCSI** implementados:
  - `INQUIRY` — identificar el dispositivo.
  - `TEST UNIT READY` — verificar que está listo.
  - `REQUEST SENSE` — limpiar condiciones de error.
  - `READ CAPACITY(10)` — obtener el tamaño del disco.
  - `READ(10)` / `WRITE(10)` — leer/escribir sectores.

- **Resultado**: al bootear una laptop como la HP Stream 14 desde USB,
  el kernel:
  1. Intenta IDE → no hay
  2. Intenta AHCI → encuentra el eMMC interno (puede que funcione o no)
  3. Intenta **xHCI** → encuentra la USB de boot → persistencia completa

### Fase 14 — Batería y monitoreo de CPU en tiempo real 🆕

- **Batería vía ACPI Embedded Controller** (`drivers/acpi_ec.*`):
  - Lee el EC por los puertos estándar ACPI `0x62` (datos) / `0x66`
    (comando/status), usando el protocolo de la especificación ACPI §12.2.
  - Incluye **6 layouts de registros conocidos** para distintos fabricantes
    (HP variante A, HP variante B, Lenovo ThinkPad, Dell, genérico×2).
  - Si ningún layout predefinido funciona, hace un **escaneo automático**
    de registros EC `0x20-0xC0` buscando un valor 1-100 que parezca un
    porcentaje de batería.
  - Reporta: porcentaje (0-100%), estado (cargando/descargando/AC),
    voltaje (mV), capacidad restante (mAh), capacidad total (mAh),
    tasa de descarga (mA).
  - En QEMU (sin EC) reporta "no battery" — no crashea.

- **CPU usage en tiempo real** (`drivers/timer.c`):
  - El timer PIT cuenta **idle ticks** (cuando el CPU ejecuta `HLT`
    esperando interrupciones) vs ticks totales.
  - `timer_cpu_usage()` calcula `100% - idle%` entre cada llamada.
  - Simple pero efectivo para un kernel single-tasking.

- **Integración en la shell**:
  - `top` ahora muestra barras de color para **CPU** (roja), Mem, Heap,
    Disk y **Batería** (roja si <15%, verde si cargando, amarilla si
    descargando).  Se auto-refresca cada ~2 segundos.
  - `neofetch` incluye una línea de batería con porcentaje y estado.
  - Nuevo comando `battery` para ver el estado detallado de la batería
    (porcentaje, AC, voltaje, capacidad, tasa).

### Fase 15 — TCC / TASM: Compilador C y Ensamblador integrados 🆕

Trinux incluye **dos herramientas de desarrollo integradas** en la propia
shell — no necesitas ningún toolchain externo.  Escribes el código con
`edit`, lo compilas/ensamblas con `tcc` o `asm`, y lo ejecutas con `exec`.

#### TCC — Trinux C Compiler (`shell/tcc.c`)

Un **compilador C completo** integrado en el kernel.  Arquitectura
single-pass recursive-descent que compila un subset de C directamente a
código máquina x86 (32-bit) y lo empaqueta en un ELF32 ejecutable.

```sh
edit hola.c           # escribe tu programa C
tcc hola.c            # compila → crea 'hola' ELF32
exec hola             # ejecuta en ring 3
```

**Lenguaje soportado:**

| Feature | Ejemplo |
|---|---|
| Tipos `int`, `char` | `int x = 42;` |
| Variables locales + arrays | `int arr[10]; arr[0] = 5;` |
| Arrays de bytes (`char buf[N]`) 🆕 | `char buf[256]; buf[0] = 65;` |
| **Variables globales** (`.bss`) 🆕 | `int counter; char log[4096];` |
| Funciones con forward refs | `int foo(int a, int b) { return a + b; }` |
| `if` / `else` | `if (x > 0) { ... } else { ... }` |
| `while` / `do-while` / `for` | `for (i = 0; i < 10; i++) { ... }` |
| `break` / `continue` | `if (x == 5) break;` |
| `return` | `return x * 2;` |
| Operadores aritméticos | `+ - * / %` |
| Operadores de comparación | `== != < > <= >=` |
| Operadores lógicos | `&& \|\| !` |
| Asignación compuesta | `x += 5; count *= 2;` |
| Incremento / decremento | `i++; --j;` (prefijo y postfijo) |
| Address-of / deref | `int *p = &x; *p = 10;` |
| **Escritura de arrays como LHS** 🆕 | `arr[i] = expr; buf[pos] = c;` |
| Strings | `print("hola\\n");` |
| Declaraciones múltiples | `int x, y, z;` |
| Comentarios `//` y `/* */` | |

> 🆕 **Globales en `.bss`**: declarados a nivel de fichero (`int buf[32768];`),
> viven en una sección BSS que el loader ELF inicializa a cero
> automáticamente.  Antes los arrays grandes desbordaban el stack del
> proceso (~8 KB) y crasheaban; ahora puedes reservar buffers de decenas
> de KB sin problema.

**Funciones built-in (syscall):**

| Función | Descripción |
|---|---|
| `print(str)` | Imprime string |
| `print_num(n)` | Imprime entero (soporta negativos) |
| `print_char(c)` | Imprime un carácter |
| `getchar()` | Lee una tecla (bloqueante) |
| `getline(buf, max)` | Lee una línea con echo + backspace |
| `read_line(buf, max)` 🆕 | Igual que `getline` (alias estándar) |
| `read_file(path, buf, max)` 🆕 | Lee un fichero entero; devuelve bytes o `-1` |
| `write_file(path, buf, len)` 🆕 | Escribe un fichero (crea/trunca); devuelve bytes o `-1` |
| `sleep(ms)` | Duerme milisegundos |
| `uptime()` | Segundos desde boot |
| `getpid()` | PID del proceso |
| `exit(code)` | Termina el programa |
| `strlen(str)` | Longitud de string |
| `strcmp(a, b)` | Compara strings |
| `strncmp(a, b, n)` | Compara hasta n chars |
| `strcpy(dst, src)` | Copia string |
| `itoa(n, buf)` | Entero → string |
| `vga_clear(color)` | Limpia pantalla |
| `vga_putchar(x,y,ch,c)` | Dibuja carácter en posición |
| `vga_print(x,y,str,c)` | Dibuja string en posición |

> Los tres nuevos builtins (`read_file`, `write_file`, `read_line`) usan
> los syscalls `SYS_READFILE` (8), `SYS_WRITEFILE` (9) y `SYS_GETLINE` (10)
> añadidos en `cpu/syscall.{h,c}`.  Esto permite escribir aplicaciones
> reales (editores, viewers, configuradores) directamente desde `tcc`.

**Ejemplo completo** — un programa interactivo:

```c
int main() {
    char buf[64];
    int i;

    print("=== Contador interactivo ===\n");
    print("Cuantas veces quieres contar? ");

    getline(buf, 64);

    int n = 0;
    for (i = 0; buf[i] >= '0' && buf[i] <= '9'; i++) {
        n = n * 10 + (buf[i] - '0');
    }

    for (i = 1; i <= n; i++) {
        print_num(i);
        print(" ");
    }
    print("\nListo!\n");
    return 0;
}
```

#### TASM — Trinux Assembler (`shell/tasm.c`)

Un **ensamblador x86** que convierte código ASM en ELF32 ejecutable.
Ideal para experiments de bajo nivel o para escribir programas más
eficientes que los que genera el TCC.

```sh
edit hola.asm
asm hola.asm          # ensambla → crea 'hola' ELF32
exec hola
```

**Instrucciones soportadas:** `mov`, `add`, `sub`, `cmp`, `and`, `or`,
`xor`, `push`, `pop`, `inc`, `dec`, `int`, `call`, `jmp`, `je`, `jne`,
`ret`, `nop`, `hlt`, `db`, y labels (`nombre:`).

---

### Fase 16 — Robustecimiento del compilador y editor `tce` 🆕

Una ronda completa de bug-hunting en el subsistema TCC más un editor
nuevo escrito en el propio dialecto del compilador, todo encadenado en
una sola actualización.

#### 🐛 Bugs cazados en `shell/tcc.c`

| # | Bug | Síntoma observable | Fix |
|---|-----|--------------------|-----|
| 1 | Lexer leía `char` con signo → bytes UTF-8 dentro de comentarios `/* … */` rompían `skip_ws()` | Errores fantasma como `tcc:8: & requires variable name` en líneas que sólo contenían texto del comentario | Helper `peek_byte()` que devuelve `unsigned char` |
| 2 | El compilador escribía el ELF aunque hubiera errores | `tcc: compiled … bytes` con éxito aparente → al `exec` el programa crasheaba sin output | Contador `err_count` y abort antes de emitir el ELF |
| 3 | El stub `_start` hacía `ret` después de `call main`, pero el user-stack no tiene una dirección de retorno válida | Page fault al volver de `main()`, EIP en zona aleatoria | Reemplazado por `mov ebx, eax; mov eax, SYS_EXIT; int 0x80` |
| 4 | Todos los syscalls usaban números viejos (`SYS_WRITE=1` cuando la nueva ABI ya tiene `SYS_EXIT=1`, `SYS_WRITE=2`, …) | Cualquier `print*` invocaba accidentalmente `SYS_EXIT(1)` → kernel panic | Sustituidos por las macros simbólicas de `cpu/syscall.h` |
| 5 | Los builtins void (`print`, `print_num`, `print_char`, `sleep`, `vga_*`) no empujaban valor de retorno, pero el `parse_expr` del llamador siempre hace `add esp, 4` | Stack drift: cada llamada movía ESP 4 bytes hacia memoria desconocida, `ret` eventualmente a basura | Helper `e_push0()` al final de cada builtin void |
| 6 | `print_num` emitía saltos cortos con distancias **hardcoded** (`jns +0x16`, `jnz +0x0B`, etc.) que estaban mal calculadas — el `jns` caía 11 bytes corto, en mitad del operando de un `mov edx, 1` | Page fault reproducible con `addr=0xF9760CC8`, EIP dentro del programa, output truncado | Saltos calculados a partir del `cp` real y *patcheados* después de emitir el destino |

#### ✨ Nuevas capacidades de TCC

- **Variables globales** (`int x;`, `int arr[N];`, `char buf[N];`) con
  almacenamiento en `.bss` (zero-fill via `p_memsz > p_filesz`).
- **`char arr[N]` real**: 1 byte por elemento, indexado por bytes, con
  zero-extend al leer (`movzx eax, byte [edx]`).  Antes los `char[]` se
  trataban como `int[]` y consumían 4× la memoria.
- **Asignación a array como LHS**: `arr[i] = expr;` y `buf[pos] = c;`
  ahora generan el `mov [edx], eax` / `mov [edx], al` correcto.
- **Tres syscalls nuevos** expuestos como builtins: `read_file`,
  `write_file`, `read_line` — el mínimo absoluto para escribir un editor
  de texto real desde un programa compilado por TCC.

#### 📝 `tce` — editor de texto compilado por TCC

```sh
cd /root
tcc tce.c           # compila usando el propio compilador del kernel
exec tce            # te pregunta el nombre del fichero a editar
```

El editor `tce` está **escrito en el dialecto C que TCC entiende** —
sin structs, sin preprocessor, sin char literals — y demuestra que el
compilador ya es capaz de producir programas no triviales.  Features:

- Buffer de 32 KB en `.bss` (`char buf[32768]`)
- Movimiento con flechas, `Home`, `End`, `Backspace`, `Enter`, `Tab`
- Carga del fichero con `read_file()`, guardado con Ctrl-S
  (`write_file()`), salida con Ctrl-X (pregunta si hay cambios)
- Barra de título + barra de estado (línea, columna, bytes)
- Pinta directo en VGA con `vga_putchar()`

> El fuente original (`/root/tce.c`) se instala automáticamente al
> primer boot, junto con `/root/mini_test.c` (un *sanity-check* de 6
> líneas que verifica `print_num`, `print_char` y `print` en un solo
> exec).

#### ⚡ Mejoras del input — pegar texto largo desde Android

El otro frente que se atacó en esta ronda fue el flujo de **input por
PS/2 y por serie**.  Pegar 700 líneas desde el portapapeles de Android
hacía que el editor se trabara en la línea 5 y que al salir todo el
shell quedara en MAYÚSCULAS para siempre.  Causas y arreglos:

| Subsistema | Problema | Fix |
|---|---|---|
| `drivers/keyboard.c` | El handler de IRQ1 leía **un solo byte por interrupción** — los scancodes apilados por un paste rápido se perdían silenciosamente | Drenado completo del 8042 mientras `(STATUS & 0x21) == 0x01`, con guard de 64 |
| `drivers/keyboard.c` | `BUF_SIZE = 16384` era insuficiente para pastes de cientos de líneas | Subido a `65536` |
| `drivers/keyboard.{c,h}` | Tras un paste, un scancode `0x3A` fantasma podía dejar **CapsLock pegado**, y `keyboard_reset_modifiers()` explícitamente NO lo tocaba | Nueva función `keyboard_reset_all()` que también limpia `caps_lock`, llamada al salir del editor y tras cada batch de paste |
| `cpu/irq.{c,h}` | BIOS/GRUB dejaba IRQ4 (COM1) **enmascarada** en el PIC — los bytes llegaban al UART pero nunca disparaban interrupción | Funciones públicas `irq_clear_mask()` / `irq_set_mask()`; `serial_enable_input()` desenmascara IRQ4, `keyboard_init()` desenmascara IRQ1 por defensiva |
| `drivers/serial.c` | El driver era TX-only (sólo logs); no había forma de meter input por el puerto serie | Reescrito con FIFO 14-byte, IRQ4, **decodificador de secuencias ANSI** (flechas, Home/End, Del, F1-F12, ESC) que inyecta directo en el buffer del teclado vía `keyboard_inject_char()` |

El resultado es que ahora puedes lanzar QEMU **sin pantalla gráfica**,
pegar miles de líneas desde el portapapeles de Termux, y los caracteres
fluyen por el UART (115200 8N1) sin que se pierda un solo byte ni se
quede Shift/Caps colgado:

```sh
qemu-system-i386 \
  -drive file=mykernel-usb.img,format=raw,if=ide \
  -m 512M \
  -display none \
  -serial mon:stdio
```

(`Ctrl-A C` alterna entre la consola del kernel y el monitor de QEMU;
`Ctrl-A X` sale.)

---

### Fase 17 — Scheduler con prioridades + `idle` task + contabilidad por proceso 🆕

Hasta ahora el scheduler era **round-robin puro**: cada 50 ms saltaba al
siguiente proceso de la cola, sin importar si estaba haciendo algo útil
o no.  Y cuando "no había nada que hacer" seguía dando vueltas sobre los
procesos placeholder a 100% de CPU — el portátil se calentaba sin
motivo.

Esta fase reemplaza el scheduler por uno **basado en prioridades estilo
Unix `nice`**, con contabilidad de CPU por proceso y un **idle task
real** que apaga el CPU cuando no hay trabajo.

#### 🎯 Modelo de prioridades

```
   -20  ────────  más prioridad (gets the CPU first, can starve others)
     0  ────────  default
   +19  ────────  menos prioridad (sólo corre cuando nadie quiere CPU)
   idl  ────────  PRIO_IDLE (interno) — el `idle` task vive aquí
```

`schedule()` recorre la run queue y selecciona el READY con **el número
de prioridad más bajo**.  En caso de empate, hace round-robin entre los
tasks de la misma prioridad usando un índice giratorio
(`last_picked_idx`), así que dos procesos con la misma `nice` comparten
el CPU equitativamente.

#### ⏱️ Quantum variable por prioridad

El quantum (cuánto tiempo se le permite a un task seguir ejecutando
antes de considerar preempción) ahora **depende de la prioridad**:

| Prioridad | Quantum | Razón |
|---|---|---|
| `-20` (max) | 20 ms | Tarea interactiva, queremos baja latencia |
| `0` (default) | 50 ms | Igual al comportamiento anterior |
| `+19` (min) | 120 ms | CPU-bound, menos overhead de switching |
| `idle` | 10 ms | Cede CPU lo antes posible si llega trabajo |

#### 🧊 Idle task — el CPU se enfría de verdad

Un proceso nuevo (PID 4, `idle`) cuya entrada es literalmente
`for(;;) { sti; hlt; }`.  Vive en `PRIO_IDLE`, así que sólo se ejecuta
cuando **nadie más** está READY.  El `hlt` pone físicamente el CPU en
estado de bajo consumo hasta el siguiente IRQ (timer, teclado, serial).

> Verificación medible: en `top`, justo después de boot el CPU global
> aparece a 100% (todavía estaba acumulando ticks desde el arranque),
> pero en el siguiente refresh — con el sistema idle — baja a **~1%**.
> El delta son los HLTs reales.

#### 📊 Contabilidad por proceso

Tres campos nuevos en `process_t`:

```c
uint32_t cpu_ticks;       /* ticks totales en RUNNING (vida del proceso) */
uint32_t ticks_window;    /* ticks en la ventana actual de %CPU (≈2 s)   */
uint32_t start_tick;      /* timer_get_ticks() al crearse                */
```

`scheduler_tick()` (que corre 100 veces por segundo desde IRQ0)
incrementa **`cpu_ticks` y `ticks_window`** del proceso actualmente
RUNNING.  `top_render()` calcula `%CPU = ticks_window × 100 / total` y
resetea las ventanas para el siguiente intervalo.

#### 🛠️ Comandos nuevos

| Comando | Qué hace |
|---|---|
| `nice <prio> <cmd> [args...]` | Lanza `cmd` con prioridad inicial `<prio>`.  Internamente almacena un hint global que el siguiente `process_create()` consume. |
| `renice <prio> <pid>` | Cambia la prioridad de un proceso ya en ejecución.  Valida que `<prio>` esté en `[-20..+19]`. |

#### 👀 Salida de `ps` y `top`

```
$ ps
  PID  PRI  STATE     TIME+      NAME
  1    idl  running   0:03.05    init
  2    idl  sleeping  0:00.00    kthreadd
  3    idl  ready     0:00.00    mysh
  4    idl  ready     0:00.00    idle
  5      0  running   0:00.00    ps
```

```
$ top
  PID  PRI  STATE  %CPU   TIME+      NAME
  2    idl  SLP     0%   0:00.00    kthreadd
  3    idl  RDY     0%   0:00.00    mysh
  4    idl  RDY    98%   0:00.20    idle      ← absorbe el tiempo idle
  5      0  RUN     1%   0:00.01    top
```

#### 🔧 API kernel-side

```c
/* process.h */
int  process_set_priority(uint32_t pid, int prio);
void process_set_next_priority(int prio);  /* hint para próximo create() */
#define PRIO_MIN     (-20)
#define PRIO_MAX     ( 19)
#define PRIO_DEFAULT (  0)
#define PRIO_IDLE    (100)

/* scheduler.h */
void scheduler_kick(void);   /* fuerza reschedule al siguiente IRQ */
```

#### 🚧 Lo que falta para una "Fase B"

- **MLFQ** (multilevel feedback queue): procesos que ceden CPU pronto
  suben de prioridad automáticamente; los que la consumen entera bajan.
- **Tickless idle**: parar el PIT cuando no hay sleepers para que el CPU
  duerma más profundo (más ahorro de batería).
- **Wakeup paths**: que un IRQ de teclado o disco que despierta a un
  sleeper llame a `scheduler_kick()` y la preempción sea instantánea.
- **`cpu_ticks` por modo** (user vs kernel) para mostrar la diferencia
  estilo `top` de Linux.

---

### Fase 18 — Fase B del scheduler: MLFQ, sleepers, wakeups y CPU honesta 🆕

Esta fase cierra los huecos que la Fase A dejó abiertos.  El scheduler
ahora se comporta como cualquier kernel "de verdad" en sus aspectos más
importantes, y sobre el camino se cazaron un par de bugs estructurales
que llevaban tiempo sin notarse.

#### 🐛 Bugs estructurales arreglados antes de empezar

| # | Bug | Síntoma | Fix |
|---|-----|---------|-----|
| 1 | `scheduler_init()` se llamaba DESPUÉS de `process_init()`, así que reseteaba `queue_len=0` borrando el registro del idle task | El scheduler nunca encontraba nada que correr cuando llegaba un quantum | `kernel_main` invierte el orden y lo documenta como dependencia |
| 2 | El SYS_EXIT del syscall handler llamaba a `schedule()`, pero los ELF compilados por tcc corren como llamada de función dentro del flujo del shell — un context-switch ahí mataba al shell | `top` mostraba el proceso ELF en `RUN` para siempre, y a veces dejaba de responder el shell | Mecanismo `setjmp`/`longjmp` (`cpu/elf_jmp.asm`) que devuelve el control directamente a `elf_exec()` sin pasar por el scheduler |
| 3 | `process_create()` solo reciclaba slots ZOMBIE cuando la tabla estaba llena → tras unos cuantos `exec` la tabla crecía sin parar | `ps` mostraba 7+ tareas en vez de 5, con `mt`s zombi acumulados | Ahora prefiere SIEMPRE un slot ZOMBIE antes de gastar uno nuevo |
| 4 | Los ticks del kernel ocioso se contaban a `init` porque `current` seguía siendo init mientras el shell hacía HLT | `top` mostraba `init` a **100% CPU** aunque el sistema estaba dormido | `looks_like_idle_period()` redirige el billing al `idle` task cuando `current` está en `PRIO_IDLE` |

Después del fix 4, `top` muestra finalmente la verdad: el CPU global
baja a **0%** en idle, y todo el tiempo se atribuye al `idle` task como
debería.

#### 🔄 MLFQ (Multilevel Feedback Queue)

Cada proceso tiene ahora **dos** componentes de prioridad:

```
  efectiva = priority (estática, set por `nice`)  +  dyn_boost (dinámica)
```

Reglas del `dyn_boost`:

- **Crédito interactivo**: cuando un proceso cede el CPU ANTES de
  agotar su quantum (típicamente bloqueado en `getchar`, `sleep`,
  `read_file`), recibe `-1` en `dyn_boost` (= más prioridad).  Hasta
  `MLFQ_BOOST_MIN = -4`.
- **Castigo CPU-bound**: cuando agota su quantum entero
  `MLFQ_DEMOTE_AFTER = 2` veces seguidas, recibe `+1`
  (= menos prioridad).  Hasta `MLFQ_BOOST_MAX = +4`.
- **Decay**: cada `MLFQ_DECAY_TICKS = 500` ticks (5 s @ 100 Hz) todos
  los `dyn_boost` se halvan hacia 0, así que un proceso que *solía*
  ser interactivo pero ahora hace trabajo CPU-bound pierde su boost.

El resultado es que editores, shells y programas interactivos suben de
prioridad por sí solos, mientras que loops infinitos bajan al fondo y
dejan respirar al resto.

#### 💤 Procesos `SLEEPING` con wakeups reales

Antes `sleep(ms)` era un busy-HLT.  Ahora hay `sleep_block(ms)`:

```c
void scheduler_sleep_current(uint32_t until_tick);
```

Marca al proceso actual como `PROC_SLEEPING` con un `sleep_until`,
llama a `schedule()` y deja correr al siguiente READY.  El IRQ del PIT
ejecuta `scheduler_wakeup_check()` cada tick: cualquier sleeper cuyo
deadline ha pasado vuelve a READY y se llama a `scheduler_kick()`.

#### 🚀 Wakeup paths

Cuando un IRQ deposita algo "interesante" en una cola — un byte en el
buffer del teclado vía `buf_push()`, un sleeper que cumple su deadline,
en el futuro un paquete de red — llama a `scheduler_kick()`.  El flag
hace que el siguiente `scheduler_tick()` fuerce una reschedule incluso
si la tarea actual no ha agotado su quantum.

Resultado práctico: cuando estás editando con `tce` y tecleas, la
respuesta es inmediata aunque haya un loop CPU-bound al lado.

#### ⏱️ Contabilidad CPU en ring 3 vs ring 0

Tres campos nuevos en `process_t`:

```c
uint32_t cpu_ticks;       /* total (igual que Fase A) */
uint32_t cpu_ticks_user;  /* de cpu_ticks, los gastados en ring 3 */
uint32_t cpu_ticks_sys;   /* de cpu_ticks, los gastados en ring 0 */
```

El `timer_callback` mira el `CS` guardado al entrar al IRQ: si
`(cs & 3) == 3` el proceso estaba en user-mode, si no, en kernel.  El
scheduler factura el tick a la cubeta adecuada.

`top` muestra esto como una columna extra **`US%`** (porcentaje del
TIME+ del proceso que fue user-mode):

```
  PID  PRI  STATE  %CPU   US%   TIME+      NAME
  1    idl  RUN     0%    0%   0:00.00    init
  2    idl  SLP     0%    0%   0:00.00    kthreadd
  3    idl  RDY     0%    0%   0:00.00    mysh
  4    idl  RDY   100%    0%   0:02.06    idle
  13     0  RUN     0%    0%   0:00.00    top
```

> Por ahora `US%` siempre es 0 porque `elf_exec()` corre los ELFs en
> ring 0 como llamada de función (es un TODO viejo del kernel).  Cuando
> se implemente la transición real a ring 3 — la infraestructura ya
> existe en `cpu/syscall_asm.asm` — esta columna empezará a llenarse
> automáticamente sin tocar nada más.

#### 🧠 API kernel-side añadida en esta fase

```c
/* scheduler.h */
void scheduler_sleep_current(uint32_t until_tick);  /* block + reschedule */
void scheduler_set_last_user(int was_user);         /* used by timer IRQ */

/* timer.h */
void sleep_block(uint32_t milliseconds);            /* blocks current task */

/* process.h */
void process_set_current(process_t *p);             /* expuesto para elf_exec */

/* cpu/syscall.c → cpu/elf_jmp.asm */
extern int  elf_jmp_setjmp (elf_jmp_t *dst);        /* tipo setjmp/longjmp */
extern void elf_jmp_longjmp(elf_jmp_t *src);        /* usado por SYS_EXIT  */
```

#### 🔧 Tuning knobs

Todos en `process/scheduler.c`:

```c
#define MLFQ_BOOST_MAX        4    /* max -dyn_boost (boost máx)     */
#define MLFQ_BOOST_MIN      (-4)   /* max +dyn_boost (democión máx)  */
#define MLFQ_DEMOTE_AFTER     2    /* quantums seguidos para democión */
#define MLFQ_DECAY_TICKS    500    /* periodo de halving del boost   */
```

Y los quantums por prioridad efectiva (en `quantum_for()`):

| Prioridad efectiva | Quantum |
|---|---|
| `-20` (max) | 20 ms |
| `0` (default) | 50 ms |
| `+19` (min) | 120 ms |
| `idle` | 10 ms |

#### 🧹 Pulido tras la Fase B (issues que aparecieron al integrar todo)

Estas mejoras se aplicaron justo después de declarar la Fase B
completa, para que `nice`, `top` y `ps` se vean bien y se comporten
como uno esperaría:

| Fix | Problema | Solución |
|---|---|---|
| **`nice` se "comía" en el placeholder del shell** | `commands_dispatch()` crea un slot transitorio por cada built-in (incluido el propio `nice` y `exec`).  Ese slot consumía el hint de prioridad antes de que llegara al ELF real, así que `nice -5 exec /bin/foo` no surtía efecto | Nueva función `process_create_tracking(name)` que crea el slot cosmético SIN tocar `next_priority_hint`.  El shell usa esta variante; solo los procesos "de verdad" (idle, ELFs) consumen el hint |
| **Columna `STATE` en blanco en pantalla** | `vga_print_color()` escribía directo a la VRAM sin avanzar el cursor lógico — el siguiente `kprintf` sobrescribía esos bytes con espacios y el campo se veía vacío en el monitor (en serial sí salía porque usábamos `serial_printf` aparte) | Se cambió a `vga_set_color(sc); kprintf("%s", st); vga_set_color(saved);` — un solo emisor que actualiza ambos sinks correctamente |
| **`idle` mostrado como `RDY` con 100% CPU** | Honesto pero confuso: el idle nunca recibe un context-switch (a propósito), así que su estado sigue siendo READY.  Pero como acumula todos los HLT-ticks, verlo como `RDY @ 100%` despistaba | En `top` lo mostramos como `RUN` cuando su `priority >= PRIO_IDLE && cpu_ticks > 0`.  Su estado interno sigue siendo `READY`; solo cambia la presentación |
| **`%CPU` podía mostrar 101 / 114 por redondeo** | `pcpu = ticks_window * 100 / window_total` puede acabar 1-2 puntos por encima cuando los billing y el reseteo de la ventana se desincronizan | Clamp `if (pcpu > 100) pcpu = 100;` |
| **Roadmap desactualizado** | El bullet `[ ] MLFQ + quantum adaptativo + tickless (Fase B)` seguía sin marcar | Marcado, y añadidas 8 entradas nuevas describiendo lo que sí se hizo |

#### 🚧 Lo que falta para una "Fase C"

- **Tickless idle real**: parar el PIT cuando el único READY es el
  idle task.  En el modelo actual de Trinux esto requiere antes
  reescribir el shell como proceso real (no como flujo del kernel).
- **Ring 3 real para ELFs**: hoy `elf_exec` ejecuta el ELF como
  función en ring 0 con un setjmp/longjmp para gestionar la salida.
  Habría que pasar al `iret` a ring 3 con TSS y manejar correctamente
  el caso de page fault.  Cuando esto se haga, la columna `US%` de
  `top` empezará a llenarse automáticamente (la infraestructura ya
  está conectada al `cs` del frame de IRQ).
- **`sleep_block()` real**: añadida en Fase B pero todavía sin
  callers — el `sleep()` clásico sigue siendo busy-HLT.  Para
  habilitarla hay que reescribir `keyboard_getchar()` y el builtin
  `sleep` de TCC para que bloqueen via scheduler en vez de HLT.
- **`looks_like_idle_period()` por identidad en vez de por prio**:
  si alguien hace `renice 0 1` el truco de billing se rompe (init
  empezaría a recibir los HLT-ticks).  Mejor comparar contra el
  puntero del idle task cacheado.
- **`SY%` complementaria a `US%`** en `top`: el campo `cpu_ticks_sys`
  ya se rellena, falta mostrarlo.
- **Smp / multinúcleo**: el run queue es global; con SMP haría falta
  una cola por CPU + load balancing.
- **CFS-like fair scheduling**: en vez de MLFQ, un árbol rojo-negro
  ordenado por vruntime (lo que usa Linux desde 2007).

---

## 🛡️ Migración a ring 3 — qué hay hecho y qué falta

Hoy Trinux corre **casi todo en ring 0** (kernel mode) por simplicidad
histórica: el shell es una llamada de función dentro del bucle del
kernel, los built-ins (`ls`, `cat`, `top`...) son funciones C dentro
del propio binario `mykernel.bin`, y los ELFs lanzados por `exec`
también se ejecutan en ring 0 — con un `setjmp/longjmp` para que
`SYS_EXIT` pueda desenrollar la pila.

Esto es **didácticamente útil pero peligroso**: un bug en cualquier
sitio puede tumbar el sistema entero, y la "protección de anillos"
del CPU no se está usando.  Esta sección es la hoja de ruta para
llevar Trinux al modelo Unix de verdad — shell y apps en ring 3,
kernel aislado y protegido — sin romper lo que ya funciona.

### 📊 Qué tiene Trinux YA listo para ring 3

| Pieza | Estado | Archivo |
|---|---|---|
| GDT con descriptores de ring 3 (CS 0x1B, DS 0x23) | ✅ | `boot/gdt.c` |
| TSS con `ss0`/`esp0` para el switch ring-3 → ring-0 | ✅ | `boot/gdt.c` + `tss_set_kernel_stack()` |
| Gate `int 0x80` con DPL=3 (invocable desde ring 3) | ✅ | `cpu/syscall.c:syscall_install()` |
| `enter_usermode(eip, esp)` que hace `iret` a ring 3 | ✅ | `cpu/syscall_asm.asm` |
| Stub `usermode_run()` que crea proceso + entra a ring 3 | ✅ | `cpu/syscall.c` |
| Page fault y GPF redirigen a `usermode_fault_kill` en vez de panic | ✅ | `cpu/isr.c` |
| Pila kernel separada por proceso (`p->kstack`, 8 KB) | ✅ | `process/process.c` |
| Stack de usuario en `USER_STACK_TOP - USER_STACK_SIZE` | ✅ | `kernel/elf.c` |
| `process_t` con campo `priority`, scheduling MLFQ | ✅ | `process/*.c` |
| Contabilidad user vs kernel (`cpu_ticks_user`, `cpu_ticks_sys`) | ✅ | `process/scheduler.c` |
| Decodificador del frame IRQ que detecta `(cs & 3) == 3` | ✅ | `drivers/timer.c` |
| 10 syscalls definidos (`SYS_EXIT`, `SYS_WRITE`, ...) | ✅ | `cpu/syscall.{h,c}` |
| ELFs compilados por TCC con BSS, globales, char[] | ✅ | `shell/tcc.c` |
| `setjmp/longjmp` para que SYS_EXIT desenrolle limpio | ✅ | `cpu/elf_jmp.asm` |

**Conclusión**: la infraestructura de ring 3 está, lo que falta es
*conectarla* y *abrir la API*.

### ❌ Qué bloquea hoy que el shell pase a ring 3

#### 1. `elf_exec()` ejecuta los ELFs como llamada de función en ring 0

```c
/* kernel/elf.c — el código actual: */
typedef void (*entry_fn_t)(void);
entry_fn_t fn = (entry_fn_t)entry;
fn();                              /* ← se ejecuta en ring 0, NO ring 3 */
```

Debería en cambio invocar `enter_usermode(entry, user_esp)` que ya
existe en `cpu/syscall_asm.asm`.  Riesgo: `enter_usermode` no
retorna (hace `iret`), así que el flujo del `elf_exec` cambia
completamente.  Hay que aplicar la pareja con `elf_arm_exit_jmp` que
ya añadimos: `SYS_EXIT` longjmp-ea de vuelta, y eso es el punto de
salida del programa.

#### 2. El shell hace 464 llamadas directas a funciones del kernel

`vfs_resolve`, `kprintf`, `vga_putchar`, `kmalloc`, `process_at`,
`users_login`...  Todas son llamadas C normales que solo funcionan
en ring 0.  En ring 3 cada una debe pasar por un syscall.

#### 3. Solo hay 10 syscalls; faltan ~30

Lo que el shell necesita y NO tiene como syscall:

| Categoría | Syscalls que faltan |
|---|---|
| Directorios | `SYS_OPENDIR`, `SYS_READDIR`, `SYS_CLOSEDIR`, `SYS_CHDIR`, `SYS_GETCWD` |
| Ficheros | `SYS_STAT`, `SYS_UNLINK`, `SYS_MKDIR`, `SYS_RMDIR`, `SYS_RENAME`, `SYS_CHMOD`, `SYS_CHOWN` |
| Procesos | `SYS_FORK`, `SYS_EXECVE`, `SYS_WAIT`, `SYS_KILL`, `SYS_PS_AT`, `SYS_PS_COUNT` |
| Usuarios | `SYS_GETUID`, `SYS_SETUID`, `SYS_GETPWENT`, `SYS_USERADD`, `SYS_PASSWD` |
| Memoria | `SYS_BRK`, `SYS_MMAP`, `SYS_MUNMAP` |
| Terminal | `SYS_TCGETATTR`, `SYS_TCSETATTR`, `SYS_IOCTL` |
| Pipes/redir | `SYS_PIPE`, `SYS_DUP2`, `SYS_CLOSE` |

#### 4. No hay aislamiento de memoria por proceso

`vmm_init()` activa paging pero hace **identity map** de los primeros
256 MiB, sin distinguir páginas user/supervisor.  Ring 3 puede leer
`/etc/shadow` directamente desde memoria si conoce el offset.

Lo que hace falta:
- Una `page_directory_t` por proceso (campo `p->page_dir` ya existe
  pero no se usa).
- Marcar las páginas del kernel como `supervisor only` (bit U/S = 0)
  en todas las page directories.
- Mapear solo las páginas del proceso como user-accessible.
- Cambiar `cr3` en cada `context_switch()`.

#### 5. No hay `fork()` ni `execve()`

En Unix el shell hace:
```
fork() → en el hijo: execve("/bin/ls", argv, envp)
                      el padre: waitpid(pid) → exit_code
```

Trinux **no implementa nada de esto**.  Cada comando es una función
C del binario del kernel.  Para hacer un shell ring-3 de verdad:

- `fork()` debe duplicar el espacio de direcciones del padre
  (copy-on-write idealmente, en su defecto copia completa de páginas).
- `execve(path, argv, envp)` debe leer el ELF de disco, mapear sus
  segmentos en el espacio del proceso actual (reemplazándolo) y
  saltar al entry point en ring 3.
- `waitpid(pid)` debe bloquear al padre hasta que el hijo muera y
  recoger su `exit_code`.

#### 6. La heurística `looks_like_idle_period()` se rompe

El truco de Fase B asume que cuando el shell hace HLT, `current` es
`init` (PRIO_IDLE).  Si el shell fuera proceso ring 3 con prioridad
0, esa heurística billarìa los HLT-ticks al shell y `top` mentiría.
Habría que comparar contra el puntero del idle task cacheado.

#### 7. Validación de punteros desde userland

Si el shell ring 3 hace `read_file(path_ptr, buf_ptr, len)`, el
kernel debe verificar que `path_ptr` y `buf_ptr` apunten a memoria
del proceso (no a una página del kernel) **antes** de leer/escribir.
Hace falta `copy_from_user()` y `copy_to_user()` que capturen page
faults y devuelvan -EFAULT en vez de tumbar el kernel.

#### 8. Los built-ins del shell deben convertirse en binarios separados

Si `ls`, `cat`, `ps`, `top` siguen siendo funciones C dentro del
kernel, el shell ring-3 no podría ejecutarlas directamente.  Hay
dos caminos:

- **A**: cada built-in se compila como ELF separado en `/bin/` (más
  Unix-like pero requiere reimplementar TODO con syscalls).
- **B**: el shell sigue siendo el único programa "especial" que
  ejecuta los built-ins via syscall `SYS_RUNBUILTIN(name, argc, argv)`
  (transición temporal, pragmática).

### 🗺️ Plan de migración por fases incrementales

Cada fase deja el sistema funcional al final.  Se pueden hacer en
orden, y cada una se puede mergear independientemente.

#### Fase C1 — ELFs en ring 3 de verdad (1-2 días)

**Objetivo**: que `exec /root/mt` (un programa TCC) corra en ring 3
sin romper nada de lo actual.

- [ ] En `kernel/elf.c`, sustituir `fn()` por `enter_usermode(entry,
  user_esp)`.  Mantener el `elf_arm_exit_jmp()`/`longjmp` como punto
  de salida.
- [ ] Verificar con `top` que la columna `US%` deja de ser 0 — ahora
  los programas TCC realmente consumen CPU en ring 3.
- [ ] Confirmar que SYS_EXIT y page fault siguen unwindeando bien al
  shell vía longjmp.
- [ ] Suite de tests existente: `tcc mini_test.c && exec mini_test`,
  `tcc tce.c && exec tce`, deben seguir funcionando.

**Riesgo bajo**: la infraestructura está, solo es cambiar 3 líneas y
probar.

#### Fase C2 — Syscalls esenciales de filesystem (1 semana)

**Objetivo**: que un programa ring-3 pueda hacer `ls`, `cat`, `cd`.

- [ ] Añadir `SYS_OPENDIR` (11), `SYS_READDIR` (12), `SYS_CLOSEDIR`
  (13), `SYS_STAT` (14), `SYS_CHDIR` (15), `SYS_GETCWD` (16),
  `SYS_UNLINK` (17), `SYS_MKDIR` (18), `SYS_RMDIR` (19),
  `SYS_RENAME` (20), `SYS_CHMOD` (21), `SYS_CHOWN` (22).
- [ ] Añadir helpers `copy_from_user(dst, user_src, n)` y
  `copy_to_user(user_dst, src, n)` que validen el puntero está en
  el rango user (`< KERNEL_BASE` o equivalente).
- [ ] Exponer los syscalls como builtins en TCC: `opendir`,
  `readdir`, `stat`, `unlink`, etc.
- [ ] Programa de prueba: un `ls` en ~80 líneas de TCC.

**Riesgo medio**: cada syscall hay que validarlo bien.

#### Fase C3 — Aislamiento de memoria por proceso (2-3 semanas)

**Objetivo**: que un crash del programa ring-3 NO pueda tumbar el
kernel ni acceder a `/etc/shadow` raw.

- [ ] Reescribir `vmm.c` para dar a cada proceso su propia
  `page_directory_t`.
- [ ] Marcar todas las páginas del kernel como `bit U/S = 0`.
- [ ] Mapear el ELF cargado solo en el espacio del proceso.
- [ ] `context_switch()` carga `cr3` del proceso entrante.
- [ ] Implementar `SYS_BRK` para que `kmalloc` userland (futuro
  malloc.c en TCC) funcione.

**Riesgo alto**: el VMM es el corazón del sistema; un bug aquí es un
triple fault al boot.

#### Fase C4 — `fork()`, `execve()`, `waitpid()` (1-2 semanas)

**Objetivo**: poder hacer `fork+exec` desde un programa ring 3.

- [ ] `SYS_FORK` (23): duplicar `process_t`, copiar page directory
  (copy-on-write si C3 ya está, copia plena si no).
- [ ] `SYS_EXECVE` (24): leer ELF de disco, descartar el espacio
  actual del proceso, mapear el nuevo y saltar al entry point.
- [ ] `SYS_WAITPID` (25): bloquear hasta que el PID hijo termine.
- [ ] El shell ring-3 (Fase C6) usará esto en cada comando.

**Riesgo alto**: COW es delicado; sin COW, fork es lento pero
funciona.

#### Fase C5 — Convertir built-ins en ELFs separados (1-3 semanas)

**Objetivo**: `/bin/ls`, `/bin/cat`, `/bin/ps`, `/bin/top` como
programas standalone.  El shell ya no los tiene como funciones C.

- [ ] Reescribir cada built-in como `.c` de TCC.
- [ ] Embeber los binarios compilados en el kernel image (igual que
  ya hacemos con `hello_elf`, `snake_elf`, `vgademo_elf`).
- [ ] `install_builtin_apps()` los pone en `/bin/` al boot.
- [ ] El shell ring-3 los invoca via `fork+exec`.

**Estimación de qué reescribir** (en líneas TCC, optimista):

| Programa | Lineas estimadas | Syscalls necesarios |
|---|---|---|
| `ls` | 80 | opendir/readdir/stat |
| `cat` | 30 | open/read |
| `pwd` | 10 | getcwd |
| `cd` | (built-in del shell, no programa) | chdir |
| `mkdir`/`rmdir`/`rm` | 20 c/u | mkdir/rmdir/unlink |
| `ps` | 60 | ps_count/ps_at |
| `top` | 200 | ps + timer + vga |
| `kill` | 15 | kill |
| `chmod`/`chown` | 25 c/u | chmod/chown |
| `cp`/`mv` | 60 c/u | open/read/write/unlink |
| `echo` | 10 | write |

**Riesgo medio**: mucho código pero mecánico.

#### Fase C6 — Shell como proceso ring 3 (1-2 semanas)

**Objetivo**: el shell deja de ser parte del kernel y se convierte
en `/bin/mysh`, el PID 1 que arranca el sistema.

- [ ] Reescribir `shell/shell.c` y `shell/commands.c` como un
  programa TCC standalone.  Es ~5000 líneas; algunas porciones
  (parser, history, line editor, dispatch) se pueden mantener casi
  como están — solo cambian las llamadas internas por syscalls.
- [ ] `kernel_main()` al final ejecuta `execve("/bin/mysh", ...)`
  en lugar de llamar a `shell_run()`.
- [ ] Si `/bin/mysh` muere, el kernel relanza (igual que `init` en
  Linux).
- [ ] Arreglar `looks_like_idle_period()` para que use el puntero
  del idle cacheado en vez de mirar la prioridad.

**Riesgo alto**: es el cambio que más toca, pero también el que
verifica que TODO lo anterior funciona.

#### Fase C7 — Limpieza y pulido

- [ ] Eliminar `setjmp/longjmp` de `cpu/elf_jmp.asm`: ya no hace
  falta porque los ELFs corren en ring 3 con su propia stack.
- [ ] Eliminar los kthreads placeholder `init`/`kthreadd`/`mysh` de
  `process_init()` ahora que el PID 1 es el shell real.
- [ ] Documentar el modelo nuevo (un proceso = una page directory =
  una user stack = un kstack) en el README.

### 📈 Estimación total

| Aspecto | Estimación |
|---|---|
| Tiempo total | 2-4 meses de trabajo dedicado |
| Líneas de código nuevas (kernel) | ~3000 |
| Líneas reescritas (built-ins en TCC) | ~2000 |
| Syscalls totales tras la migración | ~35-40 |
| Tamaño del kernel resultante | ~150 KB (más pequeño: built-ins fuera) |
| Tamaño de `/bin/` resultante | ~30-50 KB de ELFs |

### 🎯 Beneficios al terminar

1. **Aislamiento real**: un bug en el shell o en `cat` no tumba el
   kernel.  Pages faults se reportan al proceso, no al sistema.
2. **`top` mostrará `US%` y `SY%` realistas** por proceso —
   exactamente como htop en Linux.
3. **Multi-tasking real**: dos shells corriendo en consolas
   distintas (necesita pseudo-terminales).
4. **Seguridad básica**: `/etc/shadow` deja de ser legible desde
   ring 3 aunque el proceso conozca la dirección.
5. **Camino abierto** hacia `fork`+`exec`, pipes reales, signal
   handling, threads de usuario, etc.
6. **Modelo mental coherente** con cualquier libro de OS — el código
   se vuelve mucho más enseñable.

### 🚨 Riesgos y cosas que se romperán por el camino

- **Persistencia del filesystem**: si el shell cambia, `sync` /
  `diskfs_save` siguen siendo del kernel — está bien, pero hay que
  exponerlo como syscall.
- **TCC y TASM** son del shell hoy.  Tras la migración serían
  `/bin/tcc` y `/bin/tasm`, programas ring-3 que leen ficheros, los
  compilan a memoria y escriben el ELF resultante.  Pueden seguir
  funcionando con los syscalls de FS añadidos en C2.
- **El editor `edit`** (no `tce`, el de `shell/editor.c`) también
  necesita reescritura como `/bin/edit` ELF — o eliminarlo en favor
  de `tce` que ya es ring-3-compatible.
- **El monitor `top`** depende de muchas estadísticas internas; hay
  que exponerlas todas via syscalls (`SYS_PMM_STATS`, `SYS_HEAP_STATS`,
  `SYS_DISK_STATS`, `SYS_BATTERY`, etc.) o agrupar en un syscall
  genérico `SYS_SYSINFO(struct sysinfo *)`.

---

## 🛠️ Comandos de la shell (70+)

### Archivos y navegación

| Comando | Descripción |
|---|---|
| `ls` (`-l`, `-a`, `-la`) | Listar directorio. Colorea dirs en azul, ejecutables en verde |
| `cd` | Cambiar directorio (`cd ..`, `cd /`, `cd ~`) |
| `pwd` | Imprimir directorio actual |
| `mkdir` (`-p`) | Crear directorio (con `-p` crea padres) |
| `rmdir` | Eliminar directorio vacío |
| `touch` | Crear archivo vacío |
| `rm` (`-r`, `-f`, `-rf`) | Eliminar archivo o directorio recursivamente |
| `cat` | Mostrar contenido de archivo (también lee de pipe) |
| `echo` (`>`, `>>`) | Imprimir texto, con redirección a archivo |
| `write` | Escribir texto a archivo |
| `edit` / `nano` | Editor de texto a pantalla completa (Ctrl-S guardar, Ctrl-X salir) |
| `cp` (`-r`) | Copiar archivo o directorio |
| `mv` | Mover/renombrar |
| `stat` | Info detallada de archivo (tipo, tamaño, permisos, owner, timestamps) |
| `tree` | Árbol de directorios visual |
| `find` | Buscar archivos por nombre (recursivo) |
| `head` / `tail` | Primeras/últimas N líneas |
| `wc` | Contar líneas, palabras, caracteres |

### Filtros de texto

| Comando | Descripción |
|---|---|
| `grep` (`-i -v -n -c`) | Filtrar líneas por patrón |
| `sort` (`-r -u -n`) | Ordenar líneas |
| `uniq` (`-c`) | Eliminar duplicados adyacentes |
| `cut` (`-d -f -c`) | Seleccionar campos/caracteres |
| `tee` (`-a`) | Copiar stdin a archivo y terminal |
| `seq` | Generar secuencia numérica |

### Pipes y redirección

```sh
cat /etc/passwd | grep root          # filtrar
ls / | wc                            # contar
ls -l /etc | grep -n host           # con números de línea
sort archivo | uniq -c               # contar ocurrencias
cut -d: -f1 /etc/passwd             # extraer campo
seq 1 5 | tee nums.txt              # guardar y mostrar
wc < /etc/motd                       # redirección de entrada
echo hola >> log.txt                 # append
```

### Sistema

| Comando | Descripción |
|---|---|
| `clear` / `cls` | Limpiar pantalla |
| `help` / `help <cmd>` | Ayuda general o detallada |
| `uname` (`-a`) | Info del sistema |
| `uptime` | Tiempo desde boot |
| `date` | Fecha y hora (RTC) |
| `whoami` | Usuario actual |
| `hostname` | Ver/cambiar hostname |
| `free` | Uso de memoria física + heap |
| `df` | Uso de disco (soporta >4 GiB) |
| `ps` | Lista de procesos |
| `top` | Monitor en vivo: CPU, RAM, Heap, Disco, Batería, procesos |
| `kill` | Matar proceso por PID |
| `sync` | Guardar filesystem al disco (persistencia) |
| `reboot` | Reiniciar (vía 8042 reset) |
| `shutdown` / `halt` | Apagar (vía ACPI port 0x604) |
| `battery` | Estado de batería (laptop) |

### Disco y dispositivos

| Comando | Descripción |
|---|---|
| `dd` | Copiar bloques (completo: `if=`, `of=`, `bs=`, `count=`, `skip=`, `seek=`, `conv=`, `status=progress`) |
| `/dev/sda` | Disco crudo (solo root) |
| `/dev/zero` | Ceros infinitos |
| `/dev/null` | Descarta escrituras |
| `/dev/random` | Bytes aleatorios |

### Usuarios y permisos

| Comando | Descripción |
|---|---|
| `login` | Cambiar de sesión |
| `logout` | Cerrar sesión |
| `su` | Cambiar usuario (por defecto a root) |
| `useradd` | Crear usuario (root only) |
| `passwd` | Cambiar contraseña |
| `id` | Mostrar UID/GID |
| `users` / `groups` | Listar usuarios/grupos |
| `chmod` | Cambiar permisos (octal: `chmod 644 file`) |
| `chown` | Cambiar owner (root only) |
| `umask` | Ver/cambiar máscara de permisos |

### Utilidades y extras

| Comando | Descripción |
|---|---|
| `neofetch` | Info del sistema con ASCII art y batería |
| `calc` | Calculadora (`calc 6 x 7`) |
| `hexdump` | Dump hexadecimal de archivo |
| `color` | Cambiar colores de la terminal |
| `history` | Historial de comandos |
| `alias` | Crear aliases |
| `basename` / `dirname` | Manipular rutas |
| `which` | Localizar comando |
| `env` | Variables de entorno |
| `yes` | Repetir cadena (limitado a 100 líneas) |
| `usertest` | Demo de programa en ring 3 (syscalls) |
| `tcc` | Compilar C → ELF32 (`tcc prog.c`) |
| `asm` | Ensamblar x86 → ELF32 (`asm prog.asm`) |
| `exec` | Ejecutar ELF32 desde filesystem (`exec prog`) |

---

### Sistema multiusuario

Cuentas por defecto (contraseña = nombre de usuario):

| usuario | uid | home | password |
|---|---|---|---|
| root | 0 | /root | `root` |
| user | 1000 | /home/user | `user` |

El sistema implementa el modelo clásico de Unix:
- `/etc/passwd` contiene la info de cuentas.
- `/etc/shadow` contiene las contraseñas (en plaintext — es educativo).
- Prompt verde `user@host:~$` para usuarios normales.
- Prompt rojo `root@host:~#` para root (nota el `#` vs `$`).
- `useradd`, `passwd` y `chown` son solo-root.
- `su` pide contraseña (root puede cambiar a cualquiera sin contraseña).
- Cada usuario tiene su home directory con owner correcto.

### Persistencia

Cuando haces `sync`, se guarda **todo** al disco:
- Archivos y directorios
- Permisos y owners
- Usuarios y contraseñas
- Hostname
- Todo lo que está en el VFS

Al siguiente boot, el kernel restaura todo automáticamente.  Sin `sync`,
los cambios viven solo en RAM y se pierden al apagar.

---

## 📁 Estructura del proyecto

```
Kernel-Trinux-V2/
├── Makefile                  # build system (gcc -m32 / nasm / ld)
├── make-usb-image.sh         # genera .img bootable (Legacy + UEFI)
├── linker.ld                 # enlaza en 1 MiB, secciones alineadas a 4 KiB
├── boot/
│   ├── boot.asm              # entry point Multiboot + stack de 16 KiB
│   ├── gdt.c + gdt_flush.asm # GDT con 6 entradas + TSS
│   ├── kernel_entry.asm      # stub auxiliar
│   └── mbr_boot.asm          # bootloader MBR de referencia (512 bytes)
├── kernel/
│   ├── kernel.c              # kernel_main: init de todos los subsistemas
│   ├── panic.c               # kernel panic con halt
│   └── multiboot.h           # struct de info Multiboot
├── drivers/
│   ├── vga.c/h               # driver VGA texto 80x25
│   ├── keyboard.c/h          # teclado PS/2 scancode set 1
│   ├── timer.c/h             # PIT 100 Hz + idle ticks + CPU usage
│   ├── rtc.c/h               # reloj CMOS (fecha/hora)
│   ├── serial.c/h            # COM1 debug output
│   ├── pci.c/h               # enumeración PCI (mecanismo 1)
│   ├── ata.c/h               # wrapper unificado IDE/AHCI/xHCI
│   ├── ahci.c/h              # SATA via AHCI (DMA)
│   ├── xhci.c/h              # USB 3.0 + Mass Storage BOT + SCSI
│   └── acpi_ec.c/h           # batería via Embedded Controller
├── cpu/
│   ├── idt.c/h + idt_flush.asm   # Interrupt Descriptor Table
│   ├── isr.c/h + isr_asm.asm     # CPU exception handlers (0-31)
│   ├── irq.c/h + irq_asm.asm     # PIC + hardware IRQ handlers
│   ├── ports.c/h                  # inb/outb/inw/outw/inl/outl
│   └── syscall.c/h + syscall_asm.asm  # int 0x80 + ring3 runner
├── mm/
│   ├── pmm.c/h               # physical memory manager (bitmap)
│   ├── vmm.c/h               # paging + MMIO dinámico
│   └── kheap.c/h             # kernel heap (first-fit + coalescing)
├── fs/
│   ├── vfs.c/h               # Virtual File System + permisos
│   ├── ramfs.c/h             # RAM filesystem (dos modos: RAM / disco)
│   ├── path.c/h              # resolución de rutas
│   ├── diskfs.c/h            # serialización del VFS a disco
│   ├── blockfs.c/h           # almacenamiento por bloques de 4 KiB
│   ├── devfs.c/h             # /dev/sda, zero, null, random
│   ├── fat.c/h               # parser FAT genérico
│   └── fat16.c/h             # driver FAT16
├── process/
│   ├── process.c/h           # PCB + tabla de procesos
│   ├── scheduler.c/h         # scheduler round-robin cooperativo
│   ├── switch.asm            # context switch en ASM
│   └── thread_exit.asm       # limpieza de threads
├── auth/
│   └── users.c/h             # cuentas, /etc/passwd, /etc/shadow, login/su
├── shell/
│   ├── shell.c/h             # shell principal: prompt, editor, historial, pipes
│   ├── commands.c/h          # 70+ comandos integrados
│   ├── editor.c/h            # editor nano a pantalla completa
│   ├── tcc.c/h               # compilador C → ELF32 (single-pass)
│   └── tasm.c/h              # ensamblador x86 → ELF32
├── lib/
│   ├── string.c/h            # funciones de cadena/memoria/ctype
│   ├── printf.c/h            # kprintf / snprintf
│   └── types.h               # uint8_t, bool, size_t, etc.
├── user/
│   └── userprog.c/h          # programas de demo ring-3
├── include/
│   └── kernel.h              # versión, macros (KERNEL_NAME, etc.)
└── screenshots/              # capturas de pantalla del sistema
```

---

## 🤔 Notas y decisiones de diseño

- **Detección automática IDE → AHCI → xHCI.**  El kernel intenta los tres
  backends de disco en cascada.  En QEMU usa IDE (el más simple), en PCs
  con disco SATA usa AHCI (DMA), en laptops booteando de USB usa xHCI
  (USB 3.0).  El resto del kernel ni se entera de cuál está activo.

- **No se implementó un intérprete AML/ACPI completo** para la batería
  (eso son ~50,000 líneas como la implementación de referencia de Intel).
  En su lugar se lee directamente el Embedded Controller con layouts
  conocidos + auto-detección.  Funciona en la mayoría de laptops.

- **Imagen USB dual boot** gracias a `grub-mkrescue` con los paquetes
  `grub-pc-bin` (Legacy) + `grub-efi-ia32-bin` (UEFI).  La imagen
  resultante tiene tabla GPT, partición ESP con `BOOTIA32.EFI`, MBR
  híbrido y partición HFS+ para Macs.

- **Single-user mode** igual que Linux real: se parsea la cmdline de
  Multiboot buscando `single` o `init=/bin/bash`.  Desde GRUB se edita
  con `e` + Ctrl-X, exactamente como en cualquier distro.

- **Scheduler cooperativo** por diseño.  La infraestructura para preemption
  existe (context switch en ASM, timer IRQ) pero está deshabilitada para
  garantizar estabilidad de la shell interactiva.

- **Compila con `-Wall -Wextra`** prácticamente sin warnings.

- **Todo el código es original** — no se usa código de Linux, Minix, xv6
  ni ningún otro proyecto.  Cada driver, cada syscall, cada comando fue
  escrito desde cero.

---

## 🗺️ Roadmap

- [x] Boot Multiboot + modo protegido + GDT + TSS
- [x] VGA texto, teclado PS/2, timer PIT
- [x] IDT + ISR + PIC + IRQ
- [x] PMM + VMM (paging) + kernel heap
- [x] VFS + RAMFS + persistencia en disco
- [x] Block storage + dispositivos /dev
- [x] Procesos + scheduler cooperativo
- [x] Shell con 66+ comandos, pipes, redirección, historial, tab completion
- [x] Editor de texto a pantalla completa
- [x] Userspace ring 3 + syscalls int 0x80
- [x] FAT16
- [x] PCI + AHCI/SATA (disco en hardware real)
- [x] xHCI + USB Mass Storage (persistencia en USB)
- [x] Batería (ACPI EC) + CPU usage en tiempo real
- [x] Boot UEFI (via GRUB EFI 32-bit)
- [x] Single-user mode / recovery (estilo Linux)
- [x] Cargar binarios ELF desde el filesystem (TCC + TASM + exec)
- [x] **TCC: variables globales (`.bss`), `char[]` real, array-write LHS** 🆕
- [x] **Syscalls de I/O de ficheros (`read_file` / `write_file` / `read_line`)** 🆕
- [x] **Editor `tce` escrito y compilado por el propio TCC** 🆕
- [x] **Input por COM1 con IRQ4 + decodificador ANSI (pegar desde Android sin perder bytes)** 🆕
- [x] **Scheduler por prioridades (nice -20..+19) + idle task con HLT real** 🆕
- [x] **Contabilidad de CPU por proceso (%CPU, TIME+) visible en `top` y `ps`** 🆕
- [x] **MLFQ con boost/demote dinámico + decay periódico** 🆕
- [x] **`scheduler_kick()` desde IRQs (teclado/serial) para wakeup inmediato** 🆕
- [x] **Sleepers reales (`PROC_SLEEPING` + `sleep_until` + `scheduler_wakeup_check`)** 🆕
- [x] **Reciclado automático de slots ZOMBIE en `process_create`** 🆕
- [x] **`SYS_EXIT` con `setjmp`/`longjmp` para no romper el flujo del shell** 🆕
- [x] **Billing de CPU honesto: HLT-ticks van al `idle`, no a `init`** 🆕
- [x] **Contabilidad user/kernel por proceso (campo `cpu_ticks_user`, columna `US%`)** 🆕
- [ ] Tickless idle real (Fase C — requiere shell como proceso real)
- [ ] Ring 3 real para ELFs (hoy corren en ring 0 con setjmp/longjmp)
- [ ] SMP / multinúcleo + colas por CPU
- [ ] CFS-like fair scheduling (alternativa al MLFQ actual)
- [ ] Driver de red (NE2000 / virtio-net)
- [ ] Modo gráfico VBE (640×480 o mayor)
- [ ] NVMe (almacenamiento de siguiente generación)
- [ ] Soporte para más de 256 MiB de RAM
- [ ] Más comandos: `tar`, `ping`, `wget`, `ssh`...


## 📋 Tabla completa de Syscalls

Trinux implementa **10 syscalls** accesibles vía `int 0x80` (DPL=3,
invocables desde ring 3):

| # | Nombre | Regs | Args | Descripción |
|---|---|---|---|---|
| 1 | `SYS_EXIT` | `ebx=code` | exit_code | Termina el proceso. Hace `longjmp` de vuelta a `elf_exec()` o llama al scheduler. |
| 2 | `SYS_WRITE` | `ebx=fd, ecx=buf, edx=len` | buffer, longitud | Escribe al VGA y al puerto serial COM1 simultáneamente. |
| 3 | `SYS_GETPID` | — | — | Retorna el PID del proceso actual. |
| 4 | `SYS_YIELD` | — | — | Cede el CPU cooperativamente al scheduler. |
| 5 | `SYS_SLEEP` | `ebx=ms` | milisegundos | Duerme el proceso. Usa `sleep()` del driver PIT (busy-HLT). |
| 6 | `SYS_GETC` | — | — | Lee un carácter del teclado (bloqueante). |
| 7 | `SYS_UPTIME` | — | — | Segundos transcurridos desde el boot. |
| 8 | `SYS_READFILE` | `ebx=path, ecx=buf, edx=max` | ruta, buffer, máximo | Lee archivo del VFS. Retorna bytes leídos o -1. |
| 9 | `SYS_WRITEFILE` | `ebx=path, ecx=buf, edx=len` | ruta, buffer, longitud | Crea/trunca y escribe archivo en el VFS. Retorna bytes o -1. |
| 10 | `SYS_GETLINE` | `ebx=buf, ecx=max` | buffer, máximo | Lee línea del teclado con echo + backspace. |

> **Convención**: `eax` = número syscall, args en `ebx/ecx/edx`, resultado en `eax`.

---

## 🎮 Aplicaciones integradas

Al boot, el kernel instala automáticamente las siguientes aplicaciones
embebidas en `/bin` (si no existen ya en disco):

| App | Fuente embebida | Descripción |
|---|---|---|
| `hello` | `kernel/hello_elf.h` | Demo de "Hello World" usando syscalls para imprimir al VGA. |
| `vgademo` | `kernel/vgademo_elf.h` | Demo gráfico que pinta patrones de colores directo en 0xB8000. |
| `snake` | `kernel/snake_elf.h` | Juego de la serpiente implementado con VGA directa y `SYS_GETC`. |

Además, el kernel empaqueta **fuentes C** en `/root` para que puedas
compilarlos y modificarlos:

| Fuente | Descripción |
|---|---|
| `tce.c` | Editor de texto compilable con TCC. Demuestra que el compilador produce programas no triviales. |
| `mini_test.c` | Suite de tests de 6 líneas que valida `print_num`, `print_char` y `print`. |
| `slow.c` | Programa CPU-bound para probar el scheduler MLFQ con `nice`. |

```sh
cd /root
tcc tce.c && exec tce          # compila y ejecuta el editor
tcc mini_test.c && exec mini_test  # ejecuta los tests
nice +10 exec slow              # prueba el scheduler con baja prioridad
```

---

## 🔧 TASM — Instrucciones soportadas

El ensamblador integrado (`asm`) soporta las siguientes instrucciones x86:

| Categoría | Instrucciones |
|---|---|
| Movimiento | `mov reg,imm` · `mov reg,reg` · `mov [reg],imm` · `mov reg,[reg]` |
| Aritmética | `add` · `sub` · `inc` · `dec` · `mul` · `div` |
| Lógica | `and` · `or` · `xor` |
| Comparación | `cmp` |
| Stack | `push` · `pop` |
| Control | `call` · `jmp` · `ret` · `nop` · `hlt` |
| Interrupciones | `int imm` |
| Datos | `db` (define byte) |
| Labels | `nombre:` (resueltos en pass 2) |

### Ejemplo

```asm
section .text
global _start

_start:
    mov eax, 2          ; SYS_WRITE
    mov ecx, msg
    mov edx, msglen
    int 0x80

    mov eax, 1          ; SYS_EXIT
    mov ebx, 0
    int 0x80

section .data
msg:    db "Hola desde TASM!", 10, 0
msglen: equ $ - msg
```

---

## ⚙️ ELF Loader: arquitectura y restricciones

### Memory layout

| Región | Dirección | Propósito |
|---|---|---|
| Código/Datos | Según ELF (típicamente `0x08048000`) | Segmentos PT_LOAD del ELF |
| Stack de usuario | `0x0F000000 - 16 KiB` | Stack temporal para ELFs |
| Límite | 256 MiB (`0x10000000`) | No se pueden mapear segmentos más allá |

### Restricciones actuales

- Los ELFs deben ser **ELF32 i386 little-endian** de tipo `ET_EXEC`.
- Los segmentos no pueden exceder los **256 MiB** identity-mapped.
- No hay **ASLR** — las direcciones del ELF se cargan tal cual.
- No hay **aislamiento de memoria** — el ELF comparte el espacio del kernel.
- El ELF corre en **ring 0** (kernel mode), no ring 3.
- La terminación usa `setjmp/longjmp` para volver al flujo del shell.

### Mecanismo de salida (`elf_arm_exit_jmp`)

Los ELFs compilados por TCC se ejecutan como llamada de función dentro
del flujo del shell. Para poder salir limpiamente:

```
  shell_run()
    └→ commands_dispatch()
         └→ elf_exec(path, cwd)
              ├→ elf_arm_exit_jmp()     /* registra landing pad */
              ├→ entry_fn()             /* ejecuta el ELF */
              │    └→ int 0x80 SYS_EXIT
              │         └→ elf_jmp_longjmp()  /* vuelve aquí */
              ├→ elf_disarm_exit_jmp()
              └→ return exit_code
```

> 🚧 **Ver sección "Migración a ring 3"** para el plan de ejecutar ELFs
> en ring 3 con aislamiento de memoria real.

---

## 🗂️ Configuración del sistema

### Archivos de configuración

| Archivo | Propósito |
|---|---|
| `/etc/passwd` | Cuentas de usuario (formato: `nombre:pass:uid:gid:home`) |
| `/etc/shadow` | Contraseñas en plaintext (educativo) |
| `/etc/hostname` | Nombre del host (se lee al boot, aparece en el prompt) |
| `/etc/motd` | "Message of the Day" (se muestra tras el login) |

### Cuentas por defecto

| Usuario | UID | Home | Password |
|---|---|---|---|
| `root` | 0 | `/root` | `root` |
| `user` | 1000 | `/home/user` | `user` |

### Hostname y MOTD

```sh
# Cambiar el hostname
echo "mi-pc" > /etc/hostname
sync

# Cambiar el mensaje de bienvenida
echo "Bienvenido a Trinux!" > /etc/motd
sync
```

El hostname aparece en el prompt: `user@mi-pc:~$`
El MOTD se muestra en cyan antes del primer prompt.

---

## 🐛 Troubleshooting

### Problemas comunes

| Síntoma | Causa | Solución |
|---|---|---|
| `"command not found"` | Comando inexistente o mal escrito | Usa `help` para ver los 70+ comandos disponibles |
| Archivos no persisten | Falta ejecutar `sync` antes de apagar | `sync` guarda todo al disco |
| QEMU se congela | QEMU esperando input | `Ctrl-C` en la terminal o `Ctrl-Alt-2` → `quit` en QEMU |
| Teclado no responde | QEMU no captura el input | Haz clic en la ventana de QEMU; `Ctrl-Alt` para liberar |
| Paste roto en Termux | Scancodes PS/2 se pierden | Usa `-display none -serial mon:stdio` |
| `"No ATA disk"` | QEMU sin parámetro `-drive` | Usa `make run` en vez de `make` |
| `exec` falla con ELF | Formato inválido o no ELF32 | Verifica con `stat` que sea ELF32 válido |
| CapsLock pegado | Scancode fantasma tras paste | El editor llama `keyboard_reset_all()` al cerrar |

### Logs de debug

El kernel envía logs al puerto serial COM1 (115200 8N1):

```sh
make run              # logs en stderr de QEMU
qemu-system-i386 -kernel mykernel.bin -serial stdio  # logs en terminal
```

El serial también **acepta input** (Fase 16) — puedes ejecutar comandos
desde una terminal externa sin ventana gráfica:

```sh
qemu-system-i386 -kernel mykernel.bin -display none -serial stdio
```

(`Ctrl-A C` alterna entre la consola del kernel y el monitor de QEMU;
`Ctrl-A X` sale.)

---

## 🤝 Contribuir

### Cómo hacer un fork

```sh
# 1. Haz fork del repositorio en GitHub
# 2. Clona
git clone https://github.com/TU_USUARIO/Kernel-Trinux-V2.git
cd Kernel-Trinux-V2

# 3. Crea una branch
git checkout -b feature/mi-feature

# 4. Compila y prueba
make clean && make && make run

# 5. Commit y push
git add . && git commit -m "feat: mi feature" && git push origin feature/mi-feature

# 6. Abre un Pull Request
```

### Convenciones de código

- **C**: Estilo K&R, 4 espacios, `snake_case` para funciones y variables.
- **ASM**: Sintaxis NASM, indentación con tabs.
- **Headers**: Guards `#ifndef` con nombre de directorio + archivo.
- **Comentarios**: Inglés para código, español para README.

### Áreas donde se necesita ayuda

- [ ] Más comandos (`tar`, `ping`, `wget`, `ssh`)
- [ ] Driver de red (NE2000 / virtio-net)
- [ ] Modo gráfico VBE (640×480+)
- [ ] Soporte para más de 256 MiB de RAM
- [ ] Filesystem real (ext2, FAT32)
- [ ] SMP / multinúcleo
- [ ] CFS-like fair scheduling
- [ ] NVMe support
- [ ] USB keyboard/mouse (HID)
- [ ] Mejorar TCC (más features C)
- [ ] **Ring 3 real para ELFs** (ver plan Fase C1-C7)
- [ ] Tickless idle

---

## 📊 Estadísticas del proyecto

| Métrica | Valor |
|---|---|
| **Líneas de código** | ~12,000 (C + ASM) |
| **Comandos de shell** | 70+ |
| **Syscalls** | 10 |
| **Drivers** | 11 (VGA, teclado, timer, RTC, serial, PCI, ATA, AHCI, xHCI, ACPI EC) |
| **Sistemas de archivos** | 6 (VFS, RAMFS, DISKFS, BLOCKFS, DEVFS, FAT16) |
| **Fases implementadas** | 18+ |
| **Tamaño del kernel** | ~120 KB (mykernel.bin) |
| **Memoria mínima** | 16 MiB |
| **Arquitectura** | i686 (32-bit x86 protected mode) |

---
## 📜 Licencia

Por definir (sugerencia: MIT o GPL-2.0).

---

*Trinux es un proyecto educativo escrito completamente desde cero.
Si encuentras un bug o quieres añadir una feature, ¡los pull requests
son bienvenidos!*
