/* chat.h — the agentic turn loop: one typed sentence -> real kernel actions.
 *
 * PURPOSE
 *   This is the machine's only interface. There is no shell, no command
 *   language, and there never will be: the operator types a sentence, and this
 *   module turns it into kernel work. It is the piece that joins the three
 *   contracts around it — tool.h (what the model may do), model.h (how bytes
 *   reach the model), json.h (how the protocol is read and written) — into a
 *   loop that keeps running until the machine has actually done the thing.
 *
 * THE LOOP
 *   One call to chat_ask() is one *turn*, and a turn is not one request. The
 *   model may answer with tool_use blocks instead of prose; those are real
 *   kernel calls, and their results have to go back so the model can carry on.
 *   So a turn is:
 *
 *       append the operator's sentence to the history
 *       repeat:
 *           build a request  = system prompt + tools[] + the whole history
 *           send it through the transport (model.h — never a named transport)
 *           print every text block                      <- the model's voice
 *           dispatch every tool_use block (tool.h)      <- the kernel acts,
 *                                                          and traces it
 *           if there were no tool_use blocks: the turn is done
 *           append the assistant turn verbatim, then a user turn carrying one
 *           tool_result per tool_use, keyed by tool_use_id, plus one kernel
 *           note telling the model how much of the round budget is left
 *       until CHAT_MAX_ROUNDS
 *
 *   The iteration cap is not a nicety. A model that keeps calling tools would
 *   otherwise own this machine forever; there is no scheduler to preempt it and
 *   no other console to kill it from.
 *
 * SOLVING, NOT ANSWERING — the properties the loop owes a working agent
 *   A cap alone makes the machine safe, not useful. Four things here are what
 *   make a multi-step job actually finish:
 *
 *     1. THE BUDGET IS VISIBLE. Every tool_result turn carries a kernel note
 *        ("tool round 3 of 16 used ..."). A model that cannot see the cap plans
 *        past it and gets cut off mid-job; one that can see it wraps up. On the
 *        last round the note says so in as many words, and the request is sent
 *        with tool calling switched off (tool_choice=none — NOT by dropping the
 *        schema, which the history still references), so the only continuation
 *        left is prose. That turns the cap from a silent death into a report.
 *     2. TRANSIENT FAILURE IS NOT FAILURE. A rate limit or a dropped TLS read
 *        used to throw away every completed action in the turn. Retryable
 *        outcomes (HTTP 429/503/529, timeouts, transport errors) are retried up
 *        to CHAT_SEND_RETRIES times per turn without spending a round, because
 *        the alternative is to discard real work that already happened.
 *     3. EVERY tool_use IS ANSWERED, ALWAYS. The API rejects a conversation in
 *        which a tool_use has no matching tool_result, so "the results did not
 *        fit" is not a truncation, it is the death of the whole exchange. The
 *        staging writer therefore gives each pending call a FAIR SHARE of the
 *        room left (see put_tool_result), and a round whose answers provably
 *        cannot fit is refused BEFORE any tool runs rather than after.
 *     4. THE KERNEL REMEMBERS WHAT IT RAN. chat_action_at() is a ring of the
 *        tool calls this machine actually dispatched, recorded by the loop from
 *        real dispatch results — not by the model, and not from the model's
 *        memory of them. It outlives history eviction and outlives an abandoned
 *        turn, which is what lets a model that was cut off pick the job back up
 *        instead of starting again. tools/agent_tools.c hands it to the model.
 *
 * CONVERSATION HISTORY — the bounded part
 *   Multi-turn tool use is unforgiving about shape. The assistant's tool_use
 *   turn must be echoed back *verbatim*, and it must be followed by a user turn
 *   whose tool_result blocks match it one-for-one by id. A history that loses
 *   half of such a pair is not "shorter", it is invalid, and the API rejects it.
 *
 *   Storage is therefore a single fixed arena of raw JSON message *contents*
 *   (CHAT_HISTORY_BYTES) plus a fixed index of at most CHAT_HISTORY_MSGS
 *   entries. Nothing grows; nothing is allocated per turn. Every entry carries
 *   the *epoch* — the operator turn — it belongs to, and three rules follow:
 *
 *     1. Eviction is whole epochs, oldest first. Dropping a complete exchange
 *        can never orphan a tool_result from its tool_use, and can never leave
 *        the history starting on an assistant message.
 *     2. The in-flight epoch is never evicted. If a single turn cannot fit on
 *        its own, that is CHAT_EHISTORY: the turn is abandoned and reported,
 *        not silently truncated into an invalid request.
 *     3. A turn that fails for *any* reason is rolled back in full. Only turns
 *        that ran to a final assistant message are remembered, so the stored
 *        history is always a valid, strictly alternating conversation.
 *
 *   The request buffer is a second, independent bound (CHAT_REQ_BYTES, sized to
 *   stay inside the TLS transport's own request framing buffer). If a request
 *   overflows it, the loop drops the oldest epoch and rebuilds rather than
 *   failing — so the machine degrades to a shorter memory instead of dying.
 *
 * WHAT THE OPERATOR SEES
 *   Prose is the model. [Brackets] are the kernel. Tool trace lines come from
 *   trace.h via tool_dispatch(), so they are emitted in C from real return
 *   values and cannot be forged by the model. This module's own bracket lines
 *   (transport failures, the iteration cap, history eviction) deliberately do
 *   NOT go through trace.h: trace_count() must stay an exact count of kernel
 *   actions the model caused, not of things the loop had to say.
 *
 * DEPENDENCIES
 *   model.h, tool.h, json.h, trace.h, kernel.h. No transport, no lwIP, no
 *   hardware — chat.c is compiled into the host test binary as-is and driven
 *   against net/model_mock.c, which is how the loop is tested with no network,
 *   no API key and no QEMU (tests/host/test_chat.c).
 *
 * FUTURE EXTENSION POINTS
 *   - Per-channel output: input_last_source() already says which console a
 *     sentence arrived on; routing this module's prose and trace lines there is
 *     a change of output function, not of shape.
 *   - Streaming: the loop reads whole response bodies today. A streaming
 *     transport would feed the same block walk incrementally.
 *   - Confirmation for TOOL_MUTATES tools slots into the dispatch step.
 *   - Spilling evicted epochs into the VFS would turn eviction into paging and
 *     give the machine a conversation that outlives its RAM budget.
 *   - The action journal is the natural place to hang a persisted audit log:
 *     the same entries appended to a VFS file would survive chat_init(), and
 *     (once the VFS has a disk behind it) a reboot.
 *   - The budget note is the only kernel voice inside the conversation. Cost
 *     accounting ("you have spent N tokens of context") would go in the same
 *     block, since it is the same class of fact: something only the loop knows.
 */
#ifndef CHAT_H
#define CHAT_H

#include <stdint.h>
#include <stddef.h>

#include "model.h"
#include "tool.h"       /* TOOL_NAME_MAX, for the action journal below */

/* ---- result codes (errno-style negatives; 0 = the turn completed) ---- */
#define CHAT_OK           0
#define CHAT_EINVAL     -22    /* bad arguments                                */
#define CHAT_ENOSPC     -28    /* a fixed buffer could not hold the turn       */
#define CHAT_ETRANSPORT -70    /* no transport, or the exchange failed         */
#define CHAT_EPROTO     -72    /* the response was not an intelligible turn    */
#define CHAT_EHTTP     -100    /* the API answered, but not with 200           */
#define CHAT_ELIMIT    -101    /* CHAT_MAX_ROUNDS tool rounds without an answer*/
#define CHAT_EHISTORY  -102    /* one turn does not fit the history at all     */

/* ---- bounds. Every one of these is a hard, static limit. ---- */

/* Model round-trips per operator sentence. The last one is still allowed to
 * call tools; hitting the cap aborts the turn rather than answering blind.
 *
 * WHY 16 AND NOT 8. The cap has to be measured against the shape of real work
 * on this machine, which is inspect -> mutate -> read back, per step, because
 * the model is told not to assume a mutation landed. One device brought up
 * through the driver VM already costs four rounds (driver_targets,
 * driver_assemble, driver_run, answer — vm/transcripts/dvm_bringup.h, replayed
 * in tests/host/test_dvm_tools.c). A two-part job with verification —
 * "make /var/log, write the boot summary into it, then tell me it is there" —
 * costs make_dir, write_file, read_file, stat_path and an answer, and one
 * mistake anywhere in it costs two more rounds to notice and repair. Eight
 * rounds fits the job only when nothing goes wrong, which is exactly the case
 * that did not need a cap. Sixteen fits the job plus a recovery.
 *
 * The cost of the higher cap is bounded and paid only by a stuck model: rounds
 * are strictly sequential HTTP requests, so the worst case is 16 round-trips of
 * wall clock, after which the machine comes back to the prompt regardless. */
#define CHAT_MAX_ROUNDS        16

/* tool_use blocks honoured in a single assistant turn. Excess blocks are still
 * answered — with a refusal — because every tool_use needs a matching result. */
#define CHAT_MAX_TOOL_CALLS    8

/* Retries per turn that do NOT spend a round: rate limits, overload and
 * transport failures, which say nothing about the work and everything about the
 * link. Bounded per turn (not per round) so a permanently rate-limited API
 * cannot hold the prompt hostage. */
#define CHAT_SEND_RETRIES      3

/* Bytes of raw JSON message content held across the whole conversation. */
#define CHAT_HISTORY_BYTES     16384

/* Messages held across the whole conversation (user + assistant turns). */
#define CHAT_HISTORY_MSGS      32

/* Result text budget handed to one tool. Below tool.h's TOOL_RESULT_MAX on
 * purpose: eight 4 KiB results cannot fit in one request body. */
#define CHAT_TOOL_RESULT_CAP   1024

/* The assembled "tools":[...] schema. This is the model's whole syscall surface
 * written out longhand, and it is the fastest-growing number in this header:
 * 18 tools cost ~8 KiB, 39 cost 29.7 KiB, 46 cost ~34 KiB — call it 750 bytes
 * of description and JSON Schema per tool. Overflowing it offers the model NO
 * tools at all, loudly, rather than advertising a contract it cannot read; that
 * is the right failure but it is a total one, so the bound is kept ahead of the
 * registry deliberately.
 *
 * THE CLIFF THIS IS APPROACHING, stated so the next agent to add a family does
 * not discover it at boot. One request must hold this schema, the system prompt
 * and the history (up to CHAT_HISTORY_BYTES), inside CHAT_REQ_BYTES, which in
 * turn must stay inside net/net.c's 64 KiB HTTP framing buffer:
 *
 *     40960 (tools) + 16384 (history) + 4333 (prompt) + ~1400 (framing)
 *   = 63077, against CHAT_REQ_BYTES = 61440
 *
 * So at this size a MAXIMAL schema plus a FULL history already spills, and the
 * loop handles that the documented way — it drops the oldest exchange and
 * rebuilds. Growing this further therefore does not just cost BSS: past roughly
 * 52 tools the machine starts forgetting mid-job on every long conversation, and
 * the fix is not another constant here. It is either a bigger request buffer in
 * net/net.c (owned elsewhere) or sending the schema more cleverly than in full
 * on every round.
 *
 * WHERE THE REGISTRY ACTUALLY IS, so nobody re-derives it from a stale comment.
 * The boot banner prints the truth ("tools : 45 registered (N bytes of
 * schema)"), and that number lives in exactly one place in this tree — the
 * constant below — which tests/host/test_app.c, tests/host/test_agency.c and
 * tests/qemu/harness.py all read. Against that REAL total the request is
 *
 *     39125 + 16384 + 4506 + 1400 = 61415 of 61440   ->   25 bytes of slack
 *
 * and CHAT_TOOLS_BYTES has 1835 bytes free. READ THOSE TWO NUMBERS TOGETHER: the
 * binding limit is the 25, not the 1835. It is possible to add a description that
 * fits CHAT_TOOLS_BYTES comfortably and still push the worst-case request past
 * CHAT_REQ_BYTES, and that is where the machine starts forgetting mid-job.
 *
 * 25 BYTES IS NOT HEADROOM, IT IS THE CLIFF EDGE. The 49th tool (driver_install,
 * the step that turns a driver a model just wrote into this machine's audio
 * output) cost ~900 bytes and could only be paid for by taking the same number
 * back out of nine other descriptions — deleting a false claim about the screen
 * size, a lecture on console geometry repeated in three tools, and prose that
 * said twice what it needed to say once. That worked, and it will not work again.
 * The next family cannot be paid for this way: the remaining descriptions are at
 * the point where cutting them removes facts a model needs. Whoever adds tool 50
 * has to fix the STRUCTURE first — send the schema once per conversation rather
 * than once per round, or page it, or grow net/net.c's framing buffer so
 * CHAT_REQ_BYTES can move. Do not shave another 900 bytes of prose.
 *
 * The earlier version of this paragraph said 4008 bytes of slack, computed from a
 * CHAT_REGISTRY_BYTES that was 4 KiB stale — so the slack it advertised had
 * already been spent. tests/host/test_agency.c and test_app.c assert the sum
 * above; they went red the moment the constant was corrected, which is the only
 * reason the overflow was found at all. Do not "fix" them by raising a constant:
 * raising CHAT_TOOLS_BYTES moves the maximal-request cliff, and raising
 * CHAT_REQ_BYTES needs a bigger framing buffer in net/net.c (owned elsewhere).
 * Something has to get shorter. */
#define CHAT_TOOLS_BYTES       40960

/* WHAT THE 45-TOOL REGISTRY ASSEMBLES TO TODAY, in bytes, measured.
 *
 * WHY A CONSTANT AND NOT A COMMENT. No host test links all 45 tools — each test
 * binary links the two or three families it exercises — so the only place the
 * real total exists is the boot banner, and the last three times it was written
 * down it was written as a literal inside a printf, in three files, and drifted
 * 699 bytes in the UNSAFE direction while every suite stayed green. The suites
 * were printing it as a measured fact and this header was pointing at them as
 * the authority, so the next agent sizing a description would have believed it
 * had 699 bytes it did not have.
 *
 * This is now one number with teeth: `make test-qemu` parses the banner and
 * FAILS if the kernel disagrees with it (tests/qemu/harness.py, tool_registry's
 * expect_schema_bytes). Growing a description is therefore a two-line change —
 * the description, and this — and forgetting the second line is loud rather
 * than silent. Get the new value from the banner, never by arithmetic on the
 * old one; that is precisely how the last drift happened. */
/* 39125 read off the banner, 2026-07, with 49 tools. NOTE FOR WHOEVER GROWS A
 * DESCRIPTION NEXT: this is no longer bounded by CHAT_TOOLS_BYTES (1835 bytes
 * spare) but by CHAT_REQ_BYTES, and the margin there is 25 bytes. The
 * worst-case request is REGISTRY + CHAT_HISTORY_BYTES + the system prompt +
 * framing, and tests/host/test_agency.c and test_app.c assert it fits. When this
 * value was stale at 35142 those two guards were passing on a number 4 KiB too
 * small, so the overflow they exist to catch had already happened and was
 * invisible. Read the banner, set this, and if the guards then fail, something
 * has to get shorter - the failure is real. */
#define CHAT_REGISTRY_BYTES    39125

/* The user turn that carries a round's tool_result blocks plus the kernel's
 * budget note.
 *
 * Eight results at CHAT_TOOL_RESULT_CAP is already 8 KiB of text before any
 * escaping, and each block also carries a model-chosen tool_use_id (up to
 * TOOL_ID_MAX, six bytes per byte once escaped), so a full round of maximum-size
 * results does NOT fit here and never did. What changed is who pays for that:
 * put_tool_result() now divides the room between the calls still waiting to be
 * answered, so this number decides how much of each result survives — never
 * whether a result exists at all. Growing it buys longer results, not
 * correctness; the correctness comes from the division. */
#define CHAT_BLOCKS_BYTES      8192

/* One request body: the system prompt, every tool schema, and the history.
 * Must stay inside net/net.c's HTTP framing buffer, which also holds the
 * headers — going over would earn a MODEL_ENOSPC from the transport instead of
 * a clean, evict-and-retry one from here.
 *
 * WHY THIS GREW. That framing buffer is 64 KiB and the request line, Host,
 * api-key, version, content-type and content-length headers come to well under
 * 1 KiB, so 60 KiB leaves 4 KiB of slack. It had to grow because the schema
 * did: 39 tools serialise to ~30 KiB, and 30 KiB of tools plus a full 16 KiB
 * history plus this prompt exceeded 48 KiB — at which point the loop starts
 * evicting live exchanges on every request to make a body fit, i.e. the machine
 * silently forgets the middle of the job it is doing. Forgetting is the correct
 * behaviour when the request really is too big; it is the wrong behaviour when
 * the buffer was simply undersized. */
#define CHAT_REQ_BYTES         61440

/* One response body. Whole-body buffered; the transport does not stream. */
#define CHAT_RESP_BYTES        65536

/* ---- lifecycle ---- */

/* Bind the transport the loop talks through and clear the conversation, the
 * counters and the action journal. `t` may be NULL — that is the "machine booted
 * with no network" case, and chat_ask() then reports it instead of pretending.
 *
 * kernel/main.c also calls this when a lost DNS resolve is retried and the model
 * becomes reachable mid-session. That drops the journal along with the
 * conversation, which is the consistent choice: with no history to resume from,
 * a record of earlier actions has nothing to attach to. */
void chat_init(model_transport_t *t);

/* Forget the conversation. Bounds, transport and counters are untouched. */
void chat_reset(void);

/* ---- the loop ---- */

/* Run one operator turn to completion: send, print, dispatch, repeat.
 * Returns CHAT_OK when the model finished with a normal answer, or one of the
 * negative codes above. Every failure path is also printed for the operator as
 * a [bracketed] line, because there is no other interface to report it on. */
int chat_ask(const char *sentence);

/* ---- the action journal: what this machine actually ran ---- */

/* Entries kept. A ring: the oldest is overwritten, never the newest, because
 * the useful question is almost always "what did you just do". */
#define CHAT_JOURNAL_ENTRIES   32

/* First line of the tool's own result text, clipped. Enough for "wrote 34 bytes
 * to /etc/motd" or the first sentence of a failure. */
#define CHAT_ACTION_DETAIL_MAX 96

/* One dispatched tool call, recorded by the loop after the call returned.
 *
 * This is the same ground truth the operator reads in the [bracketed] trace
 * lines, kept in a form the model can be handed back. It is written from the
 * dispatch result — the tool's own is_error flag and result text — so it cannot
 * be forged by the model, and unlike the conversation history it is NOT rolled
 * back when a turn is abandoned. A turn that ran out of rounds still leaves the
 * record of the four things it did behind, which is the difference between the
 * next turn resuming a job and repeating it. */
typedef struct chat_action {
    uint32_t seq;                            /* 1-based, monotonic since boot  */
    uint16_t turn;                           /* operator turn it ran in        */
    uint8_t  failed;                         /* the tool reported is_error     */
    char     name[TOOL_NAME_MAX];            /* as dispatched (may be unknown) */
    char     detail[CHAT_ACTION_DETAIL_MAX]; /* first line of the result       */
} chat_action_t;

unsigned chat_turn(void);          /* operator turns run since boot          */
unsigned chat_action_total(void);  /* tools dispatched since boot (all turns) */
unsigned chat_action_count(void);  /* entries still in the ring               */

/* Oldest kept entry is index 0. NULL when `i` is out of range. */
const chat_action_t *chat_action_at(unsigned i);

/* ---- introspection (boot banner, tests) ---- */

size_t   chat_tools_bytes(void);        /* size of the assembled tool schema  */
size_t   chat_history_messages(void);   /* messages currently remembered      */
size_t   chat_history_bytes(void);      /* arena bytes currently in use       */
unsigned chat_last_rounds(void);        /* model round-trips in the last turn */
unsigned chat_last_tool_calls(void);    /* tools dispatched in the last turn  */
unsigned chat_evictions(void);          /* exchanges forgotten to make room   */

/* Lower the round cap (never above CHAT_MAX_ROUNDS). Lets a test prove the cap
 * without queueing eight scripted responses. 0 restores the default. */
void chat_set_max_rounds(unsigned n);

/* The system prompt the loop sends. Exposed so a test can assert the machine
 * tells the model what it is, rather than leaving it to guess. */
const char *chat_system_prompt(void);

#endif /* CHAT_H */
