/* stdlib.h — freestanding shim for lwIP. */
#ifndef PORT_STDLIB_H
#define PORT_STDLIB_H
#include <stddef.h>

void *malloc(size_t n);
void  free(void *p);
void *calloc(size_t n, size_t sz);
void *realloc(void *p, size_t n);
int   atoi(const char *s);
long  strtol(const char *s, char **end, int base);
int   abs(int x);
int   rand(void);

#endif /* PORT_STDLIB_H */
