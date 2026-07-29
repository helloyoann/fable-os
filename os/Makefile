# Build the talk-os bare-metal kernel on macOS.
#
#   make            build kernel.bin
#   make run        boot in QEMU (graphics window + serial on stdio)
#   make run ZOOM=3 same, but a 3x window instead of the default 2x
#   make run-nox    boot headless (serial only)
#   make clean
#
# The Anthropic API key is NOT a build input; see "THE API KEY" below.

ASM     := nasm
CC      := x86_64-elf-gcc
LD      := x86_64-elf-ld
OBJCOPY := x86_64-elf-objcopy

# ===========================================================================
# THE API KEY IS NOT A BUILD INPUT
#
# It used to be: `-DTALKOS_API_KEY='"$(KEY)"'` put the live secret inside
# net.o, kernel.elf, kernel.bin, talkos.iso AND .build-flags (which recorded
# the flag set verbatim, i.e. `KEY=sk-ant-...` in plain text). Five artifacts,
# kept out of git by a .gitignore — one `git add -f`, one CI cache, one shared
# build directory and the key is published. That is the wrong place for a
# safety property, so the hazard is gone rather than ignored: there is no KEY
# variable and no -D anywhere below, and `grep -r sk-ant` over every build
# output finds nothing because the compiler is never told.
#
# The key now travels host -> guest at RUN time over QEMU's fw_cfg channel and
# lives only in RAM. See the run targets (KEY_ARGS) and drivers/fwcfg/.
#
# Refuse KEY= loudly rather than ignore it: silently dropping a key someone
# believes they passed produces a kernel that gets 401 and an operator who
# does not know why.
# ===========================================================================
ifneq ($(strip $(KEY)),)
$(error KEY= is gone: the API key is never compiled in. Put KEY="sk-ant-..." \
in os/.env (gitignored) and run `make run` — it is handed to the guest at boot \
over fw_cfg. See the README section "The API key never enters the build".)
endif

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
#             "[tls] verified: chain trusted".
#
# HOW THAT IS ENFORCED, AND WHY IT IS NOT AN ORDINARY PREREQUISITE.
#
# The obvious shape — every object depends on a stamp holding the flags, and the
# stamp is rewritten when they change — DOES NOT WORK with the make this project
# is built with. /usr/bin/make here is GNU Make 3.81 (Apple's), whose
# is-the-prerequisite-newer test has WHOLE-SECOND granularity. The stamp is
# rewritten at the start of the new build, i.e. almost always inside the same
# wall-clock second as the tail of the previous build's objects, so those objects
# compare as up to date and are silently NOT recompiled. Measured: 51 to 184 of
# 184 objects skipped, non-deterministically, and a second identical make then
# recompiles ZERO, so the mixed-flag kernel is permanent until `make clean`. The
# worst observed outcome was an exit-0 build that recorded VERIFY_CERTS in the
# stamp while ten of the twelve flag-sensitive objects were still the
# non-verifying ones — precisely the failure the stamp was added to prevent, and
# it also hides the link error that a genuinely flagged build would surface.
#
# So the check does not rely on mtimes at all. It runs at PARSE time, before the
# dependency graph is built, and DELETES the objects the flag change invalidates.
# Deletion is a fact; mtime ordering is a guess. The cost is that alternating
# flag sets always pays a full rebuild — which is the honest price, and this tree
# builds clean in a couple of seconds.
#
# This stamp used to hold `KEY=sk-ant-...` in plain text, which made an
# untracked build artifact the second-easiest place to find the key. There is no
# KEY any more, so there is nothing secret in here to leak.
# Parse-time work must not happen under `make -n`: a dry run has to be free of
# side effects, or it stops being a way to find out what a build WOULD do.
# GNU Make puts the dry-run flag in the first word of MAKEFLAGS.
DRY_RUN := $(findstring n,$(firstword -$(MAKEFLAGS)))

BUILD_FLAGS   := EXTRA_CFLAGS=$(EXTRA_CFLAGS)
BUILD_STAMP   := .build-flags
FLAGS_CHANGED := $(if $(DRY_RUN),,$(shell \
  if [ "$$(cat $(BUILD_STAMP) 2>/dev/null)" != '$(BUILD_FLAGS)' ]; then \
     if [ -f $(BUILD_STAMP) ]; then echo changed; fi; \
     find . \( -name '*.o' -o -name '*.d' \) -not -path './tests/*' -delete; \
     rm -f kernel.elf kernel.bin; \
     printf '%s' '$(BUILD_FLAGS)' > $(BUILD_STAMP); \
  fi))
ifeq ($(FLAGS_CHANGED),changed)
$(info make: EXTRA_CFLAGS changed to "$(EXTRA_CFLAGS)"; discarded every kernel \
object so nothing is built with the previous flag set)
endif
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
               arch/x86_64/idt.c arch/x86_64/fault.c \
               core/kobject.c core/tool.c mm/heap.c device/device.c \
               fs/vfs/vfs.c fs/native/ramfs.c fs/fs.c \
               lib/base.c lib/libc_shim.c lib/trace.c \
               lib/fb.c lib/font.c lib/font_spleen8x16.c \
               gui/wm.c gui/widgets.c gui/gui_demo.c \
               apps/runtime.c apps/expr.c apps/app_selftest.c \
               vm/dvm.c vm/programs/ac97_boot.c \
               drivers/serial/serial.c drivers/fwcfg/fwcfg.c drivers/pci/pci.c \
               drivers/acpi/acpi.c drivers/acpi/power.c \
               drivers/acpi/power_selftest.c \
               drivers/input/input.c drivers/input/kbd.c \
               drivers/input/serial_input.c drivers/input/script.c \
               drivers/input/mouse.c \
               drivers/net/e1000.c drivers/rtc/rtc.c \
               net/json.c net/model.c net/chat.c net/sse.c net/net.c \
               net/faultchat.c net/tls_ca.c

# The exception self-test, in DIAGNOSTIC BUILDS ONLY. fault_inject.c exports
# fault_inject(), which deliberately divides by zero and dereferences an
# unmapped page; its own header argues that a machine whose only interface is
# natural language must not have a "kill yourself" verb sitting one hallucination
# away from the model, and everything that would call it is already behind
# #ifdef TALKOS_FAULT_TEST. That made "it is unreachable" a convention about the
# source; leaving the object out of a normal image makes it a fact about the
# binary. Verified before the change: no object in the kernel link had an
# undefined reference to fault_inject or fault_inject_name.
ifneq ($(findstring TALKOS_FAULT,$(EXTRA_CFLAGS)),)
KERNEL_SRCS += arch/x86_64/fault_inject.c
endif

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

# ===========================================================================
# Handing the API key to the guest at RUN time
#
#   os/.env               KEY="sk-ant-..."   (gitignored; where the key lives)
#   $ANTHROPIC_API_KEY                       used when .env has no key
#   make run TALKOS_ENV=/path/to/other/env   read a different file
#
# WHY A TEMP FILE. QEMU's -fw_cfg takes either `string=` or `file=`. `string=`
# is not an option: it puts the secret in QEMU's argv, where `ps` shows it to
# every user on the machine, and where it lands in shell history. So the key is
# written to a file created by mktemp — mode 0600, in $TMPDIR, i.e. OUTSIDE the
# repository and outside anything `make iso` copies into an image — and removed
# however the run ends: normal exit, a QEMU that refused to start, or the Ctrl-C
# that is how you actually leave `make run`. The only way to leave it behind is
# SIGKILL on the shell, which no trap anywhere can catch.
#
# That trap is why every keyed run target is ONE shell command with no `exec`:
# `exec` replaces the shell and takes its EXIT trap with it, and a second recipe
# line would be a second shell that never had the trap. Do not split them.
#
# The key is read inside the recipe's shell and never into a make variable: a
# make variable is visible in `make -p`, is exported to every sub-make, and gets
# echoed with the recipe.
# ===========================================================================
TALKOS_ENV ?= .env

# WHICH LINE OF .env IS THE KEY. Two rules, both learned the hard way.
#
#   1. The NAME IS MATCHED EXACTLY, against a two-name allowlist. This pattern
#      used to be `[A-Z_]*KEY`, i.e. any variable whose name ends in KEY, with
#      `tail -n 1` picking the LAST match. A .env file is conventionally shared
#      across a project and holds several credentials, so `KEY=sk-ant-...`
#      followed by `AWS_SECRET_ACCESS_KEY=...` sent the AWS secret to
#      api.anthropic.com in the x-api-key header — and the plainly realistic
#      `ANTHROPIC_API_KEY=` / `OPENAI_API_KEY=` pair is alphabetical, so the
#      OpenAI key won. Nothing surfaced it: fwcfg prints a byte count by design,
#      so the boot log said "32 bytes loaded" and then 401, indistinguishable
#      from a typo. Exfiltrating someone else's credential to a third party is
#      not an acceptable failure mode for a misnamed variable.
#
#   2. ORDER IN THE FILE DOES NOT DECIDE ANYTHING. ANTHROPIC_API_KEY wins over
#      KEY because it is the more specific name, not because of where it sits,
#      and `head -n 1` makes a repeated name deterministic too.
#
# Any other *KEY= line is named (NEVER printed) so the operator learns their
# other credential was ignored rather than silently transmitted.
#
# `set +x +v` is the first thing in the recipe, before `key=` is ever assigned.
# On macOS /bin/sh is bash in POSIX mode, and bash honours an INHERITED
# SHELLOPTS: with `export SHELLOPTS=xtrace` in the environment (a CI image, a
# debugging developer) every command below is traced to stderr with the value
# expanded, so `make run 2>&1 | tee build.log` puts the live key in a log file
# four times over. One token prevents it.
READ_KEY = set +x +v; key=; \
	if [ -f '$(TALKOS_ENV)' ]; then \
	  for n in ANTHROPIC_API_KEY KEY; do \
	    key=$$(sed -n "s/^[[:space:]]*\(export[[:space:]]*\)*$$n[[:space:]]*=[[:space:]]*//p" '$(TALKOS_ENV)' \
	           | head -n 1 | tr -d '\r\n' \
	           | sed -e 's/^"//' -e 's/"$$//' -e "s/^'//" -e "s/'$$//"); \
	    if [ -n "$$key" ]; then break; fi; \
	  done; \
	  other=$$(sed -n 's/^[[:space:]]*\(export[[:space:]]*\)*\([A-Z_]*KEY\)[[:space:]]*=.*/\2/p' '$(TALKOS_ENV)' \
	           | grep -vx -e KEY -e ANTHROPIC_API_KEY | sort -u | tr '\n' ' '); \
	  if [ -n "$$other" ]; then \
	    echo "note: $(TALKOS_ENV) also defines $$other - IGNORED. Only KEY and ANTHROPIC_API_KEY are read as the Anthropic key."; \
	  fi; \
	fi; \
	if [ -z "$$key" ]; then key=$${ANTHROPIC_API_KEY:-}; fi;

# Expands to the -fw_cfg arguments in "$@", or to nothing at all. No key is the
# supported default: the kernel boots, says it has none, and the API answers 401.
KEY_ARGS = $(READ_KEY) set --; \
	if [ -n "$$key" ]; then \
	  keyfile=$$(mktemp "$${TMPDIR:-/tmp}/talkos-fwcfg.XXXXXXXX") || exit 1; \
	  trap 'rm -f "$$keyfile"' EXIT HUP INT QUIT TERM; \
	  chmod 600 "$$keyfile"; \
	  printf '%s' "$$key" > "$$keyfile"; \
	  key=; \
	  set -- -fw_cfg "name=opt/talkos/apikey,file=$$keyfile"; \
	  echo "note: api key passed to the guest over fw_cfg as opt/talkos/apikey"; \
	else \
	  echo "note: no api key in $(TALKOS_ENV) and no ANTHROPIC_API_KEY - the API will answer 401"; \
	fi;

all: kernel.bin

boot/boot.o: boot/boot.asm .build-flags
	$(ASM) -f elf64 $< -o $@

# The 256 exception stubs (arch/x86_64/isr.asm), same assembler conventions.
arch/x86_64/%.o: arch/x86_64/%.asm .build-flags
	$(ASM) -f elf64 $< -o $@

%.o: %.c .build-flags
	$(CC) $(CFLAGS) $< -o $@

# Vendored code is not this project's code and cannot be fixed here: lwIP's
# altcp_tls port emits six warnings under -Wall -Wextra (ignored
# mbedtls_ssl_flush_output results, a sign-compare, two unused parameters) on
# every clean build, and their only effect is to bury the next warning in a file
# we DO own. Silence the vendored trees so a warning means something again. Same
# reasoning as -w on TESTSRC_test_tls_chain below. -w changes no code
# generation: the objects are byte-identical with and without it.
lwip/%.o mbedtls/%.o: CFLAGS += -w

# Header dependencies emitted by -MMD. Absent on a clean tree; that is fine,
# there is nothing stale to catch yet.
-include $(OBJS:.o=.d)

kernel.elf: $(OBJS) linker.ld
	$(LD) $(LDFLAGS) -o $@ $(OBJS) $(LIBGCC)

kernel.bin: kernel.elf
	$(OBJCOPY) -O elf32-i386 $< $@

# The console is VGA text mode (720x400). zoom-to-fit lets the window scale, so
# just drag it bigger (or use the green button) and the text scales up.
#
# NOT called DISPLAY: that name is the X11 environment variable, and an
# environment variable beats `?=`. Anyone with XQuartz running, or on `ssh -X`,
# silently got `-display :0` and a QEMU that refused to start, with a Makefile
# that looked correct.
QEMU_DISPLAY ?= cocoa,zoom-to-fit=on

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
	@$(KEY_ARGS) qemu-system-x86_64 -kernel kernel.bin $(NETFLAGS) -display $(QEMU_DISPLAY) -serial stdio "$$@"

run-nox: kernel.bin
	@$(KEY_ARGS) qemu-system-x86_64 -kernel kernel.bin $(NETFLAGS) -display none -serial stdio "$$@"

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
	@$(KEY_ARGS) qemu-system-x86_64 -cdrom $(ISO) $(NETFLAGS) -display $(QEMU_DISPLAY) -serial stdio "$$@"

run-iso-nox: $(ISO)
	@$(KEY_ARGS) qemu-system-x86_64 -cdrom $(ISO) $(NETFLAGS) -display none -serial stdio "$$@"

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
	@echo "  * QEMU's fw_cfg interface at 0x510, which is where the Anthropic"
	@echo "    API key arrives (it is never compiled in). No physical machine"
	@echo "    has it, so a real box prints 'fwcfg: no QEMU fw_cfg interface'"
	@echo "    and 'no api key', and every request it makes gets a 401. Giving"
	@echo "    real hardware a key needs a different channel - see the FUTURE"
	@echo "    EXTENSION POINTS in include/fwcfg.h."
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
#   make test-host-asan  the same suites under AddressSanitizer + UBSan
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

# ...and the same argument as .build-flags, one directory over. The host
# binaries depended on sources and headers only, so in an already-built tree
#   make test-host HOST_CFLAGS="... -fsanitize=address"
# compiled nothing, ran the previous uninstrumented binaries, and printed
# "host tests: PASS" — a green that says nothing about the flags you asked for.
# The stamp holds the compiler and every flag set that reaches a host binary,
# including the per-test ones, so changing any of them forces an honest rebuild.
# It has the same whole-second mtime hole as .build-flags above and is closed the
# same way: the check runs at parse time and DELETES the stale binaries, because
# `make test-host` silently running yesterday's binaries is exactly the failure
# a stamp is supposed to prevent.
# HOST_BUILD_FLAGS is recursive on purpose: HOSTCFLAGS_* and HOST_TESTS are not
# known yet. The parse-time check that consumes it therefore lives further down,
# immediately after HOST_TESTS is defined — see "host flag stamp" below.
HOST_BUILD_FLAGS = HOSTCC=$(HOSTCC) HOST_CFLAGS=$(HOST_CFLAGS) \
                   $(foreach t,$(HOST_TESTS),$(t):$(HOSTCFLAGS_$(t)))
HOST_STAMP       := tests/build/.host-flags

# Kernel sources under test, per test binary. Add a line when adding a test.
TESTSRC_test_heap := mm/heap.c
TESTSRC_test_json := net/json.c
TESTSRC_test_model_mock := net/json.c net/model.c net/model_mock.c
TESTSRC_test_input := drivers/input/input.c drivers/input/script.c
TESTSRC_test_mouse := drivers/input/mouse.c
TESTSRC_test_dev_tools := core/tool.c core/kobject.c device/device.c mm/heap.c lib/trace.c net/json.c
TESTSRC_test_e1000 := core/kobject.c device/device.c mm/heap.c
TESTSRC_test_fwcfg := drivers/fwcfg/fwcfg.c
TESTSRC_test_mem_tools := tools/mem_tools.c core/tool.c mm/heap.c lib/trace.c net/json.c
TESTSRC_test_screen_tools := tools/screen_tools.c core/tool.c lib/trace.c net/json.c
TESTSRC_test_vfs_tools := tools/vfs_tools.c core/tool.c core/kobject.c mm/heap.c fs/vfs/vfs.c fs/native/ramfs.c lib/trace.c net/json.c
TESTSRC_test_tool := core/tool.c lib/trace.c net/json.c
TESTSRC_test_rtc := drivers/rtc/rtc.c tools/time_tools.c core/tool.c lib/trace.c net/json.c
TESTSRC_test_acpi := drivers/acpi/acpi.c drivers/acpi/power.c tools/power_tools.c \
                     core/tool.c lib/trace.c net/json.c
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
TESTSRC_test_fb := lib/fb.c lib/font.c lib/font_spleen8x16.c
TESTSRC_test_gui := gui/wm.c gui/widgets.c gui/gui_demo.c tools/gui_tools.c \
                    lib/fb.c lib/font.c lib/font_spleen8x16.c \
                    drivers/input/mouse.c core/tool.c lib/trace.c net/json.c
# tools/gui_tools.c is linked in unmodified so the golden transcript can press
# the app's keys with gui_click, exactly as the model does.
TESTSRC_test_app := apps/runtime.c apps/expr.c tools/app_tools.c \
                    tools/gui_tools.c gui/wm.c gui/widgets.c gui/gui_demo.c \
                    lib/fb.c lib/font.c lib/font_spleen8x16.c \
                    drivers/input/mouse.c core/tool.c lib/trace.c \
                    net/json.c net/model.c net/model_mock.c net/chat.c
# calculator_json.h is generated from calculator.json (apps/examples/gen_header.py);
# both the kernel object that embeds it and the test that diffs it must follow it.
apps/runtime.o tests/build/test_app: apps/examples/calculator_json.h
TESTSRC_test_trace := lib/trace.c
TESTSRC_test_dvm := vm/dvm.c lib/trace.c
TESTSRC_test_dvm_ac97 := vm/dvm.c lib/trace.c
TESTSRC_test_dvm_tools := vm/dvm.c lib/trace.c core/tool.c core/kobject.c \
                          device/device.c mm/heap.c net/json.c net/model.c \
                          net/model_mock.c net/chat.c
# A test that #includes a kernel .c (the REGISTER_TOOL / REGISTER_DRIVER linker
# sections do not survive a Mach-O host link any other way) does not get that
# file as a prerequisite from TESTSRC_*, so it needs one spelled out here.
# Without it `make test-host` re-runs a STALE binary and prints PASS for code
# that is no longer in the tree — the same failure the header dependency above
# was added to stop. Every #include of a ../../*.c must appear below:
#   $ grep -n '#include "\.\./\.\./.*\.c"' tests/host/*.c
tests/build/test_dev_tools: tools/dev_tools.c
tests/build/test_e1000: drivers/net/e1000.c
# test_dvm_tools.c #includes the tool source and the recorded session, so it
# must follow both.
tests/build/test_dvm_tools: tools/dvm_tools.c vm/transcripts/dvm_bringup.h
# ac97_bringup.h holds the reference driver program as a C string literal,
# derived from vm/programs/ac97_bringup.dvm by vm/programs/gen_header.py. There
# is deliberately NO rule to run that generator: the header is checked in, the
# build needs no python3, and drift is caught where it can be explained —
# test_dvm_ac97.c diffs the two and fails with "run python3
# vm/programs/gen_header.py". These two only need to follow the header itself.
vm/programs/ac97_boot.o tests/build/test_dvm_ac97: vm/programs/ac97_bringup.h
TESTSRC_test_fault := arch/x86_64/fault.c tools/fault_tools.c core/tool.c \
                      lib/trace.c net/json.c
TESTSRC_test_fault_diagnose := net/faultchat.c arch/x86_64/fault.c \
                      tools/fault_tools.c core/tool.c lib/trace.c net/json.c \
                      net/model.c net/model_mock.c
TESTSRC_test_chat := net/json.c net/model.c net/model_mock.c net/chat.c \
                     core/tool.c lib/trace.c
# test_agency drives whole multi-step jobs through the real loop against the real
# VFS and agent tool families, so it links both of them plus the filesystem.
TESTSRC_test_agency := net/json.c net/model.c net/model_mock.c net/chat.c \
                     core/tool.c core/kobject.c lib/trace.c mm/heap.c \
                     fs/vfs/vfs.c fs/native/ramfs.c \
                     tools/vfs_tools.c tools/agent_tools.c
TESTSRC_test_sse := net/json.c net/model.c net/sse.c

HOST_TESTS := $(patsubst tests/host/%.c,%,$(wildcard tests/host/test_*.c))
HOST_BINS  := $(addprefix tests/build/,$(HOST_TESTS))

# host flag stamp — the deferred half of HOST_BUILD_FLAGS above. Every per-test
# HOSTCFLAGS_* is now known, so the stamp can be compared for real.
HOST_FLAGS_CHANGED := $(if $(DRY_RUN),,$(shell \
  mkdir -p tests/build; \
  if [ "$$(cat $(HOST_STAMP) 2>/dev/null)" != '$(HOST_BUILD_FLAGS)' ]; then \
     if [ -f $(HOST_STAMP) ]; then echo changed; fi; \
     find tests/build -type f ! -name '.host-flags' -delete; \
     printf '%s' '$(HOST_BUILD_FLAGS)' > $(HOST_STAMP); \
  fi))
ifeq ($(HOST_FLAGS_CHANGED),changed)
$(info make: host test flags changed; discarded every binary in tests/build so \
none of them is yesterday's)
endif
# The stamp is written at parse time, so it always exists by the time the link
# rules below name it. This empty recipe stops make looking for a way to make it.
$(HOST_STAMP): ;
$(BUILD_STAMP): ;

# Generate one link rule per test so each pulls in only the sources it needs.
define HOST_TEST_RULE
tests/build/$(1): tests/host/$(1).c $$(HOST_SHIM) $$(TESTSRC_$(1)) $$(HOST_HDRS) \
                  tests/build/.host-flags
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

# Nine suites rest their central claim on a sanitizer — test_tool.c's "ASan
# proves it" and "ASan's redzones catch any write outside the window",
# test_json.c's "visible under ASan", test_trace.c, test_tls_verify.c — and
# until this target existed there was no way to build with one, so an
# out-of-bounds read on exactly those paths went undetected. Adding it found two
# real defects immediately: a read below mm/heap.c's arena in test_mem_tools.c,
# and a queued reply whose stack buffer had already gone out of scope in
# test_chat.c. Same rules and same sources as test-host; only the flags differ,
# and tests/build/.host-flags is what makes the rebuild honest.
#
# THE ONE EXCEPTION. On Mach-O the tool registry is emulated by letting the
# linker pack one 8-byte global per REGISTER_TOOL into "__DATA,tool_table" and
# walking it as an array; ASan's per-global redzones make that walk read padding.
# tests/host/tooltable.h annotates the entries it owns so the walk stays legal,
# but two tool sources carry their own copy of that shim (REGISTER_VFS_TOOL in
# tools/vfs_tools.c, the same in tools/agent_tools.c) and cannot be annotated
# from here, so the two suites that link them switch global instrumentation off.
# Heap, stack, use-after-free/scope and every UBSan check stay on for all of
# them. The real fix is one attribute in those two files.
ASAN_HOST_CFLAGS := -fsanitize=address,undefined -fno-omit-frame-pointer
ASAN_NO_GLOBALS  := -mllvm -asan-globals=0
test-host-asan:
	@$(MAKE) --no-print-directory test-host \
	   HOST_CFLAGS="$(HOST_CFLAGS) $(ASAN_HOST_CFLAGS)" \
	   HOSTCFLAGS_test_vfs_tools="$(HOSTCFLAGS_test_vfs_tools) $(ASAN_NO_GLOBALS)" \
	   HOSTCFLAGS_test_agency="$(HOSTCFLAGS_test_agency) $(ASAN_NO_GLOBALS)"

test-qemu: kernel.bin
	@if [ -x tests/qemu/run.sh ]; then tests/qemu/run.sh; \
	 else echo "test-qemu: tests/qemu/run.sh not built yet"; exit 1; fi

test: test-host test-qemu

clean:
	rm -f boot/boot.o $(ARCH_ASM_OBJS) $(KERNEL_OBJS) $(LWIP_OBJS) $(MBEDTLS_OBJS) kernel.elf kernel.bin
	rm -f $(OBJS:.o=.d) .build-flags
	rm -rf tests/build

toolchain:
	brew install nasm x86_64-elf-gcc qemu

.PHONY: all run run-nox clean toolchain test test-host test-host-asan test-qemu \
        iso run-iso run-iso-nox usb iso-toolchain clean-iso
