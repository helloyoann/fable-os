# talk-os

A from-scratch x86_64 operating system that boots straight into a **minimal AI
terminal**: type a question on the keyboard, and the kernel sends it to the
Anthropic Claude API **directly over HTTPS** and prints the reply. No graphics,
no audio — just a keyboard, a text console, a network stack, and TLS. Built and
run on macOS via QEMU.

## Layout

| Directory | What it is |
|-----------|------------|
| **`os/`** | The bare-metal OS. Boot + drivers (serial, PCI, PS/2 keyboard, e1000 NIC) + lwIP networking + mbedTLS + the AI terminal. **Build and run from here.** |

## How it works

The kernel does TLS itself — mbedTLS (vendored) wired through lwIP's `altcp_tls`
layer — so it talks to `api.anthropic.com:443` with no host proxy:

```
keyboard ──▶ kernel terminal ──▶ DNS ──▶ TLS 1.2 (mbedTLS) ──▶ api.anthropic.com
                    ▲                                                  │
                    └──────────────── reply text ──────────────────────┘
```

## Quick start

```sh
cd os
make toolchain                       # once: nasm, x86_64-elf-gcc, qemu
make KEY=sk-ant-... run              # build with your API key + boot in QEMU
```

Type a question at the `you>` prompt and press Enter. At boot it also runs one
self-test request so you can see the HTTPS round-trip on the serial log.

Without a key (`make run`), the kernel still completes the TLS handshake and the
API returns `401` — which proves HTTPS works, just without an answer.

## ⚠️ Security caveat

This is a hobby OS. TLS certificate verification is **disabled**
(`MBEDTLS_SSL_VERIFY_NONE`): traffic is encrypted but **not authenticated**, so
it is vulnerable to man-in-the-middle. Entropy comes from a simple hardware poll
(RDRAND/TSC), not a vetted CSPRNG. Don't send anything sensitive.

See `os/README.md` for the driver model, the TLS build, and details.
