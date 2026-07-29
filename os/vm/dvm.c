/* dvm.c — the driver VM. See include/dvm.h for the contract and the ISA.
 *
 * LAYOUT OF THIS FILE
 *   1. tiny local string helpers      (no kernel.h: see the note below)
 *   2. the opcode table               (mnemonics, aliases, operand shapes)
 *   3. the assembler                  (two passes over the source text)
 *   4. the validator + disassembler
 *   5. policy: allowlists, the compiled-in deny list, validation
 *   6. the interpreter                (bounds, trace, traps)
 *   7. the hardware I/O backend       (kernel builds only)
 *
 * WHY NO kernel.h
 *   This file is compiled twice: freestanding for the kernel, and natively for
 *   tests/host/test_dvm.c. kernel.h declares memcpy/memset/strlen, which collide
 *   with Apple's fortifying macros in a host TU. The VM needs so little string
 *   handling that carrying its own four-line helpers is cheaper than the
 *   conditional-compilation dance, and it keeps the module free of dependencies
 *   that would stop it running on the host.
 *
 * WHY NO ALLOCATION
 *   Nothing here calls kmalloc. The caller owns the dvm_program_t, the policy
 *   and the result; the interpreter's entire mutable state is one stack frame.
 *   A driver bring-up is exactly the moment when you do not want the allocator
 *   involved, and it makes the module callable from anywhere.
 */

#include "dvm.h"
#include "trace.h"

#include <stdarg.h>
#include <stdio.h>      /* snprintf/vsnprintf; port/stdio.h when freestanding */

/* Kernel errno used for the "-> EINVAL" tail of a trap's trace line. The trace
 * table (lib/trace.c) renders unknown codes as a bare integer, so reuse the
 * canonical one and put the precise, symbolic reason in the argument field. */
#define DVM_TRACE_ERR (-22)     /* VFS_EINVAL */

/* Ceilings on the caller's budgets. A policy above these is refused rather than
 * silently clamped, so "how long can this run" is never a surprise. */
#define CEIL_STEPS        5000000u
#define CEIL_IO           1000000u
#define CEIL_DELAY_US     2000000u    /* 2 s of cumulative delay */
#define CEIL_SINGLE_US    1000000u
#define CEIL_PRINTS       4096u
#define CEIL_TRACE        100000u

/* Only the low 4 GiB is mapped (boot.asm maps 2 MiB huge pages up to 4 GiB), so
 * an MMIO access above this would fault instead of being refused. */
#define MMIO_TOP          0x100000000ull

/* ====================================================================== */
/* 1. tiny local helpers                                                  */
/* ====================================================================== */

static void dz(void *p, size_t n) {                 /* zero fill */
    unsigned char *b = (unsigned char *)p;
    while (n--) *b++ = 0;
}

static char dlower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }

/* Case-insensitive compare of a counted token against a NUL-terminated word. */
static int tok_ieq(const char *t, int tlen, const char *word) {
    int i = 0;
    for (; i < tlen; i++) {
        if (!word[i]) return 0;
        if (dlower(t[i]) != dlower(word[i])) return 0;
    }
    return word[i] == '\0';
}

/* Bounded string builder used by the disassembler and every message. */
typedef struct { char *p; size_t cap; size_t len; } sb_t;

static void sb_init(sb_t *s, char *buf, size_t cap) {
    s->p = buf; s->cap = cap; s->len = 0;
    if (cap) buf[0] = '\0';
}

static void sb_addf(sb_t *s, const char *fmt, ...) {
    if (!s->cap || s->len + 1 >= s->cap) return;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(s->p + s->len, s->cap - s->len, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    s->len += (size_t)n;
    if (s->len >= s->cap) s->len = s->cap - 1;
}

/* Copy model-authored text into a kernel-owned buffer, replacing anything that
 * is not printable ASCII. Trace lines get escaped by trace.c anyway; this is for
 * dvm_result_t.msg, which callers hand to the model and may print elsewhere. */
static void copy_printable(char *dst, size_t cap, const char *src) {
    size_t o = 0;
    if (!cap) return;
    for (size_t i = 0; src && src[i] && o + 1 < cap; i++) {
        unsigned char c = (unsigned char)src[i];
        dst[o++] = (c >= 0x20 && c < 0x7F) ? (char)c : '?';
    }
    dst[o] = '\0';
}

/* ====================================================================== */
/* 2. the opcode table                                                    */
/* ====================================================================== */

typedef enum {
    F_NONE,          /* halt, ret, nop                          */
    F_STR,           /* abort "text"                            */
    F_STR_OPT_A,     /* print "text" [, a]                      */
    F_RD_A,          /* mov rd, a ; not rd, a                   */
    F_RD_A_B,        /* add rd, a, b  (2-operand form allowed)  */
    F_A_B,           /* cmp a, b                                */
    F_A,             /* push a ; delay a                        */
    F_RD,            /* pop rd                                  */
    F_LBL,           /* jmp/branch/call L                       */
    F_PORT_A,        /* out8 port, a                            */
    F_RD_PORT,       /* in8 rd, port                            */
    F_RD_ADDR,       /* ld8 rd, [base+disp]                     */
    F_ADDR_A,        /* st8 [base+disp], a                      */
    F_RD_BDF_OFF     /* pcicfg rd, bdf, off                     */
} fmt_t;

static const struct { const char *name; uint8_t fmt; } op_info[DVM_OP__COUNT] = {
    [DVM_HALT]   = { "halt",   F_NONE       },
    [DVM_ABORT]  = { "abort",  F_STR        },
    [DVM_NOP]    = { "nop",    F_NONE       },
    [DVM_MOV]    = { "mov",    F_RD_A       },
    [DVM_ADD]    = { "add",    F_RD_A_B     },
    [DVM_SUB]    = { "sub",    F_RD_A_B     },
    [DVM_MUL]    = { "mul",    F_RD_A_B     },
    [DVM_DIV]    = { "div",    F_RD_A_B     },
    [DVM_MOD]    = { "mod",    F_RD_A_B     },
    [DVM_AND]    = { "and",    F_RD_A_B     },
    [DVM_OR]     = { "or",     F_RD_A_B     },
    [DVM_XOR]    = { "xor",    F_RD_A_B     },
    [DVM_NOT]    = { "not",    F_RD_A       },
    [DVM_SHL]    = { "shl",    F_RD_A_B     },
    [DVM_SHR]    = { "shr",    F_RD_A_B     },
    [DVM_CMP]    = { "cmp",    F_A_B        },
    [DVM_TEST]   = { "test",   F_A_B        },
    [DVM_JMP]    = { "jmp",    F_LBL        },
    [DVM_BEQ]    = { "beq",    F_LBL        },
    [DVM_BNE]    = { "bne",    F_LBL        },
    [DVM_BLT]    = { "blt",    F_LBL        },
    [DVM_BLE]    = { "ble",    F_LBL        },
    [DVM_BGT]    = { "bgt",    F_LBL        },
    [DVM_BGE]    = { "bge",    F_LBL        },
    [DVM_CALL]   = { "call",   F_LBL        },
    [DVM_RET]    = { "ret",    F_NONE       },
    [DVM_PUSH]   = { "push",   F_A          },
    [DVM_POP]    = { "pop",    F_RD         },
    [DVM_OUT8]   = { "out8",   F_PORT_A     },
    [DVM_OUT16]  = { "out16",  F_PORT_A     },
    [DVM_OUT32]  = { "out32",  F_PORT_A     },
    [DVM_IN8]    = { "in8",    F_RD_PORT    },
    [DVM_IN16]   = { "in16",   F_RD_PORT    },
    [DVM_IN32]   = { "in32",   F_RD_PORT    },
    [DVM_LD8]    = { "ld8",    F_RD_ADDR    },
    [DVM_LD16]   = { "ld16",   F_RD_ADDR    },
    [DVM_LD32]   = { "ld32",   F_RD_ADDR    },
    [DVM_ST8]    = { "st8",    F_ADDR_A     },
    [DVM_ST16]   = { "st16",   F_ADDR_A     },
    [DVM_ST32]   = { "st32",   F_ADDR_A     },
    [DVM_PCICFG] = { "pcicfg", F_RD_BDF_OFF },
    [DVM_DELAY]  = { "delay",  F_A          },
    [DVM_PRINT]  = { "print",  F_STR_OPT_A  },
};

const char *dvm_op_name(dvm_op_t op) {
    if ((unsigned)op >= DVM_OP__COUNT || !op_info[op].name) return "?";
    return op_info[op].name;
}

/* Mnemonics the assembler accepts, canonical name first. The aliases exist
 * because the model writes this text from memory of other assemblers, and a
 * rejected program costs a whole turn. */
static const struct { const char *m; uint8_t op; } mnemonic[] = {
    { "halt", DVM_HALT }, { "hlt", DVM_HALT }, { "stop", DVM_HALT },
    { "end",  DVM_HALT },
    { "abort", DVM_ABORT }, { "fail", DVM_ABORT },
    { "nop", DVM_NOP },
    { "mov", DVM_MOV }, { "li", DVM_MOV }, { "set", DVM_MOV },
    { "add", DVM_ADD }, { "sub", DVM_SUB }, { "mul", DVM_MUL },
    { "div", DVM_DIV }, { "mod", DVM_MOD }, { "rem", DVM_MOD },
    { "and", DVM_AND }, { "or", DVM_OR }, { "xor", DVM_XOR },
    { "not", DVM_NOT },
    { "shl", DVM_SHL }, { "lsl", DVM_SHL },
    { "shr", DVM_SHR }, { "lsr", DVM_SHR },
    { "cmp", DVM_CMP }, { "test", DVM_TEST }, { "tst", DVM_TEST },
    { "jmp", DVM_JMP }, { "j", DVM_JMP }, { "b", DVM_JMP },
    { "beq", DVM_BEQ }, { "je", DVM_BEQ }, { "jz", DVM_BEQ }, { "bz", DVM_BEQ },
    { "bne", DVM_BNE }, { "jne", DVM_BNE }, { "jnz", DVM_BNE }, { "bnz", DVM_BNE },
    { "blt", DVM_BLT }, { "jb", DVM_BLT }, { "jl", DVM_BLT },
    { "ble", DVM_BLE }, { "jbe", DVM_BLE }, { "jle", DVM_BLE },
    { "bgt", DVM_BGT }, { "ja", DVM_BGT }, { "jg", DVM_BGT },
    { "bge", DVM_BGE }, { "jae", DVM_BGE }, { "jge", DVM_BGE },
    { "call", DVM_CALL }, { "ret", DVM_RET },
    { "push", DVM_PUSH }, { "pop", DVM_POP },
    { "out8", DVM_OUT8 }, { "outb", DVM_OUT8 },
    { "out16", DVM_OUT16 }, { "outw", DVM_OUT16 },
    { "out32", DVM_OUT32 }, { "outl", DVM_OUT32 }, { "outd", DVM_OUT32 },
    { "in8", DVM_IN8 }, { "inb", DVM_IN8 },
    { "in16", DVM_IN16 }, { "inw", DVM_IN16 },
    { "in32", DVM_IN32 }, { "inl", DVM_IN32 }, { "ind", DVM_IN32 },
    { "ld8", DVM_LD8 }, { "mr8", DVM_LD8 },
    { "ld16", DVM_LD16 }, { "mr16", DVM_LD16 },
    { "ld32", DVM_LD32 }, { "mr32", DVM_LD32 },
    { "st8", DVM_ST8 }, { "mw8", DVM_ST8 },
    { "st16", DVM_ST16 }, { "mw16", DVM_ST16 },
    { "st32", DVM_ST32 }, { "mw32", DVM_ST32 },
    { "pcicfg", DVM_PCICFG }, { "pciread", DVM_PCICFG }, { "pcird", DVM_PCICFG },
    { "delay", DVM_DELAY }, { "udelay", DVM_DELAY }, { "usleep", DVM_DELAY },
    { "print", DVM_PRINT }, { "log", DVM_PRINT },
};
#define NMNEMONIC ((int)(sizeof mnemonic / sizeof mnemonic[0]))

const char *dvm_status_name(dvm_status_t s) {
    switch (s) {
        case DVM_OK:                return "OK";
        case DVM_TRAP_ABORT:        return "ABORT";
        case DVM_TRAP_STEPS:        return "STEP_LIMIT";
        case DVM_TRAP_IO_BUDGET:    return "IO_LIMIT";
        case DVM_TRAP_DELAY_BUDGET: return "DELAY_LIMIT";
        case DVM_TRAP_PRINT_BUDGET: return "PRINT_LIMIT";
        case DVM_TRAP_PORT_DENIED:  return "PORT_DENIED";
        case DVM_TRAP_MMIO_DENIED:  return "MMIO_DENIED";
        case DVM_TRAP_PCI_DENIED:   return "PCI_DENIED";
        case DVM_TRAP_ALIGN:        return "MISALIGNED";
        case DVM_TRAP_RANGE:        return "OPERAND_RANGE";
        case DVM_TRAP_PC:           return "BAD_PC";
        case DVM_TRAP_STACK:        return "STACK";
        case DVM_TRAP_DIV0:         return "DIVIDE_BY_ZERO";
        case DVM_TRAP_BADOP:        return "BAD_PROGRAM";
        case DVM_TRAP_NOIO:         return "NO_BACKEND";
        case DVM_TRAP_POLICY:       return "BAD_POLICY";
        default:                    return "?";
    }
}

/* ====================================================================== */
/* 3. the assembler                                                       */
/* ====================================================================== */

typedef struct { const char *s; int len; } tok_t;
#define MAXTOK 8

typedef struct {
    dvm_program_t *p;
    dvm_asm_err_t *err;
    int            failed;
    int            line;                       /* 1-based, current            */
    struct { char name[DVM_LABEL_MAX]; uint64_t val; } equ[DVM_MAX_EQU];
    int            nequ;
} as_t;

/* Record the first error and stop; later errors are almost always fallout. */
static int aerr(as_t *as, const char *fmt, ...) {
    if (as->failed) return -1;
    as->failed = 1;
    if (as->err) {
        as->err->line = as->line;
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(as->err->msg, sizeof as->err->msg, fmt, ap);
        va_end(ap);
    }
    return -1;
}

/* ---- name classification ---- */

static int is_ident_start(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == '.';
}
static int is_ident_char(char c) {
    return is_ident_start(c) || (c >= '0' && c <= '9');
}

/* "r7" / "R7" -> 7, "r16" -> -2 (a register that does not exist), else -1. */
static int reg_index(const char *t, int len) {
    if (len < 2 || (t[0] != 'r' && t[0] != 'R')) return -1;
    int v = 0;
    for (int i = 1; i < len; i++) {
        if (t[i] < '0' || t[i] > '9') return -1;
        v = v * 10 + (t[i] - '0');
        if (v > 999) return -2;
    }
    return v < DVM_NREGS ? v : -2;
}

/* Parse a numeric literal: 0x.. 0b.. decimal, optional leading '-' (two's
 * complement). Returns 0 on success. Overflow is an error, not a wrap. */
static int parse_num(const char *t, int len, uint64_t *out) {
    int i = 0, neg = 0;
    if (len <= 0) return -1;
    if (t[i] == '-' || t[i] == '+') { neg = (t[i] == '-'); i++; }
    if (i >= len) return -1;

    unsigned base = 10;
    if (len - i > 2 && t[i] == '0' && (t[i + 1] == 'x' || t[i + 1] == 'X')) { base = 16; i += 2; }
    else if (len - i > 2 && t[i] == '0' && (t[i + 1] == 'b' || t[i + 1] == 'B')) { base = 2; i += 2; }

    uint64_t v = 0;
    int digits = 0;
    for (; i < len; i++) {
        char c = dlower(t[i]);
        unsigned d;
        if (c == '_') continue;                       /* 0xdead_beef */
        if (c >= '0' && c <= '9') d = (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f') d = (unsigned)(c - 'a' + 10);
        else return -1;
        if (d >= base) return -1;
        if (v > (0xFFFFFFFFFFFFFFFFull - d) / base) return -2;   /* overflow */
        v = v * base + d;
        digits++;
    }
    if (!digits) return -1;
    *out = neg ? (uint64_t)(-(int64_t)v) : v;
    return 0;
}

/* "00:03.0" -> bdf ((bus<<8)|(dev<<3)|fn), the way pci_list prints an address. */
static int parse_bdf(const char *t, int len, uint64_t *out) {
    int i = 0, v, n;
    unsigned bus, dev, fn;

    for (n = 0, v = 0; i < len && n < 2; n++) {
        char c = dlower(t[i]);
        int d = (c >= '0' && c <= '9') ? c - '0' : (c >= 'a' && c <= 'f') ? c - 'a' + 10 : -1;
        if (d < 0) break;
        v = v * 16 + d; i++;
    }
    if (!n || i >= len || t[i] != ':') return -1;
    bus = (unsigned)v; i++;

    for (n = 0, v = 0; i < len && n < 2; n++) {
        char c = dlower(t[i]);
        int d = (c >= '0' && c <= '9') ? c - '0' : (c >= 'a' && c <= 'f') ? c - 'a' + 10 : -1;
        if (d < 0) break;
        v = v * 16 + d; i++;
    }
    if (!n || i >= len || t[i] != '.' || v > 31) return -1;
    dev = (unsigned)v; i++;

    if (i >= len) return -1;
    if (t[i] < '0' || t[i] > '7') return -1;
    fn = (unsigned)(t[i] - '0'); i++;
    if (i != len) return -1;

    *out = ((uint64_t)bus << 8) | ((uint64_t)dev << 3) | fn;
    return 0;
}

static int equ_lookup(const as_t *as, const char *t, int len, uint64_t *out) {
    for (int i = 0; i < as->nequ; i++)
        if (tok_ieq(t, len, as->equ[i].name)) { *out = as->equ[i].val; return 1; }
    return 0;
}

static int label_lookup(const dvm_program_t *p, const char *t, int len, uint32_t *pc) {
    for (uint32_t i = 0; i < p->nlabel; i++)
        if (tok_ieq(t, len, p->label[i].name)) { *pc = p->label[i].pc; return 1; }
    return 0;
}

/* ---- tokenising one line ---- */

/* Strip a trailing comment (';', '#', or '//') that is not inside a string.
 * The escape state is tracked properly rather than by peeking one character
 * back, so a string ending in a literal backslash ("a\\") does not swallow the
 * rest of the line. */
static void strip_comment(char *s) {
    int q = 0, esc = 0;
    for (int i = 0; s[i]; i++) {
        if (q) {
            if (esc)                esc = 0;
            else if (s[i] == '\\')  esc = 1;
            else if (s[i] == '"')   q = 0;
            continue;
        }
        if (s[i] == '"') { q = 1; continue; }
        if (s[i] == ';' || s[i] == '#' || (s[i] == '/' && s[i + 1] == '/')) {
            s[i] = '\0';
            return;
        }
    }
}

static int is_space(char c) { return c == ' ' || c == '\t' || c == '\r' || c == ','; }

/* Split into tokens. A quoted string and a [bracketed] address each stay whole.
 * Returns the token count, or -1 on an unterminated quote/bracket. */
static int tokenise(as_t *as, char *s, tok_t *out, int max) {
    int n = 0;
    for (int i = 0; s[i];) {
        if (is_space(s[i])) { i++; continue; }
        if (n >= max) return aerr(as, "too many operands (at most %d per instruction)", max - 1);

        if (s[i] == '"') {
            int j = i + 1, esc = 0;
            while (s[j]) {
                if (esc)               esc = 0;
                else if (s[j] == '\\') esc = 1;
                else if (s[j] == '"')  break;
                j++;
            }
            if (s[j] != '"') return aerr(as, "unterminated string literal");
            out[n].s = &s[i]; out[n].len = j - i + 1; n++;
            i = j + 1;
        } else if (s[i] == '[') {
            int j = i + 1;
            while (s[j] && s[j] != ']') j++;
            if (s[j] != ']') return aerr(as, "unterminated '[' - write an address as [r1+0x10]");
            out[n].s = &s[i]; out[n].len = j - i + 1; n++;
            i = j + 1;
        } else {
            int j = i;
            while (s[j] && !is_space(s[j]) && s[j] != '"' && s[j] != '[') j++;
            out[n].s = &s[i]; out[n].len = j - i; n++;
            i = j;
        }
    }
    return n;
}

/* ---- operand parsing ---- */

/* A register, a number, a .equ constant, or a bdf literal. Never a label. */
static int operand(as_t *as, const char *what, tok_t t, uint8_t *kind, uint64_t *val) {
    int r = reg_index(t.s, t.len);
    if (r >= 0) { *kind = DVM_O_REG; *val = (uint64_t)r; return 0; }
    if (r == -2)
        return aerr(as, "%s: register \"%.*s\" does not exist (this machine has r0-r%d)",
                    what, t.len, t.s, DVM_NREGS - 1);

    uint64_t v;
    int rc = parse_num(t.s, t.len, &v);
    if (rc == 0)  { *kind = DVM_O_IMM; *val = v; return 0; }
    if (rc == -2) return aerr(as, "%s: number \"%.*s\" does not fit in 64 bits", what, t.len, t.s);

    if (equ_lookup(as, t.s, t.len, &v)) { *kind = DVM_O_IMM; *val = v; return 0; }

    return aerr(as, "%s: expected a register (r0-r%d), a number, or a .equ name, got \"%.*s\"",
                what, DVM_NREGS - 1, t.len, t.s);
}

/* An address operand: "[base+disp]", "[base]", or a bare base. Fills two
 * operand slots. `have_disp` says whether a following bare token supplied the
 * displacement (only consulted for the unbracketed form). */
static int addr_operand(as_t *as, tok_t t, uint8_t *kind, uint64_t *val) {
    kind[0] = DVM_O_IMM; val[0] = 0;
    kind[1] = DVM_O_IMM; val[1] = 0;

    if (t.len >= 2 && t.s[0] == '[') {
        tok_t in = { t.s + 1, t.len - 2 };
        /* trim */
        while (in.len && is_space(in.s[0])) { in.s++; in.len--; }
        while (in.len && is_space(in.s[in.len - 1])) in.len--;
        if (!in.len) return aerr(as, "empty address []");

        int plus = -1;
        for (int i = 0; i < in.len; i++) if (in.s[i] == '+') { plus = i; break; }
        if (plus < 0) return operand(as, "address", in, &kind[0], &val[0]);

        tok_t b = { in.s, plus }, d = { in.s + plus + 1, in.len - plus - 1 };
        while (b.len && is_space(b.s[b.len - 1])) b.len--;
        while (d.len && is_space(d.s[0])) { d.s++; d.len--; }
        if (!b.len || !d.len)
            return aerr(as, "malformed address \"%.*s\"; expected [base+displacement]",
                        t.len, t.s);
        if (operand(as, "address base", b, &kind[0], &val[0]) != 0) return -1;
        return operand(as, "address displacement", d, &kind[1], &val[1]);
    }
    return operand(as, "address", t, &kind[0], &val[0]);
}

/* ---- string literals ---- */

/* Decode "..." (with \\ \" \n \t \r \0 escapes) into the program's pool.
 * Returns the string index, or -1. */
static int intern_string(as_t *as, tok_t t) {
    dvm_program_t *p = as->p;
    if (t.len < 2 || t.s[0] != '"' || t.s[t.len - 1] != '"')
        return aerr(as, "expected a quoted string, got \"%.*s\"", t.len, t.s);
    if (p->nstr >= DVM_MAX_STRINGS)
        return aerr(as, "too many strings (max %d)", DVM_MAX_STRINGS);

    uint32_t start = p->strused;
    for (int i = 1; i < t.len - 1; i++) {
        char c = t.s[i];
        if (c == '\\' && i + 1 < t.len - 1) {
            i++;
            switch (t.s[i]) {
                case 'n': c = '\n'; break;
                case 't': c = '\t'; break;
                case 'r': c = '\r'; break;
                case '0': c = '\0'; break;
                case '\\': c = '\\'; break;
                case '"': c = '"'; break;
                default:
                    return aerr(as, "unknown escape \"\\%c\" in a string literal", t.s[i]);
            }
        }
        if (p->strused + 1 >= DVM_STRPOOL)
            return aerr(as, "string pool full (%d bytes total across the program)",
                        DVM_STRPOOL);
        p->strpool[p->strused++] = c;
    }
    if (p->strused + 1 > DVM_STRPOOL)
        return aerr(as, "string pool full (%d bytes total across the program)", DVM_STRPOOL);
    p->strpool[p->strused++] = '\0';
    p->stroff[p->nstr] = (uint16_t)start;
    return (int)p->nstr++;
}

static const char *str_at(const dvm_program_t *p, uint64_t idx) {
    if (idx >= p->nstr) return "";
    uint16_t off = p->stroff[idx];
    if (off >= DVM_STRPOOL) return "";
    return &p->strpool[off];
}

/* ---- one instruction ---- */

/* Read a destination register operand, distinguishing "that is not a register"
 * from "that register does not exist on this machine". */
static int dst_reg(as_t *as, const char *mn, tok_t t, uint8_t *dst) {
    int r = reg_index(t.s, t.len);
    if (r >= 0) { *dst = (uint8_t)r; return 0; }
    if (r == -2)
        return aerr(as, "%s: register \"%.*s\" does not exist (this machine has r0-r%d)",
                    mn, t.len, t.s, DVM_NREGS - 1);
    return aerr(as, "%s: destination must be a register (r0-r%d), got \"%.*s\"",
                mn, DVM_NREGS - 1, t.len, t.s);
}

static int need_operands(as_t *as, const char *mn, int got, int want) {
    if (got == want) return 0;
    return aerr(as, "%s takes %d operand%s, got %d", mn, want, want == 1 ? "" : "s", got);
}

static int assemble_insn(as_t *as, uint8_t op, tok_t *t, int nt, const char *mn) {
    dvm_program_t *p = as->p;
    if (p->ninsn >= DVM_MAX_INSNS)
        return aerr(as, "program too long (max %d instructions)", DVM_MAX_INSNS);

    dvm_insn_t *ins = &p->insn[p->ninsn];
    dz(ins, sizeof *ins);
    ins->op   = op;
    ins->dst  = DVM_NOREG;
    ins->line = (uint32_t)as->line;

    int rd;
    switch (op_info[op].fmt) {
    case F_NONE:
        if (need_operands(as, mn, nt, 0) != 0) return -1;
        break;

    case F_STR:
        if (need_operands(as, mn, nt, 1) != 0) return -1;
        rd = intern_string(as, t[0]);
        if (rd < 0) return -1;
        ins->kind[0] = DVM_O_STR; ins->val[0] = (uint64_t)rd;
        break;

    case F_STR_OPT_A:
        if (nt < 1 || nt > 2)
            return aerr(as, "%s takes a string and an optional value, got %d operand%s",
                        mn, nt, nt == 1 ? "" : "s");
        rd = intern_string(as, t[0]);
        if (rd < 0) return -1;
        ins->kind[0] = DVM_O_STR; ins->val[0] = (uint64_t)rd;
        if (nt == 2 && operand(as, mn, t[1], &ins->kind[1], &ins->val[1]) != 0) return -1;
        break;

    case F_RD_A:
        if (need_operands(as, mn, nt, 2) != 0) return -1;
        if (dst_reg(as, mn, t[0], &ins->dst) != 0) return -1;
        if (operand(as, mn, t[1], &ins->kind[0], &ins->val[0]) != 0) return -1;
        break;

    case F_RD_A_B:
        if (nt < 2 || nt > 3)
            return aerr(as, "%s takes 2 or 3 operands (\"%s rd, a, b\" or the shorthand "
                            "\"%s rd, b\"), got %d", mn, mn, mn, nt);
        if (dst_reg(as, mn, t[0], &ins->dst) != 0) return -1;
        if (nt == 2) {          /* "add r1, 4" == "add r1, r1, 4" */
            ins->kind[0] = DVM_O_REG; ins->val[0] = ins->dst;
            if (operand(as, mn, t[1], &ins->kind[1], &ins->val[1]) != 0) return -1;
        } else {
            if (operand(as, mn, t[1], &ins->kind[0], &ins->val[0]) != 0) return -1;
            if (operand(as, mn, t[2], &ins->kind[1], &ins->val[1]) != 0) return -1;
        }
        break;

    case F_A_B:
        if (need_operands(as, mn, nt, 2) != 0) return -1;
        if (operand(as, mn, t[0], &ins->kind[0], &ins->val[0]) != 0) return -1;
        if (operand(as, mn, t[1], &ins->kind[1], &ins->val[1]) != 0) return -1;
        break;

    case F_A:
        if (need_operands(as, mn, nt, 1) != 0) return -1;
        if (operand(as, mn, t[0], &ins->kind[0], &ins->val[0]) != 0) return -1;
        break;

    case F_RD:
        if (need_operands(as, mn, nt, 1) != 0) return -1;
        if (dst_reg(as, mn, t[0], &ins->dst) != 0) return -1;
        break;

    case F_LBL: {
        if (need_operands(as, mn, nt, 1) != 0) return -1;
        uint32_t target;
        if (!label_lookup(p, t[0].s, t[0].len, &target))
            return aerr(as, "%s: no label named \"%.*s\" in this program", mn, t[0].len, t[0].s);
        ins->kind[0] = DVM_O_PC; ins->val[0] = target;
        break;
    }

    case F_PORT_A: {
        if (need_operands(as, mn, nt, 2) != 0) return -1;
        tok_t pt = t[0];
        if (pt.len >= 2 && pt.s[0] == '[') { pt.s++; pt.len -= 2; }   /* out8 [0x61], 1 */
        if (operand(as, "port", pt, &ins->kind[0], &ins->val[0]) != 0) return -1;
        if (operand(as, mn, t[1], &ins->kind[1], &ins->val[1]) != 0) return -1;
        break;
    }

    case F_RD_PORT: {
        if (need_operands(as, mn, nt, 2) != 0) return -1;
        if (dst_reg(as, mn, t[0], &ins->dst) != 0) return -1;
        tok_t pt = t[1];
        if (pt.len >= 2 && pt.s[0] == '[') { pt.s++; pt.len -= 2; }
        if (operand(as, "port", pt, &ins->kind[0], &ins->val[0]) != 0) return -1;
        break;
    }

    case F_RD_ADDR:
        if (nt < 2 || nt > 3)
            return aerr(as, "%s takes a destination register and an address "
                            "(\"%s rd, [base+disp]\" or \"%s rd, base, disp\"), got %d operands",
                        mn, mn, mn, nt);
        if (dst_reg(as, mn, t[0], &ins->dst) != 0) return -1;
        if (nt == 2) {
            if (addr_operand(as, t[1], ins->kind, ins->val) != 0) return -1;
        } else {
            if (t[1].len >= 2 && t[1].s[0] == '[')
                return aerr(as, "%s: give either [base+disp] or two plain operands, not both", mn);
            if (operand(as, "address base", t[1], &ins->kind[0], &ins->val[0]) != 0) return -1;
            if (operand(as, "address displacement", t[2], &ins->kind[1], &ins->val[1]) != 0) return -1;
        }
        break;

    case F_ADDR_A:
        if (nt < 2 || nt > 3)
            return aerr(as, "%s takes an address and a value (\"%s [base+disp], value\" or "
                            "\"%s base, disp, value\"), got %d operands", mn, mn, mn, nt);
        if (nt == 2) {
            if (addr_operand(as, t[0], ins->kind, ins->val) != 0) return -1;
            if (operand(as, "value", t[1], &ins->kind[2], &ins->val[2]) != 0) return -1;
        } else {
            if (t[0].len >= 2 && t[0].s[0] == '[')
                return aerr(as, "%s: give either [base+disp] or two plain operands, not both", mn);
            if (operand(as, "address base", t[0], &ins->kind[0], &ins->val[0]) != 0) return -1;
            if (operand(as, "address displacement", t[1], &ins->kind[1], &ins->val[1]) != 0) return -1;
            if (operand(as, "value", t[2], &ins->kind[2], &ins->val[2]) != 0) return -1;
        }
        break;

    case F_RD_BDF_OFF: {
        if (need_operands(as, mn, nt, 3) != 0) return -1;
        if (dst_reg(as, mn, t[0], &ins->dst) != 0) return -1;
        uint64_t bdf;
        if (parse_bdf(t[1].s, t[1].len, &bdf) == 0) {
            ins->kind[0] = DVM_O_IMM; ins->val[0] = bdf;
        } else if (operand(as, "bdf", t[1], &ins->kind[0], &ins->val[0]) != 0) {
            return -1;
        }
        if (operand(as, "config offset", t[2], &ins->kind[1], &ins->val[1]) != 0) return -1;
        break;
    }
    }

    p->ninsn++;
    return 0;
}

/* ---- .equ ---- */

static int do_equ(as_t *as, tok_t *t, int nt) {
    if (nt != 2)
        return aerr(as, ".equ takes a name and a value, e.g. \".equ NAM_BAR 0x10\"");
    if (as->nequ >= DVM_MAX_EQU)
        return aerr(as, "too many .equ constants (max %d)", DVM_MAX_EQU);
    if (t[0].len <= 0 || t[0].len >= DVM_LABEL_MAX)
        return aerr(as, "constant name \"%.*s\" is too long (max %d characters)",
                    t[0].len, t[0].s, DVM_LABEL_MAX - 1);
    if (!is_ident_start(t[0].s[0]))
        return aerr(as, "constant name \"%.*s\" must start with a letter or '_'",
                    t[0].len, t[0].s);
    for (int i = 0; i < t[0].len; i++)
        if (!is_ident_char(t[0].s[i]))
            return aerr(as, "constant name \"%.*s\" contains an illegal character",
                        t[0].len, t[0].s);
    if (reg_index(t[0].s, t[0].len) != -1)
        return aerr(as, "\"%.*s\" is a register name and cannot be a constant",
                    t[0].len, t[0].s);
    uint64_t dummy;
    uint32_t dummypc;
    if (equ_lookup(as, t[0].s, t[0].len, &dummy))
        return aerr(as, "constant \"%.*s\" is already defined", t[0].len, t[0].s);
    if (label_lookup(as->p, t[0].s, t[0].len, &dummypc))
        return aerr(as, "\"%.*s\" is already a label", t[0].len, t[0].s);

    uint64_t v;
    int rc = parse_num(t[1].s, t[1].len, &v);
    if (rc == -2) return aerr(as, ".equ %.*s: number \"%.*s\" does not fit in 64 bits",
                              t[0].len, t[0].s, t[1].len, t[1].s);
    if (rc != 0 && !equ_lookup(as, t[1].s, t[1].len, &v))
        return aerr(as, ".equ %.*s: expected a number, got \"%.*s\"",
                    t[0].len, t[0].s, t[1].len, t[1].s);

    for (int i = 0; i < t[0].len; i++) as->equ[as->nequ].name[i] = t[0].s[i];
    as->equ[as->nequ].name[t[0].len] = '\0';
    as->equ[as->nequ].val = v;
    as->nequ++;
    return 0;
}

static int add_label(as_t *as, const char *name, int len) {
    dvm_program_t *p = as->p;
    if (len <= 0 || len >= DVM_LABEL_MAX)
        return aerr(as, "label \"%.*s\" is too long (max %d characters)",
                    len, name, DVM_LABEL_MAX - 1);
    if (!is_ident_start(name[0]))
        return aerr(as, "label \"%.*s\" must start with a letter, '_' or '.'", len, name);
    for (int i = 0; i < len; i++)
        if (!is_ident_char(name[i]))
            return aerr(as, "label \"%.*s\" contains an illegal character", len, name);
    if (reg_index(name, len) != -1)
        return aerr(as, "\"%.*s\" is a register name and cannot be a label", len, name);
    for (int i = 0; i < NMNEMONIC; i++)
        if (tok_ieq(name, len, mnemonic[i].m))
            return aerr(as, "\"%.*s\" is an instruction name and cannot be a label", len, name);
    uint32_t where;
    uint64_t dummy;
    if (label_lookup(p, name, len, &where))
        return aerr(as, "label \"%.*s\" is already defined", len, name);
    if (equ_lookup(as, name, len, &dummy))
        return aerr(as, "\"%.*s\" is already a .equ constant", len, name);
    if (p->nlabel >= DVM_MAX_LABELS)
        return aerr(as, "too many labels (max %d)", DVM_MAX_LABELS);

    for (int i = 0; i < len; i++) p->label[p->nlabel].name[i] = name[i];
    p->label[p->nlabel].name[len] = '\0';
    p->label[p->nlabel].pc = p->ninsn;
    p->nlabel++;
    return 0;
}

/* One pass over the source. pass 1 collects labels and constants (and counts
 * instructions so a forward branch resolves); pass 2 emits. */
static int assemble_pass(as_t *as, const char *src, size_t len, int pass) {
    size_t i = 0;
    as->p->ninsn = 0;
    as->line = 0;

    while (i <= len) {
        /* Extract one line. A final line without a newline still counts. */
        size_t start = i;
        while (i < len && src[i] != '\n') i++;
        size_t n = i - start;
        if (i < len) i++;                 /* consume the '\n' */
        else if (n == 0 && start >= len) break;
        else i = len + 1;                 /* last line, then stop */

        as->line++;
        if (n >= DVM_LINE_MAX)
            return aerr(as, "line is %lu characters; the limit is %d",
                        (unsigned long)n, DVM_LINE_MAX - 1);

        char buf[DVM_LINE_MAX];
        for (size_t k = 0; k < n; k++) buf[k] = src[start + k];
        buf[n] = '\0';

        /* An embedded NUL truncates the line rather than being an error: the
         * source arrives as a JSON string span and a stray NUL is a model
         * artefact, not an attack. Everything after it on that line is ignored. */
        strip_comment(buf);

        tok_t t[MAXTOK];
        int nt = tokenise(as, buf, t, MAXTOK);
        if (nt < 0) return -1;
        if (nt == 0) continue;

        /* label: */
        if (t[0].len > 1 && t[0].s[t[0].len - 1] == ':') {
            if (pass == 1 && add_label(as, t[0].s, t[0].len - 1) != 0) return -1;
            for (int k = 1; k < nt; k++) t[k - 1] = t[k];
            nt--;
            if (nt == 0) continue;
        }

        /* .directive */
        if (t[0].s[0] == '.') {
            if (tok_ieq(t[0].s, t[0].len, ".equ") || tok_ieq(t[0].s, t[0].len, ".def") ||
                tok_ieq(t[0].s, t[0].len, ".set")) {
                if (pass == 1 && do_equ(as, t + 1, nt - 1) != 0) return -1;
                continue;
            }
            return aerr(as, "unknown directive \"%.*s\" (only .equ is supported)",
                        t[0].len, t[0].s);
        }

        /* mnemonic */
        int op = -1;
        for (int k = 0; k < NMNEMONIC; k++)
            if (tok_ieq(t[0].s, t[0].len, mnemonic[k].m)) { op = mnemonic[k].op; break; }
        if (op < 0) {
            /* The overwhelmingly common near-miss is an access width this ISA
             * does not spell that way ("outq", "ldw"), so name the family. */
            static const struct { const char *pfx, *hint; } near[] = {
                { "ou", " (did you mean out8, out16 or out32?)" },
                { "in", " (did you mean in8, in16 or in32?)"    },
                { "ld", " (did you mean ld8, ld16 or ld32?)"    },
                { "mr", " (did you mean ld8, ld16 or ld32?)"    },
                { "st", " (did you mean st8, st16 or st32?)"    },
                { "mw", " (did you mean st8, st16 or st32?)"    },
                { "pc", " (did you mean pcicfg?)"               },
            };
            const char *hint = "";
            for (size_t k = 0; k < sizeof near / sizeof near[0]; k++)
                if (t[0].len >= 2 && dlower(t[0].s[0]) == near[k].pfx[0] &&
                    dlower(t[0].s[1]) == near[k].pfx[1]) { hint = near[k].hint; break; }
            return aerr(as, "unknown instruction \"%.*s\"%s", t[0].len, t[0].s, hint);
        }

        if (pass == 1) {
            if (as->p->ninsn >= DVM_MAX_INSNS)
                return aerr(as, "program too long (max %d instructions)", DVM_MAX_INSNS);
            as->p->ninsn++;
        } else {
            char mn[16];
            int  ml = t[0].len < 15 ? t[0].len : 15;
            for (int k = 0; k < ml; k++) mn[k] = dlower(t[0].s[k]);
            mn[ml] = '\0';
            if (assemble_insn(as, (uint8_t)op, t + 1, nt - 1, mn) != 0) return -1;
        }
    }
    as->p->src_lines = (uint32_t)as->line;
    return 0;
}

int dvm_assemble(const char *src, size_t len, dvm_program_t *out, dvm_asm_err_t *err) {
    if (err) { err->line = 0; err->msg[0] = '\0'; }
    if (!out) return -1;
    dz(out, sizeof *out);

    as_t as;
    dz(&as, sizeof as);
    as.p = out; as.err = err;

    if (!src && len) return aerr(&as, "no program text");
    if (len > DVM_SRC_MAX) {
        as.line = 0;
        return aerr(&as, "program is %lu bytes; the limit is %d",
                    (unsigned long)len, DVM_SRC_MAX);
    }

    if (assemble_pass(&as, src, len, 1) != 0) { dz(out, sizeof *out); return -1; }
    /* Labels survive pass 1; the instruction stream is rebuilt in pass 2. */
    if (assemble_pass(&as, src, len, 2) != 0) { dz(out, sizeof *out); return -1; }

    if (out->ninsn == 0) {
        as.line = 0;
        aerr(&as, "the program contains no instructions");
        dz(out, sizeof *out);
        return -1;
    }
    return 0;
}

/* ====================================================================== */
/* 4. validator + disassembler                                            */
/* ====================================================================== */

static int kind_is_value(uint8_t k) {
    return k == DVM_O_NONE || k == DVM_O_REG || k == DVM_O_IMM;
}

/* Which formats write a register, and which operand slot (if any) must be a
 * branch target or a string. Checked structurally so a hand-built program
 * cannot, say, hand a PC-kind operand to `out32` and have it treated as data. */
static int fmt_wants_dst(int fmt) {
    return fmt == F_RD || fmt == F_RD_A || fmt == F_RD_A_B || fmt == F_RD_PORT ||
           fmt == F_RD_ADDR || fmt == F_RD_BDF_OFF;
}

dvm_status_t dvm_program_validate(const dvm_program_t *p, dvm_asm_err_t *err) {
    if (err) { err->line = 0; err->msg[0] = '\0'; }
    if (!p) return DVM_TRAP_BADOP;

    if (p->ninsn == 0 || p->ninsn > DVM_MAX_INSNS) {
        if (err) snprintf(err->msg, sizeof err->msg,
                          "program has %lu instructions (must be 1-%d)",
                          (unsigned long)p->ninsn, DVM_MAX_INSNS);
        return DVM_TRAP_BADOP;
    }
    if (p->nstr > DVM_MAX_STRINGS || p->strused > DVM_STRPOOL ||
        p->nlabel > DVM_MAX_LABELS) {
        if (err) snprintf(err->msg, sizeof err->msg, "program tables are out of range");
        return DVM_TRAP_BADOP;
    }

    for (uint32_t i = 0; i < p->ninsn; i++) {
        const dvm_insn_t *in = &p->insn[i];
        if (err) err->line = (int)in->line;

        if (in->op >= DVM_OP__COUNT || !op_info[in->op].name) {
            if (err) snprintf(err->msg, sizeof err->msg,
                              "instruction %lu: opcode %u does not exist",
                              (unsigned long)i, in->op);
            return DVM_TRAP_BADOP;
        }
        int fmt = op_info[in->op].fmt;
        if (in->dst != DVM_NOREG && in->dst >= DVM_NREGS) {
            if (err) snprintf(err->msg, sizeof err->msg,
                              "instruction %lu (%s): destination register r%u does not exist",
                              (unsigned long)i, dvm_op_name(in->op), in->dst);
            return DVM_TRAP_RANGE;
        }
        if (fmt_wants_dst(fmt) && in->dst == DVM_NOREG) {
            if (err) snprintf(err->msg, sizeof err->msg,
                              "instruction %lu (%s): no destination register",
                              (unsigned long)i, dvm_op_name(in->op));
            return DVM_TRAP_BADOP;
        }
        /* Only a branch may carry a PC operand, and only in slot 0; only abort
         * and print may carry a string, and only in slot 0. */
        int want_pc  = (fmt == F_LBL);
        int want_str = (fmt == F_STR || fmt == F_STR_OPT_A);
        for (int k = 0; k < 3; k++) {
            int is_pc  = (in->kind[k] == DVM_O_PC);
            int is_str = (in->kind[k] == DVM_O_STR);
            if ((is_pc && !(want_pc && k == 0)) || (is_str && !(want_str && k == 0)) ||
                (!is_pc && !is_str && !kind_is_value(in->kind[k]))) {
                if (err) snprintf(err->msg, sizeof err->msg,
                                  "instruction %lu (%s): operand %d has kind %u, which this "
                                  "instruction cannot take",
                                  (unsigned long)i, dvm_op_name(in->op), k, in->kind[k]);
                return DVM_TRAP_BADOP;
            }
        }
        if (want_pc && in->kind[0] != DVM_O_PC) {
            if (err) snprintf(err->msg, sizeof err->msg,
                              "instruction %lu (%s): operand 0 is not a branch target",
                              (unsigned long)i, dvm_op_name(in->op));
            return DVM_TRAP_BADOP;
        }
        if (fmt == F_STR && in->kind[0] != DVM_O_STR) {
            if (err) snprintf(err->msg, sizeof err->msg,
                              "instruction %lu (%s): operand 0 is not a string",
                              (unsigned long)i, dvm_op_name(in->op));
            return DVM_TRAP_BADOP;
        }
        for (int k = 0; k < 3; k++) {
            switch (in->kind[k]) {
                case DVM_O_NONE: case DVM_O_IMM:
                    break;
                case DVM_O_REG:
                    if (in->val[k] >= DVM_NREGS) {
                        if (err) snprintf(err->msg, sizeof err->msg,
                                          "instruction %lu (%s): register r%lu does not exist",
                                          (unsigned long)i, dvm_op_name(in->op),
                                          (unsigned long)in->val[k]);
                        return DVM_TRAP_RANGE;
                    }
                    break;
                case DVM_O_PC:
                    if (in->val[k] >= p->ninsn) {
                        if (err) snprintf(err->msg, sizeof err->msg,
                                          "instruction %lu (%s): branch target %lu is outside "
                                          "the program (0-%lu)", (unsigned long)i,
                                          dvm_op_name(in->op), (unsigned long)in->val[k],
                                          (unsigned long)p->ninsn - 1);
                        return DVM_TRAP_PC;
                    }
                    break;
                case DVM_O_STR:
                    if (in->val[k] >= p->nstr || p->stroff[in->val[k]] >= DVM_STRPOOL) {
                        if (err) snprintf(err->msg, sizeof err->msg,
                                          "instruction %lu (%s): string %lu is outside the pool",
                                          (unsigned long)i, dvm_op_name(in->op),
                                          (unsigned long)in->val[k]);
                        return DVM_TRAP_BADOP;
                    }
                    break;
                default:
                    if (err) snprintf(err->msg, sizeof err->msg,
                                      "instruction %lu (%s): operand %d has an unknown kind %u",
                                      (unsigned long)i, dvm_op_name(in->op), k, in->kind[k]);
                    return DVM_TRAP_BADOP;
            }
        }
    }
    /* The pool must be NUL-terminated wherever a string starts. */
    for (uint32_t i = 0; i < p->nstr; i++) {
        uint32_t off = p->stroff[i], j = off;
        while (j < DVM_STRPOOL && p->strpool[j]) j++;
        if (j >= DVM_STRPOOL) {
            if (err) snprintf(err->msg, sizeof err->msg,
                              "string %lu is not terminated", (unsigned long)i);
            return DVM_TRAP_BADOP;
        }
    }
    if (err) err->line = 0;
    return DVM_OK;
}

static const char *label_at(const dvm_program_t *p, uint32_t pc) {
    for (uint32_t i = 0; i < p->nlabel; i++)
        if (p->label[i].pc == pc) return p->label[i].name;
    return NULL;
}

static void put_operand(sb_t *s, const dvm_program_t *p, uint8_t kind, uint64_t v) {
    switch (kind) {
        case DVM_O_REG: sb_addf(s, "r%lu", (unsigned long)v); break;
        case DVM_O_IMM:
            if (v < 10) sb_addf(s, "%lu", (unsigned long)v);
            else        sb_addf(s, "0x%lx", (unsigned long)v);
            break;
        case DVM_O_PC: {
            const char *l = label_at(p, (uint32_t)v);
            if (l) sb_addf(s, "%s", l);
            else   sb_addf(s, "@%lu", (unsigned long)v);
            break;
        }
        case DVM_O_STR: sb_addf(s, "\"%s\"", str_at(p, v)); break;
        default: sb_addf(s, "-"); break;
    }
}

size_t dvm_disasm(const dvm_program_t *p, uint32_t pc, char *buf, size_t cap) {
    if (!p || !buf || !cap) return 0;
    buf[0] = '\0';
    if (pc >= p->ninsn) return 0;

    const dvm_insn_t *in = &p->insn[pc];
    sb_t s;
    sb_init(&s, buf, cap);
    sb_addf(&s, "%s", dvm_op_name(in->op));

    int fmt = (in->op < DVM_OP__COUNT) ? op_info[in->op].fmt : F_NONE;
    switch (fmt) {
        case F_NONE:
            break;
        case F_RD: case F_RD_A: case F_RD_A_B: case F_RD_PORT: case F_RD_BDF_OFF:
            sb_addf(&s, " r%u", in->dst);
            for (int k = 0; k < 3; k++)
                if (in->kind[k] != DVM_O_NONE) {
                    sb_addf(&s, ", ");
                    put_operand(&s, p, in->kind[k], in->val[k]);
                }
            break;
        case F_RD_ADDR:
            sb_addf(&s, " r%u, [", in->dst);
            put_operand(&s, p, in->kind[0], in->val[0]);
            if (!(in->kind[1] == DVM_O_IMM && in->val[1] == 0)) {
                sb_addf(&s, "+");
                put_operand(&s, p, in->kind[1], in->val[1]);
            }
            sb_addf(&s, "]");
            break;
        case F_ADDR_A:
            sb_addf(&s, " [");
            put_operand(&s, p, in->kind[0], in->val[0]);
            if (!(in->kind[1] == DVM_O_IMM && in->val[1] == 0)) {
                sb_addf(&s, "+");
                put_operand(&s, p, in->kind[1], in->val[1]);
            }
            sb_addf(&s, "], ");
            put_operand(&s, p, in->kind[2], in->val[2]);
            break;
        default:
            for (int k = 0, first = 1; k < 3; k++)
                if (in->kind[k] != DVM_O_NONE) {
                    sb_addf(&s, first ? " " : ", ");
                    put_operand(&s, p, in->kind[k], in->val[k]);
                    first = 0;
                }
            break;
    }
    return s.len;
}

/* ====================================================================== */
/* 5. policy                                                              */
/* ====================================================================== */

/* Ports no caller may open, whatever it asks for. Everything here can take the
 * machine down or blind the operator, and none of it is a device a runtime
 * driver has any business bringing up.
 *
 * The reason strings end up in dvm_result_t.msg, so keep them plain ASCII:
 * copy_printable() replaces anything else, and the VGA console cannot render it
 * anyway. */
static const struct { uint16_t lo, hi; const char *why; } port_deny[] = {
    { 0x0020, 0x0021, "master PIC" },
    { 0x0040, 0x0043, "PIT: millis() and mdelay() are calibrated against it" },
    { 0x0060, 0x0064, "PS/2 controller: 0x64 can pulse the CPU reset line" },
    { 0x0070, 0x0071, "CMOS/NMI" },
    { 0x0092, 0x0092, "fast A20 / system reset" },
    { 0x00A0, 0x00A1, "slave PIC" },
    { 0x02F8, 0x02FF, "COM2" },
    { 0x03F8, 0x03FF, "COM1: the console this trace is printed on" },
    { 0x0CF8, 0x0CFF, "PCI config space: use pcicfg, which is read-only" },

    /* Below: not "can crash the machine" but "can destroy something that does
     * not come back". A window derived from a BAR is bounded by alignment and a
     * cap rather than by the BAR's true size, so it can be wider than the
     * device; these are the fixed legacy ranges that an over-wide window has
     * anything to lose by reaching. None of them is ever a PCI BAR. */
    { 0x0000, 0x000F, "ISA DMA controller 1: it bus-masters into RAM" },
    { 0x0080, 0x008F, "ISA DMA page registers" },
    { 0x00C0, 0x00DF, "ISA DMA controller 2: it bus-masters into RAM" },
    { 0x0170, 0x0177, "secondary ATA task file: it can write the disk" },
    { 0x01F0, 0x01F7, "primary ATA task file: it can write the disk" },
    { 0x03B0, 0x03DF, "legacy VGA registers: the operator's console at its "
                      "fixed address, the same asset as 0xb8000" },
    { 0x03F0, 0x03F7, "floppy controller" },
};
#define NPORT_DENY ((int)(sizeof port_deny / sizeof port_deny[0]))

/* IOAPIC / HPET / local APIC. See mmio_gate(). */
#define DVM_PLATFORM_LO 0xFEC00000ull
#define DVM_PLATFORM_HI 0xFEF00000ull

/* End of everything the kernel owns: the real-mode IVT, the VGA text buffer at
 * 0xB8000 (which shares a 2 MiB huge page with page 0 and so can never be
 * unmapped), the kernel image, and the heap arena in .bss. Rounded up to a huge
 * page. No MMIO window may start below this. */
#ifndef TALKOS_HOSTTEST
extern char __bss_end[];                 /* provided by linker.ld */
#endif

static uint64_t kernel_top(void) {
#ifndef TALKOS_HOSTTEST
    uint64_t e = (uint64_t)(uintptr_t)__bss_end;
    e = (e + 0x1FFFFFull) & ~0x1FFFFFull;
    return e < 0x1000000ull ? 0x1000000ull : e;
#else
    return 0x1000000ull;                 /* 16 MiB on the host, for tests */
#endif
}

void dvm_policy_init(dvm_policy_t *p) {
    if (!p) return;
    dz(p, sizeof *p);
    p->max_steps           = 100000;
    p->max_io              = 4096;
    p->max_delay_us        = 200000;     /* 200 ms */
    p->max_single_delay_us = 50000;      /* 50 ms  */
    p->max_prints          = 64;
    p->max_trace           = 512;
    p->trace               = DVM_TRACE_IO;
}

int dvm_policy_allow_ports(dvm_policy_t *p, uint16_t lo, uint16_t hi) {
    if (!p || lo > hi) return -1;
    if (p->nport < 0 || p->nport >= DVM_MAX_PORT_RANGES) return -1;
    p->port[p->nport].lo = lo;
    p->port[p->nport].hi = hi;
    p->nport++;
    return 0;
}

int dvm_policy_allow_mmio(dvm_policy_t *p, uint64_t base, uint64_t size, uint32_t flags) {
    if (!p || !size) return -1;
    if (flags & ~(uint32_t)DVM_MMIO_RW) return -1;
    if (!(flags & DVM_MMIO_RW)) return -1;
    if (base + size < base) return -1;                 /* wraps */
    if (p->nmmio < 0 || p->nmmio >= DVM_MAX_MMIO_WINS) return -1;
    p->mmio[p->nmmio].base  = base;
    p->mmio[p->nmmio].size  = size;
    p->mmio[p->nmmio].flags = flags;
    p->nmmio++;
    return 0;
}

int dvm_policy_allow_pci(dvm_policy_t *p, uint8_t bus, uint8_t dev, uint8_t fn) {
    if (!p || dev > 31 || fn > 7) return -1;
    if (p->npci < 0 || p->npci >= DVM_MAX_PCI_FNS) return -1;
    p->pci[p->npci].bus = bus;
    p->pci[p->npci].dev = dev;
    p->pci[p->npci].fn  = fn;
    p->npci++;
    return 0;
}

dvm_status_t dvm_policy_check(const dvm_policy_t *p, char *msg, size_t cap) {
    sb_t s;
    sb_init(&s, msg, cap);
    if (!p) { sb_addf(&s, "no policy supplied"); return DVM_TRAP_POLICY; }

    if (p->nport < 0 || p->nport > DVM_MAX_PORT_RANGES ||
        p->nmmio < 0 || p->nmmio > DVM_MAX_MMIO_WINS ||
        p->npci  < 0 || p->npci  > DVM_MAX_PCI_FNS) {
        sb_addf(&s, "policy table counts are out of range");
        return DVM_TRAP_POLICY;
    }

    for (int i = 0; i < p->nport; i++) {
        if (p->port[i].lo > p->port[i].hi) {
            sb_addf(&s, "port range %d is inverted (0x%04x > 0x%04x)", i,
                    (unsigned)p->port[i].lo, (unsigned)p->port[i].hi);
            return DVM_TRAP_POLICY;
        }
        for (int d = 0; d < NPORT_DENY; d++)
            if (p->port[i].lo <= port_deny[d].hi && p->port[i].hi >= port_deny[d].lo) {
                sb_addf(&s, "port range 0x%04x-0x%04x overlaps 0x%04x-0x%04x, which is "
                            "never allowed: %s",
                        (unsigned)p->port[i].lo, (unsigned)p->port[i].hi,
                        (unsigned)port_deny[d].lo, (unsigned)port_deny[d].hi,
                        port_deny[d].why);
                return DVM_TRAP_POLICY;
            }
    }

    uint64_t ktop = kernel_top();
    for (int i = 0; i < p->nmmio; i++) {
        const dvm_mmio_win_t *w = &p->mmio[i];
        if (!w->size || w->base + w->size < w->base) {
            sb_addf(&s, "mmio window %d (0x%lx+0x%lx) is empty or wraps", i,
                    (unsigned long)w->base, (unsigned long)w->size);
            return DVM_TRAP_POLICY;
        }
        if (!(w->flags & DVM_MMIO_RW) || (w->flags & ~(uint32_t)DVM_MMIO_RW)) {
            sb_addf(&s, "mmio window %d has no valid access flags (0x%lx)", i,
                    (unsigned long)w->flags);
            return DVM_TRAP_POLICY;
        }
        if (w->base < ktop) {
            sb_addf(&s, "mmio window 0x%lx+0x%lx reaches below 0x%lx, which is kernel "
                        "memory (the VGA text buffer at 0xb8000, the kernel image and "
                        "the heap all live there)",
                    (unsigned long)w->base, (unsigned long)w->size, (unsigned long)ktop);
            return DVM_TRAP_POLICY;
        }
        if (w->base + w->size > MMIO_TOP) {
            sb_addf(&s, "mmio window 0x%lx+0x%lx runs past 0x100000000; only the low "
                        "4 GiB is mapped",
                    (unsigned long)w->base, (unsigned long)w->size);
            return DVM_TRAP_POLICY;
        }
        if (w->base + w->size > DVM_PLATFORM_LO && w->base < DVM_PLATFORM_HI) {
            sb_addf(&s, "mmio window 0x%lx+0x%lx overlaps the platform block "
                        "(0x%lx-0x%lx: IOAPIC, HPET, local APIC), which is never "
                        "allowed",
                    (unsigned long)w->base, (unsigned long)w->size,
                    (unsigned long)DVM_PLATFORM_LO, (unsigned long)DVM_PLATFORM_HI - 1);
            return DVM_TRAP_POLICY;
        }
    }

    for (int i = 0; i < p->npci; i++)
        if (p->pci[i].dev > 31 || p->pci[i].fn > 7) {
            sb_addf(&s, "pci entry %d (%02x:%02x.%x) is not a valid address", i,
                    p->pci[i].bus, p->pci[i].dev, p->pci[i].fn);
            return DVM_TRAP_POLICY;
        }

    if (p->max_steps == 0 || p->max_steps > CEIL_STEPS) {
        sb_addf(&s, "max_steps %lu is outside 1-%lu",
                (unsigned long)p->max_steps, (unsigned long)CEIL_STEPS);
        return DVM_TRAP_POLICY;
    }
    if (p->max_io > CEIL_IO) {
        sb_addf(&s, "max_io %lu exceeds %lu", (unsigned long)p->max_io, (unsigned long)CEIL_IO);
        return DVM_TRAP_POLICY;
    }
    if (p->max_delay_us > CEIL_DELAY_US) {
        sb_addf(&s, "max_delay_us %lu exceeds %lu",
                (unsigned long)p->max_delay_us, (unsigned long)CEIL_DELAY_US);
        return DVM_TRAP_POLICY;
    }
    if (p->max_single_delay_us > CEIL_SINGLE_US) {
        sb_addf(&s, "max_single_delay_us %lu exceeds %lu",
                (unsigned long)p->max_single_delay_us, (unsigned long)CEIL_SINGLE_US);
        return DVM_TRAP_POLICY;
    }
    if (p->max_prints > CEIL_PRINTS) {
        sb_addf(&s, "max_prints %lu exceeds %lu",
                (unsigned long)p->max_prints, (unsigned long)CEIL_PRINTS);
        return DVM_TRAP_POLICY;
    }
    if (p->max_trace > CEIL_TRACE) {
        sb_addf(&s, "max_trace %lu exceeds %lu",
                (unsigned long)p->max_trace, (unsigned long)CEIL_TRACE);
        return DVM_TRAP_POLICY;
    }
    if ((int)p->trace < DVM_TRACE_OFF || (int)p->trace > DVM_TRACE_ALL) {
        sb_addf(&s, "trace level %d is not 0, 1 or 2", (int)p->trace);
        return DVM_TRAP_POLICY;
    }
    return DVM_OK;
}

/* ====================================================================== */
/* 6. the interpreter                                                     */
/* ====================================================================== */

typedef struct {
    const dvm_program_t *p;
    const dvm_policy_t  *pol;
    const dvm_io_t      *io;
    dvm_result_t        *res;

    uint64_t reg[DVM_NREGS];
    uint64_t dstack[DVM_DSTACK];
    uint32_t dsp;
    uint32_t cstack[DVM_CSTACK];
    uint32_t csp;

    uint32_t pc;
    uint32_t line;
    int      zero, below;     /* flags from cmp/test */
    int      quiet;           /* trace budget spent  */
} run_t;

/* ---- trace ---- */

static int tracing(const run_t *st, dvm_trace_t level) {
    return st->pol->trace >= level;
}

/* Returns 1 when a line may be emitted. Once the budget is spent it says so
 * ONCE, then counts every line it swallows so the result can report how much of
 * the transcript is missing — a silently truncated trace would be worse than a
 * loud one. Trap lines bypass this entirely. */
static int trace_budget(run_t *st) {
    if (st->quiet || st->res->trace_lines >= st->pol->max_trace) {
        if (!st->quiet) {
            st->quiet = 1;
            st->res->trace_lines++;
            trace_ok("dvm.trace", "line budget %lu reached; further dvm lines suppressed",
                     (unsigned long)st->pol->max_trace);
        }
        st->res->trace_dropped++;
        return 0;
    }
    st->res->trace_lines++;
    return 1;
}

static void tline(run_t *st, const char *fmt, ...) {
    if (!trace_budget(st)) return;
    char buf[TRACE_LINE_MAX];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    trace_ok("dvm", "%s", buf);
}

/* ---- traps ---- */

static dvm_status_t trap(run_t *st, dvm_status_t s, const char *fmt, ...) {
    char reason[DVM_MSG_MAX];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(reason, sizeof reason, fmt, ap);
    va_end(ap);

    st->res->status = s;
    st->res->pc     = st->pc;
    st->res->line   = st->line;
    copy_printable(st->res->msg, sizeof st->res->msg, reason);

    if (st->pol->trace != DVM_TRACE_OFF) {
        /* A trap line is never suppressed: it is the one line that explains the
         * run, and the budget exists to stop noise, not evidence. */
        st->res->trace_lines++;
        trace_err(DVM_TRACE_ERR, "dvm.trap", "pc=%03lu ln=%lu %s: %s",
                  (unsigned long)st->pc, (unsigned long)st->line,
                  dvm_status_name(s), reason);
    }
    return s;
}

/* ---- operand access ---- */

static uint64_t rd(const run_t *st, const dvm_insn_t *in, int k) {
    switch (in->kind[k]) {
        case DVM_O_REG: return st->reg[in->val[k] & (DVM_NREGS - 1)];
        case DVM_O_IMM:
        case DVM_O_PC:
        case DVM_O_STR: return in->val[k];
        default:        return 0;
    }
}

/* ---- access checks ---- */

static const char *port_denied_why(uint32_t port, uint32_t width) {
    for (int d = 0; d < NPORT_DENY; d++)
        if (port <= port_deny[d].hi && port + width - 1 >= port_deny[d].lo)
            return port_deny[d].why;
    return NULL;
}

static int port_allowed(const dvm_policy_t *p, uint32_t port, uint32_t width) {
    for (int i = 0; i < p->nport; i++)
        if (port >= p->port[i].lo && port + width - 1 <= p->port[i].hi) return 1;
    return 0;
}

static void put_port_grants(sb_t *s, const dvm_policy_t *p) {
    if (!p->nport) { sb_addf(s, "none"); return; }
    for (int i = 0; i < p->nport; i++)
        sb_addf(s, "%s0x%04x-0x%04x", i ? "," : "",
                (unsigned)p->port[i].lo, (unsigned)p->port[i].hi);
}

static void put_mmio_grants(sb_t *s, const dvm_policy_t *p) {
    if (!p->nmmio) { sb_addf(s, "none"); return; }
    for (int i = 0; i < p->nmmio; i++)
        sb_addf(s, "%s0x%lx+0x%lx%s%s", i ? "," : "",
                (unsigned long)p->mmio[i].base, (unsigned long)p->mmio[i].size,
                (p->mmio[i].flags & DVM_MMIO_R) ? "r" : "",
                (p->mmio[i].flags & DVM_MMIO_W) ? "w" : "");
}

/* Every port access funnels through here. Order matters: a refusal is reported
 * ahead of an exhausted budget, so "it tried to touch the PIC" is never hidden
 * behind "it ran out of I/O credit". */
static dvm_status_t port_gate(run_t *st, uint64_t rawport, uint32_t width,
                              const char *what) {
    if (rawport > 0xFFFFu || rawport + width - 1 > 0xFFFFu)
        return trap(st, DVM_TRAP_RANGE,
                    "%s: port 0x%lx is outside the 0x0000-0xffff I/O space",
                    what, (unsigned long)rawport);

    const char *why = port_denied_why((uint32_t)rawport, width);
    if (why)
        return trap(st, DVM_TRAP_PORT_DENIED,
                    "%s: port 0x%04lx is never accessible from a driver program (%s)",
                    what, (unsigned long)rawport, why);

    if (!port_allowed(st->pol, (uint32_t)rawport, width)) {
        char g[96];
        sb_t s; sb_init(&s, g, sizeof g);
        put_port_grants(&s, st->pol);
        return trap(st, DVM_TRAP_PORT_DENIED,
                    "%s: port 0x%04lx is not in this program's allowed ranges (%s)",
                    what, (unsigned long)rawport, g);
    }
    if (st->res->io_ops >= st->pol->max_io)
        return trap(st, DVM_TRAP_IO_BUDGET,
                    "device access budget of %lu reached at %s",
                    (unsigned long)st->pol->max_io, what);
    st->res->io_ops++;
    return DVM_OK;
}

static dvm_status_t mmio_gate(run_t *st, uint64_t addr, uint32_t width, int write,
                              const char *what) {
    if (addr + width < addr || addr + width > MMIO_TOP)
        return trap(st, DVM_TRAP_RANGE,
                    "%s: address 0x%lx is above the mapped 4 GiB of physical space",
                    what, (unsigned long)addr);
    if (addr < kernel_top())
        return trap(st, DVM_TRAP_MMIO_DENIED,
                    "%s: address 0x%lx is kernel memory (below 0x%lx: the VGA text "
                    "buffer, the kernel image and the heap live there)",
                    what, (unsigned long)addr, (unsigned long)kernel_top());
    /* The platform block: IOAPIC, HPET, local APIC. Interrupt routing lives
     * here. No BAR belongs in it, so nothing legitimate is being refused, and a
     * caller that over-granted a window is not able to hand it out by accident.
     * tools/dvm_tools.c refuses to derive a window here too; this is the same
     * rule one layer down, where every caller of dvm_policy_allow_mmio() gets
     * it whether it thought about the question or not. */
    if (addr + width > DVM_PLATFORM_LO && addr < DVM_PLATFORM_HI)
        return trap(st, DVM_TRAP_MMIO_DENIED,
                    "%s: address 0x%lx is in the platform block (0x%lx-0x%lx: "
                    "IOAPIC, HPET and the local APIC). Interrupt routing is not "
                    "reachable from a driver program",
                    what, (unsigned long)addr,
                    (unsigned long)DVM_PLATFORM_LO, (unsigned long)DVM_PLATFORM_HI - 1);

    const dvm_mmio_win_t *w = NULL;
    for (int i = 0; i < st->pol->nmmio; i++) {
        const dvm_mmio_win_t *c = &st->pol->mmio[i];
        if (addr >= c->base && addr + width <= c->base + c->size) { w = c; break; }
    }
    if (!w) {
        char g[128];
        sb_t s; sb_init(&s, g, sizeof g);
        put_mmio_grants(&s, st->pol);
        return trap(st, DVM_TRAP_MMIO_DENIED,
                    "%s: address 0x%lx+%lu is not inside any allowed window (%s)",
                    what, (unsigned long)addr, (unsigned long)width, g);
    }
    if (write && !(w->flags & DVM_MMIO_W))
        return trap(st, DVM_TRAP_MMIO_DENIED,
                    "%s: window 0x%lx+0x%lx is read-only",
                    what, (unsigned long)w->base, (unsigned long)w->size);
    if (!write && !(w->flags & DVM_MMIO_R))
        return trap(st, DVM_TRAP_MMIO_DENIED,
                    "%s: window 0x%lx+0x%lx is write-only",
                    what, (unsigned long)w->base, (unsigned long)w->size);
    if (addr % width)
        return trap(st, DVM_TRAP_ALIGN,
                    "%s: address 0x%lx is not %lu-byte aligned; device registers "
                    "require a naturally aligned access",
                    what, (unsigned long)addr, (unsigned long)width);
    if (st->res->io_ops >= st->pol->max_io)
        return trap(st, DVM_TRAP_IO_BUDGET,
                    "device access budget of %lu reached at %s",
                    (unsigned long)st->pol->max_io, what);
    st->res->io_ops++;
    return DVM_OK;
}

/* ---- the loop ---- */

static dvm_status_t execute(run_t *st) {
    const dvm_program_t *p   = st->p;
    const dvm_policy_t  *pol = st->pol;
    dvm_result_t        *res = st->res;

    for (;;) {
        if (res->steps >= pol->max_steps) {
            st->line = (st->pc < p->ninsn) ? p->insn[st->pc].line : 0;
            return trap(st, DVM_TRAP_STEPS,
                        "instruction budget of %lu exhausted; the program did not "
                        "reach halt (an unterminated loop?)",
                        (unsigned long)pol->max_steps);
        }
        if (st->pc >= p->ninsn) {
            st->line = p->insn[p->ninsn - 1].line;
            return trap(st, DVM_TRAP_PC,
                        "ran past the last instruction; every path must end in halt "
                        "or abort");
        }

        const dvm_insn_t *in = &p->insn[st->pc];
        st->line = in->line;
        res->steps++;
        uint32_t next = st->pc + 1;

        uint64_t a = rd(st, in, 0), b = rd(st, in, 1), c = rd(st, in, 2);
        uint64_t v = 0;
        int      is_alu = 0;

        switch ((dvm_op_t)in->op) {

        /* ---- control ---- */
        case DVM_HALT:
            res->status = DVM_OK;
            res->pc = st->pc;
            res->msg[0] = '\0';
            return DVM_OK;

        case DVM_ABORT: {
            char msg[DVM_MSG_MAX];
            copy_printable(msg, sizeof msg, str_at(p, in->val[0]));
            return trap(st, DVM_TRAP_ABORT, "%s", msg);
        }

        case DVM_NOP:
            if (tracing(st, DVM_TRACE_ALL)) tline(st, "pc=%03lu ln=%lu nop",
                                                  (unsigned long)st->pc, (unsigned long)st->line);
            break;

        /* ---- data / ALU ---- */
        case DVM_MOV: v = a;      is_alu = 1; break;
        case DVM_ADD: v = a + b;  is_alu = 1; break;
        case DVM_SUB: v = a - b;  is_alu = 1; break;
        case DVM_MUL: v = a * b;  is_alu = 1; break;
        case DVM_DIV:
            if (!b) return trap(st, DVM_TRAP_DIV0, "div by zero");
            v = a / b; is_alu = 1; break;
        case DVM_MOD:
            if (!b) return trap(st, DVM_TRAP_DIV0, "mod by zero");
            v = a % b; is_alu = 1; break;
        case DVM_AND: v = a & b;  is_alu = 1; break;
        case DVM_OR:  v = a | b;  is_alu = 1; break;
        case DVM_XOR: v = a ^ b;  is_alu = 1; break;
        case DVM_NOT: v = ~a;     is_alu = 1; break;
        case DVM_SHL: v = a << (b & 63); is_alu = 1; break;
        case DVM_SHR: v = a >> (b & 63); is_alu = 1; break;

        case DVM_CMP:
            st->zero  = (a == b);
            st->below = (a < b);
            if (tracing(st, DVM_TRACE_ALL))
                tline(st, "pc=%03lu ln=%lu cmp 0x%lx,0x%lx eq=%d below=%d",
                      (unsigned long)st->pc, (unsigned long)st->line,
                      (unsigned long)a, (unsigned long)b, st->zero, st->below);
            break;

        case DVM_TEST:
            st->zero  = ((a & b) == 0);
            st->below = 0;
            if (tracing(st, DVM_TRACE_ALL))
                tline(st, "pc=%03lu ln=%lu test 0x%lx&0x%lx zero=%d",
                      (unsigned long)st->pc, (unsigned long)st->line,
                      (unsigned long)a, (unsigned long)b, st->zero);
            break;

        /* ---- branches ---- */
        case DVM_JMP: case DVM_BEQ: case DVM_BNE:
        case DVM_BLT: case DVM_BLE: case DVM_BGT: case DVM_BGE: {
            int take = 0;
            switch ((dvm_op_t)in->op) {
                case DVM_JMP: take = 1; break;
                case DVM_BEQ: take = st->zero; break;
                case DVM_BNE: take = !st->zero; break;
                case DVM_BLT: take = st->below; break;
                case DVM_BLE: take = st->below || st->zero; break;
                case DVM_BGT: take = !st->below && !st->zero; break;
                case DVM_BGE: take = !st->below; break;
                default: break;
            }
            uint64_t target = in->val[0];
            if (take) {
                if (in->kind[0] != DVM_O_PC || target >= p->ninsn)
                    return trap(st, DVM_TRAP_PC,
                                "%s: branch target %lu is outside the program (0-%lu)",
                                dvm_op_name(in->op), (unsigned long)target,
                                (unsigned long)p->ninsn - 1);
                next = (uint32_t)target;
            }
            if (tracing(st, DVM_TRACE_ALL))
                tline(st, "pc=%03lu ln=%lu %s %s pc->%03lu",
                      (unsigned long)st->pc, (unsigned long)st->line,
                      dvm_op_name(in->op), take ? "taken" : "not-taken",
                      (unsigned long)next);
            break;
        }

        case DVM_CALL:
            if (st->csp >= DVM_CSTACK)
                return trap(st, DVM_TRAP_STACK,
                            "call stack overflow (depth %d); recursion is bounded",
                            DVM_CSTACK);
            if (in->kind[0] != DVM_O_PC || in->val[0] >= p->ninsn)
                return trap(st, DVM_TRAP_PC, "call: target %lu is outside the program",
                            (unsigned long)in->val[0]);
            st->cstack[st->csp++] = next;
            next = (uint32_t)in->val[0];
            if (tracing(st, DVM_TRACE_ALL))
                tline(st, "pc=%03lu ln=%lu call pc->%03lu depth=%lu",
                      (unsigned long)st->pc, (unsigned long)st->line,
                      (unsigned long)next, (unsigned long)st->csp);
            break;

        case DVM_RET:
            if (st->csp == 0)
                return trap(st, DVM_TRAP_STACK, "ret with an empty call stack");
            next = st->cstack[--st->csp];
            if (next > p->ninsn)
                return trap(st, DVM_TRAP_PC, "ret: return address %lu is outside the program",
                            (unsigned long)next);
            if (tracing(st, DVM_TRACE_ALL))
                tline(st, "pc=%03lu ln=%lu ret pc->%03lu depth=%lu",
                      (unsigned long)st->pc, (unsigned long)st->line,
                      (unsigned long)next, (unsigned long)st->csp);
            break;

        case DVM_PUSH:
            if (st->dsp >= DVM_DSTACK)
                return trap(st, DVM_TRAP_STACK, "data stack overflow (depth %d)", DVM_DSTACK);
            st->dstack[st->dsp++] = a;
            if (tracing(st, DVM_TRACE_ALL))
                tline(st, "pc=%03lu ln=%lu push 0x%lx depth=%lu",
                      (unsigned long)st->pc, (unsigned long)st->line,
                      (unsigned long)a, (unsigned long)st->dsp);
            break;

        case DVM_POP:
            if (st->dsp == 0)
                return trap(st, DVM_TRAP_STACK, "pop from an empty data stack");
            v = st->dstack[--st->dsp];
            is_alu = 1;
            break;

        /* ---- port I/O ---- */
        case DVM_OUT8: case DVM_OUT16: case DVM_OUT32: {
            uint32_t width = in->op == DVM_OUT8 ? 1 : in->op == DVM_OUT16 ? 2 : 4;
            const char *mn = dvm_op_name(in->op);
            dvm_status_t rc = port_gate(st, a, width, mn);
            if (rc != DVM_OK) return rc;
            if (!st->io->port_write)
                return trap(st, DVM_TRAP_NOIO, "%s: this VM has no port I/O backend", mn);
            uint32_t val = (uint32_t)(width == 1 ? (b & 0xFF) : width == 2 ? (b & 0xFFFF)
                                                              : (b & 0xFFFFFFFFu));
            st->io->port_write(st->io->ctx, (uint16_t)a, (uint8_t)width, val);
            res->n_port_wr++;
            if (tracing(st, DVM_TRACE_IO))
                tline(st, "pc=%03lu ln=%lu %s port=0x%04lx val=0x%lx",
                      (unsigned long)st->pc, (unsigned long)st->line, mn,
                      (unsigned long)a, (unsigned long)val);
            break;
        }

        case DVM_IN8: case DVM_IN16: case DVM_IN32: {
            uint32_t width = in->op == DVM_IN8 ? 1 : in->op == DVM_IN16 ? 2 : 4;
            const char *mn = dvm_op_name(in->op);
            dvm_status_t rc = port_gate(st, a, width, mn);
            if (rc != DVM_OK) return rc;
            if (!st->io->port_read)
                return trap(st, DVM_TRAP_NOIO, "%s: this VM has no port I/O backend", mn);
            v = st->io->port_read(st->io->ctx, (uint16_t)a, (uint8_t)width);
            if (width == 1) v &= 0xFF; else if (width == 2) v &= 0xFFFF;
            res->n_port_rd++;
            is_alu = 1;
            if (tracing(st, DVM_TRACE_IO))
                tline(st, "pc=%03lu ln=%lu %s port=0x%04lx r%u=0x%lx",
                      (unsigned long)st->pc, (unsigned long)st->line, mn,
                      (unsigned long)a, in->dst, (unsigned long)v);
            break;
        }

        /* ---- MMIO ---- */
        case DVM_LD8: case DVM_LD16: case DVM_LD32: {
            uint32_t width = in->op == DVM_LD8 ? 1 : in->op == DVM_LD16 ? 2 : 4;
            const char *mn = dvm_op_name(in->op);
            uint64_t addr = a + b;
            dvm_status_t rc = mmio_gate(st, addr, width, 0, mn);
            if (rc != DVM_OK) return rc;
            if (!st->io->mmio_read)
                return trap(st, DVM_TRAP_NOIO, "%s: this VM has no MMIO backend", mn);
            v = st->io->mmio_read(st->io->ctx, addr, (uint8_t)width);
            if (width == 1) v &= 0xFF; else if (width == 2) v &= 0xFFFF;
            res->n_mmio_rd++;
            is_alu = 1;
            if (tracing(st, DVM_TRACE_IO))
                tline(st, "pc=%03lu ln=%lu %s addr=0x%lx r%u=0x%lx",
                      (unsigned long)st->pc, (unsigned long)st->line, mn,
                      (unsigned long)addr, in->dst, (unsigned long)v);
            break;
        }

        case DVM_ST8: case DVM_ST16: case DVM_ST32: {
            uint32_t width = in->op == DVM_ST8 ? 1 : in->op == DVM_ST16 ? 2 : 4;
            const char *mn = dvm_op_name(in->op);
            uint64_t addr = a + b;
            dvm_status_t rc = mmio_gate(st, addr, width, 1, mn);
            if (rc != DVM_OK) return rc;
            if (!st->io->mmio_write)
                return trap(st, DVM_TRAP_NOIO, "%s: this VM has no MMIO backend", mn);
            uint32_t val = (uint32_t)(width == 1 ? (c & 0xFF) : width == 2 ? (c & 0xFFFF)
                                                              : (c & 0xFFFFFFFFu));
            st->io->mmio_write(st->io->ctx, addr, (uint8_t)width, val);
            res->n_mmio_wr++;
            if (tracing(st, DVM_TRACE_IO))
                tline(st, "pc=%03lu ln=%lu %s addr=0x%lx val=0x%lx",
                      (unsigned long)st->pc, (unsigned long)st->line, mn,
                      (unsigned long)addr, (unsigned long)val);
            break;
        }

        /* ---- PCI config read ---- */
        case DVM_PCICFG: {
            if (a > 0xFFFFu)
                return trap(st, DVM_TRAP_RANGE,
                            "pcicfg: bdf 0x%lx is out of range (bus<<8 | dev<<3 | fn)",
                            (unsigned long)a);
            uint8_t bus = (uint8_t)((a >> 8) & 0xFF);
            uint8_t dev = (uint8_t)((a >> 3) & 0x1F);
            uint8_t fn  = (uint8_t)(a & 7);
            if (b > 0xFF)
                return trap(st, DVM_TRAP_RANGE,
                            "pcicfg: offset 0x%lx is outside the 256-byte config space",
                            (unsigned long)b);
            int ok = 0;
            for (int i = 0; i < pol->npci; i++)
                if (pol->pci[i].bus == bus && pol->pci[i].dev == dev && pol->pci[i].fn == fn) {
                    ok = 1; break;
                }
            if (!ok)
                return trap(st, DVM_TRAP_PCI_DENIED,
                            "pcicfg: %02x:%02x.%x is not one of the %d function(s) this "
                            "program may read", bus, dev, fn, pol->npci);
            if (b & 3)
                return trap(st, DVM_TRAP_ALIGN,
                            "pcicfg: offset 0x%02lx must be 4-byte aligned; config space is "
                            "read a dword at a time", (unsigned long)b);
            if (res->io_ops >= pol->max_io)
                return trap(st, DVM_TRAP_IO_BUDGET,
                            "device access budget of %lu reached at pcicfg",
                            (unsigned long)pol->max_io);
            if (!st->io->pci_read32)
                return trap(st, DVM_TRAP_NOIO, "pcicfg: this VM has no PCI backend");
            res->io_ops++;
            v = st->io->pci_read32(st->io->ctx, bus, dev, fn, (uint8_t)b);
            res->n_pci_rd++;
            is_alu = 1;
            if (tracing(st, DVM_TRACE_IO))
                tline(st, "pc=%03lu ln=%lu pcicfg %02x:%02x.%x off=0x%02lx r%u=0x%08lx",
                      (unsigned long)st->pc, (unsigned long)st->line, bus, dev, fn,
                      (unsigned long)b, in->dst, (unsigned long)v);
            break;
        }

        /* ---- delay ---- */
        case DVM_DELAY: {
            if (a > pol->max_single_delay_us)
                return trap(st, DVM_TRAP_DELAY_BUDGET,
                            "delay of %lu us exceeds the %lu us single-delay limit",
                            (unsigned long)a, (unsigned long)pol->max_single_delay_us);
            if (res->delay_us + a > pol->max_delay_us)
                return trap(st, DVM_TRAP_DELAY_BUDGET,
                            "delay budget exhausted: %lu us already spent, %lu us "
                            "requested, %lu us allowed",
                            (unsigned long)res->delay_us, (unsigned long)a,
                            (unsigned long)pol->max_delay_us);
            if (!st->io->delay_us)
                return trap(st, DVM_TRAP_NOIO, "delay: this VM has no timer backend");
            res->delay_us += a;
            st->io->delay_us(st->io->ctx, (uint32_t)a);
            if (tracing(st, DVM_TRACE_IO))
                tline(st, "pc=%03lu ln=%lu delay %lu us (total %lu)",
                      (unsigned long)st->pc, (unsigned long)st->line,
                      (unsigned long)a, (unsigned long)res->delay_us);
            break;
        }

        /* ---- print ---- */
        case DVM_PRINT: {
            if (res->prints >= pol->max_prints)
                return trap(st, DVM_TRAP_PRINT_BUDGET,
                            "print budget of %lu lines reached",
                            (unsigned long)pol->max_prints);
            res->prints++;
            const char *text = str_at(p, in->val[0]);
            if (pol->trace != DVM_TRACE_OFF && trace_budget(st)) {
                if (in->kind[1] != DVM_O_NONE)
                    trace_ok("dvm.print", "%s = 0x%lx", text, (unsigned long)b);
                else
                    trace_ok("dvm.print", "%s", text);
            }
            break;
        }

        default:
            return trap(st, DVM_TRAP_BADOP, "opcode %u is not implemented", in->op);
        }

        if (is_alu) {
            if (in->dst >= DVM_NREGS)
                return trap(st, DVM_TRAP_RANGE, "%s: destination register r%u does not exist",
                            dvm_op_name(in->op), in->dst);
            st->reg[in->dst] = v;
            /* I/O reads already traced above, with their access details. */
            if (tracing(st, DVM_TRACE_ALL) && in->op != DVM_IN8 && in->op != DVM_IN16 &&
                in->op != DVM_IN32 && in->op != DVM_LD8 && in->op != DVM_LD16 &&
                in->op != DVM_LD32 && in->op != DVM_PCICFG)
                tline(st, "pc=%03lu ln=%lu %s r%u=0x%lx",
                      (unsigned long)st->pc, (unsigned long)st->line,
                      dvm_op_name(in->op), in->dst, (unsigned long)v);
        }

        st->pc = next;
    }
}

dvm_status_t dvm_run(const dvm_program_t *p, const dvm_policy_t *pol,
                     const dvm_io_t *io, const uint64_t *args, int nargs,
                     dvm_result_t *res) {
    static const dvm_policy_t deny_all_fallback;   /* zeroed: refuses everything */

    if (!res) return DVM_TRAP_POLICY;
    dz(res, sizeof *res);

    run_t st;
    dz(&st, sizeof st);
    st.p   = p;
    st.pol = pol ? pol : &deny_all_fallback;
    st.io  = io;
    st.res = res;

    if (!p || !pol || !io) {
        res->status = DVM_TRAP_POLICY;
        copy_printable(res->msg, sizeof res->msg,
                       !p   ? "no program supplied" :
                       !pol ? "no policy supplied"  : "no I/O backend supplied");
        return DVM_TRAP_POLICY;
    }

    char pmsg[DVM_MSG_MAX];
    dvm_status_t ps = dvm_policy_check(pol, pmsg, sizeof pmsg);
    if (ps != DVM_OK) {
        res->status = ps;
        copy_printable(res->msg, sizeof res->msg, pmsg);
        if (pol->trace != DVM_TRACE_OFF)
            trace_err(DVM_TRACE_ERR, "dvm.trap", "%s: %s", dvm_status_name(ps), pmsg);
        return ps;
    }

    dvm_asm_err_t verr;
    dvm_status_t vs = dvm_program_validate(p, &verr);
    if (vs != DVM_OK) {
        res->status = vs;
        res->line   = (uint32_t)verr.line;
        copy_printable(res->msg, sizeof res->msg, verr.msg);
        if (pol->trace != DVM_TRACE_OFF)
            trace_err(DVM_TRACE_ERR, "dvm.trap", "%s: %s", dvm_status_name(vs), verr.msg);
        return vs;
    }

    if (nargs < 0) nargs = 0;
    if (nargs > DVM_NREGS) nargs = DVM_NREGS;
    for (int i = 0; i < nargs && args; i++) st.reg[i] = args[i];

    /* State the sandbox before running inside it: these lines are the record of
     * what the program was permitted to touch, which is half of any later
     * diagnosis of what it did. */
    if (pol->trace != DVM_TRACE_OFF) {
        char g[192];
        sb_t s;

        sb_init(&s, g, sizeof g);
        put_port_grants(&s, pol);
        res->trace_lines++;
        trace_ok("dvm.grant", "ports %s", g);

        sb_init(&s, g, sizeof g);
        put_mmio_grants(&s, pol);
        res->trace_lines++;
        trace_ok("dvm.grant", "mmio %s", g);

        sb_init(&s, g, sizeof g);
        if (!pol->npci) sb_addf(&s, "none");
        for (int i = 0; i < pol->npci; i++)
            sb_addf(&s, "%s%02x:%02x.%x", i ? "," : "",
                    pol->pci[i].bus, pol->pci[i].dev, pol->pci[i].fn);
        res->trace_lines++;
        trace_ok("dvm.grant", "pci %s", g);

        res->trace_lines++;
        trace_ok("dvm.run", "insns=%lu args=%d steps<=%lu io<=%lu delay<=%lu us trace=%d",
                 (unsigned long)p->ninsn, nargs, (unsigned long)pol->max_steps,
                 (unsigned long)pol->max_io, (unsigned long)pol->max_delay_us,
                 (int)pol->trace);
    }

    dvm_status_t s = execute(&st);

    for (int i = 0; i < DVM_NREGS; i++) res->reg[i] = st.reg[i];
    res->status = s;
    res->pc     = st.pc;
    res->line   = st.line;

    if (pol->trace != DVM_TRACE_OFF) {
        res->trace_lines++;
        if (s == DVM_OK)
            trace_ok("dvm.halt", "pc=%03lu ln=%lu steps=%lu io=%lu (pr=%lu pw=%lu mr=%lu "
                                 "mw=%lu pci=%lu) delay=%lu us prints=%lu",
                     (unsigned long)res->pc, (unsigned long)res->line,
                     (unsigned long)res->steps, (unsigned long)res->io_ops,
                     (unsigned long)res->n_port_rd, (unsigned long)res->n_port_wr,
                     (unsigned long)res->n_mmio_rd, (unsigned long)res->n_mmio_wr,
                     (unsigned long)res->n_pci_rd, (unsigned long)res->delay_us,
                     (unsigned long)res->prints);
        else
            trace_err(DVM_TRACE_ERR, "dvm.stop", "%s steps=%lu io=%lu delay=%lu us",
                      dvm_status_name(s), (unsigned long)res->steps,
                      (unsigned long)res->io_ops, (unsigned long)res->delay_us);
    }
    return s;
}

/* ====================================================================== */
/* 7. the hardware backend                                                */
/* ====================================================================== */

#ifndef TALKOS_HOSTTEST

#include "io.h"
#include "pci.h"
#include "kernel.h"     /* mdelay, millis */

static void hw_port_write(void *ctx, uint16_t port, uint8_t width, uint32_t val) {
    (void)ctx;
    switch (width) {
        case 1: outb(port, (uint8_t)val); break;
        case 2: outw(port, (uint16_t)val); break;
        default: outl(port, val); break;
    }
}

static uint32_t hw_port_read(void *ctx, uint16_t port, uint8_t width) {
    (void)ctx;
    switch (width) {
        case 1: return inb(port);
        case 2: return inw(port);
        default: return inl(port);
    }
}

static void hw_mmio_write(void *ctx, uint64_t addr, uint8_t width, uint32_t val) {
    (void)ctx;
    volatile void *p = (volatile void *)(uintptr_t)addr;
    switch (width) {
        case 1: *(volatile uint8_t *)p  = (uint8_t)val; break;
        case 2: *(volatile uint16_t *)p = (uint16_t)val; break;
        default: *(volatile uint32_t *)p = val; break;
    }
}

static uint32_t hw_mmio_read(void *ctx, uint64_t addr, uint8_t width) {
    (void)ctx;
    volatile void *p = (volatile void *)(uintptr_t)addr;
    switch (width) {
        case 1: return *(volatile uint8_t *)p;
        case 2: return *(volatile uint16_t *)p;
        default: return *(volatile uint32_t *)p;
    }
}

static uint32_t hw_pci_read32(void *ctx, uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off) {
    (void)ctx;
    return pci_cfg_read32(bus, dev, fn, off);
}

/* DELAY is documented as a floor, not a precise timer. mdelay() covers whole
 * milliseconds; the remainder is a TSC spin calibrated once against millis().
 * Both the calibration loops are iteration-bounded, because a delay primitive
 * that can hang is worse than one that is imprecise. */
static uint64_t tsc_per_us;      /* 0 = uncalibrated, -1 = unusable */

static void calibrate_tsc(void) {
    uint64_t t0 = millis();
    uint64_t guard = 0;
    while (millis() == t0 && ++guard < 50000000ull) { }
    if (guard >= 50000000ull) { tsc_per_us = (uint64_t)-1; return; }

    uint64_t start = rdtsc(), t1 = millis();
    guard = 0;
    while (millis() - t1 < 2 && ++guard < 50000000ull) { }
    uint64_t ticks = rdtsc() - start;
    tsc_per_us = ticks / 2000u;
    if (!tsc_per_us) tsc_per_us = (uint64_t)-1;
}

static void hw_delay_us(void *ctx, uint32_t us) {
    (void)ctx;
    uint32_t ms = us / 1000u, rem = us % 1000u;
    if (ms) mdelay(ms);
    if (!rem) return;
    if (!tsc_per_us) calibrate_tsc();
    if (tsc_per_us == (uint64_t)-1) {
        for (volatile uint32_t i = 0; i < rem * 20u; i++) { }   /* bounded fallback */
        return;
    }
    uint64_t end = rdtsc() + (uint64_t)rem * tsc_per_us;
    while (rdtsc() < end) { }
}

static const dvm_io_t hw_io = {
    .ctx        = 0,
    .port_write = hw_port_write,
    .port_read  = hw_port_read,
    .mmio_write = hw_mmio_write,
    .mmio_read  = hw_mmio_read,
    .pci_read32 = hw_pci_read32,
    .delay_us   = hw_delay_us,
};

const dvm_io_t *dvm_io_hardware(void) { return &hw_io; }

#else   /* host build: there is no hardware to reach */

const dvm_io_t *dvm_io_hardware(void) { return 0; }

#endif
