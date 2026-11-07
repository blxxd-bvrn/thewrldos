/* AMD PCnet-PCI II / PCnet-FAST III (Am79C970A / Am79C973), polled.
 * VMware calls this "vlance" ("AMD PCnet" in the guest) and gives it to VMs
 * whose guest type is Windows XP and other older systems. VirtualBox offers
 * it as "PCnet-PCI II" and "PCnet-FAST III", QEMU as -nic model=pcnet.
 * The card runs in 32-bit mode with software style 2 descriptors. */
#include "kernel.h"
#include "net.h"

/* 32-bit I/O (DWIO) register offsets */
#define RDP   0x10
#define RAP   0x14
#define RESET 0x18
#define BDP   0x1C

#define NRX 32                      /* must be powers of two (2^RLEN, 2^TLEN) */
#define NTX 16
#define LOG2_NRX 5
#define LOG2_NTX 4
#define BUFSZ 1544

typedef struct PACKED { uint32_t addr; uint32_t flags; uint32_t misc; uint32_t user; } pdesc_t;
typedef struct PACKED {
    uint16_t mode; uint8_t rlen, tlen; uint8_t padr[6]; uint16_t rsvd; uint8_t ladrf[8]; uint32_t rdra, tdra;
} initblk_t;

#define OWN (1u << 31)
#define ERR (1u << 30)
#define STP (1u << 25)
#define ENP (1u << 24)

static uint16_t io;
static pdesc_t *rx, *tx;
static uint8_t *rxbuf, *txbuf;
static uint32_t rx_cur, tx_cur;

static uint32_t csr_rd(uint32_t r) { outl(io + RAP, r); return inl(io + RDP); }
static void csr_wr(uint32_t r, uint32_t v) { outl(io + RAP, r); outl(io + RDP, v); }
static uint32_t bcr_rd(uint32_t r) { outl(io + RAP, r); return inl(io + BDP); }
static void bcr_wr(uint32_t r, uint32_t v) { outl(io + RAP, r); outl(io + BDP, v); }
static uint32_t bcnt(uint32_t n) { return 0xF000u | ((-(int32_t)n) & 0x0FFF); }

static bool pc_send(const void *frame, uint32_t len) {
    if (len > BUFSZ) return false;
    pdesc_t *d = &tx[tx_cur];
    uint32_t t0 = timer_ticks();
    while ((*(volatile uint32_t *)&d->flags & OWN) && timer_ticks() - t0 < 50) {}
    if (*(volatile uint32_t *)&d->flags & OWN) return false;
    memcpy(txbuf + tx_cur * BUFSZ, frame, len);
    if (len < 60) { memset(txbuf + tx_cur * BUFSZ + len, 0, 60 - len); len = 60; }
    d->misc = 0;
    __asm__ volatile("" ::: "memory");
    *(volatile uint32_t *)&d->flags = OWN | STP | ENP | bcnt(len);
    tx_cur = (tx_cur + 1) % NTX;
    csr_wr(0, 0x0008);                                 /* TDMD: look at the transmit ring now */
    return true;
}

static void pc_poll(void) {
    for (int n = 0; n < NRX; n++) {
        pdesc_t *d = &rx[rx_cur];
        uint32_t f = *(volatile uint32_t *)&d->flags;
        if (f & OWN) break;
        uint32_t len = d->misc & 0x0FFF;               /* includes the 4-byte FCS */
        if (!(f & ERR) && (f & STP) && (f & ENP) && len > 4) net_rx(rxbuf + rx_cur * BUFSZ, len - 4);
        else netif.rx_dropped++;
        d->misc = 0;
        __asm__ volatile("" ::: "memory");
        *(volatile uint32_t *)&d->flags = OWN | bcnt(BUFSZ);
        rx_cur = (rx_cur + 1) % NRX;
    }
    /* a missed frame or babble error stops nothing on this chip, but clear the status bits */
    uint32_t c0 = csr_rd(0);
    if (c0 & 0x7800) csr_wr(0, c0 & 0x7800);
}

static bool pc_link(void) {
    /* BCR4 is the LED0 control; its LEDOUT bit follows link status on real
     * chips. Emulations that do not model it read back 0 here; the frames
     * still flow, so only believe "down" when the bit is known to work. */
    static int works = -1;
    uint32_t b4 = bcr_rd(4);
    if (b4 & 0x8000) works = 1;
    return works != 1 || (b4 & 0x8000);
}

static const uint16_t ids[] = { 0x2000, 0x2001 };

bool pcnet_probe(nic_ops_t *ops) {
    pci_dev_t *devs; int n = pci_list(&devs);
    for (int i = 0; i < n; i++) {
        if (devs[i].vendor != 0x1022) continue;
        bool ok = false;
        for (unsigned k = 0; k < ARRAY_LEN(ids); k++) if (devs[i].device == ids[k]) ok = true;
        if (!ok) continue;
        pci_dev_t *p = &devs[i];
        bool isio; uintptr_t b0 = pci_bar(p, 0, &isio);
        if (!b0 || !isio) { klog("pcnet: no I/O BAR"); continue; }
        io = (uint16_t)b0;
        pci_enable(p, true, false);
        /* reset (reading the reset register does it, in either I/O mode), then select 32-bit I/O */
        (void)inl(io + RESET); (void)inw(io + 0x14);
        sleep_ms(2);
        for (int k = 0; k < 6; k++) netif.mac[k] = inb(io + k);   /* address PROM: byte reads only work in 16-bit mode */
        outl(io + RDP, 0);
        /* software style 2: 32-bit descriptors and init block */
        csr_wr(58, (csr_rd(58) & 0xFF00) | 0x02);
        bcr_wr(2, bcr_rd(2) | 0x02);                   /* ASEL: pick the medium automatically */
        rx = kmalloc_aligned(sizeof(pdesc_t) * NRX, 16); tx = kmalloc_aligned(sizeof(pdesc_t) * NTX, 16);
        rxbuf = kmalloc_aligned(NRX * BUFSZ, 16); txbuf = kmalloc_aligned(NTX * BUFSZ, 16);
        memset(tx, 0, sizeof(pdesc_t) * NTX);
        for (int k = 0; k < NRX; k++) { rx[k].addr = (uint32_t)dma_addr(rxbuf + k * BUFSZ); rx[k].misc = 0; rx[k].user = 0; rx[k].flags = OWN | bcnt(BUFSZ); }
        for (int k = 0; k < NTX; k++) tx[k].addr = (uint32_t)dma_addr(txbuf + k * BUFSZ);
        rx_cur = tx_cur = 0;
        initblk_t *ib = kmalloc_aligned(sizeof *ib, 16);
        memset(ib, 0, sizeof *ib);
        ib->mode = 0;
        ib->rlen = LOG2_NRX << 4; ib->tlen = LOG2_NTX << 4;
        memcpy(ib->padr, netif.mac, 6);
        memset(ib->ladrf, 0, 8);
        ib->rdra = (uint32_t)dma_addr(rx); ib->tdra = (uint32_t)dma_addr(tx);
        uint32_t ibpa = (uint32_t)dma_addr(ib);
        csr_wr(1, ibpa & 0xFFFF); csr_wr(2, ibpa >> 16);
        csr_wr(3, 0x5F00);                             /* mask every interrupt source: we poll */
        csr_wr(4, csr_rd(4) | 0x0800);                 /* APAD_XMT: pad short frames */
        csr_wr(0, 0x0001);                             /* INIT */
        uint32_t t0 = timer_ticks();
        while (!(csr_rd(0) & 0x0100) && timer_ticks() - t0 < 200) {}
        if (!(csr_rd(0) & 0x0100)) { klog("pcnet: initialisation did not complete"); continue; }
        csr_wr(0, 0x0100 | 0x0002);                    /* clear IDON, STRT */
        uint32_t ver = csr_rd(88) | (csr_rd(89) << 16);
        uint16_t part = (ver >> 12) & 0xFFFF;
        const char *name = part == 0x2621 ? "PCnet-PCI II" : part == 0x2625 ? "PCnet-FAST III" : part == 0x2624 ? "PCnet-FAST+" : "PCnet";
        snprintf(netif.driver, sizeof netif.driver, "AMD %s (1022:%04x)", name, p->device);
        ops->name = netif.driver; ops->send = pc_send; ops->poll = pc_poll; ops->link_up = pc_link;
        klog("pcnet: %s at io %04x, MAC %02x:%02x:%02x:%02x:%02x:%02x", netif.driver, io,
             netif.mac[0], netif.mac[1], netif.mac[2], netif.mac[3], netif.mac[4], netif.mac[5]);
        return true;
    }
    return false;
}
