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

```
user@trinux:/$ neofetch
      .--.        user@trinux
     |o_o |       OS: Trinux 0.2.0
     |:_/ |       Arch: i686
    //   \ \      Kernel: x86 32-bit protected mode
   (|     | )     Uptime: 42 s
  /'\_   _/`\     Memory: 16/511 MB
  \___)=(___/     Disk: 0 MB used / 14336 MB
                  Shell: mysh
                  Battery: 72% (discharging)
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
qemu-system-i386 -drive file=trinux.img,format=raw,if=ide -m 512M -display curses
```

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
| Strings | `print("hola\\n");` |
| Declaraciones múltiples | `int x, y, z;` |
| Comentarios `//` y `/* */` | |

**Funciones built-in (syscall):**

| Función | Descripción |
|---|---|
| `print(str)` | Imprime string |
| `print_num(n)` | Imprime entero (soporta negativos) |
| `print_char(c)` | Imprime un carácter |
| `getchar()` | Lee una tecla (bloqueante) |
| `getline(buf, max)` | Lee una línea con echo + backspace |
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
- [ ] Driver de red (NE2000 / virtio-net)
- [ ] Modo gráfico VBE (640×480 o mayor)
- [ ] Procesos preemptivos reales
- [ ] NVMe (almacenamiento de siguiente generación)
- [ ] Soporte para más de 256 MiB de RAM
- [ ] Más comandos: `tar`, `ping`, `wget`, `ssh`...

## 📜 Licencia

Por definir (sugerencia: MIT o GPL-2.0).

---

*Trinux es un proyecto educativo escrito completamente desde cero.
Si encuentras un bug o quieres añadir una feature, ¡los pull requests
son bienvenidos!*
