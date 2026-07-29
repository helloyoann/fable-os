/* app.h — the app runtime: the model writes a document, a real program appears.
 *
 * PURPOSE
 *   The operator types "hey I want a calculator" and a working calculator has to
 *   appear on screen. gui.h already provides windows, widgets, hit testing and an
 *   event loop, and gui/gui_demo.c proves a calculator can be built on top of them
 *   — in C, compiled in, by a human. This header is the part that removes the
 *   human: it defines WHAT AN APP IS when the model authors one at runtime, and
 *   the runtime (apps/runtime.c, apps/expr.c) that turns such a document into
 *   widgets, behaviour, and a window that can be clicked.
 *
 *   An app is a DOCUMENT, not code. It is parsed by net/json.c, validated in
 *   full before a single pixel is drawn, and executed by a total, bounded
 *   evaluator that cannot loop, cannot allocate, cannot fault and cannot reach
 *   anything but its own window.
 *
 * ============================================================================
 * DECISION 1 — WHY A DECLARATIVE DOCUMENT AND NOT CODE
 * ============================================================================
 *   Three candidates were real:
 *
 *     (a) THE MODEL EMITS C, OR MACHINE CODE. Rejected for the same reason
 *         dvm.h rejects it for drivers: there is no compiler on this machine, and
 *         the failure mode of a wrong jump on a single-threaded ring-0 kernel
 *         with all pages RWX is a triple fault with nothing to read afterwards.
 *
 *     (b) THE MODEL EMITS A PROGRAM FOR vm/dvm.c, the existing bounded VM. This
 *         was the strong candidate in the brief and it was examined closely,
 *         because reusing a sandbox is nearly always right. It was rejected on
 *         the VM's own contract, not on taste:
 *
 *           - dvm's opcodes ARE hardware primitives — in8/out8, MMIO loads,
 *             PCI config reads, delay. Not one of them can read or write a
 *             widget's text, which is the only thing an app's logic does. Using
 *             it would mean adding widget opcodes to vm/dvm.c, i.e. changing the
 *             ISA whose whole value is that it is small, allowlisted and already
 *             fuzzed against a synthetic device.
 *           - "No writable data memory" is a stated containment property of that
 *             VM. A calculator needs a display STRING and an accumulator. There
 *             is nowhere to put them.
 *           - Its values are unsigned 64-bit machine words with unsigned
 *             comparisons ("these are hardware values, not arithmetic"). A
 *             calculator needs signed fixed point, a decimal point, and text
 *             formatting.
 *           - Its trace is per-instruction device access. An app runs a handler
 *             on every click; that trace would be noise, and the kernel's
 *             ground-truth line for an app is "this widget was clicked and the
 *             display now reads 12".
 *
 *         Two sandboxes with different jobs is the honest outcome. dvm.h drives
 *         hardware the kernel has never seen; this drives a window. They share
 *         the design (bounded, allowlisted, refuse rather than guess, every
 *         failure legible) and not one line of code.
 *
 *     (c) A DECLARATIVE DOCUMENT plus a tiny expression language. Chosen.
 *         The whole document arrives through net/json.c, which is hardened,
 *         fuzzed, ASan-clean and already trusted with network data, so the
 *         attack surface facing model output is a parser that was attacked
 *         first. Layout is data (a grid), so it cannot be "wrong" in a way that
 *         corrupts anything — only in a way that looks bad. And the ONLY place
 *         real logic is needed — what a button does — is a small language with
 *         no loops, no functions, no memory and no I/O.
 *
 *   WHAT THE EXPRESSION LANGUAGE DELIBERATELY CANNOT DO. There is no loop and no
 *   recursion, so a handler's cost is bounded by its own length before it runs.
 *   There is no way to name a widget in another window, no way to open or close a
 *   window, no way to print to the console (which also means model-authored text
 *   can never put a '[' in column zero and forge a kernel trace line — see
 *   trace.h), no way to read the clock, the filesystem, memory or a device, and
 *   no way to call a tool. An app is a pure function from clicks to its own
 *   widgets' text. Everything else stays a syscall the model makes itself, where
 *   it is traced.
 *
 * ============================================================================
 * DECISION 2 — ERRORS ARE VALUES, SO EVALUATION IS TOTAL
 * ============================================================================
 *   A calculator must survive 1/0 and 999999999*999999999. The usual answers are
 *   to trap (the handler dies half-done, leaving the display holding a lie) or to
 *   wrap (the display holds a different lie). Instead there is a third value
 *   kind: ERR. Division by zero, overflow, num("abc") and arithmetic on a string
 *   all produce ERR; ERR propagates through every operator; text(ERR) is the word
 *   "error"; iserr(x) detects it. So no operation can fail, every handler runs to
 *   completion, and an app can latch a visible error state on purpose.
 *
 *   That is what makes the shipped calculator (apps/examples/calculator.json)
 *   able to say "error" and mean it, with no error handling in the runtime at
 *   all beyond a counter for the record.
 *
 * ============================================================================
 * DECISION 3 — VALIDATE EVERYTHING BEFORE ANY PIXEL
 * ============================================================================
 *   A document is compiled in full — every widget placed, every expression
 *   parsed, every name resolved, every bound checked — into a reserved instance
 *   slot, and only then is a window opened. A rejected document therefore leaves
 *   NOTHING behind: no window, no partial widget set, no slot. That is what makes
 *   the retry loop work: the tool answers with the exact JSON path, the reason,
 *   and (for an expression) the byte offset, the model fixes that one thing, and
 *   the second attempt is a fresh launch rather than a repair.
 *
 *   Every limit below is a static bound with a message that names it, because
 *   "too big" without a number costs the model another turn.
 *
 * ============================================================================
 * THE DOCUMENT FORMAT
 * ============================================================================
 *   {
 *     "title":  "Calculator",           // window title, optional
 *     "width":  216, "height": 266,     // client-ish size in pixels, clamped
 *     "grid":   { "rows":6, "cols":4, "gap":5, "pad":6 },
 *     "vars":   { "acc":0, "op":"", "err":0 },
 *     "widgets": [
 *       { "kind":"field", "name":"display", "text":"0", "align":"right",
 *         "readonly":true, "row":0, "col":0, "colspan":4 },
 *       { "kind":"button", "text":"7", "tag":"digit", "row":1, "col":0 }
 *     ],
 *     "on": [
 *       { "click":"digit", "do":[ {"set":"display","to":"cat(display,key)"} ] }
 *     ]
 *   }
 *
 *   widgets[]  kind is button | label | field | panel. Placement is either grid
 *              (row/col/rowspan/colspan, exact-tiling via gui_grid) or an
 *              explicit "rect":[x,y,w,h] in client pixels. `name` is a unique
 *              handle; `tag` groups widgets so one handler serves ten digits.
 *              Optional flags: readonly, disabled, border, align.
 *
 *              TWO RULES THE RUNTIME APPLIES ON THE APP'S BEHALF, both because
 *              the alternative is an app that looks right and misbehaves:
 *                - a READONLY field is also made unclickable. A readout is
 *                  scenery, and gui_click resolves a widget by its visible text:
 *                  the moment a display reads "7", a click on "7" would be
 *                  ambiguous with the "7" key.
 *                - a widget any handler is BOUND TO is made clickable, even if
 *                  it is a label, a panel or a readonly field. Hanging a handler
 *                  on scenery would otherwise produce an app that never
 *                  responds, which is the hardest failure to diagnose from a
 *                  document that reads correctly. An explicit handler wins.
 *   vars       named cells, initialised from a JSON number or string. A var and
 *              a widget may not share a name — that would make an expression
 *              ambiguous, so it is refused rather than resolved by a rule nobody
 *              would remember.
 *   on[]       handlers. Exactly one event key per object: "click" or "submit",
 *              whose value is a widget name, a tag, or an array of either.
 *   statements {"set":<var|widget>,"to":"<expr>"} | {"if":"<expr>","then":[...],
 *              "else":[...]} | {"stop":true}
 *   expressions infix, evaluated left to right with C-like precedence, no side
 *              effects. Operands: numbers (fixed point, six decimals),
 *              'single-quoted' strings, var names, widget names (their current
 *              text), and `key` — the text of the widget that fired the handler.
 *              Operators: + - * / % == != < <= > >= && || ! and unary -.
 *              Functions: num text cat len digits has iserr abs min max.
 *
 *   NUMBERS ARE FIXED POINT: int64 scaled by 1,000,000, so six decimal places
 *   and a magnitude limit of APP_NUM_MAX (9e9). No FPU is used or needed (this
 *   kernel is built -mno-sse); multiply and divide go through __int128 so an
 *   intermediate cannot wrap, and anything outside the range is ERR, never a
 *   wrapped number.
 *
 * PUBLIC API
 *   lifecycle   app_launch app_close app_close_all
 *   queries     app_count app_at app_title app_is_app app_describe
 *               app_var_count app_var_at app_last_error
 *   stats       app_stats
 *   examples    app_example_calculator (the shipped, hand-written document)
 *
 * DEPENDENCIES
 *   json.h to read the document, gui.h/widgets.h to instantiate it, trace.h only
 *   from tools/app_tools.c (the runtime itself prints nothing, so it cannot
 *   interleave with the console mid-paint). No heap: instances live in a static
 *   pool, for the same reason gui.h's window pool does — an app that cannot start
 *   when memory is tight is an app that vanishes exactly when the operator needs
 *   to be told why. No hardware, so the whole runtime including the calculator
 *   runs on the host against a malloc'd framebuffer (tests/host/test_app.c).
 *
 * FUTURE EXTENSION POINTS
 *   - Persisting an app is one VFS write: the document is the app, so "save that
 *     calculator" is fs_write of the bytes the model already sent.
 *   - A "timer" event kind would slot in beside click/submit and be driven from
 *     the same gui_tick() the WM already runs, with the same per-event step
 *     budget as its bound.
 *   - Widget kinds are gui.h's business: when a list or a slider appears there,
 *     it becomes a `kind` string here and nothing else changes.
 *   - The expression language has room for user functions (a name -> statement
 *     list map) without touching the evaluator, since the compiler already
 *     resolves names to indices at load time.
 */
#ifndef APP_H
#define APP_H

#include <stdint.h>
#include <stddef.h>

/* ====================================================================== */
/* bounds — every one static, every one named in the error it causes       */
/* ====================================================================== */

/* Apps running at once. Four is two more than a 1024x768 screen shows
 * comfortably and keeps the static pool near 40 KiB. */
#define APP_MAX_INSTANCES   4

/* Document bytes accepted. A hand-written calculator is ~2.6 KB; 8 KiB leaves
 * room for something twice as elaborate while staying well inside the response
 * buffer a tool_use block arrives in (chat.h). */
#define APP_DOC_MAX         8192

/* Identifier length, including the NUL: var names, widget names, tags. */
#define APP_NAME_MAX        16

#define APP_MAX_VARS        16
#define APP_MAX_WIDGETS     48      /* == GUI_MAX_WIDGETS                    */
#define APP_MAX_HANDLERS    16
#define APP_MAX_SELECTORS   8       /* names/tags one handler may bind        */
#define APP_MAX_STMTS       128
#define APP_MAX_EXPRS       96
#define APP_MAX_OPS         512     /* compiled expression ops, all handlers  */
#define APP_MAX_CONSTS      64      /* distinct numeric literals             */
#define APP_STRPOOL         768     /* bytes of string literals              */
#define APP_TEXT_MAX        40      /* == GUI_TEXT_MAX: a value's string      */

/* Structural nesting of "if" inside "do". Deeper is refused, so statement
 * execution's recursion is bounded before anything runs. */
#define APP_MAX_IF_DEPTH    8

/* Expression stack slots. The compiler computes each expression's high-water
 * mark and refuses one that would need more, so the evaluator cannot overflow. */
#define APP_STACK           16

/* Statements executed per event. There are no loops, so this is only reachable
 * by a pathological document; it exists so the bound is stated, not discovered. */
#define APP_MAX_STEPS       512

/* Fixed-point scale and magnitude limit. 1e6 gives six decimals; 9e9 is the
 * largest magnitude a value may hold (as gui/gui_demo.c's calculator uses). */
#define APP_NUM_SCALE       1000000LL
#define APP_NUM_MAX         9000000000000000LL

/* ====================================================================== */
/* errors                                                                 */
/* ====================================================================== */

#define APP_OK          0
#define APP_EINVAL    -22    /* the document is wrong; err says exactly how   */
#define APP_ENOSPC    -28    /* a static bound was exceeded; err names it     */
#define APP_ENOGUI    -19    /* no framebuffer, so no window manager          */
#define APP_EBUSY     -16    /* all APP_MAX_INSTANCES slots are in use        */

/* Why a document was refused, in the terms the model needs to fix it:
 *   path    where in the document, in JSON path form ("widgets[3].kind")
 *   msg     what is wrong and what was expected
 *   offset  byte offset inside an expression string, or -1
 * Always NUL-terminated; never contains a newline, so it composes into a
 * one-line tool result and can never break a trace line. */
typedef struct app_error {
    char path[72];
    char msg[224];
    int  offset;
} app_error_t;

/* Cumulative for the life of the machine. */
typedef struct app_stats {
    uint32_t launches;         /* documents accepted                          */
    uint32_t rejections;       /* documents refused (with a reason)           */
    uint32_t closes;
    uint32_t events;           /* handlers run                                */
    uint32_t steps;            /* statements executed                         */
    uint32_t sets;             /* assignments performed                       */
    uint32_t err_values;       /* ERR values produced by an operation         */
    uint32_t step_limits;      /* handlers cut off by APP_MAX_STEPS           */
} app_stats_t;

/* ====================================================================== */
/* lifecycle                                                              */
/* ====================================================================== */

/* Compile `doc[0..len)` and, only if every part of it validates, open a window
 * for it. `len` may be 0 for a NUL-terminated document. x/y are screen pixels or
 * GUI_POS_AUTO. On success *out_id is the window id (which is also the app id)
 * and the return is APP_OK; on failure nothing at all is left behind and *err
 * (may be NULL) says why. The document is NOT retained: everything needed is
 * compiled, so the caller's buffer may be reused immediately. */
int app_launch(const char *doc, size_t len, int32_t x, int32_t y,
               uint32_t *out_id, app_error_t *err);

/* Close a running app and release its slot. Returns APP_OK, or APP_EINVAL when
 * no app has that id. Closing the window by any other route (the close box,
 * gui_window_close, gui_close_all) releases the slot too — the runtime learns it
 * from GUI_EV_CLOSE, so there is exactly one teardown path. */
int app_close(uint32_t id);
void app_close_all(void);

/* ====================================================================== */
/* queries                                                                */
/* ====================================================================== */

int      app_count(void);
uint32_t app_at(int index);              /* 0 when out of range              */
int      app_is_app(uint32_t id);        /* 1 if that window is a launched app */
const char *app_title(uint32_t id);      /* "" when unknown                  */

/* Named vars and their current values, for the state view a tool prints.
 * `value` receives the value rendered as text (a number in the calculator's own
 * formatting, a string verbatim, or "error"). */
int  app_var_count(uint32_t id);
int  app_var_at(uint32_t id, int index, const char **name,
                char *value, size_t cap);

/* The last runtime note for this app: a step-limit cut-off or an ERR value that
 * reached a widget. "" when nothing has gone wrong. Never NULL. */
const char *app_last_error(uint32_t id);

/* One-line-per-widget human description (kind, name, tag, text) for a tool
 * result. Returns the number of bytes that WOULD be written, snprintf-style. */
size_t app_describe(uint32_t id, char *dst, size_t cap);

const app_stats_t *app_stats(void);

/* ====================================================================== */
/* the shipped example                                                    */
/* ====================================================================== */

/* The hand-written calculator document (apps/examples/calculator.json, compiled
 * in by apps/examples/calculator_json.h). It exists for the same reason
 * vm/programs/ac97_bringup.dvm does: prove the RUNTIME can run a calculator
 * before asking a model to write one, so that a model's failure is attributable
 * to the model. Never NULL. */
const char *app_example_calculator(void);

#endif /* APP_H */
