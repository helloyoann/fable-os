/* faultchat.c — the kernel asking the model to diagnose its own crash.
 *
 * See include/faultchat.h for the safe-context argument, which is the only part
 * of this that is hard. The code itself is deliberately dull: fixed buffers, one
 * request, one reply, and every interesting decision delegated to something that
 * already exists (json.h parses, fault_recover validates, fault.c decides).
 *
 * FILE LAYOUT
 *   bounded appender / sanitiser      text that cannot overflow or forge
 *   the system prompt                 what the kernel says it wants back
 *   faultchat_build_prompt            record -> everything a model needs
 *   object span finder / parse_reply  untrusted reply -> faultchat_reply_t
 *   faultchat_apply_fix               reply -> the fault_recover tool
 *   the live half                     the polling pump and its three latches
 *
 * Nothing in this file calls kmalloc, directly or indirectly, and nothing in it
 * runs inside an exception handler. Both of those are load-bearing; see the
 * header.
 */

#include "faultchat.h"

#include "json.h"
#include "kernel.h"
#include "tool.h"

#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#include <stdio.h>      /* vsnprintf — the libc shim's, when freestanding */

/* ====================================================================== */
/* bounded text                                                           */
/* ====================================================================== */

/* snprintf-style accumulation, identical in contract to arch/x86_64/fault.c's:
 * `*off` is the length that WOULD have been written, so one test at the end
 * detects overflow and every intermediate call is harmless once full. */
static void ap(char *dst, size_t cap, size_t *off, const char *fmt, ...)
    __attribute__((format(printf, 4, 5)));

static void ap(char *dst, size_t cap, size_t *off, const char *fmt, ...) {
    va_list a;
    va_start(a, fmt);
    size_t used = (*off < cap) ? *off : cap;
    int    n    = vsnprintf(dst + used, cap - used, fmt, a);
    va_end(a);
    if (n > 0) *off += (size_t)n;
}

/* Copy model-controlled text into a fixed buffer, defanged.
 *
 * Two substitutions, each for a stated reason:
 *   - control characters (and DEL) become spaces. A newline inside the model's
 *     prose would let it start a line of its own on the console.
 *   - '[' and ']' become '(' and ')'. Brackets are this kernel's mark for
 *     "the kernel is stating a fact it performed" (see trace.h). Prose that can
 *     print a bracket can forge one, and the whole audit story on a machine with
 *     no shell rests on that not being possible.
 * Bytes >= 0x80 pass through unchanged, exactly as net/chat.c passes model prose
 * through: they cannot forge anything, and mangling UTF-8 would only make a real
 * answer harder to read. Always NUL-terminates. */
static void sanitize(char *dst, size_t cap, const char *src, size_t len) {
    size_t o = 0;
    if (!dst || cap == 0) return;
    for (size_t i = 0; src && i < len && o + 1 < cap; i++) {
        unsigned char c = (unsigned char)src[i];
        if (c < 0x20 || c == 0x7F) c = ' ';
        else if (c == '[')         c = '(';
        else if (c == ']')         c = ')';
        dst[o++] = (char)c;
    }
    /* Trailing whitespace is common (models end on a newline) and looks like a
     * bug on a console that prints the string inline. */
    while (o && dst[o - 1] == ' ') o--;
    dst[o] = '\0';
}

/* True when [addr, addr+len) lies wholly inside [lo, hi). Written so addr+len
 * is never formed. Same discipline as arch/x86_64/fault.c and tools/mem_tools.c;
 * duplicated rather than exported because the alternative is widening fault.h's
 * surface for four lines. */
static int in_range(uint64_t addr, uint64_t len, uint64_t lo, uint64_t hi) {
    if (hi <= lo)        return 0;
    if (len == 0)        return 0;
    if (addr < lo)       return 0;
    if (addr >= hi)      return 0;
    if (len > hi - addr) return 0;
    return 1;
}

/* ====================================================================== */
/* what the kernel tells the model it wants                                */
/* ====================================================================== */

static const char SYSTEM_PROMPT[] =
    "You are the resident intelligence of talk-os, a bare-metal x86-64 kernel. "
    "This is not an operator turn. The KERNEL is asking you, on its own "
    "initiative, about a CPU exception it just took and survived. Nobody typed "
    "anything; there is no conversation to continue and no follow-up question "
    "you can ask.\n"
    "Reply with ONE JSON object and nothing else - no prose around it, no "
    "markdown fence:\n"
    "{\"diagnosis\":\"one or two plain sentences: what the machine was doing "
    "and why it faulted\","
    "\"fix\":{\"action\":\"skip\",\"vector\":14,\"uses\":1}}\n"
    "\"diagnosis\" is required. \"fix\" is OPTIONAL and arms what this machine "
    "will do the NEXT time it takes a fatal exception; its fields are exactly "
    "the fault_recover tool's:\n"
    "  action    skip | set_register | set_rip | refuse | none\n"
    "  register  set_register only: rax rcx rdx rbx rbp rsi rdi r8..r15 "
    "(rsp is refused)\n"
    "  value     set_register / set_rip only: a hex string like \"0x0\"\n"
    "  then      set_register only: \"retry\" (default) or \"skip\"\n"
    "  vector    optional: apply only to this exception vector, 0..255\n"
    "  uses      optional: how many faults this plan may handle, 1..16\n"
    "OMIT \"fix\" ENTIRELY when you are not sure. A wrong fix is worse than no "
    "fix: it changes what the CPU executes next, it is applied without anyone "
    "confirming it, and this machine has no shell to recover from. A diagnosis "
    "with no fix is a useful, complete answer. So is "
    "{\"action\":\"refuse\"}, which means 'let it halt, deliberately'.";

const char *faultchat_system_prompt(void) { return SYSTEM_PROMPT; }

/* ====================================================================== */
/* the prompt                                                              */
/* ====================================================================== */

/* One line of hex bytes read live from `addr`, gated on the image window. Used
 * only for the bytes BEFORE RIP: the bytes at and after RIP come from the record
 * itself, which is what the CPU actually saw at fault time. */
static void hex_line(char *dst, size_t cap, size_t *off,
                     uint64_t addr, unsigned n, const fault_window_t *w) {
    if (!in_range(addr, n, w->image_lo, w->image_hi)) return;
    const volatile unsigned char *p =
        (const volatile unsigned char *)(uintptr_t)addr;
    ap(dst, cap, off, "  0x%016lx: ", (unsigned long)addr);
    for (unsigned i = 0; i < n; i++)
        ap(dst, cap, off, "%02x ", (unsigned)p[i]);
    ap(dst, cap, off, "\n");
}

#define FAULTCHAT_PRE_BYTES 32   /* bytes of instruction stream before RIP */

size_t faultchat_build_prompt(const fault_record_t *rec,
                              const fault_window_t *window,
                              char *dst, size_t cap) {
    size_t off = 0;
    if (cap) dst[0] = '\0';
    if (!rec || !rec->valid) {
        ap(dst, cap, &off, "no fault has been captured\n");
        return off;
    }

    const fault_frame_t *f = &rec->regs;

    ap(dst, cap, &off,
       "This machine took a CPU exception and SURVIVED it - the fault handler "
       "returned and normal kernel code is running again. Everything below was "
       "captured by the kernel at the moment of the fault. Diagnose it.\n");

    /* ---- machine state the report itself does not carry ---- */
    ap(dst, cap, &off, "\nMACHINE\n");
    ap(dst, cap, &off,
       "  uptime            : %lu ms\n"
       "  faults since boot : %lu (this is fault #%lu)\n",
       (unsigned long)rec->uptime_ms,
       (unsigned long)fault_count(), (unsigned long)rec->seq);

    if (fault_refaults() > 1)
        ap(dst, cap, &off,
           "  REPEATING         : 0x%016lx has now faulted %lu times in a row. "
           "The last recovery did NOT work. The kernel gives up and halts after "
           "%u attempts at one address.\n",
           (unsigned long)fault_refault_rip(), (unsigned long)fault_refaults(),
           (unsigned)FAULT_REFAULT_MAX);

    if (window)
        ap(dst, cap, &off,
           "  resume window     : [0x%016lx, 0x%016lx) - every address a "
           "recovery resumes at must be inside this range, or it is refused\n",
           (unsigned long)window->code_lo, (unsigned long)window->code_hi);
    else
        ap(dst, cap, &off,
           "  resume window     : not published on this build, so a set_rip fix "
           "cannot be validated and will be refused\n");

    ap(dst, cap, &off,
       "  vector recoverable: %s\n",
       fault_is_recoverable(f->vector)
           ? "yes"
           : "NO - this exception can never be resumed on this machine, so no "
             "fix will be accepted for it");

    /* Whether "skip" is even available is the single most actionable fact here:
     * proposing it when the length decoder will refuse wastes the one shot. */
    int ilen = (rec->code_valid && rec->code_len)
                   ? x86_insn_length(rec->code, rec->code_len) : 0;
    if (ilen > 0)
        ap(dst, cap, &off,
           "  \"skip\" available  : yes - the instruction at RIP decodes to %d "
           "bytes with certainty, so a skip would resume at 0x%016lx\n",
           ilen, (unsigned long)(f->rip + (uint64_t)ilen));
    else
        ap(dst, cap, &off,
           "  \"skip\" available  : NO - the kernel's length decoder will not "
           "commit to a length for the bytes at RIP, so an \"action\":\"skip\" "
           "fix WILL be refused. Do not propose one.\n");

    {
        const fault_plan_t *p = fault_plan_get();
        if (!p)
            ap(dst, cap, &off, "  armed plan        : none\n");
        else
            ap(dst, cap, &off,
               "  armed plan        : action %u, %lu use(s) left - proposing a "
               "fix REPLACES it\n",
               (unsigned)p->action, (unsigned long)p->uses);
    }

    /* ---- the report: vector, decoded error, RIP, registers, code, backtrace,
     * and what the kernel decided. Byte-for-byte what the console printed. ---- */
    ap(dst, cap, &off,
       "\nFAULT REPORT (verbatim, exactly as the kernel printed it on the "
       "console)\n");
    {
        size_t used = (off < cap) ? off : cap;
        size_t n    = fault_format_report(rec, dst + used, cap - used);
        off += n;
    }

    /* ---- the instruction stream leading up to RIP ----
     * Read NOW rather than at fault time, and said so: the kernel maps every
     * page RWX, so in principle the bytes could have changed since. In practice
     * they have not, and knowing what runs into the faulting instruction is
     * often what identifies the function. */
    if (window && rec->code_valid) {
        ap(dst, cap, &off,
           "\nMEMORY AROUND RIP (the preceding bytes are read now, not at fault "
           "time)\n");
        uint64_t lo = (f->rip >= FAULTCHAT_PRE_BYTES)
                          ? f->rip - FAULTCHAT_PRE_BYTES : 0;
        for (uint64_t a = lo; a + 16 <= f->rip; a += 16)
            hex_line(dst, cap, &off, a, 16, window);
        ap(dst, cap, &off, "  0x%016lx: ", (unsigned long)f->rip);
        for (unsigned i = 0; i < rec->code_len; i++)
            ap(dst, cap, &off, "%02x ", (unsigned)rec->code[i]);
        ap(dst, cap, &off, "  <-- RIP, the faulting instruction starts here\n");
    }

    /* ---- what an answer looks like, restated with this machine's numbers ---- */
    ap(dst, cap, &off,
       "\nANSWER WITH ONE JSON OBJECT\n"
       "  {\"diagnosis\":\"...\"}                       if you do not know what "
       "to do about it\n"
       "  {\"diagnosis\":\"...\",\"fix\":{\"action\":...}}   if you do\n"
       "A fix is applied without confirmation and changes what this CPU executes "
       "next. Omit it unless the report above actually tells you what to do.\n");

    return off;
}

/* ====================================================================== */
/* the reply                                                              */
/* ====================================================================== */

/* Find one brace-balanced span starting at the first '{' at or after `from`.
 *
 * This is a LEXER, not a parser: it tracks string literals and backslash escapes
 * so that a '{' inside a string cannot open a span and a '"' cannot be escaped
 * out of, and it does nothing else. The span it finds is handed to json_parse(),
 * which is the only thing in this kernel that interprets JSON. It exists because
 * models wrap answers in ```json fences and in "Here is my analysis:" preambles,
 * and refusing a correct diagnosis over its packaging would be a bad trade.
 *
 * Returns 1 and sets the half-open span [*start, *end) on success, 0 otherwise.
 */
static int find_object(const char *t, size_t len, size_t from,
                       size_t *start, size_t *end) {
    size_t i = from;
    while (i < len && t[i] != '{') i++;
    if (i >= len) return 0;

    size_t depth = 0;
    int    instr = 0, esc = 0;
    for (size_t j = i; j < len; j++) {
        char c = t[j];
        if (instr) {
            if (esc)            esc = 0;
            else if (c == '\\') esc = 1;
            else if (c == '"')  instr = 0;
            continue;
        }
        if (c == '"') { instr = 1; continue; }
        if (c == '{') depth++;
        else if (c == '}') {
            if (depth == 0) break;        /* cannot happen; never underflow */
            depth--;
            if (depth == 0) { *start = i; *end = j + 1; return 1; }
        }
    }
    return 0;
}

static void why_say(char *why, size_t cap, const char *s) {
    size_t i = 0;
    if (!why || cap == 0) return;
    for (; s && s[i] && i + 1 < cap; i++) why[i] = s[i];
    why[i] = '\0';
}

/* The concatenated text of the model's reply. Static because this module
 * allocates nothing; sized to the response buffer because a reply that is all
 * text is legal. */
static char reply_text[FAULTCHAT_RESP_BYTES];

int faultchat_parse_reply(const char *body, size_t len,
                          faultchat_reply_t *out, char *why, size_t whycap) {
    if (whycap) why_say(why, whycap, "");
    if (!out) return FAULTCHAT_EINVAL;

    out->diagnosis[0] = '\0';
    out->has_fix      = 0;
    out->fix[0]       = '\0';
    out->fix_len      = 0;

    if (!body || len == 0) {
        why_say(why, whycap, "the model returned an empty body");
        return FAULTCHAT_EPROTO;
    }

    json_value_t root;
    if (json_parse(body, len, &root) != JSON_OK) {
        why_say(why, whycap, "the response body was not valid JSON");
        return FAULTCHAT_EPROTO;
    }

    size_t tlen = 0;
    int    rc   = json_msg_text(&root, reply_text, sizeof reply_text, &tlen);
    if (rc != JSON_OK && rc != JSON_ENOSPC) {
        why_say(why, whycap,
                "the response carried no text content block to read");
        return FAULTCHAT_EPROTO;
    }
    if (tlen == 0) {
        why_say(why, whycap, "the model's reply was empty");
        return FAULTCHAT_EPROTO;
    }

    /* Fast path: the whole trimmed text is the object, which is what the system
     * prompt asks for. Fallback: the first brace-balanced span in it. */
    size_t b = 0, e = tlen;
    while (b < e && (reply_text[b] == ' ' || reply_text[b] == '\n' ||
                     reply_text[b] == '\r' || reply_text[b] == '\t')) b++;
    while (e > b && (reply_text[e - 1] == ' ' || reply_text[e - 1] == '\n' ||
                     reply_text[e - 1] == '\r' || reply_text[e - 1] == '\t')) e--;

    json_value_t obj;
    if (b >= e || json_parse(reply_text + b, e - b, &obj) != JSON_OK ||
        obj.type != JSON_OBJECT) {
        /* Try each '{' in turn, not just the first. A brace inside the prose
         * around the answer ("the {} form", a half-quoted sentence) would
         * otherwise shadow the real object, and json_parse is the arbiter of
         * which candidate is real. Bounded so a pathological reply cannot turn
         * this into a quadratic scan. */
        int    found = 0;
        size_t from  = 0;
        for (int attempt = 0; attempt < 16 && !found; attempt++) {
            size_t s2 = 0, e2 = 0;
            if (!find_object(reply_text, tlen, from, &s2, &e2)) break;
            if (json_parse(reply_text + s2, e2 - s2, &obj) == JSON_OK &&
                obj.type == JSON_OBJECT)
                found = 1;
            else
                from = s2 + 1;
        }
        if (!found) {
            why_say(why, whycap,
                    "the reply contained no JSON object - the model answered in "
                    "prose instead of the contract it was given");
            return FAULTCHAT_EPROTO;
        }
    }

    /* ---- diagnosis: required, a string, sanitised ---- */
    json_value_t dv;
    if (json_get(&obj, "diagnosis", &dv) != JSON_OK) {
        why_say(why, whycap, "the reply object has no \"diagnosis\" field");
        return FAULTCHAT_EPROTO;
    }
    if (dv.type != JSON_STRING) {
        why_say(why, whycap, "\"diagnosis\" is not a string");
        return FAULTCHAT_EPROTO;
    }
    {
        /* Decode through json.h (so \n and \" are real characters), then defang.
         * Decoding into the same fixed buffer the caller keeps means an
         * over-long diagnosis is clipped, never refused: the fix beside it may
         * still be the useful half of the answer. */
        static char raw[FAULTCHAT_DIAG_MAX * 2];
        size_t      n = 0;
        int         r = json_str(&dv, raw, sizeof raw, &n);
        if (r != JSON_OK && r != JSON_ENOSPC) {
            why_say(why, whycap, "\"diagnosis\" could not be decoded");
            return FAULTCHAT_EPROTO;
        }
        sanitize(out->diagnosis, sizeof out->diagnosis, raw, n);
    }

    /* ---- fix: optional, must be an object, copied out verbatim ----
     * Not interpreted here. The fault_recover tool is the one place that turns
     * these fields into a fault_plan_t, and it refuses unknown keys, so adding a
     * second reader would be adding a second grammar. */
    json_value_t fv;
    int frc = json_get(&obj, "fix", &fv);
    if (frc == JSON_OK && fv.type != JSON_NULL) {
        if (fv.type != JSON_OBJECT) {
            why_say(why, whycap,
                    "\"fix\" is present but is not a JSON object");
            return FAULTCHAT_EPROTO;
        }
        if (fv.len == 0 || fv.len >= sizeof out->fix) {
            why_say(why, whycap,
                    "\"fix\" is larger than any legitimate recovery plan");
            return FAULTCHAT_EPROTO;
        }
        for (size_t i = 0; i < fv.len; i++) out->fix[i] = fv.start[i];
        out->fix[fv.len] = '\0';
        out->fix_len     = fv.len;
        out->has_fix     = 1;
    }

    if (out->diagnosis[0] == '\0' && !out->has_fix) {
        why_say(why, whycap,
                "the reply parsed but said nothing: no diagnosis text and no "
                "fix");
        return FAULTCHAT_EPROTO;
    }
    return FAULTCHAT_OK;
}

/* ====================================================================== */
/* applying a proposed fix — through the one door                          */
/* ====================================================================== */

/* The fault_recover tool's answer. Static, like everything else here. Only the
 * first sentence is ever quoted back to the operator, and refusals put their
 * reason first, so this does not need to hold the whole usage text. */
static char fix_result[1024];

int faultchat_apply_fix(const faultchat_reply_t *reply, char *why, size_t whycap) {
    if (whycap) why_say(why, whycap, "");
    if (!reply) return FAULTCHAT_EINVAL;
    if (!reply->has_fix) {
        why_say(why, whycap, "the model proposed no fix");
        return FAULTCHAT_ENONE;
    }

    tool_result_t r;
    tool_result_init(&r, fix_result, sizeof fix_result);

    tool_call_t c;
    c.id        = "toolu_kernel_faultchat";
    c.name      = "fault_recover";
    c.input     = reply->fix;
    c.input_len = reply->fix_len;

    /* tool_dispatch emits the [fault_recover ...] trace line itself, in C, from
     * the real return value. That line is the operator's proof that a fix was
     * armed — this module never prints one of its own in bracket form. */
    int rc = tool_dispatch(&c, &r);
    if (rc == TOOL_ENOENT) {
        why_say(why, whycap,
                "the fault_recover tool is not registered in this build");
        return FAULTCHAT_EFIX;
    }
    if (rc != TOOL_OK || r.is_error) {
        /* Quote the tool's own words: they are the same sentence the model would
         * have been shown, so the console and the model agree about why. */
        char raw[FAULTCHAT_WHY_MAX], one[FAULTCHAT_WHY_MAX];
        size_t n = 0;
        while (n + 1 < sizeof raw && n < r.len &&
               fix_result[n] != '\n') { raw[n] = fix_result[n]; n++; }
        raw[n] = '\0';
        /* ...but sanitise it first, because it is not all kernel-authored.
         * recover_fail() in tools/fault_tools.c echoes the model's own words
         * back ("unknown field \"...\"", the action word, the register name),
         * decoded through json_str(), so up to FAULT_WORD_MAX-1 bytes of the
         * model's choosing survive - brackets and control characters included.
         * This string is printed inside a kernel-prefixed "[fault-diagnose] ..."
         * line, so an un-defanged '[' is a forged kernel statement mid-line and
         * a raw CR is a forged one at column zero on any terminal that honours
         * it. One line above, lib/trace.c escapes the identical bytes; the two
         * voices must not disagree about the rule in the same output. */
        sanitize(one, sizeof one, raw, n);
        why_say(why, whycap, one[0] ? one : "the fault_recover tool refused it");
        return FAULTCHAT_EFIX;
    }
    why_say(why, whycap, "armed");
    return FAULTCHAT_OK;
}

/* ====================================================================== */
/* error names                                                            */
/* ====================================================================== */

const char *faultchat_strerror(int rc) {
    switch (rc) {
        case FAULTCHAT_OK:         return "a diagnosis completed";
        case FAULTCHAT_ENONE:      return "there was nothing new to diagnose";
        case FAULTCHAT_EOFF:       return "fault diagnosis is switched off or "
                                          "out of budget for this boot";
        case FAULTCHAT_EBUSY:      return "a diagnosis request is already in "
                                          "flight";
        case FAULTCHAT_EINVAL:     return "bad arguments";
        case FAULTCHAT_ENOSPC:     return "the fault does not fit one request";
        case FAULTCHAT_ETRANSPORT: return "the model could not be reached";
        case FAULTCHAT_EPROTO:     return "the reply was not an intelligible "
                                          "diagnosis";
        case FAULTCHAT_EHTTP:      return "the API answered, but not with 200";
        case FAULTCHAT_EFIX:       return "the proposed fix was refused";
        case FAULTCHAT_ERECURSE:   return "a fault occurred while the diagnosis "
                                          "request was in flight";
        default:                   return "unknown result";
    }
}

/* ====================================================================== */
/* the live half                                                          */
/* ====================================================================== */

static model_transport_t *g_transport;
static int                g_enabled = 1;

/* The three independent guards described in the header. */
static int         g_busy;              /* a request is in flight right now   */
static int         g_off;               /* latched off for the rest of boot   */
static const char *g_off_why = "";

static uint32_t g_seen;                 /* fault_count() as of the last pump  */
static uint32_t g_diagnoses;            /* requests actually sent             */
static uint32_t g_fixes;                /* proposed fixes that were armed     */
static int      g_last_result = FAULTCHAT_ENONE;
static uint32_t g_last_seq;
static char     g_last_why[FAULTCHAT_WHY_MAX];

static char   g_prompt[FAULTCHAT_PROMPT_MAX];
static size_t g_prompt_len;
static char   g_req[FAULTCHAT_REQ_BYTES];
static char   g_resp[FAULTCHAT_RESP_BYTES];

static faultchat_reply_t g_reply;

void faultchat_bind(model_transport_t *t) {
    g_transport = t;
    faultchat_reset();
}

void faultchat_reset(void) {
    g_busy        = 0;
    g_off         = 0;
    g_off_why     = "";
    g_seen        = fault_count();
    g_diagnoses   = 0;
    g_fixes       = 0;
    g_last_result = FAULTCHAT_ENONE;
    g_last_seq    = 0;
    g_last_why[0] = '\0';
    g_prompt[0]   = '\0';
    g_prompt_len  = 0;
    g_reply.diagnosis[0] = '\0';
    g_reply.has_fix      = 0;
    g_reply.fix[0]       = '\0';
    g_reply.fix_len      = 0;
    g_enabled     = 1;
}

void faultchat_enable(int on) { g_enabled = on ? 1 : 0; }

static void latch_off(const char *why) {
    g_off     = 1;
    g_off_why = why;
}

int faultchat_pending(void) {
    if (!g_enabled || g_off || g_busy) return 0;
    if (g_diagnoses >= FAULTCHAT_MAX_DIAGNOSES) return 0;
    return fault_count() != g_seen;
}

static int finish(int rc, const char *why) {
    g_last_result = rc;
    why_say(g_last_why, sizeof g_last_why, why ? why : faultchat_strerror(rc));
    return rc;
}

int faultchat_pump(void) {
    /* Guard 1: re-entrancy. Nothing reachable from here calls back into the
     * pump today, and this is what guarantees that stays true. */
    if (g_busy) return FAULTCHAT_EBUSY;

    if (!g_enabled) return FAULTCHAT_EOFF;
    if (g_off)      return FAULTCHAT_EOFF;

    uint32_t now = fault_count();
    if (now == g_seen) return FAULTCHAT_ENONE;

    const fault_record_t *rec = fault_last();
    if (!rec) { g_seen = now; return FAULTCHAT_ENONE; }

    /* Claim every fault seen so far, before anything can fail. A diagnosis that
     * goes wrong must not be retried on the next pump: the machine would spend
     * its whole life re-asking about the same crash. */
    g_seen     = now;
    g_last_seq = rec->seq;

    /* NOTE ON FORMAT STRINGS, because it has cost more than one person a day:
     * kprintf() below is lib/base.c's formatter and has NO 'l' length modifier —
     * "%lu" prints the literal text and does not consume its argument, shifting
     * every later one. The ap() calls further up go through vsnprintf (the
     * lib/libc_shim.c one, freestanding) which DOES support it. Two formatters,
     * one name-shaped difference, and the failure is silent. Everything reaching
     * kprintf here is %s / %d / %u / %p only, and 64-bit values go through %p. */
    if (g_diagnoses >= FAULTCHAT_MAX_DIAGNOSES) {
        latch_off("the per-boot diagnosis budget is spent");
        kprintf("[fault-diagnose] fault #%u survived, but this boot has "
                "already spent its %u diagnoses; not asking again\n",
                (unsigned)rec->seq, (unsigned)FAULTCHAT_MAX_DIAGNOSES);
        return finish(FAULTCHAT_EOFF, "diagnosis budget spent");
    }

    /* Budget is spent HERE, on the attempt, not on the send. A machine with no
     * transport and a fault storm would otherwise announce every one of them
     * forever, and "how much of this boot is spent talking about crashes" is
     * the quantity the cap is actually about. */
    g_diagnoses++;

    kprintf("[fault-diagnose] fault #%u (%s, vector %u, at %p) was "
            "survived; asking the model what it was\n",
            (unsigned)rec->seq, fault_vector_name(rec->regs.vector),
            (unsigned)rec->regs.vector,
            (void *)(uintptr_t)rec->regs.rip);

    if (!g_transport) {
        kputs("[fault-diagnose] no transport is bound, so nobody can be asked. "
              "The report above is the whole record.\n");
        return finish(FAULTCHAT_ETRANSPORT, "no transport bound");
    }

    /* ---- build the prompt. Static buffers only: if the fault was inside the
     * allocator, this still works. ---- */
    g_prompt_len = faultchat_build_prompt(rec, fault_window(),
                                          g_prompt, sizeof g_prompt);
    if (g_prompt_len >= sizeof g_prompt) {
        /* The report is bounded by FAULT_REPORT_MAX and the rest is fixed text,
         * so this cannot happen with the current constants — which is exactly
         * why it must be checked rather than assumed. */
        kprintf("[fault-diagnose] the machine state does not fit %u bytes; "
                "not sending a clipped crash report\n",
                (unsigned)sizeof g_prompt);
        return finish(FAULTCHAT_ENOSPC, "prompt over budget");
    }

    model_msg_t msg;
    msg.role        = "user";
    msg.content     = g_prompt;      /* escaped by model_build: raw = 0 */
    msg.content_len = 0;
    msg.raw         = 0;

    model_request_t rq;
    rq.model         = (const char *)0;       /* model.c's compiled default */
    rq.max_tokens    = 1024;
    rq.system        = SYSTEM_PROMPT;
    rq.tools         = (const char *)0;       /* no tool loop: see the header */
    rq.tools_len     = 0;
    rq.messages      = &msg;
    rq.message_count = 1;

    int n = model_build(g_req, sizeof g_req, &rq);
    if (n < 0) {
        kprintf("[fault-diagnose] could not build the request (%s)\n",
                model_strerror(n));
        return finish(n == MODEL_ENOSPC ? FAULTCHAT_ENOSPC : FAULTCHAT_EINVAL,
                      "request could not be built");
    }

    kprintf("[fault-diagnose] sending %u bytes of machine state over \"%s\"\n",
            (unsigned)n, g_transport->name ? g_transport->name : "?");

    /* ---- the exchange. Guards 2 and 3 bracket exactly this. ---- */
    uint32_t before = fault_count();
    g_busy = 1;

    model_response_t resp;
    int rc = model_send(g_transport, g_req, (size_t)n,
                        g_resp, sizeof g_resp, &resp);

    g_busy = 0;

    /* Guard 2: did the machine fault while we were talking? If so the transport
     * was interrupted mid-state-machine and its answer means nothing, even if it
     * parses. Guard 3: never ask again this boot. */
    if (fault_count() != before) {
        latch_off("a fault occurred while a diagnosis request was in flight");
        kprintf("\n[fault-diagnose] A FAULT OCCURRED WHILE THE DIAGNOSIS "
                "REQUEST WAS IN FLIGHT (%u -> %u faults).\n"
                "[fault-diagnose] Asking about a crash is what crashed the "
                "machine, so the reply is discarded, no fix is armed, and "
                "diagnosis is switched off for the rest of this boot. The "
                "network stack is not reentrant and will not be entered again.\n",
                (unsigned)before, (unsigned)fault_count());
        return finish(FAULTCHAT_ERECURSE, "faulted during the request");
    }

    if (rc != MODEL_OK) {
        const char *w = model_last_error(g_transport);
        kprintf("[fault-diagnose] the model could not be reached: %s\n",
                (w && w[0]) ? w : model_strerror(rc));
        return finish(FAULTCHAT_ETRANSPORT, "the model could not be reached");
    }
    if (resp.http_status != 200) {
        kprintf("[fault-diagnose] the model answered %s; the crash report above "
                "is the whole record\n",
                resp.status_line[0] ? resp.status_line : "with an HTTP error");
        return finish(FAULTCHAT_EHTTP, "the API did not answer 200");
    }
    if (resp.truncated) {
        kprintf("[fault-diagnose] the reply exceeded %u bytes and was cut off\n",
                (unsigned)sizeof g_resp);
        return finish(FAULTCHAT_EPROTO, "the reply was truncated");
    }

    /* ---- interpret it ---- */
    char why[FAULTCHAT_WHY_MAX];
    int  prc = faultchat_parse_reply(resp.body, resp.body_len, &g_reply,
                                     why, sizeof why);
    if (prc != FAULTCHAT_OK) {
        kprintf("[fault-diagnose] the reply was not a diagnosis: %s\n", why);
        return finish(FAULTCHAT_EPROTO, why);
    }

    /* The model's voice, prose, no prefix — the same convention net/chat.c uses.
     * Already sanitised, so it cannot start a line that looks like the kernel's. */
    if (g_reply.diagnosis[0]) {
        kputs(g_reply.diagnosis);
        kputc('\n');
    }

    if (!g_reply.has_fix) {
        kputs("[fault-diagnose] the model proposed no fix, which is a complete "
              "answer. Nothing was armed.\n");
        return finish(FAULTCHAT_OK, "diagnosed, no fix proposed");
    }

    kputs("[fault-diagnose] the model proposed a fix; handing it to the "
          "fault_recover tool, which validates it exactly as if the model had "
          "called it directly\n");

    int arc = faultchat_apply_fix(&g_reply, why, sizeof why);
    if (arc != FAULTCHAT_OK) {
        kprintf("[fault-diagnose] the proposed fix was NOT armed: %s\n", why);
        return finish(FAULTCHAT_EFIX, why);
    }

    g_fixes++;
    kputs("[fault-diagnose] the fix is armed. The next matching exception will "
          "be handled by it instead of halting the machine.\n");
    return finish(FAULTCHAT_OK, "diagnosed and a fix was armed");
}

/* ---- introspection ---- */

uint32_t    faultchat_diagnoses(void)       { return g_diagnoses; }
uint32_t    faultchat_fixes_armed(void)     { return g_fixes; }
int         faultchat_last_result(void)     { return g_last_result; }
uint32_t    faultchat_last_seq(void)        { return g_last_seq; }
const char *faultchat_last_diagnosis(void)  { return g_reply.diagnosis; }
const char *faultchat_last_prompt(void)     { return g_prompt; }
size_t      faultchat_last_prompt_len(void) { return g_prompt_len; }
const char *faultchat_last_why(void)        { return g_last_why; }
int         faultchat_disabled(void)        { return g_off; }
const char *faultchat_disabled_reason(void) { return g_off_why; }
