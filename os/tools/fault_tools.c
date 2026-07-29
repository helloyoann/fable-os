/* fault_tools.c — the fault-diagnosis and fault-recovery tool family.
 *
 * PURPOSE
 *   Let the model see what the CPU saw, and — since Phase 3b — let it decide
 *   what the machine should do about the next one. When this machine takes an
 *   exception, arch/x86_64/idt.c captures the whole machine state and renders a
 *   report; these tools hand that report, the decoded semantics behind it, and
 *   the ability to arm a recovery, to the model.
 *
 * THE TOOL SET
 *   fault_status        Has anything faulted? How many times, how long ago, and
 *                       what was it. Also reports whether the IDT is installed
 *                       at all, because "no faults" and "faults cannot be
 *                       caught" are very different answers to the same question
 *                       and only one of them is good news. Now also states what
 *                       recovery plan is armed and whether the machine is
 *                       repeating a fault at one address.
 *   fault_report        The full captured report for a fault: registers,
 *                       decoded error code, faulting instruction bytes,
 *                       backtrace, and what was decided. For the most recent
 *                       fault this is byte-identical to what the kernel printed
 *                       on the console — it is literally the same buffer. Older
 *                       faults are named by "seq" and re-rendered from the ring.
 *   fault_history       One line per retained fault. The index into the ring
 *                       the model needs before it can ask for a specific report.
 *   fault_decode        Decode an arbitrary (vector, error_code) pair without
 *                       any fault having happened, so the model can reason about
 *                       a code it read in a log or a manual.
 *   fault_recover       ARMS A RECOVERY DECISION. The only mutating tool here,
 *                       and the only tool in this kernel that can change what
 *                       the CPU executes next. TOOL_MUTATES.
 *
 * ============================================================================
 * fault_recover: the model steering a CPU, and what stands in the way
 * ============================================================================
 *   Phase 3a deliberately shipped no recovery, on the grounds that writing to a
 *   live fault_frame_t and returning into it is strictly worse than halting if
 *   any field is wrong: the machine does not fault again, it executes something
 *   arbitrary with no record of why. That argument was right, and none of it has
 *   been repealed. What changed is that the frame is no longer patched on trust.
 *
 *   The decision is armed BEFORE a fault, never during one — a handler running
 *   with IF clear, on the interrupted stack, cannot go and ask the model
 *   anything. So this tool writes a fault_plan_t and nothing else; every field
 *   is re-validated inside the exception handler against the live frame, and the
 *   plan is refused there if anything does not hold. See fault.h for the five
 *   independent checks (#DF is never recoverable, every produced RIP must be
 *   inside the loaded image, "skip" needs a certain instruction length, plans
 *   are consumed by use, and repeated faults at one RIP stop the machine).
 *
 *   This tool's own job is the front line of that: reject anything malformed
 *   loudly and specifically, so the model gets a sentence it can act on rather
 *   than a fault it caused. Unknown keys are refused rather than ignored,
 *   because a plan the model believes it armed and the kernel silently dropped
 *   half of is the worst outcome available.
 *
 *   The injector remains unreachable from here. arch/x86_64/fault_inject.c
 *   registers no tool and is called from nowhere in a normal build: a machine
 *   whose only interface is natural language must not have a "crash yourself"
 *   verb one hallucination away from the model. Arming a recovery is safe
 *   precisely because it cannot cause a fault; it can only change the response
 *   to one that happens anyway.
 *
 *   Nothing here allocates. The whole family has to remain usable when the
 *   thing being diagnosed is the allocator, which is the same constraint
 *   tools/mem_tools.c works under and for the same reason.
 *
 * WHY THE RING MATTERS TO THIS FILE SPECIFICALLY
 *   fault_report used to hand back a pointer into the kernel's one static
 *   record. That was safe only while faults were terminal. Now that they are
 *   not, a fault taken during a fault_report call would have rewritten the
 *   record under its own caller — the answer would begin describing fault #1
 *   and end describing fault #2, with no seam and no way to tell. Records live
 *   in a ring keyed by seq, and this file always names the record it wants.
 *
 * WHAT PAIRS WITH THIS
 *   fault_report gives RIP, the bytes at RIP, and a backtrace. mem_read
 *   (tools/mem_tools.c) accepts exactly the window those addresses live in, so
 *   the model can follow a backtrace entry and read the code around it. That
 *   pairing is the actual diagnostic loop, and it is why mem_read's window was
 *   drawn where it was.
 *
 * UNTRUSTED INPUT
 *   fault_status and fault_history ignore their input entirely — the most
 *   robust possible parse. fault_report accepts an optional integer "seq".
 *   fault_decode validates through json.h only: "vector" must be an integer in
 *   [0, 255], "error_code" may be a bounded hex/decimal string or a
 *   non-negative integer and defaults to 0. fault_recover validates every field
 *   twice: for shape here, and for effect inside the handler.
 *
 * OUTPUT BOUNDS — AND A KNOWN SHORTFALL, MEASURED, NOT GUESSED
 *   These tools write into whatever buffer the caller supplies. Two different
 *   numbers are in play and they do not agree:
 *
 *     TOOL_RESULT_MAX      4096   include/tool.h, the family's nominal budget,
 *                                 and what FAULT_REPORT_MAX is sized against
 *                                 (include/fault.h: "fits TOOL_RESULT_MAX")
 *     CHAT_TOOL_RESULT_CAP 1024   include/chat.h — what net/chat.c ACTUALLY
 *                                 passes for every tool call in production
 *
 *   The longest report this file can emit is 1629 bytes (measured, and printed
 *   by tests/host/test_fault.c's "longest report:" line). So in the shipped
 *   kernel a full fault_report is cut at 1023 bytes and tool_dispatch overwrites
 *   the tail with its truncation notice — losing the end of the register block,
 *   `code @RIP`, the backtrace and the disposition, which is most of the
 *   diagnosis. fault_status after a fault is in the same position.
 *
 *   This is not fixed here because every available fix is a behaviour change or
 *   sits in another owner's file: raise CHAT_TOOL_RESULT_CAP (include/chat.h),
 *   or give fault_report an offset/lines argument the way read_file and
 *   driver_trace already have. Note also that the host tests build their result
 *   buffer from TOOL_RESULT_MAX, so they cannot see this — they measure a
 *   machine that does not ship. Recorded so the next reader does not conclude
 *   from FAULT_REPORT_MAX that the report fits.
 */

#include "tool.h"
#include "trace.h"
#include "json.h"
#include "fault.h"

#include <stdint.h>
#include <stddef.h>

/* On the host, Mach-O rejects a bare section name, so REGISTER_TOOL cannot
 * expand. Export a named pointer per tool instead; tests/host/test_fault.c
 * stands in for the linker. Kernel builds are untouched. Same shim as
 * tools/mem_tools.c. */
#ifdef TALKOS_HOSTTEST
#undef  REGISTER_TOOL
#define REGISTER_TOOL(var) const tool_t *const talkos_hosttool_##var = &(var)
#else
#include "idt.h"
#endif

/* The IDT only exists in a kernel build; on the host, report it as absent so
 * the "no IDT" branch of fault_status is exercised by the tests too. */
static int idt_is_installed(void) {
#ifdef TALKOS_HOSTTEST
    return 0;
#else
    return idt_installed();
#endif
}

/* Describe the installed table, read back from the descriptor the kernel
 * actually handed to LIDT rather than from a compile-time constant — the same
 * discipline mem_map applies to the memory window. */
static void idt_describe(tool_result_t *r) {
#ifndef TALKOS_HOSTTEST
    if (!idt_installed()) return;
    tool_result_printf(r,
        "  table      : 256 gates at 0x%016lx, limit %u, kernel code selector "
        "0x%x\n"
        "  IST stacks : #DF, NMI and #MC are delivered on dedicated stacks, so "
        "a fault with an unusable RSP is still reported instead of triple-"
        "faulting\n"
        "  IRQs       : both 8259 PICs are remapped clear of the exception "
        "vectors and every line is masked; this machine polls its devices\n",
        (unsigned long)idt_base(), (unsigned)idt_limit(),
        (unsigned)SEL_KERNEL_CODE);
#else
    (void)r;
#endif
}

#define FAULT_EC_TEXT_MAX 34    /* "0x" + 16 hex digits + slack + NUL */

/* ====================================================================== */
/* shared helpers                                                         */
/* ====================================================================== */

/* Guarantee the model is told when output was clipped, by overwriting the tail
 * of a buffer that is by definition already full. Mirrors mem_tools.c. */
static void mark_truncated(tool_result_t *r) {
    static const char note[] = "\n[output truncated]";
    if (!r || !r->truncated || !r->buf || r->cap == 0) return;
    size_t n = sizeof note - 1;
    if (r->cap < n + 2) {
        size_t k = 0;
        if (r->cap > 1) r->buf[k++] = '!';
        r->buf[k] = '\0';
        r->len    = k;
        return;
    }
    size_t at = r->cap - 1 - n;
    for (size_t i = 0; i < n; i++) r->buf[at + i] = note[i];
    r->buf[r->cap - 1] = '\0';
    r->len = r->cap - 1;
}

/* Parse a bounded, NUL-terminated integer literal: optional whitespace,
 * optional "0x"/"0X" (hex) else decimal, at least one digit, optional trailing
 * whitespace, nothing else. Overflow is rejected, never wrapped. */
static int parse_u64(const char *s, uint64_t *out) {
    if (!s) return -1;
    size_t i = 0;
    while (s[i] == ' ' || s[i] == '\t') i++;

    unsigned base = 10;
    if (s[i] == '0' && (s[i + 1] == 'x' || s[i + 1] == 'X')) { base = 16; i += 2; }

    uint64_t v      = 0;
    int      digits = 0;
    for (; s[i]; i++) {
        unsigned d;
        char     c = s[i];
        if      (c >= '0' && c <= '9') d = (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f') d = (unsigned)(c - 'a') + 10u;
        else if (c >= 'A' && c <= 'F') d = (unsigned)(c - 'A') + 10u;
        else break;
        if (d >= base) break;
        if (v > (UINT64_MAX - d) / base) return -1;
        v = v * base + d;
        digits++;
    }
    if (digits == 0) return -1;
    while (s[i] == ' ' || s[i] == '\t') i++;
    if (s[i] != '\0') return -1;
    *out = v;
    return 0;
}

/* Describe the armed plan, or say plainly that there isn't one. Shared by
 * fault_status and fault_recover so the two can never describe the same state
 * differently. */
static void describe_plan(tool_result_t *r) {
    const fault_plan_t *p = fault_plan_get();
    if (!p) {
        tool_result_printf(r,
            "recovery plan   : none armed. The next fatal exception will print "
            "its report and halt the machine.\n");
        return;
    }
    switch (p->action) {
        case FAULT_ACT_SKIP:
            tool_result_printf(r,
                "recovery plan   : skip the faulting instruction\n");
            break;
        case FAULT_ACT_SET_REG:
            tool_result_printf(r,
                "recovery plan   : set %s = 0x%lx, then %s\n",
                fault_reg_name((int)p->reg), (unsigned long)p->value,
                p->then_skip ? "skip the faulting instruction"
                             : "re-execute the faulting instruction");
            break;
        case FAULT_ACT_SET_RIP:
            tool_result_printf(r,
                "recovery plan   : resume at 0x%016lx\n",
                (unsigned long)p->value);
            break;
        case FAULT_ACT_REFUSE:
            tool_result_printf(r,
                "recovery plan   : refuse - halt deliberately rather than "
                "guess\n");
            break;
        default:
            tool_result_printf(r, "recovery plan   : (unknown action %u)\n",
                               (unsigned)p->action);
            break;
    }
    tool_result_printf(r,
        "  applies to    : %s\n"
        "  uses left     : %lu\n",
        p->any_vector ? "any recoverable exception"
                      : fault_vector_name(p->vector),
        (unsigned long)p->uses);
}

/* The bounds every resume address is judged against. Worth showing: a model
 * proposing a set_rip has no other way to know what will be accepted. */
static void describe_window(tool_result_t *r) {
    const fault_window_t *w = fault_window();
    if (!w) {
        tool_result_printf(r,
            "resume window   : not published (no IDT on this build), so a "
            "set_rip plan cannot be validated and will be refused\n");
        return;
    }
    tool_result_printf(r,
        "resume window   : [0x%016lx, 0x%016lx) - a recovery may only resume "
        "at an address inside the loaded kernel image\n",
        (unsigned long)w->code_lo, (unsigned long)w->code_hi);
}

/* ====================================================================== */
/* fault_status                                                           */
/* ====================================================================== */

static int t_fault_status(const tool_call_t *call, tool_result_t *r) {
    (void)call;                     /* no arguments: nothing to misparse */

    int installed = idt_is_installed();
    const fault_record_t *rec = fault_last();
    uint32_t n = fault_count();

    tool_result_printf(r,
        "exception handling: %s\n",
        installed
            ? "IDT installed - CPU exceptions are captured and reported"
            : "NO IDT - a CPU exception would triple-fault and reset this "
              "machine with no diagnostic");
    idt_describe(r);

    if (!rec) {
        tool_result_printf(r,
            "faults since boot: 0\n"
            "No CPU exception has been captured. Nothing has gone wrong at the "
            "hardware level.\n");
        describe_window(r);
        describe_plan(r);
        mark_truncated(r);
        trace_ret(0, "fault_status", "faults=0 idt=%s", installed ? "up" : "down");
        return TOOL_OK;
    }

    char decoded[320];
    fault_decode_error(rec->regs.vector, rec->regs.error_code,
                       decoded, sizeof decoded);

    tool_result_printf(r,
        "faults since boot: %lu\n"
        "last fault (#%lu, at %lu ms since boot):\n"
        "  vector     : %lu %s - %s\n"
        "  error code : 0x%lx (%s)\n"
        "  meaning    : %s\n"
        "  RIP        : 0x%016lx\n"
        "  CR2        : 0x%016lx%s\n"
        "  disposition: %s\n",
        (unsigned long)n,
        (unsigned long)rec->seq, (unsigned long)rec->uptime_ms,
        (unsigned long)rec->regs.vector,
        fault_vector_name(rec->regs.vector),
        fault_vector_desc(rec->regs.vector),
        (unsigned long)rec->regs.error_code,
        fault_has_error_code(rec->regs.vector) ? "pushed by the CPU"
                                               : "synthesised, carries no info",
        decoded,
        (unsigned long)rec->regs.rip,
        (unsigned long)rec->regs.cr2,
        rec->regs.vector == 14 ? "  <-- the address that faulted"
                               : "  (stale; only #PF sets it)",
        rec->fix_set
            ? (rec->recovered ? "recovered - execution resumed"
                              : "fatal - the machine halted after reporting")
            : (rec->fatal ? "fatal - the machine halted after reporting"
                          : "continuable - execution resumed"));

    if (rec->fix_set) {
        tool_result_printf(r, "  outcome    : %s - %s\n",
                           fault_fix_name(rec->fix), fault_fix_desc(rec->fix));
        if (rec->fix_detail[0])
            tool_result_printf(r, "             : %s\n", rec->fix_detail);
    }

    /* A machine stuck re-faulting at one address is the single most important
     * thing to say out loud: it means the last recovery did not work, and the
     * kernel is counting down to giving up. */
    if (fault_refaults() > 1)
        tool_result_printf(r,
            "  REPEATING  : 0x%016lx has faulted %lu times in a row; the kernel "
            "gives up after %u recovery attempts at one address\n",
            (unsigned long)fault_refault_rip(), (unsigned long)fault_refaults(),
            (unsigned)FAULT_REFAULT_MAX);

    tool_result_printf(r,
        "records kept    : %lu of the last %u faults are still readable by seq\n",
        (unsigned long)fault_ring_len(), (unsigned)FAULT_RING_SIZE);
    describe_window(r);
    describe_plan(r);
    tool_result_printf(r,
        "Call fault_report for the full register dump, the faulting "
        "instruction bytes and the backtrace.\n");

    mark_truncated(r);
    trace_ret((long)n, "fault_status", "last=%s rip=0x%lx",
              fault_vector_name(rec->regs.vector),
              (unsigned long)rec->regs.rip);
    return TOOL_OK;
}

static const tool_t fault_status_tool = {
    .name        = "fault_status",
    .description =
        "Report whether this machine has taken a CPU exception (page fault, "
        "invalid opcode, general protection fault, divide by zero, ...) since "
        "boot, and summarise the most recent one: vector, decoded error code, "
        "faulting instruction pointer, faulting address, and whether it was "
        "fatal. Also states whether the interrupt descriptor table is installed "
        "at all. Read-only. Takes no arguments.",
    .input_schema = "{\"type\":\"object\",\"properties\":{}}",
    .flags        = 0,
    .invoke       = t_fault_status,
};
REGISTER_TOOL(fault_status_tool);

/* ====================================================================== */
/* fault_report                                                           */
/* ====================================================================== */

/* Scratch for rendering a record that is not the newest. Static because this
 * whole family must work when the heap is the thing under suspicion, and 3.5 KiB
 * of stack inside a tool call is not a trade worth making either.
 *
 * Note what this buffer is NOT: it is not where the kernel keeps the fault. A
 * fault taken during this call renders into the ring, not into here, and the
 * record being read lives in the ring too — so the answer this tool is halfway
 * through building cannot change identity underneath it. That property is the
 * reason the ring exists. */
static char report_scratch[FAULT_REPORT_MAX];

static int t_fault_report(const tool_call_t *call, tool_result_t *r) {
    const fault_record_t *newest = fault_last();

    if (!newest) {
        tool_result_printf(r,
            "No fault has been captured since boot, so there is no report. "
            "fault_status confirms whether exception handling is even "
            "installed.\n");
        mark_truncated(r);
        trace_ret(0, "fault_report", "faults=0");
        return TOOL_OK;
    }

    /* ---- optional "seq" ---- */
    const fault_record_t *rec = newest;
    if (call && call->input && call->input_len) {
        json_value_t root, sv;
        if (json_parse(call->input, call->input_len, &root) == JSON_OK &&
            root.type == JSON_OBJECT &&
            json_get(&root, "seq", &sv) == JSON_OK) {
            int64_t s;
            if (sv.type != JSON_NUMBER || json_int(&sv, &s) != JSON_OK ||
                s <= 0 || s > (int64_t)0xFFFFFFFF) {
                r->is_error = 1;
                tool_result_printf(r,
                    "fault_report refused: \"seq\" must be a positive integer "
                    "naming a fault. fault_history lists the ones still "
                    "retained.\n");
                mark_truncated(r);
                trace_err(TOOL_EINVAL, "fault_report", "bad seq");
                return TOOL_OK;
            }
            rec = fault_by_seq((uint32_t)s);
            if (!rec) {
                r->is_error = 1;
                tool_result_printf(r,
                    "fault #%lu is not retained. Only the last %u faults are "
                    "kept; %lu are readable right now and the newest is #%lu. "
                    "Call fault_history to see them.\n",
                    (unsigned long)s, (unsigned)FAULT_RING_SIZE,
                    (unsigned long)fault_ring_len(),
                    (unsigned long)newest->seq);
                mark_truncated(r);
                trace_err(TOOL_ENOENT, "fault_report", "seq=%lu evicted",
                          (unsigned long)s);
                return TOOL_OK;
            }
        }
    }

    if (rec == newest) {
        /* The exact bytes the kernel printed to the console when the fault
         * happened. Not a re-render: the same static buffer. */
        tool_result_printf(r, "%s", fault_last_report());
    } else {
        fault_format_report(rec, report_scratch, sizeof report_scratch);
        tool_result_printf(r, "%s", report_scratch);
    }
    mark_truncated(r);

    trace_ret((long)rec->seq, "fault_report", "vector=%lu rip=0x%lx",
              (unsigned long)rec->regs.vector, (unsigned long)rec->regs.rip);
    return TOOL_OK;
}

static const tool_t fault_report_tool = {
    .name        = "fault_report",
    .description =
        "Return the full crash report for a CPU exception: the exception name, "
        "the decoded error code, CR2, RIP/CS/RSP/SS/RFLAGS, every "
        "general-purpose register, the raw bytes of the faulting instruction, a "
        "best-effort backtrace, and what the kernel decided to do about it. "
        "With no arguments it reports the most recent fault, byte-for-byte what "
        "the kernel printed to the console. Pass \"seq\" to read an earlier "
        "fault that is still retained (see fault_history). Use it to diagnose a "
        "crash; pair it with mem_read to inspect the code around any address it "
        "reports. Read-only.",
    .input_schema =
        "{\"type\":\"object\","
        "\"properties\":{"
        "\"seq\":{\"type\":\"integer\",\"minimum\":1,"
        "\"description\":\"Which fault to report, as shown by fault_history. "
        "Omit for the most recent.\"}}}",
    .flags        = 0,
    .invoke       = t_fault_report,
};
REGISTER_TOOL(fault_report_tool);

/* ====================================================================== */
/* fault_history                                                          */
/* ====================================================================== */

static int t_fault_history(const tool_call_t *call, tool_result_t *r) {
    (void)call;

    uint32_t kept = fault_ring_len();
    if (kept == 0) {
        tool_result_printf(r,
            "No CPU exception has been captured since boot.\n");
        mark_truncated(r);
        trace_ret(0, "fault_history", "kept=0");
        return TOOL_OK;
    }

    tool_result_printf(r,
        "%lu fault(s) since boot; the last %lu are still readable "
        "(the ring holds %u).\n"
        "newest first:\n",
        (unsigned long)fault_count(), (unsigned long)kept,
        (unsigned)FAULT_RING_SIZE);

    for (uint32_t i = 0; i < kept; i++) {
        const fault_record_t *rec = fault_ring_at(i);
        if (!rec) continue;
        tool_result_printf(r,
            "  #%lu  %-4s vector %lu  err 0x%lx  RIP 0x%016lx  at %lu ms  -> %s\n",
            (unsigned long)rec->seq,
            fault_vector_name(rec->regs.vector),
            (unsigned long)rec->regs.vector,
            (unsigned long)rec->regs.error_code,
            (unsigned long)rec->regs.rip,
            (unsigned long)rec->uptime_ms,
            rec->fix_set ? fault_fix_name(rec->fix) : "not decided");
    }
    tool_result_printf(r,
        "Call fault_report with a \"seq\" to read any of these in full.\n");

    mark_truncated(r);
    trace_ret((long)kept, "fault_history", "kept=%lu total=%lu",
              (unsigned long)kept, (unsigned long)fault_count());
    return TOOL_OK;
}

static const tool_t fault_history_tool = {
    .name        = "fault_history",
    .description =
        "List the CPU exceptions this machine has taken and still remembers, "
        "newest first: sequence number, exception, error code, faulting "
        "instruction pointer, time since boot, and what was decided about each "
        "one. Only the last few faults are retained; use the sequence numbers "
        "with fault_report to read any of them in full. Read-only. Takes no "
        "arguments.",
    .input_schema = "{\"type\":\"object\",\"properties\":{}}",
    .flags        = 0,
    .invoke       = t_fault_history,
};
REGISTER_TOOL(fault_history_tool);

/* ====================================================================== */
/* fault_decode                                                           */
/* ====================================================================== */

static int decode_fail(tool_result_t *r, const char *why) {
    r->is_error = 1;
    tool_result_printf(r,
        "fault_decode refused: %s. Expected an object like "
        "{\"vector\":14,\"error_code\":\"0x2\"}: vector is an integer from 0 to "
        "255, error_code is a hex (0x-prefixed) or decimal string, or a "
        "non-negative integer, and defaults to 0.\n", why);
    mark_truncated(r);
    trace_err(TOOL_EINVAL, "fault_decode", "%s", why);
    return TOOL_OK;
}

static int t_fault_decode(const tool_call_t *call, tool_result_t *r) {
    if (!call || !call->input || call->input_len == 0)
        return decode_fail(r, "no arguments given");

    json_value_t root;
    if (json_parse(call->input, call->input_len, &root) != JSON_OK)
        return decode_fail(r, "arguments are not valid JSON");
    if (root.type != JSON_OBJECT)
        return decode_fail(r, "arguments are not a JSON object");

    /* ---- vector: required integer in [0, 255] ---- */
    json_value_t vv;
    if (json_get(&root, "vector", &vv) != JSON_OK)
        return decode_fail(r, "missing required field \"vector\"");
    if (vv.type != JSON_NUMBER)
        return decode_fail(r, "\"vector\" must be an integer");
    int64_t vi;
    if (json_int(&vv, &vi) != JSON_OK)
        return decode_fail(r, "\"vector\" is not a representable integer");
    if (vi < 0 || vi > 255)
        return decode_fail(r, "\"vector\" is outside the range 0..255");
    uint64_t vector = (uint64_t)vi;

    /* ---- error_code: optional, string or non-negative integer ---- */
    uint64_t     ec = 0;
    json_value_t ev;
    int          erc = json_get(&root, "error_code", &ev);
    if (erc == JSON_OK) {
        if (ev.type == JSON_STRING) {
            char text[FAULT_EC_TEXT_MAX];
            if (json_str(&ev, text, sizeof text, NULL) != JSON_OK)
                return decode_fail(r, "\"error_code\" string is too long");
            if (parse_u64(text, &ec) != 0)
                return decode_fail(r, "\"error_code\" is not a valid number");
        } else if (ev.type == JSON_NUMBER) {
            int64_t ei;
            if (json_int(&ev, &ei) != JSON_OK)
                return decode_fail(r, "\"error_code\" is not an integer");
            if (ei < 0)
                return decode_fail(r, "\"error_code\" is negative");
            ec = (uint64_t)ei;
        } else {
            return decode_fail(r,
                "\"error_code\" must be a string or an integer");
        }
    } else if (erc != JSON_ENOENT) {
        return decode_fail(r, "\"error_code\" could not be read");
    }

    char decoded[320];
    fault_decode_error(vector, ec, decoded, sizeof decoded);

    tool_result_printf(r,
        "vector %lu = %s (%s)\n"
        "error code 0x%lx: %s\n"
        "the CPU %s an error code for this vector\n"
        "a fault on this vector is %s in the current kernel\n",
        (unsigned long)vector, fault_vector_name(vector),
        fault_vector_desc(vector),
        (unsigned long)ec, decoded,
        fault_has_error_code(vector) ? "pushes" : "does NOT push",
        fault_is_fatal(vector) ? "fatal (the machine halts after reporting)"
                               : "continuable (execution resumes)");
    mark_truncated(r);

    trace_ret((long)vector, "fault_decode", "vector=%lu err=0x%lx",
              (unsigned long)vector, (unsigned long)ec);
    return TOOL_OK;
}

static const tool_t fault_decode_tool = {
    .name        = "fault_decode",
    .description =
        "Explain what an x86-64 exception vector and error code mean, without "
        "needing a fault to have happened. Names the exception, spells out the "
        "error code (for a page fault: present/write/user/reserved-bit/"
        "instruction-fetch; for a protection fault: the selector and which "
        "descriptor table), and says whether the CPU pushes an error code for "
        "that vector at all. Read-only and side-effect free.",
    .input_schema =
        "{\"type\":\"object\","
        "\"properties\":{"
        "\"vector\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":255,"
        "\"description\":\"Exception vector, e.g. 14 for a page fault.\"},"
        "\"error_code\":{\"type\":\"string\","
        "\"description\":\"Error code. Hex with a 0x prefix (e.g. 0x2) or plain "
        "decimal. Defaults to 0.\"}},"
        "\"required\":[\"vector\"]}",
    .flags        = 0,
    .invoke       = t_fault_decode,
};
REGISTER_TOOL(fault_decode_tool);

/* ====================================================================== */
/* fault_recover — the one tool here that changes anything                */
/* ====================================================================== */

#define FAULT_WORD_MAX 24     /* longest accepted field name or enum word */

static int streq(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == '\0' && *b == '\0';
}

static const char RECOVER_USAGE[] =
    "Expected an object like {\"action\":\"skip\"} or "
    "{\"action\":\"set_register\",\"register\":\"rax\",\"value\":\"0x0\","
    "\"then\":\"skip\"}.\n"
    "  action     (required) one of: skip, set_register, set_rip, refuse, none\n"
    "  register   set_register only: rax rcx rdx rbx rbp rsi rdi r8..r15 "
    "(rsp is refused)\n"
    "  value      set_register / set_rip only: hex \"0x...\" or decimal, or a "
    "non-negative integer\n"
    "  then       set_register only: \"retry\" (default) or \"skip\"\n"
    "  vector     optional: apply only to this exception vector, 0..255\n"
    "  uses       optional: how many faults this plan may handle, 1..16, "
    "default 1\n";

/* `extra` is echoed model input; it is already bounded to FAULT_WORD_MAX by
 * every caller, and trace.c sanitises what reaches the trace line. */
static int recover_fail(tool_result_t *r, const char *why, const char *extra) {
    r->is_error = 1;
    tool_result_printf(r, "fault_recover refused: %s%s%s%s.\n%s",
                       why,
                       extra ? " (\"" : "", extra ? extra : "",
                       extra ? "\")"  : "",
                       RECOVER_USAGE);
    mark_truncated(r);
    trace_err(TOOL_EINVAL, "fault_recover", "%s%s%s", why,
              extra ? " " : "", extra ? extra : "");
    return TOOL_OK;
}

/* Read an optional field as a 64-bit value: a bounded hex/decimal string, or a
 * non-negative JSON integer. Returns 0 on success, -1 on a bad value, and
 * leaves *out untouched when the field is absent (*present is then 0). */
static int read_u64_field(const json_value_t *root, const char *key,
                          uint64_t *out, int *present) {
    json_value_t v;
    *present = 0;
    int rc = json_get(root, key, &v);
    if (rc == JSON_ENOENT) return 0;
    if (rc != JSON_OK)     return -1;
    *present = 1;

    if (v.type == JSON_STRING) {
        char text[FAULT_EC_TEXT_MAX];
        if (json_str(&v, text, sizeof text, NULL) != JSON_OK) return -1;
        return parse_u64(text, out);
    }
    if (v.type == JSON_NUMBER) {
        int64_t i;
        if (json_int(&v, &i) != JSON_OK) return -1;
        if (i < 0) return -1;
        *out = (uint64_t)i;
        return 0;
    }
    return -1;
}

/* Read an optional short string field into `dst`. Returns 1 present, 0 absent,
 * -1 malformed or too long. */
static int read_word_field(const json_value_t *root, const char *key,
                           char *dst, size_t cap) {
    json_value_t v;
    int rc = json_get(root, key, &v);
    if (rc == JSON_ENOENT) return 0;
    if (rc != JSON_OK)     return -1;
    if (v.type != JSON_STRING) return -1;
    if (json_str(&v, dst, cap, NULL) != JSON_OK) return -1;
    return 1;
}

static int t_fault_recover(const tool_call_t *call, tool_result_t *r) {
    if (!call || !call->input || call->input_len == 0)
        return recover_fail(r, "no arguments given", NULL);

    json_value_t root;
    if (json_parse(call->input, call->input_len, &root) != JSON_OK)
        return recover_fail(r, "arguments are not valid JSON", NULL);
    if (root.type != JSON_OBJECT)
        return recover_fail(r, "arguments are not a JSON object", NULL);

    /* ---- every key must be one this tool understands ----
     * Ignoring an unrecognised field is the failure mode to avoid here: the
     * model would believe it armed {"action":"set_register","reg":"rax"} and
     * the kernel would arm a set_register with no register. Refuse instead, and
     * name the field, so the next turn can fix it. */
    static const char *const allowed[] = {
        "action", "register", "value", "then", "vector", "uses"
    };
    size_t nmemb = json_count(&root);
    for (size_t i = 0; i < nmemb; i++) {
        json_value_t k;
        char         name[FAULT_WORD_MAX];
        if (json_member(&root, i, &k, NULL) != JSON_OK)
            return recover_fail(r, "the arguments object could not be read",
                                NULL);
        if (json_str(&k, name, sizeof name, NULL) != JSON_OK)
            return recover_fail(r, "an argument name is not one of the "
                                   "accepted fields", NULL);
        int known = 0;
        for (unsigned j = 0; j < sizeof allowed / sizeof allowed[0]; j++)
            if (streq(name, allowed[j])) known = 1;
        if (!known)
            return recover_fail(r, "unknown field", name);
    }

    /* ---- action: required ---- */
    char action_word[FAULT_WORD_MAX];
    int  arc = read_word_field(&root, "action", action_word,
                               sizeof action_word);
    if (arc < 0)
        return recover_fail(r, "\"action\" must be one of the accepted words",
                            NULL);
    if (arc == 0)
        return recover_fail(r, "missing required field \"action\"", NULL);

    fault_plan_t plan;
    for (unsigned i = 0; i < sizeof plan; i++) ((unsigned char *)&plan)[i] = 0;

    int wants_reg = 0, wants_value = 0;
    if      (streq(action_word, "none"))         plan.action = FAULT_ACT_NONE;
    else if (streq(action_word, "refuse"))       plan.action = FAULT_ACT_REFUSE;
    else if (streq(action_word, "skip"))         plan.action = FAULT_ACT_SKIP;
    else if (streq(action_word, "set_register")) { plan.action = FAULT_ACT_SET_REG;
                                                   wants_reg = wants_value = 1; }
    else if (streq(action_word, "set_rip"))      { plan.action = FAULT_ACT_SET_RIP;
                                                   wants_value = 1; }
    else return recover_fail(r, "unknown action", action_word);

    /* ---- "none" disarms, and takes nothing else ---- */
    if (plan.action == FAULT_ACT_NONE) {
        if (nmemb != 1)
            return recover_fail(r,
                "\"action\":\"none\" clears the plan and accepts no other "
                "fields", NULL);
        fault_plan_clear();
        tool_result_printf(r,
            "Recovery plan cleared. The next fatal CPU exception will print its "
            "report and halt the machine.\n");
        mark_truncated(r);
        trace_ret(0, "fault_recover", "cleared");
        return TOOL_OK;
    }

    /* ---- fields that only some actions accept ---- */
    json_value_t probe;
    if (!wants_reg && json_get(&root, "register", &probe) == JSON_OK)
        return recover_fail(r,
            "\"register\" is only meaningful with \"action\":\"set_register\"",
            NULL);
    if (!wants_reg && json_get(&root, "then", &probe) == JSON_OK)
        return recover_fail(r,
            "\"then\" is only meaningful with \"action\":\"set_register\"",
            NULL);
    if (!wants_value && json_get(&root, "value", &probe) == JSON_OK)
        return recover_fail(r,
            "\"value\" is only meaningful with \"action\":\"set_register\" or "
            "\"set_rip\"", NULL);

    /* ---- register ---- */
    if (wants_reg) {
        char reg_word[FAULT_WORD_MAX];
        int  rrc = read_word_field(&root, "register", reg_word,
                                   sizeof reg_word);
        if (rrc < 0)
            return recover_fail(r, "\"register\" must be a register name",
                                NULL);
        if (rrc == 0)
            return recover_fail(r,
                "\"action\":\"set_register\" needs a \"register\"", NULL);
        int reg = fault_reg_lookup(reg_word);
        if (reg < 0)
            return recover_fail(r, "not a 64-bit general-purpose register",
                                reg_word);
        if (reg == FAULT_REG_RSP)
            return recover_fail(r,
                "rsp may not be written from a recovery plan: the saved RSP is "
                "what IRETQ reloads, and a wrong value there is exactly the "
                "unusable stack that causes a double fault", NULL);
        plan.reg = (uint8_t)reg;
    }

    /* ---- value ---- */
    if (wants_value) {
        int present = 0;
        if (read_u64_field(&root, "value", &plan.value, &present) != 0)
            return recover_fail(r,
                "\"value\" must be a hex (0x-prefixed) or decimal string, or a "
                "non-negative integer", NULL);
        if (!present)
            return recover_fail(r, "this action needs a \"value\"", NULL);
    }

    /* ---- then: retry (default) or skip ---- */
    if (wants_reg) {
        char then_word[FAULT_WORD_MAX];
        int  trc = read_word_field(&root, "then", then_word, sizeof then_word);
        if (trc < 0)
            return recover_fail(r, "\"then\" must be \"retry\" or \"skip\"",
                                NULL);
        if (trc == 0 || streq(then_word, "retry")) plan.then_skip = 0;
        else if (streq(then_word, "skip"))         plan.then_skip = 1;
        else return recover_fail(r, "\"then\" must be \"retry\" or \"skip\"",
                                 then_word);
    }

    /* ---- vector scope ---- */
    {
        uint64_t v = 0;
        int      present = 0;
        if (read_u64_field(&root, "vector", &v, &present) != 0)
            return recover_fail(r,
                "\"vector\" must be an integer from 0 to 255", NULL);
        if (present && v > 255)
            return recover_fail(r,
                "\"vector\" is outside the range 0..255", NULL);
        plan.any_vector = present ? 0u : 1u;
        plan.vector     = present ? v : 0u;
    }

    /* ---- uses ---- */
    {
        uint64_t u = 1;
        int      present = 0;
        if (read_u64_field(&root, "uses", &u, &present) != 0)
            return recover_fail(r, "\"uses\" must be a positive integer", NULL);
        if (!present) u = 1;
        if (u == 0 || u > FAULT_PLAN_USES_MAX)
            return recover_fail(r,
                "\"uses\" must be between 1 and 16: an armed plan is consumed "
                "as it is applied, so that one decision cannot silently absorb "
                "an unbounded stream of different faults", NULL);
        plan.uses = (uint32_t)u;
    }

    /* ---- arm it. Everything above checked shape; this checks effect. ---- */
    fault_fix_t rc = fault_plan_arm(&plan);
    if (rc != FAULT_FIX_OK) {
        r->is_error = 1;
        tool_result_printf(r,
            "fault_recover refused (%s): %s.\n",
            fault_fix_name(rc), fault_fix_desc(rc));
        describe_window(r);
        mark_truncated(r);
        trace_err(TOOL_EINVAL, "fault_recover", "%s", fault_fix_name(rc));
        return TOOL_OK;
    }

    tool_result_printf(r, "Recovery plan armed.\n");
    describe_plan(r);
    tool_result_printf(r,
        "The kernel re-validates every field inside the exception handler "
        "against the live frame, and refuses there if anything does not hold: "
        "#DF and #MC are never resumable, any resume address must be inside the "
        "loaded image, and \"skip\" is refused unless the instruction at RIP "
        "decodes to a certain length. After %u faults at the same address the "
        "kernel stops trying and halts.\n",
        (unsigned)FAULT_REFAULT_MAX);
    mark_truncated(r);

    trace_ret((long)plan.uses, "fault_recover", "action=%s uses=%lu%s%s",
              action_word, (unsigned long)plan.uses,
              wants_reg ? " reg=" : "",
              wants_reg ? fault_reg_name((int)plan.reg) : "");
    return TOOL_OK;
}

static const tool_t fault_recover_tool = {
    .name        = "fault_recover",
    .description =
        "Arm what this machine should do the NEXT time it takes a fatal CPU "
        "exception, instead of printing a report and halting. This changes what "
        "the CPU executes, so it is the most consequential tool available: an "
        "exception handler cannot ask a question, so the decision has to be made "
        "in advance and is applied without further confirmation.\n"
        "Actions: \"skip\" steps over the faulting instruction (refused unless "
        "its length can be decoded with certainty); \"set_register\" writes a "
        "general-purpose register and then either re-executes the instruction "
        "(\"then\":\"retry\", the default) or steps over it (\"then\":\"skip\"); "
        "\"set_rip\" resumes at another address inside the kernel image; "
        "\"refuse\" deliberately lets the machine halt; \"none\" clears the "
        "plan.\n"
        "The plan is consumed as it is applied (\"uses\", default 1) and can be "
        "limited to one exception vector (\"vector\"). Double faults and machine "
        "checks are never resumable, a resume address outside the loaded image "
        "is always refused, and after three faults in a row at the same address "
        "the kernel stops trying. Call fault_status to see what is armed.",
    .input_schema =
        "{\"type\":\"object\","
        "\"properties\":{"
        "\"action\":{\"type\":\"string\","
        "\"enum\":[\"skip\",\"set_register\",\"set_rip\",\"refuse\",\"none\"],"
        "\"description\":\"What to do when the next fatal exception fires.\"},"
        "\"register\":{\"type\":\"string\","
        "\"description\":\"With set_register: which 64-bit GPR to write, e.g. "
        "\\\"rax\\\". rsp is refused.\"},"
        "\"value\":{\"type\":\"string\","
        "\"description\":\"With set_register: the value to write. With set_rip: "
        "the address to resume at. Hex with a 0x prefix, or decimal.\"},"
        "\"then\":{\"type\":\"string\",\"enum\":[\"retry\",\"skip\"],"
        "\"description\":\"With set_register: re-execute the faulting "
        "instruction (default) or step over it.\"},"
        "\"vector\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":255,"
        "\"description\":\"Apply only to this exception vector, e.g. 14 for a "
        "page fault. Omit to apply to any recoverable exception.\"},"
        "\"uses\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":16,"
        "\"description\":\"How many faults this plan may handle before it "
        "disarms itself. Defaults to 1.\"}},"
        "\"required\":[\"action\"]}",
    .flags        = TOOL_MUTATES,
    .invoke       = t_fault_recover,
};
REGISTER_TOOL(fault_recover_tool);
