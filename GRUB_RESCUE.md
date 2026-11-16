# `grub rescue>` al arrancar el USB de Trinux

> **Nota:** la imagen que genera `make usb` a partir de esta versión ya no
> debería llegar aquí: usa `grub-mkstandalone`, que lleva el menú y los módulos
> **dentro** del `.EFI`, así que GRUB no tiene que buscar su `/boot/grub` en el
> disco. Este documento sigue siendo útil para imágenes antiguas o para el
> camino BIOS. Estado actual del soporte UEFI: `DIAGNOSTICO_UEFI.md`.

## Qué significa

`grub rescue>` **no es un fallo del kernel**. GRUB arrancó: el sector 0 del
medio tiene código válido y la BIOS/UEFI lo ejecutó. Lo que falló es el
**segundo paso**: GRUB no encontró su propio directorio `/boot/grub` (o los
módulos `.mod` dentro de él), así que no puede mostrar el menú ni ejecutar
`multiboot`.

Es decir: el kernel de Trinux ni siquiera llegó a intentarse cargar.

Diferencia importante entre los tres mensajes que se confunden:

| Mensaje | Qué pasó | Estado |
|---|---|---|
| `no bootable device` / pantalla negra | La BIOS no ejecutó nada | El MBR no vale |
| `grub rescue>` | GRUB ejecutó, pero no encontró `/boot/grub` | **Tu caso** |
| Menú GRUB, `can't find command 'multiboot'` | GRUB va bien, le falta el módulo | Otro problema distinto |
| Menú GRUB, carga, pantalla negra | El kernel corre sin salida de vídeo | Otro más |

---

## Arreglo inmediato desde el propio `grub rescue>`

No hace falta reinstalar nada. Son cuatro comandos.

```
grub rescue> ls
```

Verás algo como `(hd0) (hd0,msdos1)` o `(hd0,gpt1)`. Busca dónde está el
`grub.cfg`:

```
grub rescue> ls (hd0,msdos1)/boot/grub
grub rescue> ls (hd0)/boot/grub
```

Prueba las combinaciones hasta que uno **liste el contenido** en vez de dar
`unknown filesystem` o `file not found`. Cuando lo tengas:

```
grub rescue> set root=(hd0,msdos1)
grub rescue> set prefix=(hd0,msdos1)/boot/grub
grub rescue> insmod normal
grub rescue> normal
```

Y ya deberías ver el menú de Trinux.

Si `insmod normal` dice `file not found`, ese `prefix` no es el bueno: sigue
probando particiones. Si dice `unknown filesystem`, es que el medio está
grabado de forma corrupta (ver más abajo, `dd` con `conv=fsync`).

> Atajo si `ls` muestra `(cd0)`: prueba `set prefix=(cd0)/boot/grub`. El ISO
> de GRUB se identifica como unidad de CD aunque esté en un pendrive.

---

## Por qué pasa

Hay tres causas reales, y en tu caso la más probable es la primera:

### 1. El workflow no usa `make-usb-image.sh` (la más probable)

El paso "Generar imagen USB" de `.github/workflows/build.yml` hace:

```bash
cp trinux.iso trinux-usb-512.img
truncate -s 512M trinux-usb-512.img
```

Eso es una **copia del ISO seguida de 512 MiB de ceros pegados al final**. El
ISO sigue siendo un ISO: el GRUB que lleva dentro está configurado para
buscar `/boot/grub` en el propio ISO (unidad `(cd0)`), y **eso no es lo mismo
que un pendrive**. Según cómo el firmware presente el medio —como unidad de
disco `(hd0)` o como CD `(cd0)`— GRUB acierta o no, y cuando no acierta
aterriza en `grub rescue>`.

**No uses el `.img` del CI para el pendrive.** Usa el script del repo, que
también añade la zona de persistencia de la misma forma:

```bash
make usb           # o:  ./make-usb-image.sh 512
sudo dd if=mykernel-usb.img of=/dev/sdX bs=4M status=progress conv=fsync
```

### 2. `grub-mkrescue` falló y nadie lo notó

Antes el script hacía `grub-mkrescue ... >/dev/null 2>&1`. Redirigir a
`/dev/null` **oculta el error**, y `grub-mkrescue` a veces devuelve 0 aunque
haya fallado un paso interno. Resultado: una imagen sin `grub.cfg` utilizable
→ `grub rescue>`. Ya está corregido: el script imprime la salida y aborta si
falta la firma `0x55AA`, el `grub.cfg` o el kernel.

### 3. Herramientas incompletas en el CI

`grub-mkrescue` solo produce un medio realmente arrancable si encuentra
`xorriso` y `mtools`. El workflow instala `grub-efi-amd64-bin`, cuando el
README pide `grub-efi-ia32-bin` (32 bits). En una máquina que arranque en UEFI
puro, la imagen del CI no tiene `EFI/BOOT/BOOT*.EFI` y **no puede** arrancar.

---

## Comprobar la imagen ANTES de grabarla

Esto te habría ahorrado el viaje:

```bash
# 1. Firma de arranque en el sector 0
dd if=mykernel-usb.img bs=1 skip=510 count=2 | od -An -tx1
#    debe imprimir:  55 aa

# 2. Que el grub.cfg va dentro
grep -a "multiboot /boot/mykernel.bin" mykernel-usb.img

# 3. Que el kernel va dentro y lleva cabecera Multiboot
grep -a $'\x02\xb0\xad\x1b' mykernel-usb.img

# 4. Probar sin gastar un pendrive (necesita OVMF para el camino UEFI)
qemu-system-i386 -drive file=mykernel-usb.img,format=raw,if=ide -m 512M -serial stdio
```

`make usb` hace las tres primeras por ti y **falla** si alguna no se cumple.
Del mismo modo, `make iso` ahora también verifica la ISO generada.

---

## Grabar bien el pendrive

```bash
lsblk                                    # identifica la unidad, p.ej. /dev/sdb
sudo umount /dev/sdb* 2>/dev/null
sudo dd if=mykernel-usb.img of=/dev/sdb bs=4M status=progress conv=fsync
sync
```

- A la **unidad** (`/dev/sdb`), nunca a una partición (`/dev/sdb1`).
- `conv=fsync` es importante: sin él, quitar el pendrive antes de que se
  vacíen las cachés deja el medio corrupto → `grub rescue>` o `unknown
  filesystem`.
- En Windows, usar **Rufus en modo "DD"**, no en modo ISO. Rufus en modo ISO
  reescribe la estructura y es una causa clásica de este error con imágenes
  de GRUB.

---

## Si sigue sin arrancar

Recoge estos datos y se puede acotar el problema:

1. ¿Qué firmware usas, BIOS/CSM o UEFI puro? ¿Secure Boot activado?
2. Con el pendrive puesto, en el `grub rescue>`, la salida exacta de `ls` y
   de `set`.
3. ¿La imagen la generaste con `make usb` o es el `.img` descargado de
   GitHub Actions? (Casi seguro es lo segundo — ver causa 1.)

Si el medio arranca pero la pantalla se queda negra, eso ya es otro problema
distinto: consola VGA vs framebuffer en UEFI. Está explicado en
`DIAGNOSTICO_UEFI.md`.
