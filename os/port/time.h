/* time.h — freestanding shim. The kernel has no wall clock, and mbedTLS is
 * built with MBEDTLS_HAVE_TIME off, so these exist only to satisfy stray
 * includes; they are not used for certificate date validation. */
#ifndef PORT_TIME_H
#define PORT_TIME_H
#include <stddef.h>

typedef long time_t;

struct tm {
    int tm_sec, tm_min, tm_hour, tm_mday, tm_mon, tm_year,
        tm_wday, tm_yday, tm_isdst;
};

/* Seconds since boot — enough for mbedTLS session timestamps; not a real
 * calendar clock (certificate date validation is disabled). */
time_t time(time_t *t);

#endif /* PORT_TIME_H */
