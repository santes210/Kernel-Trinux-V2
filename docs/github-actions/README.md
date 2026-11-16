# Workflows de GitHub Actions

Esta carpeta es una **copia de referencia** de los workflows que viven en
`.github/workflows/`. Se guarda aquí para poderlos leer desde la ISO / disco
sin acceso a la web del repo (y porque el README los menciona).

> ⚠️ Si cambias un workflow, cámbialo en `.github/workflows/` **y** cópialo
> aquí. El de verdad es el de `.github/workflows/`.

| Workflow | Cuándo | Qué hace |
|---|---|---|
| `ci.yml` | PR a `master`/`arena/**` | compila, **arranca en QEMU** (`scripts/smoke_test.sh`) y sube `boot-full.log` |
| `build.yml` | push a `master`/`arena/**`, tags `v*`, manual | compila + smoke test + ISO GRUB + imagen USB + checksums + Release opcional |
| `audit.yml` | PR, push, manual | auditoría estática: 74 syscalls con handler, headers embebidos sincronizados, sin `.o`/`.bin` versionados, sin llamadas implícitas, userland compila |

## Por qué un smoke test y no solo "compila"

Hasta la ronda 2, el CI solo comprobaba `stat -c%s mykernel.bin > 100 KiB`.
Eso deja pasar un kernel que enlaza pero hace panic al inicializar el
scheduler, que es exactamente el tipo de bug que apareció en la auditoría.
`scripts/smoke_test.sh` arranca el kernel de verdad:

```bash
qemu-system-i386 -kernel mykernel.bin -m 512M -display none \
                 -serial file:boot-serial.log -no-reboot -nic none
```

y exige ver en la salida serie la secuencia completa hasta
`[init] launching /bin/sh in RING 3`, sin `PANIC` ni `PAGE FAULT EN KERNEL`.
Si no hay QEMU instalado, el script avisa y termina en 0 (para no romper
entornos sin virtualización); en el CI **sí** está instalado y por tanto las
comprobaciones son obligatorias.

## Cómo probarlo en local

```bash
make                     # construye mykernel.bin
make run                 # QEMU con ventana (Ctrl-C para salir)
bash scripts/smoke_test.sh   # versión "CI": headless + comprobaciones
```
