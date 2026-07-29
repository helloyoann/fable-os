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
 *           tool_result per tool_use, keyed by tool_use_id
 *       until CHAT_MAX_ROUNDS
 *
 *   The iteration cap is not a nicety. A model that keeps calling tools would
 *   otherwise own this machine forever; there is no scheduler to preempt it and
 *   no other console to kill it from.
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
 */
#ifndef CHAT_H
#define CHAT_H

#include <stdint.h>
#include <stddef.h>

#include "model.h"

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
 * call tools; hitting the cap aborts the turn rather than answering blind. */
#define CHAT_MAX_ROUNDS        8

/* tool_use blocks honoured in a single assistant turn. Excess blocks are still
 * answered — with a refusal — because every tool_use needs a matching result. */
#define CHAT_MAX_TOOL_CALLS    8

/* Bytes of raw JSON message content held across the whole conversation. */
#define CHAT_HISTORY_BYTES     16384

/* Messages held across the whole conversation (user + assistant turns). */
#define CHAT_HISTORY_MSGS      32

/* Result text budget handed to one tool. Below tool.h's TOOL_RESULT_MAX on
 * purpose: eight 4 KiB results cannot fit in one request body. */
#define CHAT_TOOL_RESULT_CAP   1024

/* The assembled "tools":[...] schema. This is the model's whole syscall
 * surface written out longhand: 18 tools already cost ~8 KiB, and every new
 * tool family adds to it. Overflowing it offers the model NO tools, loudly,
 * rather than advertising a contract it cannot read. */
#define CHAT_TOOLS_BYTES       32768

/* One request body: the system prompt, every tool schema, and the history.
 * Must stay inside net/net.c's HTTP framing buffer, which also holds the
 * headers — going over would earn a MODEL_ENOSPC from the transport instead of
 * a clean, evict-and-retry one from here. */
#define CHAT_REQ_BYTES         49152

/* One response body. Whole-body buffered; the transport does not stream. */
#define CHAT_RESP_BYTES        65536

/* ---- lifecycle ---- */

/* Bind the transport the loop talks through and clear the conversation.
 * `t` may be NULL — that is the "machine booted with no network" case, and
 * chat_ask() then reports it instead of pretending. */
void chat_init(model_transport_t *t);

/* Forget the conversation. Bounds, transport and counters are untouched. */
void chat_reset(void);

/* ---- the loop ---- */

/* Run one operator turn to completion: send, print, dispatch, repeat.
 * Returns CHAT_OK when the model finished with a normal answer, or one of the
 * negative codes above. Every failure path is also printed for the operator as
 * a [bracketed] line, because there is no other interface to report it on. */
int chat_ask(const char *sentence);

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
