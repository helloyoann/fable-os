/* harness.h — minimal host-native unit test harness for talk-os.
 *
 * Kernel modules that are pure logic (heap, JSON parsing, trace formatting, the
 * driver VM) are compiled against the HOST compiler and run natively, so the
 * inner development loop is milliseconds instead of a QEMU boot. Hardware-
 * dependent behaviour belongs in tests/qemu/ instead.
 *
 * Usage:
 *     #include "harness.h"
 *     static void test_thing(void) { CHECK(1 + 1 == 2); }
 *     int main(void) { RUN(test_thing); return th_report("thing"); }
 *
 * Exit code is 0 when every check passed, 1 otherwise, so `make test-host`
 * fails loudly in CI and for any agent iterating on a change.
 */
#ifndef HARNESS_H
#define HARNESS_H

#include <stdio.h>
#include <stdint.h>
#include <string.h>

extern int th_checks;
extern int th_fails;

/* Report a failure without aborting: one run surfaces every broken check. */
#define CHECK(cond)                                                           \
    do {                                                                      \
        th_checks++;                                                          \
        if (!(cond)) {                                                        \
            th_fails++;                                                       \
            printf("    FAIL %s:%d  CHECK(%s)\n", __FILE__, __LINE__, #cond); \
        }                                                                     \
    } while (0)

#define CHECK_EQ(got, want)                                                   \
    do {                                                                      \
        th_checks++;                                                          \
        long long g_ = (long long)(got), w_ = (long long)(want);               \
        if (g_ != w_) {                                                       \
            th_fails++;                                                       \
            printf("    FAIL %s:%d  %s == %s (got %lld, want %lld)\n",         \
                   __FILE__, __LINE__, #got, #want, g_, w_);                  \
        }                                                                     \
    } while (0)

#define CHECK_STR(got, want)                                                  \
    do {                                                                      \
        th_checks++;                                                          \
        const char *g_ = (got), *w_ = (want);                                 \
        if (!g_ || !w_ || strcmp(g_, w_) != 0) {                              \
            th_fails++;                                                       \
            printf("    FAIL %s:%d  %s == \"%s\" (got \"%s\")\n",             \
                   __FILE__, __LINE__, #got, w_ ? w_ : "(null)",              \
                   g_ ? g_ : "(null)");                                       \
        }                                                                     \
    } while (0)

/* Assert a substring appears — the workhorse for trace lines and JSON. */
#define CHECK_CONTAINS(haystack, needle)                                      \
    do {                                                                      \
        th_checks++;                                                          \
        const char *h_ = (haystack), *n_ = (needle);                          \
        if (!h_ || !n_ || strstr(h_, n_) == NULL) {                           \
            th_fails++;                                                       \
            printf("    FAIL %s:%d  %s contains \"%s\"\n  in: %s\n",          \
                   __FILE__, __LINE__, #haystack, n_ ? n_ : "(null)",         \
                   h_ ? h_ : "(null)");                                       \
        }                                                                     \
    } while (0)

#define RUN(fn)                                                               \
    do {                                                                      \
        printf("  - %s\n", #fn);                                              \
        fn();                                                                 \
    } while (0)

int th_report(const char *suite);

/* Provided by kshim.c: the kernel.h console surface, host-side. Declared here so
 * a test only needs harness.h plus the header of the module under test. */
void console_init(void);
void kputc(char c);
void kputs(const char *s);
void kprintf(const char *fmt, ...);

/* ---- host-side capture of kernel console output ----
 * kshim.c routes kprintf/kputs/kputc into a buffer so tests can assert on what
 * the kernel *printed* — which is exactly how kernel-emitted trace lines get
 * verified without booting. */
void        kcap_reset(void);
const char *kcap_text(void);

/* Set when the code under test calls panic(); lets a test assert that a bad
 * input panics without killing the test process. */
extern int         kpanic_hit;
extern const char *kpanic_msg;

#endif /* HARNESS_H */
