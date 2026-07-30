# fable-os

A from-scratch x86_64 kernel whose **only human interface is a sentence**. There
is no shell, no commands, no `ls` and no `cat` — there is not one `strcmp` on the
input path. You type what you want done and the kernel acts on the machine.

The kernel does its own DNS and its own TLS, in ring 0, with mbedTLS and lwIP
compiled into `kernel.bin` and no host proxy of any kind, and it gives the model
**64 real syscalls** into itself: a VFS, a FAT32 disk, the device tree, PCI config
space, physical memory, the framebuffer console, a window manager, an app runtime,
the CMOS clock, a bounded driver VM, **a C compiler that emits native x86-64 at run
time**, HTTP(S) fetch, ACPI power control, a registry of capabilities it has taught
itself, a schedule of things it does unprompted, and the ability to rewrite its own
running machine code.

It is also **awake**. It keeps what it builds across a power cycle, paints the
screen while it is thinking, acts at boot with nobody at the keyboard, and — when
an unattended action takes a fatal CPU fault — survives it, asks the model what
happened, and patches the faulting instruction out of the running kernel.

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

And this is two boots, on the same disk, with the machine switched off in between —
which is the whole of what "awake" means here:

```
> I need to test whether a number is a perfect square. Make that something this
  machine can just do from now on, and check 10201 for me.
I'll build this as a saved capability so it persists across reboots. Let me first
check what's already saved.
[cap.list -> 0]
... (writes a bisection square root, finds its own infinite loop, fixes it) ...
[cap.save is_perfect_square program -> 1]
[cap.call is_perfect_square v1 depth=0 -> 1]
Yes, 10201 is a perfect square (101²), and this machine now has a saved capability
called is_perfect_square(n) that returns 1 or 0 and survives reboots.

--- power off, power on ---
capability: 1 loaded from /disk/cap (survives a reboot)

> Is 46656 a perfect square?
[cap.call is_perfect_square v1 depth=0 -> 1]
Yes — 46656 is a perfect square (216² = 46656).
```

The second boot writes no program and assembles nothing. It read the capability
off a FAT32 disk this kernel formatted, saw it advertised in its own tool schema,
and called it. With **no key at all** and nobody typing, a scheduled item runs the
same capability before the prompt is printed:

```
agenda: 1 item(s) loaded from /disk/agenda.json (survives a reboot)
[agenda.run selfcheck boot tool runs=1 -> ok]
[cap.call is_perfect_square v1 depth=0 -> 1]
[agenda.done selfcheck ok -> 1]
agenda: 1 boot item(s) ran before anyone typed anything
```

Two things it is important to be straight about up front:

* **The model's replies cannot be trusted, and the kernel says so structurally.**
  Prose is the model. `[bracketed lines]` are the kernel, printed from C after
  the call returned. On a machine with no `ls` that distinction is the only audit
  trail there is, so it is enforced rather than promised — see
  "The trust model" in `os/README.md`.
* **A live model has driven tools, but almost nothing is *proven* that way.** The
  transcripts here are a handful of turns. What holds the tools up is millions of
  host assertions (`make test-host` — 39 suites, 9.6M assertions, also green under
  ASan + UBSan) against a scripted transport (`os/net/model_mock.c`) and six QEMU
  boot tests — not a live model's judgement, which nobody has characterised here at
  any scale. Assume the failure modes of the 64-tool surface under an adversarial
  or confused model are unexplored, because they are.
* **There IS a C compiler now, and it is the sharpest thing here.** `os/compiler/`
  turns C out of a model message into native x86-64 and calls it, which works only
  because every page on this machine is executable. Compiled code is bounded three
  ways — unforgeable fuel at every loop back-edge and call, an `rsp` check on its
  own private stack, and a subset with no function pointers so every call target is
  a compile-time constant. The boundary is a **twelve-entry symbol table of
  forwarders**, and nothing bounds a *pointer*: a wild store from compiled code
  halts the machine. There is still no ELF loader and no way to run code built
  somewhere else, deliberately.

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

**The key is not a build input.** There is no `KEY=` and no `-DFABLEOS_API_KEY`;
`make KEY=...` is a hard error. `os/.env` is gitignored and is the only file that
holds it. `make run` hands it to the guest at boot over QEMU's `fw_cfg` channel,
via a mode-0600 temp file outside the repository that is deleted however the run
ends, so no build artifact — not `net.o`, not `kernel.bin`, not `fableos.iso` —
can contain it. The kernel reports how many bytes arrived and never the bytes.
See [`os/README.md`](os/README.md#the-api-key-never-enters-the-build).

## ⚠️ Security caveat

This is a hobby OS. **In the default build, TLS certificate verification is off**
(`MBEDTLS_SSL_VERIFY_NONE`): the server's certificate is parsed and then
believed, so traffic is encrypted but **not authenticated** and is MITM-able by
anything that can answer on port 443. Don't send anything sensitive.

Real verification is implemented and works, behind a build flag:

```sh
cd os && make EXTRA_CFLAGS=-DFABLEOS_VERIFY_CERTS
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
CPU, ring 0, no userspace, no privilege separation, no memory protection, no IOMMU.
Every tool call runs in the kernel's own address space, and all pages are mapped
read-write-EXECUTE (the `LOAD segment with RWX permissions` link warning is expected
and intentional — it is also what makes generated code callable). A null dereference
does not even fault, because the low 4 GiB is mapped writable and page 0 shares its
huge page with the video framebuffer. The model is handed `mem_read`, `mem_write`, a
driver VM that can touch I/O ports and program DMA, a C compiler whose output runs
as kernel code, a writable disk, a schedule that runs unprompted, and a tool that
edits the running kernel's `.text`. The boundaries are the VM's policy, the argument
validation in `apps/cap.c`, five gates on a code patch, and — for compiled code —
**a twelve-entry symbol table of purpose-written forwarders**, reachable only
because the subset admits no function pointers so the call graph is fixed before
the first instruction. All of that is bounds checks and allowlists inside each
tool, not hardware. A model that is wrong in the right place can corrupt the
machine.

Two specific consequences, both of which were real defects found by reviewing this
branch rather than hypotheticals:

* **Anything with write access to the disk has write access to this kernel.** The
  capability store, the schedule and the boot log are files the model itself can
  write. A scheduled `power_off` at boot made the machine permanently unbootable
  with no interface left to countermand it; that specific case is now refused both
  when dictated and when read off a disk. So did a scheduled action that *crashed*,
  which the name denylist could never cover — a boot item now writes an
  "attempting" marker to the store before it runs and any boot that finds the
  marker still set disables that item and says why. The class remains.
* **A compile could kill the machine outright, and did.** About 2 KB of legal C in
  a model message walked the compiler's own recursion off the 64 KiB boot stack
  into the identity-map page tables: triple fault, no fault record, nothing on the
  serial line, and nothing the self-repair loop can reach. It is now bounded by a
  measured stack floor and the root stack has a poison band, but the lesson
  generalises — a depth counter is a proxy for stack use, and the constant relating
  the two had gone stale twice.
* **The key is readable from RAM and cannot be made otherwise here.** The outbound
  request scanner refuses to send anything key-shaped, which stops it leaving in one
  piece and stops nothing else — no content filter stops a covert channel.

Do not run this on anything you care about, and do not expose it to input you did
not type.

See `os/README.md` for the driver model, the trust model, the GUI, the app format,
the TLS build, and what is and is not tested.
