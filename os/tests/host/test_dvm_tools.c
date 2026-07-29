/* test_dvm_tools.c — the model-writes-a-driver loop, driven end to end.
 *
 * WHAT THIS SUITE IS
 *   tools/dvm_tools.c is the seam where a language model's text becomes port
 *   I/O. Everything on the kernel side of that seam is pure logic plus two
 *   function-pointer boundaries, so the whole loop runs natively:
 *
 *     - pci_cfg_read32() is replaced by a synthetic bus modelled on the machine
 *       QEMU presents, including the awkward cases (a claimed device, a
 *       bus-mastering device, a BAR that overlaps COM1, a BAR inside the
 *       interrupt-controller block, two devices with adjacent BARs);
 *     - dvm_io_t is replaced by TLK1, a synthetic device with a reset, a
 *       ready handshake, a rate register that only latches while disabled, and
 *       a counter that advances with time. It is the device the recorded
 *       session in vm/transcripts/dvm_bringup.h brings up;
 *     - net/model_mock.c is the model, and net/chat.c is the REAL turn loop.
 *
 *   So "the model submits a program, the kernel refuses it, the model reads the
 *   access log, fixes the program and the device comes up" is a test, not a
 *   story — and it needs no network, no API key and no QEMU.
 *
 * THE LOAD-BEARING ASSERTION
 *   TLK1 counts every access it should never have seen: any port outside the
 *   window the target's BAR earned, any MMIO at all, any PCI function other
 *   than the target. `forbidden` must be zero at the end of the suite,
 *   including after the fuzzers. That is a mechanical proof that a program can
 *   only reach the device it was pointed at.
 *
 * WHAT THIS SUITE CANNOT PROVE
 *   That a real model, with a real API key, writes programs like these. There
 *   is no key compiled into this kernel (the API answers 401), so the assistant
 *   turns in the transcript are authored rather than captured. What is proved
 *   is that when those bytes arrive, the kernel does exactly this. It also
 *   cannot prove dvm_io_hardware() reaches silicon — vm/programs/ac97_boot.c
 *   and tests/qemu/cases/ac97.case do that.
 */

#include "harness.h"

#include "tool.h"
#include "device.h"
#include "driver.h"
#include "kobject.h"
#include "trace.h"
#include "json.h"
#include "pci.h"
#include "dvm.h"
#include "chat.h"
#include "model.h"

#include <stdlib.h>
#include <string.h>

#include "../../vm/transcripts/dvm_bringup.h"

/* ====================================================================== */
/* the synthetic PCI bus                                                  */
/* ====================================================================== */

typedef struct {
    uint8_t  bus, dev, fn;
    uint32_t w[64];                 /* 256 bytes of config space */
} fake_fn_t;

#define FAKE_MAX 32
static fake_fn_t fake[FAKE_MAX];
static int       nfake;
static long      cfg_writes;        /* MUST stay 0 for the whole suite */

static fake_fn_t *fake_add(uint8_t bus, uint8_t dev, uint8_t fn,
                           uint16_t vendor, uint16_t device,
                           uint8_t cls, uint8_t sub) {
    CHECK(nfake < FAKE_MAX);
    if (nfake >= FAKE_MAX) return &fake[FAKE_MAX - 1];
    fake_fn_t *f = &fake[nfake++];
    memset(f, 0, sizeof *f);
    f->bus = bus; f->dev = dev; f->fn = fn;
    f->w[0x00 / 4] = ((uint32_t)device << 16) | vendor;
    f->w[0x04 / 4] = 0x00000001u;                  /* I/O space enabled */
    f->w[0x08 / 4] = ((uint32_t)cls << 24) | ((uint32_t)sub << 16);
    return f;
}

uint32_t pci_cfg_read32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off) {
    for (int i = 0; i < nfake; i++)
        if (fake[i].bus == bus && fake[i].dev == dev && fake[i].fn == fn)
            return fake[i].w[(off & 0xFC) >> 2];
    return 0xFFFFFFFFu;
}

void pci_cfg_write32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off, uint32_t val) {
    (void)bus; (void)dev; (void)fn; (void)off; (void)val;
    cfg_writes++;                   /* a tool doing this is a test failure */
}

/* ====================================================================== */
/* TLK1: the synthetic device a program is supposed to bring up           */
/* ====================================================================== */

#define TLK_BASE      0xD000u
#define TLK_ID_VALUE  0x4B4C5401u

#define TLK_R_ID      0x00
#define TLK_R_CTL     0x04
#define TLK_R_STAT    0x08
#define TLK_R_RATE    0x0C
#define TLK_R_COUNT   0x10
#define TLK_R_IRQMASK 0x14

#define TLK_CTL_RESET  0x1
#define TLK_CTL_ENABLE 0x2
#define TLK_ST_READY   0x1
#define TLK_ST_RUN     0x2

typedef struct {
    uint32_t rate, count, irqmask;
    int      ready, running, was_reset;
    uint64_t delay_total, ready_at;
} tlk_t;

static tlk_t tlk;

/* The sandbox the current run is supposed to be confined to. Anything outside
 * it that reaches this backend is a containment failure, counted and asserted
 * on at the end of the suite. */
static unsigned exp_lo = TLK_BASE, exp_hi = TLK_BASE + 0xFF;
static unsigned exp_bdf = (0 << 8) | (5 << 3) | 0;
static long     forbidden;
static long     tlk_accesses;
static long     tlk_delays;

static void tlk_reset_device(void) {
    memset(&tlk, 0, sizeof tlk);
}

static void tlk_tick_ready(void) {
    if (tlk.was_reset && !tlk.ready && tlk.delay_total >= tlk.ready_at) tlk.ready = 1;
}

static void tlk_port_write(void *ctx, uint16_t port, uint8_t width, uint32_t val) {
    (void)ctx;
    tlk_accesses++;
    if (port < exp_lo || port > exp_hi) { forbidden++; return; }
    unsigned off = port - TLK_BASE;
    switch (off) {
        case TLK_R_CTL:
            if (val & TLK_CTL_RESET) {
                uint64_t d = tlk.delay_total;
                memset(&tlk, 0, sizeof tlk);
                tlk.delay_total = d;
                tlk.was_reset   = 1;
                tlk.ready_at    = d + 2000;      /* 2 ms to come back */
            }
            /* The channel only starts if the device came back ready. */
            if ((val & TLK_CTL_ENABLE) && tlk.ready) tlk.running = 1;
            if (!(val & TLK_CTL_ENABLE)) tlk.running = 0;
            break;
        case TLK_R_RATE:
            /* The bug the recorded session trips over: the divisor latches
             * only while the channel is stopped. A running device swallows the
             * write silently, exactly like real hardware does. */
            if (!tlk.running) tlk.rate = val & (width == 1 ? 0xFFu : width == 2 ? 0xFFFFu : 0xFFFFFFFFu);
            break;
        case TLK_R_IRQMASK: tlk.irqmask = val; break;
        default: break;                          /* read-only or absent */
    }
}

static uint32_t tlk_port_read(void *ctx, uint16_t port, uint8_t width) {
    (void)ctx;
    tlk_accesses++;
    if (port < exp_lo || port > exp_hi) { forbidden++; return 0xFFFFFFFFu; }
    tlk_tick_ready();
    unsigned off = port - TLK_BASE;
    uint32_t v;
    switch (off) {
        case TLK_R_ID:    v = TLK_ID_VALUE; break;
        case TLK_R_CTL:   v = (uint32_t)((tlk.running ? TLK_CTL_ENABLE : 0)); break;
        case TLK_R_STAT:  v = (uint32_t)((tlk.ready ? TLK_ST_READY : 0) |
                                         (tlk.running ? TLK_ST_RUN : 0)); break;
        case TLK_R_RATE:  v = tlk.rate; break;
        case TLK_R_COUNT: v = tlk.count; break;
        case TLK_R_IRQMASK: v = tlk.irqmask; break;
        default: v = 0xFFFFFFFFu; break;         /* nothing decodes there */
    }
    if (width == 1) v &= 0xFFu;
    else if (width == 2) v &= 0xFFFFu;
    return v;
}

/* No MMIO window is granted unless a test points exp_mmio_* at one, so any
 * other MMIO access is a containment failure. The modelled window is a plain
 * 256-byte scratch block: enough to prove ld/st reach it. */
static unsigned char mspace[256];
static uint64_t      exp_mmio_lo, exp_mmio_hi;

static int mmio_in_window(uint64_t addr, uint8_t width) {
    return exp_mmio_hi && addr >= exp_mmio_lo && addr + width - 1 <= exp_mmio_hi;
}

static void tlk_mmio_write(void *ctx, uint64_t addr, uint8_t width, uint32_t val) {
    (void)ctx;
    tlk_accesses++;
    if (!mmio_in_window(addr, width)) { forbidden++; return; }
    for (uint8_t i = 0; i < width; i++)
        mspace[(addr - exp_mmio_lo + i) % sizeof mspace] = (unsigned char)(val >> (8 * i));
}
static uint32_t tlk_mmio_read(void *ctx, uint64_t addr, uint8_t width) {
    (void)ctx;
    tlk_accesses++;
    if (!mmio_in_window(addr, width)) { forbidden++; return 0xFFFFFFFFu; }
    uint32_t v = 0;
    for (uint8_t i = 0; i < width; i++)
        v |= (uint32_t)mspace[(addr - exp_mmio_lo + i) % sizeof mspace] << (8 * i);
    return v;
}

static uint32_t tlk_pci_read32(void *ctx, uint8_t bus, uint8_t dev, uint8_t fn,
                               uint8_t off) {
    (void)ctx;
    tlk_accesses++;
    unsigned bdf = ((unsigned)bus << 8) | ((unsigned)dev << 3) | fn;
    if (bdf != exp_bdf) { forbidden++; return 0xFFFFFFFFu; }
    return pci_cfg_read32(bus, dev, fn, off);
}

static void tlk_delay(void *ctx, uint32_t us) {
    (void)ctx;
    tlk_delays++;
    tlk.delay_total += us;
    if (tlk.running) tlk.count += us / 100;
}

static const dvm_io_t tlk_io = {
    NULL,
    tlk_port_write, tlk_port_read,
    tlk_mmio_write, tlk_mmio_read,
    tlk_pci_read32, tlk_delay,
};

/* ====================================================================== */
/* the module under test                                                  */
/* ====================================================================== */

#ifdef __APPLE__
/* REGISTER_TOOL collects pointers in a linker section. Mach-O spells the
 * section and its bounds differently; same shim tests/host/test_chat.c uses, so
 * core/tool.c walks the same table the kernel's linker script builds. */
__asm__(".globl ___start_tool_table\n"
        ".set   ___start_tool_table, section$start$__DATA$tool_table\n"
        ".globl ___stop_tool_table\n"
        ".set   ___stop_tool_table, section$end$__DATA$tool_table\n");
#  undef  REGISTER_TOOL
#  define REGISTER_TOOL(v)                                                     \
      static const tool_t *const __toolptr_##v                                 \
          __attribute__((used, section("__DATA,tool_table"))) = &(v)
/* Apple's <string.h> defines memcpy/memset/... as fortifying macros, which
 * collide with kernel.h's plain prototypes when both land in one TU. */
#  undef memcpy
#  undef memset
#  undef memmove
#  undef memcmp
#  undef strlen
#endif

#include "../../tools/dvm_tools.c"

/* Exported by the file above for the kernel and for this suite. */
void dvm_tools_set_io(const dvm_io_t *io);
void dvm_tools_reset_attempts(void);

/* ====================================================================== */
/* fixtures                                                               */
/* ====================================================================== */

static driver_t fake_driver = { "e1000fake", 0, DEV_CLASS_NETWORK, 0, 0, 0, 0, 0 };

static void add_node(const char *name, device_class_t cls, driver_t *drv,
                     device_res_type_t rt, uint64_t base) {
    device_t *d = device_create(name, cls);
    if (rt != RES_NONE) device_add_resource(d, rt, base, 0);
    device_register(d);
    if (drv) {
        device_bind_driver(d, drv);
        device_set_state(d, DEV_STATE_ACTIVE);
    }
}

/* A bus with one good target and one of every awkward case. */
static void build_machine(void) {
    fake_fn_t *f;

    fake_add(0, 0, 0, 0x8086, 0x1237, 0x06, 0x00);            /* host bridge, no BAR */

    f = fake_add(0, 3, 0, 0x8086, 0x100E, 0x02, 0x00);        /* claimed NIC */
    f->w[0x10 / 4] = 0xFE900000u;

    f = fake_add(0, 4, 0, 0x8086, 0x2415, 0x04, 0x01);        /* bus mastering */
    f->w[0x04 / 4] = 0x0007u;                                 /* io+mem+busmaster */
    f->w[0x10 / 4] = 0xC000u | 1u;

    f = fake_add(0, 5, 0, 0x1234, 0x5678, 0x08, 0x80);        /* THE TARGET */
    f->w[0x10 / 4] = TLK_BASE | 1u;

    f = fake_add(0, 6, 0, 0x1234, 0x0006, 0x08, 0x80);        /* BAR over COM1 */
    f->w[0x10 / 4] = 0x03F8u | 1u;

    f = fake_add(0, 7, 0, 0x1234, 0x0007, 0x08, 0x80);        /* BAR in the APIC block */
    f->w[0x10 / 4] = 0xFEC01000u;

    f = fake_add(0, 8, 0, 0x1234, 0x0008, 0x08, 0x80);        /* the target's neighbour */
    f->w[0x10 / 4] = 0xD100u | 1u;

    f = fake_add(0, 9, 0, 0x1234, 0x0009, 0x08, 0x80);        /* clamped to 64 bytes */
    f->w[0x10 / 4] = 0xE000u | 1u;

    f = fake_add(0, 10, 0, 0x1234, 0x000A, 0x08, 0x80);       /* ...by this one */
    f->w[0x10 / 4] = 0xE040u | 1u;

    f = fake_add(0, 11, 0, 0x1234, 0x000B, 0x08, 0x80);       /* an MMIO device */
    f->w[0x10 / 4] = 0xFE9D0000u;

    f = fake_add(0, 12, 0, 0x1234, 0x000C, 0x08, 0x80);       /* MMIO, targetable */
    f->w[0x10 / 4] = 0xFEB00000u;

    f = fake_add(0, 13, 0, 0x1234, 0x000D, 0x08, 0x80);       /* nonsense I/O BAR */
    f->w[0x10 / 4] = 0x1F000000u | 1u;

    add_node("pci00:00.0", DEV_CLASS_BUS,     NULL,         RES_NONE,   0);
    add_node("pci00:03.0", DEV_CLASS_NETWORK, &fake_driver, RES_MMIO,   0xFE900000);
    add_node("pci00:04.0", DEV_CLASS_AUDIO,   NULL,         RES_IOPORT, 0xC000);
    add_node("pci00:05.0", DEV_CLASS_SYSTEM,  NULL,         RES_IOPORT, TLK_BASE);
    add_node("pci00:06.0", DEV_CLASS_SYSTEM,  NULL,         RES_IOPORT, 0x03F8);
    add_node("pci00:07.0", DEV_CLASS_SYSTEM,  NULL,         RES_MMIO,   0xFEC01000);
    add_node("pci00:08.0", DEV_CLASS_SYSTEM,  NULL,         RES_IOPORT, 0xD100);
    add_node("pci00:09.0", DEV_CLASS_SYSTEM,  NULL,         RES_IOPORT, 0xE000);
    add_node("pci00:0a.0", DEV_CLASS_SYSTEM,  NULL,         RES_IOPORT, 0xE040);
    add_node("pci00:0b.0", DEV_CLASS_SYSTEM,  NULL,         RES_MMIO,   0xFE9D0000);
    /* The awkward one: the PCI node for 00:0b.0 is driverless, but a DIFFERENT
     * registered device owns the same window and has a driver — exactly how
     * pci00:03.0/eth0 look on the real machine. */
    add_node("snd0",       DEV_CLASS_AUDIO,   &fake_driver, RES_MMIO,   0xFE9D0000);
    add_node("pci00:0c.0", DEV_CLASS_SYSTEM,  NULL,         RES_MMIO,   0xFEB00000);
    add_node("pci00:0d.0", DEV_CLASS_SYSTEM,  NULL,         RES_IOPORT, 0x1F000000);
    add_node("pci00:1f.0", DEV_CLASS_SYSTEM,  NULL,         RES_NONE,   0);  /* absent */
    add_node("serial0",    DEV_CLASS_SERIAL,  NULL,         RES_IOPORT, 0x3F8);
}

/* The machine this kernel actually boots, on bus 1 so it coexists with the
 * fixture above. Every address here was read out of QEMU's own monitor
 * (`info pci`, after the BIOS programmed the BARs) on the same command line
 * tests/qemu uses, so the derivation below is checked against the real thing
 * rather than against something invented to make it look good:
 *
 *   00:01.1 IDE   BAR4 I/O  0xc540 [0xc54f]        (16 bytes)
 *   00:02.0 VGA   BAR0 mem  0xfd000000 [0xfdffffff] (16 MiB, prefetchable)
 *                 BAR2 mem  0xfebf0000 [0xfebf0fff] (4 KiB)
 *   00:03.0 e1000 BAR0 mem  0xfebc0000 [0xfebdffff] (128 KiB)
 *                 BAR1 I/O  0xc500 [0xc53f]         (64 bytes)
 *   00:04.0 AC97  BAR0 I/O  0xc000 [0xc3ff]         (1 KiB)
 *                 BAR1 I/O  0xc400 [0xc4ff]         (256 bytes)
 */
static void build_qemu_replica(void) {
    fake_fn_t *f;

    fake_add(1, 0, 0, 0x8086, 0x1237, 0x06, 0x00);
    fake_add(1, 1, 0, 0x8086, 0x7000, 0x06, 0x01);
    f = fake_add(1, 1, 1, 0x8086, 0x7010, 0x01, 0x01);
    f->w[0x20 / 4] = 0xC540u | 1u;                        /* BAR4 */
    fake_add(1, 1, 3, 0x8086, 0x7113, 0x06, 0x80);
    f = fake_add(1, 2, 0, 0x1234, 0x1111, 0x03, 0x00);
    f->w[0x10 / 4] = 0xFD000000u | 0x8u;                  /* prefetchable */
    f->w[0x18 / 4] = 0xFEBF0000u;
    f = fake_add(1, 3, 0, 0x8086, 0x100E, 0x02, 0x00);
    f->w[0x10 / 4] = 0xFEBC0000u;
    f->w[0x14 / 4] = 0xC500u | 1u;
    f = fake_add(1, 4, 0, 0x8086, 0x2415, 0x04, 0x01);
    f->w[0x10 / 4] = 0xC000u | 1u;
    f->w[0x14 / 4] = 0xC400u | 1u;

    add_node("pci01:00.0", DEV_CLASS_BUS,     NULL, RES_NONE,   0);
    add_node("pci01:01.1", DEV_CLASS_STORAGE, NULL, RES_IOPORT, 0xC540);
    add_node("pci01:02.0", DEV_CLASS_DISPLAY, NULL, RES_MMIO,   0xFD000000);
    add_node("pci01:03.0", DEV_CLASS_NETWORK, NULL, RES_MMIO,   0xFEBC0000);
    add_node("pci01:04.0", DEV_CLASS_AUDIO,   NULL, RES_IOPORT, 0xC000);
}

/* ====================================================================== */
/* calling a tool                                                         */
/* ====================================================================== */

static char          rbuf[TOOL_RESULT_MAX];
static tool_result_t rres;

/* `cap` mirrors the real budget a tool gets from chat.c (CHAT_TOOL_RESULT_CAP)
 * when it matters, and the full TOOL_RESULT_MAX when a test wants to read the
 * whole answer.
 *
 * tool_dispatch() returns TOOL_OK whenever the HANDLER RAN, even when the
 * handler refused the request (that is the contract in tool.h — the model still
 * gets an answer). These helpers fold `is_error` into the return value, so a
 * test can say "this call must fail" in one place. */
static int call_cap(const char *name, const char *input, size_t cap) {
    if (cap > sizeof rbuf) cap = sizeof rbuf;
    tool_call_t c;
    c.id        = "toolu_test";
    c.name      = name;
    c.input     = input;
    c.input_len = strlen(input);
    tool_result_init(&rres, rbuf, cap);
    kcap_reset();
    int rc = tool_dispatch(&c, &rres);
    if (rc != TOOL_OK) return rc;
    return rres.is_error ? TOOL_EINVAL : TOOL_OK;
}

static int call(const char *name, const char *input) {
    return call_cap(name, input, sizeof rbuf);
}

/* Raw span form: the input is NOT NUL-terminated and may contain anything. */
static int call_raw(const char *name, const char *input, size_t len) {
    tool_call_t c;
    c.id        = "toolu_test";
    c.name      = name;
    c.input     = input;
    c.input_len = len;
    tool_result_init(&rres, rbuf, sizeof rbuf);
    kcap_reset();
    int rc = tool_dispatch(&c, &rres);
    if (rc != TOOL_OK) return rc;
    return rres.is_error ? TOOL_EINVAL : TOOL_OK;
}

/* Build {"target":"pci00:05.0","source":"..."} from an UNESCAPED program. */
static char progbuf[24000];
static const char *run_input(const char *src) {
    static char in[24000];
    size_t n = json_escape(progbuf, sizeof progbuf, src);   /* body, no quotes */
    CHECK(n < sizeof progbuf);
    snprintf(in, sizeof in, "{\"target\":\"pci00:05.0\",\"source\":\"%s\"}", progbuf);
    return in;
}

static const char *asm_input(const char *src) {
    static char in[24000];
    size_t n = json_escape(progbuf, sizeof progbuf, src);
    CHECK(n < sizeof progbuf);
    snprintf(in, sizeof in, "{\"source\":\"%s\"}", progbuf);
    return in;
}

static long acc_base;

static void fresh(void) {
    tlk_reset_device();
    dvm_tools_reset_attempts();
    dvm_tools_set_io(&tlk_io);
    exp_lo      = TLK_BASE;
    exp_hi      = TLK_BASE + 0xFF;
    exp_bdf     = (0 << 8) | (5 << 3) | 0;
    exp_mmio_lo = exp_mmio_hi = 0;
    memset(mspace, 0, sizeof mspace);
    acc_base = tlk_accesses;      /* tlk_accesses is a whole-suite total */
}

/* Device accesses since the last fresh(). */
static long acc_delta(void) { return tlk_accesses - acc_base; }

/* ====================================================================== */
/* programs used by the direct-dispatch tests                             */
/* ====================================================================== */

/* The recorded session's programs, un-escaped, so the same bytes drive both
 * the direct tests and the replay. Decoded from the transcript at startup so
 * there is exactly one copy of each program in the tree. */
static char prog_v1[8192];
static char prog_v2[8192];
static char prog_bad[512];

static void decode_transcript_programs(void) {
    /* The transcript holds JSON; pull the program back out of it the same way
     * the tool does, which also proves the transcript is well-formed. */
    static const char *bodies[3] = { TR_ROUND2, TR_ROUND3, TR_STUCK1 };
    char *dst[3] = { prog_v1, prog_v2, prog_bad };
    size_t cap[3] = { sizeof prog_v1, sizeof prog_v2, sizeof prog_bad };

    for (int i = 0; i < 3; i++) {
        json_value_t root, content, blk, type, input, src;
        CHECK_EQ(json_parse(bodies[i], strlen(bodies[i]), &root), JSON_OK);
        CHECK_EQ(json_msg_content(&root, &content), JSON_OK);
        int found = 0;
        for (size_t b = 0; b < json_count(&content); b++) {
            if (json_at(&content, b, &blk) != JSON_OK) continue;
            if (json_get(&blk, "type", &type) != JSON_OK) continue;
            if (!json_str_eq(&type, "tool_use")) continue;
            CHECK_EQ(json_get(&blk, "input", &input), JSON_OK);
            CHECK_EQ(json_get(&input, "source", &src), JSON_OK);
            CHECK_EQ(json_str(&src, dst[i], cap[i], NULL), JSON_OK);
            found = 1;
        }
        CHECK(found);
    }
    CHECK(strstr(prog_v1, "out32 r5, CTL_ENABLE") != NULL);
    CHECK(strstr(prog_v2, "out16 r3, RATE_DIV") != NULL);
    /* The whole point of the pair: the fix is the ORDER of those two writes. */
    CHECK(strstr(prog_v1, "out32 r5, CTL_ENABLE") < strstr(prog_v1, "out16 r3, RATE_DIV"));
    CHECK(strstr(prog_v2, "out16 r3, RATE_DIV") < strstr(prog_v2, "out32 r5, CTL_ENABLE"));
}

/* ====================================================================== */
/* 1. driver_targets                                                      */
/* ====================================================================== */

static void test_targets(void) {
    fresh();

    CHECK_EQ(call("driver_targets", "{}"), TOOL_OK);
    CHECK_EQ(rres.is_error, 0);
    CHECK_CONTAINS(rbuf, "target=pci00:05.0 00:05.0 1234:5678");
    CHECK_CONTAINS(rbuf, "usable: yes");
    CHECK_CONTAINS(rbuf, "ports 0xd000-0xd0ff; mmio none; pci 00:05.0");
    CHECK_CONTAINS(rbuf, "r0=0xd000");
    CHECK_CONTAINS(rbuf, "r6=bdf 0x28");
    /* A claimed device is not offered at all without include_unusable. */
    CHECK(strstr(rbuf, "pci00:03.0") == NULL);
    CHECK_CONTAINS(rbuf, "PCI function(s) known");
    /* The kernel says how many, in a line the model cannot forge. */
    CHECK_CONTAINS(kcap_text(), "[driver_targets known=");

    /* The neighbour clamp: 00:09.0 is 64 bytes because 00:0a.0 starts there. */
    CHECK_CONTAINS(rbuf, "ports 0xe000-0xe03f");
    /* ...while an isolated I/O BAR gets the PCI maximum of 256 bytes. */
    CHECK_CONTAINS(rbuf, "ports 0xd100-0xd1ff");
    /* A memory BAR is granted by its alignment, capped at 1 MiB, and stopped
     * at the next BAR on the bus (0xfebc0000 here). */
    CHECK_CONTAINS(rbuf, "mmio 0xfeb00000+0xc0000");
}

static void test_targets_refusals(void) {
    fresh();

    CHECK_EQ(call("driver_targets", "{\"include_unusable\":true,\"limit\":64}"), TOOL_OK);
    CHECK_CONTAINS(rbuf, "driver \"e1000fake\" is already bound to pci00:03.0");
    CHECK_CONTAINS(rbuf, "pci00:04.0");
    CHECK_CONTAINS(rbuf, "PCI bus mastering enabled");
    CHECK_CONTAINS(rbuf, "no programmed BAR");
    CHECK_CONTAINS(rbuf, "interrupt-controller block");
    CHECK_CONTAINS(rbuf, "COM1");          /* the VM's own words, via dvm_policy_check */

    /* Bad arguments are refused, not guessed at. */
    CHECK(call("driver_targets", "{\"limit\":0}") != TOOL_OK);
    CHECK_CONTAINS(rbuf, "error: \"limit\" must be an integer 1-64");
    CHECK(call("driver_targets", "{\"limit\":\"lots\"}") != TOOL_OK);
    CHECK(call("driver_targets", "{\"include_unusable\":7}") != TOOL_OK);
    CHECK(call("driver_targets", "[]") != TOOL_OK);
    CHECK_CONTAINS(rbuf, "input must be a JSON object");
}

/* The windows the model would really be handed on the machine this kernel
 * boots. Exact numbers, including where the inference is imprecise, because
 * "roughly the right window" is not a security property. */
static void test_qemu_bus_layout(void) {
    fresh();
    CHECK_EQ(call("driver_targets", "{\"limit\":64}"), TOOL_OK);

    /* IDE: BAR4 is 16 bytes; alignment only proves 64, and nothing else on the
     * bus sits above 0xc540, so 64 is what is granted. Over by 48 bytes of
     * unallocated I/O space — the price of not writing config space to size a
     * BAR. */
    CHECK_CONTAINS(rbuf, "target=pci01:01.1");
    CHECK_CONTAINS(rbuf, "ports 0xc540-0xc57f; mmio none; pci 01:01.1");

    /* e1000: BAR1 is exactly 64 bytes and the clamp finds it exactly, because
     * the IDE BAR starts 64 bytes later. BAR0 is 128 KiB; alignment says 256
     * KiB and the VGA BAR2 above it cuts that to 192 KiB. */
    CHECK_CONTAINS(rbuf, "ports 0xc500-0xc53f; mmio 0xfebc0000+0x30000");

    /* AC97: BAR0 is 1 KiB but I/O windows are capped at the PCI maximum of
     * 256 bytes, so the mixer block is reachable and the rest is not. BAR1 is
     * 256 bytes and comes out exact. */
    CHECK_CONTAINS(rbuf, "ports 0xc000-0xc0ff,0xc400-0xc4ff");

    /* VGA: NOT LISTED AS USABLE, AND NO WINDOW IS EVER PRINTED FOR IT.
     *
     * This is the one that matters most on this machine. Its BAR0 is the stdvga
     * VRAM linear aperture and its BAR2+0x400 is the legacy CRTC/sequencer
     * block, so the window this arithmetic WOULD have produced
     * ("mmio 0xfd000000+0x100000,0xfebf0000+0x10000") is read-write access to
     * the operator's console at an address vm/dvm.c's deny list cannot recognise
     * as the console — a second door to the same character memory it guards at
     * 0xb8000. A program with it can repaint the transcript and forge a
     * [bracketed] kernel line. So the tool refuses the whole device class, and
     * this asserts that the numbers never appear in anything the model reads. */
    CHECK(strstr(rbuf, "0xfd000000") == NULL);
    CHECK(strstr(rbuf, "0xfebf0000") == NULL);

    /* And the BAR-less functions QEMU's chipset is mostly made of. */
    CHECK_EQ(call("driver_run", "{\"target\":\"pci01:00.0\",\"source\":\"halt\"}"),
             TOOL_EINVAL);
    CHECK_CONTAINS(rbuf, "no programmed BAR");
    CHECK_EQ(forbidden, 0);
}

/* ====================================================================== */
/* 2. driver_assemble                                                     */
/* ====================================================================== */

static void test_assemble(void) {
    fresh();

    CHECK_EQ(call("driver_assemble",
                  "{\"source\":\"mov r1, 5\\nadd r1, 2\\nhalt\\n\",\"listing\":true}"),
             TOOL_OK);
    CHECK_EQ(rres.is_error, 0);
    CHECK_CONTAINS(rbuf, "ok: 3 instructions");
    CHECK_CONTAINS(rbuf, "add r1, r1, 2");        /* the shorthand, expanded */
    CHECK_CONTAINS(rbuf, "this only parsed the program");

    /* An error names the line and quotes it back. */
    CHECK(call("driver_assemble",
               "{\"source\":\"mov r1, 5\\noutq 0x10, r1\\nhalt\\n\"}") != TOOL_OK);
    CHECK_EQ(rres.is_error, 1);
    CHECK_CONTAINS(rbuf, "assembly failed at line 2");
    CHECK_CONTAINS(rbuf, "outq");
    CHECK_CONTAINS(rbuf, "line 2: outq 0x10, r1");

    /* An undefined label is caught before anything can run. */
    CHECK(call("driver_assemble", "{\"source\":\"jmp nowhere\\nhalt\\n\"}") != TOOL_OK);
    CHECK_CONTAINS(rbuf, "assembly failed at line 1");

    /* Assembling never touches the device. */
    CHECK_EQ(acc_delta(), 0);

    /* Argument validation. */
    CHECK(call("driver_assemble", "{}") != TOOL_OK);
    CHECK_CONTAINS(rbuf, "\"source\" is required");
    CHECK(call("driver_assemble", "{\"source\":42}") != TOOL_OK);
    CHECK_CONTAINS(rbuf, "\"source\" must be a string");
    CHECK(call("driver_assemble", "{\"source\":\"\"}") != TOOL_OK);
    CHECK_CONTAINS(rbuf, "\"source\" is empty");
    CHECK(call("driver_assemble", "{\"source\":\"halt\",\"listing\":\"yes\"}") != TOOL_OK);
}

static void test_assemble_oversized(void) {
    fresh();
    /* A program larger than the whole source budget is a clean refusal, not a
     * buffer problem. */
    static char big[DVM_SRC_MAX * 2];
    static char in[DVM_SRC_MAX * 2 + 64];
    size_t n = 0;
    while (n < sizeof big - 8) { memcpy(big + n, "nop\\n", 5); n += 5; }
    big[n] = '\0';
    snprintf(in, sizeof in, "{\"source\":\"%s\"}", big);

    CHECK(call("driver_assemble", in) != TOOL_OK);
    CHECK_CONTAINS(rbuf, "longer than");
    CHECK_EQ(acc_delta(), 0);
}

/* ====================================================================== */
/* 3. driver_run: the good program                                        */
/* ====================================================================== */

static void test_run_success(void) {
    fresh();

    CHECK_EQ(call_cap("driver_run", run_input(prog_v2), CHAT_TOOL_RESULT_CAP), TOOL_OK);
    CHECK_EQ(rres.is_error, 0);

    CHECK_CONTAINS(rbuf, "target=pci00:05.0 00:05.0 1234:5678 attempt 1 of 5");
    CHECK_CONTAINS(rbuf, "sandbox: ports 0xd000-0xd0ff; mmio none; pci 00:05.0");
    CHECK_CONTAINS(rbuf, "status=OK");

    /* The device really is up: ready, running, rate latched, counter moving. */
    CHECK_EQ(tlk.ready, 1);
    CHECK_EQ(tlk.running, 1);
    CHECK_EQ(tlk.rate, 0x100u);
    CHECK(tlk.count > 0);

    /* ...and the program's own output channel says the same thing. */
    CHECK_CONTAINS(rbuf, "r8=0x4b4c5401");    /* the identity it read back */
    CHECK_CONTAINS(rbuf, "r11=0x100");        /* the rate, read back        */
    CHECK_CONTAINS(rbuf, "r12=0x3");          /* status: ready | running    */
    CHECK_CONTAINS(rbuf, "r0=0xd000");        /* the BAR the kernel handed in */

    /* The kernel's ground truth, not the model's. */
    CHECK_CONTAINS(kcap_text(), "[dvm.grant ports 0xd000-0xd0ff -> ok]");
    CHECK_CONTAINS(kcap_text(), "[dvm.grant pci 00:05.0 -> ok]");
    CHECK_CONTAINS(kcap_text(), "[dvm.halt");
    CHECK_CONTAINS(kcap_text(), "[driver_run pci00:05.0 attempt=1");
    CHECK_CONTAINS(kcap_text(), "-> ok]");

    /* The whole answer fitted the budget chat.c really gives a tool. */
    CHECK_EQ(rres.truncated, 0);
    CHECK(rres.len < CHAT_TOOL_RESULT_CAP);

    /* A clean run clears the attempt count. */
    CHECK_EQ((int)attempts_used("pci00:05.0"), 0);
    CHECK_EQ(forbidden, 0);
}

/* ====================================================================== */
/* 4. driver_run: the failing program, and what comes back                */
/* ====================================================================== */

static void test_run_failure_feedback(void) {
    fresh();

    CHECK(call_cap("driver_run", run_input(prog_v1), CHAT_TOOL_RESULT_CAP) != TOOL_OK);
    CHECK_EQ(rres.is_error, 1);

    /* Everything the model needs to fix it, in one answer. */
    CHECK_CONTAINS(rbuf, "status=ABORT at line");
    CHECK_CONTAINS(rbuf, "rate did not stick");
    CHECK_CONTAINS(rbuf, "instruction: abort");
    CHECK_CONTAINS(rbuf, "abort \"rate did not stick\"");   /* its own source line */
    CHECK_CONTAINS(rbuf, "4 attempt(s) left on this target");

    /* The access log is the evidence: the rate write, then the readback of 0. */
    CHECK_CONTAINS(rbuf, "out16 0xd00c <= 0x100");
    CHECK_CONTAINS(rbuf, "in16 0xd00c => 0x0");

    /* The device was left enabled but unconfigured — which is the truth. */
    CHECK_EQ(tlk.running, 1);
    CHECK_EQ(tlk.rate, 0u);

    CHECK_EQ((int)attempts_used("pci00:05.0"), 1);
    CHECK_EQ(forbidden, 0);
    CHECK_CONTAINS(kcap_text(), "[driver_run pci00:05.0 attempt=1 ABORT line=");
    CHECK_CONTAINS(kcap_text(), "-> EINVAL]");

    /* Exactly what the model is handed to revise from — the same bytes chat.c
     * puts in the tool_result. Printed on request, because a reviewer should be
     * able to read it rather than take this suite's word for it. */
    if (getenv("DVM_SHOW_TRANSCRIPT"))
        printf("\n----- the tool_result the model reads after attempt 1 -----\n%s"
               "-----------------------------------------------------------\n", rbuf);
}

/* ====================================================================== */
/* 5. driver_run: the sandbox holds                                       */
/* ====================================================================== */

static void test_run_denied_port(void) {
    fresh();

    /* Inside the BAR, then one byte past the end of it. */
    CHECK(call("driver_run", run_input(
        "    in32 r1, r0\n"
        "    mov  r2, 0xd100\n"
        "    in8  r3, r2\n"
        "    halt\n")) != TOOL_OK);
    CHECK_CONTAINS(rbuf, "status=PORT_DENIED");
    CHECK_CONTAINS(rbuf, "0xd100");
    CHECK_CONTAINS(rbuf, "instruction: in8 r3, r2");
    /* The legal access before it still shows, so the model can see how far it got. */
    CHECK_CONTAINS(rbuf, "in32 0xd000 => 0x4b4c5401");
    CHECK_EQ(forbidden, 0);            /* the neighbour was never touched */

    /* The compiled-in deny list, reached through the tool. */
    fresh();
    CHECK(call("driver_run", run_input("    out8 0x64, 0xfe\n    halt\n")) != TOOL_OK);
    CHECK_CONTAINS(rbuf, "status=PORT_DENIED");
    CHECK_CONTAINS(rbuf, "PS/2");
    CHECK_EQ(forbidden, 0);

    /* MMIO, when no MMIO window was granted. */
    fresh();
    CHECK(call("driver_run", run_input("    ld32 r1, [0xfebd0000]\n    halt\n")) != TOOL_OK);
    CHECK_CONTAINS(rbuf, "status=MMIO_DENIED");
    CHECK_EQ(forbidden, 0);

    /* Another device's config space. */
    fresh();
    CHECK(call("driver_run", run_input("    pcicfg r1, 00:03.0, 0x00\n    halt\n")) != TOOL_OK);
    CHECK_CONTAINS(rbuf, "status=PCI_DENIED");
    CHECK_EQ(forbidden, 0);

    /* Its own function is allowed, and reads real config space. */
    fresh();
    CHECK_EQ(call("driver_run", run_input(
        "    pcicfg r1, r6, 0x00\n"
        "    halt\n")), TOOL_OK);
    CHECK_CONTAINS(rbuf, "r1=0x56781234");
    CHECK_CONTAINS(rbuf, "pcicfg 00:05.0 off 0x00 => 0x56781234");
    CHECK_EQ(forbidden, 0);
}

/* A memory BAR earns an MMIO window, and ld/st reach it — the other half of
 * the sandbox, which the port tests never touch. */
static void test_run_mmio_window(void) {
    fresh();
    exp_mmio_lo = 0xFEB00000ull;
    exp_mmio_hi = 0xFEB00000ull + 0xC0000ull - 1;
    exp_bdf     = (0 << 8) | (12 << 3) | 0;

    CHECK_EQ(call("driver_run",
                  "{\"target\":\"pci00:0c.0\",\"source\":\""
                  "    st32 [r0+0x10], 0xa5a5\\n"
                  "    ld32 r1, [r0+0x10]\\n"
                  "    ld8  r2, [r0+0x11]\\n"
                  "    halt\\n\"}"), TOOL_OK);
    CHECK_CONTAINS(rbuf, "sandbox: ports none; mmio 0xfeb00000+0xc0000");
    CHECK_CONTAINS(rbuf, "r1=0xa5a5");
    CHECK_CONTAINS(rbuf, "r2=0xa5");
    CHECK_CONTAINS(rbuf, "st32 0xfeb00010 <= 0xa5a5");
    CHECK_CONTAINS(rbuf, "ld32 0xfeb00010 => 0xa5a5");

    /* One byte past the granted window is refused, and never reaches the
     * device — even though the device would happily have answered. */
    CHECK(call("driver_run",
               "{\"target\":\"pci00:0c.0\",\"source\":\""
               "    ld32 r1, [0xfebc0000]\\n"
               "    halt\\n\"}") != TOOL_OK);
    CHECK_CONTAINS(rbuf, "status=MMIO_DENIED");
    CHECK_EQ(forbidden, 0);
}

static void test_run_bounded(void) {
    fresh();
    /* An unbounded loop stops on the cycle limit, not on the operator. */
    CHECK(call("driver_run",
               "{\"target\":\"pci00:05.0\",\"source\":\"loop:\\n jmp loop\\n\","
               "\"max_steps\":500}") != TOOL_OK);
    CHECK_CONTAINS(rbuf, "status=STEP_LIMIT");
    CHECK_CONTAINS(rbuf, "ran 500 steps");

    /* A delay longer than the budget is refused mid-run, with the budget named. */
    fresh();
    CHECK(call("driver_run",
               "{\"target\":\"pci00:05.0\",\"source\":\"loop:\\n delay 50000\\n jmp loop\\n\","
               "\"delay_budget_ms\":10}") != TOOL_OK);
    CHECK_CONTAINS(rbuf, "status=DELAY_LIMIT");

    /* And the knobs themselves are bounded. */
    fresh();
    CHECK(call("driver_run",
               "{\"target\":\"pci00:05.0\",\"source\":\"halt\","
               "\"delay_budget_ms\":9999}") != TOOL_OK);
    CHECK_CONTAINS(rbuf, "\"delay_budget_ms\" must be 0-2000");
    CHECK(call("driver_run",
               "{\"target\":\"pci00:05.0\",\"source\":\"halt\","
               "\"max_steps\":0}") != TOOL_OK);
    CHECK_CONTAINS(rbuf, "\"max_steps\" must be 1-1000000");
    CHECK(call("driver_run",
               "{\"target\":\"pci00:05.0\",\"source\":\"halt\","
               "\"trace\":\"loud\"}") != TOOL_OK);
    CHECK_CONTAINS(rbuf, "\"trace\" must be");
    CHECK_EQ(forbidden, 0);
}

/* ====================================================================== */
/* 6. driver_run: target validation                                       */
/* ====================================================================== */

static void test_run_target_validation(void) {
    fresh();

    CHECK(call("driver_run", "{\"source\":\"halt\"}") != TOOL_OK);
    CHECK_CONTAINS(rbuf, "\"target\" is required");

    CHECK(call("driver_run", "{\"target\":\"pci00:05.0\"}") != TOOL_OK);
    CHECK_CONTAINS(rbuf, "\"source\" is required");

    CHECK(call("driver_run", "{\"target\":\"pci99:99.9\",\"source\":\"halt\"}") != TOOL_OK);
    CHECK_CONTAINS(rbuf, "no device named");

    /* The bare address form pci_list prints also resolves — through the
     * registry, so an address with no device node is still refused. */
    CHECK_EQ(call("driver_run", "{\"target\":\"00:05.0\",\"source\":\"halt\"}"), TOOL_OK);
    CHECK_CONTAINS(rbuf, "target=pci00:05.0");
    CHECK(call("driver_run", "{\"target\":\"00:1e.0\",\"source\":\"halt\"}") != TOOL_OK);
    CHECK_CONTAINS(rbuf, "no device named");

    /* A registered device that is not PCI has no BARs to derive a window from. */
    CHECK(call("driver_run", "{\"target\":\"serial0\",\"source\":\"halt\"}") != TOOL_OK);
    CHECK_CONTAINS(rbuf, "not a PCI device");

    /* A node whose function has since gone away. */
    CHECK(call("driver_run", "{\"target\":\"pci00:1f.0\",\"source\":\"halt\"}") != TOOL_OK);
    CHECK_CONTAINS(rbuf, "nothing is answering");

    /* Claimed, bus-mastering, and unusable BARs are refused here too, not just
     * listed as unusable by driver_targets. */
    CHECK(call("driver_run", "{\"target\":\"pci00:03.0\",\"source\":\"halt\"}") != TOOL_OK);
    CHECK_CONTAINS(rbuf, "already bound");
    CHECK(call("driver_run", "{\"target\":\"pci00:04.0\",\"source\":\"halt\"}") != TOOL_OK);
    CHECK_CONTAINS(rbuf, "bus mastering");
    CHECK(call("driver_run", "{\"target\":\"pci00:06.0\",\"source\":\"halt\"}") != TOOL_OK);
    CHECK_CONTAINS(rbuf, "not permitted");
    CHECK(call("driver_run", "{\"target\":\"pci00:07.0\",\"source\":\"halt\"}") != TOOL_OK);
    CHECK_CONTAINS(rbuf, "interrupt-controller block");
    CHECK(call("driver_run", "{\"target\":\"pci00:00.0\",\"source\":\"halt\"}") != TOOL_OK);
    CHECK_CONTAINS(rbuf, "no programmed BAR");

    /* An I/O BAR whose base does not fit x86's 16-bit I/O space: refused, not
     * truncated into a window over some other device. */
    CHECK(call("driver_run", "{\"target\":\"pci00:0d.0\",\"source\":\"halt\"}") != TOOL_OK);
    CHECK_CONTAINS(rbuf, "outside the 64 KiB");

    /* A driverless PCI node whose BAR is nonetheless driven by another device.
     * This is the shape of pci00:03.0 vs eth0 on the real machine, and getting
     * it wrong would let a program poke a live NIC mid-conversation. */
    CHECK(call("driver_run", "{\"target\":\"pci00:0b.0\",\"source\":\"halt\"}") != TOOL_OK);
    CHECK_CONTAINS(rbuf, "already driven by \"e1000fake\" as device \"snd0\"");

    /* None of those reached the device, and none of them cost an attempt on a
     * target that was never run against. */
    CHECK_EQ(forbidden, 0);
    CHECK_EQ((int)attempts_used("pci00:03.0"), 0);
}

/* ====================================================================== */
/* 7. garbage in                                                          */
/* ====================================================================== */

static void test_garbage_input(void) {
    fresh();

    /* Truncated JSON. */
    CHECK(call("driver_run", "{\"target\":\"pci00:05.0\",\"source\":\"hal") != TOOL_OK);
    CHECK_CONTAINS(rbuf, "input must be a JSON object");

    /* Not an object. */
    CHECK(call("driver_run", "\"halt\"") != TOOL_OK);
    CHECK(call("driver_run", "") != TOOL_OK);
    CHECK(call("driver_run", "null") != TOOL_OK);

    /* Wrong types everywhere. */
    CHECK(call("driver_run", "{\"target\":5,\"source\":\"halt\"}") != TOOL_OK);
    CHECK_CONTAINS(rbuf, "\"target\" must be a string");
    CHECK(call("driver_run", "{\"target\":\"pci00:05.0\",\"source\":[1,2]}") != TOOL_OK);
    CHECK(call("driver_run", "{\"target\":\"pci00:05.0\",\"source\":null}") != TOOL_OK);

    /* An absurd device name cannot match and cannot overrun. */
    {
        static char in[2048];
        int n = snprintf(in, sizeof in, "{\"target\":\"");
        for (int i = 0; i < 1500; i++) in[n++] = 'x';
        snprintf(in + n, sizeof in - (size_t)n, "\",\"source\":\"halt\"}");
        CHECK(call("driver_run", in) != TOOL_OK);
        CHECK_CONTAINS(rbuf, "no such device");
    }

    /* A source span that is not NUL-terminated and holds an embedded NUL. The
     * NUL ends the line as far as the assembler is concerned, so this program
     * is `mov r1, 1` with no halt after it: a bounded trap, not a crash, and
     * the model is told exactly that. */
    {
        static const char raw[] =
            "{\"target\":\"pci00:05.0\",\"source\":\"mov r1, 1\\u0000halt\"}TRAILING";
        CHECK(call_raw("driver_run", raw, strlen(raw) - 8) != TOOL_OK);
        CHECK_EQ(rres.is_error, 1);
        CHECK_CONTAINS(rbuf, "status=BAD_PC");
        CHECK_CONTAINS(rbuf, "every path must end in halt or abort");
    }

    /* Bytes that are not text at all. */
    {
        char noise[512];
        for (size_t i = 0; i < sizeof noise; i++) noise[i] = (char)(i * 7 + 3);
        CHECK(call_raw("driver_run", noise, sizeof noise) != TOOL_OK);
    }

    CHECK_EQ(acc_delta(), 0);
    CHECK_EQ(forbidden, 0);
    CHECK_EQ(kpanic_hit, 0);
}

/* ====================================================================== */
/* 8. the attempt bound                                                   */
/* ====================================================================== */

static void test_attempt_budget(void) {
    fresh();

    const char *in = run_input("    out8 0x64, 0xfe\n    halt\n");
    static char keep[24000];
    snprintf(keep, sizeof keep, "%s", in);

    for (int i = 1; i <= 5; i++) {
        CHECK(call("driver_run", keep) != TOOL_OK);
        char want[64];
        snprintf(want, sizeof want, "attempt %d of 5", i);
        CHECK_CONTAINS(rbuf, want);
        CHECK_CONTAINS(rbuf, "status=PORT_DENIED");
        CHECK_EQ((int)attempts_used("pci00:05.0"), i);
    }
    CHECK_CONTAINS(rbuf, "no attempts left on this target");

    /* The sixth is refused without assembling or running anything. */
    long before = tlk_accesses;
    CHECK(call("driver_run", keep) != TOOL_OK);
    CHECK_CONTAINS(rbuf, "5 attempts against pci00:05.0 have all failed");
    CHECK_CONTAINS(rbuf, "Stop here");
    CHECK_EQ(tlk_accesses, before);   /* nothing ran */
    CHECK_EQ((int)attempts_used("pci00:05.0"), 5);

    /* A different target has its own budget. */
    CHECK(call("driver_run", "{\"target\":\"pci00:08.0\",\"source\":\"halt\"}") == TOOL_OK);

    /* An assembly failure spends an attempt too: it is still a turn spent
     * against that device. */
    dvm_tools_reset_attempts();
    CHECK_EQ(call("driver_run", run_input(prog_v2)), TOOL_OK);   /* a full log */
    CHECK(call("driver_run", "{\"target\":\"pci00:05.0\",\"source\":\"frobnicate\"}")
          != TOOL_OK);
    CHECK_CONTAINS(rbuf, "attempt 1 of 5");
    CHECK_CONTAINS(rbuf, "the device was not touched");
    CHECK_EQ((int)attempts_used("pci00:05.0"), 1);
    /* ...and the trace of the attempt that did not run is empty, rather than
     * the previous run's accesses filed under this attempt's verdict. */
    CHECK_EQ(call("driver_trace", "{}"), TOOL_OK);
    CHECK_CONTAINS(rbuf, "status=BAD_PROGRAM");
    CHECK_CONTAINS(rbuf, "no device events were recorded");

    /* A success wipes the slate. */
    CHECK_EQ(call("driver_run", run_input(prog_v2)), TOOL_OK);
    CHECK_EQ((int)attempts_used("pci00:05.0"), 0);
    CHECK_EQ(forbidden, 0);
}

/* The budget must not be something the model can clear for itself. Two ways it
 * could, both closed here:
 *
 *   (a) a one-instruction `halt`. It reaches halt, so it used to be recorded as
 *       a success and zeroed the failure count — while touching nothing. Four
 *       failures, one no-op, repeat, forever, on the stock machine with a single
 *       target. This is the one that mattered.
 *   (b) target churn. attempts_used() reports 0 for a name the 8-entry table has
 *       forgotten, so evicting a nearly-exhausted target handed its budget back.
 */
static void test_budget_is_not_self_service(void) {
    fresh();

    const char *deny = run_input("    out8 0x64, 0xfe\n    halt\n");
    static char keep[24000];
    snprintf(keep, sizeof keep, "%s", deny);

    /* (a) a no-op halt is NOT convergence. */
    for (int i = 1; i <= 4; i++) CHECK(call("driver_run", keep) != TOOL_OK);
    CHECK_EQ((int)attempts_used("pci00:05.0"), 4);

    long before = tlk_accesses;
    CHECK_EQ(call("driver_run", "{\"target\":\"pci00:05.0\",\"source\":\"halt\"}"),
             TOOL_OK);                            /* the run itself still succeeds */
    CHECK_CONTAINS(rbuf, "status=OK");
    CHECK_CONTAINS(rbuf, "without touching the device");
    CHECK_EQ(tlk_accesses, before);               /* it really did nothing */
    CHECK_EQ((int)attempts_used("pci00:05.0"), 5);/* ...and it SPENT the attempt */

    CHECK(call("driver_run", keep) != TOOL_OK);
    CHECK_CONTAINS(rbuf, "5 attempts against pci00:05.0 have all failed");

    /* A program that really does speak to the device still clears the count —
     * the budget is a brake on non-convergence, not a cap on work. */
    dvm_tools_reset_attempts();
    for (int i = 1; i <= 4; i++) CHECK(call("driver_run", keep) != TOOL_OK);
    CHECK_EQ((int)attempts_used("pci00:05.0"), 4);
    CHECK_EQ(call("driver_run", run_input(prog_v2)), TOOL_OK);
    CHECK_EQ((int)attempts_used("pci00:05.0"), 0);

    /* (b) churn. Only 8 functions on the fixture bus are targetable, and the
     * table holds 8, so the eviction path cannot be reached through the tool
     * here — it needs a machine with nine targetable devices, which is exactly
     * why it went unnoticed. Drive attempts_record() directly instead: it is
     * the function under test, and the two invariants are about it alone.
     *
     * Real targets are used for the names so that attempts_used() is being asked
     * the same question driver_run asks it. */
    dvm_tools_reset_attempts();

    static const char *names[] = {
        "pci00:05.0", "pci00:08.0", "pci00:09.0", "pci00:0a.0",
        "pci00:0c.0", "pci01:01.1", "pci01:03.0", "pci01:04.0",
    };
    const int NN = (int)(sizeof names / sizeof names[0]);   /* == the table size */

    /* INVARIANT 1 — no inherited history. Fill every slot to 3 failures, then
     * let a ninth target arrive. It must start from its OWN first failure: the
     * old code wrote the new name over the evicted entry but computed
     * `fails = evicted->fails + 1`, so a never-before-attempted device could be
     * refused with "5 attempts against ... have all failed" having had one. */
    for (int rep = 0; rep < 3; rep++)
        for (int i = 0; i < NN; i++) attempts_record(names[i], 0);
    for (int i = 0; i < NN; i++) CHECK_EQ((int)attempts_used(names[i]), 3);

    attempts_record("pci00:0e.0", 0);
    CHECK_EQ((int)attempts_used("pci00:0e.0"), 1);      /* not 4 */

    /* INVARIANT 2 — a nearly-exhausted target is not the one forgotten.
     * attempts_used() reports 0 for a name the table no longer holds, so
     * evicting the entry closest to its budget would hand that budget straight
     * back, and naming eight other devices once each would be a reset the model
     * performs on itself. Eviction takes the LEAST-failed entry instead. */
    dvm_tools_reset_attempts();
    for (int i = 0; i < 4; i++) attempts_record(names[0], 0);   /* A: 4 failures */
    for (int i = 1; i < NN; i++) attempts_record(names[i], 0);  /* others: 1 each */
    CHECK_EQ((int)attempts_used(names[0]), 4);

    for (int i = 0; i < 16; i++) {                 /* churn a further 16 targets */
        char nm[32];
        snprintf(nm, sizeof nm, "pci07:%02x.0", i);
        attempts_record(nm, 0);
        CHECK_EQ((int)attempts_used(nm), 1);       /* every newcomer starts at 1 */
        CHECK_EQ((int)attempts_used(names[0]), 4); /* and A never loses its count */
    }

    /* ...and the refusal really fires through the tool after the churn. */
    CHECK(call("driver_run", keep) != TOOL_OK);
    CHECK_EQ((int)attempts_used(names[0]), 5);
    CHECK(call("driver_run", keep) != TOOL_OK);
    CHECK_CONTAINS(rbuf, "5 attempts against pci00:05.0 have all failed");
    CHECK_EQ(forbidden, 0);
}

/* The display adapter is the operator's console, and its BARs are a second
 * decode path to the character memory vm/dvm.c's deny list protects at 0xb8000.
 * driver_run's description promises a program "can only reach the device you
 * named - not ... the console"; this is what makes that true. */
static void test_console_is_not_a_target(void) {
    fresh();

    /* driver_targets lists it, and says no. */
    CHECK_EQ(call_cap("driver_targets",
                      "{\"limit\":64,\"include_unusable\":true}", sizeof rbuf),
             TOOL_OK);
    CHECK_CONTAINS(rbuf, "target=pci01:02.0");
    CHECK_CONTAINS(rbuf, "display controller");
    CHECK_CONTAINS(rbuf, "operator's console");
    /* No window is printed for it, at either of its two addresses. */
    CHECK(strstr(rbuf, "0xfd000000") == NULL);
    CHECK(strstr(rbuf, "0xfebf0000") == NULL);

    /* driver_run refuses before assembling, running or spending an attempt. */
    long before = tlk_accesses;
    CHECK(call("driver_run",
               "{\"target\":\"pci01:02.0\",\"source\":"
               "\"st16 [r0+3200], 0x0f5b\\nhalt\\n\"}") != TOOL_OK);
    CHECK_CONTAINS(rbuf, "no display device is targetable");
    CHECK_EQ(tlk_accesses, before);
    CHECK_EQ((int)attempts_used("pci01:02.0"), 0);

    /* driver_assemble is unaffected: syntax checking touches no hardware and
     * names no target, so there is nothing to protect there. */
    CHECK_EQ(call("driver_assemble", "{\"source\":\"st16 [r0+0], 1\\nhalt\\n\"}"),
             TOOL_OK);
    CHECK_EQ(forbidden, 0);
}

/* ====================================================================== */
/* 9. driver_trace                                                        */
/* ====================================================================== */

static void test_trace_tool(void) {
    fresh();

    /* Nothing has run in this process state? The log is per-run, so run first. */
    CHECK_EQ(call("driver_run", run_input(prog_v2)), TOOL_OK);

    CHECK_EQ(call("driver_trace", "{}"), TOOL_OK);
    CHECK_CONTAINS(rbuf, "last run: target=pci00:05.0 attempt 1 status=OK");
    CHECK_CONTAINS(rbuf, "in32 0xd000 => 0x4b4c5401");
    CHECK_CONTAINS(rbuf, "shown ");

    /* Paging from the start. */
    CHECK_EQ(call("driver_trace", "{\"offset\":0,\"limit\":2}"), TOOL_OK);
    CHECK_CONTAINS(rbuf, "(events 1-2)");

    /* ...and from the end. */
    CHECK_EQ(call("driver_trace", "{\"limit\":3,\"tail\":true}"), TOOL_OK);
    CHECK_CONTAINS(rbuf, "shown 3 of ");

    CHECK(call("driver_trace", "{\"offset\":9999}") != TOOL_OK);
    CHECK_CONTAINS(rbuf, "past the end");
    CHECK(call("driver_trace", "{\"limit\":0}") != TOOL_OK);
    CHECK(call("driver_trace", "{\"tail\":\"yes\"}") != TOOL_OK);

    /* More events than the ring holds: the answer says so rather than lying
     * about which access was first. */
    fresh();
    CHECK(call("driver_run",
               "{\"target\":\"pci00:05.0\",\"source\":\""
               "    mov r9, 400\\n"
               "loop:\\n"
               "    in32 r1, r0\\n"
               "    sub  r9, 1\\n"
               "    cmp  r9, 0\\n"
               "    bne  loop\\n"
               "    halt\\n\"}") == TOOL_OK);
    CHECK_EQ(call("driver_trace", "{\"limit\":2}"), TOOL_OK);
    CHECK_CONTAINS(rbuf, "400 events happened; the last 256 are kept");
    CHECK_CONTAINS(rbuf, "(events 145-146)");
    CHECK_EQ(forbidden, 0);
}

static void test_trace_before_any_run(void) {
    /* Fresh state is asserted by running this before anything else touches
     * g_last — see the order in main(). */
    CHECK(call("driver_trace", "{}") != TOOL_OK);
    CHECK_CONTAINS(rbuf, "no program has been run yet");
}

/* ====================================================================== */
/* 10. no I/O backend                                                     */
/* ====================================================================== */

static void test_no_backend(void) {
    fresh();
    dvm_tools_set_io(NULL);           /* dvm_io_hardware() is NULL on the host */
    CHECK(call("driver_run", "{\"target\":\"pci00:05.0\",\"source\":\"halt\"}") != TOOL_OK);
    CHECK_CONTAINS(rbuf, "no hardware I/O backend");
    dvm_tools_set_io(&tlk_io);
}

/* ====================================================================== */
/* 11. the recorded session, replayed through the real turn loop          */
/* ====================================================================== */

static void queue(const char *body) { CHECK_EQ(model_mock_queue(200, body), MODEL_OK); }

static void test_golden_transcript(void) {
    fresh();
    model_mock_reset();
    chat_reset();
    chat_set_max_rounds(0);
    chat_init(model_mock_transport());
    kcap_reset();

    queue(TR_ROUND1);
    queue(TR_ROUND2);
    queue(TR_ROUND3);
    queue(TR_ROUND4);

    CHECK_EQ(chat_ask(TR_SENTENCE), CHAT_OK);

    /* Four round trips: three tool rounds and the answer. */
    CHECK_EQ((int)model_mock_request_count(), 4);
    CHECK_EQ((int)chat_last_rounds(), 4);
    CHECK_EQ((int)chat_last_tool_calls(), 3);
    CHECK_EQ((int)model_mock_pending(), 0);

    const char *console = kcap_text();

    /* --- what the kernel did, in the kernel's own words --- */
    CHECK_CONTAINS(console, "[driver_targets known=");
    CHECK_CONTAINS(console, "[dvm.grant ports 0xd000-0xd0ff -> ok]");
    CHECK_CONTAINS(console, "[dvm.grant mmio none -> ok]");
    CHECK_CONTAINS(console, "[dvm.grant pci 00:05.0 -> ok]");
    CHECK_CONTAINS(console, "[dvm.trap");
    CHECK_CONTAINS(console, "ABORT: rate did not stick");
    CHECK_CONTAINS(console, "[driver_run pci00:05.0 attempt=1 ABORT");
    CHECK_CONTAINS(console, "[driver_run pci00:05.0 attempt=2");
    CHECK_CONTAINS(console, "[dvm.halt");
    /* The program's own print lines, for the operator. */
    CHECK_CONTAINS(console, "[dvm.print id = 0x4b4c5401 -> ok]");
    CHECK_CONTAINS(console, "[dvm.print count =");

    /* --- the target list survived chat.c's 1 KiB result clip WITH the row the
     * model has to act on: that is why usable targets are listed first --- */
    const char *req2 = model_mock_request(1);
    CHECK(req2 != NULL);
    CHECK_CONTAINS(req2, "target=pci00:05.0");
    CHECK_CONTAINS(req2, "usable: yes  sandbox: ports 0xd000-0xd0ff");

    /* --- what the model saw between the two attempts --- */
    const char *req3 = model_mock_request(2);
    CHECK(req3 != NULL);
    CHECK_CONTAINS(req3, "status=ABORT");
    CHECK_CONTAINS(req3, "rate did not stick");
    CHECK_CONTAINS(req3, "out16 0xd00c <= 0x100");   /* the write... */
    CHECK_CONTAINS(req3, "in16 0xd00c => 0x0");      /* ...and the readback */
    CHECK_CONTAINS(req3, "attempt(s) left on this target");
    /* ...marked as a failure, so the model knows the action did not happen. */
    CHECK_CONTAINS(req3, "\"is_error\":true");

    /* --- and what the last request carried: a successful run --- */
    const char *req4 = model_mock_request(3);
    CHECK(req4 != NULL);
    CHECK_CONTAINS(req4, "status=OK - the program reached halt");
    CHECK_CONTAINS(req4, "r11=0x100");

    /* --- the device is genuinely up --- */
    CHECK_EQ(tlk.ready, 1);
    CHECK_EQ(tlk.running, 1);
    CHECK_EQ(tlk.rate, 0x100u);
    CHECK(tlk.count > 0);
    CHECK_EQ(forbidden, 0);
    CHECK_EQ(cfg_writes, 0);

    /* The operator's whole view of the session, on request. */
    if (getenv("DVM_SHOW_TRANSCRIPT")) {
        printf("\n----- session as the operator saw it -----\n> %s\n%s"
               "------------------------------------------\n",
               TR_SENTENCE, console);
    }

    /* The closing claim is checked against the assembler, so the recorded prose
     * cannot drift away from the recorded program. */
    CHECK_EQ(call("driver_assemble", asm_input(prog_v2)), TOOL_OK);
    CHECK_CONTAINS(rbuf, "ok: 39 instructions");
    CHECK(strstr(TR_ROUND4, "39 instructions") != NULL);

    /* --- and it replays identically --- */
    tlk_reset_device();
    dvm_tools_reset_attempts();
    model_mock_reset();
    chat_reset();
    queue(TR_ROUND1);
    queue(TR_ROUND2);
    queue(TR_ROUND3);
    queue(TR_ROUND4);
    kcap_reset();
    CHECK_EQ(chat_ask(TR_SENTENCE), CHAT_OK);
    CHECK_EQ(tlk.rate, 0x100u);
    CHECK_EQ(tlk.running, 1);
    CHECK_CONTAINS(kcap_text(), "[driver_run pci00:05.0 attempt=2");
}

/* ====================================================================== */
/* 12. the model that never converges                                     */
/* ====================================================================== */

static void test_never_converges(void) {
    fresh();
    model_mock_reset();
    chat_reset();
    chat_init(model_mock_transport());
    kcap_reset();

    queue(TR_STUCK1);
    queue(TR_STUCK2);
    queue(TR_STUCK3);
    queue(TR_STUCK4);
    queue(TR_STUCK5);
    queue(TR_STUCK6);
    queue(TR_STUCK_GIVE_UP);

    CHECK_EQ(chat_ask(TR_SENTENCE), CHAT_OK);
    CHECK_EQ((int)model_mock_request_count(), 7);
    CHECK_EQ((int)model_mock_pending(), 0);

    /* Five ran and were refused by the VM; the sixth never reached it. */
    CHECK_CONTAINS(kcap_text(), "[driver_run pci00:05.0 attempt=5 PORT_DENIED");
    CHECK(strstr(kcap_text(), "attempt=6") == NULL);

    const char *last_tool_req = model_mock_request(6);
    CHECK(last_tool_req != NULL);
    CHECK_CONTAINS(last_tool_req, "5 attempts against pci00:05.0 have all failed");

    /* The device was never touched: every one of those programs died on the
     * denied port before its first legal access... */
    CHECK_EQ(tlk.running, 0);
    CHECK_EQ(forbidden, 0);

    /* ...and the turn still ended with the model answering the operator. */
    CHECK_CONTAINS(kcap_text(), "I cannot bring that device up");
}

/* The other bound: a model that keeps calling tools forever is stopped by
 * chat.c, and the turn is abandoned rather than answered blind. */
static void test_round_cap(void) {
    fresh();
    model_mock_reset();
    chat_reset();
    chat_init(model_mock_transport());
    chat_set_max_rounds(2);
    kcap_reset();

    queue(TR_STUCK1);
    queue(TR_STUCK2);
    CHECK_EQ(chat_ask(TR_SENTENCE), CHAT_ELIMIT);
    CHECK_CONTAINS(kcap_text(), "stopped after 2 tool rounds");
    chat_set_max_rounds(0);
}

/* ====================================================================== */
/* 13. the model's syscall surface still fits the request                 */
/* ====================================================================== */

static void test_schema(void) {
    static char schema[CHAT_TOOLS_BYTES];
    size_t n = tools_build_schema(schema, sizeof schema);
    CHECK(n > 0);
    CHECK(n < sizeof schema);

    json_value_t v;
    CHECK_EQ(json_parse(schema, n, &v), JSON_OK);
    CHECK_EQ((int)v.type, (int)JSON_ARRAY);
    CHECK_EQ((int)json_count(&v), 4);          /* only this family is linked here */

    CHECK_CONTAINS(schema, "driver_targets");
    CHECK_CONTAINS(schema, "driver_assemble");
    CHECK_CONTAINS(schema, "driver_run");
    CHECK_CONTAINS(schema, "driver_trace");
    /* The ISA the model has to write in is in the schema, not in its memory. */
    CHECK_CONTAINS(schema, "out8|out16|out32 port,a");
    CHECK_CONTAINS(schema, "pcicfg rd,bdf,off");

    printf("    (this family's schema: %u bytes of the %u-byte budget)\n",
           (unsigned)n, (unsigned)CHAT_TOOLS_BYTES);

    /* Only driver_run mutates the machine. */
    CHECK_EQ((int)(tool_find("driver_run")->flags & TOOL_MUTATES), TOOL_MUTATES);
    CHECK_EQ((int)tool_find("driver_targets")->flags, 0);
    CHECK_EQ((int)tool_find("driver_assemble")->flags, 0);
    CHECK_EQ((int)tool_find("driver_trace")->flags, 0);
}

/* ====================================================================== */
/* 14. fuzz: nothing the model can say may escape the sandbox             */
/* ====================================================================== */

static uint32_t rng_state = 0x13572468u;
static uint32_t rnd(void) {
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}

static void test_fuzz_programs(void) {
    static const char *frag[] = {
        "halt", "nop", "mov r%u, 0x%x", "add r%u, r%u, 3", "in8 r%u, 0x%x",
        "out8 0x%x, r%u", "in32 r%u, r0", "out32 r0, r%u", "ld32 r%u, [0x%x]",
        "st16 [r%u+0x%x], r1", "pcicfg r%u, 0x%x, 0x%x", "delay %u",
        "cmp r%u, r%u", "bne top", "jmp top", "call top", "ret", "push r%u",
        "pop r%u", "div r%u, r1, r2", "shl r%u, r%u, %u", "abort \"x\"",
        "print \"p\", r%u", "top:", "; comment", ".equ K 0x%x", "@@@garbage",
        "in16 r%u, r%u", "st8 [r0+0x%x], 0x%x", "test r%u, 0x%x",
    };
    const int nfrag = (int)(sizeof frag / sizeof frag[0]);

    int ran = 0, refused = 0;
    for (int iter = 0; iter < 3000; iter++) {
        char src[2048];
        int  n = 0;
        int  lines = 1 + (int)(rnd() % 24);
        for (int i = 0; i < lines && n < (int)sizeof src - 80; i++) {
            const char *f = frag[rnd() % (unsigned)nfrag];
            n += snprintf(src + n, sizeof src - (size_t)n, f,
                          rnd() % 20, rnd() % 0x12000, rnd() % 64, rnd() % 40000);
            n += snprintf(src + n, sizeof src - (size_t)n, "\n");
        }
        fresh();
        int rc = call("driver_run", run_input(src));
        if (rc == TOOL_OK) ran++; else refused++;

        /* Whatever it was, it stayed inside. */
        CHECK_EQ(forbidden, 0);
        CHECK_EQ(kpanic_hit, 0);
        CHECK_EQ(cfg_writes, 0);
        CHECK(rres.len < rres.cap);
    }
    printf("    (3000 fuzzed programs: %d halted, %d refused, forbidden=%ld)\n",
           ran, refused, forbidden);
    CHECK_EQ(forbidden, 0);
}

static void test_fuzz_inputs(void) {
    static const char *shapes[] = {
        "{}", "[]", "null", "true", "0", "\"x\"", "{\"target\":", "{,}",
        "{\"target\":\"pci00:05.0\",\"source\":\"halt\",}",
        "{\"target\":\"pci00:05.0\",\"source\":{\"a\":1}}",
        "{\"target\":{},\"source\":\"halt\"}",
        "{\"target\":\"pci00:05.0\",\"source\":\"halt\",\"max_steps\":-1}",
        "{\"target\":\"pci00:05.0\",\"source\":\"halt\",\"max_steps\":99999999999}",
        "{\"target\":\"pci00:05.0\",\"source\":\"halt\",\"delay_budget_ms\":-5}",
        "{\"target\":\"pci00:05.0\",\"source\":\"halt\",\"trace\":42}",
        "{\"TARGET\":\"pci00:05.0\",\"SOURCE\":\"halt\"}",
    };
    const int n = (int)(sizeof shapes / sizeof shapes[0]);

    for (int i = 0; i < n; i++) {
        fresh();
        for (int t = 0; t < 4; t++) {
            const char *names[4] = { "driver_run", "driver_assemble",
                                     "driver_targets", "driver_trace" };
            call(names[t], shapes[i]);
            CHECK_EQ(kpanic_hit, 0);
            CHECK(rres.len < rres.cap);
        }
    }

    /* Every prefix of a valid call: truncation must never be mistaken for a
     * shorter but valid request. */
    const char *good = "{\"target\":\"pci00:05.0\",\"source\":\"mov r1,1\\nhalt\"}";
    for (size_t len = 0; len < strlen(good); len++) {
        fresh();
        CHECK(call_raw("driver_run", good, len) != TOOL_OK);
        CHECK_EQ(kpanic_hit, 0);
    }
    CHECK_EQ(forbidden, 0);
}

/* ====================================================================== */

int main(void) {
    console_init();
    build_machine();
    build_qemu_replica();
    decode_transcript_programs();
    dvm_tools_set_io(&tlk_io);

    RUN(test_trace_before_any_run);      /* must precede any run */
    RUN(test_targets);
    RUN(test_targets_refusals);
    RUN(test_qemu_bus_layout);
    RUN(test_assemble);
    RUN(test_assemble_oversized);
    RUN(test_run_success);
    RUN(test_run_failure_feedback);
    RUN(test_run_denied_port);
    RUN(test_run_mmio_window);
    RUN(test_run_bounded);
    RUN(test_run_target_validation);
    RUN(test_garbage_input);
    RUN(test_attempt_budget);
    RUN(test_budget_is_not_self_service);
    RUN(test_console_is_not_a_target);
    RUN(test_trace_tool);
    RUN(test_no_backend);
    RUN(test_golden_transcript);
    RUN(test_never_converges);
    RUN(test_round_cap);
    RUN(test_schema);
    RUN(test_fuzz_programs);
    RUN(test_fuzz_inputs);

    printf("    (device accesses: %ld, delays: %ld, forbidden: %ld, "
           "config writes: %ld)\n", tlk_accesses, tlk_delays, forbidden, cfg_writes);
    CHECK_EQ(forbidden, 0);
    CHECK_EQ(cfg_writes, 0);

    return th_report("dvm_tools");
}
