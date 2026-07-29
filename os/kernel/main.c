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
 * place to service anything else the machine needs to do between sentences. */
static int wait_for_sentence(char *buf, int cap) {
    if (input_source_count() == 0) return INPUT_NONE;
    for (;;) {
        int n = input_poll(buf, cap);
        if (n != INPUT_NONE) return n;
        __asm__ volatile("pause");
    }
}

void kernel_main(void) {
    console_init();
    idt_init();               /* before anything can fault: see include/idt.h */
    time_init();
    drivers_init();           /* serial, pci, nic, keyboard */

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
     * With no API key compiled in, Anthropic answers 401 — which still means
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
        kputs("\n> ");

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

        chat_ask(line);
    }
}
