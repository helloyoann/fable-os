/* chat.c — the agentic turn loop. See chat.h for the contract and for why the
 * conversation history is shaped the way it is.
 *
 * Everything here is fixed storage: one history arena, one request buffer, one
 * response buffer, one staging buffer for tool_result blocks. Nothing is
 * allocated per turn, so a turn cannot fail halfway through for want of memory
 * and the worst case is knowable by reading the constants in chat.h.
 *
 * The file knows nothing about lwIP, TLS, the e1000 or the console device — it
 * talks to model.h's transport seam and kernel.h's kputs, which is what lets
 * tests/host/test_chat.c compile it natively and drive the whole loop against
 * net/model_mock.c.
 */

#include "chat.h"
#include "kernel.h"
#include "json.h"
#include "tool.h"
#include "trace.h"

/* ====================================================================== */
/* what the machine tells the model it is                                  */
/* ====================================================================== */

static const char SYSTEM_PROMPT[] =
    "You are the resident intelligence of talk-os, a bare-metal x86-64 kernel. "
    "You are not an assistant running on the machine: you ARE its user "
    "interface. There is no shell, no commands and no other way for the "
    "operator to drive this computer.\n"
    "Use the tools to inspect and change real kernel state. Never claim an "
    "action you did not perform with a tool, and never describe what a tool "
    "would do instead of calling it.\n"
    "The kernel prints its own [bracketed] trace line for every tool call, so "
    "the operator can already see exactly what happened - do not repeat the "
    "mechanics back to them. Answer in one or two plain sentences. If nothing "
    "you have can do what was asked, say so plainly.";

const char *chat_system_prompt(void) { return SYSTEM_PROMPT; }

/* ====================================================================== */
/* fixed storage                                                           */
/* ====================================================================== */

static const char ROLE_USER[]      = "user";
static const char ROLE_ASSISTANT[] = "assistant";

/* One remembered message. `off`/`len` locate its raw JSON *content* value in
 * the arena; `epoch` is the operator turn it belongs to, which is the unit of
 * eviction (see chat.h). */
typedef struct {
    uint32_t    off;
    uint32_t    len;
    uint16_t    epoch;
    const char *role;
} hist_msg_t;

static char       hist_arena[CHAT_HISTORY_BYTES];
static hist_msg_t hist[CHAT_HISTORY_MSGS];
static size_t     hist_n;        /* messages held                    */
static size_t     hist_used;     /* arena bytes held (contiguous)    */
static uint16_t   hist_epoch;    /* the turn currently being run     */

static char   req_buf[CHAT_REQ_BYTES];
static char   resp_buf[CHAT_RESP_BYTES];
static char   tools_buf[CHAT_TOOLS_BYTES];
static size_t tools_len;
static int    tools_ready;

/* The user turn that carries this round's tool_result blocks, assembled before
 * it is copied into the history arena. */
static char blocks_buf[8192];

/* One tool's answer, and one decoded text block. */
static char result_buf[CHAT_TOOL_RESULT_CAP];
static char text_buf[8192];

static model_transport_t *transport;

static unsigned max_rounds = CHAT_MAX_ROUNDS;
static unsigned stat_rounds;
static unsigned stat_tool_calls;
static unsigned stat_evictions;

/* ====================================================================== */
/* history                                                                 */
/* ====================================================================== */

void chat_reset(void) {
    hist_n     = 0;
    hist_used  = 0;
    hist_epoch = 0;
}

static void tools_load(void);

void chat_init(model_transport_t *t) {
    transport = t;
    chat_reset();
    stat_rounds = stat_tool_calls = stat_evictions = 0;
    max_rounds  = CHAT_MAX_ROUNDS;
    tools_ready = 0;
    /* Assemble the schema now rather than on the first sentence: its size is a
     * fixed property of this build, and the operator should learn at boot — not
     * mid-turn — if the machine's own syscall surface does not fit. */
    tools_load();
}

size_t   chat_history_messages(void) { return hist_n; }
size_t   chat_history_bytes(void)    { return hist_used; }
unsigned chat_last_rounds(void)      { return stat_rounds; }
unsigned chat_last_tool_calls(void)  { return stat_tool_calls; }
unsigned chat_evictions(void)        { return stat_evictions; }
size_t   chat_tools_bytes(void)      { return tools_len; }

void chat_set_max_rounds(unsigned n) {
    max_rounds = (n == 0 || n > CHAT_MAX_ROUNDS) ? CHAT_MAX_ROUNDS : n;
}

/* Drop every message of the oldest epoch present and compact the arena.
 * Returns 1 if anything was dropped. The in-flight epoch is never touched:
 * a turn is only valid as a whole, and half of one is worse than none. */
static int hist_drop_oldest_epoch(void) {
    if (hist_n == 0) return 0;

    uint16_t victim = hist[0].epoch;
    if (victim == hist_epoch) return 0;      /* only the in-flight turn is left */

    size_t k = 0;
    while (k < hist_n && hist[k].epoch == victim) k++;

    size_t bytes = hist[k - 1].off + hist[k - 1].len;   /* entries are in order */
    memmove(hist_arena, hist_arena + bytes, hist_used - bytes);
    hist_used -= bytes;

    for (size_t i = k; i < hist_n; i++) {
        hist[i - k]      = hist[i];
        hist[i - k].off -= (uint32_t)bytes;
    }
    hist_n -= k;
    stat_evictions++;
    return 1;
}

/* Make room for `need` bytes of content, evicting whole old exchanges as
 * required. Returns where to write, or NULL when even an empty history cannot
 * hold this turn (CHAT_EHISTORY territory). */
static char *hist_reserve(size_t need) {
    for (;;) {
        if (hist_n < CHAT_HISTORY_MSGS && hist_used + need <= CHAT_HISTORY_BYTES)
            return hist_arena + hist_used;
        if (!hist_drop_oldest_epoch()) return (char *)0;
    }
}

static void hist_commit(const char *role, size_t len) {
    hist[hist_n].off   = (uint32_t)hist_used;
    hist[hist_n].len   = (uint32_t)len;
    hist[hist_n].epoch = hist_epoch;
    hist[hist_n].role  = role;
    hist_used += len;
    hist_n++;
}

/* Append a raw JSON content value (an array of blocks, typically). */
static int hist_append_raw(const char *role, const char *json, size_t len) {
    char *dst = hist_reserve(len);
    if (!dst) return CHAT_EHISTORY;
    memcpy(dst, json, len);
    hist_commit(role, len);
    return CHAT_OK;
}

/* Append plain text as a JSON string, escaped on the way in. The escaped size
 * is measured first (json_escape with a zero capacity is a pure length query),
 * so no scratch buffer is needed and an over-long sentence is refused before
 * anything is written. */
static int hist_append_text(const char *role, const char *text) {
    size_t need = json_escape((char *)0, 0, text) + 2;   /* + the two quotes */

    /* One byte over: the writer keeps its buffer NUL-terminated, and that NUL
     * lives past the content the history actually keeps. */
    char *dst = hist_reserve(need + 1);
    if (!dst) return CHAT_EHISTORY;

    json_writer_t w;
    json_writer_init(&w, dst, need + 1);
    json_put_string(&w, text);
    if (!json_writer_ok(&w)) return CHAT_EHISTORY;

    hist_commit(role, json_writer_len(&w));
    return CHAT_OK;
}

/* Undo everything the in-flight turn appended. A turn that did not reach a
 * final assistant message must leave no trace in the history, or the next
 * request would carry a dangling tool_use, or two user turns in a row. */
static void hist_rollback_epoch(void) {
    while (hist_n && hist[hist_n - 1].epoch == hist_epoch) {
        hist_n--;
        hist_used = hist[hist_n].off;
    }
}

/* ====================================================================== */
/* request assembly                                                        */
/* ====================================================================== */

static void tools_load(void) {
    if (tools_ready) return;
    tools_ready = 1;

    size_t need = tools_build_schema(tools_buf, sizeof tools_buf);

    /* Validate rather than trust the length. A schema clipped mid-array is not
     * a short schema, it is a corrupt request body, and the model would be told
     * about tools whose contract it cannot read. Parsing is also the only
     * reliable overflow signal here: tools_build_schema() returns the bytes it
     * actually wrote, so a chunk the writer refused whole leaves a length that
     * looks like it fitted. (Worth fixing in core/tool.c; not this file's.) */
    json_value_t v;
    if (need >= sizeof tools_buf || json_parse(tools_buf, need, &v) != JSON_OK) {
        kprintf("[chat: the tool schema does not fit %u bytes - the model will "
                "be offered no tools at all]\n", (unsigned)sizeof tools_buf);
        tools_buf[0] = '\0';
        tools_len    = 0;
        return;
    }
    tools_len = need;
}

/* Build the next request from the whole history. On overflow, forget the
 * oldest exchange and try again — a shorter memory beats a dead machine. */
static int build_request(size_t *out_len) {
    static model_msg_t msgs[CHAT_HISTORY_MSGS];

    tools_load();

    for (;;) {
        for (size_t i = 0; i < hist_n; i++) {
            msgs[i].role        = hist[i].role;
            msgs[i].content     = hist_arena + hist[i].off;
            msgs[i].content_len = hist[i].len;
            msgs[i].raw         = 1;
        }

        model_request_t rq;
        rq.model         = (const char *)0;      /* model.c's compiled default */
        rq.max_tokens    = 1024;
        rq.system        = SYSTEM_PROMPT;
        rq.tools         = tools_len ? tools_buf : (const char *)0;
        rq.tools_len     = tools_len;
        rq.messages      = msgs;
        rq.message_count = hist_n;

        int n = model_build(req_buf, sizeof req_buf, &rq);
        if (n >= 0) { *out_len = (size_t)n; return CHAT_OK; }

        if (n == MODEL_ENOSPC && hist_drop_oldest_epoch()) {
            kputs("[chat: request over budget - forgot the oldest exchange]\n");
            continue;
        }
        if (n == MODEL_ENOSPC) {
            kputs("[chat: this turn alone does not fit one request]\n");
            return CHAT_ENOSPC;
        }
        kputs("[chat: could not build a valid request]\n");
        return CHAT_EINVAL;
    }
}

/* ====================================================================== */
/* printing                                                                */
/* ====================================================================== */

/* The model's voice: plain prose, no prefix. Always ends on a fresh line so a
 * kernel trace line never starts mid-sentence.
 *
 * FORGERY RESISTANCE — this is the model's largest uncontrolled output channel.
 *   trace.c escapes brackets and control bytes in the argument and op fields of
 *   a trace line, so a hostile *path* cannot fabricate kernel output. But the
 *   reply text printed here is entirely model-chosen, and a bare kputs() of it
 *   would let the model simply TYPE
 *
 *       [vfs_write /etc/shadow bytes=9999 -> ok]
 *
 *   producing bytes indistinguishable from genuine ground truth. That would
 *   defeat the whole point of the trace mechanism, since the operator has no
 *   `ls` with which to check.
 *
 *   The invariant this function enforces is the cheapest one that cannot be
 *   talked around: A KERNEL TRACE LINE IS A '[' IN COLUMN ZERO. Model prose is
 *   never allowed to put one there — a line that would start with '[' gets a
 *   single leading space. Prose is otherwise untouched, so the model can still
 *   write about brackets, quote a trace line back, or use them mid-sentence;
 *   it just cannot occupy the one position that means "the kernel says so".
 */
static void print_model_prose(const char *s) {
    int at_line_start = 1;
    for (size_t i = 0; s[i]; i++) {
        char c = s[i];
        if (at_line_start && c == '[') kputc(' ');
        kputc(c);
        at_line_start = (c == '\n');
    }
}

static void print_text_block(const json_value_t *v) {
    size_t n  = 0;
    int    rc = json_str(v, text_buf, sizeof text_buf, &n);
    if (rc != JSON_OK && rc != JSON_ENOSPC) return;
    if (n == 0) return;

    print_model_prose(text_buf);
    if (rc == JSON_ENOSPC) kputs("\n[chat: reply clipped to the print buffer]");
    if (text_buf[n - 1] != '\n') kputc('\n');
}

/* Whatever the API said when it did not say 200. The operator has no other way
 * to find out why the machine will not act. */
static void print_http_error(const model_response_t *r) {
    kprintf("[model %s]\n", r->status_line[0] ? r->status_line : "http error");

    json_value_t root, err, msg;
    if (json_parse(r->body, r->body_len, &root) == JSON_OK &&
        json_get(&root, "error", &err) == JSON_OK &&
        json_get(&err, "message", &msg) == JSON_OK &&
        json_str(&msg, text_buf, sizeof text_buf, (size_t *)0) == JSON_OK) {
        kprintf("[model said: %s]\n", text_buf);
        return;
    }
    if (r->body_len) {
        size_t n = r->body_len < 240 ? r->body_len : 240;
        kputs("[model said: ");
        for (size_t i = 0; i < n; i++) kputc(r->body[i]);
        kputs("]\n");
    }
}

/* ====================================================================== */
/* one round of tool calls                                                 */
/* ====================================================================== */

/* Append one tool_result block to the staging writer. The result text is
 * clipped to whatever escaped room is left rather than being allowed to
 * overflow: a tool_use with no matching result invalidates the whole
 * conversation, so a short answer is always better than a missing one. */
static void put_tool_result(json_writer_t *w, int first, const char *id,
                            const char *text, int is_error) {
    static const char MARK[] = "...[clipped]";

    if (!first) json_put_char(w, ',');
    json_put(w, "{\"type\":\"tool_result\",\"tool_use_id\":");
    json_put_string(w, id);
    json_put(w, ",\"content\":");

    /* Room left for the escaped text, keeping back this block's own tail and
     * the array's closing bracket. */
    size_t used = json_writer_len(w);
    size_t room = (w->cap > used + 64) ? w->cap - used - 64 : 0;

    if (json_escape((char *)0, 0, text) <= room) {
        json_put_string(w, text);
    } else {
        /* Escaping expands a byte to at most six (""), so room/6 source
         * bytes always fit. Conservative beats clever here. */
        char   clip[CHAT_TOOL_RESULT_CAP];
        size_t limit = sizeof clip - sizeof MARK;
        size_t keep  = room / 6;
        size_t tlen  = strlen(text);

        if (keep > tlen)  keep = tlen;
        if (keep > limit) keep = limit;
        memcpy(clip, text, keep);
        memcpy(clip + keep, MARK, sizeof MARK);      /* includes the NUL */
        json_put_string(w, clip);
    }

    if (is_error) json_put(w, ",\"is_error\":true");
    json_put_char(w, '}');
}

/* ====================================================================== */
/* the turn                                                                */
/* ====================================================================== */

static int abort_turn(int rc) {
    hist_rollback_epoch();
    return rc;
}

int chat_ask(const char *sentence) {
    if (!sentence) return CHAT_EINVAL;

    stat_rounds     = 0;
    stat_tool_calls = 0;

    if (!transport) {
        kputs("[no link to the model - this machine cannot act]\n");
        return CHAT_ETRANSPORT;
    }

    hist_epoch++;
    if (hist_append_text(ROLE_USER, sentence) != CHAT_OK) {
        kputs("[chat: that is longer than the whole conversation buffer - "
              "nothing was sent]\n");
        return abort_turn(CHAT_EHISTORY);
    }

    for (;;) {
        if (stat_rounds >= max_rounds) {
            kprintf("[chat: stopped after %u tool rounds without an answer - "
                    "the turn was abandoned]\n", max_rounds);
            return abort_turn(CHAT_ELIMIT);
        }

        size_t req_len = 0;
        int    rc      = build_request(&req_len);
        if (rc != CHAT_OK) return abort_turn(rc);

        model_response_t r;
        rc = model_send(transport, req_buf, req_len, resp_buf, sizeof resp_buf, &r);
        stat_rounds++;
        if (rc != MODEL_OK) {
            const char *why = model_last_error(transport);
            kprintf("[model unreachable: %s]\n",
                    (why && why[0]) ? why : model_strerror(rc));
            return abort_turn(CHAT_ETRANSPORT);
        }
        if (r.http_status != 200) {
            print_http_error(&r);
            return abort_turn(CHAT_EHTTP);
        }
        if (r.truncated) {
            kprintf("[chat: reply exceeded %u bytes and was cut off]\n",
                    (unsigned)sizeof resp_buf);
            return abort_turn(CHAT_EPROTO);
        }

        json_value_t root, content;
        if (json_parse(r.body, r.body_len, &root) != JSON_OK) {
            kputs("[chat: the reply was not valid JSON]\n");
            return abort_turn(CHAT_EPROTO);
        }
        if (json_msg_content(&root, &content) != JSON_OK) {
            kputs("[chat: the reply carried no content blocks]\n");
            return abort_turn(CHAT_EPROTO);
        }

        size_t nblocks = json_count(&content);

        /* --- pass 1: how many tool calls, and are they all answerable? -----
         * A tool_use with no usable id can never be answered, and an assistant
         * turn containing one can never be echoed back. Find that out before
         * running anything: it is the difference between a turn that fails and
         * a machine that has acted but cannot say so. */
        size_t ncalls = 0;
        for (size_t i = 0; i < nblocks; i++) {
            json_value_t blk, type, id;
            if (json_at(&content, i, &blk) != JSON_OK)     continue;
            if (json_get(&blk, "type", &type) != JSON_OK)  continue;
            if (!json_str_eq(&type, "tool_use"))           continue;

            char idbuf[TOOL_ID_MAX];
            if (json_get(&blk, "id", &id) != JSON_OK ||
                json_str(&id, idbuf, sizeof idbuf, (size_t *)0) != JSON_OK ||
                idbuf[0] == '\0') {
                kputs("[chat: the model asked for a tool with no usable id - "
                      "nothing was run]\n");
                return abort_turn(CHAT_EPROTO);
            }
            ncalls++;
        }

        /* --- the assistant turn goes into the history before anything runs,
         * so a full history can still cancel the round cleanly. -------------*/
        if (ncalls) {
            if (hist_append_raw(ROLE_ASSISTANT, content.start, content.len)
                    != CHAT_OK) {
                kputs("[chat: no room left to remember this turn - "
                      "nothing was run]\n");
                return abort_turn(CHAT_EHISTORY);
            }
        }

        /* --- pass 2: print prose and dispatch, in the order they arrived --- */
        json_writer_t w;
        json_writer_init(&w, blocks_buf, sizeof blocks_buf);
        json_put_char(&w, '[');

        size_t done = 0;
        for (size_t i = 0; i < nblocks; i++) {
            json_value_t blk, type;
            if (json_at(&content, i, &blk) != JSON_OK)    continue;
            if (json_get(&blk, "type", &type) != JSON_OK) continue;

            if (json_str_eq(&type, "text")) {
                json_value_t txt;
                if (json_get(&blk, "text", &txt) == JSON_OK)
                    print_text_block(&txt);
                continue;
            }
            if (!json_str_eq(&type, "tool_use")) continue;

            char idbuf[TOOL_ID_MAX]   = "";
            char namebuf[TOOL_NAME_MAX] = "";
            json_value_t id, name, input;

            json_get(&blk, "id", &id);
            json_str(&id, idbuf, sizeof idbuf, (size_t *)0);
            /* A missing or over-long name is not fatal: tool_dispatch turns it
             * into a legible error the model can recover from next round. */
            if (json_get(&blk, "name", &name) != JSON_OK ||
                json_str(&name, namebuf, sizeof namebuf, (size_t *)0) != JSON_OK)
                namebuf[0] = '\0';

            if (done >= CHAT_MAX_TOOL_CALLS) {
                /* Refuse, but still answer: every tool_use needs a result. */
                put_tool_result(&w, done == 0, idbuf,
                                "refused: too many tool calls in one turn", 1);
                done++;
                continue;
            }

            const char *in_ptr = "{}";
            size_t      in_len = 2;
            if (json_get(&blk, "input", &input) == JSON_OK) {
                in_ptr = input.start;
                in_len = input.len;
            }

            tool_call_t call;
            call.id        = idbuf;
            call.name      = namebuf[0] ? namebuf : (const char *)0;
            call.input     = in_ptr;
            call.input_len = in_len;

            tool_result_t res;
            tool_result_init(&res, result_buf, sizeof result_buf);
            tool_dispatch(&call, &res);          /* emits the kernel trace line */
            stat_tool_calls++;

            put_tool_result(&w, done == 0, idbuf, result_buf, res.is_error);
            done++;
        }

        if (ncalls == 0) {
            /* A plain answer: remember it and hand the machine back. */
            if (hist_append_raw(ROLE_ASSISTANT, content.start, content.len)
                    != CHAT_OK) {
                kputs("[chat: answered, but there was no room to remember it]\n");
                hist_rollback_epoch();
            }
            if (nblocks == 0) kputs("[chat: the model said nothing]\n");
            return CHAT_OK;
        }

        json_put_char(&w, ']');
        if (!json_writer_ok(&w)) {
            kprintf("[chat: %u tool results do not fit one turn - "
                    "conversation abandoned]\n", (unsigned)done);
            return abort_turn(CHAT_ENOSPC);
        }

        if (hist_append_raw(ROLE_USER, blocks_buf, json_writer_len(&w))
                != CHAT_OK) {
            kprintf("[chat: no room to record %u tool results - "
                    "conversation abandoned]\n", (unsigned)done);
            return abort_turn(CHAT_EHISTORY);
        }
    }
}
