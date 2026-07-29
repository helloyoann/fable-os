/* fault.h — what the machine knows about the last thing that went wrong, and
 *           what it is allowed to do about it.
 *
 * PURPOSE
 *   idt.h gets the CPU into a C function when an exception fires. This header
 *   owns everything after that: the exact shape of the saved machine state, the
 *   semantics of the error code, the bytes of the instruction that failed, a
 *   best-effort walk back up the call stack, one legible report built out of all
 *   of it — and, since Phase 3b, the machinery that turns a fault from a
 *   tombstone into something execution can come back from.
 *
 *   The report matters more here than it would in a normal kernel. There is no
 *   shell on this machine, no `dmesg`, no core file and no debugger. When the
 *   kernel dies, what it printed on the way down is the *entire* record of why.
 *   So the report is not a hex dump with a header — it says which exception
 *   fired, what the error code means in words, where it happened, what the
 *   instruction was, how the machine got there, and what was done about it.
 *
 * ARCHITECTURE
 *   Deliberately split so that almost all of it is pure, portable logic:
 *
 *     fault_frame_t      the live stack image isr.asm built. A pointer to this
 *                        IS the interrupted machine state — writing to it and
 *                        returning through IRETQ is how recovery resumes.
 *     fault_window_t     the address ranges the capture code is allowed to read,
 *                        and the range a resumed RIP must land inside. Passed in
 *                        rather than compiled in, so the same code runs against a
 *                        synthetic stack in tests/host/test_fault.c.
 *     fault_record_t     a self-contained snapshot: the frame, the decoded
 *                        extras, the instruction bytes, the backtrace, and the
 *                        disposition. Copied out of the frame so it survives the
 *                        stack it came from.
 *     fault_capture()    frame + window -> record. The only part that reads
 *                        arbitrary memory, and it reads nothing outside `window`.
 *     fault_format_...() record -> text. Pure; no hardware, no allocation.
 *     x86_insn_length()  the length of the instruction at RIP, or 0 for "I do
 *                        not know" — the only honest third answer.
 *     fault_plan_t       a recovery decision, armed in advance by the model.
 *     fault_apply_plan() plan + frame -> a patched frame, or a refusal with a
 *                        reason. Pure, and hammered by the host tests.
 *
 *   The kernel renders each report into a per-fault ring slot and prints it.
 *   fault_last_report() hands back the very same bytes, so what the operator saw
 *   on the console and what the model is later told about the crash cannot drift
 *   apart — they are the same string.
 *
 * RECOVERY, AND WHY IT IS SHAPED LIKE THIS
 *   A fault handler cannot ask the model what to do: it runs with IF clear, on
 *   the interrupted stack, with the heap and the network under suspicion. So the
 *   decision is made *before* the fault, not during it. The model arms a
 *   fault_plan_t through tools/fault_tools.c; the handler consults it, validates
 *   every field again against the live frame, and either applies it or refuses
 *   with a reason that ends up in the report.
 *
 *   Five things stand between a hallucinated plan and an unbootable machine:
 *
 *     1. #DF is never recoverable. Its frame's RSP is the thing that broke; a
 *        resume would double-fault again on the same unusable stack.
 *     2. Any RIP the plan produces — a jump target, or RIP + an instruction
 *        length — must land inside [code_lo, code_hi). Resuming outside the
 *        loaded image is not recovery, it is executing whatever is there with no
 *        record of why, which is strictly worse than halting.
 *     3. "Skip the faulting instruction" needs a real length. Guessing lands
 *        mid-instruction and the machine then executes the tail of an operand as
 *        an opcode. x86_insn_length() decodes a deliberately partial subset and
 *        returns 0 for everything else; a refusal is a correct answer, a wrong
 *        length is not.
 *     4. A plan is consumed. It carries a use count, so a single armed decision
 *        cannot silently paper over an unbounded stream of different faults.
 *     5. Repeated faults at the same RIP are counted. A "set a register and
 *        retry" plan that does not actually fix anything re-faults at exactly
 *        the same address; after FAULT_REFAULT_MAX attempts the kernel stops
 *        trying and halts, so a recovery loop cannot spin forever.
 *
 * WHY A RING OF RECORDS
 *   With one static record, a fault taken while the model is reading the report
 *   of an earlier one rewrites the record under its own caller: the tool starts
 *   describing fault #1 and finishes describing fault #2, with no seam. That is
 *   impossible while faults are terminal and unavoidable once they are not, so
 *   the records live in a ring keyed by `seq` and every reader names the record
 *   it wants.
 *
 * SAFETY MODEL
 *   Every function here is callable from inside an exception handler. That is a
 *   hard constraint, not an aspiration:
 *     - No allocation. Not one call into the heap, because the heap is a prime
 *       suspect whenever this code runs.
 *     - No device access. The console is reached through kernel.h's kputs, which
 *       is the same path trace.c relies on for the same reason.
 *     - No unbounded reads. Every dereference of a fault-supplied address is
 *       gated on fault_window_t first, and every loop has a fixed iteration cap.
 *     - No recursion, and the walkers require strictly increasing frame pointers
 *       so a corrupted stack produces a short backtrace, never a hang.
 *
 * WHY A NULL DEREFERENCE DOES NOT FAULT ON THIS MACHINE
 *   Worth stating because it surprises everyone who tests this: boot.asm
 *   identity-maps the low 4 GiB with present+writable 2 MiB pages, so virtual
 *   address 0 is real, writable RAM — and it shares its huge page with the VGA
 *   framebuffer at 0xB8000, so it cannot simply be unmapped. `*(int *)0 = 1`
 *   therefore succeeds silently. To actually raise #PF you need an address
 *   outside the identity map but still canonical; the injector uses one, and
 *   fault_inject.c says so at the call site.
 *
 * PUBLIC API
 *   Naming and semantics   fault_vector_name / _desc, fault_has_error_code,
 *                          fault_is_fatal, fault_is_recoverable.
 *   Decoding               fault_decode_error  (page-fault bits, selector error
 *                          codes, and the "no error code" case, in words).
 *                          fault_decode_rflags.
 *   Capture                fault_capture, fault_backtrace, fault_backtrace_scan.
 *   Reporting              fault_format_report.
 *   Instruction length     x86_insn_length.
 *   Registers              fault_reg_name, fault_reg_lookup, fault_reg_slot.
 *   Recovery (pure)        fault_apply_plan, fault_fix_name, fault_fix_desc,
 *                          fault_plan_check.
 *   Recovery (live)        fault_plan_arm, fault_plan_clear, fault_plan_get,
 *                          fault_recover, fault_refaults.
 *   Live state (read-only) fault_occurred, fault_count, fault_last,
 *                          fault_last_report, fault_ring_len, fault_ring_at,
 *                          fault_by_seq. This is the surface tools/
 *                          fault_tools.c exposes to the model.
 *   Injection              fault_inject — testing only, see fault_inject.c.
 *
 * DEPENDENCIES
 *   <stdint.h>/<stddef.h> and snprintf, nothing else. The same object file
 *   compiles freestanding into the kernel and natively for the host tests, which
 *   is why the decoder that the operator's only crash record depends on is the
 *   decoder the tests hammer.
 *
 * FUTURE EXTENSION POINTS
 *   x86_insn_length() covers the encodings this kernel's compiler actually
 *   emits; widening it (SSE, the 0F 38 / 0F 3A maps, VEX) is a table edit and a
 *   row of test vectors, and every byte it does not know is already refused
 *   rather than guessed. Beyond that: a plan that names a *condition* (this
 *   vector, at this RIP, with this CR2) rather than just a vector; a plan that
 *   rewrites the faulting instruction in place instead of stepping over it —
 *   every page is RWX, so that is legal here; and, once the model can be asked
 *   a question from inside the handler, a synchronous "what should I do?" turn
 *   in place of the pre-armed decision.
 */
#ifndef FAULT_H
#define FAULT_H

#include <stdint.h>
#include <stddef.h>

/* ====================================================================== */
/* the saved machine state                                                */
/* ====================================================================== */

/* Exactly the layout arch/x86_64/isr.asm builds on the interrupted stack, in
 * ascending address order. Nothing may be reordered, resized or inserted
 * without changing isr.asm to match; the compile-time size check in
 * arch/x86_64/idt.c exists to make a mistake here a build failure.
 *
 * How it is assembled, from the top of the struct downwards:
 *   - ss..rip      pushed by the CPU. In long mode SS and RSP are pushed even
 *                  when there is no privilege change, so the frame is the same
 *                  size for every vector.
 *   - error_code   pushed by the CPU for ten vectors (#DF #TS #NP #SS #GP #PF
 *                  #AC #CP #VC #SX) and by the stub, as a zero, for the other
 *                  246. That asymmetry is the single nastiest detail in x86
 *                  exception handling and it is normalised here, once.
 *   - vector       pushed by the stub; the only way the common path can tell
 *                  which of the 256 entry points it came through.
 *   - r15..rax     pushed by the common path.
 *   - reserved     a zero, pushed to restore 16-byte stack alignment before the
 *                  call into C (the CPU frame plus the GPRs comes to an odd
 *                  number of qwords once CR2 is added). Not otherwise used.
 *   - cr2          read and pushed by the common path before anything else can
 *                  disturb it. Only meaningful for #PF, but always captured:
 *                  a stale CR2 is still evidence about the previous page fault.
 */
typedef struct fault_frame {
    uint64_t cr2;
    uint64_t reserved;

    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;

    uint64_t vector;
    uint64_t error_code;

    uint64_t rip;
    uint64_t cs;
    uint64_t rflags;
    uint64_t rsp;
    uint64_t ss;
} fault_frame_t;

/* ====================================================================== */
/* what capture is allowed to read                                        */
/* ====================================================================== */

/* Three half-open ranges. Nothing in this module dereferences an address that
 * is not inside one of them, so a fault taken with garbage in every register
 * cannot turn the reporting path into a second fault.
 *
 *   image_lo..image_hi   backed RAM that may be read for instruction bytes. In
 *                        the kernel this is [1 MiB, __bss_end) — the same window
 *                        tools/mem_tools.c justifies at length, and for the same
 *                        reason: the loader wrote it and boot.asm zeroed the rest
 *                        of it, so every byte is demonstrably present.
 *   code_lo..code_hi     addresses plausible as a return address / RIP. In the
 *                        kernel this is [1 MiB, __bss_start): text, rodata and
 *                        data, i.e. everything that is not zero-filled at boot.
 *   stack_lo..stack_hi   readable stack. The kernel stack lives inside .bss, so
 *                        [__bss_start, __bss_end) bounds it without needing a
 *                        symbol boot.asm does not export.
 */
typedef struct fault_window {
    uint64_t image_lo, image_hi;
    uint64_t code_lo,  code_hi;
    uint64_t stack_lo, stack_hi;
} fault_window_t;

/* ====================================================================== */
/* the snapshot                                                           */
/* ====================================================================== */

#define FAULT_CODE_BYTES  16    /* bytes grabbed at RIP; longest x86 insn is 15 */
#define FAULT_BT_MAX      12    /* backtrace depth cap                          */
#define FAULT_REPORT_MAX  3584  /* rendered report; fits TOOL_RESULT_MAX (4096) */
#define FAULT_DETAIL_MAX  120   /* one line of "what was done, exactly"         */

/* How many faults are kept. Small on purpose: the value of the ring is that a
 * reader's record cannot be rewritten under it, not that the machine keeps a
 * long history of its own failures. Eight is more than a diagnostic loop will
 * hold open at once and costs ~3 KiB of .bss. */
#define FAULT_RING_SIZE   8

/* Where a backtrace came from, because "0x100abc, 0x100def" means two very
 * different things depending on the answer. */
#define FAULT_BT_NONE   0   /* nothing usable                                  */
#define FAULT_BT_FRAME  1   /* an RBP chain walk: these really are call frames */
#define FAULT_BT_SCAN   2   /* a raw stack scan: candidates, possibly stale     */

typedef struct fault_record {
    uint32_t      valid;       /* 0 until a fault has been captured         */
    uint32_t      seq;         /* 1-based; which fault since boot this was  */
    uint64_t      uptime_ms;   /* millis() at capture time                  */

    fault_frame_t regs;        /* verbatim copy of the frame, AS IT FAULTED */

    uint8_t       code[FAULT_CODE_BYTES];
    uint8_t       code_len;    /* bytes actually readable at RIP            */
    uint8_t       code_valid;  /* 0 when RIP was outside the image window   */
    uint8_t       fatal;       /* 0 only for the continuable traps          */

    uint8_t       bt_count;
    uint8_t       bt_source;   /* FAULT_BT_NONE / _FRAME / _SCAN            */
    uint64_t      bt[FAULT_BT_MAX];

    /* ---- disposition: filled in after the recovery decision ----
     * `regs` above is deliberately never patched. It is what the CPU handed
     * over; these fields are what the kernel then did about it. Keeping the two
     * apart is the difference between a record and a rationalisation. */
    uint8_t       fix_set;     /* 1 once a disposition has been recorded    */
    uint8_t       fix;         /* fault_fix_t                               */
    uint8_t       recovered;   /* 1 when execution actually resumed         */
    uint8_t       insn_len;    /* decoded length at RIP, 0 = undecodable    */
    uint64_t      resume_rip;  /* where it resumed; only when recovered     */
    char          fix_detail[FAULT_DETAIL_MAX];
} fault_record_t;

/* ====================================================================== */
/* naming and semantics                                                   */
/* ====================================================================== */

/* "#PF", "NMI", "IRQ" (>= 32), "??" for a reserved vector. Never NULL. */
const char *fault_vector_name(uint64_t vector);

/* "Page Fault", "Invalid Opcode", ... Never NULL. */
const char *fault_vector_desc(uint64_t vector);

/* 1 when the CPU itself pushes an error code for this vector. The stub pushes a
 * zero for the rest; the report says which of the two it is showing so a zero is
 * never mistaken for information. */
int fault_has_error_code(uint64_t vector);

/* 0 for the traps execution can simply continue past (#DB, #BP) and for the
 * unexpected-interrupt vectors >= 32; 1 for everything else. A fatal fault
 * halts unless a recovery plan is armed AND fault_is_recoverable() agrees. */
int fault_is_fatal(uint64_t vector);

/* 1 when a recovery plan is even allowed to be considered for this vector.
 *
 * #DF (8) is the one hard no, and the reason is not caution: a double fault
 * means the CPU could not deliver an earlier exception, overwhelmingly because
 * RSP was unusable. It is reported at all only because it arrives on a
 * dedicated IST stack. Patching RIP and returning would IRETQ straight back
 * onto the broken stack the CPU already proved it cannot use, and the next
 * event is a triple fault and a silent VM reset — the exact outcome this whole
 * module exists to prevent.
 *
 * #MC (18) is refused for the same shape of reason: a machine check is a
 * hardware error report, not a software mistake, and "continue anyway" is not a
 * decision software gets to make. */
int fault_is_recoverable(uint64_t vector);

/* ====================================================================== */
/* decoding                                                               */
/* ====================================================================== */

/* Render `error_code` in words for `vector`, into `dst`. snprintf-style return:
 * the length that would have been written, and `dst` is always NUL-terminated
 * when cap > 0.
 *
 *   #PF   present/write/user/reserved/instruction-fetch/protection-key/
 *         shadow-stack, spelled out. A model reading "0x2" learns nothing; a
 *         model reading "not-present, write, kernel-mode, data access" can name
 *         the bug.
 *   #GP #SS #NP #TS #CP #SX  selector error codes: external, descriptor table,
 *         and the selector index.
 *   #DF   always zero, and said so.
 *   other "no error code (the stub pushed zero)".
 */
size_t fault_decode_error(uint64_t vector, uint64_t error_code,
                          char *dst, size_t cap);

/* "CF PF AF ZF SF TF IF DF OF ..." — the flags that are set, space separated,
 * plus IOPL. snprintf-style return. */
size_t fault_decode_rflags(uint64_t rflags, char *dst, size_t cap);

/* ====================================================================== */
/* capture                                                                */
/* ====================================================================== */

/* Walk the RBP chain starting at `rbp`, writing up to `max` return addresses
 * into `out`. Reads only inside window->stack_lo..stack_hi, accepts a return
 * address only inside window->code_lo..code_hi, and requires each frame pointer
 * to be 8-aligned and strictly greater than the last, so a corrupt chain
 * terminates instead of looping. Returns the number of entries written.
 *
 * Exposed (rather than buried in fault_capture) so tests/host/test_fault.c can
 * build a synthetic frame chain in a normal array and check the walk exactly. */
int fault_backtrace(uint64_t rbp, const fault_window_t *window,
                    uint64_t *out, int max);

/* Fallback for when RBP has been destroyed: scan upward from `rsp` and collect
 * every qword that looks like a code address. These are candidates, not frames —
 * stale return addresses from earlier calls will be included. Bounded to
 * FAULT_SCAN_SLOTS qwords. Returns the number written. */
#define FAULT_SCAN_SLOTS 512
int fault_backtrace_scan(uint64_t rsp, const fault_window_t *window,
                         uint64_t *out, int max);

/* frame + window -> record. Copies the frame, grabs the instruction bytes at
 * RIP when RIP is inside the image window, and fills in a backtrace (frame walk
 * first, raw scan only if the walk found fewer than two frames). Sets
 * valid = 1; the caller owns `seq`. Never reads outside `window`. */
void fault_capture(const fault_frame_t *frame, const fault_window_t *window,
                   uint64_t uptime_ms, fault_record_t *out);

/* ====================================================================== */
/* reporting                                                              */
/* ====================================================================== */

/* Render the full operator-facing report. snprintf-style return; `dst` is always
 * NUL-terminated when cap > 0. Pure: no hardware, no allocation, no globals. */
size_t fault_format_report(const fault_record_t *rec, char *dst, size_t cap);

/* ====================================================================== */
/* instruction length — "skip this instruction", done honestly            */
/* ====================================================================== */

/* Length in bytes of the x86-64 instruction encoded at `code`, or 0 when it
 * cannot be determined WITH CERTAINTY.
 *
 * Zero is a first-class answer and by far the most important one. The only use
 * for this function is computing RIP + len as a resume address; a length that
 * is too short resumes in the middle of an instruction, where the CPU will
 * happily read the tail of a displacement as an opcode, and a length that is
 * too long silently skips a second instruction. Either produces a machine that
 * is executing something nobody chose, with no fault to mark the moment — which
 * is worse than halting and, on a box with no debugger, undiagnosable.
 *
 * So the decoder is deliberately partial. It knows the legacy prefixes, REX,
 * ModRM/SIB/displacement (which is exact and complete), the one-byte opcode map
 * and the 0F two-byte map for the encodings a C compiler actually emits, and
 * the operand-size rules that decide immediate widths. Everything else — the
 * 0F 38 / 0F 3A maps, VEX/EVEX, the moffs forms of A0..A3, x87 with unusual
 * escapes, anything that would need `avail` bytes it was not given, anything
 * longer than the architectural 15-byte limit — returns 0.
 *
 * `avail` is how many bytes at `code` are known to be readable (fault_record_t
 * carries at most FAULT_CODE_BYTES of them). Decoding never reads past it.
 *
 * Not named fault_* because it is not about faults: it is a general property of
 * the byte string, and it is tested as one. */
int x86_insn_length(const uint8_t *code, int avail);

/* ====================================================================== */
/* naming registers                                                       */
/* ====================================================================== */

/* Architectural encoding order, so a register index here is the same number the
 * ModRM reg field would carry. RSP is present for completeness and refused by
 * the recovery path; see fault_apply_plan. */
enum {
    FAULT_REG_RAX = 0, FAULT_REG_RCX, FAULT_REG_RDX, FAULT_REG_RBX,
    FAULT_REG_RSP,     FAULT_REG_RBP, FAULT_REG_RSI, FAULT_REG_RDI,
    FAULT_REG_R8,      FAULT_REG_R9,  FAULT_REG_R10, FAULT_REG_R11,
    FAULT_REG_R12,     FAULT_REG_R13, FAULT_REG_R14, FAULT_REG_R15,
    FAULT_REG_COUNT
};

/* "rax" ... "r15", or "?" for an out-of-range index. Never NULL. */
const char *fault_reg_name(int reg);

/* Name -> index, case-insensitive, with or without a leading '%'. Returns -1
 * for anything that is not one of the sixteen. */
int fault_reg_lookup(const char *name);

/* Pointer to `reg`'s slot inside a frame, or NULL for a bad index. This is the
 * only place the register index -> struct field mapping exists, so a test that
 * checks all sixteen checks the whole mapping. */
uint64_t *fault_reg_slot(fault_frame_t *frame, int reg);

/* ====================================================================== */
/* the recovery decision                                                  */
/* ====================================================================== */

typedef enum fault_action {
    FAULT_ACT_NONE = 0,   /* no plan; the fault runs its normal course      */
    FAULT_ACT_REFUSE,     /* an explicit "do not recover" — see below       */
    FAULT_ACT_SKIP,       /* step over the faulting instruction             */
    FAULT_ACT_SET_REG,    /* write a GPR, then retry or skip                */
    FAULT_ACT_SET_RIP,    /* resume at a different address in the image     */
    FAULT_ACT_COUNT
} fault_action_t;

/* FAULT_ACT_REFUSE is not the same as FAULT_ACT_NONE and the difference is the
 * point: NONE means nobody has decided anything, REFUSE means the model looked
 * at the situation and chose to let the machine halt. The report says which,
 * because "I did not know" and "I decided not to" are different diagnoses. */

#define FAULT_PLAN_USES_MAX 16   /* how many faults one armed plan may handle */

typedef struct fault_plan {
    uint8_t  action;       /* fault_action_t                                  */
    uint8_t  reg;          /* FAULT_REG_* — FAULT_ACT_SET_REG only            */
    uint8_t  then_skip;    /* SET_REG: 1 = skip the instruction, 0 = retry it */
    uint8_t  any_vector;   /* 1 = applies to any vector, 0 = only `vector`    */
    uint64_t vector;       /* the one vector this plan is for, when scoped    */
    uint64_t value;        /* the register value, or the target RIP           */
    uint32_t uses;         /* remaining applications; decremented on each use */
} fault_plan_t;

/* Why a fault did or did not resume. Ordered so that OK is 0 and every other
 * value is a refusal with a distinct, quotable reason — the model is going to
 * read these and try again. */
typedef enum fault_fix {
    FAULT_FIX_OK = 0,           /* the plan was applied; execution resumes    */
    FAULT_FIX_TRAP,             /* a continuable trap; resumed untouched      */
    FAULT_FIX_NO_PLAN,          /* nothing was armed                          */
    FAULT_FIX_REFUSED,          /* the plan said "let it halt"                */
    FAULT_FIX_VECTOR,           /* this vector is never recoverable (#DF)     */
    FAULT_FIX_WRONG_VECTOR,     /* the plan was scoped to a different vector  */
    FAULT_FIX_BAD_PLAN,         /* malformed: bad action, bad register        */
    FAULT_FIX_NO_CODE,          /* RIP is outside the image; no bytes to read */
    FAULT_FIX_UNDECODABLE,      /* x86_insn_length() would not commit         */
    FAULT_FIX_RIP_WINDOW,       /* the resume address is outside the image    */
    FAULT_FIX_REFAULT,          /* same RIP, too many times: stop trying      */
    FAULT_FIX_EXHAUSTED,        /* the plan ran out of uses                   */
    FAULT_FIX_COUNT
} fault_fix_t;

/* "ok", "refault", ... — a short stable token. Never NULL. */
const char *fault_fix_name(int fix);
/* One sentence a model or an operator can act on. Never NULL. */
const char *fault_fix_desc(int fix);

/* How many consecutive recoveries at one RIP are attempted before the kernel
 * stops. Three is enough for "set a register and retry" to work if it was ever
 * going to, and small enough that a loop is over in microseconds. */
#define FAULT_REFAULT_MAX 3

/* ---- the pure core ---- */

/* Static validation, with no frame in hand: is this plan even well-formed?
 * Returns FAULT_FIX_OK or the refusal. `window` may be NULL, in which case a
 * FAULT_ACT_SET_RIP target cannot be checked and is refused. */
fault_fix_t fault_plan_check(const fault_plan_t *plan,
                             const fault_window_t *window);

/* plan + live frame -> a patched frame, or a refusal. THE function that decides
 * whether this machine keeps running.
 *
 *   frame     patched in place, and ONLY on FAULT_FIX_OK. Every refusal leaves
 *             the frame byte-identical, so a caller can refuse and halt without
 *             wondering what was half-applied.
 *
 * THE POSTCONDITION, which is the whole safety argument in one line:
 *   on FAULT_FIX_OK, frame->rip is inside [window->code_lo, window->code_hi).
 *   That holds for a jump target, for RIP + a decoded length, and for "retry",
 *   which reuses the faulting RIP and is refused if the fault happened outside
 *   the image — a machine already executing outside its own text is not one a
 *   register write can rescue.
 *   window    code_lo..code_hi bounds every RIP this can produce.
 *   code/len  the bytes at the faulting RIP, or NULL/0 if RIP was unreadable.
 *   why       optional; a short sentence naming exactly what was done or why it
 *             was not. Always NUL-terminated when whycap > 0.
 *   insn_len  optional out; the decoded instruction length, 0 if undecodable.
 *
 * Pure: no globals, no hardware, no allocation, no use counting (the caller
 * owns that). This is the function tests/host/test_fault.c fuzzes. */
fault_fix_t fault_apply_plan(fault_frame_t *frame,
                             const fault_window_t *window,
                             const fault_plan_t *plan,
                             const uint8_t *code, unsigned code_len,
                             char *why, size_t whycap,
                             uint8_t *insn_len);

/* ====================================================================== */
/* live state — read-only, and the whole point of tools/fault_tools.c      */
/* ====================================================================== */

/* The window the kernel captures and validates against. idt.c computes it from
 * the linker symbols and publishes it here once, at idt_init(), so the tools
 * can show the model the bounds a plan will be judged by and fault_plan_arm()
 * can reject an out-of-image RIP at arm time rather than at fault time.
 * fault_window() returns NULL until it has been set. */
void                  fault_set_window(const fault_window_t *window);
const fault_window_t *fault_window(void);

/* Capture into the next ring slot, assign the next seq, and render the report.
 * Called by isr_dispatch(); nothing else should call it. */
void fault_note(const fault_frame_t *frame, const fault_window_t *window,
                uint64_t uptime_ms);

/* Consult the armed plan and, if everything checks out, patch `frame` so IRETQ
 * resumes. Records the disposition into the current ring slot and re-renders
 * its report, so what the console prints already says what was done. Consumes
 * one use of the plan on FAULT_FIX_OK, and maintains the same-RIP re-fault
 * counter. Returns FAULT_FIX_OK when the caller should return into the frame. */
fault_fix_t fault_recover(fault_frame_t *frame, const fault_window_t *window);

/* Arm / disarm / inspect the plan. fault_plan_arm validates through
 * fault_plan_check against fault_window() and returns FAULT_FIX_OK on success;
 * on refusal nothing is armed and any previous plan is left alone. */
fault_fix_t        fault_plan_arm(const fault_plan_t *plan);
void               fault_plan_clear(void);
const fault_plan_t *fault_plan_get(void);   /* NULL when nothing is armed */

/* Consecutive faults seen at the current RIP, and that RIP. Zero when the last
 * fault was somewhere else. Exposed so the model can see the loop it is in. */
uint32_t fault_refaults(void);
uint64_t fault_refault_rip(void);

int             fault_occurred(void);      /* 1 once anything has been captured */
uint32_t        fault_count(void);         /* faults captured since boot        */
const fault_record_t *fault_last(void);    /* the newest one, or NULL           */
const char     *fault_last_report(void);   /* the exact text that was printed   */

/* The ring. `back` is 0 for the newest retained record, 1 for the one before
 * it, up to fault_ring_len() - 1. NULL past the end. The returned pointer stays
 * valid and unchanged until FAULT_RING_SIZE further faults have occurred, which
 * is the entire reason the ring exists. */
uint32_t              fault_ring_len(void);
const fault_record_t *fault_ring_at(uint32_t back);

/* The retained record with this seq, or NULL if it never existed or has been
 * overwritten. seq is 1-based and matches "fault #N" in the reports. */
const fault_record_t *fault_by_seq(uint32_t seq);

/* Tests only: forget everything — ring, counters, plan, window. Not reachable
 * from the model. */
void fault_reset(void);

/* ====================================================================== */
/* injection — testing only                                               */
/* ====================================================================== */

/* Every one of these deliberately breaks the machine. See arch/x86_64/
 * fault_inject.c for why each is written the way it is, and for why a plain
 * null dereference is not on the list. */
typedef enum fault_inject_kind {
    FAULT_INJECT_NONE = 0,
    FAULT_INJECT_DE,        /* integer divide by zero            -> #DE (0)  */
    FAULT_INJECT_BP,        /* int3                              -> #BP (3)  */
    FAULT_INJECT_UD,        /* ud2                               -> #UD (6)  */
    FAULT_INJECT_DF,        /* fault on a non-canonical stack    -> #DF (8)  */
    FAULT_INJECT_GP,        /* load a selector past the GDT      -> #GP (13) */
    FAULT_INJECT_PF_READ,   /* read an unmapped canonical page   -> #PF (14) */
    FAULT_INJECT_PF_WRITE,  /* write one                         -> #PF (14) */
    FAULT_INJECT_PF_EXEC    /* call into one                     -> #PF (14) */
} fault_inject_kind_t;

/* Raise the requested exception, from three stack frames down so the backtrace
 * has something real to reconstruct. Returns only for FAULT_INJECT_NONE and for
 * the continuable traps. */
void fault_inject(fault_inject_kind_t kind);

/* Name for a kind, for logging. Never NULL. */
const char *fault_inject_name(fault_inject_kind_t kind);

#endif /* FAULT_H */
