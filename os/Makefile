# Build the talk-os bare-metal kernel on macOS.
#
#   make            build kernel.bin
#   make run        boot in QEMU (graphics window + serial on stdio)
#   make run-nox    boot headless (serial only)
#   make clean

ASM     := nasm
CC      := x86_64-elf-gcc
LD      := x86_64-elf-ld
OBJCOPY := x86_64-elf-objcopy

# Anthropic API key compiled into the kernel for direct HTTPS (optional).
# Build with `make KEY=sk-ant-...` to get real answers; without it the kernel
# still completes the TLS handshake and the API returns 401 (proves HTTPS).
KEY ?=

INCLUDES := -Iinclude -Iport -Ilwip/src/include -Imbedtls/include
CFLAGS  := -ffreestanding -m64 -mno-red-zone -mno-mmx -mno-sse -mno-sse2 \
           -fno-stack-protector -fno-pic -fno-builtin \
           -fno-tree-loop-distribute-patterns -Wall -Wextra -std=gnu11 \
           -DMBEDTLS_CONFIG_FILE='"mbedtls_config.h"' \
           -DTALKOS_API_KEY='"$(KEY)"' \
           $(INCLUDES) -c
LDFLAGS := -n -T linker.ld
# libgcc supplies compiler runtime helpers (e.g. __udivti3 for 128-bit division
# used by mbedTLS bignum) that aren't otherwise available freestanding.
LIBGCC  := $(shell $(CC) -print-libgcc-file-name)

# Kernel + drivers. Add a driver by dropping a .c here (or under drivers/).
KERNEL_SRCS := kernel/main.c kernel/drivers.c \
               mm/heap.c \
               lib/base.c lib/libc_shim.c \
               drivers/serial/serial.c drivers/pci/pci.c \
               drivers/input/kbd.c drivers/net/e1000.c net/net.c
KERNEL_OBJS := $(KERNEL_SRCS:.c=.o)

# Vendored lwIP (core + IPv4 + Ethernet + the altcp_tls mbedTLS port).
LWIP_SRCS := $(wildcard lwip/src/core/*.c) \
             $(wildcard lwip/src/core/ipv4/*.c) \
             lwip/src/netif/ethernet.c \
             lwip/src/apps/altcp_tls/altcp_tls_mbedtls.c \
             lwip/src/apps/altcp_tls/altcp_tls_mbedtls_mem.c
LWIP_OBJS := $(LWIP_SRCS:.c=.o)

# Vendored mbedTLS (whole library; disabled modules compile to ~nothing).
MBEDTLS_SRCS := $(wildcard mbedtls/library/*.c)
MBEDTLS_OBJS := $(MBEDTLS_SRCS:.c=.o)

OBJS    := boot/boot.o $(KERNEL_OBJS) $(LWIP_OBJS) $(MBEDTLS_OBJS)

# User-mode networking: the guest reaches the host (proxy.py) at 10.0.2.2.
NETFLAGS   := -netdev user,id=n0 -device e1000,netdev=n0

all: kernel.bin

boot/boot.o: boot/boot.asm
	$(ASM) -f elf64 $< -o $@

%.o: %.c
	$(CC) $(CFLAGS) $< -o $@

kernel.elf: $(OBJS) linker.ld
	$(LD) $(LDFLAGS) -o $@ $(OBJS) $(LIBGCC)

kernel.bin: kernel.elf
	$(OBJCOPY) -O elf32-i386 $< $@

# The console is VGA text mode (720x400). zoom-to-fit lets the window scale, so
# just drag it bigger (or use the green button) and the text scales up.
DISPLAY ?= cocoa,zoom-to-fit=on

run: kernel.bin
	qemu-system-x86_64 -kernel kernel.bin $(NETFLAGS) -display $(DISPLAY) -serial stdio

run-big: kernel.bin
	qemu-system-x86_64 -kernel kernel.bin $(NETFLAGS) -display cocoa,zoom-to-fit=on,full-screen=on -serial stdio

run-nox: kernel.bin
	qemu-system-x86_64 -kernel kernel.bin $(NETFLAGS) -display none -serial stdio

clean:
	rm -f boot/boot.o $(KERNEL_OBJS) $(LWIP_OBJS) $(MBEDTLS_OBJS) kernel.elf kernel.bin net.pcap

toolchain:
	brew install nasm x86_64-elf-gcc qemu

.PHONY: all run run-big run-nox clean toolchain
