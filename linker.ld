/* Link the kernel at the 1 MiB mark — the conventional load address that keeps
 * us clear of low-memory BIOS/VGA regions. The multiboot header must come
 * first so QEMU's loader finds it near the start of the file. */

ENTRY(start)

SECTIONS {
    . = 1M;

    .boot :
    {
        /* multiboot header must be early in the image */
        *(.multiboot)
    }

    .text :
    {
        *(.text)
    }

    .rodata :
    {
        *(.rodata)
    }

    .data :
    {
        *(.data)
    }

    .bss :
    {
        *(.bss)
    }
}
