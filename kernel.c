/*
 * kernel.c — the C half of the kernel.
 *
 * By the time kernel_main runs we are in 64-bit long mode with a flat memory
 * model, so we can write straight to the VGA text buffer at 0xB8000. Each cell
 * is two bytes: the low byte is the ASCII character, the high byte is the
 * colour attribute (foreground in the low nibble, background in the high).
 */

#include <stdint.h>

#define VGA_BUFFER ((volatile uint16_t *)0xB8000)
#define VGA_WIDTH  80
#define VGA_HEIGHT 25

/* light grey on black */
#define COLOR_DEFAULT 0x07
/* white on black */
#define COLOR_BRIGHT  0x0F

static uint16_t vga_entry(char c, uint8_t color) {
    return (uint16_t)c | ((uint16_t)color << 8);
}

static void clear_screen(void) {
    for (int i = 0; i < VGA_WIDTH * VGA_HEIGHT; i++) {
        VGA_BUFFER[i] = vga_entry(' ', COLOR_DEFAULT);
    }
}

static void print_at(const char *str, int row, int col, uint8_t color) {
    volatile uint16_t *cell = VGA_BUFFER + row * VGA_WIDTH + col;
    while (*str) {
        *cell++ = vga_entry(*str++, color);
    }
}

void kernel_main(void) {
    clear_screen();
    print_at("Hello, World!", 0, 0, COLOR_BRIGHT);
    print_at("Running in 64-bit long mode on x86_64.", 1, 0, COLOR_DEFAULT);

    /* Nothing left to do — idle forever. */
    for (;;) {
        __asm__ volatile("hlt");
    }
}
