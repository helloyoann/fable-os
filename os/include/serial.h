/* serial.h — COM1 (16550 UART) debug console and serial input line.
 *
 * Output is the kernel's debug console (base.c's kprintf lands here). Input is
 * the headless way into the machine: with `qemu ... -serial stdio` a test script
 * can pipe a natural-language question straight into the kernel, which is
 * impossible with the PS/2 keyboard. Receive is polled and non-blocking, like
 * everything else here — this kernel has no IDT, so no IRQ 4 handler exists.
 * The receive FIFO is left disabled on purpose (see serial_init) so the far end
 * hands us one byte at a time and nothing is lost while the kernel is busy. The
 * line editing on top of these bytes lives in input.c; see include/input.h.
 */
#ifndef SERIAL_H
#define SERIAL_H

void serial_putc(char c);
void serial_write(const char *s);

/* Nonzero when at least one received byte is waiting in the UART. */
int serial_rx_ready(void);

/* Next received byte (0..255), or -1 if none is waiting. Never blocks. */
int serial_getc_nb(void);

#endif /* SERIAL_H */
