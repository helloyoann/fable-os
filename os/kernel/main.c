/* main.c — kernel entry. Brings up the drivers and networking, runs one
 * self-test HTTPS request (so the TLS path can be verified headlessly), then
 * drops into the terminal: type a question, it's sent to the Anthropic API
 * over TLS, and the reply is printed. Keyboard in, text out. */

#include "kernel.h"
#include "driver.h"
#include "kbd.h"
#include "net.h"

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
