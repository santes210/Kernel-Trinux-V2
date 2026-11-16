# examples/

Programas de ejemplo para **userland dentro de Trinux**, no para el kernel.
No se compilan con el kernel (`Makefile` solo recoge `boot/ kernel/ drivers/
cpu/ mm/ fs/ process/ shell/ auth/ lib/ user/`), así que antes se quedaban en
la raíz del repo y cualquier `gcc *.c` o script de CI tropezaba con ellos:

* `mini_test.c` — hola mundo mínimo (le falta el `;` a propósito: es un
  ejemplo de error para el compilador `tcc` in-kernel).
* `slow.c` — duerme 4 s para ver la prioridad en `ps`/`top`.
* `tce.c` — editor de texto a pantalla completa (Trinux Code Editor);
  se compila con `tcc tce.c` **dentro** del sistema.

Se compilan con el toolchain de `user/trinux.h` (ring 3), no con el del
kernel:

    tcc tce.c      # desde la shell de Trinux
