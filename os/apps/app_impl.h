/* app_impl.h — the compiled form of an app document. Internal to apps/.
 *
 * PURPOSE
 *   include/app.h is the contract the rest of the kernel and the model's tools
 *   see. This header is the representation those two files agree on: what a
 *   document turns INTO once it has been validated, and the seam between
 *   apps/expr.c (values, numbers, expressions — pure arithmetic and text, no GUI)
 *   and apps/runtime.c (documents, widgets, statements, windows).
 *
 *   It is a private header on purpose. Nothing outside apps/ should be able to
 *   build an app by hand: app_launch() is the only way in, so the only code that
 *   can produce these structures is the code that validates them.
 *   tests/host/test_app.c includes it directly, which is how the evaluator is
 *   tested as a unit rather than only through a window.
 *
 * WHY POSTFIX
 *   Expressions are compiled at LOAD time into a flat postfix op list with a
 *   pre-computed stack high-water mark. Three properties fall out of that, all of
 *   which matter on a kernel with no memory protection:
 *     1. Evaluation is a loop over an array with no recursion, so a hostile
 *        "((((((..." costs stack in the COMPILER (bounded by APP_PAREN_DEPTH and
 *        rejected with a message) and none at all in the evaluator.
 *     2. Every name is resolved to an index while the document is being checked,
 *        so a typo is a load error with a byte offset instead of a lookup failure
 *        on the tenth click.
 *     3. The stack bound is proved before the app runs, not defended at runtime.
 *
 * EVERY OPERATION IS TOTAL
 *   There is no failure path out of eval(): division by zero, overflow, a string
 *   in an arithmetic slot and num("abc") all produce the ERR value, which
 *   propagates. See DECISION 2 in include/app.h.
 */
#ifndef APP_IMPL_H
#define APP_IMPL_H

#include <stdint.h>
#include <stddef.h>

#include "app.h"
#include "gui.h"
#include "widgets.h"

/* Bytes of one expression source string, including the NUL. Long enough for
 * anything readable and short enough that a runaway string is refused with a
 * number in the message. */
#define APP_EXPR_SRC_MAX   192

/* Nesting of parentheses and calls the compiler will follow. The compiler is the
 * only recursive part of this module. */
#define APP_PAREN_DEPTH    12

/* ====================================================================== */
/* values                                                                 */
/* ====================================================================== */

#define AV_NUM  0     /* fixed point, scaled by APP_NUM_SCALE               */
#define AV_STR  1     /* NUL-terminated, at most APP_TEXT_MAX-1 bytes        */
#define AV_ERR  2     /* the error value; text() renders it as "error"       */

typedef struct app_val {
    uint8_t kind;
    int64_t num;
    char    str[APP_TEXT_MAX];
} app_val_t;

/* ====================================================================== */
/* compiled expressions                                                   */
/* ====================================================================== */

enum {
    AO_CONST = 0,   /* arg = index into konst[]                            */
    AO_STR,         /* arg = offset into strpool                           */
    AO_VAR,         /* arg = var index                                     */
    AO_WIDGET,      /* arg = widget index; yields the widget's text         */
    AO_KEY,         /* the text of the widget that fired the handler        */
    AO_ADD, AO_SUB, AO_MUL, AO_DIV, AO_MOD,
    AO_EQ, AO_NE, AO_LT, AO_LE, AO_GT, AO_GE,
    AO_AND, AO_OR,
    AO_NOT, AO_NEG,
    AO_NUM, AO_TEXT, AO_CAT, AO_LEN, AO_DIGITS, AO_HAS, AO_ISERR,
    AO_ABS, AO_MIN, AO_MAX,
    AO__COUNT
};

typedef struct app_op {
    uint8_t  op;
    uint8_t  pad_;
    uint16_t arg;
} app_op_t;

typedef struct app_expr {
    uint16_t first;      /* index into inst->op                             */
    uint16_t count;
    uint8_t  depth;      /* stack slots this expression needs (<= APP_STACK) */
} app_expr_t;

/* ====================================================================== */
/* compiled statements                                                    */
/* ====================================================================== */

#define AS_SET   0
#define AS_IF    1
#define AS_STOP  2

#define AT_VAR     0
#define AT_WIDGET  1

typedef struct app_stmt {
    uint8_t  kind;
    uint8_t  tgt_kind;      /* AS_SET: AT_VAR or AT_WIDGET                  */
    uint16_t tgt;           /* AS_SET: var or widget index                  */
    uint16_t expr;          /* AS_SET / AS_IF: expression index             */
    uint16_t then_first, then_count;
    uint16_t else_first, else_count;
} app_stmt_t;

/* ====================================================================== */
/* handlers                                                               */
/* ====================================================================== */

#define AE_CLICK   0
#define AE_SUBMIT  1

/* Selectors (names and tags) are resolved at load time into a bitmask of widget
 * indices, so dispatch is a shift and a test and no string comparison happens on
 * a click. APP_MAX_WIDGETS is 48, which fits a uint64_t with room to spare. */
typedef struct app_handler {
    uint8_t  ev;
    uint64_t mask;
    uint16_t first, count;      /* statement block                          */
} app_handler_t;

/* ====================================================================== */
/* one running app                                                        */
/* ====================================================================== */

typedef struct app_inst {
    uint8_t   used;
    uint32_t  win;                                  /* window id == app id  */
    char      title[GUI_TITLE_MAX];

    char      wname[APP_MAX_WIDGETS][APP_NAME_MAX];
    uint16_t  nwidget;

    char      vname[APP_MAX_VARS][APP_NAME_MAX];
    app_val_t var[APP_MAX_VARS];
    uint16_t  nvar;

    app_stmt_t stmt[APP_MAX_STMTS];
    uint16_t   nstmt;
    app_expr_t expr[APP_MAX_EXPRS];
    uint16_t   nexpr;
    app_op_t   op[APP_MAX_OPS];
    uint16_t   nop;
    int64_t    konst[APP_MAX_CONSTS];
    uint16_t   nkonst;
    char       strpool[APP_STRPOOL];
    uint16_t   strused;

    app_handler_t handler[APP_MAX_HANDLERS];
    uint16_t      nhandler;

    char      lasterr[128];
} app_inst_t;

/* ====================================================================== */
/* apps/expr.c — numbers, values, compilation and evaluation               */
/* ====================================================================== */

/* Fixed-point text <-> number. Both hand-rolled: there is no strtod in this
 * kernel, no %f in its printf, and a calculator's formatting rules (six
 * decimals, trailing zeros trimmed, the point kept only when it earns its place)
 * are not printf's rules anyway. */
int  app_num_parse(const char *s, int64_t *out);          /* 0 ok, -1 not a number */
void app_num_format(int64_t v, char *dst, size_t cap);

/* Value constructors and the one renderer everything text-shaped goes through. */
app_val_t   app_val_num(int64_t scaled);
app_val_t   app_val_str(const char *s);
app_val_t   app_val_err(void);
const char *app_val_text(const app_val_t *v, char *dst, size_t cap);
int         app_val_truthy(const app_val_t *v);           /* -1 when ERR */

/* Compile one expression source string into `in`. Returns APP_OK, or APP_EINVAL /
 * APP_ENOSPC with err->msg and err->offset (a byte offset into `src`) filled in.
 * On failure `in` may hold partly-emitted ops; the caller abandons the whole
 * instance, which is why that is safe. */
int app_expr_compile(app_inst_t *in, const char *src, uint16_t *out_index,
                     app_error_t *err);

/* Evaluate a compiled expression. `key` is the value of `key` in that context
 * (may be NULL, in which case `key` evaluates to the empty string) and `win` is
 * where widget text is read from (may be NULL: every widget then reads ERR).
 * Total: always returns a value. */
app_val_t app_expr_eval(const app_inst_t *in, uint16_t index,
                        const app_val_t *key, const gui_window_t *win);

/* Bump the shared ERR counter in app_stats(). Defined in runtime.c so that
 * expr.c stays free of state. */
void app_note_err_value(void);

#endif /* APP_IMPL_H */
