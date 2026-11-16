# Auditoría funcional — qué falta para que Trinux sea 100% funcional

> Fecha: 2026-11-16 · Rama `arena/3d9a4585-kernel-trinux-v2`
> Base auditada: `607f6df` (master)

Este documento es el resultado de auditar el árbol completo: compilación de
los ~120 archivos C con `-Wall -Wextra`, enlace del kernel, arranque real bajo
emulación y revisión de los subsistemas que el README declara funcionando.

**Método**: en este entorno no hay `qemu-system` ni `nasm` (la red solo
permite npm/pypi/github), así que se montó un arnés propio sobre
[unicorn](https://pypi.org/project/unicorn/) (el core de CPU de QEMU) que
carga el ELF Multiboot, construye un `multiboot_info`, emula PIC/PIT/PS2/COM1
y **entrega las interrupciones leyendo la IDT viva del kernel** (Unicorn no
emula el *interrupt delivery*). Con eso el kernel arranca de verdad:

```
   Trinux v0.2.0  (i686)
  [ OK ] IDT installed
  [ OK ] PIC remapped, IRQ handlers ready
  [ OK ] Kernel heap (32 MiB arena)
  [ OK ] Physical memory manager
  [ OK ] Paging enabled
  [ OK ] PIT timer @ 100 Hz
  [ OK ] RAM filesystem mounted at /
  [ OK ] Built-in apps installed in /bin
  [ OK ] Process table + scheduler
  [init] launching /bin/sh in RING 3 (CPL=3)
```

(El arnés no puede emular el cambio de pila privilegiada del `int 0x80`, así
que la ejecución se detiene al entrar en la shell; todo lo demás sí se
ejecuta.)

---

## 1. Bugs encontrados y **corregidos** en esta rama

### 🔴 Crítico — `vmm_free_address_space()` liberaba memoria del kernel

`mm/vmm.c`. `vmm_create_address_space()` copia **superficialmente** el
directorio de páginas: las entradas 0–255 apuntan a las page tables globales
(`page_tables[]`, `.bss` estático, **no** frames del PMM). Pero
`vmm_free_address_space()` recorría desde `IDENTITY_TABLES` y hacía
`pmm_free_frame()` de *cada página presente* — y en el identity-map **todo
está presente**. Resultado: cada `process_exit()` devolvía al PMM ~896 MiB
que nunca le pertenecieron, corrompiendo el bitmap; tras unos cuantos
`exec`/`fork` el kernel podía entregar el mismo frame a dos usuarios
(corrupción de memoria silenciosa) o agotar la memoria antes de tiempo.

**Arreglo**: solo se libera lo que el address space posee de verdad — se
saltan las entradas que apuntan a la misma page table que el directorio
maestro, y se exige `pmm_is_allocated()` (nueva función en `mm/pmm.c`).

### 🟠 Grave — el framebuffer agotaba el pool de page tables

`mm/vmm.c`. `vmm_map_page()` reciclaba un pool estático de **8 page tables**
para todo el sistema. Un framebuffer de 1366×768×32bpp necesita ~3 tablas, y
`SYS_BRK`, AHCI y xHCI otras tantas: con dos procesos el pool se agotaba y
las siguientes llamadas **retornaban sin hacer nada** (fallo silencioso de
mapeo). **Arreglo**: nuevo `new_page_table_for()` que pide la tabla al PMM y
hereda el identity-map; el pool de 8 tablas se eliminó. Además
`vmm_map_page_in()` ahora fuerza el bit `PAGE_USER` cuando se mapea una
página de usuario en una zona marcada supervisor-only (antes el acceso
fallaba en ring 3 mucho después, difícil de depurar).

### 🟠 Grave — desbordamiento de pila en la pila de red (ring 0)

`drivers/net.c`. Dos overflows con datos que llegan de la red:

1. `icmp_send_reply()` hacía `memcpy(icmp->data, payload, plen)` con un
   `plen` derivado del `total_len` del paquete recibido y **sin acotar** al
   tamaño del buffer → un echo request grande escribía fuera de `frame[1500]`.
2. `net_ping()` declaraba `uint8_t frame[64]` pero escribía un `struct
   icmp_t` completo (40 B) desplazado 34 bytes = 74 bytes → 10 bytes fuera
   (`-Warray-bounds` lo avisaba y estaba ignorado).

**Arreglo**: `plen` acotado a 1458 B, `frame[]` dimensionado con
`sizeof(icmp_t)`, y validación de `ip->total_len` contra el tamaño realmente
recibido (antes `ip_total - 20` podía desbordar a 65516 y copiar memoria
arbitraria).

### 🔴 Crítico — `fork()` no podía funcionar

`mm/fork.c` + `process/process.c`. `process_create(name, NULL)` deja
`context.eip = 0`; `process_fork()` creaba el hijo, copiaba su address space y
lo añadía al run queue **sin construir el contexto**. El primer `schedule()`
que elegía al hijo saltaba a la dirección 0. Ahora `process_fork()` arma
`eip = return address`, `esp` en la pila kernel propia del hijo (con
`kernel_thread_exit` de retorno) y `process_create()` explícita el caso
`entry == NULL`.

### 🟠 `tar` no existía y `tcc` no compilaba

- `user/coreutils/tar_u.c` **no compilaba** (`bool`/`true`/`false` sin
  `<stdbool.h>`, y `sys_mkdir_()` inexistente) y además no estaba en
  `build.sh` → el README prometía `tar` pero nunca llegaba a `/bin`.
- `user/coreutils/cide_u.c` tampoco estaba en `build.sh`.
- `user/tccu/tcc_main.c`: el `kprintf` propio empaquetaba los argumentos en
  un `long[]`, así que **todo `%s` truncaba el puntero** (`-Wint-conversion`
  en 10 sitios). Reemplazado por varargs reales con soporte de ancho y
  `%08x`.

**Arreglo**: ambos programas añadidos a `build.sh` (71/71 compilan ahora;
antes 69 con `tar` roto), `kprintf` reescrito. `user_bins.h`,
`hdrs/tar.h`, `hdrs/cide.h` y `bin/*` regenerados.

### 🟡 `build.sh` fallaba en silencio

`user/coreutils/build.sh` usaba `xxd` (no existe en imágenes mínimas) y
`2>/dev/null` al compilar: si un programa fallaba, **el header quedaba vacío
o viejo** y nadie se enteraba. Ahora hay fallback de `xxd` en Python, los
errores se reportan, y la tabla `user_bins[]` solo incluye lo que compiló de
verdad (ordenada alfabéticamente, así el diff es reproducible).

### 🟡 Trampoline de SMP: escritura fuera de rango

`cpu/smp_boot.c`. El array `trampoline_code[256]` declaraba 256 bytes e
inicializaba 265 (9 de más, descartados con `-Wexcess-elements`), y el
parámetro `cr3` vive en `0xFE..0x101` → `memcpy(tramp + 0xFE, &cr3, 4)`
escribía 2 bytes en `0x8100`, fuera de nuestro bloque. Ahora el tamaño es
`TRAMP_SIZE` (= `0x102`) y los huecos se declaran con inicializadores
designados (`[0xF0 ... 0xF5] = 0`), no contando ceros a mano.

### 🟡 `argv` de `SYS_SPAWN` se leía de memoria ya pisada

`cpu/syscall.c`. El `char **argv` del shell ring 3 apunta a su pila de
usuario, pero `elf_exec_argv()` carga el ELF hijo **en la misma VA**
(`0x08048000`) antes de leer `argv` → argumentos basura o crash. Nuevo
`copy_user_argv()` que copia punteros + strings a memoria del kernel
(validando con `uaccess`) antes de tocar la imagen del proceso.

### 🟡 `uaccess_ok()` tenía un overflow de 32 bits

`cpu/uaccess.h`. `if (addr + len > USER_SPACE_END)` desborda con `addr` alto
+ `len` grande y deja pasar el test. Ahora compara `len > (END - addr)`.

### 🟡 Declaraciones implícitas (argumentos con el tipo equivocado)

- `kernel/elf.c` llamaba `tss_set_kernel_stack()` sin prototipo (GCC asumía
  `void*`): prototipo movido al inicio del archivo.
- `process/process.c` llamaba `schedule()` sin incluir `scheduler.h`.

---

## 2. El problema estructural: hay **dos** Trinux vivos en el código

Lo más importante que apareció al auditar no es un bug puntual, sino que el
árbol contiene **dos arquitecturas conviviendo** y el README describe una
mezcla de ambas:

| Capa | Estado real |
|---|---|
| Shell ring-3 (`/bin/sh`, PID 5) | ✅ existe, `kernel.c` la lanza con `[init] launching /bin/sh in RING 3` |
| Builtins del kernel (`shell/commands.c`, 90 comandos) | ✅ funcionan, corren en ring 0 |
| `exec` de un comando desde `/bin/sh` | ✅ vía `SYS_SPAWN` → `elf_exec_argv()` |
| **Pero**: `elf_exec_argv_inner()` hace `usermode_save_and_enter()` + `setjmp/longjmp` | ⚠️ ejecuta el hijo **anidado** en la pila del kernel |
| Syscalls | ✅ 73 definidos, **69 con handler** (`SYS_TIME` 28, `SYS_PUTPIXEL` 29, `SYS_DUP2_FD` 34, `SYS_VFS_CAP` 46 no existen → devuelven `-1` + mensaje en consola) |
| `fork()` / `execve()` | ✅ implementados (con el arreglo de arriba), pero `fork()` es **sincrónico** |
| Aislamiento de memoria | ⚠️ **no es real** (ver §3.1) |

Tareas concretas para terminar esta unificación:

1. Eliminar el camino `setjmp/longjmp` (`cpu/elf_jmp.asm`,
   `usermode_save_and_enter`) y ejecutar cada ELF con `enter_usermode()` +
   `SYS_EXIT` normal. Hoy un programa que no llama `exit()` deja el kernel
   dentro de un `longjmp` y `g_elf_jmp` es **global**, no por proceso: con dos
   procesos en ring 3 el primero que sale desarma el frame del segundo.
2. Implementar los 4 syscalls que faltan o borrarlos de `user/trinux.h`
   (mejor lo segundo: `SYS_VFS_CAP` es un dump de VGA que ya no tiene sentido
   con el framebuffer).
3. Hacer `fork()` asíncrono (hoy el padre no continúa hasta que el hijo
   termina), lo que además arregla `waitpid()` sin el `schedule()` en bucle
   con el page directory del hijo activo.

---

## 3. Lo que falta para "100% funcional"

### 3.1 Aislamiento de memoria real (el agujero de seguridad más grande)

`grep "0x08048000" kernel/elf.c` → **todos** los ELF se cargan en la misma
dirección virtual, y `vmm_create_address_space()` comparte las page tables.
Consecuencias verificables:

- Dos procesos no pueden tener ELF cargados a la vez.
- Un proceso ring 3 puede leer la memoria de otro (mismo VA, mismas tablas).
- `/etc/shadow` es legible desde ring 3 si se conoce la dirección, aunque el
  `cat` sí respete permisos (v0.3.1) — las páginas del kernel están
  protegidas (`U/S=0`), pero *las de otros procesos no*.

**Falta**: page tables privadas por proceso (hoy son compartidas), ELF
cargado en un VA por proceso o con COW, y `cr3` por proceso también para los
kstack. Es el punto 1 del roadmap del propio README (Fase C3) y sigue abierto.

### 3.2 Networking — ✅ ARREGLADO en la ronda 2 (ver §6)

`drivers/rtl8139.c` + `drivers/net.c` existen y el driver está completo, pero
**nadie llama `net_init()`**: no hay comando `ping`, no hay syscall de red, y
`net_poll()` nunca se ejecuta (solo dentro de `net_ping`, que nada invoca).
El README lo anuncia como "`ping` funcional". Falta:

1. Llamar `net_init()` en `kernel_main()` cuando `rtl8139_present()`, con IP
   por DHCP fija (p.ej. 10.0.2.15/24 gw 10.0.2.2 para QEMU) o configurable.
2. Un builtin `ifconfig`/`ip` y un builtin `ping` (o `/bin/ping`) que llame
   `net_ping()`. Hoy `ping` no existe en `shell/commands.c` ni en
   `coreutils`.
3. `net_poll()` desde el timer IRQ o desde el idle, si no los paquetes se
   acumulan solo mientras alguien hace ping.

### 3.3 FAT16 — ✅ ARREGLADO en la ronda 2 (ver §6)

`fs/fat16.c:513`. `fat16_init()` lee el MBR, y si el sector 0 **no** tiene
firma `0xAA55` deja `partition_lba = 0` y sigue: lee el BPB desde el propio
MBR, y si `bytes_per_sector` coincide no valida ni firma FAT ni
`sectors_per_fat`. Después `root->children[...] = fat_root_node` **sin
comprobar `fat_root_node != NULL`** → `strcpy(NULL, "fat")`.

**Falta**: validar firma FAT (`"FAT16"` / `55 AA`), `spc != 0`,
`total_sectors > data_start_lba`, `total_clusters > 4085` (si no es FAT16) y
comprobar el puntero antes de montar. Y, sobre todo, decidir qué se quiere:
hoy `/fat` y el `diskfs` propietario conviven sobre el mismo disco con
regiones reservadas a mano (`DISKFS_TAIL_BYTES`, `BOOT_RESERVE`), lo que ya
provocó un bug de corrupción (documentado en `fs/diskfs.c`).

### 3.4 Persistencia: `sync` sin `disk.img` no hace nada

Sin disco, todo vive en RAM y se pierde. Con `make run` sí hay `disk.img`,
pero el `make-usb-image.sh` y el workflow de CI copian la ISO y la
**extienden** con `truncate`: la zona de persistencia (`blockfs`) queda a
ceros y se "formatea" en el primer `sync`. Funciona, pero conviene
documentarlo y probarlo en hardware real (no hay logs de que se haya hecho).

### 3.5 SMP es decorativo

`cpu/smp_boot.c` arranca los APs y cada uno hace `sti; hlt` en un bucle
infinito (`ap_main`). No hay colas por CPU ni locks: el scheduler es
single-core. El README ya lo admite ("SMP: arranque de APs via LAPIC IPI"),
pero **no** hay exclusión mutua en ninguna estructura global (heap, VFS,
tabla de procesos): en cuanto un AP toque algo, hay carrera. Falta:

- Un spinlock para `kmalloc`/`pmm_alloc_frame`/VFS.
- Colas de run queue por CPU o una global con lock.
- `tlb_shootdown` por IPI.

### 3.6 Scheduler

- Preemption: sí existe (el timer IRQ llama `schedule()`), pero
  `looks_like_idle_period()` factura los ticks de `hlt` al idle por
  heurística de prioridad; con varias tareas ring 3 la contabilidad `US%`
  puede mentir (el propio README lo señala).
- `waitpid()` hace `schedule()` en bucle mientras el **page directory del
  hijo** puede estar activo (fork sincrónico): con más de un proceso listo,
  el padre reanuda con el `cr3` equivocado. El arreglo correcto es fork
  asíncrono.

### 3.7 Otros huecos menores

| Hueco | Dónde | Impacto |
|---|---|---|
| `SYS_OPEN`/`read`/`write` con fd por proceso | `cpu/syscall.c` (`kfds[]` es global, con `owner_pid`) | Dos procesos que abren archivos a la vez comparten slots |
| `SYS_BRK` no desmapea al encoger | `cpu/syscall.c` | Fuga de frames hasta que el proceso muere |
| No hay `mmap`/`munmap` | — | Cualquier libc real necesitará `mmap` |
| `SYS_DUP2` no existe | — | Redirección dentro de un proceso imposible (`spawn_r` la emula desde el shell) |
| Sin TTY por proceso (`tcgetattr`, `ioctl`) | — | `top`/`edit` son builtins del kernel, no programas |
| Sin `kill -9` real para tareas bloqueadas en ring 0 | `process_deliver_signal` | Un proceso atascado en un syscall no muere |
| 32-bit only, max 1 GiB identity-mapped | `mm/vmm.c` | `longmode.asm` existe pero no se enlaza |
| No hay relocación de ELF (`.rel`) | `kernel/elf.c` | Solo valen binarios enlazados a 0x08048000 |
| `display_init()` en pánico/`top` | `drivers/vga.c` | Sin framebuffer no hay modo gráfico real |

---

## 4. Cómo verificar cada arreglo

> **Nota sobre este entorno**: aquí no hay `nasm` ni QEMU, así que el arranque
> se verificó con un arnés de emulación (unicorn) que ejecuta el ELF hasta
> `[init] launching /bin/sh in RING 3`; **el `int 0x80` desde ring 3 no es
> emulable** en ese arnés. La verificación completa la hace el workflow
> `.github/workflows/ci.yml` (QEMU real) y, para hardware, `make run`.

```bash
# 1. Compila todo (esto ya lo hace el CI)
make clean && make -j$(nproc)

# 2. Regenera los binarios ring 3 (necesita xxd o python3)
cd user/coreutils && bash build.sh && cd ../..

# 3. Arranca y comprueba que la shell ring 3 entra
make run          # o: qemu-system-i386 -kernel mykernel.bin -m 512M -serial stdio

# 4. Los arreglos concretos
ls /bin/tar /bin/cide                 # ahora existen
tar -c /etc/passwd > /tmp/p.tar       # ahora compila y corre
/root/brktest                          # SYS_BRK por proceso
exec /bin/execvetest                   # SYS_EXECVE (mismo PID)
ringtest                               # debe morir con #GP => ring 3 real
free                                   # tras varios exec, la RAM no debe caer

# 5. Persistencia (necesita disco)
make disk
make run
# dentro de Trinux:
mkdir /root/persist && echo hola > /root/persist/a.txt && sync
# reinicia y: cat /root/persist/a.txt
```

---

## 5. Estado por subsistema (resumen)

| Subsistema | Estado | Comentario |
|---|---|---|
| Boot Multiboot + GDT/TSS | ✅ | Verificado en emulación |
| IDT/ISR/IRQ/PIC | ✅ | Verificado (IRQ0 entregada y atendida) |
| Paging + PMM + heap | ✅ / 🔧 | Liberación de address spaces, heap (§10) y detección de RAM por mmap (§11) corregidos |
| VFS + ramfs + diskfs | ✅ / 🔧 | `vfs_get_path`/`path_*`/`ramfs_write` acotados (§7.5-7.7); escritura sin permisos cerrada (§7.4). Persistencia sin probar en hardware real |
| FAT16 | ✅ / 🔧 | Valida BPB, geometría y `total_clusters` antes de montar (§6.2) |
| Procesos + scheduler | ✅ / 🔧 | Contexto de `fork()` corregido; `longjmp` por nivel (§7.1); `process_init` sin NULL deref (§8.1); preemption real |
| Ring 3 + 74 syscalls | ✅ / 🔧 | 74/74 con handler (§6.7); falta unificar con el camino setjmp |
| `fork`/`execve`/`waitpid` | ⚠️ | Sincrónicos, sin COW |
| Aislamiento de memoria | ❌ | Page tables compartidas |
| Networking | ✅ / ⚠️ | Cableado: `net_init()` en boot, `net_poll()` a 50 Hz, `ifconfig`, `ping` (builtin y `/bin/ping`); probado solo sin NIC (§6.1) |
| SMP | ⚠️ | APs arrancan y duermen; sin locks |
| VBE/framebuffer | ⚠️ | Requiere que GRUB dé el modo; pool de tablas corregido |
| ACPI/batería | ✅ | |
| TCC/TASM | ✅ / 🔧 | `kprintf` arreglado; `patch32`/fixups acotados (§7.9); compila 72/72 |
| Editor/`cide`/`tar` | ✅ | Añadidos a `build.sh` |
| CI | ✅ / 🔧 | Compila + **arranca en QEMU** (`scripts/smoke_test.sh`) + auditoría estática + `-fanalyzer` + regresiones (§6.8, §8.4) |

### Lo que le faltaba al CI — ✅ resuelto en la ronda 2

El workflow solo comprobaba que `mykernel.bin` pesara >100 KiB y no arrancaba
el kernel. Ahora `scripts/smoke_test.sh` lo arranca en QEMU, valida la salida
serie, falla ante `PANIC`/`#PF en ring 0` y sube el log; y `audit.yml` añade
las comprobaciones estáticas (syscalls, headers embebidos, artefactos
versionados, llamadas implícitas, userland). Ver §6.8.

## 6. Ronda 2 — arreglos aplicados tras el primer informe

Todo lo de esta sección está compilado (`0 fallos`, `0` llamadas implícitas) y
el kernel arranca hasta `/bin/sh` en ring 3 con **21 `[ OK ]`**.  Los puntos
que siguen abiertos están en §3 y se marcan aquí como tales.

### 6.1 Red cableada de verdad (antes: código muerto)

`drivers/net.c` y `drivers/rtl8139.c` eran ~600 líneas que nadie llamaba:
`net_init()` no se invocaba nunca y `net_poll()` solo desde `net_ping()`, que
tampoco se llamaba. Ahora:

| Cambio | Archivo |
|---|---|
| `net_init_from_cli()` parsea `trinux.ip=… trinux.gw=… trinux.mask=…` de la cmdline Multiboot; por defecto 10.0.2.15/24 gw 10.0.2.2 (QEMU user-mode) | `drivers/net.c`, `drivers/net.h` |
| `kernel_main()` llama a `net_init_from_cli()` y reporta `[ OK ] Network stack …` o `No network adapter` | `kernel/kernel.c` |
| `MULTIBOOT_FLAG_CMDLINE` (bit 2) que faltaba | `kernel/multiboot.h` |
| `net_poll()` a 50 Hz desde el tick del PIT → las respuestas ARP/ICMP ya no se quedan en el ring de la NIC | `drivers/timer.c` |
| Builtins `ifconfig` (MAC/IP/GW/MSK + contadores) y `ping <ip> [-c n]` | `shell/commands.c` |
| `parse_ipv4()`, `net_status()`, `net_ready()`, contadores rx/tx | `drivers/net.c/.h` |
| Syscall `SYS_NET_PING` (74) → `/bin/ping` real en ring 3 | `cpu/syscall.c`, `user/trinux.h`, `user/coreutils/ping_u.c` |

Para probarlo: `make run` (QEMU con `-nic user` por defecto) y dentro de Trinux
`ifconfig` / `ping 10.0.2.2`. Sin NIC (como en este entorno) el arranque
imprime `No network adapter` y `ifconfig` explica cómo habilitarla; no rompe
nada.

### 6.2 FAT16 valida el BPB antes de montar

`fs/fat16.c` (`fat16_init`) ahora rechaza, en vez de asumir:

- `bytes_per_sector != 512`, `sectors_per_cluster == 0`, `sectors_per_fat_16 == 0`,
  `reserved_sectors == 0`, `fat_count == 0`;
- `total_sectors <= data_start_lba` (el underflow de `uint32` que hacía
  `total_clusters` gigante) y `root_dir_sectors == 0`;
- volúmenes que no son FAT16 (`total_clusters < 4085` → FAT12,
  `>= 65525` → FAT32), con mensaje explícito;
- `fat16_read_dir()` devolviendo `NULL` — antes se hacía
  `strcpy(fat_root_node->name, "fat")` sobre `NULL`.

En todos los casos se resetea `partition_lba = 0` para no dejar el driver
apuntando a una partición que no es FAT16.

### 6.3 `pmm_alloc_frame()` devuelve frames a cero

Antes entregaba el frame tal como estuviera. Eso importa en dos sitios:

- `vmm_create_address_space()` usa el frame como **page directory**: basura con
  `PAGE_PRESENT` → entradas espurias y #PF aleatorios.
- `new_page_table_for()` para VA ≥ 1 GiB hacía `memset`, pero el PD no.

Ahora `pmm_alloc_frame()` hace `memset(addr, 0, 4096)`, empieza en el frame 1
(el 0 queda reservado como "null") y `pmm_free_frame()` ignora direcciones no
alineadas y el frame 0.

### 6.4 `process_exit()` ya no libera el page directory en caliente

`process_exit()` corre en la pila del proceso que muere y sigue ejecutándose
tras marcarse `ZOMBIE`. Llamar allí a `vmm_free_address_space(current->page_dir)`
dejaba el `cr3` activo apuntando a frames ya devueltos al PMM (y el siguiente
`process_create()` podía reutilizarlos). Ahora la liberación se difiere al
reciclado del slot en `process_create()`, que ya la hacía.

### 6.5 Un solo mapa de memoria de usuario

Antes cada subsistema tenía sus propios números:

| Sitio | Rango que usaba |
|---|---|
| `cpu/uaccess.h` | `0x08000000 … 0x10000000` |
| `mm/vmm.c` | usuario desde `0x05000000` |
| `mm/fork.c` | copiaba `0x08048000 … 0x0F100000` (113 MiB/fork) |
| `cpu/syscall.c` | `brk` limitado a `0x0E000000` |
| `kernel/elf.c` | stacks en `0x0F000000`, `0x0EE00000`, … |

Ahora hay **una** fuente de verdad, `include/userlayout.h`, y todos incluyen
de ahí. `brk` no puede solaparse con el stack más bajo y `fork()` deja de
copiar 1 MiB que nadie usa.

### 6.6 🔴 `fork()` corrompía la memoria de **todos** los procesos

El bug más serio encontrado en esta ronda. `vmm_create_address_space()` clona
el identity-map del kernel **superficialmente**: las entradas del hijo apuntan
a las `page_tables[]` estáticas. Para `exec`/`brk` eso es correcto (phys ==
virt, el PTE no cambia). Pero `vmm_copy_region()` (fork) mapea
`virt -> frame_privado`, y lo hacía con `vmm_map_page_in()`, que escribe en esa
tabla **global**: un solo `fork()` remapeaba `0x08048000…0x0F000000` a los
frames del hijo para el padre y para todo el mundo.

Arreglo: nueva `vmm_map_page_private_in()` (`mm/vmm.c`, exportada en
`mm/vmm.h`) que copia la tabla compartida a un frame propio antes de tocar el
PTE, e invalida la TLB si el pd es el activo. `mm/fork.c` la usa.

### 6.7 Syscalls: 74/74 con handler

Se completaron los 4 que eran placeholder y se añadió uno nuevo:

| Syscall | Antes | Ahora |
|---|---|---|
| `SYS_TIME` (28) | sin handler | `timer_get_ticks()` |
| `SYS_PUTPIXEL` (29) | "reservado" | `fb_put_pixel()` si hay framebuffer |
| `SYS_DUP2_FD` (34) | placeholder | clona fds de archivo y redirige extremos de pipe sobre 0/1/2 (`low_fd_map[]` en `fs/pipe.c`); `SYS_WRITE`/`FILE_READ`/`FILE_WRITE` pasan por `pipe_low_fd()` |
| `SYS_VFS_CAP` (46) | sin handler | copia la captura de VGA (`vga_capture_data/len`) |
| `SYS_NET_PING` (74) | — | **nuevo**, ICMP desde ring 3 |

`user/trinux.h` gana `dup2_()` y `ping_()`.

### 6.8 CI que arranca el kernel (antes solo medía el tamaño del .bin)

`.github/workflows/ci.yml` y `build.yml` comprobaban
`[ $(stat -c%s mykernel.bin) -gt 102400 ]`. Un kernel que enlazaba pero hacía
panic en `process_init()` pasaba en verde. Ahora:

- `scripts/smoke_test.sh`: arranca `mykernel.bin` en `qemu-system-i386
  -display none -serial file:… -no-reboot`, espera, y exige en la salida
  serie: IDT, paginación, PMM, scheduler, interrupciones, VFS, `/bin`, salto a
  ring 3 y entrada real en el usuario; además **falla** si aparece `PANIC`,
  `PAGE FAULT EN KERNEL` o `triple fault`, y exige ≥ 15 `[ OK ]`. Sube
  `boot-full.log` como artefacto. Sin QEMU instalado avisa y sale 0, para no
  romper entornos sin virtualización.
- `.github/workflows/audit.yml` (nuevo): 74 syscalls declarados vs
  implementados, `scripts/gen_embedded.py --check`, "no hay `.o`/`.bin`/`.iso`
  versionados", "ninguna llamada implícita en el kernel" y "el userland
  compila entero".

### 6.9 Limpieza del repo

- **35 `.o` versionados** (`auth/users.o`, `boot/gdt.o`, …) que `make` no
  reconstruía: si tocabas un `.asm` se usaba el objeto viejo. Eliminados del
  índice y las excepciones `!boot/boot.o` de `.gitignore` borradas.
- `trinux.iso` (binario de ~70 MiB) eliminado del índice.
- `mini_test.c`, `slow.c`, `tce.c` movidos a `examples/` con `README.md`
  propio: estaban en la raíz y cualquier `gcc *.c` tropezaba con ellos.
- `scripts/gen_embedded.py`: regenera `kernel/{tce,mini_test,slow}_src.h` desde
  `examples/`. Antes se generaban a mano con `xxd` y se desincronizaban.
- `trinux.h`: los comentarios de los syscalls 28/29/34/46 ya no dicen
  "placeholder"/"reservado".

### 6.10 Lo que NO se ha arreglado (y por qué)

| Hueco | Estado | Motivo |
|---|---|---|
| Aislamiento de memoria real (§3.1) | ❌ abierto | Requiere ELF en VA por proceso + `cr3` por proceso. Cambia el modelo de ejecución completo (hoy `exec` es síncrono con `setjmp/longjmp`); sin poder arrancar en QEMU ni ensamblar NASM aquí, es demasiado riesgo para ir a ciegas. |
| `fork()` devuelve 0 al hijo | ❌ abierto | El scheduler sí puede correr al hijo, pero el `ret` del hijo en ring 3 necesita fabricar un trap frame; verificado el diseño, no la implementación. **Nota**: el `longjmp` de un solo nivel que lo habría roto está arreglado (§7.1). |
| SMP con locks (§3.5) | ❌ abierto | Los APs siguen en `sti; hlt`. Añadir spinlocks sin poder probar concurrencia es empeorar el riesgo. |
| fds por proceso | ⚠️ parcial | `kfds[]` sigue siendo global, pero desde la ronda 3 `kfd_get()` **valida `owner_pid`** en read/write/close/seek/dup2: un proceso ya no puede usar los fds de otro. Falta la tabla completa por proceso. |
| `SYS_BRK` no desmapea | ❌ abierto | Conservador a propósito: desmapear liberaría frames del identity-map compartido. |
| Relocación de ELF (`.rel`) | ❌ abierto | Solo valen binarios a `0x08048000`. |
| Prueba en hardware real | ❌ no hecho | No hay acceso a hardware ni a QEMU en este entorno (ver §4). |

### 6.11 Estado verificado al cerrar la ronda 2

```
kernel:     51 .c → 0 errores, 0 llamadas implícitas
link:       ld -m elf_i386 → OK (620 KiB)
arranque:   21 × [ OK ], sin panic, hasta "[init] launching /bin/sh in RING 3"
userland:   72/72 programas + sh (8456 B) + 2 alias
syscalls:   74 declarados / 74 con handler
```

## 7. Ronda 3 — bugs encontrados revisando, no solo listando

Segunda pasada buscando activamente (no leyendo la lista de pendientes). Todo
compila (`0` fallos, `0` implícitas), enlaza y arranca hasta `/bin/sh` en
ring 3 con 21 `[ OK ]`.

### 7.1 🔴 La pila de `setjmp/longjmp` de ring 3 era de un solo nivel

`cpu/syscall.c` tenía **un** `g_elf_jmp` global. `SYS_EXIT` y el manejador de
`#PF` de userland hacían `longjmp(&g_elf_jmp)`. Como `elf_exec_argv()` se
anida (shell ring 3 → `SYS_SPAWN` → el hijo hace otro spawn), cada nivel
llamaba a `elf_arm_exit_jmp()` y **sobrescribía el buffer del nivel anterior**.
Al terminar el hijo, el `longjmp` saltaba a un `EIP`/`ESP` del *otro* marco
de pila: corrupción de la pila del kernel justo al volver de un `exec`
anidado. Ahora es una pila de `ELF_EXIT_MAX_NEST` buffers (`g_exit_stack[]`),
y `elf_unwind()` solo desarma el tope.

### 7.2 🔴 Cualquier proceso podía robar/cerrar los fds de otro

`kfds[]` es una tabla global con `owner_pid`, pero **nadie lo comprobaba**:
`SYS_FILE_READ`, `SYS_FILE_WRITE`, `SYS_FILE_CLOSE` y `SYS_FILE_SEEK` solo
miraban `used`. Un proceso sin privilegios podía cerrar el fd que el shell
tuviera abierto sobre `/etc/shadow`, leerlo o moverle el offset. Se añadió
`kfd_get(fd)`, que valida `owner_pid`, y los cuatro handlers lo usan.

### 7.3 🔴 `SYS_FILE_SEEK` desbordaba `k->pos`

`newpos = k->pos + off` en `uint32_t` con `off` negativo grande daba la vuelta
y dejaba el offset por encima del tamaño del archivo (el siguiente `read`
leía con `off` fuera de rango). Reescrito con aritmética `int64_t` y
saturación a `[0, 0x7FFFFFFF]`.

### 7.4 🔴 `SYS_WRITEFILE` no comprobaba permisos de escritura

`SYS_READFILE` sí tenía `vfs_check_access(n, ACC_READ)`, pero el de escritura
**no tenía el equivalente**. `writefile("/etc/shadow", ...)` reescribía las
contraseñas, y `writefile("/bin/ls", ...)` truncaba binarios del sistema
(`vfs_create` devuelve el nodo existente). Añadido `ACC_WRITE`.

### 7.5 🔴 Overflow de `uint32` en `ramfs_write` / `diskfile_write`

`needed = off + size` con `size = 0xFFFFFFF0` daba la vuelta; `krealloc`
devolvía un buffer minúsculo y el `memcpy` copiaba GB desde el buffer del
usuario **encima del heap del kernel**. En el camino con disco, el mismo
desbordamiento hacía un `ensure_blocks()` con un `nblocks` pequeño y luego
un `memcpy` gigante. Ambas rutas acotan ahora a `RAMFS_MAX_FILE_BYTES`
(16 MiB) con la resta protegida.

### 7.6 Overflow de pila en `vfs_get_path()`

Reconstruía la ruta con `strcpy(tmp, piece); strcat(tmp, rev); strcpy(rev, tmp)`
sobre buffers de `PATH_MAX`. Con anidamiento profundo (64 niveles) y nombres
de 64 bytes cada iteración se pasa: desbordamiento de la pila del kernel
alcanzable con `mkdir -p` + `cd`. Reescrito en un solo buffer acotado.

### 7.7 `path_normalize()` / `path_join()` sin límites

`strcpy(out, ...)` + `strcat(out, ...)` con `out` de tamaño desconocido desde
la firma: cualquier ruta de más de 256 bytes desbordaba el buffer del
llamante. Ahora todo se copia acotado y se trunca en un límite de componente
(con aviso por serial). Verificado con un banco de pruebas fuera del kernel
(`/tmp/normtest.c`) sobre los 6 casos típicos + una ruta de 1200 bytes.

### 7.8 El tokenizador del shell dejaba tokens sin terminar

En `tokenize()`, el test `dst < scratch_end` se usaba también para escribir el
`'\0'` final, así que en una línea larga el último token quedaba **sin
terminador** y cualquier `strlen()` posterior leía la pila hasta encontrar un
cero. Corregido a `dst <= scratch_end`.

### 7.9 `patch32()` del `tcc` in-kernel escribía fuera de `code[]`

`e8()` descartaba bytes al llenarse el buffer, pero `patch32(off, ...)`
escribía `code[off..off+3]` **sin comprobar nada**. Con un fuente grande, los
offsets de los fixups se salían del array y escribían encima de `data_sec[]`,
`locals[]` o `globals[]` (están juntos en `.bss`). Ahora hay una bandera
`tcc_error` pegajosa: `patch32()` es no-op fuera de rango y `tcc_compile()`
aborta sin escribir salida. Mismo guardia añadido a los fixups de `tasm.c`.

### 7.10 Verificación de esta ronda

```
kernel:     51 .c → 0 errores, 0 llamadas implícitas
link:       OK (620 KB)
arranque:   21 × [ OK ], hasta "[init] launching /bin/sh in RING 3"
userland:   72/72 + sh
make audit: OK (syscalls 74/74, headers, artefactos, regresiones)
```

Los chequeos de §7.1–7.9 están automatizados en `.github/workflows/audit.yml`
(paso "Regresiones de seguridad / robustez"): si alguien vuelve a introducir
el `longjmp` global, el `owner_pid` sin comprobar o el `strcat` sin límite, el
CI falla.

## 8. Ronda 3b — análisis estático (`-fanalyzer`)

En lugar de seguir leyendo código a ojo, se pasó el analizador de GCC 12
(`-fanalyzer`) por los 51 archivos del kernel. Encontró dos cosas reales y
permitió descartar muchas sospechas.

### 8.1 🔴 `process_init()` desreferenciaba `NULL`

```c
process_t *init = spawn_kthread("init", PROC_RUNNING);
init->priority = PRIO_IDLE;          // ← sin comprobar
process_t *kth  = spawn_kthread("kthreadd", PROC_SLEEPING);
kth->priority  = PRIO_IDLE;          // ← idem
```

`spawn_kthread()` devuelve `NULL` si no hay hueco en la tabla de procesos o
si falla el `kmalloc` del kstack. En ese caso el arranque hacía `#PF` en
ring 0 **antes** de llegar al shell, sin mensaje. Ahora se comprueba cada
puntero (si `init` es NULL, `current` se queda NULL y el resto sigue).

### 8.2 `KERNEL_END_VIRT` definido dos veces

`mm/vmm.h` decía `0x05000000` y `mm/vmm.c` lo definía otra vez. Mientras los
dos valores coincidan no pasa nada, pero es exactamente el tipo de deriva que
ya provocó la desincronización de los límites de usuario (§6.5). Ahora vive
solo en `include/userlayout.h` y `vmm.h` lo reexporta.

### 8.3 `fb_set_cursor()` con dos `if` engañosos

```c
if (col < 0) col = 0; if (col >= cols) col = cols - 1;
```

`-Wmisleading-indentation`: el segundo `if` **no** está dentro del primero.
Se ve a simple vista, y precisamente por eso alguien añadirá llaves algún día
y cambiará el comportamiento sin querer. Explicitado, y con guarda por si el
framebuffer no se inicializó (`cols == 0` dejaba `col = -1`).

### 8.4 Lo que `-fanalyzer` NO encontró

Ningún doble free, uso después de free, ni buffer overflow detectable en
`fs/`, `mm/`, `drivers/`, `auth/`, `kernel/` ni `shell/commands.c`. Los
problemas de esta ronda y de la anterior eran de **lógica** (un `owner_pid`
que nadie miraba, un `longjmp` global, un `if` que no comprobaba límites), no
de gestión de memoria, y eso no lo ve un analizador.

Automatizado como `make analyze` y como paso "Análisis estático
(-fanalyzer)" en `.github/workflows/audit.yml`.

## 9. Ronda 3c — límites `uint32 -> int` en la entrada de usuario

Patrón repetido que encontré al revisar el camino de teclado: el kernel
recibe tamaños como `uint32_t` y los castea a `int` para llamar a funciones
del driver que esperan `int`. Si el valor tiene el bit 31 puesto, el cast
produce un **número negativo**, y la función del driver no está preparada
para eso (`len < max_len - 1` con `max_len` negativo es siempre falso, así
que el guardia no protege nada, pero la escritura del terminador sí ocurre).

### 9.1 `SYS_GETLINE` (`getline(buf, 0x80000000)`)

```c
if (max == 0) { ... }              // 0x80000000 pasa
if (!uaccess_ok(buf, max)) ...     // “válido” si el rango cae en user space
keyboard_readline(buf, (int)max);  // ← int negativo
```

`uaccess_ok(buf, 0x80000000)` también sale mal por el mismo motivo, pero el
efecto visible es el `buffer[-1]` del terminador. Se acota `max` a 4096
(una línea de shell) **antes** del cast.

### 9.2 `read(0, buf, len)` en `SYS_FILE_READ`

Idéntico: `int len = (int)a3;` y luego `uaccess_ok(b, (uint32_t)len)` con
`len` negativo otra vez. Mismo acotado a 4096.

### 9.3 `keyboard_readline()` no se defendía sola

Aunque los dos llamantes ya acotan, el driver también se protege ahora:
`if (!buffer || max_len <= 0) return 0;`. Antes con `max_len == 0` escribía
`buffer[-1]` al pulsar Enter, y con `buffer == NULL` escribía en la
dirección 0.

`vga_set_cursor()` (usado por `SYS_SET_CURSOR` con coordenadas del usuario)
**sí** acotaba ya los dos ejes a `[0, VGA_WIDTH/HEIGHT)`, así que ahí no había
nada que arreglar.

## 10. Ronda 3d — tres bugs en el asignador del kernel (`mm/kheap.c`)

El heap es first-fit con coalescing sobre un arena estático de 32 MiB. Al
revisarlo con una simulación fuera del kernel (`/tmp/kheaplab.py`,
reimplementación 1:1) salieron tres cosas.

### 10.1 🟠 `kmalloc_aligned()` gastaba ~4 KiB de más en cada asignación

Pedía siempre `size + 4096 + sizeof(block_t)` para poder desplazar el payload
hasta el siguiente límite de página, y si el bloque **ya** venía alineado
hacía `return raw;` — es decir, el llamante se quedaba con un bloque de
`size + pad` pero `kfree()` solo puede devolver el bloque entero. Ninguna de
esas páginas extra vuelve nunca al arena mientras el bloque esté vivo.

Medido: 200 × `kmalloc_aligned(8 KiB)` consumían **2.3 MiB** para 1.6 MiB
pedidos (≈700 KiB perdidos). `process.c` usa esta función para **cada
kstack**, así que se paga en cada proceso creado.

Arreglo: camino exacto cuando el bloque ya está alineado (`kmalloc(size)`,
sin pad). El caso con desplazamiento se mantiene, y el hueco delantero queda
como bloque libre reutilizable en vez de perderse.

### 10.2 🟠 `align_up()` podía devolver 0 y colar una petición imposible

`(n + 7) & ~7` con `n = 0xFFFFFFFF` da la vuelta a **0**. `kmalloc(0xFFFFFFFF)`
pasaba el test de "cabe en el arena" y devolvía un puntero válido a un bloque
minúsculo: el llamante escribía encima de todo lo que hubiera detrás.
Añadido `KHEAP_MAX_REQUEST` (el tamaño del arena): una petición mayor no se
puede satisfacer nunca, así que rechazarla es correcto.

### 10.3 🟠 `kfree()` leía `b->magic` sin validar el puntero

`block_from_ptr(ptr)` resta `sizeof(block_t)` **antes** de mirar nada. Un
puntero basura (o de un buffer que no venga de `kmalloc`) podía "colar" si la
palabra en `ptr - sizeof(block_t)` coincidía con `MAGIC_USED`; entonces
`kfree()` enlazaba un bloque falso en la lista de libres y el siguiente
`kmalloc()` escribía en cualquier dirección. Ahora se comprueba que el
puntero (y el `raw` recuperado del camino alineado) caiga dentro de
`[heap, heap + HEAP_SIZE)` antes de tocar ninguna cabecera.

### 10.4 Verificación

```
kernel:    51 .c → 0 errores, 0 implícitas, 0 warnings en kheap.c
-analyzer: limpio en kheap.c
link:      OK
arranque:  21 × [ OK ], sin panic, hasta "launching /bin/sh in RING 3"
```

## 11. Ronda 3e — el PMM reservaba RAM que no existe (hardware real)

`detect_memory()` decidía cuánta memoria tiene la máquina con
`mem_upper * 1024 + 1 MiB`, es decir **"toda la RAM por encima de 1 MiB"**,
ignorando el mapa de memoria (`mmap`) que Multiboot ya entrega.

`pmm_init()` indexa su bitmap como "frames contiguos desde la dirección 0",
así que esa cifra se interpreta como *memoria contigua utilizable*. En QEMU
(con `-m 512M` casi todo es contiguo) da igual. En un portátil real no: los
huecos de MMIO de PCI están en ~3–4 GiB, y el PMM acababa marcando como
libres frames del hueco. La primera asignación que cayera ahí escribía en un
agujero del mapa, no en RAM.

Arreglo: si hay `mmap`, se busca el tramo **disponible que contiene 0x100000**
(donde empieza el kernel) y se usa su final; si no, se mantiene `mem_upper`.

Medido con mapas simulados:

| Mapa | Antes (`mem_upper`) | Ahora (mmap) |
|---|---|---|
| QEMU `-m 512M` | 512 MiB | 512 MiB |
| Portátil 4 GiB (hueco PCI 3–4 GiB) | **4096 MiB** ❌ | 3071 MiB ✅ |
| Máquina pequeña 64 MiB | **64 MiB** ❌ | 49 MiB ✅ |

**Ojo con el arreglo "obvio"**: cortar en el primer hueco del mapa no vale,
porque *todos* los mapas reales tienen un hueco en `0x9FC00..0x100000`
(EBDA/VGA). Lo escribí así primero y el resultado eran 640 KiB — el kernel se
quedaba sin memoria. Se ve en la traza que hice antes de dar con la versión
buena; el comentario del código lo explica para que no se repita.

También se acotó `pmm_reserve_region()`: `addr + len + 4095` desbordaba
`uint32` y `end` daba la vuelta, dejando la región **sin reservar** (p. ej. al
reservar el framebuffer si GRUB lo coloca cerca de 4 GiB).
