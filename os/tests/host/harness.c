/* harness.c — counters and the pass/fail report for the host test harness. */

#include "harness.h"

int th_checks = 0;
int th_fails  = 0;

int th_report(const char *suite) {
    if (th_fails == 0) {
        printf("  PASS %s (%d checks)\n", suite, th_checks);
        return 0;
    }
    printf("  FAIL %s (%d/%d checks failed)\n", suite, th_fails, th_checks);
    return 1;
}
