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

    /* Self-registering driver table. KEEP() stops --gc-sections dropping the
     * entries; __start_/__stop_ are provided automatically by the linker. */
    .driver_table :
    {
        __start_driver_table = .;
        KEEP(*(driver_table))
        __stop_driver_table = .;
    }

    .data :
    {
        *(.data)
    }

    .bss :
    {
        __bss_start = .;
        *(COMMON)
        *(.bss)
        __bss_end = .;
    }
}
