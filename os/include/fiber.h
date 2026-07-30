/* fiber.h — cooperative concurrency: green threads with explicit yield points.
 *
 * PURPOSE
 *   Stop the machine dying every time it thinks. An API turn is seconds of
 *   spinning inside net/net.c waiting for lwIP, and for all of those seconds
 *   nothing else in this kernel gets a single instruction: the pointer freezes,
 *   windows stop repainting, a clock app stops being a clock, and typed
 *   characters pile up in the 8042. This header is the smallest thing that
 *   fixes that — a second stack, and a place to switch to it.
 *
 * WHY COOPERATIVE, AND NOT A TIMER INTERRUPT
 *   Preemption would mean a context switch can happen at ANY instruction. Every
 *   one of this kernel's ~25k lines would then have to be reentrant, plus the
 *   vendored lwIP and mbedTLS, both configured NO_SYS=1 / single-threaded, plus
 *   a lock discipline over the heap, the console, the framebuffer, the device
 *   model and the driver VM. The failure mode of getting one of those wrong is
 *   an intermittent corruption on a machine with no memory protection, which is
 *   the worst class of bug this project can own.
 *
 *   So: switches happen ONLY where a fiber says fiber_yield(), and there are
 *   exactly two kinds of place that do (see YIELD POINTS below). Between two
 *   yields a fiber has the machine to itself, which means:
 *
 *       - no locks, anywhere;
 *       - no reentrancy audit of lwIP or mbedTLS;
 *       - a data structure is only ever observed by another fiber in the state
 *         it was left in at a yield point, and those points are enumerable.
 *
 *   IF STAYS 0. This changes nothing about interrupts; every device is still
 *   polled. There is no timer tick and no preemption anywhere in this file.
 *
 * WHAT HAPPENS IF A FIBER NEVER YIELDS — the honest answer
 *   It keeps the CPU forever and the machine hangs. There is no preemption, so
 *   nothing can take the CPU back: not the scheduler, not a watchdog, not the
 *   operator. That is the price of the paragraph above and it is not
 *   recoverable at run time.
 *
 *   What this design does instead is make it hard to do by accident and loud
 *   when it happens:
 *     - The yield points are inside the two loops that actually wait
 *       (net/net.c's net_service(), kernel/main.c's wait_for_sentence()), so
 *       code that waits gets them for free without knowing fibers exist.
 *     - Fiber bodies are meant to be short and bounded. The one the kernel
 *       creates is `ui`, whose whole body is app_tick(); gui_tick(); yield —
 *       and both of those are documented as bounded and non-blocking
 *       (include/gui.h, include/app.h).
 *     - fiber_yield() measures the gap since the running fiber was last
 *       scheduled and prints ONE line naming it when the gap exceeds
 *       FIBER_HOG_MS *and* somebody else was runnable and got nothing. It
 *       cannot fix it. It can tell you which fiber to look at, which is the
 *       difference between a five-minute diagnosis and an afternoon.
 *
 * STACKS ARE GUARDED, BECAUSE THIS KERNEL HAS NO MEMORY PROTECTION
 *   All pages are RWX and identity mapped; a fiber that runs off the end of its
 *   stack does not fault, it silently writes over whatever kmalloc handed out
 *   next and the damage surfaces somewhere else entirely, later. So every fiber
 *   stack is a kmalloc'd block with a FIBER_GUARD_BYTES poison band at BOTH
 *   ends, the usable middle is pre-filled with a second pattern, and
 *   fiber_check_stacks() — which fiber_yield() calls on every single switch —
 *   verifies both bands of every live fiber and that each saved stack pointer
 *   is inside its own stack. A violation is a panic naming the fiber, the band,
 *   and how far past the end it went.
 *
 *   LIMIT, stated plainly: a poison band catches a walk off the end, not a
 *   wild store that jumps clean over 256 bytes. And it is checked at yield
 *   points, so the report says "between these two switches", not which
 *   instruction. It turns "something corrupted the heap" into "fiber ui blew
 *   its stack", which is the whole of the win.
 *
 * THE ROOT FIBER IS NOT GUARDED
 *   fiber_init() adopts the already-running kernel context as fiber 0 so that
 *   there is always something to switch back to. Its stack is the 64 KiB
 *   reserved in boot/boot.asm, whose bounds are not visible to C, so it gets no
 *   poison band. That is deliberate rather than unfortunate: the root fiber is
 *   the one that runs mbedTLS, and the deepest user of the stack should keep
 *   the biggest stack.
 *
 *   AND THE NUMBER OFFERED IN ITS PLACE IS NOT A BOUND. fiber_report() prints how
 *   far the root has descended since fiber_init(), and that figure is STRUCTURALLY
 *   BLIND, not merely approximate. It is updated only inside
 *   fiber_check_stacks(), only when the root is the fiber running, i.e. only at
 *   the yield points the root passes through — and those are shallow polling
 *   loops. net_service()'s yield is its LAST statement, after net_poll_rx() has
 *   returned, and the mbedTLS handshake runs INSIDE net_poll_rx() via lwIP's
 *   callbacks. So the deepest frames on the root can never be sampled, by
 *   construction, and the printed figure is identical on a boot that completed a
 *   TLS handshake and an HTTPS round trip and on one that did nothing: 656 bytes,
 *   every time. The true high-water mark, read out of RAM after a 25 s boot (the
 *   untouched part of .bss is provably zero, so the lowest non-zero byte below
 *   stack_top is the real depth), was 5128 bytes — about eight times the number
 *   this prints.
 *
 *   What is on the other side matters more than the ratio. boot/boot.asm lays out
 *   .bss as P4, P3 and four P2 tables and then stack_bottom, so the 24 KiB
 *   directly below the unguarded root stack ARE the page tables providing the
 *   identity map. A root overflow destroys the identity map and triple-faults with
 *   no report at all — the one failure this whole subsystem exists to prevent.
 *   Measured headroom is comfortable (5128 of 65536 bytes, 7.8%), so this is a
 *   gap in the DIAGNOSTICS and not a live hazard.
 *
 *   The fix is one line in boot/boot.asm: `global stack_bottom` and
 *   `global stack_top` would let C see the bounds and guard the root like every
 *   other fiber. Until then, read the printed figure as "the deepest the root has
 *   been seen at a yield point", which is what the console string says, and not as
 *   a depth bound — because it is not one.
 *
 * PUBLIC API
 *   fiber_init                  adopt the current context as fiber 0
 *   fiber_create                a named fiber with its own guarded stack
 *   fiber_yield                 hand the CPU to the next runnable fiber
 *   fiber_exit                  end the running fiber (never returns)
 *   fiber_join                  run until a fiber has exited, then free its slot
 *   fiber_self / fiber_name / fiber_done / fiber_count / fiber_runnable
 *   fiber_stack_bounds / fiber_stack_size / fiber_stack_used
 *   fiber_switches / fiber_switch_count
 *   fiber_check_stacks          verify every poison band right now
 *   fiber_report                one console line per fiber
 *   fiber_reset                 tear the whole thing down (tests, and re-init)
 *
 * ARCH SEAM
 *   Exactly two functions are machine-specific and both are declared at the
 *   bottom of this header. The kernel's implementations are in
 *   arch/x86_64/switch.asm. tests/host/test_fiber.c supplies its own, because
 *   the host this is developed on is not x86_64 — which is also the reason the
 *   seam is a pair of extern functions rather than inline asm in core/fiber.c.
 *
 * DEPENDENCIES
 *   kernel.h (kprintf, panic, millis, memset), heap.h (kmalloc/kfree). No
 *   hardware, no device model, no interrupts.
 *
 * FUTURE EXTENSION POINTS
 *   - fiber_sleep(ms) and a sleep queue: today a waiter spins through
 *     fiber_yield(), which is correct but burns the idle CPU. Nothing else has
 *     to change to add one.
 *   - A blocked state plus a wait/wake pair would let a fiber park on a
 *     condition instead of polling it; the scheduler already skips any fiber
 *     that is not FIBER_READY.
 *   - Per-fiber statistics are already collected, so a `fibers` tool exposing
 *     fiber_report() to the model is a tools/ file and nothing else.
 *   - If preemption is ever genuinely wanted, this is the wrong file to change:
 *     the switch itself already works: what is missing is every argument in the
 *     second paragraph.
 */
#ifndef FIBER_H
#define FIBER_H

#include <stdint.h>
#include <stddef.h>

/* How many fibers can exist at once, including the root. Small on purpose:
 * this is cooperative concurrency for a machine with two or three things to
 * do, not a thread pool. Raising it costs one slot of static state each. */
#define FIBER_MAX            8

/* Longest fiber name, excluding the NUL. A longer name is truncated — a name
 * is a diagnostic label and nothing looks it up, so truncation cannot make
 * anything address the wrong object (contrast include/vfs.h on paths). */
#define FIBER_NAME_MAX       15

/* Poison band at each end of every fiber stack, in bytes. 16-byte multiple so
 * the usable region stays 16-aligned for the SysV ABI. 256 bytes is two or
 * three typical kernel frames: big enough that a function walking off the end
 * lands in it rather than stepping over it, small enough to be free. */
#define FIBER_GUARD_BYTES    256

/* Smallest and largest usable stack a fiber may ask for, excluding the two
 * guard bands. The floor exists because a fiber with a 200-byte stack is a
 * corruption waiting for its first call; the ceiling because a bad size from a
 * caller must not be able to eat the heap. */
#define FIBER_STACK_MIN      (4u * 1024u)
#define FIBER_STACK_MAX      (256u * 1024u)

/* What kernel/main.c gives the ui fiber, and a sane default for anything else.
 * The ui fiber's deepest path is app_tick() -> an app's capability call ->
 * core/audio.c -> the driver VM; fiber_report() prints the measured high-water
 * mark so this number can be checked against reality rather than guessed at
 * twice. */
#define FIBER_STACK_DEFAULT  (32u * 1024u)

/* A fiber that has not been scheduled for this long, while another fiber was
 * runnable and got nothing, gets named on the console once. See "WHAT HAPPENS
 * IF A FIBER NEVER YIELDS" above: this is a diagnosis, not a remedy. Generous,
 * because a false accusation is worse than a late one — a model turn's local
 * work (JSON, the tool dispatch, a driver VM program) legitimately runs for a
 * while between two network waits. */
#define FIBER_HOG_MS         5000u

/* At most this many hog lines for the whole life of the machine. A warning
 * that repeats forever is a way of scrolling the evidence off the screen. */
#define FIBER_HOG_MAX_LINES  8

typedef void (*fiber_fn_t)(void *arg);

/* Opaque. The handle a caller holds is a pointer into a static table and stays
 * valid until fiber_join() releases the slot; see the lifetime note on
 * fiber_join(). */
typedef struct fiber fiber_t;

/* ---- lifecycle ---------------------------------------------------------- */

/* Adopt the running context as fiber 0, named "main". Idempotent: a second
 * call with the system already up returns 0 and changes nothing. Must be
 * called before any other function here; everything else degrades to a no-op
 * or an error until it has been. Returns 0. */
int fiber_init(void);

/* Create a runnable fiber with `stack_bytes` of usable stack (clamped into
 * [FIBER_STACK_MIN, FIBER_STACK_MAX]; 0 means FIBER_STACK_DEFAULT) plus a
 * poison band at each end. It does not run until something yields to it.
 *
 * Returns NULL, having printed why, when the system is not initialised, when
 * every slot is taken, or when the heap cannot hold the stack. A NULL return
 * is survivable by design — kernel/main.c falls back to doing the ui work
 * inline — because a machine that will not boot because it could not create a
 * cosmetic fiber is a worse machine.
 *
 * When `fn` returns, the fiber ends exactly as if it had called fiber_exit();
 * a fiber body may simply return. */
fiber_t *fiber_create(const char *name, fiber_fn_t fn, void *arg,
                      size_t stack_bytes);

/* Hand the CPU to the next runnable fiber, round-robin from the running one.
 *
 * Returns as soon as it is this fiber's turn again. A no-op — but still a
 * stack check — when this fiber is the only runnable one, and a complete no-op
 * before fiber_init(), so it is safe to sprinkle in polling loops that may run
 * during early boot.
 *
 * WHAT MAY BE HELD ACROSS A YIELD: nothing another fiber can reach. Anything
 * global and half-modified, an open handle another fiber might close, a device
 * mid-transaction, or a heap block whose header is being rewritten, must be
 * left consistent BEFORE calling this. In practice the rule is satisfied by
 * only yielding at the top of a polling loop, which is what both call sites in
 * this kernel do. */
void fiber_yield(void);

/* End the running fiber and switch away; never returns. Panics if the root
 * fiber calls it (there would be nothing left to switch to that can outlive
 * it). The stack is freed as soon as control is off it. The slot, and
 * therefore the fiber_t handle, stays valid until fiber_join(). */
void fiber_exit(void);

/* Yield until `f` has exited, then release its slot and return 0. -1 if `f` is
 * NULL, is not a live handle, or is the calling fiber.
 *
 * LIFETIME: after fiber_join() returns 0 the handle is dead and must not be
 * used again. A fiber that exits and is never joined keeps its slot forever —
 * deliberately, so that a recycled slot can never make a stale handle silently
 * name a different fiber. With FIBER_MAX slots and a kernel that creates one
 * long-lived fiber, leaking a slot is a bounded and visible cost;
 * fiber_create() says so out loud when it runs out. */
int fiber_join(fiber_t *f);

/* Tear everything down: free every fiber stack, forget every slot, and leave
 * the system uninitialised. For tests, which need each suite to start from
 * nothing, and for a re-init. Panics unless called from the root fiber. */
void fiber_reset(void);

/* ---- introspection ------------------------------------------------------ */

fiber_t    *fiber_self(void);                  /* NULL before fiber_init() */
const char *fiber_name(const fiber_t *f);      /* never NULL; "?" if f is */
int         fiber_done(const fiber_t *f);      /* 1 once it has exited */
int         fiber_count(void);                 /* live slots, root included */
int         fiber_runnable(void);              /* of those, how many can run */

/* The usable region of a fiber's stack, i.e. between the two poison bands, in
 * the natural [lo, hi) order (a stack grows down from hi). Either output may be
 * NULL. Returns 0 on success, -1 for the root fiber and for a fiber whose stack
 * has already been freed, in which case the outputs are untouched. */
int    fiber_stack_bounds(const fiber_t *f, void **lo, void **hi);
size_t fiber_stack_size(const fiber_t *f);     /* usable bytes; 0 if unknown */
/* High-water mark. For a created fiber this is a real measurement, taken from how
 * much of the FILL_WORD-stamped stack is no longer stamped. FOR THE ROOT IT IS
 * NOT: it returns the depth sampled at yield points, which cannot see the root's
 * deepest frames at all. See "THE ROOT FIBER IS NOT GUARDED" above. */
size_t fiber_stack_used(const fiber_t *f);

uint64_t fiber_switches(void);                 /* context switches since boot */
uint64_t fiber_switch_count(const fiber_t *f); /* times this fiber was resumed */

/* Verify every live fiber's poison bands and saved stack pointer right now.
 * Returns 0 when everything is intact, and the number of fibers found damaged
 * otherwise — having already panicked with a message naming the first one. (In
 * the kernel panic() does not return, so the count only ever reaches a caller
 * in host tests, where panic is captured; that is what makes the guards
 * testable.) Called by fiber_yield() on every switch. */
int fiber_check_stacks(void);

/* One line per fiber: name, state, stack high-water against its size, and how
 * often it has been scheduled. Ordinary kernel prose (no leading '['), so it
 * can never be mistaken for a trace line. */
void fiber_report(void);

/* ---- the arch seam ------------------------------------------------------
 *
 * Everything above is portable C. These two are the whole of the machine
 * dependency, and arch/x86_64/switch.asm is the whole of the implementation.
 */

/* Save the callee-saved registers of the running context onto its own stack,
 * store the resulting stack pointer through `save_sp`, then adopt `new_sp` and
 * restore the registers saved there. Returns to its caller in whichever
 * context `new_sp` belonged to.
 *
 * On x86-64 that is rbx, rbp, r12-r15 and the return address, and nothing
 * else: the kernel is compiled -mno-sse -mno-mmx with no x87, so there is no
 * FPU state to carry and MXCSR is never touched. Anything the compiler wanted
 * kept across the call is caller-saved and is already on the stack. */
void fiber_arch_switch(void **save_sp, void *new_sp);

/* Build a context on a fresh stack such that switching to the returned stack
 * pointer begins executing fn(arg). `stack_top` is the exclusive high end of
 * the usable region and is aligned down by the implementation. When fn returns
 * the arch trampoline calls fiber_exit(), so a fiber body need not. */
void *fiber_arch_stack_init(void *stack_top, fiber_fn_t fn, void *arg);

#endif /* FIBER_H */
