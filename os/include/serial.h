/* serial.h — COM1 (16550 UART) debug console. */
#ifndef SERIAL_H
#define SERIAL_H

void serial_putc(char c);
void serial_write(const char *s);

#endif /* SERIAL_H */
