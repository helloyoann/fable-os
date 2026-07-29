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
| Certificate verification (pinned roots + hostname + expiry) | ✅ done, **off by default** — `-DTALKOS_VERIFY_CERTS` |
| CSPRNG entropy | ⬜ future (see caveat) |

## ⚠️ Security caveat

**The default build does not authenticate the server.** TLS runs with
`MBEDTLS_SSL_VERIFY_NONE` (`ALTCP_MBEDTLS_AUTHMODE 0` in `port/lwipopts.h`): the
certificate is parsed and then believed. The connection is encrypted but not
authenticated, so anything that can answer on port 443 — a proxy, a hostile
router, QEMU's own user-mode stack — can read and rewrite the conversation, and
with a key compiled in, take the key. Do not send anything sensitive.

Real verification exists and works; it is opt-in:

```sh
make EXTRA_CFLAGS=-DTALKOS_VERIFY_CERTS
```

With the flag the kernel requires the chain to build to one of two **pinned**
roots, requires the certificate to name the host it asked for, and enforces
`notBefore`/`notAfter` against the CMOS clock. Details, and the reasons the flag
is off by default, are in [`TALKOS_VERIFY_CERTS`](#tls-certificate-verification-talkos_verify_certs)
below.

**Entropy is not fixed by any of this, and is the remaining weakness.** The
CTR_DRBG is seeded by `mbedtls_hardware_poll()` in `lib/libc_shim.c`, which is
RDRAND when the CPU has it and a TSC mix when it does not. That is a functional
source, not a vetted CSPRNG: there is no entropy accounting, no health test, no
pool, and the TSC fallback in particular is weakly unpredictable at best.
Verification tells you *who* you are talking to; it says nothing about the
quality of the keys protecting it.

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
  `mbedtls_ssl_set_hostname()`. Under `TALKOS_VERIFY_CERTS` that same name is
  what the certificate is checked against.

## TLS certificate verification (`TALKOS_VERIFY_CERTS`)

Off by default. On with:

```sh
make EXTRA_CFLAGS=-DTALKOS_VERIFY_CERTS
```

### What it turns on

| Piece | Where | Effect |
|---|---|---|
| `MBEDTLS_SSL_VERIFY_REQUIRED` | `ALTCP_MBEDTLS_AUTHMODE 2`, `port/lwipopts.h` | a failed chain aborts the handshake instead of being recorded and ignored |
| Pinned CA bundle | `net/tls_ca.c`, installed by `net_init()` | the chain must build to a trusted root |
| Hostname check | `mbedtls_ssl_set_hostname()` in `net/net.c` | the certificate must name `api.anthropic.com` |
| `MBEDTLS_HAVE_TIME_DATE` | `include/mbedtls_config.h` | `notBefore`/`notAfter` are compared instead of merely parsed |
| `mbedtls_time` → `talkos_tls_time` | `include/mbedtls_config.h` → `net/tls_ca.c` | the comparison uses the CMOS RTC, not seconds since boot |
| `MBEDTLS_PLATFORM_GMTIME_R_ALT` | `include/mbedtls_config.h` → `net/tls_ca.c` | supplies the `gmtime_r` a freestanding build does not have |

Without `MBEDTLS_HAVE_TIME_DATE`, mbedTLS compiles `mbedtls_x509_time_is_past()`
and `_is_future()` to `return 0` — the dates are stored and never looked at. It
is set here, and the enforcement is demonstrated below rather than asserted.

### What is trusted

Two roots, both Google Trust Services, chosen from the chain
`api.anthropic.com` actually serves (`leaf → WE1 → GTS Root R4`):

| Root | Key | Valid to | Why |
|---|---|---|---|
| GTS Root R4 | ECDSA P-384 | 2036-06-22 | anchors the live chain |
| GTS Root R1 | RSA-4096 | 2036-06-22 | the RSA sibling hierarchy the CA may move a leaf to without notice |

Provenance, fingerprints, and the reasoning are in `include/tls_ca.h`. The
fingerprints are re-derived from the embedded bytes by
`tests/host/test_tls_verify.c`, so the bundle cannot be edited without the
suite noticing.

**The pin is a real operational risk and is not hedged.** If Anthropic moves to
a different CA, or Google rotates these roots, the kernel stops being able to
reach the API at all — immediately, with `The certificate is not correctly
signed by the trusted CA`, and until somebody rebuilds. There is also no
revocation checking of any kind: no CRL, no OCSP. That tradeoff is the price of
a small precise trust set instead of a 150-root generic bundle, and it is the
main reason the flag is not the default.

### The clock

Verification needs to know the date, which the CMOS RTC driver
(`drivers/rtc/rtc.c`) supplies. Two consequences worth knowing:

- **It fails closed.** If the RTC cannot be read, `tls_ca_now_unix()` returns
  the epoch, every certificate looks not-yet-valid, and no handshake succeeds.
- **It is only as good as the host's clock**, which is assumed to be UTC. There
  is no NTP, and the RTC driver deliberately offers no way to set the clock —
  a clock the model can move is not a clock you can validate a certificate
  against.

### Evidence

Same kernel, built once with the flag, against the real endpoint:

```
net: certificate verification ON (2 pinned roots, hostname + validity enforced)
net:   trust GTS Root R4 (ECDSA P-384, pin expires 2036-06-22) - anchors api.anthropic.com today
net:   trust GTS Root R1 (RSA-4096, pin expires 2036-06-22) - the RSA sibling hierarchy
net:   validity is checked against 2026-07-29T00:58:34Z (CMOS RTC, assumed UTC)
[tls] verified: chain trusted, hostname api.anthropic.com matches, valid 2026-7-24..2026-10-22 UTC
[http] HTTP/1.1 401 Unauthorized
```

and refusing four different bad situations (the last two are the same binary
with QEMU's `-rtc base=` moved, which is what proves the date check runs):

```
example.com, anchored at an SSL.com root:
  [tls] certificate REJECTED for example.com (verify flags 0x8)
  [tls] The certificate is not correctly signed by the trusted CA

a locally generated self-signed cert claiming CN=api.anthropic.com:
  [tls] certificate REJECTED for api.anthropic.com (verify flags 0x8)
  [tls] The certificate is not correctly signed by the trusted CA

the same impostor, asked for under a different name:
  [tls] certificate REJECTED for not-the-real-api.test (verify flags 0xc)
  [tls] The certificate Common Name (CN) does not match with the expected CN;
        The certificate is not correctly signed by the trusted CA

the real API with the clock at 2030-01-01 / 2020-01-01:
  [tls] certificate REJECTED for api.anthropic.com (verify flags 0x1)
  [tls] The certificate validity has expired
  [tls] certificate REJECTED for api.anthropic.com (verify flags 0x200)
  [tls] The certificate validity starts in the future
```

The three `-DTALKOS_TLS_HOST` / `-DTALKOS_TLS_SNI` / `-DTALKOS_TLS_PORT` build
macros used to produce those runs are documented in `net/net.c` and only exist
under the verification flag.
