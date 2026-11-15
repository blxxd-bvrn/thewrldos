/* Read-only PCI BAR decoding. Drivers opt into the high-MMIO mapper explicitly;
 * the old identity-map accessor retains its below-4-GiB contract. */
#include "kernel.h"
uint64_t pci_bar64(const pci_dev_t *p, int i, bool *is_io) {
    if (is_io) *is_io = false;
    if (!p || i < 0 || i >= 6) return 0;
    uint32_t v = pci_cfg_read(p->bus, p->dev, p->fn, 0x10 + i * 4);
    if (!v || v == ~0u) return 0;
    if (is_io) *is_io = (v & 1) != 0;
    if (v & 1) return v & ~3u;
    uint64_t address = v & ~0xFull;
    if ((v & 6) == 4) {
        if (i == 5) return 0;
        address |= (uint64_t)pci_cfg_read(p->bus, p->dev, p->fn, 0x14 + i * 4) << 32;
    }
    return address;
}
uintptr_t pci_bar(const pci_dev_t *p, int i, bool *is_io) {
    uint64_t address = pci_bar64(p, i, is_io);
    return address <= 0xFFFFFFFFull ? (uintptr_t)address : 0;
}
