/* kernel.h — shared kernel services: console, strings, memory, time. */
#ifndef KERNEL_H
#define KERNEL_H

#include <stdint.h>
#include <stddef.h>

/* ---- VGA text console ---- */
void console_init(void);
void kputc(char c);
void kputs(const char *s);
void kprintf(const char *fmt, ...);   /* supports %s %c %d %u %x %p %% */

/* Read-only view of the console's own cells, geometry and cursor. Exists so a
 * tool that paints the framebuffer (tools/screen_tools.c) shares one definition
 * of where the screen is and where output goes next, instead of duplicating
 * 0xB8000 and 80x25. Writing through console_fb() does not move the cursor. */
volatile uint16_t *console_fb(void);
void console_geometry(int *rows, int *cols);
void console_cursor(int *row, int *col);

/* ---- Freestanding string/memory ---- */
void  *memcpy(void *dst, const void *src, size_t n);
void  *memset(void *dst, int c, size_t n);
void  *memmove(void *dst, const void *src, size_t n);
int    memcmp(const void *a, const void *b, size_t n);
size_t strlen(const char *s);

/* ---- Kernel heap (kmalloc/kfree/krealloc; see heap.h) ---- */
#include "heap.h"

/* ---- Time (calibrated against the PIT) ---- */
void     time_init(void);
uint64_t millis(void);     /* milliseconds since time_init() */
void     mdelay(uint32_t ms);

/* ---- Panic ---- */
void panic(const char *msg);

#endif /* KERNEL_H */
