# Auditoría y plan de mejoras — Trinux v0.3.x

> Revisión integral del kernel (rama `arena/562edde7-kernel-trinux-v2`,
> commit base `607f6df`). El análisis cubre corrección, arquitectura,
> rendimiento, seguridad, robustez e ingeniería. Cada hallazgo incluye la
> ubicación exacta en el código y una propuesta accionable.

**Resumen de una línea:** Trinux es un kernel hobby notablemente completo
(ring 3, VFS, disco, red básica, MLFQ, usuarios), pero su techo actual está
marcado por **aislamiento de procesos simulado**, **estructuras de datos
lineales**, **E/S por polling/PIO sin caché** y un **SMP a medio construir**.

---

## 1. Bugs críticos (corregir antes de cualquier otra cosa)

| # | Bug | Ubicación | Impacto |
|---|-----|-----------|---------|
| C1 | *Stack buffer overflow* en `net_ping`: buffer `frame[64]`, pero se escribe cabecera ICMP en offset 34 + 8 bytes + 32 de payload = hasta el offset **74** (10 bytes fuera del buffer) | `drivers/net.c:332-360` | Corrupción de pila del kernel al hacer `ping` |
| C2 | ~~Trampoline SMP declarado `trampoline_code[256]` con **309 inicializadores**~~ **RESUELTO v0.5.3**: inicializadores designados con offsets fijos, NOP de alineación y `_Static_assert` | `cpu/smp_boot.c` | Arranque de APs roto (latente: ver C3, sigue sin invocarse) |
| C3 | `smp_boot_aps()` **nunca se llama**: solo existe el `extern` en kernel.c. Toda la maquinaria SMP (~700 líneas) está muerta | `kernel/kernel.c:198` | Detección de cores sí, multi-core real no |
| C4 | **No hay aislamiento real entre procesos**: `vmm_create_address_space()` copia los *punteros físicos* de las mismas tablas de páginas (shallow copy). Todos los procesos comparten las páginas físicas de usuario (80 MiB–1 GiB mapeadas `PAGE_USER` en tablas compartidas) | `mm/vmm.c:197-216` | Cualquier proceso ring 3 puede leer/escribir la memoria de los demás |
| C5 | ELF loader con hack de respaldo/restauración de la región `0x08048000` y stacks en direcciones fijas por nivel de anidamiento (máx. 4 niveles, luego colisión) | `kernel/elf.c:30-57`, `96-130` | Correctividad frágil, imposible escalar a procesos concurrentes reales |
| C6 | `uaccess_ok()` valida solo **rangos**, no páginas presentes. Un puntero en rango pero desmapeado se toca en ring 0 → page fault en kernel → `panic()` (el handler solo perdona fallos con U/S=1) | `cpu/uaccess.h:33-43`, `mm/vmm.c:71-101` | Un proceso de usuario puede tumbar el kernel entero con un puntero inválido |
| C7 | `fork()` copia una región fija de **113 MiB** (`0x08048000–0x0F100000`, ~28 mil páginas) en cada llamada, haya páginas mapeadas o no; y además es síncrono | `mm/fork.c:23-25`, `82-95` | Lentitud enorme y desperdicio de RAM |
| C8 | 41 warnings de compilación, incluidos 8× `-Warray-bounds`, declaraciones implícitas (`tss_set_kernel_stack` en `elf.c:178`, `schedule` en `process.c:404`) y variable posiblemente sin inicializar (`editor.c:284`) | todo el árbol | Los implicit-declaration en `-O2` generan ABI incorrecto |

**Solución de C4/C5/C7 (la madre de todas las reformas):** tablas de páginas
privadas por proceso: copiar *estructura* (PD/PT) pero mapear frames físicos
nuevos para las regiones de usuario; COW (copy-on-write) para fork;
eliminar el hack de backup de `elf.c` y los stacks fijos por nivel.

---

## 2. Arquitectura: estructuras de datos O(n) que escalan mal

Todos los caminos calientes del kernel usan barrido lineal. Con unos pocos
objetos funciona; con carga real se degrada cuadráticamente.

### 2.1 Gestión de memoria

- **PMM — bitmap con scan lineal** (`mm/pmm.c:58-69`): cada `pmm_alloc_frame()`
  recorre desde el frame 0. Solución: *hint* del último frame asignado (casi
  O(1) inmediato) y, después, **buddy allocator** con free-lists por orden.
  Devolver 0 como OOM es ambiguo (0 es dirección física válida).
- **kheap — first-fit sobre lista enlazada** (`mm/kheap.c:62-78`): O(n) por
  allocación, fragmentación. Evolución: *slab allocator* para objetos pequeños
  (los más frecuentes: `vfs_node_t`, descriptores, etc.) + free-lists
  segregados por tamaño. El arena es fijo de 32 MiB en `.bss` (no demand-paged)
  y `kmalloc_aligned` guarda el puntero original *dentro* del payload
  (`kheap.c:103-106`), frágil.
- Sin **caché de páginas** ni page daemon: no hay presión de memoria, ni
  swapping, ni estadísticas más allá del heap.

### 2.2 Sistema de archivos

- **VFS con arrays estáticos** (`fs/vfs.h:24`): `children[256]` = 1 KiB por
  nodo; `finddir` y resolución de paths son O(n) por componente; borrar hace
  `memmove` de todo el array (`fs/ramfs.c:205-219`). Solución: lista enlazada
  + **tabla hash de nombres (dcache)** por directorio, e inodo con número único.
- **blockfs — bitmap lineal en disco** (`fs/blockfs.c:105-114`): mismo patrón
  que el PMM.
- **No existe buffer/page cache**: cada lectura de un archivo respaldado en
  disco baja a ATA/AHCI por PIO/polling. Una caché LRU de bloques de 4 KiB con
  write-back daría el mayor salto de rendimiento perceptible del sistema.
- **diskfs serializa el árbol completo en cada `sync`** (`fs/diskfs.c:3,121-156`):
  O(tamaño total del FS). El siguiente nivel es journaling o, al menos,
  escritura incremental de nodos modificados.
- Código duplicado/muerto: `fs/fat.c` (solo lectura, **nunca se llama**) vs
  `fs/fat16.c` (el real), más el backup `fs/fat.c.orig`.

### 2.3 Procesos y planificación

- Tabla de procesos estática de 128 entradas con búsquedas lineales en todas
  partes (`process_get`, `process_at`, waitpid, reparto de huérfanos…).
- El planificador MLFQ está bien diseñado, pero es **una sola cola global**
  barrida en cada tick (`process/scheduler.c:228-247`); con SMP real harían
  falta runqueues por CPU con robo de trabajo (*work stealing*).
- Señales limitadas: un solo entero `signal_pending` — no se pueden encolar
  varias señales, y la entrega se difiere al borde de syscall en vez de usar
  un *signal trampoline* real sobre el stack de usuario.

### 2.4 Syscalls

- ~~`switch` gigante de **69 casos**: O(n) por syscall~~ **Actualización
  v0.5.3**: verificado en el ensamblador que GCC -O2 ya compila el switch
  denso 0..73 como **jump table indexada** (`jmp *table(,%eax,4)`), por lo que
  el dispatch ya es O(1); el número mágico se reemplazó por `SYS_MAX`. Una
  tabla de punteros a función manual sería hoy una refactorización cosmética.
- Mecanismo `int 0x80`: en x86 de 32 bits, **SYSENTER/SYSEXIT** reduce el
  coste de entrada/salida de varios cientos de ciclos a decenas (pendiente).

---

## 3. E/S: el mayor cuello de botella de rendimiento

| Subsistema | Actual | Mejora |
|-----------|--------|--------|
| ATA | PIO **polling** por sondeo de registro de estado (`drivers/ata.c:73-90`) | IRQ-driven + DMA (bus mastering PCI) |
| AHCI | polling, un solo slot de comando, un solo puerto (`drivers/ahci.c:18`) | Cola de comandos NCQ, interrupciones, múltiples puertos |
| xHCI | polling (`drivers/xhci.c`) | interrupciones MSI/X, transferencias asíncronas |
| RTL8139 | polling a pesar de habilitar IRQs (`drivers/rtl8139.c:175,212`) | RX/TX por interrupción con descriptores |
| Timer | PIT a 100 Hz únicamente (`drivers/timer.c`) | TSC como clocksource de alta resolución + LAPIC timer por CPU (requisito para SMP); HPET opcional |
| Teclado | buffer circular, correcto | Mantener; considerar tipado con scancode set 2 completo |

Patrón transversal: falta una **capa de bloque** intermedia con peticiones
asíncronas, cola y caché, en vez de que cada FS hable directo con el hardware.

---

## 4. Seguridad y robustez

1. **Sin primitivas de sincronización en todo el kernel** (cero apariciones de
   spinlock/mutex/secciones `cli`-`sti` en C). Aunque el SMP está dormido, ya
   existen carreras entre contexto IRQ (teclado/serie/timer) y el flujo
   principal. Introducir `spin_lock_irqsave`/`spin_unlock_irqrestore` es
   prerrequisito para activar los APs y blindar kmalloc, PMM, VFS y tablas.
2. **Passwords con SHA-256 sin sal** (`auth/users.c:81-84`): hash rápido,
   dos usuarios con la misma contraseña producen el mismo hash. Añadir sal
   aleatoria por usuario (ya hay `/dev/random`) y un KDF con iteraciones
   (PBKDF2-style sobre el SHA-256 existente).
3. **Sin W^X / NX**: en 32 bits sin PAE no existe bit de no-ejecución; las
   páginas de usuario son ejecutables y escribibles. La migración a long mode
   (o PAE) lo habilita, junto con SMEP/SMAP.
4. **Sin ASLR**: ELF fijo en `0x08048000`, stacks fijos. Con tablas privadas
   se puede randomizar base del programa, stack y mmap.
5. `-fno-stack-protector` y 32 llamadas `strcpy/strcat/sprintf` sin cota.
   Reactivar *stack protector* (hay que proveer `__stack_chk_fail`) y migrar a
   `strncpy`/`snprintf`.
6. `panic()` mínimo (`kernel/panic.c`): sin volcado de registros, sin
   stack trace ni símbolos. Un "oops" con backtrace (frame pointers o tabla de
   símbolos) aceleraría toda depuración futura.
7. Validación del ELF loader: no se verifican límites de `p_memsz`, solapamiento
   de segmentos ni que las direcciones caigan en la región de usuario.

---

## 5. Higiene de repositorio y engineering

- **Git**: hay **34 `.o` versionados** y `trinux.iso` (21 MiB) pese al
  `.gitignore` (se añadieron antes de ignorarlos). `git rm --cached` reduce el
  clon y elimina binarios obsoletos que enmascaran builds sucios.
- **Código muerto / duplicado**: `fs/fat.c` + `fs/fat.c.orig`,
  `kernel/{snake,vgademo,hello}_elf.h` (~180 KiB de blobs sin referencias),
  `cpu/smp_boot.c` (hasta arreglar C2/C3), `tce.c`/`mini_test.c`/`slow.c`
  sueltos en la raíz (duplican contenido), 7 archivos `.patch` y
  `FASE5_REGEN.sh` (51 KiB) que son histórico, no código vivo.
- **Deriva de versión**: `KERNEL_VERSION "0.2.0"` en `include/kernel.h:8`
  mientras el README anuncia v0.3.2.
- **Makefile sin dependencias de headers** (no hay `-MMD -MP` + `-include`):
  cambiar un `.h` no reconstruye lo afectado → builds corruptos. Tampoco hay
  variante debug (`-g`) ni `-Werror`.
- **CI solo compila** (`.github/workflows/ci.yml`): falta (a) `make` con
  `-Werror`, (b) smoke test de arranque en QEMU con assert por serial
  (el kernel ya escribe `[boot] kernel_main entered`), (c) análisis estático
  (`cppcheck`/`scan-build`), (d) test de regresión de comandos de la shell.
- Sin tests unitarios en el árbol; toda la validación es manual con QEMU.
- **Hallazgo v0.5.3**: `build.yml` estaba roto en master —el heredoc de
  `grub.cfg` sin indentar invalidaba el YAML y todos los runs fallaban en 0 s
  sin crear jobs— además de incluir el paquete inexistente `truncate`. El fix
  existe localmente pero el token de CI no tiene permiso `workflows` para
  subirlo; debe aplicarlo un mantenedor. `ci.yml` sí funciona y valida el PR.

---

## 6. Plan por fases

### Fase 0 — Quick wins (bajo riesgo, alto valor) — ✅ COMPLETADA (v0.5.3)

- [x] Arreglar el overflow de `net_ping` (buffer de 128 bytes) (C1)
- [x] Cero warnings + `-Werror` en el Makefile/CI; `tss_set_kernel_stack` a
      `boot/gdt.h`, `schedule()` vía `scheduler.h` (C8: de 41 warnings a 0)
- [x] `memcpy`/`memset` con `rep movsl`/`rep stosl` (`lib/string.c`), verificado
      con pruebas host freestanding (16 casos string/PMM, 14 de auth)
- [x] PMM con cursor de asignación (scan amortiguado O(1) + pasada de rescate)
- [x] Dispatch O(1): verificado que GCC ya emite jump table para el switch
      denso 0..73 (`jmp *table(,%eax,4)`); se formalizó `SYS_MAX` en vez de
      una refactorización manual de 1300 líneas sin beneficio adicional
- [x] `git rm --cached` de 34 `.o` y la ISO de 21 MiB; eliminados
      `fs/fat.c`/`fat.h`/`fat.c.orig` y los blobs ELF muertos (snake/vgademo/hello)
- [x] `-MMD -MP` en el Makefile (deps `.d`, limpieza en `make clean`) y
      `KERNEL_VERSION` sincronizado (0.2.0 → 0.5.2)
- [x] Sal aleatoria de 8 bytes + 4096 iteraciones PBKDF2-style en
      `/etc/shadow` (`$sal$hash`), con migración automática de registros legacy
      y corrección del truncamiento por `USER_PASS_MAX=32` (ahora 96)
- [x] Extra: trampoline SMP reescrito con inicializadores designados (C2),
      con NOP de alineación que corrige el desfase del `jmp far` y
      `_Static_assert` de tamaño de 256 bytes

### Fase 1 — Base sólida (prerrequisitos para todo lo grande)

- [ ] Spinlocks con `irqsave`; proteger PMM, kheap, VFS, tablas de procesos
- [ ] `copy_from_user`/`copy_to_user` con validación por tabla de páginas y
      *fault fixup* (elimina el pánico por puntero inválido, C6)
- [ ] Tablas de páginas **privadas por proceso** + stacks de usuario dinámicos;
      eliminar backup/restore de `elf.c` (C4, C5)
- [ ] Slab allocator para objetos pequeños + buddy en PMM
- [ ] Caché LRU de bloques (page cache) bajo el VFS
- [ ] Señales con bitmap y entrega por trampoline real

### Fase 2 — E/S moderna y multi-core

- [ ] Capa de bloque asíncrona; ATA/AHCI/RTL8139 por interrupciones (DMA)
- [ ] Arreglar trampoline SMP (array dimensionado correctamente), invocar
      `smp_boot_aps()`, per-CPU data, LAPIC timer e IPI para *TLB shootdown*
- [ ] Runqueues por CPU con work stealing; COW fork y demand paging
- [ ] SYSENTER/SYSEXIT; TSC de alta resolución
- [ ] Journaling básico o FAT32 con LFN; eliminación de FAT16 legacy

### Fase 3 — El salto a "sistema extremadamente bueno": x86_64

La infraestructura ya está sembrada (`boot/longmode.asm`, `longmode_detect.c`):

1. Toolchain x86_64-elf y `linker.ld` ELF64; entrada en long mode (PML4).
2. IDT de 64 bits con IST; **SYSCALL/SYSRET** (MSR STAR/LSTAR).
3. Paginación de 4 niveles: RAM >4 GiB, **NX (W^X)**, SMEP/SMAP, KASLR.
4. SMP completo con esquema de locking ya probado en Fase 1-2.
5. Red TCP/UDP con sockets POSIX (hoy hay Ethernet/ARP/IP/ICMP), y userspace
   con ELF ET_DYN/PIE.

---

## 7. Matriz prioridad / esfuerzo

| Mejora | Prioridad | Esfuerzo | Beneficio |
|--------|-----------|----------|-----------|
| Fix overflow `net_ping` | 🔴 Crítica | XS | Elimina corrupción |
| Cero warnings + `-Werror` + deps en Makefile | 🔴 Alta | XS | Fiabilidad de builds |
| Limpieza de git/código muerto | 🟠 Media | XS | Mantenibilidad |
| memcpy/memset optimizados | 🟠 Media | XS | Rendimiento inmediato |
| Cursor PMM + tabla de syscalls | 🟠 Media | S | Rendimiento O(1) |
| Spinlocks + copy_from_user seguro | 🔴 Alta | M | Robidez, base SMP |
| Espacios de direcciones privados + COW | 🔴 Alta | L | Aislamiento real |
| Page cache + E/S por interrupciones | 🔴 Alta | L | Ordenes de magnitud en I/O |
| SMP funcional | 🟠 Media | L | Multi-core real |
| Migración x86_64 | 🟢 Estratégica | XL | NX, >4 GiB, syscalls modernos |
