/* main.c — kernel entry. Brings up the drivers and networking, runs one
 * self-test HTTPS request (so the TLS path can be verified headlessly), then
 * drops into the terminal: type a question, it's sent to the Anthropic API
 * over TLS, and the reply is printed. Keyboard in, text out. */

#include "kernel.h"
#include "driver.h"
#include "kbd.h"
#include "net.h"
#include "fs.h"
#include "vfs.h"

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

void kernel_main(void) {
    console_init();
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

    if (net_init() != 0) {
        kputs("\n[net init failed — halting]\n");
        for (;;) __asm__ volatile("hlt");
    }

    /* Boot self-test: prove the HTTPS round-trip works without needing the
     * keyboard. With no API key compiled in, Anthropic answers 401 — which
     * still means the TLS handshake, request, and response all succeeded. */
    kputs("\n--- self-test: HTTPS to api.anthropic.com ---\n");
    net_ask("Reply with exactly: talk-os online");
    kputs("---------------------------------------------\n");

    /* The TLS round-trip above churns thousands of allocations. Verify the heap
     * is still structurally intact and report how much it reclaimed — proof the
     * allocator's free path works (in-use should be far below the peak). */
    heap_check();
    heap_dump();

    kputs("\n");
    kputs("==============================================\n");
    kputs("  talk-os : ask the AI a question, press Enter\n");
    kputs("==============================================\n\n");

    static char line[4096];
    for (;;) {
        kputs("you> ");
        int n = kbd_readline(line, sizeof line);
        if (n == 0) continue;

        kputs("\nai> ");
        if (net_ask(line) != 0)
            kputs("[no answer — TLS/connection failed]\n");
        kputs("\n");
    }
}
