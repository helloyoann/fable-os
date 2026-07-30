/* main.c — kernel entry.
 *
 * Brings up the hardware, the filesystem and the network, then hands the
 * machine over to natural language. There is no shell here and there is no
 * fallback interface: after boot the only thing this kernel does is read a
 * sentence and let the model act on the machine through tool.h's syscalls.
 *
 * THE TRANSCRIPT
 *   Three voices share one console, and they are told apart by shape, not by
 *   chat-client labels:
 *
 *       > write hello into /etc/motd          <- the operator; only the '>' is
 *                                                printed by the kernel, the rest
 *                                                is the line editor's echo
 *       [vfs_write /etc/motd bytes=5 -> ok]   <- the kernel, in C, after the
 *                                                call returned (trace.h)
 *       Done - /etc/motd now holds "hello".   <- the model, prose, persuasive
 *                                                and unverified
 *
 *   Brackets are facts. Prose is a claim. That distinction is the only audit
 *   trail a machine with no `ls` can offer, so nothing else is allowed to print
 *   in bracket form.
 */

#include "kernel.h"
#include "idt.h"
#include "driver.h"
#include "input.h"
#include "net.h"
#include "fs.h"
#include "vfs.h"
#include "tool.h"
#include "chat.h"
#include "model.h"
#include "gui.h"
#include "app.h"

/* Exercise the VFS end-to-end: mount, mkdir, create/write/seek/read a file,
 * stat it, and list a directory. Proves the whole path without a disk. */
static void vfs_selftest(void) {
    if (fs_init() != 0) { kputs("[fs init failed]\n"); return; }
    kputs("\n--- filesystem self-test (ramfs at /) ---\n");

    vfs_mkdir("/etc");
    vfs_mkdir("/tmp");

    const char *msg = "talk-os native filesystem online\n";
    file_t *f = vfs_open("/etc/motd", O_CREAT | O_RDWR);
    if (!f) { kputs("open /etc/motd failed\n"); return; }
    vfs_write(f, msg, strlen(msg));

    /* Seek back and read it into a buffer to prove read/write/seek. */
    char buf[64];
    vfs_seek(f, 0, SEEK_SET);
    int64_t n = vfs_read(f, buf, sizeof buf - 1);
    if (n > 0) buf[n] = '\0';
    kprintf("read /etc/motd (%d bytes): %s", (int)n, buf);
    vfs_close(f);

    vfs_stat_t st;
    if (vfs_stat("/etc/motd", &st) == VFS_OK)
        kprintf("stat /etc/motd: type=%s size=%u mode=0x%x\n",
                st.type == VNODE_DIR ? "dir" : "file",
                (unsigned)st.size, (unsigned)st.mode);

    kputs("ls / :");
    char name[64];
    for (uint32_t i = 0; vfs_readdir("/", i, name, sizeof name) == VFS_OK; i++)
        kprintf(" %s", name);
    kputs("\n-----------------------------------------\n");
}

/* What the operator is handed after boot: what this machine can do, what it
 * cannot, and how to read what comes next. No command list, because there are
 * no commands — the tool count is the closest thing to a vocabulary and the
 * model is the only one who needs to know the names. */
static void ready_banner(int net_up) {
    kputs("\n");
    kputs("==============================================================\n");
    kputs("  talk-os\n");
    kputs("==============================================================\n");
    kprintf("  tools  : %d registered (%u bytes of schema)\n",
            tool_count(), (unsigned)chat_tools_bytes());
    kprintf("  model  : %s\n", net_up ? "reachable over TLS" : "UNREACHABLE");
    kputs("\n");
    kputs("  No shell, no commands. Say what you want done, in a sentence.\n");
    kputs("  Prose is the model talking. [Brackets] are this kernel saying\n");
    kputs("  what it actually did.\n");

    if (!net_up) {
        kputs("\n");
        kputs("  The model cannot be reached, and it is the only interface\n");
        kputs("  this machine has. It will listen, but it cannot act.\n");
    }
    if (tool_count() == 0) {
        kputs("\n");
        kputs("  No tools are registered, so the model can talk about this\n");
        kputs("  machine but cannot change it.\n");
    }
}

/* Block for a line, but through the non-blocking poll so there is one obvious
 * place to service anything else the machine needs to do between sentences.
 *
 * This is that place. Two things run in it: the pointer, the windows and the
 * repaint (gui_tick), and the periodic handlers of model-authored apps
 * (app_tick). Both are bounded, neither blocks, and both return immediately
 * when nothing is open — see the event-loop section of include/gui.h and the
 * APP_MAX_TICKS / APP_TICK_MIN_MS reasoning in include/app.h.
 *
 * ORDER MATTERS: app_tick() first. A tick handler's assignments change widget
 * text, which marks damage; gui_tick()'s gui_sync() is what paints damage. The
 * other way round a clock would be a frame stale, which on a machine whose only
 * moving picture IS the clock is the whole of the visible behaviour.
 *
 * Without this line a document carrying {"tick": 1000} is validated, accepted,
 * answered with a success trace line and a window — and never updates. Every
 * other failure in this system comes back as an error the model can act on;
 * that one came back as a lie. */
static int wait_for_sentence(char *buf, int cap) {
    if (input_source_count() == 0) return INPUT_NONE;
    for (;;) {
        (void)app_tick();
        gui_tick();
        int n = input_poll(buf, cap);
        if (n != INPUT_NONE) return n;
        __asm__ volatile("pause");
    }
}

void kernel_main(void) {
    console_init();
    idt_init();               /* before anything can fault: see include/idt.h */
    time_init();
    drivers_init();           /* serial, pci, nic, keyboard, mouse */
    gui_init();               /* window manager; no windows, so nothing drawn */

    /* Device model self-test: list the discovered device tree, then exercise
     * the driver power lifecycle (suspend everything capable, then resume). */
    kputs("\n--- device tree ---\n");
    driver_report();
    kputs("--- power cycle: suspend all, then resume all ---\n");
    drivers_suspend_all();
    drivers_resume_all();
    kputs("-------------------\n");

    vfs_selftest();

    /* Networking is required to *act* on anything — natural language is the
     * only interface, and the model lives on the far side of TLS. But a failure
     * here must not halt: the machine still boots, still reports its own state,
     * and still listens, so it can say why it cannot answer. */
    int net_up = (net_init() == 0);
    if (!net_up)
        kputs("\n[net init failed - no actions possible until networking works]\n");

    /* Boot self-test: prove the HTTPS round-trip works without needing input.
     * With no API key supplied over fw_cfg, Anthropic answers 401 — which still means
     * the TLS handshake, request, and response all succeeded. */
    if (net_up) {
        kputs("\n--- self-test: HTTPS to api.anthropic.com ---\n");
        net_ask("Reply with exactly: talk-os online");
        kputs("---------------------------------------------\n");
    }

    /* The TLS round-trip above churns thousands of allocations. Verify the heap
     * is still structurally intact and report how much it reclaimed — proof the
     * allocator's free path works (in-use should be far below the peak). */
    heap_check();
    heap_dump();

    /* Bind the turn loop to the real transport. NULL when the network never
     * came up, which chat_ask() reports rather than papering over. */
    chat_init(net_up ? model_tls_transport() : (model_transport_t *)0);

    ready_banner(net_up);

    static char line[INPUT_LINE_MAX];
    for (;;) {
        /* The prompt states where the next line is going, because it is not
         * always the model. A click on a text field borrows the keyboard for
         * exactly one line — and gui_click_at() is reached by the model's
         * gui_click tool as well as by a physical click, so the model can arm it
         * without the operator asking. The one-line advisory printed at arm time
         * scrolls away behind the rest of a turn; a prompt cannot. Without this,
         * an operator types an instruction and it silently becomes the contents
         * of a text box, with no shell to check with and nothing to tell them. */
        const char *field = gui_key_target();
        if (field) kprintf("\n[typing into \"%s\", not to the model] > ", field);
        else       kputs("\n> ");

        int n = wait_for_sentence(line, sizeof line);
        if (n == INPUT_NONE) {
            kputs("\n[no input device - this machine cannot be spoken to]\n");
            for (;;) __asm__ volatile("hlt");
        }
        if (n == 0) continue;                    /* bare Enter: ask again */

        /* A sentence that lost its tail is a different sentence, and this one
         * gets executed. Refuse it loudly rather than act on half of it. */
        if (input_line_was_truncated()) {
            kprintf("[input truncated at %d characters - not sent. A cut-off "
                    "sentence could mean something you did not say; "
                    "say it again, shorter.]\n", n);
            continue;
        }

        /* A click on a text field in a window borrows the keyboard for exactly
         * one line (include/gui.h, DECISION 3). If one is armed, this line was
         * meant for that field and not for the model; the grab is consumed
         * either way, so the prompt can never be captured for longer. After the
         * truncation check, because a cut-off sentence is not what was typed and
         * must be refused before anything consumes it. */
        if (gui_take_line(line, n)) continue;

        /* One re-resolve per sentence while the model is unreachable.
         *
         * net_init() resolves the API host exactly once, so a single lost DNS
         * query at boot used to end the session: chat holds a NULL transport,
         * every sentence is refused, and the operator cannot ask for a retry
         * because asking is what needs the network. There is no shell to fall
         * back to and no other way in. Retrying here is the whole remedy — it
         * costs nothing when the network is up, and the attempt is bounded and
         * announced so a machine with no NIC answers promptly instead of
         * looking hung. Deliberately not a background timer: nothing else in
         * this kernel runs on its own, and a retry is only useful at the moment
         * somebody actually wants something done. */
        if (!net_up && net_retry_resolve() == 0) {
            net_up = 1;
            chat_init(model_tls_transport());
            kputs("[net recovered - the model is reachable again]\n");
        }

        chat_ask(line);
    }
}
