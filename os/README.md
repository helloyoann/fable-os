# talk-os (bare-metal OS)

A from-scratch x86_64 kernel, built and run on macOS via QEMU. It boots into a
prompt with no shell and no commands: you type a sentence, the kernel sends it to
the Anthropic Claude API **directly over HTTPS** from ring 0, and the model drives
the machine through 45 registered tools. It has a framebuffer console, a window
manager, clickable apps described as JSON documents, a PS/2 mouse, real audio
through a sandboxed driver VM, and ACPI power control.

## Status

| Subsystem | State |
|---|---|
| Boot → 64-bit long mode, 4 GiB identity map | ✅ done |
| IDT — 256 gates, TSS, fault capture + diagnosis | ✅ done |
| Kernel object base (identity + refcounted lifetime) | ✅ done |
| Kernel heap (coalescing free-list: kmalloc/kfree/krealloc, stats) | ✅ done |
| Device model (id/class/resources/state) + registry | ✅ done |
| Driver framework (self-registering, lifecycle: init/probe/suspend/resume) | ✅ done |
| VFS + native RAM filesystem (files/dirs/mount/handles/seek) | ✅ done |
| Serial console (debug log, and an input source) | ✅ done |
| PCI bus (enumerated into the device model) | ✅ done |
| QEMU `fw_cfg` (the host→guest channel the API key arrives on) | ✅ done |
| Framebuffer console — 1024x768x32, Spleen 8x16, UTF-8, 128x48 text | ✅ done |
| VGA text console fallback (80x25) — `console=vga` | ✅ done |
| PS/2 keyboard (polled) | ✅ done |
| PS/2 mouse (polled, wheel, resync) | ✅ done |
| Window manager — windows, z-order, focus, drag, damage compositor | ✅ done |
| Widget toolkit + app runtime (apps are JSON documents, not code) | ✅ done |
| Networking (e1000 + lwIP, DNS) | ✅ done |
| TLS (mbedTLS 2.28 via lwIP altcp_tls) | ✅ done |
| CMOS real-time clock | ✅ done |
| ACPI — RSDP/RSDT/XSDT/FADT, `\_S5_` from the DSDT, soft-off + reboot | ✅ done |
| Bounded driver VM (allowlisted port/MMIO, cycle limit, full trace) | ✅ done |
| AC'97 audio, brought up entirely inside the driver VM | ✅ done |
| The turn loop — 45 tools, 16 rounds, budget notes, retries, a journal | ✅ done |
| Disk-backed filesystem (FAT32/ext2), ATA/block layer | ⬜ future (VFS ready) |
| Certificate verification (pinned roots + hostname + expiry) | ✅ done, **off by default** — `-DTALKOS_VERIFY_CERTS` |
| CSPRNG entropy | ⬜ future (see caveat) |
| Userspace, processes, memory protection, a scheduler | ⬜ none, by design so far |

## What is actually verified, and how

Everything below is reproducible from a clean checkout on macOS + QEMU.

| Check | Command | Result |
|---|---|---|
| Build | `make clean && make -j8` | clean; the only warning is the intentional RWX one |
| Host unit tests | `make test-host` | **28 suites, 9,143,897 assertions, all pass** |
| The same under ASan + UBSan | `make test-host-asan` | 28 suites pass |
| Boot tests | `make test-qemu` | **4 / 4** |
| Headless boot + screenshot | `./capture.sh 20` | reaches `> `, 0 panics |
| Real audio out of the driver VM | `vm/programs/capture_audio.sh` | 440 Hz then 660 Hz, measured from the captured WAV |
| The hardening build | `make EXTRA_CFLAGS=-DTALKOS_VERIFY_CERTS` | links, boots, verifies a live certificate |

**What that does and does not prove.** The host suites are the real evidence: they
drive whole jobs through the actual turn loop against a scripted transport
(`net/model_mock.c`), assert on kernel trace lines, on filesystem state read back
behind the tools, and on what went out on the wire. A live model has driven tools
on this machine — opening a window, clicking its keys, writing and re-reading a
file — but only for a handful of turns. **Nobody has characterised this tool
surface under a live model at scale, adversarially, or over a long session.**
Treat that as unexplored rather than working.

## ⚠️ Security caveat

This kernel has **no isolation of any kind**: ring 0, no userspace, no memory
protection, every page mapped read-write-execute, and a language model holding
`mem_write` and a driver VM. See "Known weaknesses and things nobody has tested"
at the end of this file for the whole list. The network-facing half is here.

**The default build does not authenticate the server.** TLS runs with
`MBEDTLS_SSL_VERIFY_NONE` (`ALTCP_MBEDTLS_AUTHMODE 0` in `port/lwipopts.h`): the
certificate is parsed and then believed. The connection is encrypted but not
authenticated, so anything that can answer on port 443 — a proxy, a hostile
router, QEMU's own user-mode stack — can read and rewrite the conversation, and
take the key. Do not send anything sensitive.

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
make                       # build kernel.bin (identical with or without a key)
make run                   # boot (QEMU window + serial); picks the key up from .env
make run-nox               # headless, serial only (no keyboard; self-test still runs)
```

At boot the kernel runs one self-test HTTPS request and prints the HTTP status +
reply to the console, so the TLS path is verifiable even headlessly.

### The API key never enters the build

There is no `KEY=` and no `-DTALKOS_API_KEY`. `make KEY=...` is a hard error.

The key used to be compiled in, which put the live secret inside `net.o`,
`kernel.elf`, `kernel.bin`, `talkos.iso` **and** `.build-flags` (that stamp
recorded the flag set verbatim, i.e. `KEY=sk-ant-...` in plain text). Five
artifacts, kept out of git by a `.gitignore` — one `git add -f`, one CI cache,
one shared build directory, and a live key is published. So the hazard was
removed rather than ignored: the compiler is never told the key, and

```sh
grep -rI 'sk-ant' *.o kernel.elf kernel.bin .build-flags talkos.iso build/ tests/build/
```

finds nothing after a keyed run.

Instead the key travels host → guest at **run** time, over QEMU's `fw_cfg`
channel, and lives only in RAM:

```sh
cp .env.example .env       # then put your real key in it; .env is gitignored
make run
```

`make run` reads the key out of `.env` (or `$ANTHROPIC_API_KEY`) inside the
recipe's own shell, writes it to a mode-0600 temp file in `$TMPDIR` — outside the
repository, and outside anything `make iso` copies into an image — passes
`-fw_cfg name=opt/talkos/apikey,file=<temp>`, and deletes the temp file however
the run ends: normal exit, a QEMU that would not start, or the Ctrl-C that is how
you actually leave `make run`. `string=` is not used because that would put the
key in QEMU's argv, where `ps` shows it to every user on the machine.

**Exactly two names are read, and order in the file decides nothing:**
`ANTHROPIC_API_KEY` wins over `KEY` because it is the more specific name, and a
repeated name takes the first. Any other `*KEY=` line is named in a note and
IGNORED. This matters more than it sounds: the pattern used to be "any variable
ending in KEY, last one wins", and a `.env` holding both `ANTHROPIC_API_KEY=` and
`OPENAI_API_KEY=` — alphabetical, and an entirely ordinary file — sent the OpenAI
key to `api.anthropic.com` in the `x-api-key` header. Because the kernel prints
only a byte count, the boot log said "32 bytes loaded" and then `401`, which is
indistinguishable from a typo. Exfiltrating somebody else's credential to a third
party is not an acceptable consequence of a misnamed variable.

The recipe also begins with `set +x +v`, before the key is ever assigned to a
shell variable. On macOS `/bin/sh` is bash in POSIX mode and bash honours an
*inherited* `SHELLOPTS`, so with `export SHELLOPTS=xtrace` in the environment
every command in that recipe was traced to stderr with the value expanded — and
`make run 2>&1 | tee build.log` then put the live key in a log file four times.

`drivers/fwcfg/` reads the blob at boot, strips the trailing newline any `.env`
will have (an unstripped newline in an `x-api-key` header is request splitting,
so a key containing a control character is refused outright), and reports one
line:

```
fwcfg: api key: 108 bytes loaded from fw_cfg (opt/talkos/apikey)
```

A byte count is the *only* thing the kernel will ever say about the key — not a
prefix, not a suffix, not a fingerprint. With no key at all it says so and the
API answers `401`, which is the default developer experience and what the test
suite assumes:

```
fwcfg: no api key (opt/talkos/apikey: not supplied by the host) - requests will
go out unauthenticated and the API will answer 401
```

The remaining exposure is that this kernel has no memory protection and a
`mem_read` tool, so the model can in principle read the key out of RAM. That was
equally true when it lived in `.rodata`; what has changed is that it is no longer
also in five files on disk.

## Layout

```
boot/        boot.asm — multiboot, long mode, .bss zero, paging
kernel/      main.c (terminal REPL + boot self-tests), drivers.c (driver manager)
core/        kobject.c — kernel object base (identity + refcounted lifetime)
mm/          heap.c — coalescing kernel heap (kmalloc/kfree/krealloc + stats)
device/      device.c — generic device model + registry
fs/          vfs/vfs.c (fs-independent VFS core), native/ramfs.c (native FS),
             fs.c (subsystem bring-up + root mount)
lib/         base.c (console/printf/time), libc_shim.c (libc + snprintf +
             mbedtls_hardware_poll entropy), fb.c (framebuffer + clipped drawing),
             font.c + font_spleen8x16.c (977 glyphs, BSD-2-Clause), trace.c
arch/x86_64/ idt.c (256 gates + TSS), isr.asm (the stubs), fault.c (diagnosis)
gui/         wm.c (windows, z-order, focus, damage compositor), widgets.c,
             gui_demo.c (the reference apps), gui_probe.sh (drives a real mouse)
apps/        runtime.c + expr.c — an app is a JSON document, not code;
             examples/calculator.json is the reference artifact
vm/          dvm.c — the bounded driver VM; programs/ac97_boot.c brings up audio
tools/       the model's syscall table, one family per file (wildcarded into the
             build: a new tool needs no Makefile edit)
drivers/
  serial/    COM1 UART debug console, and an input source
  fwcfg/     QEMU fw_cfg — the host->guest channel the API key arrives on
  pci/       PCI config space + enumeration
  acpi/      RSDP/RSDT/XSDT/FADT + \_S5_ from the DSDT; soft-off and reboot
  input/     input.c (sources behind one interface), kbd.c, mouse.c, serial_input.c
  net/       e1000 NIC
  rtc/       CMOS real-time clock
net/         net.c (lwIP netif glue + DNS + HTTPS), chat.c (the turn loop),
             json.c (hardened reader AND writer), model.c/model_mock.c (the
             transport seam and its scriptable fake), sse.c, tls_ca.c
port/        freestanding shims for lwIP/mbedTLS (string/stdlib/stdio/time/
             assert/ctype) + lwipopts.h
lwip/        vendored lwIP 2.2.0 (core + ipv4 + altcp_tls mbedTLS port)
mbedtls/     vendored mbedTLS 2.28.9 (library + headers)
include/     shared headers, each with an architecture comment (kobject/heap/
             device/driver/vfs/fs/tool/trace/json/dvm/gui/widgets/app/chat/
             mouse/acpi/fb/font + mbedtls_config)
tests/       host/ (28 native suites + the shims), qemu/ (boot cases + harness)
linker.ld    1 MiB load, driver_table and tool_table sections
```

The kernel is organised as isolated subsystems behind stable headers
(`kobject.h`, `heap.h`, `device.h`, `driver.h`, `vfs.h`): each has an
architecture comment at the top of its header explaining purpose,
responsibilities, public API, dependencies, and future extension points.
Subsystems talk through those interfaces, not each other's internals — e.g.
an on-disk filesystem later implements `vnode_ops_t` without touching the
kernel API, and another allocator can replace `mm/heap.c` behind `heap.h`.

## The trust model

There is no `ls` here, so the operator cannot independently check anything the
model claims. The console therefore carries two voices and tells them apart by
shape:

```
> put 'talk-os was here' in /etc/motd
Writing that now.                                    <- the model: a claim
[vfs_write /etc/motd mode=overwrite bytes=16 -> ok]  <- the kernel: what happened
Done - /etc/motd now says 'talk-os was here'.        <- the model again
```

**A kernel trace line is a `[` in column zero.** Three things hold that up, and
all three are regression-tested (`tests/host/test_trace.c`,
`tests/host/test_chat.c`, `tests/host/test_vfs_tools.c`):

1. **Model-controlled data cannot forge a line.** `lib/trace.c` escapes every
   control byte, `[` and `]` as `\xNN`, in both the argument field and the
   operation name. A path like `/tmp/x\n[vfs_write -> ok]` cannot break out of
   its own line.
2. **A tool cannot suppress its own line.** `core/tool.c`'s `tool_dispatch()`
   does not trust handlers to log. It forces tracing on across the call,
   restores the caller's setting after, and measures with `trace_emissions()` — a
   monotonic byte counter that `trace_reset()` cannot rewind. If a handler emitted
   nothing, the dispatcher writes the line itself. `rc != TOOL_OK` also forces
   `is_error`, so the kernel's line and the model's `tool_result` cannot disagree.
3. **Model prose cannot reach column zero.** `net/chat.c`'s
   `print_model_prose()` gives a `[` that would land in column zero one leading
   space, and escapes every C0 control byte except `\n` and `\t`.

Point 3 is written the way it is because the first version of it was wrong, in
three ways that are worth knowing if you ever touch it. It modelled the console
(`at_line_start = (c == '\n')`) instead of asking it, and `lib/base.c`'s `kputc()`
also returns the cursor to column zero on `\r`, on `\b` (which from column zero
steps *up* a row, so enough backspaces walk into a genuine trace line already on
screen and overwrite it), and on wrapping past the last column — no newline
involved at all. So prose no longer gets to move the cursor, and the column is
asked for via `console_cursor()` rather than tracked. Do not reintroduce a
newline scan.

What this does **not** claim: nothing here stops the model from lying in prose.
It stops the model from lying *in the kernel's voice*. The operator still has to
read the brackets.

## The tool surface

45 tools, assembled by the linker. A tool self-registers with `REGISTER_TOOL`
(`include/tool.h`) and `tools/*.c` is wildcarded into the build, so **adding a
syscall to the model's surface means dropping in a `.c` file and editing nothing
else at all** — no Makefile line, no central table.

```
filesystem    read_file write_file list_dir stat_path make_dir delete_path
screen        read_screen write_screen screen_info
memory        mem_read mem_write mem_map heap_stats heap_check
devices       device_list device_info device_suspend device_resume driver_*
pci           pci_config_read
clock         time_now time_uptime
driver VM     driver_assemble driver_run driver_trace driver_targets
faults        fault_report fault_diagnose
power         power_info power_off power_reboot
gui           gui_list gui_open gui_window gui_click
apps          app  (launch / state / close / list / format)
agency        plan_set plan_mark plan_show action_log wait_ms
```

Model input is untrusted and is read only through `net/json.c` (a hardened,
fuzzed, ASan-clean reader *and* writer — never write new parsing here). Input
spans are **not** NUL-terminated. Every tool checks types before values, values
before ranges, and ranges before touching memory, and every failure becomes a
legible error the model can recover from on the next turn rather than a fault.

`tools/mem_tools.c` is the quality bar; `include/tool.h` is the spec.

**The schema budget is the thing to watch.** All tool schemas travel in every
request and share `CHAT_TOOLS_BYTES` (40960). At 45 tools the assembled schema is
33,213 bytes. If it ever does not fit, `net/chat.c` refuses the whole array and
the model is offered **no tools at all** — the machine keeps booting and silently
stops being able to act. `tests/qemu/cases/boot.case` asserts the size so that
fails loudly instead, but anyone adding a tool family should measure first.

## The GUI

The **console is the desktop.** Windows composite over the text; the window
manager re-rasterises console cells from `console_fb()` with the same font and
palette and floats windows above them. `lib/base.c` is untouched and does not
know the GUI exists — which is the point: the console must still work when the
GUI does not, on a machine whose only way to report a problem is words.

Nothing repaints because time passed. Every state change names the rectangle it
invalidated, and `gui_tick()` — called from the one loop this machine has, in
`kernel/main.c` — paints only that. With no window open it does nothing at all, so
a machine that never opens a window is the machine that existed before the GUI.

Two rules keep the two interfaces from fighting:

* **The keyboard belongs to the prompt.** A click on a text field borrows it for
  exactly one line, then hands it back. Because `gui_click` is a tool, the *model*
  can arm that grab too, so the prompt itself says where your next line is going:
  `[typing into "Notes", not to the model] > `. A widget that can silently swallow
  the operator's instruction is a control-flow hazard on a machine with no shell.
* **The pointer belongs to the GUI.** The keyboard and the mouse share one
  one-byte i8042 output buffer and are told apart only by AUX status bit 5, so
  both drivers check it and `drivers/input/kbd.c` hands an aux byte to
  `mouse_accept_byte()` rather than decoding it. Without that, mouse motion was
  decoded as scancodes — packet byte 1 is `0x08|buttons|signs`, and a delta of
  `0x1C` is Enter — so moving the mouse typed garbage at the prompt and SUBMITTED
  it to the model, while the pointer itself received nothing.

`gui/gui_probe.sh <plan>` drives the running kernel from outside: real PS/2 motion
and clicks through the QEMU monitor, real sentences down a serial socket, and
screenshots. That is how the click path is proven against hardware rather than
asserted.

## Apps are documents, not code

An app is a JSON document: a widget tree plus handlers written in a tiny total
expression language. It arrives through the same hardened `net/json.c` that reads
everything else the model sends, is validated **completely before a single pixel
exists**, and is then run by a bounded evaluator that cannot loop, allocate, fault,
or reach anything outside its own window.

```json
{"title":"Counter","width":180,"height":120,
 "grid":{"rows":2,"cols":1},"vars":{"n":0},
 "widgets":[{"kind":"field","name":"out","text":"0","readonly":true,"row":0,"col":0},
            {"kind":"button","text":"+1","name":"up","row":1,"col":0}],
 "on":[{"click":"up","do":[{"set":"n","to":"n + 1"},
                           {"set":"out","to":"text(n)"}]}]}
```

Numbers are fixed point (int64 scaled by 1e6, `__int128` intermediates) because
there is no FPU here. **Errors are a value, not a trap**: `1/0`, overflow and
`num("abc")` all produce `ERR`, `ERR` propagates, and `text(ERR)` is the word
`error` — which is how the reference calculator latches `error` with no error
handling in the runtime at all, and why no handler can ever die half-done leaving
a display holding a lie. A rejected document leaves no window, no widgets and no
slot, and the refusal names the JSON path, the reason, the byte offset inside an
expression, and what *would* have been accepted.

`apps/examples/calculator.json` is the reference artifact — 4 KB, hand-written,
and the thing the live transcript in the top-level README is clicking.

It is deliberately not a general language: no loops, no user functions, no arrays,
no timers, no way for an app to call a tool, read the clock, touch memory or
print. Keeping the console unreachable from an app is also what stops
model-authored text from ever putting a `[` in column zero.

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

## Known weaknesses and things nobody has tested

Stated plainly, because this is a hobby kernel going public and the interesting
part is where it stops.

**No isolation whatsoever.** One CPU, ring 0, single-threaded, no userspace, no
scheduler, no memory protection. Every tool call runs in the kernel's own address
space and finishes before the next one starts. All pages are mapped
read-write-EXECUTE and NX is never set — the `LOAD segment with RWX permissions`
link warning is **expected and intentional**, do not "fix" it. A null dereference
does not fault, because the low 4 GiB is mapped writable and page 0 shares its
2 MiB huge page with the video framebuffer; `0x1000000000` is the address to use
for a guaranteed page fault. `mem_write` and the driver VM are real capabilities
handed to a language model, bounded only by checks inside each tool.

**The model can read the key out of RAM.** No memory protection plus `mem_read`
means the `x-api-key` header sitting in `net.c`'s request buffer is reachable. The
runtime injection work removed the key from five files on disk; it did not and
cannot make RAM opaque.

**Certificate verification is off by default** (see the caveat above), so in a
default build "the model" is whatever answers on 443. Everything a 4xx/5xx
response prints is therefore treated as hostile and escaped, including the raw
body — a `[` from the far end gets shifted out of column zero and a `\b` is
rendered as `\x08`, because backspaces from column zero walk *up* into genuine
trace lines and overwrite them.

**Never booted on physical hardware.** Everything here is QEMU. Several things
would probably break on real silicon: e1000 MMIO goes through a write-back
cacheable identity map with no PAT or MTRR, `fw_cfg` does not exist outside QEMU
(so a machine booted from `talkos.iso` gets no key and 401s), and the framebuffer
is obtained either from a multiboot tag or from the Bochs/QEMU VBE dispi
registers. The pinned TLS roots also expire: 2036, and the endpoint's CA may
change long before that.

**Interrupts are never enabled.** IF stays 0, all 16 PIC lines are masked, and
everything is polled. An IDT exists and captures faults, which is why
`fault_report` can say anything useful, but no device interrupt is serviced. The
consequence a user sees: `chat_ask()` blocks for as long as the model takes, and
during a turn the GUI does not repaint and mouse bytes are lost to the one-byte
i8042 buffer. Nothing is corrupted — the decoder resynchronises — but a click
during a turn does not happen. The fix is one line (`net_service()` should call
`gui_tick()`) and has not been made.

**`kmalloc` panics instead of returning NULL.** That makes every `ENOSPC` path in
`fs/` and `tools/` unreachable today, and it means a large enough model-driven
write halts the machine rather than returning an error. The mitigation is that
sizes are bounded before allocation (ramfs caps a file at 1 MiB), not that the
allocator is failable.

**No DHCP.** The IP, gateway and DNS server are hardcoded QEMU user-mode defaults
(`10.0.2.15/24`).

**Untested, honestly:** the XSDT/ACPI-2.0 path (QEMU always publishes a revision-0
RSDP, so only the host suite exercises it); IMEX five-button mice; the `console=vga`
fallback under a live model; long sessions and context eviction; anything about
how a model behaves when the tool surface is exercised adversarially. The GUI's
`gui_click` resolves widgets by visible label, so a document with two buttons
sharing a label leaves one unreachable that way.

**Two known defects deliberately left in place, with reasons.**

*A model's `gui_click` on a text field still arms the keyboard grab.* The grab is
the intended mechanism — "click the box so I can type into it" is a real flow, and
the tool and a physical click go through one dispatch path on purpose — but it
means a model that clicks a field for its own reasons owns the operator's next
sentence. The fix applied is visibility, not prevention: the prompt now reads
`[typing into "Notes", not to the model] > `, so the operator cannot be surprised
about where their words went. Actually removing the arm on synthesised clicks
would leave the model no way to fill a field at all, since no tool sets widget text.

*`net_service()` still does not call `gui_tick()`.* That one line would stop the
GUI freezing for the length of a model round-trip, and it is tempting. It is not
applied because `gui_tick()` DISPATCHES events, and dispatching from inside a turn
can close a window a running tool is holding a pointer into — which is exactly why
`gui_sync()` (repaint only, no dispatch) exists and is what the tools call. Doing
this properly means a repaint-only tick in the transport, not the obvious line.

**The reduced `vsnprintf`.** `lib/libc_shim.c` implements no `%*d` star-width, and
`lib/base.c`'s `kprintf` has a narrower grammar still (no `l` modifier, and an
unknown conversion does not consume its vararg). `tests/host/kshim.c` now checks
kernel format strings against that grammar and fails the suite on a violation,
because a shipped `%lu` had already cost real time.
