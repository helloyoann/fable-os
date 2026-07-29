/* base.c — kernel runtime: VGA console, string/mem helpers, bump allocator,
 * PIT-calibrated millisecond clock, and panic. */

#include "kernel.h"
#include "io.h"
#include <stdarg.h>

/* ====================================================================== */
/* String / memory                                                        */
/* ====================================================================== */

void *memcpy(void *dst, const void *src, size_t n) {
    uint8_t *d = dst;
    const uint8_t *s = src;
    while (n--) *d++ = *s++;
    return dst;
}

void *memset(void *dst, int c, size_t n) {
    uint8_t *d = dst;
    while (n--) *d++ = (uint8_t)c;
    return dst;
}

void *memmove(void *dst, const void *src, size_t n) {
    uint8_t *d = dst;
    const uint8_t *s = src;
    if (d < s) {
        while (n--) *d++ = *s++;
    } else {
        d += n; s += n;
        while (n--) *--d = *--s;
    }
    return dst;
}

int memcmp(const void *a, const void *b, size_t n) {
    const uint8_t *x = a, *y = b;
    while (n--) {
        if (*x != *y) return (int)*x - (int)*y;
        x++; y++;
    }
    return 0;
}

size_t strlen(const char *s) {
    size_t n = 0;
    while (s[n]) n++;
    return n;
}

/* ====================================================================== */
/* VGA text console (80x25, scrolling)                                    */
/* ====================================================================== */

#define VGA ((volatile uint16_t *)0xB8000)
#define VGA_W 80
#define VGA_H 25
#define ATTR 0x07   /* light grey on black */

static int cur_row, cur_col;

/* The serial driver overrides this; until it loads (or if absent) it's a
 * no-op. Lets kprintf reach the serial log once we leave VGA text mode. */
__attribute__((weak)) void serial_emit(char c) { (void)c; }

void console_init(void) {
    for (int i = 0; i < VGA_W * VGA_H; i++) VGA[i] = ((uint16_t)ATTR << 8) | ' ';
    cur_row = cur_col = 0;
}

static void scroll(void) {
    for (int i = 0; i < VGA_W * (VGA_H - 1); i++) VGA[i] = VGA[i + VGA_W];
    for (int i = 0; i < VGA_W; i++)
        VGA[VGA_W * (VGA_H - 1) + i] = ((uint16_t)ATTR << 8) | ' ';
    cur_row = VGA_H - 1;
}

/* Move the blinking hardware cursor to (cur_row, cur_col) via the VGA CRTC. */
static void update_cursor(void) {
    uint16_t pos = (uint16_t)(cur_row * VGA_W + cur_col);
    outb(0x3D4, 0x0F); outb(0x3D5, (uint8_t)(pos & 0xFF));
    outb(0x3D4, 0x0E); outb(0x3D5, (uint8_t)(pos >> 8));
}

void kputc(char c) {
    serial_emit(c);
    if (c == '\n') {
        cur_col = 0;
        if (++cur_row >= VGA_H) scroll();
        update_cursor();
        return;
    }
    if (c == '\r') { cur_col = 0; update_cursor(); return; }
    if (c == '\b') {                       /* backspace: step left, don't erase */
        if (cur_col > 0) cur_col--;
        else if (cur_row > 0) { cur_row--; cur_col = VGA_W - 1; }
        update_cursor();
        return;
    }
    if (c == '\t') { cur_col = (cur_col + 8) & ~7; }
    else {
        VGA[cur_row * VGA_W + cur_col] = ((uint16_t)ATTR << 8) | (uint8_t)c;
        cur_col++;
    }
    if (cur_col >= VGA_W) {
        cur_col = 0;
        if (++cur_row >= VGA_H) scroll();
    }
    update_cursor();
}

void kputs(const char *s) { while (*s) kputc(*s++); }

/* ---- direct access to the console's cells, for tools that paint ----
 * tools/screen_tools.c lets the model read the screen back and paint into it.
 * Rather than have it duplicate 0xB8000 and 80x25 (which would silently rot the
 * day this console changes), it asks here. The cursor accessor matters most: a
 * painted region is transient precisely because THIS cursor decides where the
 * next character lands and when scroll() fires, so a tool can only be honest
 * about that if it can see the real value. All three are read-only. */
volatile uint16_t *console_fb(void) { return VGA; }

void console_geometry(int *rows, int *cols) {
    if (rows) *rows = VGA_H;
    if (cols) *cols = VGA_W;
}

void console_cursor(int *row, int *col) {
    if (row) *row = cur_row;
    if (col) *col = cur_col;
}

static void put_uint(uint64_t v, int base, int width, char pad) {
    char buf[32];
    int i = 0;
    if (v == 0) buf[i++] = '0';
    while (v) {
        int d = v % base;
        buf[i++] = d < 10 ? '0' + d : 'a' + d - 10;
        v /= base;
    }
    while (i < width) buf[i++] = pad;
    while (i--) kputc(buf[i]);
}

void kprintf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    for (; *fmt; fmt++) {
        if (*fmt != '%') { kputc(*fmt); continue; }
        fmt++;
        int width = 0; char pad = ' ';
        if (*fmt == '0') { pad = '0'; fmt++; }
        while (*fmt >= '0' && *fmt <= '9') { width = width * 10 + (*fmt - '0'); fmt++; }
        switch (*fmt) {
            case 's': kputs(va_arg(ap, const char *)); break;
            case 'c': kputc((char)va_arg(ap, int)); break;
            case 'd': {
                int v = va_arg(ap, int);
                if (v < 0) { kputc('-'); put_uint((uint64_t)(-(int64_t)v), 10, 0, ' '); }
                else put_uint((uint64_t)v, 10, width, pad);
                break;
            }
            case 'u': put_uint(va_arg(ap, unsigned), 10, width, pad); break;
            case 'x': put_uint(va_arg(ap, unsigned), 16, width, pad); break;
            case 'p': kputs("0x"); put_uint((uint64_t)va_arg(ap, void *), 16, 0, ' '); break;
            case '%': kputc('%'); break;
            default: kputc('%'); kputc(*fmt); break;
        }
    }
    va_end(ap);
}

/* The kernel heap (kmalloc/kfree/krealloc) now lives in mm/heap.c. */

/* ====================================================================== */
/* Time — calibrate the TSC against PIT channel 2                          */
/* ====================================================================== */

static uint64_t tsc_per_ms;
static uint64_t tsc_base;

void time_init(void) {
    /* Use PIT channel 2 in mode 0 to time a known interval against the TSC.
     * The PIT input clock is 1193182 Hz. Count 11932 ticks ≈ 10 ms. */
    uint8_t p = (inb(0x61) & 0xFC) | 0x01;   /* speaker off, gate on */
    outb(0x61, p);
    outb(0x43, 0xB0);                        /* ch2, lo/hi byte, mode 0, binary */
    uint16_t count = 11932;
    outb(0x42, count & 0xFF);
    outb(0x42, count >> 8);

    /* Restart the count by toggling the gate. */
    p = inb(0x61) & 0xFE;
    outb(0x61, p);
    outb(0x61, p | 0x01);

    uint64_t start = rdtsc();
    while (!(inb(0x61) & 0x20)) { }          /* wait for OUT to go high */
    uint64_t end = rdtsc();

    tsc_per_ms = (end - start) / 10;
    if (tsc_per_ms == 0) tsc_per_ms = 1;     /* avoid divide-by-zero */
    tsc_base = rdtsc();
}

uint64_t millis(void) {
    return (rdtsc() - tsc_base) / tsc_per_ms;
}

void mdelay(uint32_t ms) {
    uint64_t target = millis() + ms;
    while (millis() < target) { }
}

/* ====================================================================== */
/* Panic                                                                   */
/* ====================================================================== */

void panic(const char *msg) {
    kputs("\n*** PANIC: ");
    kputs(msg);
    kputs(" ***\n");
    for (;;) __asm__ volatile("cli; hlt");
}
