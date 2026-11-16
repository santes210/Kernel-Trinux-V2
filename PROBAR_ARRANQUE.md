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
