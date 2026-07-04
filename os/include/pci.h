/* pci.h — PCI configuration-space access and device lookup. */
#ifndef PCI_H
#define PCI_H

#include <stdint.h>

typedef struct {
    uint8_t  bus, dev, func;
    uint16_t vendor, device;
    uint8_t  class, subclass;
} pci_dev_t;

uint32_t pci_cfg_read32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off);
void     pci_cfg_write32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off, uint32_t val);

uint32_t pci_read32(const pci_dev_t *d, uint8_t off);
void     pci_write32(const pci_dev_t *d, uint8_t off, uint32_t val);

/* Find the first matching device. Returns 1 and fills *out, or 0. */
int  pci_find_vendor(uint16_t vendor, uint16_t device, pci_dev_t *out);
int  pci_find_class(uint8_t class, uint8_t subclass, pci_dev_t *out);

void     pci_enable_bus_master(const pci_dev_t *d);
uint32_t pci_bar(const pci_dev_t *d, int bar);   /* base address, low bits masked */

#endif /* PCI_H */
