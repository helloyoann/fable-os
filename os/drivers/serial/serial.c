/* serial.c — COM1 16550 UART, used as the kernel debug console.
 *
 * Once we switch the display into a graphics framebuffer the VGA text buffer is
 * no longer visible, so kprintf output is routed here (base.c calls the weak
 * serial_emit, which this driver overrides). QEMU forwards COM1 to a file/stdio
 * with -serial, giving us a verifiable boot log. */

#include "serial.h"
#include "driver.h"
#include "io.h"

#define COM1 0x3F8

void serial_putc(char c) {
    while (!(inb(COM1 + 5) & 0x20)) { }   /* wait for THR empty */
    if (c == '\n') {                      /* CRLF for terminals */
        outb(COM1, '\r');
        while (!(inb(COM1 + 5) & 0x20)) { }
    }
    outb(COM1, (uint8_t)c);
}

void serial_write(const char *s) {
    while (*s) serial_putc(*s++);
}

/* Override base.c's weak no-op so kprintf reaches the serial line. */
void serial_emit(char c) {
    serial_putc(c);
}

static int serial_init(void) {
    outb(COM1 + 1, 0x00);   /* disable interrupts */
    outb(COM1 + 3, 0x80);   /* DLAB on */
    outb(COM1 + 0, 0x01);   /* divisor 1 => 115200 baud (lo) */
    outb(COM1 + 1, 0x00);   /* (hi) */
    outb(COM1 + 3, 0x03);   /* 8 bits, no parity, 1 stop; DLAB off */
    outb(COM1 + 2, 0xC7);   /* enable+clear FIFO, 14-byte threshold */
    outb(COM1 + 4, 0x0B);   /* DTR/RTS/OUT2 */
    serial_write("serial: COM1 up @ 115200\n");
    return 0;
}

static const driver_t serial_driver = {
    .name = "serial",
    .level = DRV_LEVEL_EARLY,
    .init = serial_init,
};
REGISTER_DRIVER(serial_driver);
