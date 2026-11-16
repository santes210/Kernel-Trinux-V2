# Cómo saber qué está roto: GRUB o el kernel

Este documento existe porque el síntoma que viste —la palabra `GRUB ` y el cursor
parpadeando, sin menú, sin error y sin nada por el puerto serie— **no distingue**
entre dos fallos muy distintos:

| Qué ves | Qué significa |
|---|---|
| `GRUB ` + cursor, y el teclado no hace nada | GRUB cargó su primera etapa pero **no pudo continuar** (módulos, `prefix` o `normal`) |
| Menú de GRUB pero al elegir se queda negro | GRUB está **sano**; el roto es el kernel o el handoff Multiboot |
| Menú y todo funciona | Resuelto |

Las tres pruebas de abajo están ordenadas de menos a más trabajo. Con la **Prueba 1**
basta para decidir qué arreglar después.

---

## Prueba 1 — QEMU en Windows (5 minutos, sin flashear nada)

No necesitas el USB ni tocar la BIOS del portátil. Descarga el artefacto
`trinus-usb-512.img` desde la pestaña **Actions** del repositorio (último run verde
de *Build Trinux*) y, en la carpeta donde lo tengas:

```powershell
qemu-system-i386 -nographic -drive file=trinus-usb-512.img,format=raw,if=ide -m 512M
```

Para salir: `Ctrl-A`, luego `X`.

**Qué mirar.** `-nographic` manda la pantalla de la BIOS, el menú de GRUB y la
salida del kernel **a la misma ventana de texto**. Es la única forma de verlos
juntos; con `-serial stdio` la ventana de vídeo de QEMU se queda en negro y parece
colgada aunque el kernel esté corriendo.

- **Se ve el menú de GRUB** → GRUB está bien. El arreglo de `grub-mkrescue`
  funcionó y lo que falla es el kernel. Salta a la Prueba 3.
- **Se ve `GRUB ` y nada más** → GRUB sigue roto. Mándame la captura **y** el
  nombre exacto de la carpeta del artefacto (por ejemplo
  `trinus-usb-unknown-f3a5bd4`): el run que probaste tiene que ser el de este commit.
- **Sale `Booting from Hard Disk...` y vuelve a la BIOS** → el MBR híbrido no se
  está leyendo. Casi siempre es el `.img` descomprimido de un ZIP corrupto:
  vuelve a descargarlo.

---

## Prueba 1b — El mismo test, pero por **UEFI** (el que usa tu portátil)

La Prueba 1 arranca por BIOS (`SeaBIOS`). Tu portátil es UEFI-only, así que hay
que probar ese camino por separado. **No hace falta descargar nada**: QEMU para
Windows ya trae el firmware, pero se llama distinto de lo que dicen casi todas
las guías.

En `C:\Program Files\qemu\share` los ficheros son:

| Fichero | Qué es |
|---|---|
| `edk2-x86_64-code.fd` | **OVMF de 64 bits — el que quieres** |
| `edk2-i386-code.fd` | OVMF de 32 bits (plan B) |
| `edk2-x86_64-secure-code.fd` | ⚠️ variante **Secure Boot** — NO usar |

No existe `vars.fd` para x86_64 en este paquete; con `-bios` no lo necesitas
(las variables UEFI no persisten entre reinicios, y aquí solo probamos un
arranque).

```powershell
cd "C:\Program Files\qemu"
qemu-system-x86_64 -nographic -bios "share\edk2-x86_64-code.fd" ^
  -drive file="C:\Users\<TU_USUARIO>\Downloads\trinus-v0.3.0-45-g4a7d6f0\trinus-usb-512.img",format=raw,if=ide -m 512M
```

**Qué mirar para saber que es UEFI de verdad:** ya no aparece `SeaBIOS` ni
`Booting from Hard Disk...`. En su lugar sale la salida de `BdsDxe` / edk2, y
luego el mismo menú de GRUB 2.06 y el mismo `root@trinux:/#`.

Si sale `qemu: could not load PC BIOS`, la ruta está mal: comprueba con
`dir "C:\Program Files\qemu\share\edk2*.fd"`.

---

## Prueba 2 — El menú de diagnóstico (solo si la Prueba 1 es ambigua)

En el menú de GRUB hay una entrada llamada:

> **Trinux — Diagnostico de GRUB (no arranca el kernel)**

No ejecuta `multiboot`. Imprime `root`, `prefix`, la lista de dispositivos, el
contenido de `/boot` y los primeros bytes del kernel. Si esa entrada dibuja en
pantalla, GRUB funciona con seguridad y el problema está en el kernel.

---

## Prueba 3 — Arranque real en el portátil

Solo tiene sentido si la Prueba 1 ya muestra el menú.

1. **Desactiva Secure Boot** en la UEFI. Sin esto no hay arranque: el módulo
   `multiboot` de GRUB no está firmado y el firmware lo rechaza en silencio.
2. Flashea `trinus-usb-512.img` con **Rufus en modo DD** (nunca modo ISO: rompe
   el MBR híbrido y acabas en `grub rescue>`). Alternativas: balenaEtcher o
   Win32DiskImager.
3. Arranca desde el USB con el menú de arranque del equipo (F12/F9/Esc según marca).

---

## Qué cambió en este intento

Los tres intentos anteriores montaban el arranque BIOS a mano y los tres daban el
mismo síntoma. Ahora `make-usb-image.sh` delega en **`grub-mkrescue`**, que genera
de una sola vez el El Torito BIOS y la partición ESP UEFI:

```sh
grub-mkrescue --output=trinus-usb-512.img iso/
```

Lo comprobé en el código fuente de GRUB (`util/grub-mkrescue.c`): su `load_cfg`
embebido solo hace `search --fs-uuid` + `set prefix=(${root})/boot/grub`, y deja
todos los `.mod` en `/boot/grub/i386-pc/` dentro del ISO. Es exactamente lo que
faltaba antes: los módulos **existen** y el `prefix` **apunta donde están**.

El script bajó de ~600 a 328 líneas y ya no hace `dd` ni `mkfs.vfat` a mano.
