# Por qué Trinux arranca en BIOS/MBR pero no en UEFI

Resumen: **el kernel no es el problema, es el camino de arranque.** Trinux es
un kernel Multiboot v1 de **32 bits** (`readelf -h` → `ELF32`, `Intel 80386`,
entrada `_start`). Eso funciona de forma nativa en BIOS/MBR y tiene tres
barreras distintas en UEFI, en este orden de importancia:

1. **Secure Boot** (la causa más probable si ni siquiera ves el menú GRUB).
2. **El módulo `multiboot` de GRUB-EFI** (si ves el menú pero falla al cargar).
3. **La consola**: en UEFI no existe el modo texto VGA `0xB8000`.

Y hay un cuarto problema que sí está en el repo: **la imagen "USB" que genera
el CI no tiene partición ESP**, así que no arranca en UEFI puro aunque el
firmware esté en modo CSM desactivado.

---

## 1. Secure Boot (mirar esto primero)

Si en la máquina UEFI **no aparece ni el menú de GRUB**, casi seguro es esto.

Para arrancar un kernel Multiboot desde GRUB-EFI, GRUB tiene que ejecutar
código que **no está firmado** por Microsoft/el fabricante. Eso es exactamente
lo que Secure Boot bloquea, y por eso las distribuciones **desactivaron** esos
módulos en las builds para EFI:

> "The multiboot{,2} modules aren't built for EFI anymore since commit
> 1295c2dffe42 ('Disable multiboot, multiboot2, and linux16 modules on EFI
> builds'). The reason … is that with them an attacker could bypass the
> Secure Boot mechanism and execute unsigned binaries (CVE-2015-5281)."
> — [Bugzilla 1654202](https://bugzilla.redhat.com/show_bug.cgi?id=1654202)

Resumiendo la situación real de los binarios de GRUB:

| Build de GRUB para EFI | ¿Tiene `multiboot`? |
|---|---|
| `grub-efi-ia32` de Debian/Ubuntu (sin Secure Boot) | Sí |
| `x86_64-efi` de Debian/Ubuntu | Sí, **pero** no se puede ejecutar desde el binario firmado de Secure Boot |
| Fedora/RHEL con Secure Boot | No (eliminado, ver arriba) |

**Prueba rápida**: entra en la BIOS/UEFI y desactiva **Secure Boot**. Si con
eso arranca, es exactamente esto. No es un bug de Trinux — le pasa a Xen, a
tboot y a cualquier kernel Multiboot.

---

## 2. El módulo `multiboot` de GRUB-EFI

Síntoma típico si llegas al menú GRUB y eliges Trinux:

```
error: can't find command `multiboot'.
```

o

```
error: file `/EFI/BOOT/x86_64-efi/multiboot.mod' not found.
```

Causas y arreglo:

- **Falta el paquete de módulos.** En Fedora/RHEL hay que instalar
  `grub2-efi-x64-modules` y copiar `multiboot.mod` (y `relocator.mod`) a
  `/boot/efi/EFI/<distro>/x86_64-efi/`. Es el procedimiento documentado para
  Xen y para tboot ([seimaxim](https://www.seimaxim.com/kb/cant-find-command-multiboot2-tboot-fails-in-uefi-system)).
- **El `grub.cfg` usa `multiboot` pero el binario EFI solo trae `multiboot2`.**
  Son comandos distintos; Trinux declara cabecera **Multiboot v1**
  (`0x1BADB002`), así que **no** sirve poner `multiboot2` en el `grub.cfg`.
- **Confusión de arquitectura.** `grub-efi-amd64-bin` produce GRUB en
  **64 bits**, y ese GRUB solo puede cargar kernels a los que antes pasa por
  su *relocator*. Para kernels de 32 bits lo correcto es **`grub-efi-ia32`**
  (GRUB EFI de 32 bits), que es el que el README ya menciona… pero que el
  workflow de CI **no instala** (ver §4).

---

## 3. La consola: en UEFI no hay `0xB8000`

Esto es lo que hace que, incluso cuando "arranca", parezca que no arranca.

Trinux escribe en `0xB8000` (modo texto VGA) salvo que GRUB le entregue un
framebuffer lineal. En BIOS/CSM eso siempre funciona: el firmware deja la
máquina en modo texto y ahí está la memoria de vídeo.

En UEFI **no existe el modo texto VGA**. Si el kernel no recibe framebuffer:

- `fb_init()` devuelve `false` (`drivers/fb.c`: exige
  `framebuffer_type == RGB` y `bpp == 32`),
- `display_init()` no activa el framebuffer,
- y todo lo que se escribe en `0xB8000` **no se ve**. El kernel puede estar
  corriendo perfectamente y tú ves una pantalla negra.

Diagnóstico sin pantalla: arranca con `-serial stdio` en QEMU, o en hardware
real con un cable serie / `console=ttyS0`. **Trinux ya escribe todo por COM1**
(`serial_write_char()`), así que si ves texto por el puerto serie y nada en
pantalla, es este caso y no un cuelgue.

Por qué GRUB-EFI a veces no da framebuffer en modo RGB 32bpp:

- El kernel pide en su cabecera `mode_type=0, 1366x768, 32bpp`
  (`boot/boot.asm`). Si GRUB no tiene ese modo exacto, debería caer al mejor
  disponible, pero en EFI su `video` se apoya en **GOP** y no en VBE, y las
  combinaciones que devuelve no siempre son `RGB` lineal.
- `fb_init()` es **estricto**: si el `bpp` no es exactamente 32 o el tipo no
  es `RGB`, rechaza el framebuffer y se queda en texto VGA. Un GOP que dé
  `BGR` o `bpp=24` es un caso perfectamente normal y hoy se descarta.

---

## 4. La imagen que genera el CI no tiene partición ESP

Esto sí es un bug del repo, independiente de la máquina.

`.github/workflows/build.yml` afirma generar una "ISO GRUB híbrida BIOS+UEFI"
y una "imagen USB" con "MBR híbrido GRUB (bootea en BIOS y UEFI)". Lo que hace
en realidad es:

```bash
grub-mkrescue --output=trinux.iso iso     # ← depende de lo que haya instalado
cp trinux.iso trinux-usb-512.img
truncate -s 512M trinux-usb-512.img        # ← solo añade ceros al final
```

`grub-mkrescue` **solo** añade el soporte EFI si encuentra las herramientas
`grub-efi-*` y `mtools`, y el workflow instala **`grub-efi-amd64-bin`**
(64 bits) cuando el README dice que hace falta **`grub-efi-ia32-bin`**
(32 bits). Si falla, `grub-mkrescue` no se queja: genera una ISO/MBR válida y
**sin** `EFI/BOOT/BOOT*.EFI`. Eso explica exactamente el síntoma que describes:
MBR/BIOS perfecto, UEFI nada.

Además, `truncate` no crea tabla de particiones: el resultado es una ISO con
MBR híbrido y 512 MiB de ceros detrás, sin ESP. Y **engordar la imagen
truncándola** es delicado porque el kernel usa el final del disco como zona de
persistencia (`diskfs`); en UEFI el firmware necesita leer la ESP del
principio, así que mezclar ambas cosas pide cuidado.

---

## 5. Cómo saber en qué punto se rompe (30 segundos)

| Qué ves | Dónde se rompe | Acción |
|---|---|---|
| No aparece ni el menú GRUB | Secure Boot / el firmware no arranca el medio | Desactiva Secure Boot; comprueba que el USB está en FAT32 con `EFI/BOOT/BOOTIA32.EFI` |
| Menú GRUB, y al elegir: `can't find command 'multiboot'` | Falta `multiboot.mod` en el GRUB EFI | Instala `grub2-efi-*-modules` y copia `multiboot.mod` + `relocator.mod`, o desactiva Secure Boot |
| Menú GRUB, carga y pantalla negra | Estás en el caso §3 (sin framebuffer) | Mira el puerto serie: si hay texto, el kernel está vivo |
| Menú GRUB, carga, se reinicia solo | Triple fault / excepción temprana | Arranca en QEMU con `-d int,cpu_reset` y mira dónde |

Nota sobre QEMU: `qemu-system-i386 -kernel` usa su propio cargador Multiboot y
**no** reproduce nada de esto. Para probar el camino UEFI de verdad hace falta
`qemu-system-x86_64 -bios OVMF.fd -drive file=trinux.iso,format=raw`, o
`-bios OVMF` + una imagen con ESP.

## 6. Memoria

El kernel reserva **~36 MiB de `.bss`** (heap del kernel de 32 MiB + bitmap
del PMM de 128 KiB + page tables). El segmento RW tiene `MemSiz = 35.9 MiB`
— comprobado con `readelf -l`. Cualquier firmware que cargue el ELF sin
inicializar `.bss` a cero (o con menos RAM útil) dará basura en el heap. No es
la causa de tu caso, pero conviene saberlo: pide al menos 64 MiB.

---

## Lo que habría que cambiar para soportar UEFI de verdad

Por orden de coste/beneficio:

1. **CI**: instalar `grub-efi-ia32-bin` (no `amd64`), verificar que la ISO
   contiene `EFI/BOOT/BOOTIA32.EFI` y **fallar el build si no está**. Es un
   `ls` dentro de la ISO con `xorriso`. Con eso la ISO pasaría a ser
   híbrida de verdad.
2. **Imagen USB**: crear una tabla **GPT + ESP FAT32** real (con
   `sgdisk`/`mkfs.vfat`/`grub-install --target=i386-efi`) en lugar de
   `truncate`. La zona de persistencia del kernel tendría que moverse a una
   partición propia después de la ESP.
3. **Consola**: hacer `fb_init()` tolerante a `bpp=24` y a `BGR`, y añadir un
   *stub* de consola serie como salida por defecto cuando no haya
   framebuffer, para no dejar al usuario con una pantalla negra.
4. **Documentación**: el README dice "bootea en UEFI ✅" y "Mac Intel ✅ (vía
   partición HFS+)" sin que nada de eso esté implementado. O se implementa o
   se corrige el texto.

Ninguno de los cuatro requiere tocar el kernel: el kernel de 32 bits sigue
siendo válido en UEFI siempre que GRUB (o un cargador propio) lo prepare.
