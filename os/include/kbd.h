/* kbd.h — PS/2 keyboard (polled). */
#ifndef KBD_H
#define KBD_H

/* Block until a key is pressed, returning its ASCII value. Enter is '\n',
 * Backspace is '\b'. Keys with no ASCII mapping are skipped. */
char kbd_getchar(void);

/* Read a line into buf (NUL-terminated, at most cap-1 chars), echoing as you
 * type and handling Backspace. Returns the length, not counting the NUL.
 * The terminating Enter is consumed but not stored. */
int kbd_readline(char *buf, int cap);

#endif /* KBD_H */
