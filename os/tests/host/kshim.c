/* kshim.c — host implementations of the kernel services in kernel.h.
 *
 * This is what lets real kernel source files (mm/heap.c, and the JSON/trace/VM
 * modules to come) link and run in a normal host process. Console output is
 * captured into a buffer instead of a VGA framebuffer, and panic() is recorded
 * rather than halting, so a test can assert that bad input panics.
 *
 * memcpy/memset/memmove/memcmp/strlen are intentionally NOT defined here — the
 * host libc already provides them, and kernel.h's declarations are compatible.
 */

#include "harness.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

/* ---- captured console ---- */

#define KCAP_CAP (1 << 20)
static char   kcap_buf[KCAP_CAP];
static size_t kcap_len = 0;

void kcap_reset(void) {
    kcap_len = 0;
    kcap_buf[0] = '\0';
}

const char *kcap_text(void) {
    kcap_buf[kcap_len] = '\0';
    return kcap_buf;
}

static void kcap_push(const char *s, size_t n) {
    if (kcap_len + n >= KCAP_CAP) n = KCAP_CAP - kcap_len - 1;
    memcpy(kcap_buf + kcap_len, s, n);
    kcap_len += n;
}

/* ---- kernel.h console surface ---- */

void console_init(void) { kcap_reset(); }

void kputc(char c) { kcap_push(&c, 1); }

void kputs(const char *s) { kcap_push(s, strlen(s)); }

/* The kernel's kprintf supports %s %c %d %u %x %p %%. The host vsnprintf is a
 * superset, so delegating is safe and keeps the shim tiny. */
void kprintf(const char *fmt, ...) {
    char    tmp[8192];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    if (n > 0) kcap_push(tmp, (size_t)n > sizeof tmp - 1 ? sizeof tmp - 1 : (size_t)n);
}

/* ---- panic: record, don't die ---- */

int         kpanic_hit = 0;
const char *kpanic_msg = NULL;

void panic(const char *msg) {
    kpanic_hit = 1;
    kpanic_msg = msg;
    printf("    [panic captured: %s]\n", msg ? msg : "(null)");
}

/* ---- time ---- */

void time_init(void) {}

uint64_t millis(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000);
}

void mdelay(uint32_t ms) { (void)ms; }
