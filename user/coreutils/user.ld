/* Linker script para programas Trinux ring-3.
 * Carga en 0x08048000 (estándar i386 ELF), entry _start. */
ENTRY(_start)
SECTIONS {
    . = 0x08048000;
    .text : { *(.text*) }
    .rodata : { *(.rodata*) }
    .data : { *(.data*) }
    .bss : { *(.bss*) *(COMMON) }
    /DISCARD/ : { *(.note*) *(.comment) *(.eh_frame*) }
}
