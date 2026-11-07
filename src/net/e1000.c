/* Intel PRO/1000 (e1000 / e1000e family) Ethernet driver, polled.
 * This is the default network card of QEMU, VirtualBox ("Intel PRO/1000 MT
 * Desktop") and most VMware configurations. */
#include "kernel.h"
#include "net.h"

#define REG_CTRL   0x0000
#define REG_STATUS 0x0008
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

#define NRX 64
#define NTX 32
#define BUFSZ 2048

typedef struct PACKED { uint64_t addr; uint16_t len, csum; uint8_t status, errors; uint16_t special; } rxd_t;
typedef struct PACKED { uint64_t addr; uint16_t len; uint8_t cso, cmd, status, css; uint16_t special; } txd_t;

static volatile uint8_t *mmio;
static rxd_t *rx; static txd_t *tx;
static uint8_t *rxbuf, *txbuf;
static uint32_t rx_cur, tx_cur;
static bool eerd_new_layout;

static inline uint32_t rd(uint32_t r) { return *(volatile uint32_t *)(mmio + r); }
static inline void wr(uint32_t r, uint32_t v) { *(volatile uint32_t *)(mmio + r) = v; }

static uint16_t eeprom_read(uint8_t a) {
    wr(REG_EERD, eerd_new_layout ? ((uint32_t)a << 2) | 1 : ((uint32_t)a << 8) | 1);
    uint32_t t0 = timer_ticks();
    do {
        uint32_t v = rd(REG_EERD);
        if (eerd_new_layout ? (v & 2) : (v & 0x10)) return v >> 16;
    } while (timer_ticks() - t0 < 20);
    return 0;
}
static bool e1000_send(const void *frame, uint32_t len) {
    if (len > BUFSZ) return false;
    txd_t *d = &tx[tx_cur];
    { uint32_t t0 = timer_ticks(); while (d->cmd && !(*(volatile uint8_t *)&d->status & 1) && timer_ticks() - t0 < 50) {} }   /* wait for this slot to be free */
    memcpy(txbuf + tx_cur * BUFSZ, frame, len);
    if (len < 60) { memset(txbuf + tx_cur * BUFSZ + len, 0, 60 - len); len = 60; }
    d->addr = (uint32_t)(uintptr_t)(txbuf + tx_cur * BUFSZ);
    d->len = len; d->cso = 0; d->css = 0; d->special = 0;
    d->status = 0;
    d->cmd = 0x0B;                         /* EOP | IFCS | RS */
    tx_cur = (tx_cur + 1) % NTX;
    wr(REG_TDT, tx_cur);
    return true;
}
static void e1000_poll(void) {
    for (int n = 0; n < NRX; n++) {
        rxd_t *d = &rx[rx_cur];
        if (!(d->status & 1)) break;
        if ((d->status & 2) && !d->errors && d->len) net_rx(rxbuf + rx_cur * BUFSZ, d->len);
        else netif.rx_dropped++;
        d->status = 0;
        wr(REG_RDT, rx_cur);
        rx_cur = (rx_cur + 1) % NRX;
    }
}
static bool e1000_link(void) { return (rd(REG_STATUS) & 2) != 0; }

static const uint16_t ids[] = { 0x100E, 0x100F, 0x1004, 0x1008, 0x1009, 0x100C, 0x100D, 0x1010, 0x1011, 0x1012, 0x1013, 0x1015, 0x1016, 0x1017,
    0x1018, 0x1019, 0x101A, 0x101D, 0x101E, 0x1026, 0x1027, 0x1028, 0x1075, 0x1076, 0x1077, 0x1078, 0x1079, 0x107A, 0x107B, 0x107C,
    0x10B5, 0x10D3, 0x10F6, 0x10EA, 0x10EF, 0x1502, 0x1503, 0x153A, 0x153B, 0x15A0, 0x15A1, 0x15A2, 0x15A3, 0x15B7, 0x15B8, 0x156F, 0x1570 };

bool e1000_probe(nic_ops_t *ops) {
    pci_dev_t devs[48];
    int n = pci_scan(devs, 48);
    for (int i = 0; i < n; i++) {
        if (devs[i].vendor != 0x8086) continue;
        bool ok = false;
        for (unsigned k = 0; k < ARRAY_LEN(ids); k++) if (devs[i].device == ids[k]) ok = true;
        if (!ok) continue;
        pci_dev_t *p = &devs[i];
        uint32_t bar0 = pci_cfg_read(p->bus, p->dev, p->fn, 0x10);
        if (bar0 & 1) { klog("e1000: BAR0 is I/O space, unsupported"); continue; }
        uint32_t cmd = pci_cfg_read(p->bus, p->dev, p->fn, 0x04);
        pci_cfg_write(p->bus, p->dev, p->fn, 0x04, (cmd & 0xFFFF) | 0x06);     /* memory space + bus master */
        uint32_t base = bar0 & ~0xFu;
        paging_set_uncached(base);
        mmio = (volatile uint8_t *)(uintptr_t)base;
        eerd_new_layout = p->device >= 0x10B5 && p->device != 0x1075 && p->device != 0x1076 && p->device != 0x1077 && p->device != 0x1078 && p->device != 0x1079;
        /* reset, then link up */
        wr(REG_IMC, 0xFFFFFFFF);
        wr(REG_CTRL, rd(REG_CTRL) | (1u << 26));
        for (int k = 0; k < 100000 && (rd(REG_CTRL) & (1u << 26)); k++) io_wait();
        sleep_ms(10);
        wr(REG_IMC, 0xFFFFFFFF); (void)rd(REG_ICR);
        wr(REG_CTRL, (rd(REG_CTRL) | (1 << 6) | (1 << 5)) & ~((1u << 3) | (1u << 31) | (1u << 7)));   /* SLU, ASDE; clear LRST, PHY_RST, ILOS */
        /* MAC address: receive address register, else EEPROM */
        uint32_t ral = rd(REG_RAL), rah = rd(REG_RAH);
        if ((rah & 0x80000000u) && (ral || (rah & 0xFFFF))) {
            for (int k = 0; k < 4; k++) netif.mac[k] = ral >> (8 * k);
            netif.mac[4] = rah; netif.mac[5] = rah >> 8;
        } else {
            for (int k = 0; k < 3; k++) { uint16_t w = eeprom_read(k); netif.mac[k * 2] = w; netif.mac[k * 2 + 1] = w >> 8; }
            wr(REG_RAL, netif.mac[0] | (netif.mac[1] << 8) | (netif.mac[2] << 16) | ((uint32_t)netif.mac[3] << 24));
            wr(REG_RAH, netif.mac[4] | (netif.mac[5] << 8) | 0x80000000u);
        }
        for (int k = 0; k < 128; k++) wr(REG_MTA + k * 4, 0);
        /* receive ring */
        rx = kmalloc_aligned(sizeof(rxd_t) * NRX, 128); memset(rx, 0, sizeof(rxd_t) * NRX);
        rxbuf = kmalloc_aligned(NRX * BUFSZ, 16);
        for (int k = 0; k < NRX; k++) rx[k].addr = (uint32_t)(uintptr_t)(rxbuf + k * BUFSZ);
        wr(REG_RDBAL, (uint32_t)(uintptr_t)rx); wr(REG_RDBAH, 0); wr(REG_RDLEN, sizeof(rxd_t) * NRX);
        wr(REG_RDH, 0); wr(REG_RDT, NRX - 1); rx_cur = 0;
        wr(REG_RCTL, (1 << 1) | (1 << 15) | (1 << 26));        /* EN, BAM (broadcast), SECRC; 2048-byte buffers */
        /* transmit ring */
        tx = kmalloc_aligned(sizeof(txd_t) * NTX, 128); memset(tx, 0, sizeof(txd_t) * NTX);
        txbuf = kmalloc_aligned(NTX * BUFSZ, 16);
        wr(REG_TDBAL, (uint32_t)(uintptr_t)tx); wr(REG_TDBAH, 0); wr(REG_TDLEN, sizeof(txd_t) * NTX);
        wr(REG_TDH, 0); wr(REG_TDT, 0); tx_cur = 0;
        wr(REG_TCTL, (1 << 1) | (1 << 3) | (0x0F << 4) | (0x40 << 12));   /* EN, PSP, CT, COLD */
        wr(REG_TIPG, 0x0060200A);
        snprintf(netif.driver, sizeof netif.driver, "Intel PRO/1000 (8086:%04x)", p->device);
        ops->name = netif.driver; ops->send = e1000_send; ops->poll = e1000_poll; ops->link_up = e1000_link;
        klog("e1000: %s at %08x, MAC %02x:%02x:%02x:%02x:%02x:%02x, link %s", netif.driver, base,
             netif.mac[0], netif.mac[1], netif.mac[2], netif.mac[3], netif.mac[4], netif.mac[5], e1000_link() ? "up" : "down");
        return true;
    }
    return false;
}
