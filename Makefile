# Build a tiny x86_64 "hello world" kernel on macOS.
#
# Requires the Homebrew packages installed by `make toolchain`:
#   nasm, x86_64-elf-gcc (ELF cross-compiler), qemu
#
# Usage:
#   make            # build kernel.bin
#   make run        # build and boot it in QEMU
#   make clean

CC      := x86_64-elf-gcc
LD      := x86_64-elf-ld
OBJCOPY := x86_64-elf-objcopy
ASM     := nasm

# Freestanding 64-bit code: no host libc, no red zone (interrupts), no SIMD,
# no stack protector, and no position-independent code.
CFLAGS  := -ffreestanding -m64 -mno-red-zone -mno-mmx -mno-sse -mno-sse2 \
           -fno-stack-protector -fno-pic -Wall -Wextra -c
LDFLAGS := -n -T linker.ld

OBJS    := boot.o kernel.o

all: kernel.bin

boot.o: boot.asm
	$(ASM) -f elf64 $< -o $@

kernel.o: kernel.c
	$(CC) $(CFLAGS) $< -o $@

# Link the 64-bit kernel, then repackage as a 32-bit ELF. The code (including
# the 64-bit instructions) is unchanged — only the ELF container becomes
# 32-bit, which is what QEMU's multiboot `-kernel` loader requires. Our entry
# point is 32-bit boot code, so this is exactly what the loader expects.
kernel.elf: $(OBJS) linker.ld
	$(LD) $(LDFLAGS) -o $@ $(OBJS)

kernel.bin: kernel.elf
	$(OBJCOPY) -O elf32-i386 $< $@

run: kernel.bin
	qemu-system-x86_64 -kernel kernel.bin

# Headless run (no GUI window) — handy over SSH or for screenshots.
run-nox: kernel.bin
	qemu-system-x86_64 -kernel kernel.bin -display none -vga std

toolchain:
	brew install nasm x86_64-elf-gcc qemu

clean:
	rm -f *.o kernel.elf kernel.bin

.PHONY: all run run-nox toolchain clean
