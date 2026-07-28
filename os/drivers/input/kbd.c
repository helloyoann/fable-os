/* kbd.c — PS/2 keyboard driver (i8042, polled, scancode set 1).
 *
 * We don't use interrupts: the terminal is a simple read-a-line-then-act loop,
 * so busy-polling the controller's status/data ports is enough and keeps the
 * kernel free of an IDT. Make codes (key down) are < 0x80; break codes (key up)
 * have bit 7 set. We track Shift for the upper-case / symbol layer and ignore
 * everything else (Ctrl, Alt, function keys, the keypad). */

#include "kbd.h"
#include "driver.h"
#include "device.h"
#include "kernel.h"
#include "io.h"

#define PS2_DATA   0x60
#define PS2_STATUS 0x64
#define PS2_OBF    0x01            /* output buffer full -> a byte is ready */

/* US QWERTY, scancode set 1, indexed by make code (0x00..0x39). 0 == no ASCII. */
static const char map_lower[0x40] = {
    0,    0x1B, '1',  '2',  '3',  '4',  '5',  '6',   /* 00-07 */
    '7',  '8',  '9',  '0',  '-',  '=',  '\b', '\t',  /* 08-0F */
    'q',  'w',  'e',  'r',  't',  'y',  'u',  'i',    /* 10-17 */
    'o',  'p',  '[',  ']',  '\n', 0,    'a',  's',    /* 18-1F (1D = Ctrl) */
    'd',  'f',  'g',  'h',  'j',  'k',  'l',  ';',    /* 20-27 */
    '\'', '`',  0,    '\\', 'z',  'x',  'c',  'v',    /* 28-2F (2A = LShift) */
    'b',  'n',  'm',  ',',  '.',  '/',  0,    '*',    /* 30-37 (36 = RShift) */
    0,    ' ',  0,    0,    0,    0,    0,    0,       /* 38-3F (38 = Alt) */
};

static const char map_upper[0x40] = {
    0,    0x1B, '!',  '@',  '#',  '$',  '%',  '^',
    '&',  '*',  '(',  ')',  '_',  '+',  '\b', '\t',
    'Q',  'W',  'E',  'R',  'T',  'Y',  'U',  'I',
    'O',  'P',  '{',  '}',  '\n', 0,    'A',  'S',
    'D',  'F',  'G',  'H',  'J',  'K',  'L',  ':',
    '"',  '~',  0,    '|',  'Z',  'X',  'C',  'V',
    'B',  'N',  'M',  '<',  '>',  '?',  0,    '*',
    0,    ' ',  0,    0,    0,    0,    0,    0,
};

static int shift_down;

char kbd_getchar(void) {
    for (;;) {
        if (!(inb(PS2_STATUS) & PS2_OBF)) {
            __asm__ volatile("pause");
            continue;
        }
        uint8_t sc = inb(PS2_DATA);

        if (sc & 0x80) {                       /* break (key release) */
            uint8_t make = sc & 0x7F;
            if (make == 0x2A || make == 0x36) shift_down = 0;
            continue;
        }
        if (sc == 0x2A || sc == 0x36) { shift_down = 1; continue; }
        if (sc >= 0x40) continue;              /* outside our table */

        char c = shift_down ? map_upper[sc] : map_lower[sc];
        if (c) return c;
    }
}

int kbd_readline(char *buf, int cap) {
    int len = 0;
    for (;;) {
        char c = kbd_getchar();
        if (c == '\n') { kputc('\n'); break; }
        if (c == '\b') {
            if (len > 0) { len--; kputs("\b \b"); }   /* erase last char */
            continue;
        }
        if (len < cap - 1) {
            buf[len++] = c;
            kputc(c);                          /* echo */
        }
    }
    buf[len] = '\0';
    return len;
}

static void kbd_drain(void) {
    while (inb(PS2_STATUS) & PS2_OBF) (void)inb(PS2_DATA);
}

static const driver_t kbd_driver;

static int kbd_init(void) {
    kbd_drain();                       /* clear any stale byte */
    kprintf("kbd: PS/2 keyboard ready\n");

    device_t *d = device_create("kbd0", DEV_CLASS_INPUT);
    device_add_resource(d, RES_IOPORT, PS2_DATA, 1);
    device_add_resource(d, RES_IRQ, 1, 1);
    device_register(d);
    device_bind_driver(d, (driver_t *)&kbd_driver);
    device_set_state(d, DEV_STATE_ACTIVE);
    return 0;
}

/* Power management: nothing to persist; on resume just clear stale input so a
 * key held across the transition doesn't leak in. Demonstrates the lifecycle. */
static int kbd_suspend(device_t *d) { (void)d; shift_down = 0; return 0; }
static int kbd_resume(device_t *d)  { (void)d; kbd_drain(); return 0; }

static const driver_t kbd_driver = {
    .name = "kbd",
    .level = DRV_LEVEL_DEVICE,
    .cls = DEV_CLASS_INPUT,
    .init = kbd_init,
    .suspend = kbd_suspend,
    .resume = kbd_resume,
};
REGISTER_DRIVER(kbd_driver);
