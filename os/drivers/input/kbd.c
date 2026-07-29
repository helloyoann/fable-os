/* kbd.c — PS/2 keyboard driver (i8042, polled, scancode set 1).
 *
 * We don't use interrupts: the terminal is a simple read-a-line-then-act loop,
 * so busy-polling the controller's status/data ports is enough and keeps the
 * kernel free of an IDT. Make codes (key down) are < 0x80; break codes (key up)
 * have bit 7 set. We track Shift for the upper-case / symbol layer and ignore
 * everything else (Ctrl, Alt, function keys, the keypad).
 *
 * This driver's only job is scancode -> ASCII. Line assembly (echo, backspace,
 * line endings, overflow) used to live here; it now lives in drivers/input/
 * input.c behind input_source_t, shared with the serial and scripted sources.
 * The keyboard just registers itself as one more source. */

#include "kbd.h"
#include "input.h"
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

/* Drain the controller until it yields a printable/edit character, or run out
 * of pending bytes. Never blocks — this is the input_source_t contract. */
int kbd_getchar_nb(void) {
    while (inb(PS2_STATUS) & PS2_OBF) {
        uint8_t sc = inb(PS2_DATA);

        if (sc & 0x80) {                       /* break (key release) */
            uint8_t make = sc & 0x7F;
            if (make == 0x2A || make == 0x36) shift_down = 0;
            continue;
        }
        if (sc == 0x2A || sc == 0x36) { shift_down = 1; continue; }
        if (sc >= 0x40) continue;              /* outside our table */

        char c = shift_down ? map_upper[sc] : map_lower[sc];
        if (c) return (int)(unsigned char)c;
    }
    return -1;
}

char kbd_getchar(void) {
    for (;;) {
        int c = kbd_getchar_nb();
        if (c >= 0) return (char)c;
        __asm__ volatile("pause");
    }
}

/* Keyboard-only line read. The general path is input_readline(), which reads
 * from whichever source has input; this stays for a caller that specifically
 * wants the local console and nothing else. Same editor, same behaviour. */
int kbd_readline(char *buf, int cap) {
    input_line_t ed;
    input_line_init(&ed, buf, cap, 1 /* echo */);
    while (!input_line_push(&ed, kbd_getchar())) { }
    return input_line_take(&ed, buf, cap);
}

/* ---- input_source_t adapter ---- */

static int kbd_src_getc(input_source_t *self) {
    (void)self;
    int c = kbd_getchar_nb();
    return (c < 0) ? INPUT_NONE : c;
}

static input_source_t kbd_src = {
    .name    = "kbd",
    .getc_nb = kbd_src_getc,
    .priv    = NULL,
    .flags   = INPUT_ECHO,
};

static void kbd_drain(void) {
    while (inb(PS2_STATUS) & PS2_OBF) (void)inb(PS2_DATA);
}

static const driver_t kbd_driver;

static int kbd_init(void) {
    kbd_drain();                       /* clear any stale byte */
    input_register_source(&kbd_src);
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
