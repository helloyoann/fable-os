/* pci.c — PCI bus driver: config-space access, device enumeration, lookup. */

#include "pci.h"
#include "driver.h"
#include "kernel.h"
#include "io.h"

#define CONFIG_ADDR 0xCF8
#define CONFIG_DATA 0xCFC

static uint32_t cfg_addr(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off) {
    return (1u << 31) | ((uint32_t)bus << 16) | ((uint32_t)dev << 11) |
           ((uint32_t)fn << 8) | (off & 0xFC);
}

uint32_t pci_cfg_read32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off) {
    outl(CONFIG_ADDR, cfg_addr(bus, dev, fn, off));
    return inl(CONFIG_DATA);
}

void pci_cfg_write32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off, uint32_t val) {
    outl(CONFIG_ADDR, cfg_addr(bus, dev, fn, off));
    outl(CONFIG_DATA, val);
}

uint32_t pci_read32(const pci_dev_t *d, uint8_t off) {
    return pci_cfg_read32(d->bus, d->dev, d->func, off);
}
void pci_write32(const pci_dev_t *d, uint8_t off, uint32_t val) {
    pci_cfg_write32(d->bus, d->dev, d->func, off, val);
}

static int probe(uint8_t bus, uint8_t dev, uint8_t fn, pci_dev_t *out) {
    uint32_t id = pci_cfg_read32(bus, dev, fn, 0x00);
    uint16_t vendor = id & 0xFFFF;
    if (vendor == 0xFFFF) return 0;
    uint32_t cls = pci_cfg_read32(bus, dev, fn, 0x08);
    out->bus = bus; out->dev = dev; out->func = fn;
    out->vendor = vendor; out->device = id >> 16;
    out->class = (cls >> 24) & 0xFF;
    out->subclass = (cls >> 16) & 0xFF;
    return 1;
}

int pci_find_vendor(uint16_t vendor, uint16_t device, pci_dev_t *out) {
    for (uint16_t bus = 0; bus < 256; bus++)
        for (uint8_t dev = 0; dev < 32; dev++)
            for (uint8_t fn = 0; fn < 8; fn++) {
                pci_dev_t d;
                if (!probe((uint8_t)bus, dev, fn, &d)) { if (fn == 0) break; else continue; }
                if (d.vendor == vendor && d.device == device) { *out = d; return 1; }
            }
    return 0;
}

int pci_find_class(uint8_t class, uint8_t subclass, pci_dev_t *out) {
    for (uint16_t bus = 0; bus < 256; bus++)
        for (uint8_t dev = 0; dev < 32; dev++)
            for (uint8_t fn = 0; fn < 8; fn++) {
                pci_dev_t d;
                if (!probe((uint8_t)bus, dev, fn, &d)) { if (fn == 0) break; else continue; }
                if (d.class == class && d.subclass == subclass) { *out = d; return 1; }
            }
    return 0;
}

void pci_enable_bus_master(const pci_dev_t *d) {
    uint32_t cmd = pci_read32(d, 0x04);
    cmd |= (1 << 1) | (1 << 2);   /* memory space + bus master */
    pci_write32(d, 0x04, cmd);
}

uint32_t pci_bar(const pci_dev_t *d, int bar) {
    uint32_t v = pci_read32(d, 0x10 + bar * 4);
    if (v & 1) return v & ~0x3u;   /* I/O BAR */
    return v & ~0xFu;              /* memory BAR */
}

static int pci_init(void) {
    int n = 0;
    for (uint16_t bus = 0; bus < 256; bus++)
        for (uint8_t dev = 0; dev < 32; dev++)
            for (uint8_t fn = 0; fn < 8; fn++) {
                pci_dev_t d;
                if (!probe((uint8_t)bus, dev, fn, &d)) { if (fn == 0) break; else continue; }
                kprintf("pci: %02x:%02x.%x  %04x:%04x  class %02x:%02x\n",
                        d.bus, d.dev, d.func, d.vendor, d.device, d.class, d.subclass);
                n++;
            }
    kprintf("pci: %d device(s)\n", n);
    return 0;
}

static const driver_t pci_driver = {
    .name = "pci",
    .level = DRV_LEVEL_BUS,
    .init = pci_init,
};
REGISTER_DRIVER(pci_driver);
