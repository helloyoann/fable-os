# Build the talk-os bare-metal kernel on macOS.
#
#   make            build kernel.bin
#   make run        boot in QEMU (graphics window + serial on stdio)
#   make run ZOOM=3 same, but a 3x window instead of the default 2x
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

# One-off diagnostic builds. Empty in every normal build; the only current use
# is the exception self-test, which is compiled out entirely without it:
#   make EXTRA_CFLAGS=-DTALKOS_FAULT_TEST=FAULT_INJECT_PF_WRITE
# See arch/x86_64/fault_inject.c.
EXTRA_CFLAGS ?=

INCLUDES := -Iinclude -Iport -Ilwip/src/include -Imbedtls/include
CFLAGS  := -ffreestanding -m64 -mno-red-zone -mno-mmx -mno-sse -mno-sse2 \
           -fno-stack-protector -fno-pic -fno-builtin \
           -fno-tree-loop-distribute-patterns -Wall -Wextra -std=gnu11 \
           -DMBEDTLS_CONFIG_FILE='"mbedtls_config.h"' \
           -DTALKOS_API_KEY='"$(KEY)"' \
           $(INCLUDES) $(EXTRA_CFLAGS) -MMD -MP -c

# A build that silently disagrees with its own flags is worse than one that
# fails. Two things used to be untracked, and both of them could make the kernel
# behave one way while printing that it behaves another:
#
#   headers   Editing include/mbedtls_config.h or port/lwipopts.h rebuilt
#             nothing, so the defines that switch certificate verification on
#             never reached the mbedTLS and lwIP objects that consume them.
#             -MMD -MP above emits a .d per object; they are included below.
#
#   flags     `make EXTRA_CFLAGS=-DTALKOS_VERIFY_CERTS` in an already-built tree
#             printed "Nothing to be done" and left the previous kernel in
#             place. Worse, recompiling ONE object without the flag afterwards
#             produced a kernel that verified nothing and still printed
#             "[tls] verified: chain trusted". Every object now depends on a
#             stamp holding the flag set it was built with, so changing KEY or
#             EXTRA_CFLAGS forces a full, honest rebuild.
BUILD_FLAGS := KEY=$(KEY) EXTRA_CFLAGS=$(EXTRA_CFLAGS)
.build-flags: .FORCE
	@if [ "$$(cat $@ 2>/dev/null)" != '$(BUILD_FLAGS)' ]; then \
	   printf '%s' '$(BUILD_FLAGS)' > $@; \
	 fi
.FORCE:
LDFLAGS := -n -T linker.ld
# libgcc supplies compiler runtime helpers (e.g. __udivti3 for 128-bit division
# used by mbedTLS bignum) that aren't otherwise available freestanding.
LIBGCC  := $(shell $(CC) -print-libgcc-file-name)

# Kernel + drivers. Add a driver by dropping a .c here (or under drivers/).
#
# tools/ is wildcarded on purpose: a tool self-registers through the linker
# (REGISTER_TOOL, see include/tool.h), so adding one to the model's syscall
# surface means dropping in a .c file and editing nothing else at all.
TOOL_SRCS := $(wildcard tools/*.c)

KERNEL_SRCS := kernel/main.c kernel/drivers.c $(TOOL_SRCS) \
               arch/x86_64/idt.c arch/x86_64/fault.c arch/x86_64/fault_inject.c \
               core/kobject.c core/tool.c mm/heap.c device/device.c \
               fs/vfs/vfs.c fs/native/ramfs.c fs/fs.c \
               lib/base.c lib/libc_shim.c lib/trace.c \
               vm/dvm.c vm/programs/ac97_boot.c \
               drivers/serial/serial.c drivers/pci/pci.c \
               drivers/input/input.c drivers/input/kbd.c \
               drivers/input/serial_input.c drivers/input/script.c \
               drivers/net/e1000.c drivers/rtc/rtc.c \
               net/json.c net/model.c net/chat.c net/sse.c net/net.c \
               net/faultchat.c net/tls_ca.c
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

ARCH_ASM_OBJS := arch/x86_64/isr.o

OBJS    := boot/boot.o $(ARCH_ASM_OBJS) $(KERNEL_OBJS) $(LWIP_OBJS) $(MBEDTLS_OBJS)

# User-mode networking: the guest reaches the host at 10.0.2.2. Nothing runs
# there — the kernel does its own DNS and its own TLS straight to the internet.
NETFLAGS   := -netdev user,id=n0 -device e1000,netdev=n0

all: kernel.bin

boot/boot.o: boot/boot.asm
	$(ASM) -f elf64 $< -o $@

# The 256 exception stubs (arch/x86_64/isr.asm), same assembler conventions.
arch/x86_64/%.o: arch/x86_64/%.asm .build-flags
	$(ASM) -f elf64 $< -o $@

%.o: %.c .build-flags
	$(CC) $(CFLAGS) $< -o $@

# Header dependencies emitted by -MMD. Absent on a clean tree; that is fine,
# there is nothing stale to catch yet.
-include $(OBJS:.o=.d)

kernel.elf: $(OBJS) linker.ld
	$(LD) $(LDFLAGS) -o $@ $(OBJS) $(LIBGCC)

kernel.bin: kernel.elf
	$(OBJCOPY) -O elf32-i386 $< $@

# The console is VGA text mode (720x400). zoom-to-fit lets the window scale, so
# just drag it bigger (or use the green button) and the text scales up.
DISPLAY ?= cocoa,zoom-to-fit=on

# ...but QEMU opens that window at the raw framebuffer size, which is unreadably
# small on a Retina display, and -display cocoa has no option for the initial
# size. So resize it from the outside once it appears: ZOOM is the multiple of
# the 720x400 console to aim for, +28 covers the title bar, and QEMU snaps the
# result back to the 720:400 aspect itself. The window is moved to the top-left
# first because it can only grow as far as the screen edge — resizing it in
# place silently clamps to whatever room is left to the right.
#
# This needs Accessibility permission for the terminal you run make from
# (System Settings > Privacy & Security > Accessibility). Without it the resize
# is skipped with a one-line warning and you get the small window as before;
# dragging it bigger by hand still works. Runs in the background so the serial
# console on stdio stays in the foreground.
ZOOM ?= 2
RESIZE_WINDOW = ( W=$$((720 * $(ZOOM))); H=$$((400 * $(ZOOM) + 28)); \
	for i in $$(seq 40); do \
	  osascript -e 'tell application "System Events" to tell process "qemu-system-x86_64"' \
	            -e 'set position of window 1 to {40, 45}' \
	            -e "set size of window 1 to {$$W, $$H}" \
	            -e 'end tell' >/dev/null 2>&1 && exit 0; \
	  sleep 0.25; \
	done; \
	echo "note: could not resize the QEMU window (grant your terminal Accessibility permission)" >&2 ) &

run: kernel.bin
	@$(RESIZE_WINDOW)
	qemu-system-x86_64 -kernel kernel.bin $(NETFLAGS) -display $(DISPLAY) -serial stdio

run-big: kernel.bin
	qemu-system-x86_64 -kernel kernel.bin $(NETFLAGS) -display cocoa,zoom-to-fit=on,full-screen=on -serial stdio

run-nox: kernel.bin
	qemu-system-x86_64 -kernel kernel.bin $(NETFLAGS) -display none -serial stdio

# ===========================================================================
# Bootable media — GRUB, an ISO, and a USB stick
#
#   make iso            build talkos.iso (GRUB + kernel, BIOS El Torito,
#                       isohybrid MBR so the same file works on a USB stick)
#   make run-iso        boot the ISO in QEMU through GRUB (-cdrom, no -kernel)
#   make run-iso-nox    same, headless, serial on stdio
#   make usb            print the exact macOS recipe for writing it to a stick
#   make iso-toolchain  brew install what `make iso` needs
#
# WHY: every other run target uses QEMU's -kernel shortcut, which reads the
# Multiboot header directly. No physical machine does that. `make iso` produces
# an artifact a real BIOS will start: GRUB in the boot catalogue, kernel.bin at
# /boot/kernel.bin, boot/grub/grub.cfg as the menu. The kernel is byte-for-byte
# the same file `make run` boots.
#
# grub-mkrescue is not part of the base toolchain and is named differently
# depending on which cross-GRUB you installed, so we look for all three spellings
# and say something useful if none is present. This lookup is lazy (?= makes a
# recursive variable) so a normal `make` never pays for it.
# ===========================================================================

GRUB_MKRESCUE ?= $(shell command -v grub-mkrescue 2>/dev/null || \
                         command -v i686-elf-grub-mkrescue 2>/dev/null || \
                         command -v x86_64-elf-grub-mkrescue 2>/dev/null)
ISO     := talkos.iso
ISOROOT := build/iso

iso: $(ISO)

$(ISO): kernel.bin boot/grub/grub.cfg
	@if [ -z "$(GRUB_MKRESCUE)" ]; then \
	   echo "iso: no grub-mkrescue on PATH. Run 'make iso-toolchain'."; exit 1; fi
	@command -v xorriso >/dev/null 2>&1 || { \
	   echo "iso: xorriso not found (grub-mkrescue needs it). Run 'make iso-toolchain'."; exit 1; }
	rm -rf $(ISOROOT)
	mkdir -p $(ISOROOT)/boot/grub
	cp kernel.bin $(ISOROOT)/boot/kernel.bin
	cp boot/grub/grub.cfg $(ISOROOT)/boot/grub/grub.cfg
	$(GRUB_MKRESCUE) --compress=xz -o $@ $(ISOROOT)
	@echo "iso: $@ is $$(du -h $@ | cut -f1); boot it with 'make run-iso'"

# Boot through a real bootloader instead of -kernel. This is the target that
# proves the ISO, and the only difference from `make run` is how the kernel got
# into memory.
run-iso: $(ISO)
	@$(RESIZE_WINDOW)
	qemu-system-x86_64 -cdrom $(ISO) $(NETFLAGS) -display $(DISPLAY) -serial stdio

run-iso-nox: $(ISO)
	qemu-system-x86_64 -cdrom $(ISO) $(NETFLAGS) -display none -serial stdio

# Deliberately does NOT write anything. Picking the wrong /dev/diskN erases a
# disk, and no build system should guess which one you meant.
usb: $(ISO)
	@echo "$(ISO) is an isohybrid image: it carries a protective MBR, so the"
	@echo "same bytes boot from optical media and from a USB stick."
	@echo ""
	@echo "On macOS:"
	@echo "  1. diskutil list                     # find the stick, e.g. /dev/disk4"
	@echo "  2. diskutil unmountDisk /dev/diskN   # unmount, do NOT eject"
	@echo "  3. sudo dd if=$(ISO) of=/dev/rdiskN bs=4m status=progress"
	@echo "         (rdiskN, with the r, is the raw device and is ~20x faster)"
	@echo "  4. diskutil eject /dev/diskN"
	@echo ""
	@echo "Then boot the target machine from USB with LEGACY BIOS / CSM enabled."
	@echo ""
	@echo "WHAT THIS KERNEL NEEDS FROM THE MACHINE, and what it does when it is"
	@echo "missing. None of the following has been tried on physical hardware -"
	@echo "the ISO is verified only under QEMU (make run-iso):"
	@echo "  * Legacy BIOS or CSM. The image has no UEFI boot path at all, and"
	@echo "    even with one the console is VGA text mode at 0xB8000, which pure"
	@echo "    UEFI does not provide. A UEFI-only laptop will not boot this."
	@echo "  * An Intel 82540EM/82545EM NIC (PCI 8086:100e/1004/100f). Anything"
	@echo "    else - and that is every modern machine - prints 'e1000: no"
	@echo "    supported NIC found' and boots to a prompt with no model behind"
	@echo "    it. There is also no DHCP: the address is hardcoded 10.0.2.15/24"
	@echo "    via 10.0.2.2, which is QEMU's user-mode network, not your LAN."
	@echo "  * A PS/2 keyboard controller (i8042). USB keyboards work only on"
	@echo "    firmware that still does i8042 emulation; there is no USB stack."
	@echo "    A serial console on COM1 at 115200 8N1 always works and is the"
	@echo "    reliable way to talk to this thing on real hardware."

iso-toolchain:
	brew install i686-elf-grub xorriso mtools

# Added as a prerequisite rather than by editing the clean recipe, so this
# block stays self-contained.
clean: clean-iso
clean-iso:
	rm -rf build $(ISO)

# ===========================================================================
# Tests
#
#   make test        run everything (host unit tests + QEMU boot tests)
#   make test-host   host-native unit tests only (milliseconds — the inner loop)
#   make test-qemu   boot the kernel in QEMU and assert on the serial log
#
# Pure-logic kernel modules (heap, JSON, trace formatting, the driver VM) are
# compiled with the HOST compiler against tests/host/kshim.c and run natively,
# so iterating on them costs no QEMU boot. Anything hardware-dependent is tested
# in tests/qemu/ instead. See tests/host/test_heap.c for the reference example.
# ===========================================================================

HOSTCC     ?= cc
# NOTE: deliberately no -Iport. port/ holds freestanding shims (stdio.h, time.h,
# string.h) for lwIP/mbedTLS; on the host they would shadow the real libc
# headers. Host tests link against real libc.
HOST_CFLAGS := -std=gnu11 -g -O1 -Wall -Wextra -Iinclude -Itests/host \
               -DTALKOS_HOSTTEST
HOST_SHIM  := tests/host/harness.c tests/host/kshim.c
# Each test compiles its sources in one command, so there are no stale objects to
# worry about here — but the BINARY was previously newer than any header it was
# built from, so editing a header left the suite testing the old contract. The
# headers are cheap to depend on wholesale.
HOST_HDRS  := $(wildcard include/*.h) $(wildcard tests/host/*.h)

# Kernel sources under test, per test binary. Add a line when adding a test.
TESTSRC_test_heap := mm/heap.c
TESTSRC_test_json := net/json.c
TESTSRC_test_model_mock := net/json.c net/model.c net/model_mock.c
TESTSRC_test_input := drivers/input/input.c drivers/input/script.c
TESTSRC_test_dev_tools := core/tool.c core/kobject.c device/device.c mm/heap.c lib/trace.c net/json.c
TESTSRC_test_e1000 := core/kobject.c device/device.c mm/heap.c
TESTSRC_test_mem_tools := tools/mem_tools.c core/tool.c mm/heap.c lib/trace.c net/json.c
TESTSRC_test_screen_tools := tools/screen_tools.c core/tool.c lib/trace.c net/json.c
TESTSRC_test_vfs_tools := tools/vfs_tools.c core/tool.c core/kobject.c mm/heap.c fs/vfs/vfs.c fs/native/ramfs.c lib/trace.c net/json.c
TESTSRC_test_tool := core/tool.c lib/trace.c net/json.c
TESTSRC_test_rtc := drivers/rtc/rtc.c tools/time_tools.c core/tool.c lib/trace.c net/json.c
TESTSRC_test_tls_verify := net/tls_ca.c drivers/rtc/rtc.c
# test_tls_chain links the REAL vendored mbedTLS and asks it for verdicts, which
# is the only way to test the verifier rather than a transcription of it. It is
# the one host test that needs the kernel's mbedTLS config and TALKOS_VERIFY_CERTS
# — before this, nothing in the tree built with that flag at all, so a
# path-validation check could be switched off with `make test` still green.
# -w because the vendored library is not warning-clean under -Wall -Wextra on the
# host; the sources under test here are tls_ca.c and rtc.c, both covered warning-
# clean by test_tls_verify.
TESTSRC_test_tls_chain := net/tls_ca.c drivers/rtc/rtc.c $(MBEDTLS_SRCS)
HOSTCFLAGS_test_tls_chain := -DTALKOS_VERIFY_CERTS -Imbedtls/include \
                             -DMBEDTLS_CONFIG_FILE='"mbedtls_config.h"' -w
TESTSRC_test_trace := lib/trace.c
TESTSRC_test_dvm := vm/dvm.c lib/trace.c
TESTSRC_test_dvm_ac97 := vm/dvm.c lib/trace.c
TESTSRC_test_dvm_tools := vm/dvm.c lib/trace.c core/tool.c core/kobject.c \
                          device/device.c mm/heap.c net/json.c net/model.c \
                          net/model_mock.c net/chat.c
# test_dvm_tools.c #includes the tool source (for REGISTER_TOOL on Mach-O) and
# the recorded session, so it must follow both.
tests/build/test_dvm_tools: tools/dvm_tools.c vm/transcripts/dvm_bringup.h
# ac97_bringup.h is generated from ac97_bringup.dvm; both users must follow it.
vm/programs/ac97_boot.o tests/build/test_dvm_ac97: vm/programs/ac97_bringup.h
TESTSRC_test_fault := arch/x86_64/fault.c tools/fault_tools.c core/tool.c \
                      lib/trace.c net/json.c
TESTSRC_test_fault_diagnose := net/faultchat.c arch/x86_64/fault.c \
                      tools/fault_tools.c core/tool.c lib/trace.c net/json.c \
                      net/model.c net/model_mock.c
TESTSRC_test_chat := net/json.c net/model.c net/model_mock.c net/chat.c \
                     core/tool.c lib/trace.c
TESTSRC_test_sse := net/json.c net/model.c net/sse.c

HOST_TESTS := $(patsubst tests/host/%.c,%,$(wildcard tests/host/test_*.c))
HOST_BINS  := $(addprefix tests/build/,$(HOST_TESTS))

# Generate one link rule per test so each pulls in only the sources it needs.
define HOST_TEST_RULE
tests/build/$(1): tests/host/$(1).c $$(HOST_SHIM) $$(TESTSRC_$(1)) $$(HOST_HDRS)
	@mkdir -p tests/build
	@$$(HOSTCC) $$(HOST_CFLAGS) $$(HOSTCFLAGS_$(1)) -o $$@ tests/host/$(1).c $$(HOST_SHIM) $$(TESTSRC_$(1))
endef
$(foreach t,$(HOST_TESTS),$(eval $(call HOST_TEST_RULE,$(t))))

test-host: $(HOST_BINS)
	@fail=0; for b in $(HOST_BINS); do \
	   echo "[host] $$b"; ./$$b || fail=1; \
	 done; \
	 if [ $$fail -eq 0 ]; then echo "host tests: PASS"; \
	 else echo "host tests: FAIL"; exit 1; fi

test-qemu: kernel.bin
	@if [ -x tests/qemu/run.sh ]; then tests/qemu/run.sh; \
	 else echo "test-qemu: tests/qemu/run.sh not built yet"; exit 1; fi

test: test-host test-qemu

clean:
	rm -f boot/boot.o $(ARCH_ASM_OBJS) $(KERNEL_OBJS) $(LWIP_OBJS) $(MBEDTLS_OBJS) kernel.elf kernel.bin net.pcap
	rm -f $(OBJS:.o=.d) .build-flags
	rm -rf tests/build

toolchain:
	brew install nasm x86_64-elf-gcc qemu

.PHONY: all run run-big run-nox clean toolchain test test-host test-qemu \
        iso run-iso run-iso-nox usb iso-toolchain clean-iso .FORCE
