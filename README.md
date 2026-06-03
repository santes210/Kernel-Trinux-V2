# Trinux

Un pequeño **kernel estilo Unix/Linux** escrito desde cero en **C** y
**ensamblador x86 (NASM)**. Arranca vía **Multiboot** (GRUB /
`qemu-system-i386 -kernel`), corre en modo protegido de 32 bits, y te deja
en una **shell interactiva** con un montón de comandos integrados, un
sistema de archivos en RAM con persistencia en disco, gestión de memoria,
interrupciones, teclado, timer, dispositivos en `/dev`, `dd` estilo Linux,
multiusuario, y mucho más.

Funciona tanto en **QEMU** como en **hardware real** (USB bootable) gracias
a sus drivers IDE *y* AHCI/SATA con detección automática vía PCI.

```
user@mykernel:/$ neofetch
      .--.        user@mykernel
     |o_o |       OS: mykernel 0.2.0
     |:_/ |       Arch: i686
    //   \ \      Kernel: x86 32-bit protected mode
   (|     | )     Uptime: 2 s
  /'\_   _/`\     Memory: 16/63 MB
  \___)=(___/     Disk: 882 B used / 1024 MB
                  Shell: mysh
```

---

## 🚀 Inicio rápido

### En Linux

```sh
make          # compila mykernel.bin
make run      # lo arranca en QEMU (Multiboot, -kernel)
```

Luego escribe `help` en el prompt para ver todos los comandos.

> La compilación usa el `gcc -m32`, `nasm`, `ld` y `qemu-system-i386` de tu
> sistema. Si tienes una toolchain cruzada `i686-elf` puedes usar:
> `make USE_CROSS=1` (llamará a `i686-elf-gcc` / `i686-elf-ld`).

### En hardware real (USB bootable) 💾

Genera una imagen bootable y grábala a una USB:

```sh
./make-usb-image.sh 15360    # genera mykernel-usb.img de 15 GB
sudo dd if=mykernel-usb.img of=/dev/sdX bs=4M status=progress conv=fsync
```

> ⚠️ Cambia `sdX` por tu USB real (verifica con `lsblk` antes — **BORRA TODO**
> en esa unidad).

Bootea tu PC desde la USB (Legacy/CSM en BIOS). El kernel detecta
automáticamente si el disco está en un controlador **IDE** o **AHCI/SATA**
(vía PCI scan) y usa el driver adecuado. Funciona en laptops modernas como
la HP Stream 14 donde no hay IDE legacy.

### En Termux (Android) 📱

Si quieres correr Trinux en tu teléfono Android, mira **[TERMUX_QUICKSTART.md](TERMUX_QUICKSTART.md)**.
Resumen ultra rápido:

```sh
pkg install qemu-system-i386-headless
qemu-system-i386 -drive file=trinux.img,format=raw,if=ide -m 512M -display curses
```

Login: `user`/`user` o `root`/`root`.

---

### Requisitos

| Herramienta      | Paquete Debian/Ubuntu      |
|------------------|----------------------------|
| GCC (32-bit)     | `gcc` + `gcc-multilib`     |
| NASM             | `nasm`                     |
| GNU ld           | `binutils`                 |
| QEMU             | `qemu-system-x86`          |
| (opcional) ISO   | `grub-pc-bin xorriso`      |

```sh
sudo apt-get install build-essential gcc-multilib nasm qemu-system-x86
```

### Targets del Makefile

| Target              | Qué hace                                                  |
|---------------------|----------------------------------------------------------|
| `make`              | Compila + enlaza `mykernel.bin`                           |
| `make run`          | Arranca en QEMU (`-kernel mykernel.bin`, salida serial)   |
| `make run-curses`   | Arranca en QEMU con display tipo terminal (curses)        |
| `make iso`          | Genera una ISO booteable `mykernel.iso` (usa grub-mkrescue) |
| `make run-iso`      | Genera la ISO y la arranca vía `-cdrom`                   |
| `make clean`        | Borra los .o y los binarios                               |

### Imagen única bootable + persistente

Si quieres **un solo archivo** que bootee Y guarde tus datos (ideal para
USB físico o Termux), usa el script:

```sh
./make-usb-image.sh 1024     # genera trinux.img de 1 GB
qemu-system-i386 -drive file=mykernel-usb.img,format=raw,if=ide -m 512M
```

Lo puedes grabar a un USB real con `sudo dd if=mykernel-usb.img of=/dev/sdX bs=4M`
(¡cuidado con `sdX`, verifica con `lsblk` antes!).

---

## ✨ Lo que tiene implementado

**Fase 1 — Boot**

- Header Multiboot v1 (`boot/boot.asm`), kernel enlazado en 1 MiB.
- Setup de GDT (`boot/gdt.c` + `boot/gdt_flush.asm`): null, ring0 code/data, ring3 code/data.
- Bootloader MBR clásico en modo real de 512 bytes incluido como referencia (`boot/mbr_boot.asm`).

**Fase 2 — Driver VGA texto** (`drivers/vga.*`)

- Acceso directo a `0xB8000`, 16 colores, cursor por hardware, scrolling.
- Manejo de `\n \t \b \r`, `vga_printf`.

**Fase 3 — Interrupciones** (`cpu/*`)

- IDT con 256 entradas, ISR 0-31 con mensajes descriptivos + dump de registros,
PIC (8259) remapeado a 32-47, registro de handlers por IRQ, EOI.

**Fase 4 — Entrada**

- Teclado PS/2 (`drivers/keyboard.*`): scancode set 1, Shift/Caps/Ctrl/Alt,
flechas, F1-F12, Home/End/Del, buffer circular, getchar bloqueante + readline.
- Timer PIT (`drivers/timer.*`): 100 Hz, ticks, `sleep()`, `uptime()`.

**Fase 5 — Memoria**

- Gestor de memoria física (`mm/pmm.*`): bitmap de frames, detección via multiboot.
- Paging (`mm/vmm.*`): identity-map de 256 MiB + mapeo dinámico de MMIO para
regiones altas (AHCI ABAR, etc.) con pool de page tables extra.
- Heap del kernel (`mm/kheap.*`): first-fit + coalescing, `kmalloc`,
`kmalloc_aligned`, `kfree`, `krealloc`, `kheap_stats`.

**Fase 6 — Sistema de archivos**

- Capa VFS (`fs/vfs.*`) con callbacks por nodo (read/write/readdir/finddir).
- Filesystem RAM (`fs/ramfs.*`) con árbol inicial
(`/bin /etc /home/user /tmp /var/log /dev /mnt /root`, `/etc/hostname`, `/etc/motd`).
- Resolución de rutas (`fs/path.*`): normalizar, juntar, basename, dirname, `.`/`..`.
- **Persistencia en disco** (`drivers/ata.*` + `fs/diskfs.*`): driver de disco
unificado que soporta **IDE PIO** y **AHCI/SATA** con detección automática
(ver Fase 10). `fs/diskfs.c` serializa el árbol VFS (dirs + files +
permisos/owners) como snapshot de metadatos (`MKFS` superblock + registros de
nodos en preorder) al final del disco y lo restaura al boot. Ejecuta `sync`
en la shell para guardar; el kernel auto-carga al arrancar así que **los
archivos sobreviven a reboots/apagados**.
- **Almacenamiento por bloques** (`fs/blockfs.*`): cuando hay disco presente, el
*contenido* de los archivos vive en **bloques de 4 KiB en el disco**, leídos y
escritos bajo demanda en lugar de en el heap del kernel. Un **bitmap** rastrea
la asignación de bloques. Esto significa que los datos no consumen RAM y el
almacenamiento total está limitado por **el tamaño del disco, no por la RAM**.
- **Dispositivos `/dev`** (`fs/devfs.*`): `/dev/sda` (disco crudo), `/dev/zero`,
`/dev/null`, `/dev/random` — los clásicos de Unix, totalmente integrados con
el VFS y con permisos correctos.

**Fase 7 — Procesos**

- PCB + tabla de procesos (`process/process.*`): pid, estado, contexto, cwd.
- Procesos iniciales estilo Unix: PID 1 = `init`, PID 2 = `kthreadd`, PID 3 = `mysh`.
- Cada comando ejecutado se registra como proceso temporal con su propio PID
(visible en `ps`/`top`).
- Esqueleto de scheduler round-robin (`process/scheduler.*`) + context switch
en ASM (`process/switch.asm`). Corre cooperativamente (la shell interactiva
es PID 1) para mantener el boot path estable; `ps`/`kill` operan sobre la
tabla de procesos.

**Fase 8 — Shell** (`shell/*`)

- Prompt `user@host:/path$` (con colores), editor de línea con **historial (↑/↓)**,
movimiento del cursor (←/→, Home/End), backspace, inserción.
- Comandos: ver tabla más abajo.
- Mensajes de error detallados estilo Unix (`Permission denied`,
`No such file or directory`, `File exists`, etc.) — no mensajes genéricos.

**Fase 9 — Librerías**

- `lib/string.*` (set completo de string/mem/ctype/conversiones), `lib/printf.*`
(`kprintf`/`snprintf` con ancho + padding zero/left), RTC/CMOS
(`drivers/rtc.*`), serial COM1 debug (`drivers/serial.*`).

**Fase 10 — PCI + AHCI/SATA (hardware real)** 🆕

- **Enumeración PCI** (`drivers/pci.*`): escaneo completo de los 256 buses ×
32 devices × 8 funciones para encontrar controladores por clase/subclase.
Acceso a configuración vía mecanismo 1 (puertos `0xCF8`/`0xCFC`). Soporte
para habilitar bus-mastering (DMA).
- **Driver AHCI** (`drivers/ahci.*`): inicialización completa del HBA
(BIOS/OS Handoff, detección de puertos con dispositivos SATA presentes,
rebase de Command List / FIS / Command Table con buffers DMA alineados),
IDENTIFY DEVICE para obtener el tamaño del disco, y lectura/escritura de
sectores vía **READ/WRITE DMA EXT** (48-bit LBA) con polling.
- **Driver de disco unificado** (`drivers/ata.c`): `ata_init()` intenta
primero **IDE PIO** en los puertos legacy `0x1F0` (funciona en QEMU y PCs
antiguos). Si no hay IDE, **escanea PCI** buscando un controlador AHCI
(clase `01:06:01`) y lo inicializa. La API pública (`ata_read_sectors`,
`ata_write_sectors`, `ata_present`, `ata_total_sectors`) no cambia — todo
el resto del kernel (diskfs, blockfs, devfs, `dd`, `sync`) funciona
idéntico sin importar si el backend es IDE o AHCI.
- **Mapeo MMIO extendido** (`mm/vmm.c`): el VMM ahora puede mapear páginas
en cualquier parte del espacio de 4 GiB (no solo los primeros 256 MiB),
usando un pool estático de page tables extra. Esto es necesario porque los
registros AHCI (ABAR) típicamente viven en `0xFEBxxxxx`.

> **Resultado**: una USB con Trinux ahora funciona tanto en QEMU (IDE) como
> en laptops/PCs modernos (AHCI) **sin cambiar nada**. Al bootear verás:
> ```
> [ata] no legacy IDE; probing PCI for AHCI...
> [pci] AHCI found at 0:1f.2  vendor=8086 device=...  ABAR=febf1000
> [ OK ] Disk detected via AHCI/SATA (15360 MiB)
> ```

---

## 🛠️ Comandos de la shell

**Archivos y navegación:**
`ls` (`-l`, `-a`, combinables como `-la`; colorea dirs en azul y ejecutables
en verde), `cd`, `pwd`, `mkdir`, `rmdir`, `touch`, `rm`, `cat`,
`echo` (con redirección `>` truncar y `>>` añadir), `write`, `edit`/`nano`, `cp`, `mv`, `stat`,
`tree`, `find`, `head`, `tail`, `wc`

> **`edit <archivo>`** (alias **`nano`**) abre un editor de texto a pantalla
> completa: flechas / Home / End para moverte, Enter para partir líneas,
> Backspace para borrar, **Ctrl-S** para guardar, **Ctrl-K** para cortar línea,
> **Ctrl-X** para salir (pregunta si guardar). Combínalo con `sync` para
> persistir al disco.

**Sistema:**
`clear`/`cls`, `help`, `uname`/`uname -a`, `uptime`, `date`, `whoami`,
`hostname`, `free`, `df`, `ps`, `top`, `kill`, `sync`, `reboot`, `shutdown`/`halt`

> **`top`** es un monitor del sistema a pantalla completa (estilo htop): barras
> de color para memoria física, heap del kernel y uso de disco, más la tabla
> de procesos con estados coloreados. Se auto-refresca cada ~2 segundos.
> Pulsa `r` para refrescar ahora o `q`/`ESC`/`Enter` para salir.

> **`df`** reporta uso de disco (tamaño / usado / disponible), y `neofetch`
> también muestra una línea **Disk:**.

> **`sync`** escribe todo el filesystem al disco para que tus archivos persistan
> entre reboots. `make run` adjunta automáticamente un `disk.img` (creado en
> la primera ejecución). Prueba: crea un archivo, ejecuta `sync`, rebootea
> QEMU — tu archivo se restaura al arrancar (`[ OK ] Filesystem restored from disk`).

**Acceso a dispositivos y disco crudo:**

> **`dd`** estilo Linux completo: `if=`, `of=`, `bs=`, `count=`, `skip=`,
> `seek=`, `conv=notrunc,sync,noerror`, `status=progress`. Soporta sufijos de
> tamaño (`1K`, `2M`, etc.). Ejemplos:
> ```
> dd if=/dev/zero of=/tmp/big bs=1K count=64
> dd if=/dev/sda of=/tmp/mbr bs=512 count=1
> dd if=/dev/random of=/tmp/noise bs=16 count=8
> dd if=archivo.txt of=/dev/null
> ```

> **`/dev/sda`** expone el disco físico como bytes crudos (solo root, igual que
> Linux real). **`/dev/zero`** devuelve infinitos ceros, **`/dev/null`** descarta
> escrituras, **`/dev/random`** genera bytes pseudoaleatorios.

**Usuarios y cuentas:**
`login`, `logout`, `su`, `useradd`, `passwd`, `whoami`, `id`, `users`, `groups`

**Permisos:**
`chmod`, `chown`, `umask` (más `ls -l` y `stat` con info de owner, sticky bit)

**Filtros de texto:**
`sort` (`-r` reverse, `-u` unique, `-n` numeric), `uniq` (`-c` count),
`cut` (`-d<delim> -f<n>`, `-c<n>`), `tee` (`-a`), `seq`

**Pipes y redirección:**
`grep` (`-i -v -n -c`), pipes `|`, redirección de salida `>` / `>>` y de
entrada `<`. Ejemplos:
`cat /etc/passwd | grep root`, `ls / | wc`, `ls -l /etc | grep -n host`,
`wc < /etc/motd`, `grep root < /etc/passwd`, `echo hola >> log.txt`,
`sort archivo | uniq -c`, `cut -d: -f1 /etc/passwd`, `seq 1 5 | tee nums.txt`

**Utilidades de ruta y entorno:**
`basename`, `dirname`, `which`, `env`, `yes`

**Userspace (ring 3):**
`usertest` (lanza un programa no privilegiado que habla con el kernel solo por
syscalls `int 0x80`) y `usertest bad` (demuestra el aislamiento: el programa
intenta una instrucción privilegiada y el CPU lo bloquea con un #GP)

**Ayuda:**
`help` (lista en columnas) y `help <comando>` (detalle de un comando)

**Extras:**
`neofetch`, `calc`, `hexdump`, `color`, `history`, `alias`

### Usuarios / multi-usuario

El kernel mantiene una base de datos de cuentas a nivel sistema (como Linux),
respaldada por `/etc/passwd` y `/etc/shadow` en el filesystem RAM. Esto es
**gestión de cuentas**, no aislamiento de hardware — todo corre en ring 0,
pero el kernel rastrea quién está logueado y aplica restricciones a operaciones
solo-root en la shell.

Cuentas por defecto (contraseña = nombre de usuario):

| usuario | uid  | home         | password |
|---------|------|--------------|----------|
| root    | 0    | /root        | `root`   |
| user    | 1000 | /home/user   | `user`   |

Al arrancar te aparece un prompt `login:`. Prueba:

```sh
login: user      # password: user
whoami           # -> user
id               # uid=1000(user) gid=1000 ...
su root          # password: root  -> prompt rojo, muestra '#'
useradd carlos pass123
passwd carlos    # (root puede cambiar cualquier password)
users            # root user carlos
logout           # vuelve a la pantalla de login
```

- Usuarios normales tienen un prompt verde `user@host:~$`; **root** tiene un
prompt rojo `root@host:~#` (nota el `#` vs `$`, como en Unix de verdad).
- `useradd` y cambiar la password de otro usuario son **operaciones solo-root**.
- Las passwords se guardan en plaintext en `/etc/shadow` a propósito (kernel
educativo, sin crypto). `cd` solo y `cd ~` van al home del usuario logueado.

### Permisos de archivos (rwx + propiedad)

Cada archivo/directorio tiene un **uid/gid de owner** y bits de permiso `rwx`
estilo Unix. El VFS los aplica en read/write/create/delete:

- `chmod 600 archivo` — pone permisos (octal). Solo el **owner o root** puede chmod.
- `chown <usuario> archivo` — cambia owner. **Solo root**.
- `ls -l` y `stat` muestran la cadena de permisos y el nombre del owner.
- **root (uid 0) bypassa todos los checks**, exactamente como Unix real.

Ejemplo trabajado (prueba que el aislamiento es real):

```sh
login: user            # password: user
echo topsecret > /home/user/priv.txt
chmod 600 /home/user/priv.txt    # solo el owner
su root                # password: root
useradd bob bob        # crea otro usuario
logout
login: bob             # password: bob
cat /home/user/priv.txt
#  -> cat: /home/user/priv.txt: Permission denied
```

El home de cada usuario es propiedad de ese usuario (uid 1000 para `user`,
homes creados para usuarios nuevos). Nota: esto es **enforcement de permisos**
**en software** (VFS + shell), no aislamiento ring-3 por hardware — el kernel
sigue corriendo en ring 0.

### umask, permisos por defecto y sticky bit

- **`umask`** controla qué bits de permisos se quitan a archivos/dirs nuevos.
Por defecto es `0022` (archivos nuevos = `644`, dirs nuevos = `755`).

  ```sh
  umask            # -> 0022
  umask 077        # restringe nuevos archivos al owner
  touch t.txt
  ls -l            # -> -rw------- ... t.txt   (0600)
  ```

- **Sticky bit** (`chmod 1xxx`): en un directorio sticky y world-writable
cualquiera puede crear archivos pero solo el **owner** del archivo (o del
directorio, o root) puede borrarlos. `/tmp` viene con `1777` (`drwxrwxrwt`),
exactamente como en un Unix de verdad:

  ```sh
  # como user:
  echo hi > /tmp/userfile
  su root && useradd bob bob && logout
  # como bob:
  echo bobs > /tmp/bobfile      # ok, /tmp es world-writable
  rm /tmp/userfile              # -> Operation not permitted (sticky dir)
  rm /tmp/bobfile               # ok, bob es el owner
  ```

### Pipes y grep

La shell soporta pipelines `|`: la salida de cada etapa se captura y se pasa
como stdin a la siguiente. `grep` filtra líneas; `cat`/`wc` también leen stdin
cuando no reciben argumento de archivo.

```sh
cat /etc/passwd | grep root      # solo la línea de root
ls / | wc                        # cuenta entradas
ls -l /etc | grep -n host        # -n añade números de línea
ps | grep kernel
grep -ivc user /etc/passwd       # -i ignore case, -v invert, -c count
```

Prueba:

```sh
help
ls -l /etc
cat /etc/motd
mkdir /home/user/docs && cd /home/user/docs
echo hola mundo > nota.txt
cat nota.txt
wc nota.txt
hexdump nota.txt
tree /
free
calc 6 x 7
date
neofetch
```

---

## 📁 Estructura del proyecto

```
mykernel/
├── Makefile            # build system (gcc -m32 / nasm / ld / qemu)
├── make-usb-image.sh   # script para crear .img bootable+persistente
├── linker.ld           # enlaza en 1 MiB, secciones alineadas a página
├── boot/               # entry multiboot, GDT, bootloader MBR de referencia
├── kernel/             # kernel_main, panic, info multiboot
├── drivers/            # vga, keyboard, timer, rtc, serial, ata, pci, ahci
├── cpu/                # idt, isr, irq, ports (inb/outb), syscall
├── mm/                 # pmm, vmm (con MMIO dinámico), kheap
├── fs/                 # vfs, ramfs, path, diskfs (persist), blockfs, devfs (/dev)
├── process/            # process, scheduler, context switch
├── auth/               # users (/etc/passwd, /etc/shadow), login/su
├── shell/              # shell (line editor/history) + commands + editor (estilo nano)
├── lib/                # string, printf, types
├── user/               # programas de demostración ring-3 (userprog)
└── include/            # kernel.h (versión/macros)
```

## 🤔 Notas y decisiones de diseño

- **Multiboot en lugar de un bootloader custom de 512 bytes.** `make run`
usa el cargador Multiboot built-in de QEMU. El MBR real-mode hecho a mano
(`boot/mbr_boot.asm`) se mantiene como referencia educativa y se puede ensamblar
standalone con `nasm -f bin`.
- **Detección automática IDE / AHCI.** En QEMU el disco aparece como IDE
(puertos `0x1F0`) y funciona directo. En hardware real (laptops modernas,
USB bootable) no hay IDE: el kernel escanea PCI buscando un controlador AHCI
(clase `01:06:01`), mapea sus registros MMIO con el VMM, y usa DMA para
leer/escribir. Toda la lógica vive en `drivers/ata.c` (wrapper unificado)
+ `drivers/pci.c` + `drivers/ahci.c`. El resto del kernel ni se entera
de qué backend está activo.
- **Scheduler cooperativo.** La API completa del scheduler y una rutina x86 de
context-switch existen, pero el cambio preemptivo desde la IRQ del timer está
deshabilitado por defecto; cambiar de tareas bajo una shell interactiva en
vivo es la parte más propensa a crashes de un kernel hobby, y el requisito
principal es "`make run` → shell funcionando". Para activarlo, haz que un
proceso llame `schedule()` cooperativamente, o conecta `scheduler_tick()` a
`schedule()` cuando añadas stacks de kernel por-tarea.
- **Compila con `-Wall -Wextra` sin warnings.**
- **Userspace en ring 3 + syscalls.** El kernel corre en ring 0, pero ahora
puede lanzar programas **no privilegiados en ring 3** que solo tocan el sistema
vía `int 0x80`. Hay TSS (`ss0:esp0`), un gate de syscall con DPL 3, y un
runner (`usermode_run`) que baja a ring 3 con su propio stack y vuelve al
kernel cuando el programa llama `SYS_EXIT`. Pruébalo con el comando **`usertest`**
(demo amable) y **`usertest bad`** (intenta una instrucción privilegiada → el
CPU lanza un General Protection Fault y el kernel **termina solo el programa**,
sin caerse). Syscalls: `exit, write, getpid, yield, sleep, getc, uptime`.

---

## 🗺️ Roadmap (ideas para el futuro)

Cosas que estaría guay añadir, en orden de "bang for the buck":

- [ ] Segundo disco IDE como `/dev/sdb` + comandos `mount`/`umount`
- [ ] Driver FAT12/16 (intercambiar archivos con Linux/Windows real)
- [ ] Driver de red NE2000 (¡Trinux con internet!)
- [ ] Modo gráfico VBE (640×480 con colores reales en vez de VGA texto)
- [x] **Userspace en ring 3 + syscalls** (¡hecho! ver `usertest`)
- [x] **PCI + AHCI/SATA** (¡hecho! disco funciona en hardware real)
- [ ] Cargar binarios ELF de usuario desde el filesystem (en vez de demos enlazadas)
- [ ] Procesos preemptivos reales (con interrupción del timer)
- [ ] NVMe (siguiente generación de almacenamiento después de AHCI)
- [ ] Más comandos: `tar`, `zip`, `ping`, `wget`...

## 📜 Licencia

Por definir (sugerencia: MIT o GPL-2.0).

---

*Trinux es un proyecto educativo. Si encuentras un bug o quieres añadir una
feature, ¡los pull requests son bienvenidos!*
