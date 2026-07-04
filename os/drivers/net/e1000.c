/* e1000.c — driver for the Intel 82540EM, the card QEMU presents as `e1000`.
 *
 * We talk to it over MMIO (BAR0), set up DMA descriptor rings for RX and TX,
 * and poll the descriptor "done" bits rather than using interrupts. Because
 * the kernel identity-maps physical memory, a buffer's virtual address is also
 * its physical address, so we can hand descriptor addresses straight to the
 * card. */

#include "e1000.h"
#include "kernel.h"
#include "io.h"
#include "pci.h"
#include "driver.h"

/* ---- e1000 register offsets ---- */
#define REG_CTRL   0x0000
#define REG_EERD   0x0014
#define REG_ICR    0x00C0
#define REG_IMC    0x00D8
#define REG_RCTL   0x0100
#define REG_TCTL   0x0400
#define REG_TIPG   0x0410
#define REG_RDBAL  0x2800
#define REG_RDBAH  0x2804
#define REG_RDLEN  0x2808
#define REG_RDH    0x2810
#define REG_RDT    0x2818
#define REG_TDBAL  0x3800
#define REG_TDBAH  0x3804
#define REG_TDLEN  0x3808
#define REG_TDH    0x3810
#define REG_TDT    0x3818
#define REG_MTA    0x5200
#define REG_RAL    0x5400
#define REG_RAH    0x5404

#define CTRL_SLU   (1u << 6)
#define CTRL_ASDE  (1u << 5)

#define RCTL_EN    (1u << 1)
#define RCTL_UPE   (1u << 3)
#define RCTL_MPE   (1u << 4)
#define RCTL_BAM   (1u << 15)
#define RCTL_SECRC (1u << 26)

#define TCTL_EN    (1u << 1)
#define TCTL_PSP   (1u << 3)

#define TX_CMD_EOP  (1u << 0)
#define TX_CMD_IFCS (1u << 1)
#define TX_CMD_RS   (1u << 3)
#define STAT_DD     (1u << 0)

#define NUM_RX 32
#define NUM_TX 8
#define BUF_SZ 2048

struct rx_desc {
    uint64_t addr;
    uint16_t length;
    uint16_t checksum;
    uint8_t  status;
    uint8_t  errors;
    uint16_t special;
} __attribute__((packed));

struct tx_desc {
    uint64_t addr;
    uint16_t length;
    uint8_t  cso;
    uint8_t  cmd;
    uint8_t  status;
    uint8_t  css;
    uint16_t special;
} __attribute__((packed));

static volatile uint8_t *mmio;
static uint8_t mac[6];

static struct rx_desc rx_ring[NUM_RX] __attribute__((aligned(16)));
static struct tx_desc tx_ring[NUM_TX] __attribute__((aligned(16)));
static uint8_t rx_buf[NUM_RX][BUF_SZ] __attribute__((aligned(16)));
static uint8_t tx_buf[NUM_TX][BUF_SZ] __attribute__((aligned(16)));
static int rx_cur, tx_cur;

static uint32_t reg_read(uint32_t r)  { return mmio_read32(mmio + r); }
static void     reg_write(uint32_t r, uint32_t v) { mmio_write32(mmio + r, v); }

static int find_e1000(pci_dev_t *out) {
    static const uint16_t ids[] = {0x100E, 0x1004, 0x100F};
    for (unsigned i = 0; i < sizeof ids / sizeof ids[0]; i++)
        if (pci_find_vendor(0x8086, ids[i], out)) return 1;
    return 0;
}

int e1000_init(void) {
    pci_dev_t pdev;
    if (!find_e1000(&pdev)) {
        kprintf("e1000: no supported NIC found\n");
        return -1;
    }

    pci_enable_bus_master(&pdev);
    uint32_t bar0 = pci_bar(&pdev, 0);
    mmio = (volatile uint8_t *)(uintptr_t)bar0;
    kprintf("e1000: %02x:%02x.%x MMIO=%p\n", pdev.bus, pdev.dev, pdev.func,
            (void *)(uintptr_t)bar0);

    reg_write(REG_IMC, 0xFFFFFFFF);   /* mask all interrupts; we poll */
    reg_read(REG_ICR);

    /* Read the MAC from the receive-address registers. */
    uint32_t ral = reg_read(REG_RAL);
    uint32_t rah = reg_read(REG_RAH);
    mac[0] = ral; mac[1] = ral >> 8; mac[2] = ral >> 16; mac[3] = ral >> 24;
    mac[4] = rah; mac[5] = rah >> 8;

    /* Bring the link up. */
    reg_write(REG_CTRL, reg_read(REG_CTRL) | CTRL_SLU | CTRL_ASDE);

    /* Make sure our unicast filter is programmed (AV = address valid). */
    reg_write(REG_RAL, ral);
    reg_write(REG_RAH, (rah & 0xFFFF) | (1u << 31));

    /* Clear the multicast table. */
    for (int i = 0; i < 128; i++) reg_write(REG_MTA + i * 4, 0);

    /* RX ring. */
    for (int i = 0; i < NUM_RX; i++) {
        rx_ring[i].addr = (uint64_t)(uintptr_t)rx_buf[i];
        rx_ring[i].status = 0;
    }
    reg_write(REG_RDBAL, (uint32_t)(uintptr_t)rx_ring);
    reg_write(REG_RDBAH, (uint32_t)((uint64_t)(uintptr_t)rx_ring >> 32));
    reg_write(REG_RDLEN, NUM_RX * sizeof(struct rx_desc));
    reg_write(REG_RDH, 0);
    reg_write(REG_RDT, NUM_RX - 1);
    rx_cur = 0;
    reg_write(REG_RCTL, RCTL_EN | RCTL_UPE | RCTL_MPE | RCTL_BAM | RCTL_SECRC);

    /* TX ring. */
    for (int i = 0; i < NUM_TX; i++) {
        tx_ring[i].addr = (uint64_t)(uintptr_t)tx_buf[i];
        tx_ring[i].status = STAT_DD;   /* mark free */
        tx_ring[i].cmd = 0;
    }
    reg_write(REG_TDBAL, (uint32_t)(uintptr_t)tx_ring);
    reg_write(REG_TDBAH, (uint32_t)((uint64_t)(uintptr_t)tx_ring >> 32));
    reg_write(REG_TDLEN, NUM_TX * sizeof(struct tx_desc));
    reg_write(REG_TDH, 0);
    reg_write(REG_TDT, 0);
    tx_cur = 0;
    reg_write(REG_TIPG, 0x0060200A);
    reg_write(REG_TCTL, TCTL_EN | TCTL_PSP | (0x0F << 4) | (0x40 << 12));

    return 0;
}

void e1000_get_mac(uint8_t out[6]) {
    memcpy(out, mac, 6);
}

int e1000_send(const void *data, uint16_t len) {
    if (len > BUF_SZ) return -1;
    int idx = tx_cur;
    memcpy(tx_buf[idx], data, len);
    tx_ring[idx].length = len;
    tx_ring[idx].cmd = TX_CMD_EOP | TX_CMD_IFCS | TX_CMD_RS;
    tx_ring[idx].status = 0;

    tx_cur = (idx + 1) % NUM_TX;
    reg_write(REG_TDT, tx_cur);

    /* Wait for the descriptor-done bit (bounded so we never hang forever). */
    for (uint64_t spin = 0; spin < 100000000ULL; spin++) {
        if (tx_ring[idx].status & STAT_DD) return 0;
    }
    return -1;
}

int e1000_receive(void *buf, uint16_t *len) {
    int idx = rx_cur;
    if (!(rx_ring[idx].status & STAT_DD)) return 0;

    uint16_t l = rx_ring[idx].length;
    memcpy(buf, rx_buf[idx], l);
    *len = l;

    rx_ring[idx].status = 0;
    reg_write(REG_RDT, idx);            /* hand the descriptor back */
    rx_cur = (idx + 1) % NUM_RX;
    return 1;
}

static const driver_t e1000_driver = {
    .name = "e1000",
    .level = DRV_LEVEL_DEVICE,
    .init = e1000_init,
};
REGISTER_DRIVER(e1000_driver);
