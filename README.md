# talk-os

A minimal x86_64 operating system that boots into 64-bit long mode and prints
"Hello, World!" to the screen. Written in C and assembly, built and run on macOS.

## How it works

1. **`boot.asm`** contains a [Multiboot v1](https://www.gnu.org/software/grub/manual/multiboot/multiboot.html)
   header. QEMU's built-in loader recognises it and starts the kernel in 32-bit
   protected mode — no GRUB or disk image needed.
2. The 32-bit entry code checks that the CPU supports long mode, identity-maps
   the first 1 GiB of memory with 2 MiB pages, enables PAE + paging + long mode,
   loads a 64-bit GDT, and far-jumps into 64-bit code.
3. The 64-bit stub calls **`kernel_main`** in **`kernel.c`**, which writes
   directly to the VGA text buffer at physical address `0xB8000`.

## Requirements (macOS)

Install the toolchain with Homebrew:

```sh
make toolchain      # brew install nasm x86_64-elf-gcc qemu
```

`x86_64-elf-gcc` is an ELF cross-compiler; the system clang/ld on macOS produce
Mach-O binaries, which can't be used for a bare-metal ELF kernel.

## Build & run

```sh
make            # produces kernel.bin
make run        # boots kernel.bin in QEMU
```

A QEMU window opens showing the greeting. Close the window (or `Ctrl-C` the
terminal) to quit.

## Files

| File         | Purpose                                            |
|--------------|----------------------------------------------------|
| `boot.asm`   | Multiboot header, 32-bit setup, long-mode trampoline |
| `kernel.c`   | 64-bit kernel entry; writes to VGA text buffer     |
| `linker.ld`  | Places the kernel at 1 MiB, header first           |
| `Makefile`   | Build, run, and toolchain targets                  |
