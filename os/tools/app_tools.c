/* app_tools.c — the model builds an application and runs it.
 *
 * PURPOSE
 *   "hey I want a calculator" ends here. gui_open (tools/gui_tools.c) can start
 *   an app that a human compiled into the kernel; this tool lets the model AUTHOR
 *   one — write a document, launch it, look at what it did, fix it, launch it
 *   again — with no C, no compiler and no reboot.
 *
 * ============================================================================
 * ONE TOOL, WITH AN `action` — AND THE REASON IS A HARD LIMIT, NOT TASTE
 * ============================================================================
 *   The assembled tool schema is a shared, EXHAUSTED resource. chat.h sets
 *   CHAT_TOOLS_BYTES to 32768 and refuses the whole array if it does not fit,
 *   which offers the model NO TOOLS AT ALL. Measured on this tree, with this file
 *   removed: 44 tools, 32330 bytes. That is 437 bytes for everything anyone adds
 *   next, and this file is what came next.
 *
 *   So launch / list / state / close / format are ONE tool with an `action`, the
 *   schema states no enum (the description lists the actions instead), x and y
 *   are accepted but documented in action=format's OUTPUT rather than in the
 *   schema, and the entire format specification — which is the thing a model most
 *   needs — is tool output too, because a result costs nothing from the schema
 *   budget. Cost of this file, computed by the same formula core/tool.c uses:
 *   432 bytes. It fits with FIVE BYTES to spare.
 *
 *   That is not a healthy state and it is not a state this file can fix from
 *   inside its own ownership: raising CHAT_TOOLS_BYTES also raises CHAT_REQ_BYTES
 *   and has to agree with net/net.c's HTTP framing buffer. It is reported rather
 *   than papered over, and tests/host/test_app.c pins the number so that a future
 *   edit to the description cannot silently push the machine over the edge.
 *
 * THE FEEDBACK LOOP IS THE PRODUCT
 *   A model gets a document wrong on the first try — a mistyped key, a widget
 *   name it never declared, an unbalanced bracket in an expression. What makes
 *   the second try work is being told exactly what was wrong, so every rejection
 *   reports the JSON path, the reason, the byte offset inside an expression, and
 *   the names or values that WOULD have been accepted. It also says plainly that
 *   nothing was launched, because a model that believes a half-built window
 *   exists will try to repair it instead of resending it.
 *
 * UNTRUSTED INPUT
 *   `call->input` is model output and is read only through json.h. The document
 *   is handed to app_launch() as a raw span (or decoded out of a JSON string into
 *   one fixed buffer), where it is validated in full before any window exists.
 *   Nothing here indexes memory with a number from the model: ids are looked up
 *   in the instance pool, and coordinates are range-checked here and clamped
 *   again by the WM.
 *
 * EVERY CALL EMITS EXACTLY ONE TRACE LINE
 *   Success and failure both, through trace.h, from real return values after the
 *   call happened — so the transcript shows the id that was really opened and the
 *   path that was really refused, not what the model said it did.
 */

#include "tool.h"
#include "trace.h"
#include "json.h"
#include "app.h"
#include "gui.h"
#include "widgets.h"

#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#include <stdio.h>       /* vsnprintf, via port/stdio.h when freestanding */

/* On the host, Mach-O rejects a bare section name, so REGISTER_TOOL cannot
 * expand. Export a named pointer instead; tests/host/test_app.c stands in for
 * the linker. Kernel builds are untouched. */
#ifdef TALKOS_HOSTTEST
#undef  REGISTER_TOOL
#define REGISTER_TOOL(var) const tool_t *const talkos_hosttool_##var = &(var)
#endif

#define COORD_LIMIT  100000

/* Keep this much result budget free so a truncated listing can always say so. */
#define TAIL_RESERVE 120

/* ====================================================================== */
/* helpers (the tools/gui_tools.c + mem_tools.c convention)                */
/* ====================================================================== */

static void argf(char *dst, size_t cap, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(dst, cap, fmt, ap);
    va_end(ap);
}

/* `args` is a finished string, never a format: trace_err renders it with "%s" so
 * a value that came from the model can never be read as a conversion. */
static int fail(tool_result_t *r, int err, const char *args,
                const char *fmt, ...) {
    char    msg[420];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);

    r->is_error  = 1;
    r->len       = 0;
    r->truncated = 0;
    if (r->buf && r->cap) r->buf[0] = '\0';
    tool_result_printf(r, "error: %s", msg);
    trace_err(err, "app", "%s", args ? args : "");
    return err;
}

static int parse_obj(const tool_call_t *c, json_value_t *obj) {
    const char *p = "{}";
    size_t      n = 2;
    if (c && c->input && c->input_len) { p = c->input; n = c->input_len; }
    if (json_parse(p, n, obj) != JSON_OK) return TOOL_EINVAL;
    if (obj->type != JSON_OBJECT) return TOOL_EINVAL;
    return TOOL_OK;
}

/* 1 = present and valid, 0 = absent/null, -1 = wrong type, -2 = out of range. */
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

/* -3 additionally means "the decoded string contains an embedded NUL". A JSON
 * string may legally carry one and every comparison below is on a C string. */
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

static int streq(const char *a, const char *b) {
    size_t i = 0;
    if (!a || !b) return 0;
    for (; a[i] && a[i] == b[i]; i++) if (i > 64) return 0;
    return a[i] == '\0' && b[i] == '\0';
}

static void put_running(tool_result_t *r) {
    int n = app_count();
    if (!n) { tool_result_printf(r, "no apps are running"); return; }
    tool_result_printf(r, "running app ids:");
    for (int i = 0; i < n; i++)
        tool_result_printf(r, " %u", (unsigned)app_at(i));
}

static void put_widgets(tool_result_t *r, uint32_t id, const char *indent) {
    char   desc[560];
    size_t want = app_describe(id, desc, sizeof desc);
    tool_result_printf(r, "%s", desc);
    if (want >= sizeof desc)
        tool_result_printf(r, "%s[widget list clipped]\n", indent);
}

static void put_vars(tool_result_t *r, uint32_t id) {
    int nv = app_var_count(id);
    if (!nv) return;
    tool_result_printf(r, "vars:");
    for (int i = 0; i < nv && r->len + TAIL_RESERVE + 40 < r->cap; i++) {
        const char *name = "";
        char        val[APP_TEXT_MAX];
        if (app_var_at(id, i, &name, val, sizeof val) == 0)
            tool_result_printf(r, " %s=%s", name, val);
    }
    tool_result_printf(r, "\n");
}

/* ====================================================================== */
/* action=format — the specification, as OUTPUT                            */
/* ====================================================================== */

/* This is where the format is documented, because a tool result costs nothing
 * from the schema budget (see the header). It is a real, launchable document
 * rather than a grammar, because that is what a model can copy and adapt.
 *
 * IT MUST FIT CHAT_TOOL_RESULT_CAP (1024 bytes). The turn loop clips anything
 * longer, and a specification that stops mid-sentence is worse than a terser one
 * that finishes: the model would be missing exactly the part it did not know was
 * missing. tests/host/test_app.c asserts the length, so an edit here cannot
 * quietly lose the last paragraph. What did not fit is carried by the rejection
 * messages instead, which is where a model reads it anyway — a wrong widget key
 * is answered with the list of accepted keys, an unknown function with the list
 * of functions, and every bound with its number, so none of that has to be spent
 * here. */
static void put_format(tool_result_t *r) {
    tool_result_printf(r,
        "Launch this document, then adapt it:\n"
        "{\"title\":\"Counter\",\"width\":180,\"height\":120,"
        "\"grid\":{\"rows\":2,\"cols\":1},\"vars\":{\"n\":0},"
        "\"widgets\":["
        "{\"kind\":\"field\",\"name\":\"out\",\"text\":\"0\","
        "\"readonly\":true,\"row\":0,\"col\":0},"
        "{\"kind\":\"button\",\"text\":\"+1\",\"name\":\"up\",\"row\":1,"
        "\"col\":0}],"
        "\"on\":[{\"click\":\"up\",\"do\":["
        "{\"set\":\"n\",\"to\":\"n + 1\"},"
        "{\"set\":\"out\",\"to\":\"text(n)\"}]}]}\n");
    tool_result_printf(r,
        "kind: button label field panel; place by row/col (+rowspan/colspan) or "
        "rect:[x,y,w,h]. name is unique and is how handlers and expressions reach "
        "a widget; tag is shared, so one handler serves ten keys. Events: click, "
        "submit. Statements: {set,to} {if,then,else} {stop:true}; no loops.\n"
        "EXPRESSIONS are strings: numbers (6dp), 'quoted text', vars, a widget "
        "name (its text), key (text of the widget that fired), + - * / %% == != "
        "< <= > >= && || !, num text cat len digits has iserr abs min max. num(x) "
        "reads a widget as a number, text(x) writes one back. 1/0 and overflow "
        "give an error value: text() of it is \"error\" and iserr() finds it. "
        "launch takes x,y too.\n");
}

/* ====================================================================== */
/* action=launch                                                          */
/* ====================================================================== */

/* A document sent as a JSON *string* is decoded into this. One launch at a time
 * (one thread, and an app cannot launch an app), and 8 KiB is too much for the
 * 64 KiB kernel stack to spend on a maybe. */
static char doc_buf[APP_DOC_MAX + 1];

static int do_launch(const json_value_t *in, tool_result_t *r) {
    json_value_t doc;

    int64_t x = 0, y = 0;
    int has_x = f_int(in, "x", -COORD_LIMIT, COORD_LIMIT, &x);
    int has_y = f_int(in, "y", -COORD_LIMIT, COORD_LIMIT, &y);
    if (has_x < 0 || has_y < 0)
        return fail(r, TOOL_EINVAL, "action=launch",
                    "\"x\" and \"y\" must be whole numbers of pixels within "
                    "+/-%d when present", COORD_LIMIT);

    if (json_get(in, "document", &doc) != JSON_OK)
        return fail(r, TOOL_EINVAL, "action=launch",
                    "\"document\" is required: the app document itself, as a "
                    "JSON object. Call action=format for the format and a worked "
                    "example");

    const char *text = (const char *)0;
    size_t      len  = 0;

    if (doc.type == JSON_OBJECT) {
        /* The preferred case: a nested object is already a bounded span of the
         * request, so it needs no copy and no decoding at all. */
        text = doc.start;
        len  = doc.len;
    } else if (doc.type == JSON_STRING) {
        /* Also accepted: the whole document escaped inside one JSON string. It
         * costs a decode and it is easy to get the escaping wrong, but refusing
         * it outright would cost a turn to explain. */
        size_t n = 0;
        int    rc = json_str(&doc, doc_buf, sizeof doc_buf, &n);
        if (rc == JSON_ENOSPC)
            return fail(r, TOOL_ENOSPC, "action=launch",
                        "the document is longer than %d bytes", APP_DOC_MAX);
        if (rc != JSON_OK)
            return fail(r, TOOL_EINVAL, "action=launch",
                        "\"document\" is a string but could not be decoded");
        text = doc_buf;
        len  = n;
    } else {
        return fail(r, TOOL_EINVAL, "action=launch",
                    "\"document\" must be a JSON object (preferred), or a string "
                    "containing the document's JSON - not a number, an array or "
                    "a boolean");
    }

    uint32_t    id = 0;
    app_error_t e;
    int rc = app_launch(text, len,
                        has_x == 1 ? (int32_t)x : GUI_POS_AUTO,
                        has_y == 1 ? (int32_t)y : GUI_POS_AUTO,
                        &id, &e);
    if (rc != APP_OK) {
        char args[96];
        argf(args, sizeof args, "action=launch rejected at %s",
             e.path[0] ? e.path : "the document");
        r->is_error  = 1;
        r->len       = 0;
        r->truncated = 0;
        if (r->buf && r->cap) r->buf[0] = '\0';
        tool_result_printf(r, "error: the app was NOT launched - nothing was "
                              "created and there is nothing on screen to "
                              "repair.\n");
        if (e.path[0]) tool_result_printf(r, "where: %s\n", e.path);
        tool_result_printf(r, "problem: %s\n", e.msg);
        tool_result_printf(r, "Fix that one thing and call app action=launch "
                              "again with the whole corrected document.\n");
        trace_err(rc, "app", "%s", args);
        return rc == APP_ENOSPC ? TOOL_ENOSPC : TOOL_EINVAL;
    }

    gui_sync();                 /* on screen now, not on the next input poll */

    gui_window_t *w = gui_window(id);
    tool_result_printf(r,
        "launched \"%s\" as app id=%u (also its window id) at %d,%d %dx%d with "
        "%d widget(s), focused and on top. It is live: a real mouse can click it, "
        "and gui_click id=%u label=\"<text>\" presses a widget.\n",
        app_title(id), (unsigned)id, (int)w->frame.x, (int)w->frame.y,
        (int)w->frame.w, (int)w->frame.h, (int)w->nwidgets, (unsigned)id);
    put_vars(r, id);
    put_widgets(r, id, "");

    trace_ret((long)id, "app", "action=launch title=%s widgets=%d vars=%d "
                               "bytes=%d",
              app_title(id), (int)w->nwidgets, app_var_count(id), (int)len);
    return TOOL_OK;
}

/* ====================================================================== */
/* the tool                                                               */
/* ====================================================================== */

static int t_app(const tool_call_t *c, tool_result_t *r) {
    json_value_t in;
    char         act[16];

    if (parse_obj(c, &in) != TOOL_OK)
        return fail(r, TOOL_EINVAL, "",
                    "input must be a JSON object, e.g. {\"action\":\"format\"} "
                    "or {\"action\":\"launch\",\"document\":{...}}");

    int arc = f_str(&in, "action", act, sizeof act);
    if (arc <= 0)
        return fail(r, TOOL_EINVAL, "",
                    "\"action\" is required: format, launch, list, state or "
                    "close");

    /* format needs no window manager: a model may read the spec on a machine
     * that cannot draw, and then be told why launching is impossible. */
    if (streq(act, "format")) {
        put_format(r);
        trace_ok("app", "%s", "action=format");
        return TOOL_OK;
    }

    if (!gui_attached())
        return fail(r, TOOL_EINVAL, "",
                    "this machine has no window manager: there is no linear "
                    "framebuffer (the console is VGA text mode), so an app "
                    "cannot be drawn. The text console tools still work");

    if (streq(act, "launch")) return do_launch(&in, r);

    int64_t id64 = 0;
    int has_id = f_int(&in, "id", 1, 0x7FFFFFFF, &id64);
    if (has_id < 0)
        return fail(r, TOOL_EINVAL, "",
                    "\"id\" must be the integer id that launching returned");

    char args[64];
    argf(args, sizeof args, "action=%s id=%u", act, (unsigned)id64);

    if (streq(act, "list")) {
        int n = app_count();
        tool_result_printf(r,
            "%d of %d app slots in use. These are apps launched from a document; "
            "gui_list shows every window, including built-in ones.\n",
            n, APP_MAX_INSTANCES);
        for (int i = 0; i < n && r->len + TAIL_RESERVE + 80 < r->cap; i++) {
            uint32_t wid = app_at(i);
            tool_result_printf(r, "  id=%u \"%s\"%s%s\n", (unsigned)wid,
                               app_title(wid),
                               gui_window(wid) ? "" : " (window gone)",
                               gui_focused() == wid ? " focused" : "");
        }
        const app_stats_t *s = app_stats();
        tool_result_printf(r, "totals: %u launched, %u refused, %u closed, "
                              "%u handler run(s)\n",
                           (unsigned)s->launches, (unsigned)s->rejections,
                           (unsigned)s->closes, (unsigned)s->events);
        trace_ret((long)n, "app", "action=list running=%d", n);
        return TOOL_OK;
    }

    if (!streq(act, "state") && !streq(act, "close"))
        return fail(r, TOOL_EINVAL, args,
                    "unknown action \"%s\". Valid: format (the document format "
                    "and a worked example), launch, list, state, close", act);

    if (has_id != 1) {
        r->is_error = 1;
        tool_result_printf(r, "error: \"%s\" needs \"id\". ", act);
        put_running(r);
        tool_result_printf(r, "\n");
        trace_err(TOOL_EINVAL, "app", "action=%s id=?", act);
        return TOOL_EINVAL;
    }
    if (!app_is_app((uint32_t)id64)) {
        r->is_error = 1;
        tool_result_printf(r, "error: no app with id %u%s. ", (unsigned)id64,
                           gui_window((uint32_t)id64)
                               ? " (that window exists, but it was not launched "
                                 "from a document)" : "");
        put_running(r);
        tool_result_printf(r, "\n");
        trace_err(TOOL_ENOENT, "app", "%s", args);
        return TOOL_ENOENT;
    }

    if (streq(act, "state")) {
        tool_result_printf(r, "app id=%u \"%s\"%s\n", (unsigned)id64,
                           app_title((uint32_t)id64),
                           gui_focused() == (uint32_t)id64 ? " (focused)" : "");
        put_vars(r, (uint32_t)id64);
        put_widgets(r, (uint32_t)id64, "  ");
        const char *why = app_last_error((uint32_t)id64);
        if (why && why[0]) tool_result_printf(r, "note: %s\n", why);
        trace_ok("app", "%s", args);
        return TOOL_OK;
    }

    /* close — the title is captured first, because closing frees it. */
    char   title[GUI_TITLE_MAX];
    size_t i = 0;
    for (const char *t = app_title((uint32_t)id64);
         t[i] && i < sizeof title - 1; i++) title[i] = t[i];
    title[i] = '\0';

    int crc = app_close((uint32_t)id64);
    gui_sync();
    if (crc != APP_OK)
        return fail(r, TOOL_EINVAL, args, "app %u could not be closed",
                    (unsigned)id64);
    tool_result_printf(r,
        "closed app id=%u \"%s\". %d app(s) and %d window(s) remain.\n",
        (unsigned)id64, title, app_count(), gui_window_count());
    trace_ret((long)app_count(), "app", "action=close id=%u title=%s",
              (unsigned)id64, title);
    return TOOL_OK;
}

/* EVERY BYTE OF THE TWO STRINGS BELOW IS SPENT FROM A SHARED BUDGET THAT HAS 5
 * BYTES LEFT. Read the header before editing them, and re-run
 * tests/host/test_app.c's test_schema_cost(), which fails if this grows. */
static const tool_t app_tool = {
    .name = "app",
    .description =
        "Author and run a graphical app from a JSON document you write: this is "
        "how \"I want a calculator\" becomes a real, clickable window. action: "
        "format (the format and a worked example - read it first), launch (with "
        "document), list, state (with id), close.",
    .input_schema =
        "{\"type\":\"object\",\"properties\":{\"action\":{\"type\":\"string\"},"
        "\"document\":{\"type\":\"object\"},\"id\":{\"type\":\"integer\"}},"
        "\"required\":[\"action\"]}",
    .flags  = TOOL_MUTATES,
    .invoke = t_app,
};
REGISTER_TOOL(app_tool);
