/* Realtek RTL8139 family (RTL8139/8139C/8139D/8100/8101L/8130), polled.
 * Common on 2000s PCs and in QEMU (-nic model=rtl8139). Receive goes to one
 * 8 KB ring buffer; transmit uses the four fixed descriptor registers. */
#include "kernel.h"
#include "net.h"
#include "devmgr.h"

#define IDR0   0x00
#define TSD0   0x10
#define TSAD0  0x20
#define RBSTART 0x30
#define CR     0x37
#define CAPR   0x38
#define IMR    0x3C
#define ISR    0x3E
#define TCR    0x40
#define RCR    0x44
#define MPC    0x4C
#define CONFIG1 0x52
#define MSR    0x58

#define RXRING 8192
#define TXBUF  1536

static uint16_t io;
static uint8_t *rxring, *txbuf;
static uint32_t rx_off, tx_cur;

static bool rt_send(const void *frame, uint32_t len) {
    if (len > TXBUF) return false;
    uint16_t tsd = TSD0 + tx_cur * 4;
    uint32_t t0 = timer_ticks();
    while (!(inl(io + tsd) & (1u << 13)) && timer_ticks() - t0 < 50) {}      /* OWN: the chip is done with this slot */
    memcpy(txbuf + tx_cur * TXBUF, frame, len);
    if (len < 60) { memset(txbuf + tx_cur * TXBUF + len, 0, 60 - len); len = 60; }
    outl(io + TSAD0 + tx_cur * 4, (uint32_t)dma_addr(txbuf + tx_cur * TXBUF));
    outl(io + tsd, len | (8u << 16));                   /* clears OWN, starts; early-TX threshold 256 bytes */
    tx_cur = (tx_cur + 1) % 4;
    return true;
}

static void rx_reset(void) {
    outb(io + CR, 0x04);                                /* receiver off, transmitter on */
    outb(io + CR, 0x0C);
    outl(io + RBSTART, (uint32_t)dma_addr(rxring));
    outl(io + RCR, 0x0000E78E);
    rx_off = 0;
    outw(io + CAPR, (uint16_t)(rx_off - 16));
}

static void rt_poll(void) {
    for (int n = 0; n < 64 && !(inb(io + CR) & 0x01); n++) {          /* BUFE: ring empty */
        uint8_t *h = rxring + rx_off;
        uint16_t status = h[0] | (h[1] << 8), len = h[2] | (h[3] << 8);
        if (len == 0xFFF0) break;                       /* header still being written */
        if (!(status & 1) || len < 64 || len > 1522) { netif.rx_dropped++; rx_reset(); break; }
        net_rx(h + 4, len - 4);                         /* with WRAP set the frame is contiguous */
        rx_off = (rx_off + len + 4 + 3) & ~3u;
        rx_off %= RXRING;
        outw(io + CAPR, (uint16_t)(rx_off - 16));
    }
    uint16_t isr = inw(io + ISR);
    if (isr) outw(io + ISR, isr);
}

static bool rt_link(void) { return !(inb(io + MSR) & 0x04); }    /* LINKB is active low */

static const struct { uint16_t ven, dev; } ids[] = {
    { 0x10EC, 0x8139 }, { 0x10EC, 0x8138 }, { 0x1113, 0x1211 }, { 0x1186, 0x1300 }, { 0x1186, 0x1340 },
    { 0x13D1, 0xAB06 }, { 0x1259, 0xA117 }, { 0x14EA, 0xAB06 }, { 0x4033, 0x1360 }, { 0x1743, 0x8139 },
    { 0x021B, 0x8139 }, { 0x1500, 0x1360 }, { 0x018A, 0x0106 }, { 0x126C, 0x1211 },
};

static bool rtl8139_match(const pci_dev_t *p) {
    for (unsigned k = 0; k < ARRAY_LEN(ids); k++) if (p->vendor == ids[k].ven && p->device == ids[k].dev) return true;
    return false;
}
DRIVER_PCI(drv_rtl8139, "rtl8139", "Realtek RTL8139 Fast Ethernet", DC_NETWORK, rtl8139_match);

bool rtl8139_probe(nic_ops_t *ops) {
    pci_dev_t *devs; int n = pci_list(&devs);
    for (int i = 0; i < n; i++) {
        bool ok = false;
        for (unsigned k = 0; k < ARRAY_LEN(ids); k++) if (devs[i].vendor == ids[k].ven && devs[i].device == ids[k].dev) ok = true;
        if (!ok) continue;
        pci_dev_t *p = &devs[i];
        bool isio; uintptr_t b0 = pci_bar(p, 0, &isio);
        if (!b0 || !isio) { klog("rtl8139: no I/O BAR"); continue; }
        io = (uint16_t)b0;
        pci_enable(p, true, false);
        outb(io + CONFIG1, 0);                          /* wake up */
        outb(io + CR, 0x10);                            /* software reset */
        uint32_t t0 = timer_ticks();
        while ((inb(io + CR) & 0x10) && timer_ticks() - t0 < 100) {}
        for (int k = 0; k < 6; k++) netif.mac[k] = inb(io + IDR0 + k);
        rxring = kmalloc_aligned(RXRING + 16 + 2048, 16);
        txbuf = kmalloc_aligned(4 * TXBUF, 16);
        memset(rxring, 0, RXRING + 16 + 2048);
        outw(io + IMR, 0);                              /* polled */
        outb(io + CR, 0x0C);                            /* enable RX/TX before setting their configuration */
        outl(io + RBSTART, (uint32_t)dma_addr(rxring));
        /* RCR: no FIFO threshold, 8 KB ring with WRAP, unlimited DMA burst, accept broadcast/multicast/my MAC */
        outl(io + RCR, 0x0000E78E);
        outl(io + TCR, (3u << 24) | (6u << 8));         /* standard IFG, 1024-byte DMA burst */
        outl(io + MPC, 0);
        rx_off = 0; tx_cur = 0;
        outw(io + CAPR, (uint16_t)(rx_off - 16));
        snprintf(netif.driver, sizeof netif.driver, "Realtek RTL8139 (%04x:%04x)", p->vendor, p->device);
        ops->name = netif.driver; ops->send = rt_send; ops->poll = rt_poll; ops->link_up = rt_link;
        klog("rtl8139: at io %04x, MAC %02x:%02x:%02x:%02x:%02x:%02x, link %s", io,
             netif.mac[0], netif.mac[1], netif.mac[2], netif.mac[3], netif.mac[4], netif.mac[5], rt_link() ? "up" : "down");
        dev_claim(p, &drv_rtl8139, netif.driver);
        return true;
    }
    return false;
}
