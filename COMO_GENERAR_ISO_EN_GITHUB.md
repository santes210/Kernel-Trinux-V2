# Cómo hacer que GitHub Actions genere el ISO actualizado

El código ya está en `master` (`85104d4`): `make-usb-image.sh` nuevo,
reserva de 48 MiB en `blockfs`/`diskfs`, `Makefile` y toda la documentación.

Lo único que **falta** es `.github/workflows/build.yml`, porque la GitHub App
de este entorno no tiene permiso `workflows` y el push se rechaza entero. Hay
que reemplazarlo a mano una vez. Son 3 minutos.

> El `build.yml` que hay ahora en `master` **no es YAML válido** (el heredoc del
> `grub.cfg` está sin indentar) y además hace `cp trinux.iso` + `truncate`. Es
> decir: GitHub no lo ejecuta, y aunque lo ejecutara no generaría UEFI.
> Por eso hay que reemplazarlo entero, no "editarlo".

---

## Paso 1 — Abrir el archivo en GitHub

Ve a:

```
https://github.com/santes210/Kernel-Trinux-V2/blob/master/docs/github-actions/build.yml
```

Ese archivo es la **copia buena y verificada** (YAML válido, 12 pasos, usa
`make-usb-image.sh`).

Pulsa el botón del **lápiz** (Edit this file) arriba a la derecha.

---

## Paso 2 — Crear el archivo en la ubicación correcta

GitHub no te deja "mover" tan fácil. Lo más rápido:

1. En esa misma página del lápiz, arriba del editor, **cambia el nombre** en el
   campo del path: borra `docs/github-actions/` y deja
   `.github/workflows/build.yml`.

   Es decir, el campo debe quedar exactamente así:

   ```
   Kernel-Trinux-V2/.github/workflows/build.yml
   ```

   (GitHub te dará un aviso de que estás "moving the file". Es lo que
   queremos: reemplaza el `build.yml` viejo que está en esa ruta.)

2. Baja hasta abajo.

3. En **"Commit changes"** deja el mensaje por defecto o escribe:

   ```
   ci: build.yml válido + ISO/imagen USB híbrida con UEFI
   ```

4. Elige **"Commit directly to the `master` branch"**.

5. **Commit changes**.

---

## Alternativa más cómoda (pegar el contenido)

Si prefieres no mover archivos:

1. Ve a
   `https://github.com/santes210/Kernel-Trinux-V2/blob/master/.github/workflows/build.yml`
2. Lápiz (Edit).
3. Selecciona **todo** el contenido (`Ctrl+A`) y bórralo.
4. Abre en otra pestaña
   `https://raw.githubusercontent.com/santes210/Kernel-Trinux-V2/master/docs/github-actions/build.yml`
5. `Ctrl+A`, `Ctrl+C`, y pégalo en el editor.
6. Commit a `master`.

Resultado idéntico. Elige la que te resulte menos confusa.

---

## Paso 3 — Lanzar la generación

Al commitear en `master`, el workflow se dispara **automáticamente** (su
trigger es `push` a `master`).

Si quieres lanzarlo a mano o con otro tamaño de USB:

1. Pestaña **Actions** del repo.
2. En la izquierda, **"Trinux — Build & Release"**.
3. Botón **"Run workflow"** (a la derecha) → rama `master`.
4. Opcionalmente ajusta:
   - **usb_size_mb**: por defecto `512`. Para un USB de 8 GB pon `8192`.
   - **create_release**: déjalo en `false`.
5. **Run workflow**.

---

## Paso 4 — Esperar y descargar

El build tarda unos **3–6 minutos**. Al terminar:

1. Entra en la ejecución (la más reciente, con check verde).
2. Abajo, en **Artifacts**, descarga **`trinux-artifacts`** (o el nombre que
   muestre; es el que contiene los ficheros).

Dentro encontrarás:

| Fichero | Qué es | Para qué |
|---|---|---|
| `trinux-usb-512.img` | imagen de disco, BIOS **+** UEFI | **este es el que grabas al pendrive** |
| `trinux.iso` | el mismo contenido como ISO | para grabarlo como CD o con Rufus en modo DD |
| `mykernel.bin` | el kernel suelto | solo para QEMU `-kernel` |
| `checksums.txt` | SHA-256 | verificar la descarga |

> **Cuidado con la extensión**: si GitHub te lo baja como `.zip`, descomprímelo
> antes. `dd` necesita el `.img`, no un `.zip`.

---

## Paso 5 — Grabar y arrancar

```bash
# 1. Verifica QUÉ unidad es (no adivines)
lsblk

# 2. Desmonta si está montada
sudo umount /dev/sdX* 2>/dev/null

# 3. Graba a la UNIDAD, nunca a una partición
sudo dd if=trinux-usb-512.img of=/dev/sdX bs=4M status=progress conv=fsync
sync
```

En la máquina nueva:

1. Entra en la UEFI (F2 / F10 / Supr / Esc al encender).
2. **Desactiva Secure Boot** → `Disabled`. Sin esto no arranca: el módulo
   `multiboot` de GRUB no está firmado (CVE-2015-5281) y el firmware lo bloquea.
3. Selecciona el pendrive en el orden de arranque (o F12 al encender).
4. Deberías ver el menú GRUB con 3 entradas: normal, single-user y shell ring-0.

---

## Cómo saber que el workflow quedó bien

Antes de lanzarlo, en la página del archivo en GitHub, arriba debería poner
**"Valid"** o no mostrar errores. Si GitHub te avisa **"Invalid workflow file"**,
es que el pegado se cortó: vuelve a empezar desde el paso 1.

Señal de que todo fue bien en el log del build, dentro del paso
**"💿 Generar ISO + imagen USB (BIOS + UEFI) y verificarlas"**:

```
    BIOS : MBR 0x55AA + grub.cfg + kernel ......... OK
    UEFI : EFI/BOOT/BOOTX64.EFI (PE) + ESP FAT .... OK
    UEFI32: EFI/BOOT/BOOTIA32.EFI ................. OK
```

Si ves `❌ La ISO NO contiene EFI/BOOT/BOOTX64.EFI`, el build se detiene a
propósito en vez de publicarte un medio que no arranca. En ese caso pásame el
log y lo miro.

---

## Errores típicos y qué significan

| Lo que ves | Qué pasó | Qué hacer |
|---|---|---|
| Actions no muestra ningún workflow | El `build.yml` sigue inválido | Rehacer el paso 2 y comprobar el aviso "Invalid workflow file" |
| `❌ falta grub-mkstandalone` o `xorriso` | Falta una dependencia | Ya están en el `build.yml` bueno; confirma que pegaste el completo |
| `❌ La ISO NO contiene EFI/BOOT/BOOTX64.EFI` | El GRUB EFI no se generó | Pásame el log; suele ser `grub-efi-amd64-bin` ausente |
| El pendrive no arranca y no sale ni el menú | Secure Boot activo, o el firmware no ve el medio | Desactiva Secure Boot; prueba F12 para elegir el pendrive |
| Menú GRUB y `error: file not found` | No encontró el kernel | `ls` para ver las particiones; ver `GRUB_RESCUE.md` |
| Arranca pero pantalla negra | Consola VGA en UEFI | Mira el puerto serie; ver `DIAGNOSTICO_UEFI.md` |

---

## Si prefieres no depender de GitHub Actions

Puedes generar la imagen en tu propia máquina, con el código que ya está en
`master`:

```bash
git clone https://github.com/santes210/Kernel-Trinux-V2
cd Kernel-Trinux-V2

sudo apt install gcc-multilib nasm \
                 grub-pc-bin grub-efi-amd64-bin grub-efi-ia32-bin \
                 xorriso mtools dosfstools

make          # compila mykernel.bin
make usb      # genera y VERIFICA mykernel-usb.img
```

Es el mismo script que usa el CI, así que el resultado es el mismo. Ventaja:
ves los errores al instante y no dependes de mis permisos.
