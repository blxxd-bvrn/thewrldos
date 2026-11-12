/* Realtek gigabit family: RTL8169/8110, RTL8111/8168 (most desktop and laptop
 * boards), RTL8101/8102/8103/8105/8106 fast Ethernet, RTL8125 2.5G. Polled,
 * descriptor rings in "C+" mode. The chip generation comes from the
 * TxConfig hardware ID (the same field Linux r8169 uses); only the settings
 * that differ between generations are applied, and the PHY is left as the
 * firmware configured it. */
#include "kernel.h"
#include "net.h"
#include "devmgr.h"

#define IDR0      0x00
#define MAR0      0x08
#define TNPDS     0x20          /* transmit descriptors, low/high */
#define CR        0x37
#define TPPOLL    0x38
#define TPPOLL_8125 0x90
#define IMR       0x3C
#define ISR       0x3E
#define IMR_8125  0x38
#define ISR_8125  0x3C
#define TCR       0x40
#define RCR       0x44
#define CFG9346   0x50
#define PHYSTATUS 0x6C
#define RMS       0xDA
#define CPCR      0xE0
#define RDSAR     0xE4          /* receive descriptors, low/high */
#define MTPS      0xEC
#define MISC      0xF0

#define NRX 64
#define NTX 32
#define BUFSZ 2048

#define OWN (1u << 31)
#define EOR (1u << 30)
#define FS  (1u << 29)
#define LS  (1u << 28)
#define RX_RES (1u << 21)

typedef struct PACKED { uint32_t opts1, opts2; uint64_t addr; } rdesc_t;

static uint16_t io;
static rdesc_t *rx, *tx;
static uint8_t *rxbuf, *txbuf;
static uint32_t rx_cur, tx_cur;
static bool is8125;

static bool re_send(const void *frame, uint32_t len) {
    if (len > BUFSZ) return false;
    rdesc_t *d = &tx[tx_cur];
    uint32_t t0 = timer_ticks();
    while ((*(volatile uint32_t *)&d->opts1 & OWN) && timer_ticks() - t0 < 50) {}
    if (*(volatile uint32_t *)&d->opts1 & OWN) return false;
    memcpy(txbuf + tx_cur * BUFSZ, frame, len);
    if (len < 60) { memset(txbuf + tx_cur * BUFSZ + len, 0, 60 - len); len = 60; }
    d->addr = dma_addr(txbuf + tx_cur * BUFSZ);
    d->opts2 = 0;
    __asm__ volatile("mfence" ::: "memory");
    *(volatile uint32_t *)&d->opts1 = OWN | FS | LS | (tx_cur == NTX - 1 ? EOR : 0) | len;
    __asm__ volatile("mfence" ::: "memory");
    tx_cur = (tx_cur + 1) % NTX;
    if (is8125) outb(io + TPPOLL_8125, 0x01); else outb(io + TPPOLL, 0x40);   /* NPQ: normal-priority queue has work */
    return true;
}

static void re_poll(void) {
    for (int n = 0; n < NRX; n++) {
        rdesc_t *d = &rx[rx_cur];
        uint32_t o = *(volatile uint32_t *)&d->opts1;
        if (o & OWN) break;
        uint32_t len = o & 0x3FFF;                      /* includes the 4-byte FCS */
        if ((o & FS) && (o & LS) && !(o & RX_RES) && len > 4 && len <= BUFSZ) net_rx(rxbuf + rx_cur * BUFSZ, len - 4);
        else netif.rx_dropped++;
        d->opts2 = 0;
        __asm__ volatile("mfence" ::: "memory");
        *(volatile uint32_t *)&d->opts1 = OWN | (rx_cur == NRX - 1 ? EOR : 0) | BUFSZ;
        rx_cur = (rx_cur + 1) % NRX;
    }
    /* acknowledge status bits (nothing is unmasked, but a full RX FIFO latches) */
    if (is8125) { uint32_t s = inl(io + ISR_8125); if (s) outl(io + ISR_8125, s); }
    else { uint16_t s = inw(io + ISR); if (s) outw(io + ISR, s); }
}

static bool re_link(void) { return (inb(io + PHYSTATUS) & 0x02) != 0; }

static const char *chip_name(uint32_t xid, uint16_t dev) {
    if (dev == 0x8125 || dev == 0x3000 || (xid & 0x7c0) == 0x600 || (xid & 0x7c0) == 0x640) return "RTL8125 2.5GbE";
    if (dev == 0x8136 || (xid & 0x7c0) == 0x340 || (xid & 0x7c0) == 0x440 || (xid & 0x7cf) == 0x4c8 || (xid & 0x7cf) == 0x4c9) return "RTL810x Fast Ethernet";
    if (dev == 0x8169 || dev == 0x8167 || xid < 0x100 || (xid & 0x7c0) == 0x180 || xid == 0x980) return "RTL8169/8110 Gigabit";
    return "RTL8111/8168 Gigabit";
}

static const struct { uint16_t ven, dev; } ids[] = {
    { 0x10EC, 0x8169 }, { 0x10EC, 0x8167 }, { 0x10EC, 0x8168 }, { 0x10EC, 0x8161 }, { 0x10EC, 0x8162 },
    { 0x10EC, 0x8136 }, { 0x10EC, 0x8125 }, { 0x10EC, 0x3000 }, { 0x10EC, 0x2502 }, { 0x10EC, 0x2600 },
    { 0x1186, 0x4300 }, { 0x1186, 0x4302 }, { 0x1259, 0xC107 }, { 0x16EC, 0x0116 }, { 0x1737, 0x1032 },
    { 0x10FF, 0x8168 }, { 0x1B86, 0x8168 },
};

static bool rtl8169_match(const pci_dev_t *p) {
    for (unsigned k = 0; k < ARRAY_LEN(ids); k++) if (p->vendor == ids[k].ven && p->device == ids[k].dev) return true;
    return false;
}
DRIVER_PCI(drv_rtl8169, "rtl8169", "Realtek RTL8111/8168/8169/810x/8125 Ethernet", DC_NETWORK, rtl8169_match);

bool rtl8169_probe(nic_ops_t *ops) {
    pci_dev_t *devs; int n = pci_list(&devs);
    for (int i = 0; i < n; i++) {
        bool ok = false;
        for (unsigned k = 0; k < ARRAY_LEN(ids); k++) if (devs[i].vendor == ids[k].ven && devs[i].device == ids[k].dev) ok = true;
        if (!ok) continue;
        pci_dev_t *p = &devs[i];
        bool isio; uintptr_t b0 = pci_bar(p, 0, &isio);
        if (!b0 || !isio) { klog("rtl8169: no I/O BAR"); continue; }
        io = (uint16_t)b0;
        pci_enable(p, true, false);
        uint32_t xid = (inl(io + TCR) >> 20) & 0xFCF;
        is8125 = p->device == 0x8125 || p->device == 0x3000 || (xid & 0x7c0) == 0x600 || (xid & 0x7c0) == 0x640;
        bool old8169 = p->device == 0x8169 || p->device == 0x8167 || xid < 0x100 || (xid & 0x7c0) == 0x180 || xid == 0x980;
        bool newgen = (xid & 0x7c0) >= 0x4c0;           /* RTL8168G and later, RTL8107E, RTL8125 */
        /* mask and acknowledge interrupts, reset */
        if (is8125) { outl(io + IMR_8125, 0); outl(io + ISR_8125, 0xFFFFFFFF); }
        else { outw(io + IMR, 0); outw(io + ISR, 0xFFFF); }
        outb(io + CR, 0x10);
        uint32_t t0 = timer_ticks();
        while ((inb(io + CR) & 0x10) && timer_ticks() - t0 < 100) sleep_ms(1);
        if (inb(io + CR) & 0x10) { klog("rtl8169: reset did not complete"); continue; }
        for (int k = 0; k < 6; k++) netif.mac[k] = inb(io + IDR0 + k);
        /* rings: 256-byte aligned */
        rx = kmalloc_aligned(sizeof(rdesc_t) * NRX, 256); tx = kmalloc_aligned(sizeof(rdesc_t) * NTX, 256);
        rxbuf = kmalloc_aligned(NRX * BUFSZ, 64); txbuf = kmalloc_aligned(NTX * BUFSZ, 64);
        memset(tx, 0, sizeof(rdesc_t) * NTX);
        tx[NTX - 1].opts1 = EOR;
        for (int k = 0; k < NRX; k++) {
            rx[k].addr = dma_addr(rxbuf + k * BUFSZ); rx[k].opts2 = 0;
            rx[k].opts1 = OWN | (k == NRX - 1 ? EOR : 0) | BUFSZ;
        }
        rx_cur = tx_cur = 0;
        outb(io + CFG9346, 0xC0);                       /* unlock configuration registers */
        uint16_t cp = inw(io + CPCR);
        cp &= ~((1u << 6) | (1u << 5));                 /* no VLAN tag stripping, no RX checksum offload */
        if (old8169) cp |= (1u << 3);                   /* PCI multiple read/write */
        outw(io + CPCR, cp);
        outw(io + RMS, 1600);                           /* biggest frame accepted into a 2 KB buffer */
        if (!is8125) outb(io + MTPS, 0x3B);
        if (newgen) outl(io + MISC, inl(io + MISC) & ~(1u << 19));   /* RXDV gate: no receive while it is set */
        outl(io + TNPDS + 4, (uint32_t)(dma_addr(tx) >> 32)); outl(io + TNPDS, (uint32_t)dma_addr(tx));
        outl(io + RDSAR + 4, (uint32_t)(dma_addr(rx) >> 32)); outl(io + RDSAR, (uint32_t)dma_addr(rx));
        outb(io + CFG9346, 0x00);
        (void)inb(io + CR);
        outb(io + CR, 0x0C);                            /* TE | RE, before RX/TX configuration as Linux does */
        uint32_t rcr = is8125 ? ((8u << 27) | (7u << 8)) : old8169 ? ((7u << 13) | (7u << 8)) : ((1u << 15) | (7u << 8));
        outl(io + RCR, rcr | 0x0E);                     /* accept broadcast, multicast, my MAC */
        uint32_t tcr = (3u << 24) | (7u << 8);
        if (!old8169 && p->device != 0x8136 && (xid & 0x7c0) >= 0x2c0) tcr |= (1u << 7);   /* auto FIFO (RTL8168E-VL and later) */
        outl(io + TCR, tcr);
        outl(io + MAR0, 0xFFFFFFFF); outl(io + MAR0 + 4, 0xFFFFFFFF);
        snprintf(netif.driver, sizeof netif.driver, "Realtek %s", chip_name(xid, p->device));
        ops->name = netif.driver; ops->send = re_send; ops->poll = re_poll; ops->link_up = re_link;
        klog("rtl8169: %s (%04x:%04x, XID %03x) at io %04x, MAC %02x:%02x:%02x:%02x:%02x:%02x, link %s", netif.driver,
             p->vendor, p->device, xid, io, netif.mac[0], netif.mac[1], netif.mac[2], netif.mac[3], netif.mac[4], netif.mac[5],
             re_link() ? "up" : "down");
        dev_claim(p, &drv_rtl8169, netif.driver);
        return true;
    }
    return false;
}
