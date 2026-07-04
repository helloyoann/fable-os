# talk-os (bare-metal OS)

A from-scratch x86_64 kernel, built and run on macOS via QEMU. It boots into a
minimal terminal: you type a question, the kernel sends it to the Anthropic
Claude API **directly over HTTPS**, and the reply is printed. Keyboard in, text
out — no graphics, no audio.

## Status

| Subsystem | State |
|---|---|
| Boot → 64-bit long mode, 4 GiB identity map | ✅ done |
| Kernel object base (identity + refcounted lifetime) | ✅ done |
| Kernel heap (coalescing free-list: kmalloc/kfree/krealloc, stats) | ✅ done |
| Device model (id/class/resources/state) + registry | ✅ done |
| Driver framework (self-registering, lifecycle: init/probe/suspend/resume) | ✅ done |
| VFS + native RAM filesystem (files/dirs/mount/handles/seek) | ✅ done |
| Serial console (debug log) | ✅ done |
| PCI bus (enumerated into the device model) | ✅ done |
| VGA text console (80x25, scrolling, cursor) | ✅ done |
| PS/2 keyboard (polled) | ✅ done |
| Networking (e1000 + lwIP, DNS) | ✅ done |
| TLS (mbedTLS 2.28 via lwIP altcp_tls) | ✅ done |
| AI terminal — ask over HTTPS, print the reply | ✅ done |
| Disk-backed filesystem (FAT32/ext2), ATA/block layer | ⬜ future (VFS ready) |
| Certificate verification / CSPRNG entropy | ⬜ future (see caveat) |

## ⚠️ Security caveat

TLS runs with `MBEDTLS_SSL_VERIFY_NONE` (`ALTCP_MBEDTLS_AUTHMODE 0` in
`port/lwipopts.h`): the server certificate is parsed but **not trust-checked**,
because the kernel has no CA bundle and no wall clock. The connection is
encrypted but **not authenticated** — MITM-able. Entropy comes from
`mbedtls_hardware_poll()` (RDRAND, or a TSC mix as fallback) in `lib/libc_shim.c`
— functional, not a vetted CSPRNG. Fine for a demo; not for real secrets.

## Build & run

```sh
make                       # build kernel.bin (no key -> API returns 401)
make KEY=sk-ant-... run    # build with your API key + boot (QEMU window + serial)
make run-nox               # headless, serial only (no keyboard; self-test still runs)
```

The API key is compiled in via `-DTALKOS_API_KEY`. Because `make` doesn't track
the `KEY` variable as a dependency, change it with a clean build (or
`touch net/net.c`) so `net.o` is rebuilt:

```sh
make clean && make KEY=sk-ant-... run
```

At boot the kernel runs one self-test HTTPS request and prints the HTTP status +
reply to the console, so the TLS path is verifiable even headlessly.

## Layout

```
boot/        boot.asm — multiboot, long mode, .bss zero, paging
kernel/      main.c (terminal REPL + boot self-tests), drivers.c (driver manager)
core/        kobject.c — kernel object base (identity + refcounted lifetime)
mm/          heap.c — coalescing kernel heap (kmalloc/kfree/krealloc + stats)
device/      device.c — generic device model + registry
fs/          vfs/vfs.c (fs-independent VFS core), native/ramfs.c (native FS),
             fs.c (subsystem bring-up + root mount)
lib/         base.c (VGA console/printf/time), libc_shim.c (libc + snprintf +
             mbedtls_hardware_poll entropy)
drivers/
  serial/    COM1 UART debug console
  pci/       PCI config space + enumeration
  input/     PS/2 keyboard (polled)
  net/       e1000 NIC
net/         net.c — lwIP netif glue + DNS + the HTTPS "ask" client
port/        freestanding shims for lwIP/mbedTLS (string/stdlib/stdio/time/
             assert/ctype) + lwipopts.h
lwip/        vendored lwIP 2.2.0 (core + ipv4 + altcp_tls mbedTLS port)
mbedtls/     vendored mbedTLS 2.28.9 (library + headers)
include/     shared headers (kobject/heap/device/driver/vfs/fs + mbedtls_config)
linker.ld    1 MiB load, driver_table section
```

The kernel is organised as isolated subsystems behind stable headers
(`kobject.h`, `heap.h`, `device.h`, `driver.h`, `vfs.h`): each has an
architecture comment at the top of its header explaining purpose,
responsibilities, public API, dependencies, and future extension points.
Subsystems talk through those interfaces, not each other's internals — e.g.
an on-disk filesystem later implements `vnode_ops_t` without touching the
kernel API, and another allocator can replace `mm/heap.c` behind `heap.h`.

## The driver model

Drivers self-register — there is **no central list to edit**. To add one, drop a
`.c` under `drivers/<subsystem>/`, define a `driver_t`, and register it:

```c
#include "driver.h"
static int mything_init(void) { /* ... */ return 0; }
static const driver_t mything_driver = {
    .name = "mything",
    .level = DRV_LEVEL_DEVICE,   // EARLY < BUS < DEVICE < LATE
    .init = mything_init,
};
REGISTER_DRIVER(mything_driver);
```

`drivers_init()` walks the linker-collected table in ascending level order, so
serial and PCI come up before the devices that need them. Add the source path to
`KERNEL_SRCS` in the Makefile and it's in the build.

## TLS notes

- mbedTLS config: `include/mbedtls_config.h` — a TLS 1.2 client with ECDHE +
  AES-GCM / ChaCha20-Poly1305, P-256/P-384/X25519, no filesystem, no sockets.
- The whole `mbedtls/library/*.c` set is compiled; disabled modules compile to
  almost nothing.
- `libgcc` is linked for 128-bit division helpers used by the bignum code.
- SNI is set per connection in `net.c` via `altcp_tls_context()` +
  `mbedtls_ssl_set_hostname()`.
```
