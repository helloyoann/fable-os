/* dvm_bringup.h — a recorded session: the model writes a driver, twice.
 *
 * WHAT THIS IS
 *   Every byte below is a wire artifact. The strings are Messages API response
 *   bodies of the shape api.anthropic.com returns, in the order they were
 *   returned, and the programs inside them are the programs that were submitted
 *   to driver_run. Queued into net/model_mock.c they replay the whole loop —
 *   chat_ask -> tool_use -> tool_dispatch -> dvm_assemble -> dvm_run -> a
 *   tool_result the model reads -> a corrected program — with no network, no
 *   API key and no QEMU. tests/host/test_dvm_tools.c is the replay.
 *
 *   Without this, "the model wrote a driver" is an anecdote. With it, it is a
 *   test that fails the build when the loop stops working.
 *
 * HONESTY ABOUT WHAT IS RECORDED
 *   This kernel has no API key compiled in (the API answers 401, which is how
 *   the TLS path is proved), so a live model has never actually reached these
 *   tools. THE ASSISTANT TURNS BELOW ARE AUTHORED, NOT CAPTURED FROM A LIVE
 *   MODEL. What is real is everything on the kernel side of the seam: the tool
 *   dispatch, the assembler, the policy derived from the device's own BARs, the
 *   VM's execution and refusals, the access log fed back between attempts, and
 *   the attempt bound. The device is the synthetic TLK1 modelled in the test
 *   file, because a host test has no PCI bus.
 *
 *   Read this as: "when a model does write these programs, this is exactly what
 *   happens" — proved for the machinery, pending for the model.
 *
 * THE SESSION
 *   > There is an unclaimed device at 00:05.0. Work out what it is, bring it
 *     up, and tell me it is running.
 *
 *   round 1  driver_targets                    -> pci00:05.0, ports 0xd000-0xd0ff
 *   round 2  driver_run  (TLK1_PROG_V1)        -> ABORT "rate did not stick"
 *                                                 the access log shows the rate
 *                                                 register reading back 0 right
 *                                                 after the enable
 *   round 3  driver_run  (TLK1_PROG_V2)        -> OK: the same program with the
 *                                                 rate written BEFORE the enable
 *   round 4  prose: what the device is and that it is running
 *
 *   The bug in attempt 1 is the realistic kind — not a syntax error but a
 *   misunderstanding of the device: TLK1 latches its rate divisor only while
 *   the channel is disabled. Nothing in the tool's answer says that; the model
 *   has to read it out of the recorded accesses, which is the whole point of
 *   returning them.
 *
 * THE SECOND SESSION (the one that does not converge)
 *   Same operator sentence, a model that decides the way to reset the device is
 *   to pulse the PS/2 controller's CPU-reset line. That port is on the VM's
 *   compiled-in deny list, so the program is refused every single time, and
 *   after DRV_ATTEMPT_BUDGET failures driver_run stops running programs against
 *   that target at all. Recorded so the bound is a test rather than a claim.
 *
 * EDITING
 *   These are JSON string literals in C, so a program's own `"` is written
 *   \\\" and its newlines are \\n. Keep the two program versions differing by
 *   exactly the fix described above: the diff is the evidence that the loop
 *   converged for a reason.
 */
#ifndef DVM_TRANSCRIPT_BRINGUP_H
#define DVM_TRANSCRIPT_BRINGUP_H

/* ---- what the operator typed ---- */
#define TR_SENTENCE \
    "There is an unclaimed device at 00:05.0. Work out what it is, bring it up, " \
    "and tell me it is running."

/* ---- response envelopes ---- */
#define TR_MSG(blocks, stop)                                                   \
    "{\"id\":\"msg_01TranscriptDvmBringup\",\"type\":\"message\","             \
    "\"role\":\"assistant\",\"model\":\"claude-transcript\",\"content\":["     \
    blocks "],\"stop_reason\":\"" stop "\"}"

#define TR_TEXT(t)  "{\"type\":\"text\",\"text\":\"" t "\"}"
#define TR_USE(id, name, input)                                                \
    "{\"type\":\"tool_use\",\"id\":\"" id "\",\"name\":\"" name "\","          \
    "\"input\":" input "}"

/* ======================================================================= */
/* the programs, exactly as they were submitted (JSON-escaped)             */
/* ======================================================================= */

/* Shared prologue: identify the device, cold-reset it, and wait — with a
 * counted retry, never an unbounded spin — for it to report ready. */
#define TLK1_HEAD                                                              \
    "; TLK1 bring-up. r0 is BAR0, handed in by the kernel.\\n"                 \
    ".equ R_ID       0x00\\n"                                                  \
    ".equ R_CTL      0x04\\n"                                                  \
    ".equ R_STAT     0x08\\n"                                                  \
    ".equ R_RATE     0x0c\\n"                                                  \
    ".equ R_COUNT    0x10\\n"                                                  \
    ".equ CTL_RESET  0x01\\n"                                                  \
    ".equ CTL_ENABLE 0x02\\n"                                                  \
    ".equ ST_READY   0x01\\n"                                                  \
    ".equ ST_RUN     0x02\\n"                                                  \
    ".equ TLK1_ID    0x4b4c5401\\n"                                            \
    ".equ RATE_DIV   0x0100\\n"                                                \
    "\\n"                                                                      \
    "    in32  r8, r0\\n"                                                      \
    "    cmp   r8, TLK1_ID\\n"                                                 \
    "    bne   wrong_device\\n"                                                \
    "    print \\\"id\\\", r8\\n"                                              \
    "\\n"                                                                      \
    "    add   r5, r0, R_CTL\\n"                                               \
    "    out32 r5, CTL_RESET\\n"                                               \
    "    delay 1000\\n"                                                        \
    "\\n"                                                                      \
    "    add   r4, r0, R_STAT\\n"                                              \
    "    mov   r9, 20\\n"                                                      \
    "wait_ready:\\n"                                                           \
    "    in32  r10, r4\\n"                                                     \
    "    test  r10, ST_READY\\n"                                               \
    "    bne   ready\\n"                                                       \
    "    delay 1000\\n"                                                        \
    "    sub   r9, 1\\n"                                                       \
    "    cmp   r9, 0\\n"                                                       \
    "    bne   wait_ready\\n"                                                  \
    "    abort \\\"device never reported ready after reset\\\"\\n"             \
    "\\n"                                                                      \
    "ready:\\n"

/* Shared epilogue: prove it is really running, and that the counter moves. */
#define TLK1_TAIL                                                              \
    "    in32  r12, r4\\n"                                                     \
    "    test  r12, ST_RUN\\n"                                                 \
    "    beq   not_running\\n"                                                 \
    "\\n"                                                                      \
    "    add   r2, r0, R_COUNT\\n"                                             \
    "    in32  r13, r2\\n"                                                     \
    "    delay 2000\\n"                                                        \
    "    in32  r14, r2\\n"                                                     \
    "    cmp   r14, r13\\n"                                                    \
    "    ble   not_counting\\n"                                                \
    "    print \\\"count\\\", r14\\n"                                          \
    "    halt\\n"                                                              \
    "\\n"                                                                      \
    "wrong_device:\\n"                                                         \
    "    abort \\\"identity register is not a TLK1\\\"\\n"                     \
    "rate_stuck:\\n"                                                           \
    "    abort \\\"rate did not stick\\\"\\n"                                  \
    "not_running:\\n"                                                          \
    "    abort \\\"enabled but status does not say running\\\"\\n"             \
    "not_counting:\\n"                                                         \
    "    abort \\\"the counter did not advance\\\"\\n"

/* Attempt 1: enable the channel, then program the rate. Wrong on this device —
 * the rate divisor only latches while the channel is disabled — and the
 * readback catches it. */
#define TLK1_PROG_V1                                                           \
    TLK1_HEAD                                                                  \
    "    out32 r5, CTL_ENABLE\\n"                                              \
    "    delay 1000\\n"                                                        \
    "\\n"                                                                      \
    "    add   r3, r0, R_RATE\\n"                                              \
    "    out16 r3, RATE_DIV\\n"                                                \
    "    in16  r11, r3\\n"                                                     \
    "    cmp   r11, RATE_DIV\\n"                                               \
    "    bne   rate_stuck\\n"                                                  \
    "\\n"                                                                      \
    TLK1_TAIL

/* Attempt 2: rate first, then enable. The only change. */
#define TLK1_PROG_V2                                                           \
    TLK1_HEAD                                                                  \
    "    add   r3, r0, R_RATE\\n"                                              \
    "    out16 r3, RATE_DIV\\n"                                                \
    "    in16  r11, r3\\n"                                                     \
    "    cmp   r11, RATE_DIV\\n"                                               \
    "    bne   rate_stuck\\n"                                                  \
    "\\n"                                                                      \
    "    out32 r5, CTL_ENABLE\\n"                                              \
    "    delay 1000\\n"                                                        \
    "\\n"                                                                      \
    TLK1_TAIL

/* ======================================================================= */
/* session 1: it converges                                                 */
/* ======================================================================= */

#define TR_ROUND1                                                              \
    TR_MSG(TR_TEXT("Let me see what I am allowed to drive.")                   \
           ","                                                                 \
           TR_USE("toolu_tr_1", "driver_targets", "{\"include_unusable\":true}"), \
           "tool_use")

#define TR_ROUND2                                                              \
    TR_MSG(TR_TEXT("00:05.0 answers with an unknown ID. I will identify it, "  \
                   "reset it, set its rate and confirm it runs.")              \
           ","                                                                 \
           TR_USE("toolu_tr_2", "driver_run",                                  \
                  "{\"target\":\"pci00:05.0\",\"source\":\"" TLK1_PROG_V1 "\"}"), \
           "tool_use")

#define TR_ROUND3                                                              \
    TR_MSG(TR_TEXT("The log shows out16 0xd00c <= 0x100 reading back as 0, "   \
                   "and it happened after the enable. This device latches its "\
                   "rate only while the channel is disabled. Reordering.")     \
           ","                                                                 \
           TR_USE("toolu_tr_3", "driver_run",                                  \
                  "{\"target\":\"pci00:05.0\",\"source\":\"" TLK1_PROG_V2 "\"}"), \
           "tool_use")

#define TR_ROUND4                                                              \
    TR_MSG(TR_TEXT("00:05.0 is a TLK1 rate generator (ID 0x4b4c5401). It is "  \
                   "reset, its divisor is 0x100, status reads 0x3 - ready and "\
                   "running - and its counter advanced, so it really is "      \
                   "clocking. The driver is 39 instructions of VM code."),     \
           "end_turn")

/* ======================================================================= */
/* session 2: it does not converge                                         */
/* ======================================================================= */

/* A model convinced that the way to reset a device is to pulse the PS/2
 * controller's CPU-reset line. Short, so five of them plus their results fit
 * the conversation history with room to spare. */
#define TLK1_PROG_BAD                                                          \
    "; force a reset the hard way\\n"                                          \
    "    in32  r1, r0\\n"                                                      \
    "    out8  0x64, 0xfe\\n"                                                  \
    "    halt\\n"

#define TR_STUCK_TRY(id)                                                       \
    TR_MSG(TR_USE(id, "driver_run",                                            \
                  "{\"target\":\"pci00:05.0\",\"source\":\"" TLK1_PROG_BAD "\"}"), \
           "tool_use")

#define TR_STUCK1  TR_STUCK_TRY("toolu_st_1")
#define TR_STUCK2  TR_STUCK_TRY("toolu_st_2")
#define TR_STUCK3  TR_STUCK_TRY("toolu_st_3")
#define TR_STUCK4  TR_STUCK_TRY("toolu_st_4")
#define TR_STUCK5  TR_STUCK_TRY("toolu_st_5")
#define TR_STUCK6  TR_STUCK_TRY("toolu_st_6")

#define TR_STUCK_GIVE_UP                                                       \
    TR_MSG(TR_TEXT("I cannot bring that device up. Port 0x64 is refused by the "\
                   "kernel - it can pulse the CPU reset line - and I have used "\
                   "every attempt on this target. It needs a device-specific "  \
                   "reset I do not know."),                                    \
           "end_turn")

#endif /* DVM_TRANSCRIPT_BRINGUP_H */
