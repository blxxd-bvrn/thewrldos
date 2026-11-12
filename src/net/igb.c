/* Intel 82575/82576/I350/I354/I210/I211 gigabit Ethernet (Linux "igb"),
 * polled. Common on desktop boards (I210/I211), servers (I350) and in QEMU
 * (-nic model=igb, an 82576). Queue 0 only, "advanced" one-buffer
 * descriptors (the format the whole family shares). */
#include "kernel.h"
#include "net.h"
#include "devmgr.h"

#define CTRL     0x0000
#define STATUS   0x0008
#define EECD     0x0010
#define EERD     0x0014
#define CTRL_EXT 0x0018
#define MDIC     0x0020
#define ICR      0x00C0
#define IMC      0x00D8
#define RCTL     0x0100
#define TCTL     0x0400
#define RDBAL0   0xC000
#define RDBAH0   0xC004
#define RDLEN0   0xC008
#define SRRCTL0  0xC00C
#define RDH0     0xC010
#define RDT0     0xC018
#define RXDCTL0  0xC028
#define TDBAL0   0xE000
#define TDBAH0   0xE004
#define TDLEN0   0xE008
#define TDH0     0xE010
#define TDT0     0xE018
#define TXDCTL0  0xE028
#define MTA      0x5200
#define RAL0     0x5400
#define RAH0     0x5404

#define NRX 64
#define NTX 32
#define BUFSZ 2048

/* read format: buffer and header addresses; write-back: info, RSS, status/errors, length, VLAN */
typedef union PACKED { struct PACKED { uint64_t pkt, hdr; } rd; struct PACKED { uint32_t info, rss, status; uint16_t len, vlan; } wb; } rxd_t;
/* read format: buffer, cmd_type_len, olinfo_status; write-back puts DD in bit 0 of the last dword */
typedef struct PACKED { uint64_t addr; uint32_t cmd_type_len, olinfo; } txd_t;
#define TX_DTYP_DATA (3u << 20)
#define TX_EOP  (1u << 24)
#define TX_IFCS (1u << 25)
#define TX_RS   (1u << 27)
#define TX_DEXT (1u << 29)

static volatile uint8_t *mmio;
static rxd_t *rx; static txd_t *tx;
static uint8_t *rxbuf, *txbuf;
static uint32_t rx_cur, tx_cur;

static inline uint32_t rd(uint32_t r) { return *(volatile uint32_t *)(mmio + r); }
static inline void wr(uint32_t r, uint32_t v) { *(volatile uint32_t *)(mmio + r) = v; }

static bool mdic(int phy, int reg, bool write, uint16_t *val) {
    wr(MDIC, (write ? *val : 0) | ((uint32_t)reg << 16) | ((uint32_t)phy << 21) | (write ? 1u << 26 : 2u << 26));
    for (int k = 0; k < 2000; k++) {
        uint32_t v = rd(MDIC);
        if (v & (1u << 28)) { if (v & (1u << 30)) return false; if (!write) *val = v & 0xFFFF; return true; }
        for (int j = 0; j < 10; j++) io_wait();
    }
    return false;
}

static bool igb_send(const void *frame, uint32_t len) {
    if (len > BUFSZ) return false;
    txd_t *d = &tx[tx_cur];
    uint32_t t0 = timer_ticks();
    while (d->cmd_type_len && !(*(volatile uint32_t *)&d->olinfo & 1) && timer_ticks() - t0 < 50) {}
    memcpy(txbuf + tx_cur * BUFSZ, frame, len);
    if (len < 60) { memset(txbuf + tx_cur * BUFSZ + len, 0, 60 - len); len = 60; }
    d->addr = dma_addr(txbuf + tx_cur * BUFSZ);
    d->olinfo = len << 14;                              /* PAYLEN */
    d->cmd_type_len = len | TX_DTYP_DATA | TX_EOP | TX_IFCS | TX_RS | TX_DEXT;
    __asm__ volatile("mfence" ::: "memory");
    tx_cur = (tx_cur + 1) % NTX;
    wr(TDT0, tx_cur);
    return true;
}

static void igb_poll(void) {
    for (int n = 0; n < NRX; n++) {
        rxd_t *d = &rx[rx_cur];
        uint32_t st = *(volatile uint32_t *)&d->wb.status;
        if (!(st & 1)) break;                           /* DD */
        uint16_t len = d->wb.len;
        if ((st & 2) && !(st & 0xFFF00000u) && len && len <= BUFSZ) net_rx(rxbuf + rx_cur * BUFSZ, len);
        else netif.rx_dropped++;
        d->rd.pkt = dma_addr(rxbuf + rx_cur * BUFSZ); d->rd.hdr = 0;   /* write-back overwrote the addresses */
        __asm__ volatile("mfence" ::: "memory");
        wr(RDT0, rx_cur);
        rx_cur = (rx_cur + 1) % NRX;
    }
}

static bool igb_link(void) { return (rd(STATUS) & 2) != 0; }

static const struct { uint16_t id; const char *name; } ids[] = {
    { 0x10A7, "82575EB" }, { 0x10A9, "82575EB" }, { 0x10D6, "82575GB" },
    { 0x10C9, "82576" }, { 0x10E6, "82576" }, { 0x10E7, "82576" }, { 0x10E8, "82576" }, { 0x1526, "82576" },
    { 0x150A, "82576" }, { 0x1518, "82576" }, { 0x150D, "82576" },
    { 0x1521, "I350" }, { 0x1522, "I350" }, { 0x1523, "I350" }, { 0x1524, "I350" }, { 0x1546, "I350" },
    { 0x1533, "I210" }, { 0x1536, "I210" }, { 0x1537, "I210" }, { 0x1538, "I210" }, { 0x157B, "I210" },
    { 0x157C, "I210" }, { 0x15F6, "I210" }, { 0x1539, "I211" },
    { 0x1F40, "I354" }, { 0x1F41, "I354" }, { 0x1F45, "I354" },
    { 0x0438, "DH89xxCC" }, { 0x043A, "DH89xxCC" }, { 0x043C, "DH89xxCC" }, { 0x0440, "DH89xxCC" },
};

static bool igb_match(const pci_dev_t *p) {
    if (p->vendor != 0x8086) return false;
    for (unsigned k = 0; k < ARRAY_LEN(ids); k++) if (p->device == ids[k].id) return true;
    return false;
}
DRIVER_PCI(drv_igb, "igb", "Intel 82575/82576, I210/I211, I350 Gigabit Ethernet", DC_NETWORK, igb_match);

bool igb_probe(nic_ops_t *ops) {
    pci_dev_t *devs; int n = pci_list(&devs);
    for (int i = 0; i < n; i++) {
        if (devs[i].vendor != 0x8086) continue;
        const char *name = NULL;
        for (unsigned k = 0; k < ARRAY_LEN(ids); k++) if (devs[i].device == ids[k].id) name = ids[k].name;
        if (!name) continue;
        pci_dev_t *p = &devs[i];
        bool isio; uintptr_t base = pci_bar(p, 0, &isio);
        if (!base || isio) { klog("igb: BAR0 not usable"); continue; }
        pci_enable(p, false, true);
        paging_set_uncached(base);
        mmio = (volatile uint8_t *)base;
        /* reset; the NVM reloads the MAC address and defaults afterwards */
        wr(IMC, 0xFFFFFFFF);
        wr(RCTL, 0); wr(TCTL, 0x8);
        sleep_ms(10);
        wr(CTRL, rd(CTRL) | (1u << 26));
        sleep_ms(20);
        uint32_t t0 = timer_ticks();
        while ((rd(CTRL) & (1u << 26)) && timer_ticks() - t0 < 100) {}
        t0 = timer_ticks();
        while (!(rd(EECD) & (1u << 9)) && timer_ticks() - t0 < 50) {}       /* NVM auto-read done */
        wr(IMC, 0xFFFFFFFF); (void)rd(ICR);
        wr(CTRL_EXT, rd(CTRL_EXT) | (1u << 28));        /* DRV_LOAD: tell the management firmware a driver owns the port */
        wr(CTRL, (rd(CTRL) | (1u << 6)) & ~((1u << 3) | (1u << 31) | (1u << 7)));   /* SLU; clear LRST, PHY_RST, ILOS */
        /* the copper PHY may have been left powered down: power it up and (re)start
         * autonegotiation; after the reset the link only comes up once that is done */
        uint16_t bmcr;
        if (mdic(1, 0, false, &bmcr)) { bmcr = (bmcr & ~0x0800) | 0x1200; mdic(1, 0, true, &bmcr); }
        /* MAC address */
        uint32_t ral = rd(RAL0), rah = rd(RAH0);
        if ((rah & 0x80000000u) && (ral || (rah & 0xFFFF))) {
            for (int k = 0; k < 4; k++) netif.mac[k] = ral >> (8 * k);
            netif.mac[4] = rah; netif.mac[5] = rah >> 8;
        } else {
            for (int k = 0; k < 3; k++) {
                wr(EERD, ((uint32_t)k << 2) | 1);
                uint32_t v = 0; t0 = timer_ticks();
                do v = rd(EERD); while (!(v & 2) && timer_ticks() - t0 < 20);
                netif.mac[k * 2] = v >> 16; netif.mac[k * 2 + 1] = v >> 24;
            }
            wr(RAL0, netif.mac[0] | (netif.mac[1] << 8) | (netif.mac[2] << 16) | ((uint32_t)netif.mac[3] << 24));
            wr(RAH0, netif.mac[4] | (netif.mac[5] << 8) | 0x80000000u);
        }
        for (int k = 0; k < 128; k++) wr(MTA + k * 4, 0);
        /* receive queue 0 */
        rx = kmalloc_aligned(sizeof(rxd_t) * NRX, 128); memset(rx, 0, sizeof(rxd_t) * NRX);
        rxbuf = kmalloc_aligned(NRX * BUFSZ, 64);
        for (int k = 0; k < NRX; k++) { rx[k].rd.pkt = dma_addr(rxbuf + k * BUFSZ); rx[k].rd.hdr = 0; }
        wr(RDBAL0, (uint32_t)dma_addr(rx)); wr(RDBAH0, (uint32_t)(dma_addr(rx) >> 32)); wr(RDLEN0, sizeof(rxd_t) * NRX);
        wr(SRRCTL0, (BUFSZ / 1024) | (1u << 25) | (1u << 31));     /* 2 KB buffers, advanced one-buffer descriptors, drop when full */
        wr(RDH0, 0); wr(RDT0, 0); rx_cur = 0;
        wr(RXDCTL0, (rd(RXDCTL0) & ~0x1F1F1Fu) | (1u << 25) | (8u) | (8u << 8) | (1u << 16));
        t0 = timer_ticks();
        while (!(rd(RXDCTL0) & (1u << 25)) && timer_ticks() - t0 < 20) {}
        wr(RCTL, (1u << 1) | (1u << 15) | (1u << 26));  /* EN, BAM, SECRC; 2048-byte buffers */
        wr(RDT0, NRX - 1);                              /* only once the queue is enabled */
        /* transmit queue 0 */
        tx = kmalloc_aligned(sizeof(txd_t) * NTX, 128); memset(tx, 0, sizeof(txd_t) * NTX);
        txbuf = kmalloc_aligned(NTX * BUFSZ, 64);
        wr(TDBAL0, (uint32_t)dma_addr(tx)); wr(TDBAH0, (uint32_t)(dma_addr(tx) >> 32)); wr(TDLEN0, sizeof(txd_t) * NTX);
        wr(TDH0, 0); wr(TDT0, 0); tx_cur = 0;
        wr(TXDCTL0, (rd(TXDCTL0) & ~0x1F1F1Fu) | (1u << 25) | 8u | (1u << 8) | (1u << 16));
        t0 = timer_ticks();
        while (!(rd(TXDCTL0) & (1u << 25)) && timer_ticks() - t0 < 20) {}
        wr(TCTL, (1u << 1) | (1u << 3) | (0x0F << 4) | (0x3F << 12) | (1u << 24));   /* EN, PSP, CT, COLD, RTLC */
        snprintf(netif.driver, sizeof netif.driver, "Intel %s Gigabit (8086:%04x)", name, p->device);
        ops->name = netif.driver; ops->send = igb_send; ops->poll = igb_poll; ops->link_up = igb_link;
        klog("igb: %s at %08x, MAC %02x:%02x:%02x:%02x:%02x:%02x, link %s", netif.driver, (uint32_t)base,
             netif.mac[0], netif.mac[1], netif.mac[2], netif.mac[3], netif.mac[4], netif.mac[5], igb_link() ? "up" : "down");
        dev_claim(p, &drv_igb, netif.driver);
        return true;
    }
    return false;
}
