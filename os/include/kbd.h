/* kbd.h — PS/2 keyboard (polled), scancode -> ASCII.
 *
 * The keyboard registers itself as an input source at bring-up, so the normal
 * way to read a line is input_readline() in input.h — it works whether the
 * characters came from here, from the serial line, or from a test script. The
 * functions below are the keyboard-specific path, kept for callers that
 * deliberately want the local console only. */
#ifndef KBD_H
#define KBD_H

/* Next key as ASCII, or -1 if none is waiting. Never blocks. Enter is '\n',
 * Backspace is '\b'. Keys with no ASCII mapping are skipped. */
int kbd_getchar_nb(void);

/* Block until a key is pressed, returning its ASCII value. */
char kbd_getchar(void);

/* Read a line into buf (NUL-terminated, at most cap-1 chars), echoing as you
 * type and handling Backspace. Returns the length, not counting the NUL.
 * The terminating Enter is consumed but not stored. Keyboard only — prefer
 * input_readline(). */
int kbd_readline(char *buf, int cap);

#endif /* KBD_H */
