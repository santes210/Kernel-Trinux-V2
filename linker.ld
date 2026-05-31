/* linker.ld  -  link the kernel at 1 MiB, multiboot header first. */
ENTRY(_start)

SECTIONS
{
    . = 1M;                      /* kernel loaded at physical 0x100000 */

    kernel_start = .;

    .multiboot : ALIGN(4)
    {
        *(.multiboot)           /* must be within first 8 KiB for GRUB */
    }

    .text : ALIGN(4K)
    {
        *(.text*)
    }

    .rodata : ALIGN(4K)
    {
        *(.rodata*)
    }

    .data : ALIGN(4K)
    {
        *(.data*)
    }

    .bss : ALIGN(4K)
    {
        *(COMMON)
        *(.bss*)
    }

    kernel_end = .;
}
