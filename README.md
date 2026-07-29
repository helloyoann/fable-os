# talk-os

A from-scratch x86_64 kernel whose **only human interface is a sentence**. There
is no shell, no commands, no `ls` and no `cat` — there is not one `strcmp` on the
input path. You type what you want done and the kernel acts on the machine.

The kernel does its own DNS and its own TLS, in ring 0, with mbedTLS and lwIP
compiled into `kernel.bin` and no host proxy of any kind, and it gives the model
**45 real syscalls** into itself: a VFS, the device tree, PCI config space,
physical memory, the framebuffer console, a window manager, an app runtime, the
CMOS clock, a bounded driver VM, and ACPI power control.

It really does work end to end. With a key in `os/.env`:

```
> I want a calculator on screen please
I'll open a calculator window for you.
[gui_open app=calculator at=40,48 size=216x266 widgets=18 -> 1]
[gui_list windows=1 focus=1 detail=1 -> 1]
The calculator is open and on screen (window id=1), with its display and full
keypad of buttons ready to click. Note it lives in RAM, so it's gone on the next
reboot.

> now use that calculator to work out 7 plus 8, by clicking its keys, and tell me
  what its display reads
[gui_click id=1 widget=107 label=7 at=70,130 -> 1]
[gui_click id=1 widget=5 label=+ at=225,249 -> 1]
[gui_click id=1 widget=108 label=8 at=121,130 -> 1]
[gui_click id=1 widget=3 label== at=148,289 -> 1]
The display reads 15.

> in one word, what is the capital of France?
Paris
```

That is one real boot: the `[bracketed]` lines are this kernel printing what it
dispatched, and a screenshot of that session shows the calculator's display
reading `15` with the text console composited around it.

Two things it is important to be straight about up front:

* **The model's replies cannot be trusted, and the kernel says so structurally.**
  Prose is the model. `[bracketed lines]` are the kernel, printed from C after
  the call returned. On a machine with no `ls` that distinction is the only audit
  trail there is, so it is enforced rather than promised — see
  "The trust model" in `os/README.md`.
* **A live model has driven tools, but almost nothing is *proven* that way.** The
  transcript above is a handful of turns. What holds the tools up is 9,143,897
  host assertions (`make test-host`, also green under ASan + UBSan) against a
  scripted transport (`os/net/model_mock.c`) and four QEMU boot tests — not a live model's judgement, which nobody has characterised
  here at any scale. Assume the failure modes of the 45-tool surface under an
  adversarial or confused model are unexplored, because they are.

Built and run on macOS via QEMU. Never booted on physical hardware.

## Layout

| Directory | What it is |
|-----------|------------|
| **`os/`** | The bare-metal OS. Boot, an IDT, drivers (serial, PCI, fw_cfg, PS/2 keyboard and mouse, e1000 NIC, CMOS RTC, ACPI power), a framebuffer console, a window manager and app runtime, a bounded driver VM, lwIP + mbedTLS, and the turn loop. **Build and run from here.** |

## How it works

The kernel does TLS itself — mbedTLS (vendored) wired through lwIP's `altcp_tls`
layer — so it talks to `api.anthropic.com:443` with no host proxy:

```
your sentence ──▶ turn loop ──▶ DNS ──▶ TLS 1.2 (mbedTLS) ──▶ api.anthropic.com
                   ▲     │                                            │
                   │     └──── tool_use ──▶ tool_dispatch() ──▶ the real machine
                   │                              │            (VFS, PCI, heap,
                   └──── tool_result ─────────────┘             screen, power)
```

A turn is not one request. The model may call tools, see the results, and call
more, up to 16 rounds. The kernel tells the model how much budget is left in
every round, sends the final round with tool calling switched off so the cap ends
in a report rather than a rollback, and retries a transient 429/503 without
spending a round. Every dispatch prints exactly one kernel-written `[bracketed]`
line that the tool cannot suppress, and the kernel keeps its own journal of what
it dispatched so an abandoned turn can be resumed rather than restarted.

## Quick start

```sh
cd os
make toolchain                       # once: nasm, x86_64-elf-gcc, qemu
cp .env.example .env                 # then put your Anthropic key in it
make run                             # build + boot in QEMU
```

Type a sentence at the `>` prompt and press Enter. At boot it also runs one
self-test request so you can see the HTTPS round-trip on the serial log.

Without a key (just `make run` with no `.env`), the kernel still completes the
TLS handshake and the API returns `401` — which proves HTTPS works, just without
an answer.

**The key is not a build input.** There is no `KEY=` and no `-DTALKOS_API_KEY`;
`make KEY=...` is a hard error. `os/.env` is gitignored and is the only file that
holds it. `make run` hands it to the guest at boot over QEMU's `fw_cfg` channel,
via a mode-0600 temp file outside the repository that is deleted however the run
ends, so no build artifact — not `net.o`, not `kernel.bin`, not `talkos.iso` —
can contain it. The kernel reports how many bytes arrived and never the bytes.
See [`os/README.md`](os/README.md#the-api-key-never-enters-the-build).

## ⚠️ Security caveat

This is a hobby OS. **In the default build, TLS certificate verification is off**
(`MBEDTLS_SSL_VERIFY_NONE`): the server's certificate is parsed and then
believed, so traffic is encrypted but **not authenticated** and is MITM-able by
anything that can answer on port 443. Don't send anything sensitive.

Real verification is implemented and works, behind a build flag:

```sh
cd os && make EXTRA_CFLAGS=-DTALKOS_VERIFY_CERTS
```

That requires the chain to build to one of two **pinned** Google Trust Services
roots, requires the certificate to name `api.anthropic.com`, and enforces
`notBefore`/`notAfter` against the CMOS real-time clock. It is opt-in because a
pinned trust set eventually breaks: when the endpoint's CA changes or the roots
rotate, the kernel simply cannot reach the API until it is rebuilt. There is
also no revocation checking (no CRL, no OCSP).

**Entropy is a separate weakness and is not fixed by any of the above.** The
CTR_DRBG is seeded by `mbedtls_hardware_poll()` — RDRAND, or a TSC mix when
RDRAND is absent — with no entropy accounting and no health tests. Functional,
not a vetted CSPRNG.

**There is no isolation of any kind, and that is the largest caveat here.** One
CPU, ring 0, no userspace, no scheduler, no memory protection: every tool call
runs in the kernel's own address space, and all pages are mapped read-write-EXECUTE
(the `LOAD segment with RWX permissions` link warning is expected and intentional).
A null dereference does not even fault, because the low 4 GiB is mapped writable
and page 0 shares its huge page with the video framebuffer. The model is handed
`mem_read`, `mem_write` and a driver VM that can touch I/O ports, so a model that
is wrong in the right place can corrupt the machine — and can in principle read
the API key out of RAM. The mitigations are bounds checks, allowlists and cycle
limits inside each tool, not hardware. Do not run this on anything you care
about, and do not expose it to input you did not type.

See `os/README.md` for the driver model, the trust model, the GUI, the app format,
the TLS build, and what is and is not tested.
