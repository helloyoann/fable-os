/* dvm.h — the driver VM: a bounded machine the model can program.
 *
 * PURPOSE
 *   The end goal is a kernel that writes a device driver at runtime for hardware
 *   it has never heard of. The obvious way to do that — ask the model for x86-64
 *   machine code and jump to it — is the wrong way twice over: models are
 *   unreliable at raw encoding, and the failure mode is a silent triple fault
 *   with no story. There is nothing to read afterwards and nothing to blame.
 *
 *   So the model does not get the CPU. It gets THIS machine: a small register VM
 *   whose opcodes are exactly the primitives a bring-up sequence needs — port
 *   I/O, MMIO, PCI config reads, delays, compare-and-branch — and nothing else.
 *   There is no instruction that computes an address the VM does not check, no
 *   instruction that reaches kernel memory, and no instruction that runs forever.
 *   Every access is checked against an allowlist the CALLER supplies, and every
 *   instruction can be printed as a kernel-authored trace line.
 *
 *   That trade buys three things a native-code path cannot give:
 *     1. Containment. A confused program is refused, not fatal.
 *     2. A transcript. A failed bring-up leaves an exact list of the ports and
 *        registers it touched, in order, with the values it read back.
 *     3. Separability. When a program does not work you can tell whether the VM
 *        executed it wrongly or the program was wrong, because the VM's
 *        semantics are unit-tested on the host against a synthetic device.
 *
 * THE MACHINE
 *   16 general registers (r0-r15), 64-bit, unsigned. r0.. are preloaded from the
 *   caller's argument vector, which is how a program is handed the BAR address
 *   of the device it is supposed to drive instead of hardcoding one.
 *
 *   A data stack (PUSH/POP, 32 deep) and a separate call stack (CALL/RET, 16
 *   deep). Overflow or underflow of either is a trap, not a wrap.
 *
 *   Two flags, set by CMP and TEST, read by the six conditional branches.
 *   Comparisons are UNSIGNED: these are hardware values, not arithmetic.
 *
 *   No general writable data memory. Device registers, the stack, and ONE
 *   caller-granted DMA scratch buffer (below) are the only mutable state a
 *   driver program has. Leaving out a heap removes an entire class of
 *   containment problem.
 *
 * THE DMA SCRATCH BUFFER — the one memory grant, and what it really costs
 *   A driver that only pokes registers can reset a codec. It can never play a
 *   sound, because every DMA engine on the machine is told where to work by
 *   being handed a PHYSICAL ADDRESS, and a machine with no memory has no
 *   address to give. So the VM has exactly one memory region:
 *
 *       dvm_dma_region()          -> the arena this module owns, once, forever
 *       dvm_policy_allow_dma()    -> grant all or part of it to one run
 *       on entry: DVM_ARG_DMA_BASE and DVM_ARG_DMA_SIZE hold base and bytes
 *       ld8/ld16/ld32, st8/st16/st32 reach it, exactly as they reach MMIO
 *
 *   The arena is a single static, page-aligned DVM_DMA_SIZE-byte array inside
 *   vm/dvm.c. It is not the heap, not the stack, and not part of any other
 *   subsystem's memory; the low 4 GiB is identity-mapped, so its virtual
 *   address IS its physical address and a 32-bit bus master can reach it.
 *   dvm_policy_allow_dma() refuses any range that is not a subrange of THAT
 *   array — the caller supplies no address of its own, so a caller bug cannot
 *   open a window on kernel memory either. Accesses to it are bounds-checked,
 *   alignment-checked and counted against their own budget (max_dma_ops), kept
 *   separate from max_io so that filling a second of PCM does not consume the
 *   device-access budget a bring-up needs.
 *
 *   THE HONEST PART. This is a real escalation of privilege and there is no
 *   point pretending otherwise. The VM bounds what the PROGRAM touches. It
 *   cannot bound what the HARDWARE does once the program has written an address
 *   into a device's DMA register. There is no IOMMU on this machine. A device
 *   handed a wrong physical address will DMA over kernel memory, and no
 *   instruction check anywhere in this file can see it happen.
 *
 *   What is actually mitigated:
 *     - only the ONE named target gets PCI bus mastering, and the C dispatcher
 *       (tools/dvm_tools.c) sets that bit, not the program: there is still no
 *       config-space WRITE in the ISA;
 *     - the program is told exactly one valid buffer address and one size;
 *     - the arena is its own object, far from the heap's live blocks, so a
 *       device that overruns the buffer by a little damages only scratch;
 *     - every value the program writes into a device register appears in the
 *       access log, so the address it handed the device is on the record.
 *
 *   What is NOT mitigated: a determined program can compute any 32-bit number
 *   and write it into a device's descriptor-base register. Nothing here stops
 *   it. That is the same trust every real kernel extends to a driver it loads,
 *   and it is the reason the buffer is granted per-run by a C caller that names
 *   the device, rather than being a standing capability.
 *
 * THE INSTRUCTION SET (assembler syntax; `a`/`b` are register-or-immediate)
 *
 *   control     halt                     stop, status DVM_OK
 *               abort "why"              stop, status DVM_TRAP_ABORT
 *               nop
 *               jmp L                    unconditional branch
 *               beq/bne/blt/ble/bgt/bge L   branch on the flags from cmp/test
 *               call L / ret             bounded subroutine call
 *               push a / pop rd          bounded data stack
 *
 *   data/ALU    mov rd, a
 *               add/sub/mul/div/mod rd, a, b
 *               and/or/xor/shl/shr  rd, a, b
 *               not rd, a
 *               cmp a, b                 sets zero + below (unsigned)
 *               test a, b                sets zero = ((a & b) == 0)
 *
 *   port I/O    out8/out16/out32 port, a
 *               in8/in16/in32   rd, port
 *
 *   MMIO +      ld8/ld16/ld32 rd, [base+disp]
 *   DMA buffer  st8/st16/st32 [base+disp], a
 *
 *   PCI         pcicfg rd, bdf, off      one 32-bit config-space READ
 *
 *   time        delay a                  microseconds (see DELAY below)
 *
 *   log         print "text"
 *               print "text", a          appends "= 0x..." with a's value
 *
 * SOURCE SYNTAX
 *   One instruction per line. Comments run from `;`, `#` or `//` to the end of
 *   the line (so `#5` is a comment, not an ARM-style immediate). Commas between
 *   operands are optional. Mnemonics, register names and labels are
 *   case-insensitive. A label is `name:` on its own line or before an
 *   instruction. Numbers may be decimal, 0x hex, 0b binary, negative, or contain
 *   `_` separators. `.equ NAME value` (also spelled `.def` / `.set`) defines a
 *   constant usable anywhere a number is, which is how a program says
 *   `AC97_RESET` instead of `0x2c`.
 *
 *   Two-operand ALU forms are accepted as shorthand (`add r1, 4` means
 *   `add r1, r1, 4`), the address of an MMIO op may be written `[r1+0x10]`,
 *   `[r1]`, `[0xfebc0000]` or as two plain operands, and a PCI bdf may be
 *   written the way pci_list prints it (`00:03.0`). The assembler is forgiving
 *   on purpose: the model writes this text, and a rejected program costs a turn.
 *
 * CONTAINMENT — what a hostile or confused program CANNOT do
 *   Nothing is reachable by default. dvm_policy_init() produces a deny-all
 *   policy and the caller opens exactly the windows the target device needs:
 *
 *       dvm_policy_t pol;
 *       dvm_policy_init(&pol);
 *       dvm_policy_allow_ports(&pol, 0xC000, 0xC03F);          // this BAR only
 *       dvm_policy_allow_mmio (&pol, 0xFEBC0000, 0x20000, DVM_MMIO_RW);
 *       dvm_policy_allow_pci  (&pol, 0, 4, 0);                 // one function
 *       dvm_policy_allow_dma  (&pol, dma_base, dma_size);      // the scratch arena
 *
 *   On top of the allowlist there is a DENY list compiled into vm/dvm.c that no
 *   caller can override: the PICs, the PIT (millis() is calibrated against it),
 *   the PS/2 controller (port 0x64 can pulse the CPU reset line), CMOS/NMI, the
 *   fast-A20/reset port, the PCI config address/data ports (writes there move a
 *   live device's BAR), the serial console (the only place this trace is
 *   printed), and all of low memory up to the end of the kernel image — which is
 *   also what keeps the program off the VGA text buffer at 0xB8000, since page 0
 *   and the framebuffer share a 2 MiB huge page and cannot be unmapped. MMIO
 *   above 4 GiB is refused too: only the low 4 GiB is mapped, so an access there
 *   would be a page fault rather than a refusal.
 *
 *   The deny list is applied twice: once when the run starts, so a policy that
 *   overlaps it is refused outright with a message naming the conflict, and
 *   again on every access, so a bug in policy validation cannot become a poke at
 *   the PIC.
 *
 *   The DMA arena is the ONE exception to "all memory below the kernel top is
 *   denied", and it is not a hole in that rule so much as a different object:
 *   the address is not supplied by the caller at all. dvm_policy_allow_dma()
 *   compares the request against this module's own static array and refuses
 *   anything else, so the only way to widen the grant is to edit vm/dvm.c.
 *
 *   Everything else is a budget, all of them enforced per run:
 *     max_steps      instructions executed  (this is the cycle limit; a program
 *                    that loops forever stops here with DVM_TRAP_STEPS)
 *     max_io         port + MMIO + PCI accesses (NOT DMA-buffer accesses)
 *     max_dma_ops    ld/st into the DMA scratch buffer
 *     max_delay_us   cumulative microseconds spent in DELAY
 *     max_single_delay_us  the largest one DELAY may request
 *     max_prints     PRINT lines
 *     max_trace      trace lines, after which tracing goes quiet (once, loudly)
 *
 *   And the structural bounds, checked by the assembler and re-checked by
 *   dvm_program_validate() before every run so a corrupted or hand-built program
 *   cannot escape either: every opcode known, every register index < 16, every
 *   branch target inside the program, every string index inside the pool.
 *
 * MMIO READS HAVE SIDE EFFECTS
 *   Many device registers are clear-on-read. That is why a read needs an
 *   allowlist at all, why DVM_MMIO_R and DVM_MMIO_W are separate bits (a caller
 *   can hand out a read-only window), and why the window granularity is a BAR
 *   rather than "all MMIO".
 *
 * DELAY
 *   The operand is MICROseconds. The hardware backend spends the whole-
 *   millisecond part in mdelay() and the remainder in a TSC spin calibrated once
 *   against millis(); if that calibration cannot be trusted the remainder
 *   degrades to a short bounded spin. So DELAY is a floor with millisecond-grade
 *   accuracy, not a precise timer. Bring-up code should poll with a counted
 *   retry loop and treat DELAY as backoff, which is also what makes a program
 *   deterministic under test.
 *
 * PUBLIC API
 *   dvm_assemble()          text -> dvm_program_t, with line-numbered errors
 *   dvm_program_validate()  structural re-check (run does this itself)
 *   dvm_disasm()            one instruction back to text, for listings
 *   dvm_policy_init()       deny-all policy + default budgets
 *   dvm_policy_allow_*()    open one port range / MMIO window / PCI function
 *   dvm_dma_region()        the one DMA scratch arena this module owns
 *   dvm_policy_allow_dma()  grant all or part of it to one run
 *   dvm_run()               execute; returns dvm_status_t, fills dvm_result_t
 *   dvm_status_name()       symbolic status for a message or a trace line
 *   dvm_io_hardware()       the real-hardware backend (NULL on a host build)
 *
 * DEPENDENCIES
 *   trace.h for the execution trace, and the libc shim's snprintf. No heap: the
 *   VM allocates nothing, ever, and the caller owns the dvm_program_t. Hardware
 *   is reached only through dvm_io_t, which is why the whole ISA is testable on
 *   a host against a synthetic device (tests/host/test_dvm.c).
 *
 * SIZE NOTE
 *   The DMA arena adds DVM_DMA_SIZE bytes of .bss to any kernel that links this
 *   file. It is static and always present, because a buffer that is sometimes
 *   there is a buffer no program can rely on.
 *
 *   dvm_program_t is ~12 KB (256 instructions plus the label and string tables).
 *   The kernel stack is 64 KiB and shared with lwIP and mbedTLS — make one
 *   static or kmalloc it, do not put it on the stack. dvm_policy_t (~300 bytes)
 *   and dvm_result_t (~400 bytes) are stack-sized on purpose.
 *
 * FUTURE EXTENSION POINTS
 *   - The tool wrapper (next phase) hands the model dvm_assemble + dvm_run with
 *     a policy derived from the target device's BARs, so the model can never
 *     name a window the device does not own.
 *   - A program that completes successfully is a driver: keeping the assembled
 *     dvm_program_t and re-running its "service" entry point on demand is how a
 *     model-authored driver becomes persistent. Entry-point support wants one
 *     more field here (a label -> pc map is already retained for listings).
 *   - The deny list is deliberately not policy-overridable. If a legitimate use
 *     ever needs the PS/2 controller, the right change is an explicit unsafe
 *     opt-in flag with its own trace line, not a hole in the table.
 *   - Writes to PCI config space are absent for the same reason dev_tools.c
 *     refuses them; if they are ever added they belong behind a per-offset
 *     allowlist plus a restore path, as a new opcode with its own budget. The
 *     ONE config write a DMA driver needs — the bus-master bit — is done by the
 *     C dispatcher instead (tools/dvm_tools.c), which is why the ISA still has
 *     no way to move a live BAR.
 *   - An IOMMU (VT-d) is the only thing that would turn the DMA grant from
 *     trust into containment: a domain whose only mapping is the arena would
 *     make a wrong descriptor address a fault instead of corruption. Until then
 *     the honest description of the DMA path is "trusted driver", not "sandbox".
 *   - A second concurrent grant (two devices DMAing out of disjoint halves of
 *     the arena) needs the policy to carry a list rather than one range. The
 *     single-range shape is deliberate while exactly one program runs at a time.
 */
#ifndef DVM_H
#define DVM_H

#include <stdint.h>
#include <stddef.h>

/* ---- machine + program limits (all enforced, all testable) ---- */
#define DVM_NREGS            16     /* r0 .. r15                              */
#define DVM_MAX_INSNS        256    /* instructions per program               */
#define DVM_MAX_LABELS       64
#define DVM_LABEL_MAX        24     /* label/constant name length, incl. NUL  */
#define DVM_MAX_STRINGS      32
#define DVM_STRPOOL          512    /* bytes of string literal storage        */
#define DVM_MAX_EQU          32     /* .equ constants                         */
#define DVM_SRC_MAX          16384  /* source bytes accepted                  */
#define DVM_LINE_MAX         256    /* source line length                     */
#define DVM_DSTACK           32     /* push/pop depth                         */
#define DVM_CSTACK           16     /* call/ret depth                         */
#define DVM_MSG_MAX          192    /* result / assembler message buffer      */

/* ---- the DMA scratch arena ----
 *
 * SIZE. 48 kHz stereo 16-bit PCM — the format every PC audio codec since AC'97
 * accepts without resampling — costs 48000 * 2 channels * 2 bytes = 187.5 KiB
 * per second. 256 KiB is the next power of two above that: it holds 1.09 s of
 * audio, which is long enough that a human hears a TONE rather than a click and
 * long enough that a polled kernel can watch a position counter advance, and it
 * still leaves ~68 KiB for whatever descriptor list, ring or command buffer the
 * device wants alongside the samples. A power of two also means any aligned
 * sub-buffer a program carves out of it stays inside it.
 *
 * ALIGNMENT. 4096 bytes. Alignment is not cosmetic to a DMA engine: hardware
 * commonly ignores the low bits of a descriptor-base register instead of
 * faulting on them, so an under-aligned buffer does not produce an error, it
 * produces DMA at a *different address* — which on this machine means DMA into
 * whatever is below the buffer. One page is the strongest alignment a PCI
 * function's base-address register is architecturally permitted to require, and
 * it is a multiple of every weaker one that real audio hardware asks for (8 for
 * an AC'97 buffer-descriptor list, 128 for an HD Audio BDL/CORB/RIRB, 64 for a
 * cache line, 32 for a scatter-gather entry). Being page-aligned also means the
 * arena's physical address has 12 zero low bits, so a device that truncates the
 * address it is given still lands on the buffer's start. */
#define DVM_DMA_SIZE         0x40000ull   /* 256 KiB: 1.09 s of 48k/16/stereo  */
#define DVM_DMA_ALIGN        4096u        /* one page; see above              */

/* Poison either side of the arena, so that a DEVICE writing past the end of the
 * buffer is a reported fact instead of silent corruption of whatever .bss object
 * the linker put next. See dvm_dma_check(). This is the exact extent of the only
 * DMA-bounds check that exists on a machine with no IOMMU: an overrun bigger than
 * this, or a completely wrong address, is not caught by anything. */
#define DVM_DMA_GUARD        0x10000ull   /* 64 KiB */

/* ---- the entry argument vector ----
 *
 * dvm_run() preloads r0..r(nargs-1) from `args`, so the meaning of a register on
 * entry is a convention between a caller and the program it runs. This is the
 * convention tools/dvm_tools.c uses and driver_run's description publishes, and
 * it is spelled out here so the two cannot drift apart. A caller with a
 * different contract (vm/programs/ac97_boot.c has one) just passes its own
 * vector; these constants oblige nobody. */
#define DVM_ARG_BAR0         0            /* r0..r5: the target's BAR bases   */
#define DVM_ARG_BDF          6            /* r6: bus<<8 | dev<<3 | fn         */
#define DVM_ARG_DMA_BASE     7            /* r7: DMA buffer physical address  */
#define DVM_ARG_DMA_SIZE     8            /* r8: its size in bytes            */
#define DVM_NARGS            9

/* ---- policy table sizes (inline, so a policy has no lifetime problem) ---- */
#define DVM_MAX_PORT_RANGES  8
#define DVM_MAX_MMIO_WINS    8
#define DVM_MAX_PCI_FNS      8

/* ====================================================================== */
/* status                                                                 */
/* ====================================================================== */

typedef enum {
    DVM_OK = 0,               /* ran to `halt`                              */
    DVM_TRAP_ABORT,           /* program executed `abort "..."`             */
    DVM_TRAP_STEPS,           /* instruction (cycle) budget exhausted       */
    DVM_TRAP_IO_BUDGET,       /* too many device accesses                   */
    DVM_TRAP_DELAY_BUDGET,    /* cumulative or single delay too long        */
    DVM_TRAP_PRINT_BUDGET,    /* too many print lines                       */
    DVM_TRAP_PORT_DENIED,     /* port not in the allowlist, or denied always */
    DVM_TRAP_MMIO_DENIED,     /* address outside every window, or read-only  */
    DVM_TRAP_PCI_DENIED,      /* config read of a function not allowed       */
    DVM_TRAP_ALIGN,           /* MMIO/PCI access not naturally aligned       */
    DVM_TRAP_RANGE,           /* operand out of range (port > 0xffff, ...)   */
    DVM_TRAP_PC,              /* branch target outside the program           */
    DVM_TRAP_STACK,           /* data/call stack overflow or underflow       */
    DVM_TRAP_DIV0,            /* div/mod by zero                            */
    DVM_TRAP_BADOP,           /* corrupt program: unknown opcode/operand     */
    DVM_TRAP_NOIO,            /* backend has no hook for this access class   */
    DVM_TRAP_POLICY,          /* the caller's policy is itself invalid       */
    DVM_TRAP_DMA_BUDGET,      /* too many ld/st into the DMA scratch buffer  */
    DVM_STATUS__COUNT
} dvm_status_t;

/* "OK", "TRAP_STEPS", ... Never NULL. */
const char *dvm_status_name(dvm_status_t s);

/* ====================================================================== */
/* opcodes                                                                */
/* ====================================================================== */

typedef enum {
    DVM_HALT = 0, DVM_ABORT, DVM_NOP,
    DVM_MOV,
    DVM_ADD, DVM_SUB, DVM_MUL, DVM_DIV, DVM_MOD,
    DVM_AND, DVM_OR,  DVM_XOR, DVM_NOT, DVM_SHL, DVM_SHR,
    DVM_CMP, DVM_TEST,
    DVM_JMP, DVM_BEQ, DVM_BNE, DVM_BLT, DVM_BLE, DVM_BGT, DVM_BGE,
    DVM_CALL, DVM_RET, DVM_PUSH, DVM_POP,
    DVM_OUT8, DVM_OUT16, DVM_OUT32,
    DVM_IN8,  DVM_IN16,  DVM_IN32,
    DVM_LD8,  DVM_LD16,  DVM_LD32,
    DVM_ST8,  DVM_ST16,  DVM_ST32,
    DVM_PCICFG,
    DVM_DELAY,
    DVM_PRINT,
    DVM_OP__COUNT
} dvm_op_t;

/* "add", "out32", ... Never NULL. */
const char *dvm_op_name(dvm_op_t op);

/* ---- operand kinds ---- */
typedef enum {
    DVM_O_NONE = 0,
    DVM_O_REG,        /* val = register index                      */
    DVM_O_IMM,        /* val = literal                             */
    DVM_O_PC,         /* val = instruction index (branch target)   */
    DVM_O_STR         /* val = string index                        */
} dvm_okind_t;

#define DVM_NOREG 0xFF

/* One decoded instruction. Fixed size; the assembler is the only producer, and
 * dvm_program_validate() re-checks every field before execution. */
typedef struct {
    uint8_t  op;          /* dvm_op_t                                   */
    uint8_t  dst;         /* register written, or DVM_NOREG             */
    uint8_t  kind[3];     /* dvm_okind_t per source operand             */
    uint8_t  _pad[3];
    uint32_t line;        /* 1-based source line, for traces and errors */
    uint64_t val[3];
} dvm_insn_t;

typedef struct {
    char     name[DVM_LABEL_MAX];
    uint32_t pc;
} dvm_label_t;

/* An assembled program. Self-contained: no pointers into the source text. */
typedef struct {
    dvm_insn_t  insn[DVM_MAX_INSNS];
    uint32_t    ninsn;

    dvm_label_t label[DVM_MAX_LABELS];
    uint32_t    nlabel;

    char        strpool[DVM_STRPOOL];
    uint16_t    stroff[DVM_MAX_STRINGS];   /* offset into strpool, NUL-term'd */
    uint32_t    nstr;
    uint32_t    strused;

    uint32_t    src_lines;                 /* lines of source consumed */
} dvm_program_t;

/* ====================================================================== */
/* assembler                                                              */
/* ====================================================================== */

typedef struct {
    int  line;                 /* 1-based source line, 0 if not line-specific */
    char msg[DVM_MSG_MAX];     /* "unknown mnemonic \"outq\"" etc.            */
} dvm_asm_err_t;

/* Assemble `src` (len bytes; NOT required to be NUL-terminated — it arrives as
 * a raw JSON string span) into `out`. Returns 0, or -1 with *err filled in.
 * `err` may be NULL. `out` is fully overwritten on success and left in a
 * defined, empty state on failure. */
int dvm_assemble(const char *src, size_t len, dvm_program_t *out,
                 dvm_asm_err_t *err);

/* Structural check of an already-built program: opcodes, register indices,
 * branch targets, string indices, operand kinds. dvm_run() calls this first, so
 * a program that never went through the assembler still cannot escape.
 * Returns DVM_OK or DVM_TRAP_BADOP/DVM_TRAP_PC/DVM_TRAP_RANGE; when it fails and
 * `err` is non-NULL, err->line is the offending source line. */
dvm_status_t dvm_program_validate(const dvm_program_t *p, dvm_asm_err_t *err);

/* Render instruction `pc` back to assembler text (no trailing newline).
 * Returns the number of bytes written, 0 if pc is out of range. */
size_t dvm_disasm(const dvm_program_t *p, uint32_t pc, char *buf, size_t cap);

/* ====================================================================== */
/* policy — what the program is allowed to touch                          */
/* ====================================================================== */

typedef struct { uint16_t lo, hi; } dvm_port_range_t;    /* inclusive */

#define DVM_MMIO_R   0x1
#define DVM_MMIO_W   0x2
#define DVM_MMIO_RW  (DVM_MMIO_R | DVM_MMIO_W)

typedef struct {
    uint64_t base;
    uint64_t size;      /* bytes; [base, base+size) */
    uint32_t flags;     /* DVM_MMIO_R | DVM_MMIO_W  */
} dvm_mmio_win_t;

typedef struct { uint8_t bus, dev, fn; } dvm_pci_fn_t;

/* Trace verbosity. IO is the useful default: every device access, every trap,
 * and the run summary, but not the arithmetic in between. */
typedef enum {
    DVM_TRACE_OFF = 0,
    DVM_TRACE_IO,      /* device accesses, prints, traps, summary */
    DVM_TRACE_ALL      /* + one line per instruction executed     */
} dvm_trace_t;

typedef struct {
    dvm_port_range_t port[DVM_MAX_PORT_RANGES];
    int              nport;
    dvm_mmio_win_t   mmio[DVM_MAX_MMIO_WINS];
    int              nmmio;
    dvm_pci_fn_t     pci[DVM_MAX_PCI_FNS];
    int              npci;

    /* The DMA scratch grant. 0/0 means "this program has no memory", which is
     * what dvm_policy_init() leaves behind and what every caller that does not
     * need DMA should keep. Only dvm_policy_allow_dma() should set these: it is
     * the function that checks the range against the arena. */
    uint64_t         dma_base;
    uint64_t         dma_size;

    uint64_t         max_steps;
    uint64_t         max_io;
    uint64_t         max_dma_ops;
    uint64_t         max_delay_us;
    uint32_t         max_single_delay_us;
    uint32_t         max_prints;
    uint32_t         max_trace;
    dvm_trace_t      trace;
} dvm_policy_t;

/* Deny everything — including the DMA buffer — with the default budgets:
 *   max_steps 100000, max_io 4096, max_dma_ops 131072, max_delay_us 200000
 *   (200 ms), max_single_delay_us 50000, max_prints 64, max_trace 512,
 *   trace = IO. */
void dvm_policy_init(dvm_policy_t *p);

/* Open one window. Return 0, or -1 if the table is full or the request is
 * malformed (lo > hi, zero size, an address range that wraps, flags with no
 * access bits). A refused request leaves the policy unchanged — it does NOT
 * silently open something smaller. */
int dvm_policy_allow_ports(dvm_policy_t *p, uint16_t lo, uint16_t hi);
int dvm_policy_allow_mmio (dvm_policy_t *p, uint64_t base, uint64_t size,
                           uint32_t flags);
int dvm_policy_allow_pci  (dvm_policy_t *p, uint8_t bus, uint8_t dev, uint8_t fn);

/* The single DMA scratch arena this module owns: a static, DVM_DMA_ALIGN-aligned
 * DVM_DMA_SIZE-byte array. `base` is its address, which on this kernel is also
 * its physical address (the low 4 GiB is identity-mapped by boot/boot.asm), so
 * it is what a 32-bit bus master must be given. Either pointer may be NULL.
 *
 * The same call works on a host test build, where the "physical" address is just
 * the array's address in the test process and a store through the VM is a store
 * the test can read back. That is deliberate: it makes the buffer path the same
 * code on both, so a host test proves the real one. */
void dvm_dma_region(uint64_t *base, uint64_t *size);

/* Grant [base, base+size) of the DMA arena to a run. Returns 0, or -1 — leaving
 * the policy UNCHANGED — if the request is empty, wraps, or is not entirely
 * inside dvm_dma_region(). That last check is the point: the caller does not get
 * to name a location, only a subrange of one address this module published, so a
 * miscomputed base in a caller cannot become a write into the heap.
 *
 * Once granted, ld8/ld16/ld32 and st8/st16/st32 reach the range with the same
 * bounds and alignment rules as MMIO, and each access is counted against
 * max_dma_ops rather than max_io. THE GRANT IS ALSO WHAT MAKES DEVICE DMA
 * POSSIBLE, and the VM cannot police that half — see the header comment. */
int dvm_policy_allow_dma  (dvm_policy_t *p, uint64_t base, uint64_t size);

/* Did a DEVICE write outside the arena? The arena is embedded in a larger block
 * with 64 KiB of poison either side, re-armed by every dvm_run() that carries a
 * DMA grant. Returns 0 if both guard bands are intact, or 1 with a sentence in
 * `msg` naming the direction and the byte offset — then re-arms, so one accident
 * is reported once instead of for ever.
 *
 * Call it AFTER dvm_run(). This is the only mechanism on this machine that
 * notices a bad DMA address at all, and it only notices near misses: an overrun
 * of up to 64 KiB, or an underrun of the same. A device pointed somewhere else
 * entirely corrupts that somewhere else silently. Do not read a clean result as
 * "the DMA was correct"; read it as "it was not wrong in the usual way". */
int dvm_dma_check(char *msg, size_t cap);

/* Validate a policy against the compiled-in deny list and the structural rules.
 * dvm_run() does this before executing anything; exposed so a caller (or a tool)
 * can report a bad window before the model is told the program will run.
 * Returns DVM_OK or DVM_TRAP_POLICY, and writes the reason into msg. */
dvm_status_t dvm_policy_check(const dvm_policy_t *p, char *msg, size_t cap);

/* ====================================================================== */
/* the I/O backend — the only path to hardware                            */
/* ====================================================================== */

/* width is always 1, 2 or 4 bytes. A NULL hook means "this machine has no such
 * access class": the corresponding opcode traps with DVM_TRAP_NOIO instead of
 * being skipped, so a program can never believe an access happened when it did
 * not. Every hook is called ONLY after the access passed policy. */
typedef struct dvm_io {
    void     *ctx;
    void     (*port_write)(void *ctx, uint16_t port, uint8_t width, uint32_t val);
    uint32_t (*port_read )(void *ctx, uint16_t port, uint8_t width);
    void     (*mmio_write)(void *ctx, uint64_t addr, uint8_t width, uint32_t val);
    uint32_t (*mmio_read )(void *ctx, uint64_t addr, uint8_t width);
    uint32_t (*pci_read32)(void *ctx, uint8_t bus, uint8_t dev, uint8_t fn,
                           uint8_t off);
    void     (*delay_us  )(void *ctx, uint32_t us);
} dvm_io_t;

/* The real machine: port I/O instructions, volatile MMIO, pci_cfg_read32(),
 * mdelay()+TSC. NULL on a host build, where none of that exists. */
const dvm_io_t *dvm_io_hardware(void);

/* ====================================================================== */
/* execution                                                              */
/* ====================================================================== */

typedef struct {
    dvm_status_t status;
    uint32_t     pc;            /* where it stopped                        */
    uint32_t     line;          /* source line of that instruction         */
    uint64_t     steps;
    uint64_t     io_ops;
    uint64_t     delay_us;
    uint32_t     prints;
    uint32_t     n_port_rd, n_port_wr;
    uint32_t     n_mmio_rd, n_mmio_wr;
    uint32_t     n_pci_rd;
    /* ld/st into the DMA scratch buffer. Their SUM is what max_dma_ops bounds;
     * there is no third counter, so the budget and the report cannot disagree. */
    uint32_t     n_dma_rd, n_dma_wr;
    uint32_t     trace_lines;
    uint32_t     trace_dropped;
    uint64_t     reg[DVM_NREGS];   /* final register file — the program's output */
    char         msg[DVM_MSG_MAX]; /* "" on a clean halt, else the exact reason  */
} dvm_result_t;

/* Run `p` under `pol` against `io`. r0..r(nargs-1) are preloaded from `args`
 * (pass NULL/0 for none); the rest start at zero. See DVM_ARG_* for the
 * convention tools/dvm_tools.c uses. `res` must be non-NULL and is always fully
 * populated, including on refusal. Returns res->status.
 *
 * The program cannot outlive this call: there is no yielding, no callback into
 * model code, and no state kept between runs. */
dvm_status_t dvm_run(const dvm_program_t *p, const dvm_policy_t *pol,
                     const dvm_io_t *io, const uint64_t *args, int nargs,
                     dvm_result_t *res);

#endif /* DVM_H */
