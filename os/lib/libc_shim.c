/* libc_shim.c — the handful of libc functions lwIP and mbedTLS need that aren't
 * already in base.c, plus the lwIP port hooks and an entropy source for TLS. */

#include "kernel.h"
#include "io.h"
#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>

/* ---- string ---- */
int strcmp(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

int strncmp(const char *a, const char *b, size_t n) {
    while (n && *a && *a == *b) { a++; b++; n--; }
    if (n == 0) return 0;
    return (unsigned char)*a - (unsigned char)*b;
}

char *strncpy(char *dst, const char *src, size_t n) {
    size_t i = 0;
    for (; i < n && src[i]; i++) dst[i] = src[i];
    for (; i < n; i++) dst[i] = '\0';
    return dst;
}

char *strchr(const char *s, int c) {
    for (; *s; s++) if (*s == (char)c) return (char *)s;
    return (c == 0) ? (char *)s : NULL;
}

/* NOT dead code, however it looks from a default build. mbedTLS's PEM decoder
 * (mbedtls_pem_read_buffer) and mbedtls_x509_crt_parse call strstr to find the
 * "-----BEGIN ..." delimiters, and include/mbedtls_config.h only defines
 * MBEDTLS_PEM_PARSE_C under TALKOS_VERIFY_CERTS. So `nm -u` over a default
 * build's objects reports no caller, deleting this looks safe, every
 * translation unit still compiles because port/string.h declares it — and
 * `make EXTRA_CFLAGS=-DTALKOS_VERIFY_CERTS` fails at LINK time with three
 * undefined references. That happened. The documented security-hardening build
 * is the one thing an unbounded-by-construction primitive earns its place for;
 * if it is ever removed again, remove MBEDTLS_PEM_PARSE_C's users first. */
char *strstr(const char *haystack, const char *needle) {
    if (!*needle) return (char *)haystack;
    for (; *haystack; haystack++) {
        const char *h = haystack, *n = needle;
        while (*h && *n && *h == *n) { h++; n++; }
        if (!*n) return (char *)haystack;
    }
    return NULL;
}

/* ---- stdlib ----
 * mbedTLS reaches the kernel heap through calloc/free only; malloc and realloc
 * are here because port/stdlib.h declares the whole quartet for vendored code,
 * and a heap shim that offers three quarters of it is the kind of asymmetry
 * that costs someone an afternoon. They have no caller in the current build. */
void *malloc(size_t n)            { return kmalloc(n); }
void  free(void *p)               { kfree(p); }
void *calloc(size_t n, size_t sz) { return kcalloc(n, sz); }
void *realloc(void *p, size_t n)  { return krealloc(p, n); }
int   abs(int x)                  { return x < 0 ? -x : x; }

long strtol(const char *s, char **end, int base) {
    while (*s == ' ' || *s == '\t') s++;
    int neg = 0;
    if (*s == '+' || *s == '-') { neg = (*s == '-'); s++; }
    if ((base == 16 || base == 0) && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) { s += 2; base = 16; }
    if (base == 0) base = 10;
    long v = 0;
    for (;;) {
        int d;
        if (*s >= '0' && *s <= '9') d = *s - '0';
        else if (*s >= 'a' && *s <= 'z') d = *s - 'a' + 10;
        else if (*s >= 'A' && *s <= 'Z') d = *s - 'A' + 10;
        else break;
        if (d >= base) break;
        v = v * base + d;
        s++;
    }
    if (end) *end = (char *)s;
    return neg ? -v : v;
}

int atoi(const char *s) { return (int)strtol(s, NULL, 10); }

/* ---- time (seconds since boot; mbedTLS session timestamps only) ---- */
long time(long *t) {
    long s = (long)(millis() / 1000);
    if (t) *t = s;
    return s;
}

/* ---- lwIP port hooks ---- */
unsigned int sys_now(void) { return (unsigned int)millis(); }

/* Cheap LCG seeded once from the TSC. Good enough for ISN/txid entropy. */
int rand(void) {
    static uint32_t seed;
    if (seed == 0) seed = (uint32_t)rdtsc() | 1;
    seed = seed * 1103515245u + 12345u;
    return (int)((seed >> 16) & 0x7FFF);
}

unsigned int lwip_rand(void) {
    return ((unsigned int)rand() << 17) ^ ((unsigned int)rand() << 9) ^ (unsigned int)rand();
}

/* ====================================================================== */
/* snprintf / vsnprintf — small, bounded formatter for mbedTLS             */
/* Supports: flags '-' '0', width, '.'precision, length l/ll/z, and the    */
/* conversions d i u x X p c s %. Returns the C99 "would-have-written" len. */
/* ====================================================================== */

struct sbuf { char *p; size_t cap; size_t len; };

static void sb_putc(struct sbuf *s, char c) {
    if (s->len + 1 < s->cap) s->p[s->len] = c;
    s->len++;
}

static void sb_pad(struct sbuf *s, int n, char pad) {
    while (n-- > 0) sb_putc(s, pad);
}

static void emit_uint(struct sbuf *s, uint64_t v, int base, int upper,
                      int width, int prec, int left, char padc) {
    char tmp[32];
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    int n = 0;
    /* `prec` comes from the format string and is accumulated unbounded by the
     * parser, so it must be clamped before it is used as a write count: "%.40d"
     * would otherwise run the zero-fill below straight off the end of tmp and
     * corrupt the caller's stack frame. 32 is past every representable value
     * (20 digits for a base-10 uint64, 16 for base-16), so no legitimate
     * conversion is affected by the clamp. */
    if (prec > (int)sizeof tmp) prec = (int)sizeof tmp;
    if (v == 0 && prec != 0) tmp[n++] = '0';
    while (v) { tmp[n++] = digits[v % base]; v /= base; }
    while (n < prec) tmp[n++] = '0';                 /* precision = min digits */
    int pad = width - n;
    if (!left) sb_pad(s, pad, padc);
    while (n--) sb_putc(s, tmp[n]);
    if (left) sb_pad(s, pad, ' ');
}

int vsnprintf(char *str, size_t size, const char *fmt, va_list ap) {
    struct sbuf s = { str, size, 0 };
    for (; *fmt; fmt++) {
        if (*fmt != '%') { sb_putc(&s, *fmt); continue; }
        fmt++;
        /* A format ending in a bare '%' leaves fmt on the terminator. Without
         * this the switch below falls to `default`, the loop's own fmt++ steps
         * PAST the NUL, and formatting continues into whatever .rodata follows —
         * consuming varargs that were never passed. */
        if (!*fmt) { sb_putc(&s, '%'); break; }
        int left = 0, zero = 0;
        for (;; fmt++) {                              /* flags */
            if (*fmt == '-') left = 1;
            else if (*fmt == '0') zero = 1;
            else break;
        }
        int width = 0;
        while (*fmt >= '0' && *fmt <= '9') width = width * 10 + (*fmt++ - '0');
        int prec = -1;
        if (*fmt == '.') { fmt++; prec = 0; while (*fmt >= '0' && *fmt <= '9') prec = prec * 10 + (*fmt++ - '0'); }
        int lng = 0;
        while (*fmt == 'l') { lng++; fmt++; }
        if (*fmt == 'z') { lng = 2; fmt++; }
        char padc = (zero && !left) ? '0' : ' ';

        switch (*fmt) {
        case 'd': case 'i': {
            int64_t v = (lng >= 1) ? va_arg(ap, long) : va_arg(ap, int);
            if (v < 0) { sb_putc(&s, '-'); emit_uint(&s, (uint64_t)(-v), 10, 0, width ? width - 1 : 0, prec, left, padc); }
            else emit_uint(&s, (uint64_t)v, 10, 0, width, prec, left, padc);
            break;
        }
        case 'u': emit_uint(&s, (lng >= 1) ? va_arg(ap, unsigned long) : va_arg(ap, unsigned), 10, 0, width, prec, left, padc); break;
        case 'x': emit_uint(&s, (lng >= 1) ? va_arg(ap, unsigned long) : va_arg(ap, unsigned), 16, 0, width, prec, left, padc); break;
        case 'X': emit_uint(&s, (lng >= 1) ? va_arg(ap, unsigned long) : va_arg(ap, unsigned), 16, 1, width, prec, left, padc); break;
        case 'p': sb_putc(&s, '0'); sb_putc(&s, 'x'); emit_uint(&s, (uint64_t)(uintptr_t)va_arg(ap, void *), 16, 0, 0, -1, 0, ' '); break;
        case 'c': sb_putc(&s, (char)va_arg(ap, int)); break;
        case 's': {
            const char *str2 = va_arg(ap, const char *);
            if (!str2) str2 = "(null)";
            int n = 0; while (str2[n] && (prec < 0 || n < prec)) n++;
            if (!left) sb_pad(&s, width - n, ' ');
            for (int i = 0; i < n; i++) sb_putc(&s, str2[i]);
            if (left) sb_pad(&s, width - n, ' ');
            break;
        }
        case '%': sb_putc(&s, '%'); break;
        default: sb_putc(&s, '%'); if (*fmt) sb_putc(&s, *fmt); break;
        }
    }
    if (s.cap) s.p[s.len < s.cap ? s.len : s.cap - 1] = '\0';
    return (int)s.len;
}

int snprintf(char *str, size_t size, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(str, size, fmt, ap);
    va_end(ap);
    return n;
}

/* ====================================================================== */
/* Entropy for TLS — mbedTLS calls this to seed CTR_DRBG.                   */
/* Uses RDRAND when the CPU advertises it, else mixes the TSC. NOTE: this   */
/* is functional, not a vetted CSPRNG — fine for a hobby OS, not for real   */
/* secrets. mbedtls_hardware_poll is enabled via MBEDTLS_ENTROPY_HARDWARE_ALT. */
/* ====================================================================== */

static int have_rdrand(void) {
    uint32_t eax, ebx, ecx, edx;
    eax = 1;
    __asm__ volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(eax));
    return (ecx >> 30) & 1;
}

/* The weak fallback: an LCG over the TSC. Not a CSPRNG, but never
 * uninitialised and never a repeated constant, which is the property that
 * matters when it is the last resort for seeding CTR_DRBG. */
static uint64_t tsc_mix(void) {
    static uint64_t st;
    st = (st ^ rdtsc()) * 6364136223846793005ull + 1442695040888963407ull;
    return st ^ (st >> 31) ^ ((uint64_t)rand() << 32);
}

static uint64_t rdrand64(void) {
    uint64_t v; unsigned char ok;
    for (int tries = 0; tries < 32; tries++) {
        __asm__ volatile("rdrand %0; setc %1" : "=r"(v), "=qm"(ok));
        if (ok) return v;
    }
    /* CPUID advertised RDRAND but the instruction kept declining — a documented
     * hardware/hypervisor state. Falling back is not optional: returning `v`
     * here would return an UNINITIALISED local and mbedtls_hardware_poll below
     * would still report success, seeding TLS from stack garbage. */
    return tsc_mix();
}

int mbedtls_hardware_poll(void *data, unsigned char *output, size_t len, size_t *olen) {
    (void)data;
    static int rdrand = -1;
    if (rdrand < 0) rdrand = have_rdrand();
    size_t i = 0;
    while (i < len) {
        uint64_t r = rdrand ? rdrand64() : tsc_mix();
        size_t n = (len - i < sizeof r) ? (len - i) : sizeof r;
        memcpy(output + i, &r, n);
        i += n;
    }
    *olen = len;
    return 0;
}
