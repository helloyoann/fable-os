/* runtime.c — turn an app document into a running window.
 *
 * PURPOSE
 *   This is the file the headline capability rests on: the operator says "hey I
 *   want a calculator", the model writes a JSON document, and this code checks
 *   it, lays it out, binds its behaviour and opens it — or refuses it with a
 *   sentence precise enough that the model's second attempt works.
 *
 * THE SHAPE OF THE CODE FOLLOWS THE SAFETY RULE
 *   Nothing is created until everything is understood. app_launch() runs in two
 *   halves that cannot be interleaved:
 *
 *     1. COMPILE. Read the document with json.h into a reserved instance slot:
 *        title, size, grid, vars, widget descriptors, handlers, statements,
 *        expressions. Every name is resolved to an index here, every bound is
 *        checked here, and every failure returns with a JSON path and a reason.
 *        Not one GUI call happens in this half.
 *     2. INSTANTIATE. Only if the first half succeeded: open the window, add the
 *        widgets in document order, attach the hook, invalidate.
 *
 *   So a rejected document leaves nothing behind — no window, no half-built
 *   widget list, no slot — and the model may simply try again. A document that is
 *   accepted cannot then fail to instantiate, because the widget count was
 *   checked against GUI_MAX_WIDGETS while it was still data.
 *
 * WHY THE ERROR MESSAGES ARE THE PRODUCT
 *   A model that is told "invalid document" burns a turn guessing. A model that
 *   is told `on[3].do[1].to: unknown name "displya" at offset 4. Known names
 *   here: acc, op, err, display, key` fixes exactly one character. Every refusal
 *   in this file therefore carries the path, the expectation, and — where there
 *   is one — the offset and the list of what WOULD have been accepted. That is
 *   the whole feedback loop the "build me a calculator" capability depends on,
 *   and tests/host/test_app.c asserts the text of it.
 *
 * UNTRUSTED INPUT
 *   The document is model output. It is read exclusively through json.h (never
 *   scanned by hand), types are checked before values and values before ranges,
 *   strings are length-checked and rejected if they contain an embedded NUL (a
 *   JSON string may legally carry one, and every name comparison here is on a C
 *   string), and no number from the document ever indexes memory before it has
 *   been range-checked. Nothing here allocates; every structure is a fixed array
 *   in a static pool.
 *
 * WHAT AN APP CANNOT DO, ENFORCED HERE
 *   A handler runs at most APP_MAX_STEPS statements with at most APP_MAX_IF_DEPTH
 *   nesting, has no loop construct at all, cannot see another window, cannot
 *   print, and cannot call a kernel function. The runtime prints nothing during
 *   an event: the console belongs to the turn loop and to trace.h, and an app
 *   that could write to it could forge a kernel trace line.
 */

#include "app_impl.h"

#include "json.h"
#include "gui.h"
#include "widgets.h"

#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#include <stdio.h>       /* vsnprintf / snprintf; port/stdio.h freestanding */

/* The document as a C string, generated from apps/examples/calculator.json. */
#include "examples/calculator_json.h"

/* ====================================================================== */
/* small helpers                                                          */
/* ====================================================================== */

static size_t a_len(const char *s) {
    size_t n = 0;
    if (!s) return 0;
    while (s[n]) n++;
    return n;
}

static void a_cpy(char *dst, size_t cap, const char *src) {
    size_t i = 0;
    if (!dst || cap == 0) return;
    if (src) for (; src[i] && i + 1 < cap; i++) dst[i] = src[i];
    dst[i] = '\0';
}

static int a_eq(const char *a, const char *b) {
    size_t i = 0;
    if (!a || !b) return 0;
    for (; a[i] && a[i] == b[i]; i++) { }
    return a[i] == '\0' && b[i] == '\0';
}

/* ====================================================================== */
/* the instance pool                                                      */
/* ====================================================================== */

static app_inst_t   pool[APP_MAX_INSTANCES];
static app_stats_t  stats;

void app_note_err_value(void) { stats.err_values++; }

const app_stats_t *app_stats(void) { return &stats; }

static app_inst_t *inst_of(uint32_t id) {
    if (!id) return (app_inst_t *)0;
    for (int i = 0; i < APP_MAX_INSTANCES; i++)
        if (pool[i].used && pool[i].win == id) return &pool[i];
    return (app_inst_t *)0;
}

static void inst_clear(app_inst_t *in) {
    unsigned char *p = (unsigned char *)in;
    for (size_t i = 0; i < sizeof *in; i++) p[i] = 0;
}

static app_inst_t *inst_alloc(void) {
    for (int i = 0; i < APP_MAX_INSTANCES; i++)
        if (!pool[i].used) { inst_clear(&pool[i]); pool[i].used = 1; return &pool[i]; }
    return (app_inst_t *)0;
}

/* ====================================================================== */
/* errors                                                                 */
/* ====================================================================== */

static void err_clear(app_error_t *e) {
    if (!e) return;
    e->path[0] = '\0';
    e->msg[0]  = '\0';
    e->offset  = -1;
}

/* Set the path and the message and return the code. The path is built by the
 * caller as it walks, so a message never has to describe where it is. */
static int rej(app_error_t *e, int code, const char *path, const char *fmt, ...) {
    if (e) {
        va_list ap;
        a_cpy(e->path, sizeof e->path, path ? path : "");
        va_start(ap, fmt);
        vsnprintf(e->msg, sizeof e->msg, fmt, ap);
        va_end(ap);
    }
    stats.rejections++;
    return code;
}

static void pathf(char *dst, size_t cap, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(dst, cap, fmt, ap);
    va_end(ap);
}

/* ====================================================================== */
/* typed field readers (the tools/gui_tools.c convention)                  */
/*   1 present and valid, 0 absent or null, -1 wrong type,                */
/*   -2 out of range / too long, -3 embedded NUL                          */
/* ====================================================================== */

static int f_str(const json_value_t *o, const char *key, char *dst, size_t cap) {
    json_value_t v;
    int rc = json_get(o, key, &v);
    if (rc == JSON_ENOENT) return 0;
    if (rc != JSON_OK) return -1;
    if (v.type == JSON_NULL) return 0;
    if (v.type != JSON_STRING) return -1;
    size_t n = 0;
    rc = json_str(&v, dst, cap, &n);
    if (rc == JSON_ENOSPC) return -2;
    if (rc != JSON_OK) return -1;
    for (size_t i = 0; i < n; i++) if (dst[i] == '\0') return -3;
    return 1;
}

static int f_int(const json_value_t *o, const char *key,
                 int64_t lo, int64_t hi, int64_t *out) {
    json_value_t v;
    int rc = json_get(o, key, &v);
    if (rc == JSON_ENOENT) return 0;
    if (rc != JSON_OK) return -1;
    if (v.type == JSON_NULL) return 0;
    if (v.type != JSON_NUMBER) return -1;
    for (size_t i = 0; i < v.len; i++)
        if (v.start[i] == '.' || v.start[i] == 'e' || v.start[i] == 'E') return -1;
    int64_t x = 0;
    if (json_int(&v, &x) != JSON_OK) return -2;
    if (x < lo || x > hi) return -2;
    *out = x;
    return 1;
}

static int f_bool(const json_value_t *o, const char *key, int *out) {
    json_value_t v;
    int rc = json_get(o, key, &v);
    if (rc == JSON_ENOENT) return 0;
    if (rc != JSON_OK) return -1;
    if (v.type == JSON_NULL) return 0;
    if (v.type != JSON_BOOL) return -1;
    if (json_bool(&v, out) != JSON_OK) return -1;
    return 1;
}

/* A JSON number as a fixed-point value. Fractions are welcome here (unlike a
 * pixel coordinate): 0.5 is a perfectly good initial value for a var. The span
 * is re-parsed by app_num_parse() rather than by json_int(), because json_int
 * truncates a fraction and rejects nothing that a calculator would care about. */
static int f_scaled(const json_value_t *v, int64_t *out) {
    char buf[32];
    if (v->len >= sizeof buf) return -2;
    for (size_t i = 0; i < v->len; i++) buf[i] = v->start[i];
    buf[v->len] = '\0';
    if (app_num_parse(buf, out) != 0) return -2;
    return 1;
}

/* ====================================================================== */
/* names                                                                  */
/* ====================================================================== */

static const char *const reserved[] = {
    "key", "num", "text", "cat", "len", "digits", "has", "iserr",
    "abs", "min", "max", 0
};

/* A name must be usable as an identifier in an expression, or the app would
 * declare something it could never refer to. */
static int name_ok(const char *s) {
    if (!s || !s[0]) return 0;
    if (!((s[0] >= 'a' && s[0] <= 'z') || (s[0] >= 'A' && s[0] <= 'Z') ||
          s[0] == '_')) return 0;
    for (size_t i = 1; s[i]; i++) {
        char ch = s[i];
        if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
              (ch >= '0' && ch <= '9') || ch == '_')) return 0;
    }
    return 1;
}

static int name_reserved(const char *s) {
    for (int i = 0; reserved[i]; i++) if (a_eq(s, reserved[i])) return 1;
    return 0;
}

static int name_taken(const app_inst_t *in, const char *s) {
    for (uint16_t i = 0; i < in->nvar; i++) if (a_eq(s, in->vname[i])) return 1;
    for (uint16_t i = 0; i < in->nwidget; i++)
        if (in->wname[i][0] && a_eq(s, in->wname[i])) return 1;
    return 0;
}

/* Refuse a key nobody reads. A silently ignored "hight" or "colSpan" is the
 * worst failure this format can have: the app launches, looks wrong, and nothing
 * says why — so the model has nothing to correct. "note" is accepted everywhere
 * as a comment, because JSON has none and a document is also something a human
 * reads. */
static int check_keys(const json_value_t *obj, const char *path,
                      const char *const *allowed, app_error_t *err) {
    size_t n = json_count(obj);
    for (size_t i = 0; i < n; i++) {
        json_value_t k;
        char         key[APP_NAME_MAX + 8];
        size_t       kl = 0;
        if (json_member(obj, i, &k, (json_value_t *)0) != JSON_OK) continue;
        if (json_str(&k, key, sizeof key, &kl) != JSON_OK)
            return rej(err, APP_EINVAL, path, "has a key longer than %d bytes",
                       (int)sizeof key - 1);
        if (a_eq(key, "note")) continue;
        int ok = 0;
        for (int j = 0; allowed[j] && !ok; j++) ok = a_eq(key, allowed[j]);
        if (!ok) {
            char list[176];
            size_t o = 0;
            list[0] = '\0';
            for (int j = 0; allowed[j]; j++) {
                size_t need = a_len(allowed[j]) + (o ? 2 : 0);
                if (o + need + 1 >= sizeof list) break;
                if (o) { list[o++] = ','; list[o++] = ' '; }
                a_cpy(list + o, sizeof list - o, allowed[j]);
                o += a_len(allowed[j]);
            }
            return rej(err, APP_EINVAL, path,
                       "unknown key \"%s\" (nothing would read it). Accepted "
                       "here: %s, note", key, list);
        }
    }
    return APP_OK;
}

/* ====================================================================== */
/* widget descriptors — the layout, still as data                         */
/* ====================================================================== */

typedef struct {
    uint8_t  kind;
    uint8_t  align;
    uint8_t  state;
    uint8_t  use_rect;
    int32_t  row, col, rowspan, colspan;
    int32_t  rect[4];
    char     text[APP_TEXT_MAX];
    char     tag[APP_NAME_MAX];
} wdesc_t;

/* One launch at a time: this kernel has one thread, app_launch() is only reached
 * from a tool or from bring-up, and an app's handlers cannot launch an app. Kept
 * out of app_inst_t because it is needed only while compiling. */
static wdesc_t   descs[APP_MAX_WIDGETS];
static int32_t   g_rows, g_cols, g_gap, g_pad;

/* ====================================================================== */
/* compiling statements                                                   */
/* ====================================================================== */

static int compile_block(app_inst_t *in, const json_value_t *arr,
                         const char *path, int depth,
                         uint16_t *out_first, uint16_t *out_count,
                         app_error_t *err);

static int compile_expr_field(app_inst_t *in, const json_value_t *obj,
                              const char *key, const char *path,
                              uint16_t *out_expr, app_error_t *err) {
    char src[APP_EXPR_SRC_MAX];
    char p[72];
    int  rc = f_str(obj, key, src, sizeof src);

    pathf(p, sizeof p, "%s.%s", path, key);
    if (rc == 0)
        return rej(err, APP_EINVAL, p,
                   "missing: an expression string is required here");
    if (rc == -1)
        return rej(err, APP_EINVAL, p,
                   "must be a STRING holding an expression, e.g. \"acc + 1\" or "
                   "\"cat(display, key)\" (numbers and booleans are written "
                   "inside that string too)");
    if (rc == -2)
        return rej(err, APP_ENOSPC, p, "expression is longer than %d bytes",
                   APP_EXPR_SRC_MAX - 1);
    if (rc == -3)
        return rej(err, APP_EINVAL, p,
                   "contains an embedded NUL character");

    app_error_t sub;
    err_clear(&sub);
    int crc = app_expr_compile(in, src, out_expr, &sub);
    if (crc != APP_OK) {
        if (err) {
            a_cpy(err->path, sizeof err->path, p);
            if (sub.offset >= 0)
                snprintf(err->msg, sizeof err->msg, "%s (at offset %d of \"%s\")",
                         sub.msg, sub.offset, src);
            else
                a_cpy(err->msg, sizeof err->msg, sub.msg);
            err->offset = sub.offset;
        }
        stats.rejections++;
        return crc;
    }
    return APP_OK;
}

static int compile_stmt(app_inst_t *in, const json_value_t *st,
                        const char *path, int depth, uint16_t slot,
                        app_error_t *err) {
    json_value_t v;
    app_stmt_t  *out = &in->stmt[slot];

    static const char *const skeys[] = {
        "set", "to", "if", "then", "else", "stop", 0
    };

    if (st->type != JSON_OBJECT)
        return rej(err, APP_EINVAL, path,
                   "a statement must be a JSON object: {\"set\":...,\"to\":...}, "
                   "{\"if\":...,\"then\":[...]} or {\"stop\":true}");
    if (check_keys(st, path, skeys, err) != APP_OK) return APP_EINVAL;

    int has_set  = json_get(st, "set",  &v) == JSON_OK;
    int has_if   = json_get(st, "if",   &v) == JSON_OK;
    int has_stop = json_get(st, "stop", &v) == JSON_OK;
    if (has_set + has_if + has_stop == 0)
        return rej(err, APP_EINVAL, path,
                   "no known verb. A statement is {\"set\":\"<name>\",\"to\":"
                   "\"<expr>\"}, {\"if\":\"<expr>\",\"then\":[...],\"else\":"
                   "[...]} or {\"stop\":true}");
    if (has_set + has_if + has_stop > 1)
        return rej(err, APP_EINVAL, path,
                   "a statement must carry exactly one verb, but it has more "
                   "than one of \"set\", \"if\" and \"stop\"");

    if (has_stop) {
        int on = 0;
        if (f_bool(st, "stop", &on) != 1 || !on)
            return rej(err, APP_EINVAL, path,
                       "\"stop\" must be the boolean true; it ends this handler");
        out->kind = AS_STOP;
        return APP_OK;
    }

    if (has_set) {
        char target[APP_NAME_MAX];
        char p[72];
        int  rc = f_str(st, "set", target, sizeof target);
        pathf(p, sizeof p, "%s.set", path);
        if (rc == 0 || rc == -1)
            return rej(err, APP_EINVAL, p,
                       "must be the name of a var or of a widget to assign to");
        if (rc == -2)
            return rej(err, APP_EINVAL, p,
                       "name is longer than %d bytes", APP_NAME_MAX - 1);
        if (rc == -3)
            return rej(err, APP_EINVAL, p, "contains an embedded NUL character");

        out->kind = AS_SET;
        int found = 0;
        for (uint16_t i = 0; i < in->nvar; i++)
            if (a_eq(target, in->vname[i])) {
                out->tgt_kind = AT_VAR; out->tgt = i; found = 1; break;
            }
        if (!found)
            for (uint16_t i = 0; i < in->nwidget; i++)
                if (in->wname[i][0] && a_eq(target, in->wname[i])) {
                    out->tgt_kind = AT_WIDGET; out->tgt = i; found = 1; break;
                }
        if (!found)
            return rej(err, APP_EINVAL, p,
                       "\"%s\" is not a var or a named widget. Declare it in "
                       "\"vars\", or give the widget you mean a \"name\"", target);

        return compile_expr_field(in, st, "to", path, &out->expr, err);
    }

    /* if / then / else */
    if (depth + 1 > APP_MAX_IF_DEPTH)
        return rej(err, APP_ENOSPC, path,
                   "\"if\" statements are nested more than %d deep",
                   APP_MAX_IF_DEPTH);

    out->kind = AS_IF;
    int rc = compile_expr_field(in, st, "if", path, &out->expr, err);
    if (rc != APP_OK) return rc;

    char p[72];
    pathf(p, sizeof p, "%s.then", path);
    if (json_get(st, "then", &v) != JSON_OK || v.type != JSON_ARRAY)
        return rej(err, APP_EINVAL, p,
                   "an \"if\" needs \"then\": an array of statements (\"else\" "
                   "is optional)");
    rc = compile_block(in, &v, p, depth + 1, &out->then_first, &out->then_count,
                       err);
    if (rc != APP_OK) return rc;

    if (json_get(st, "else", &v) == JSON_OK && v.type != JSON_NULL) {
        pathf(p, sizeof p, "%s.else", path);
        if (v.type != JSON_ARRAY)
            return rej(err, APP_EINVAL, p, "\"else\" must be an array of "
                                           "statements");
        rc = compile_block(in, &v, p, depth + 1, &out->else_first,
                           &out->else_count, err);
        if (rc != APP_OK) return rc;
    }
    return APP_OK;
}

/* A block's statements must be contiguous, so the slots are reserved BEFORE any
 * nested block is compiled into the same pool. */
static int compile_block(app_inst_t *in, const json_value_t *arr,
                         const char *path, int depth,
                         uint16_t *out_first, uint16_t *out_count,
                         app_error_t *err) {
    size_t n = json_count(arr);

    if (depth > APP_MAX_IF_DEPTH)
        return rej(err, APP_ENOSPC, path, "statements are nested more than %d "
                                          "deep", APP_MAX_IF_DEPTH);
    if ((size_t)in->nstmt + n > APP_MAX_STMTS)
        return rej(err, APP_ENOSPC, path,
                   "this app has more than %d statements in total", APP_MAX_STMTS);

    uint16_t base = in->nstmt;
    in->nstmt = (uint16_t)(base + n);
    *out_first = base;
    *out_count = (uint16_t)n;

    for (size_t i = 0; i < n; i++) {
        json_value_t st;
        char         p[72];
        pathf(p, sizeof p, "%s[%d]", path, (int)i);
        if (json_at(arr, i, &st) != JSON_OK)
            return rej(err, APP_EINVAL, p, "could not be read");
        int rc = compile_stmt(in, &st, p, depth, (uint16_t)(base + i), err);
        if (rc != APP_OK) return rc;
    }
    return APP_OK;
}

/* ====================================================================== */
/* compiling the document                                                 */
/* ====================================================================== */

static int parse_vars(app_inst_t *in, const json_value_t *root,
                      app_error_t *err) {
    json_value_t vars;
    int rc = json_get(root, "vars", &vars);
    if (rc == JSON_ENOENT) return APP_OK;
    if (rc != JSON_OK || vars.type != JSON_OBJECT)
        return rej(err, APP_EINVAL, "vars",
                   "must be an object of name -> initial value, e.g. "
                   "{\"acc\":0,\"op\":\"\"}");

    size_t n = json_count(&vars);
    if (n > APP_MAX_VARS)
        return rej(err, APP_ENOSPC, "vars", "%d vars declared; the limit is %d",
                   (int)n, APP_MAX_VARS);

    for (size_t i = 0; i < n; i++) {
        json_value_t k, v;
        char         name[APP_NAME_MAX];
        char         p[72];
        if (json_member(&vars, i, &k, &v) != JSON_OK)
            return rej(err, APP_EINVAL, "vars", "member %d could not be read",
                       (int)i);
        size_t nl = 0;
        if (json_str(&k, name, sizeof name, &nl) != JSON_OK)
            return rej(err, APP_EINVAL, "vars",
                       "a var name is longer than %d bytes", APP_NAME_MAX - 1);
        pathf(p, sizeof p, "vars.%s", name);
        /* A JSON key may legally contain a NUL, and every name comparison here
         * is on a C string: without this, {"a\0b":0} would quietly declare a
         * var called "a" that the document never refers to. */
        for (size_t j = 0; j < nl; j++)
            if (name[j] == '\0')
                return rej(err, APP_EINVAL, "vars",
                           "a var name contains an embedded NUL character");
        if (!name_ok(name))
            return rej(err, APP_EINVAL, p,
                       "not a usable name: use a letter or '_' then letters, "
                       "digits or '_' (it has to be writable in an expression)");
        if (name_reserved(name))
            return rej(err, APP_EINVAL, p,
                       "\"%s\" is a reserved word in expressions", name);
        if (name_taken(in, name))
            return rej(err, APP_EINVAL, p, "declared twice");

        app_val_t val;
        if (v.type == JSON_NUMBER) {
            int64_t scaled = 0;
            if (f_scaled(&v, &scaled) != 1)
                return rej(err, APP_EINVAL, p,
                           "number is outside what this machine can hold (6 "
                           "decimal places, magnitude up to 9000000000)");
            val = app_val_num(scaled);
        } else if (v.type == JSON_STRING) {
            char buf[APP_TEXT_MAX];
            size_t sl = 0;
            if (json_str(&v, buf, sizeof buf, &sl) != JSON_OK)
                return rej(err, APP_EINVAL, p,
                           "string is longer than %d bytes", APP_TEXT_MAX - 1);
            for (size_t j = 0; j < sl; j++)
                if (buf[j] == '\0')
                    return rej(err, APP_EINVAL, p,
                               "contains an embedded NUL character");
            val = app_val_str(buf);
        } else if (v.type == JSON_BOOL) {
            int b = 0;
            json_bool(&v, &b);
            val = app_val_num(b ? APP_NUM_SCALE : 0);
        } else {
            return rej(err, APP_EINVAL, p,
                       "a var's initial value must be a number, a string or a "
                       "boolean");
        }

        a_cpy(in->vname[in->nvar], APP_NAME_MAX, name);
        in->var[in->nvar] = val;
        in->nvar++;
    }
    return APP_OK;
}

static int kind_of(const char *s, uint8_t *out) {
    if (a_eq(s, "button")) { *out = GUI_BUTTON; return 1; }
    if (a_eq(s, "label"))  { *out = GUI_LABEL;  return 1; }
    if (a_eq(s, "field"))  { *out = GUI_FIELD;  return 1; }
    if (a_eq(s, "panel"))  { *out = GUI_PANEL;  return 1; }
    return 0;
}

static int align_of(const char *s, uint8_t *out) {
    if (a_eq(s, "left"))   { *out = GUI_ALIGN_LEFT;   return 1; }
    if (a_eq(s, "center") || a_eq(s, "centre")) { *out = GUI_ALIGN_CENTER; return 1; }
    if (a_eq(s, "right"))  { *out = GUI_ALIGN_RIGHT;  return 1; }
    return 0;
}

static int parse_widgets(app_inst_t *in, const json_value_t *root,
                         app_error_t *err) {
    json_value_t arr;
    if (json_get(root, "widgets", &arr) != JSON_OK || arr.type != JSON_ARRAY)
        return rej(err, APP_EINVAL, "widgets",
                   "required: an array of widget objects, each with a \"kind\" "
                   "of button, label, field or panel");

    size_t n = json_count(&arr);
    if (n == 0)
        return rej(err, APP_EINVAL, "widgets",
                   "an app with no widgets would be an empty window");
    if (n > APP_MAX_WIDGETS)
        return rej(err, APP_ENOSPC, "widgets",
                   "%d widgets; a window holds at most %d", (int)n,
                   APP_MAX_WIDGETS);

    for (size_t i = 0; i < n; i++) {
        json_value_t w;
        wdesc_t     *d = &descs[i];
        char         p[72];
        char         buf[APP_TEXT_MAX];
        int          rc;

        static const char *const wkeys[] = {
            "kind", "text", "name", "tag", "align", "readonly", "disabled",
            "border", "rect", "row", "col", "rowspan", "colspan", 0
        };

        pathf(p, sizeof p, "widgets[%d]", (int)i);
        if (json_at(&arr, i, &w) != JSON_OK || w.type != JSON_OBJECT)
            return rej(err, APP_EINVAL, p, "must be a JSON object");
        if ((rc = check_keys(&w, p, wkeys, err)) != APP_OK) return rc;

        d->kind = GUI_BUTTON;
        d->align = 0xFF;                     /* 0xFF = "the kind's default" */
        d->state = 0;
        d->use_rect = 0;
        d->row = d->col = -1;
        d->rowspan = d->colspan = 1;
        d->text[0] = '\0';
        d->tag[0]  = '\0';
        in->wname[i][0] = '\0';

        rc = f_str(&w, "kind", buf, sizeof buf);
        if (rc <= 0 || !kind_of(buf, &d->kind)) {
            char kp[72];
            pathf(kp, sizeof kp, "%s.kind", p);
            return rej(err, APP_EINVAL, kp,
                       "required, and must be one of: button, label, field, "
                       "panel");
        }

        rc = f_str(&w, "text", d->text, sizeof d->text);
        if (rc == -1 || rc == -3) {
            char tp[72];
            pathf(tp, sizeof tp, "%s.text", p);
            return rej(err, APP_EINVAL, tp,
                       "must be a string with no embedded NUL");
        }
        if (rc == -2) {
            char tp[72];
            pathf(tp, sizeof tp, "%s.text", p);
            return rej(err, APP_ENOSPC, tp,
                       "widget text is longer than %d bytes", APP_TEXT_MAX - 1);
        }

        rc = f_str(&w, "name", buf, sizeof buf);
        if (rc == 1) {
            char np[72];
            pathf(np, sizeof np, "%s.name", p);
            if (a_len(buf) >= APP_NAME_MAX)
                return rej(err, APP_EINVAL, np, "longer than %d bytes",
                           APP_NAME_MAX - 1);
            if (!name_ok(buf))
                return rej(err, APP_EINVAL, np,
                           "not a usable name: a letter or '_' then letters, "
                           "digits or '_'");
            if (name_reserved(buf))
                return rej(err, APP_EINVAL, np,
                           "\"%s\" is a reserved word in expressions", buf);
            if (name_taken(in, buf))
                return rej(err, APP_EINVAL, np,
                           "\"%s\" is already the name of a var or another "
                           "widget; expressions would be ambiguous", buf);
            a_cpy(in->wname[i], APP_NAME_MAX, buf);
        } else if (rc < 0) {
            char np[72];
            pathf(np, sizeof np, "%s.name", p);
            if (rc == -3)
                return rej(err, APP_EINVAL, np,
                           "contains an embedded NUL character, so nothing could "
                           "ever refer to it");
            return rej(err, APP_EINVAL, np, "must be a short identifier string");
        }
        /* Published even before the whole array is read, so a later widget's
         * name collision is caught and a handler can bind to an earlier one. */
        in->nwidget = (uint16_t)(i + 1);

        rc = f_str(&w, "tag", d->tag, sizeof d->tag);
        if (rc < 0) {
            char tp[72];
            pathf(tp, sizeof tp, "%s.tag", p);
            return rej(err, APP_EINVAL, tp,
                       "must be a short identifier string shared by the widgets "
                       "one handler serves");
        }
        if (rc == 1 && !name_ok(d->tag)) {
            char tp[72];
            pathf(tp, sizeof tp, "%s.tag", p);
            return rej(err, APP_EINVAL, tp,
                       "not a usable tag: a letter or '_' then letters, digits "
                       "or '_'");
        }

        rc = f_str(&w, "align", buf, sizeof buf);
        if (rc == 1 && !align_of(buf, &d->align)) {
            char ap[72];
            pathf(ap, sizeof ap, "%s.align", p);
            return rej(err, APP_EINVAL, ap,
                       "must be left, center or right");
        }
        if (rc < 0) {
            char ap[72];
            pathf(ap, sizeof ap, "%s.align", p);
            return rej(err, APP_EINVAL, ap, "must be a string");
        }

        int flag = 0;
        if (f_bool(&w, "readonly", &flag) < 0)
            return rej(err, APP_EINVAL, p, "\"readonly\" must be a boolean");
        /* A read-only field is a readout, and a readout is scenery: it is also
         * made unclickable, so that gui_click resolving a widget by its visible
         * text can never match a display showing "7" instead of the "7" key. */
        if (flag) d->state |= (uint8_t)(GUI_W_READONLY | GUI_W_NOHIT);
        flag = 0;
        if (f_bool(&w, "disabled", &flag) < 0)
            return rej(err, APP_EINVAL, p, "\"disabled\" must be a boolean");
        if (flag) d->state |= GUI_W_DISABLED;
        flag = 0;
        if (f_bool(&w, "border", &flag) < 0)
            return rej(err, APP_EINVAL, p, "\"border\" must be a boolean");
        if (flag) d->state |= GUI_W_BORDER;

        /* Placement: an explicit rect, or a grid cell. */
        json_value_t r;
        if (json_get(&w, "rect", &r) == JSON_OK && r.type != JSON_NULL) {
            char rp[72];
            pathf(rp, sizeof rp, "%s.rect", p);
            if (r.type != JSON_ARRAY || json_count(&r) != 4)
                return rej(err, APP_EINVAL, rp,
                           "must be [x,y,width,height] in client pixels");
            for (int k = 0; k < 4; k++) {
                json_value_t e;
                int64_t      x = 0;
                if (json_at(&r, (size_t)k, &e) != JSON_OK ||
                    e.type != JSON_NUMBER || json_int(&e, &x) != JSON_OK ||
                    x < -4096 || x > 4096)
                    return rej(err, APP_EINVAL, rp,
                               "element %d must be an integer within +/-4096",
                               k);
                d->rect[k] = (int32_t)x;
            }
            if (d->rect[2] <= 0 || d->rect[3] <= 0)
                return rej(err, APP_EINVAL, rp,
                           "width and height must both be positive");
            d->use_rect = 1;
        } else {
            int64_t x = 0;
            char    gp[72];
            pathf(gp, sizeof gp, "%s.row", p);
            if (f_int(&w, "row", 0, 4095, &x) != 1)
                return rej(err, APP_EINVAL, gp,
                           "required (with \"col\") unless the widget gives a "
                           "\"rect\": the 0-based grid row");
            d->row = (int32_t)x;
            pathf(gp, sizeof gp, "%s.col", p);
            if (f_int(&w, "col", 0, 4095, &x) != 1)
                return rej(err, APP_EINVAL, gp,
                           "required (with \"row\") unless the widget gives a "
                           "\"rect\": the 0-based grid column");
            d->col = (int32_t)x;
            x = 1;
            pathf(gp, sizeof gp, "%s.rowspan", p);
            if (f_int(&w, "rowspan", 1, 4095, &x) < 0)
                return rej(err, APP_EINVAL, gp, "must be a positive integer");
            d->rowspan = (int32_t)x;
            x = 1;
            pathf(gp, sizeof gp, "%s.colspan", p);
            if (f_int(&w, "colspan", 1, 4095, &x) < 0)
                return rej(err, APP_EINVAL, gp, "must be a positive integer");
            d->colspan = (int32_t)x;

            if (g_rows <= 0 || g_cols <= 0)
                return rej(err, APP_EINVAL, p,
                           "this widget is placed on a grid, so the document "
                           "needs \"grid\":{\"rows\":R,\"cols\":C} (or give the "
                           "widget a \"rect\")");
            if (d->row + d->rowspan > g_rows || d->col + d->colspan > g_cols)
                return rej(err, APP_EINVAL, p,
                           "row %d+%d / col %d+%d does not fit the %dx%d grid",
                           (int)d->row, (int)d->rowspan, (int)d->col,
                           (int)d->colspan, (int)g_rows, (int)g_cols);
        }
    }
    in->nwidget = (uint16_t)n;
    return APP_OK;
}

/* Resolve one selector to the widgets it names: a widget "name" or a "tag". */
static uint64_t select_mask(const app_inst_t *in, const char *sel) {
    uint64_t mask = 0;
    for (uint16_t i = 0; i < in->nwidget; i++) {
        if (in->wname[i][0] && a_eq(sel, in->wname[i])) mask |= (uint64_t)1 << i;
        else if (descs[i].tag[0] && a_eq(sel, descs[i].tag)) mask |= (uint64_t)1 << i;
    }
    return mask;
}

/* Names and tags that exist, for the error a mistyped selector earns. */
static void put_selectors(const app_inst_t *in, char *dst, size_t cap) {
    size_t o = 0;
    if (!cap) return;
    dst[0] = '\0';
    for (uint16_t i = 0; i < in->nwidget; i++) {
        const char *n = in->wname[i][0] ? in->wname[i]
                      : (descs[i].tag[0] ? descs[i].tag : (const char *)0);
        if (!n) continue;
        int dup = 0;
        for (uint16_t k = 0; k < i && !dup; k++) {
            const char *m = in->wname[k][0] ? in->wname[k]
                          : (descs[k].tag[0] ? descs[k].tag : (const char *)0);
            if (m && a_eq(m, n)) dup = 1;
        }
        if (dup) continue;
        size_t need = a_len(n) + (o ? 2 : 0);
        if (o + need + 1 >= cap) break;
        if (o) { dst[o++] = ','; dst[o++] = ' '; }
        a_cpy(dst + o, cap - o, n);
        o += a_len(n);
    }
    if (!o) a_cpy(dst, cap, "(none: no widget has a \"name\" or a \"tag\")");
}

static int parse_selectors(app_inst_t *in, const json_value_t *sel,
                          const char *path, uint64_t *out, app_error_t *err) {
    char     one[APP_NAME_MAX];
    uint64_t mask = 0;
    size_t   n = 1;
    int      is_arr = (sel->type == JSON_ARRAY);

    if (is_arr) {
        n = json_count(sel);
        if (n == 0 || n > APP_MAX_SELECTORS)
            return rej(err, APP_EINVAL, path,
                       "must name between 1 and %d widgets", APP_MAX_SELECTORS);
    } else if (sel->type != JSON_STRING) {
        return rej(err, APP_EINVAL, path,
                   "must be a widget \"name\", a \"tag\", or an array of them");
    }

    for (size_t i = 0; i < n; i++) {
        json_value_t v = *sel;
        size_t       nl = 0;
        if (is_arr && json_at(sel, i, &v) != JSON_OK)
            return rej(err, APP_EINVAL, path, "element %d could not be read",
                       (int)i);
        if (v.type != JSON_STRING)
            return rej(err, APP_EINVAL, path,
                       "every selector must be a string (a widget name or tag)");
        if (json_str(&v, one, sizeof one, &nl) != JSON_OK)
            return rej(err, APP_EINVAL, path,
                       "a selector is longer than %d bytes", APP_NAME_MAX - 1);
        uint64_t m = select_mask(in, one);
        if (!m) {
            char known[160];
            put_selectors(in, known, sizeof known);
            return rej(err, APP_EINVAL, path,
                       "no widget has the name or tag \"%s\". Available: %s",
                       one, known);
        }
        mask |= m;
    }
    *out = mask;
    return APP_OK;
}

static int parse_handlers(app_inst_t *in, const json_value_t *root,
                          app_error_t *err) {
    json_value_t arr;
    int rc = json_get(root, "on", &arr);
    if (rc == JSON_ENOENT) return APP_OK;          /* a static picture is legal */
    if (rc != JSON_OK || arr.type != JSON_ARRAY)
        return rej(err, APP_EINVAL, "on",
                   "must be an array of handlers, e.g. [{\"click\":\"digit\","
                   "\"do\":[...]}]");

    size_t n = json_count(&arr);
    if (n > APP_MAX_HANDLERS)
        return rej(err, APP_ENOSPC, "on", "%d handlers; the limit is %d",
                   (int)n, APP_MAX_HANDLERS);

    for (size_t i = 0; i < n; i++) {
        json_value_t h, sel, doarr;
        char         p[72];
        static const char *const hkeys[] = { "click", "submit", "do", 0 };

        pathf(p, sizeof p, "on[%d]", (int)i);
        if (json_at(&arr, i, &h) != JSON_OK || h.type != JSON_OBJECT)
            return rej(err, APP_EINVAL, p, "must be a JSON object");
        if (check_keys(&h, p, hkeys, err) != APP_OK) return APP_EINVAL;

        int has_click  = json_get(&h, "click",  &sel) == JSON_OK;
        int has_submit = json_get(&h, "submit", &sel) == JSON_OK;
        if (has_click && has_submit)
            return rej(err, APP_EINVAL, p,
                       "a handler carries exactly one event: \"click\" or "
                       "\"submit\", not both");
        if (!has_click && !has_submit)
            return rej(err, APP_EINVAL, p,
                       "needs an event key: \"click\" (a button or field was "
                       "pressed) or \"submit\" (a line was typed into a field)");

        app_handler_t *out = &in->handler[in->nhandler];
        out->ev = has_click ? AE_CLICK : AE_SUBMIT;
        json_get(&h, has_click ? "click" : "submit", &sel);

        char sp[72];
        pathf(sp, sizeof sp, "%s.%s", p, has_click ? "click" : "submit");
        int src = parse_selectors(in, &sel, sp, &out->mask, err);
        if (src != APP_OK) return src;

        pathf(sp, sizeof sp, "%s.do", p);
        if (json_get(&h, "do", &doarr) != JSON_OK || doarr.type != JSON_ARRAY)
            return rej(err, APP_EINVAL, sp,
                       "required: an array of statements to run when this "
                       "happens");
        int brc = compile_block(in, &doarr, sp, 0, &out->first, &out->count, err);
        if (brc != APP_OK) return brc;

        in->nhandler++;
    }
    return APP_OK;
}

/* ====================================================================== */
/* running                                                                */
/* ====================================================================== */

static void note(app_inst_t *in, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(in->lasterr, sizeof in->lasterr, fmt, ap);
    va_end(ap);
}

/* Returns 1 when the block asked to stop. */
static int exec_block(app_inst_t *in, gui_window_t *win, const app_val_t *key,
                      uint16_t first, uint16_t count, int depth, int *budget) {
    if (depth > APP_MAX_IF_DEPTH) return 1;

    for (uint16_t i = 0; i < count; i++) {
        if (first + i >= in->nstmt) return 1;
        if (--*budget < 0) {
            stats.step_limits++;
            note(in, "a handler was cut off after %d statements", APP_MAX_STEPS);
            return 1;
        }
        const app_stmt_t *s = &in->stmt[first + i];
        stats.steps++;

        if (s->kind == AS_STOP) return 1;

        if (s->kind == AS_SET) {
            app_val_t v = app_expr_eval(in, s->expr, key, win);
            stats.sets++;
            if (s->tgt_kind == AT_VAR) {
                if (s->tgt < in->nvar) in->var[s->tgt] = v;
            } else if (win && s->tgt < win->nwidgets) {
                char         txt[APP_TEXT_MAX];
                gui_widget_t *w = &win->widgets[s->tgt];
                app_val_text(&v, txt, sizeof txt);
                if (v.kind == AV_ERR)
                    note(in, "an expression produced an error value, so \"%s\" "
                             "now reads \"error\"",
                         in->wname[s->tgt][0] ? in->wname[s->tgt] : "a widget");
                gui_set_text(w, txt);
                if (w->kind == GUI_FIELD) w->caret = (uint16_t)a_len(txt);
                gui_invalidate_widget(win->id, w);
            }
            continue;
        }

        /* AS_IF */
        app_val_t c = app_expr_eval(in, s->expr, key, win);
        int       t = app_val_truthy(&c);
        if (t < 0) {
            note(in, "a condition produced an error value and was treated as "
                     "false");
            t = 0;
        }
        if (t) {
            if (exec_block(in, win, key, s->then_first, s->then_count,
                           depth + 1, budget)) return 1;
        } else if (s->else_count) {
            if (exec_block(in, win, key, s->else_first, s->else_count,
                           depth + 1, budget)) return 1;
        }
    }
    return 0;
}

/* The one hook every model-authored app shares. Widget ids are index+1, so the
 * index a handler mask uses comes straight back out of the event. */
static int app_event(gui_window_t *win, gui_widget_t *w, const gui_event_t *ev) {
    app_inst_t *in = (app_inst_t *)(win ? win->user : (void *)0);
    if (!in || !in->used) return 0;

    if (ev->kind == GUI_EV_CLOSE) {
        in->used = 0;
        win->user = (void *)0;
        stats.closes++;
        return 0;                       /* an app document may not veto a close */
    }
    if (!w) return 0;
    if (ev->kind != GUI_EV_CLICK && ev->kind != GUI_EV_SUBMIT) return 0;

    int idx = gui_widget_index(win, w);
    if (idx < 0 || idx >= APP_MAX_WIDGETS) return 0;

    uint8_t   want = (ev->kind == GUI_EV_CLICK) ? AE_CLICK : AE_SUBMIT;
    app_val_t key  = app_val_str(w->text);
    int       ran  = 0;

    for (uint16_t i = 0; i < in->nhandler; i++) {
        const app_handler_t *h = &in->handler[i];
        if (h->ev != want) continue;
        if (!((h->mask >> idx) & 1)) continue;
        /* Each matching handler gets its own step budget, and a "stop" ends only
         * the handler it is in: a document that binds two handlers to one widget
         * should not have the first one silently suppress the second. */
        int budget = APP_MAX_STEPS;
        stats.events++;
        ran = 1;
        (void)exec_block(in, win, &key, h->first, h->count, 0, &budget);
    }
    return ran;
}

/* ====================================================================== */
/* instantiation                                                          */
/* ====================================================================== */

static gui_rect_t place(const wdesc_t *d, gui_rect_t area) {
    if (d->use_rect)
        return gui_rect(d->rect[0], d->rect[1], d->rect[2], d->rect[3]);
    return gui_grid(area, g_rows, g_cols, d->row, d->col,
                    d->rowspan, d->colspan, g_gap);
}

static int instantiate(app_inst_t *in, int32_t x, int32_t y,
                       int32_t w, int32_t h, app_error_t *err) {
    uint32_t id = gui_window_open(in->title, x, y, w, h, 0);
    if (!id)
        return rej(err, APP_EBUSY, "",
                   "the window manager has no free window (up to %d windows can "
                   "be open); close one and try again", GUI_MAX_WINDOWS);

    gui_window_t *win = gui_window(id);
    gui_rect_t    area = gui_inset(gui_client_area(win), g_pad, g_pad);

    for (uint16_t i = 0; i < in->nwidget; i++) {
        const wdesc_t *d = &descs[i];
        gui_rect_t     r = place(d, area);
        gui_widget_t  *wd = (gui_widget_t *)0;

        switch (d->kind) {
        case GUI_BUTTON: wd = gui_add_button(win, r, d->text, (uint32_t)(i + 1));
                         break;
        case GUI_FIELD:  wd = gui_add_field(win, r, d->text, (uint32_t)(i + 1),
                                            d->state);
                         break;
        case GUI_LABEL:  wd = gui_add_label(win, r, d->text, 0);
                         break;
        default:         wd = gui_add_panel(win, r, gui_theme()->client, d->state);
                         break;
        }
        if (!wd) {
            /* Cannot happen: the widget count was checked against
             * APP_MAX_WIDGETS while it was still data. Handled anyway, because
             * "cannot happen" is how a half-built window gets shipped. */
            (void)gui_window_close(id);
            in->used = 0;
            return rej(err, APP_ENOSPC, "widgets",
                       "widget %d could not be created", (int)i);
        }
        wd->id = (uint32_t)(i + 1);
        wd->state |= d->state;
        if (d->align != 0xFF) wd->align = d->align;
    }

    /* A WIDGET A HANDLER IS BOUND TO MUST BE CLICKABLE. Labels and panels are
     * scenery by default (widgets.h) and a read-only field is made scenery here,
     * so a document that hangs a handler on one would produce an app that simply
     * does not respond — the single most confusing failure a model could be left
     * to debug, because the document looks right. An explicit handler is a
     * statement of intent, so it wins over the default. */
    uint64_t bound = 0;
    for (uint16_t i = 0; i < in->nhandler; i++) bound |= in->handler[i].mask;
    for (uint16_t i = 0; i < in->nwidget && i < win->nwidgets; i++)
        if ((bound >> i) & 1)
            win->widgets[i].state &= (uint8_t)~GUI_W_NOHIT;

    in->win = id;
    win->user = in;
    gui_set_hook(id, app_event);
    gui_invalidate_window(id);
    return APP_OK;
}

/* ====================================================================== */
/* app_launch                                                             */
/* ====================================================================== */

int app_launch(const char *doc, size_t len, int32_t x, int32_t y,
               uint32_t *out_id, app_error_t *err) {
    json_value_t root;
    app_inst_t  *in;
    char         buf[APP_TEXT_MAX];
    int64_t      wv = 240, hv = 280;
    int          rc;

    err_clear(err);
    if (out_id) *out_id = 0;
    if (!doc) return rej(err, APP_EINVAL, "", "no document was supplied");
    if (len == 0) len = a_len(doc);

    if (!gui_attached())
        return rej(err, APP_ENOGUI, "",
                   "this machine has no window manager: there is no linear "
                   "framebuffer, so nothing can be drawn");
    if (len > APP_DOC_MAX)
        return rej(err, APP_ENOSPC, "",
                   "the document is %d bytes; the limit is %d. Shorten it (fewer "
                   "widgets, or shorter expressions)", (int)len, APP_DOC_MAX);
    if (json_parse(doc, len, &root) != JSON_OK)
        return rej(err, APP_EINVAL, "",
                   "not valid JSON. Send one object, with every key and string "
                   "in double quotes, no trailing commas, and nesting no deeper "
                   "than %d", JSON_MAX_DEPTH);
    if (root.type != JSON_OBJECT)
        return rej(err, APP_EINVAL, "",
                   "the document must be a JSON object with \"widgets\" in it, "
                   "not a bare array or value");
    {
        static const char *const rkeys[] = {
            "title", "width", "height", "grid", "vars", "widgets", "on", 0
        };
        rc = check_keys(&root, "", rkeys, err);
        if (rc != APP_OK) return rc;
    }

    in = inst_alloc();
    if (!in) {
        char ids[64];
        size_t o = 0;
        ids[0] = '\0';
        for (int i = 0; i < APP_MAX_INSTANCES; i++)
            if (pool[i].used && o + 12 < sizeof ids)
                o += (size_t)snprintf(ids + o, sizeof ids - o, "%s%u",
                                      o ? ", " : "", (unsigned)pool[i].win);
        return rej(err, APP_EBUSY, "",
                   "%d apps are already running (ids %s); close one first",
                   APP_MAX_INSTANCES, ids);
    }

    /* ---- title and size ---- */
    a_cpy(in->title, sizeof in->title, "App");
    rc = f_str(&root, "title", buf, sizeof buf);
    if (rc < 0) {
        in->used = 0;
        return rej(err, APP_EINVAL, "title",
                   "must be a string of at most %d bytes with no embedded NUL",
                   APP_TEXT_MAX - 1);
    }
    if (rc == 1) a_cpy(in->title, sizeof in->title, buf);

    if (f_int(&root, "width", 32, 4096, &wv) < 0) {
        in->used = 0;
        return rej(err, APP_EINVAL, "width",
                   "must be an integer number of pixels between 32 and 4096");
    }
    if (f_int(&root, "height", 32, 4096, &hv) < 0) {
        in->used = 0;
        return rej(err, APP_EINVAL, "height",
                   "must be an integer number of pixels between 32 and 4096");
    }

    /* ---- grid ---- */
    g_rows = g_cols = 0;
    g_gap  = 4;
    g_pad  = 6;
    json_value_t grid;
    if (json_get(&root, "grid", &grid) == JSON_OK && grid.type != JSON_NULL) {
        static const char *const gkeys[] = { "rows", "cols", "gap", "pad", 0 };
        int64_t v = 0;
        if (grid.type != JSON_OBJECT) {
            in->used = 0;
            return rej(err, APP_EINVAL, "grid",
                       "must be an object: {\"rows\":R,\"cols\":C,\"gap\":G,"
                       "\"pad\":P}");
        }
        if (check_keys(&grid, "grid", gkeys, err) != APP_OK) {
            in->used = 0;
            return APP_EINVAL;
        }
        if (f_int(&grid, "rows", 1, 64, &v) != 1) {
            in->used = 0;
            return rej(err, APP_EINVAL, "grid.rows",
                       "required with a grid: 1 to 64 rows");
        }
        g_rows = (int32_t)v;
        if (f_int(&grid, "cols", 1, 64, &v) != 1) {
            in->used = 0;
            return rej(err, APP_EINVAL, "grid.cols",
                       "required with a grid: 1 to 64 columns");
        }
        g_cols = (int32_t)v;
        v = 4;
        if (f_int(&grid, "gap", 0, 64, &v) < 0) {
            in->used = 0;
            return rej(err, APP_EINVAL, "grid.gap",
                       "must be 0 to 64 pixels between cells");
        }
        g_gap = (int32_t)v;
        v = 6;
        if (f_int(&grid, "pad", 0, 64, &v) < 0) {
            in->used = 0;
            return rej(err, APP_EINVAL, "grid.pad",
                       "must be 0 to 64 pixels of margin inside the window");
        }
        g_pad = (int32_t)v;
    }

    /* ---- vars, widgets, handlers: all still data ---- */
    rc = parse_vars(in, &root, err);
    if (rc != APP_OK) { in->used = 0; return rc; }
    rc = parse_widgets(in, &root, err);
    if (rc != APP_OK) { in->used = 0; return rc; }
    rc = parse_handlers(in, &root, err);
    if (rc != APP_OK) { in->used = 0; return rc; }

    /* ---- and only now, a window ---- */
    rc = instantiate(in, x, y, (int32_t)wv, (int32_t)hv, err);
    if (rc != APP_OK) { in->used = 0; return rc; }

    stats.launches++;
    if (out_id) *out_id = in->win;
    return APP_OK;
}

/* ====================================================================== */
/* lifecycle and queries                                                  */
/* ====================================================================== */

int app_close(uint32_t id) {
    app_inst_t *in = inst_of(id);
    if (!in) return APP_EINVAL;
    /* gui_window_close() delivers GUI_EV_CLOSE, and app_event() releases the
     * slot from there — one teardown path, however the window was closed. */
    if (gui_window_close(id) != 0) {
        in->used = 0;
        stats.closes++;
    }
    return APP_OK;
}

void app_close_all(void) {
    for (int i = 0; i < APP_MAX_INSTANCES; i++)
        if (pool[i].used) app_close(pool[i].win);
}

int app_count(void) {
    int n = 0;
    for (int i = 0; i < APP_MAX_INSTANCES; i++) if (pool[i].used) n++;
    return n;
}

uint32_t app_at(int index) {
    int n = 0;
    if (index < 0) return 0;
    for (int i = 0; i < APP_MAX_INSTANCES; i++)
        if (pool[i].used && n++ == index) return pool[i].win;
    return 0;
}

int app_is_app(uint32_t id) { return inst_of(id) != (app_inst_t *)0; }

const char *app_title(uint32_t id) {
    app_inst_t *in = inst_of(id);
    return in ? in->title : "";
}

int app_var_count(uint32_t id) {
    app_inst_t *in = inst_of(id);
    return in ? (int)in->nvar : 0;
}

int app_var_at(uint32_t id, int index, const char **name,
               char *value, size_t cap) {
    app_inst_t *in = inst_of(id);
    if (!in || index < 0 || index >= (int)in->nvar) return -1;
    if (name) *name = in->vname[index];
    if (value && cap) app_val_text(&in->var[index], value, cap);
    return 0;
}

const char *app_last_error(uint32_t id) {
    app_inst_t *in = inst_of(id);
    return in ? in->lasterr : "";
}

static const char *kindname(uint8_t k) {
    switch (k) {
    case GUI_PANEL:  return "panel";
    case GUI_LABEL:  return "label";
    case GUI_BUTTON: return "button";
    case GUI_FIELD:  return "field";
    default:         return "?";
    }
}

size_t app_describe(uint32_t id, char *dst, size_t cap) {
    app_inst_t   *in  = inst_of(id);
    gui_window_t *win = in ? gui_window(id) : (gui_window_t *)0;
    size_t        need = 0;

    if (dst && cap) dst[0] = '\0';
    if (!in || !win) return 0;

    for (uint16_t i = 0; i < win->nwidgets && i < in->nwidget; i++) {
        gui_widget_t *w = &win->widgets[i];
        char          line[160];
        int           n = snprintf(line, sizeof line,
                                   "  %s id=%u%s%s text=\"%s\"%s\n",
                                   kindname(w->kind), (unsigned)w->id,
                                   in->wname[i][0] ? " name=" : "",
                                   in->wname[i][0] ? in->wname[i] : "",
                                   w->text,
                                   (w->state & GUI_W_NOHIT) ? " not-clickable"
                                                            : "");
        if (n < 0) continue;
        if (dst && need + (size_t)n < cap) a_cpy(dst + need, cap - need, line);
        need += (size_t)n;
    }
    return need;
}

/* ====================================================================== */
/* the shipped example                                                    */
/* ====================================================================== */

const char *app_example_calculator(void) { return APP_CALCULATOR_JSON; }
