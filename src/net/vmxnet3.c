/* VMware VMXNET3 paravirtual Ethernet (vSphere/ESXi, Workstation, Fusion,
 * Player; also QEMU -nic model=vmxnet3). Revision 1 of the device interface:
 * one transmit queue, one receive queue (ring 1 holds the buffers, ring 2 is
 * left empty), polled, no offloads. Structure layouts follow the
 * vmxnet3_defs.h interface that VMware publishes with its Linux driver. */
#include "kernel.h"
#include "net.h"

/* BAR0 (PT) */
#define PT_IMR     0x000
#define PT_TXPROD  0x600
#define PT_RXPROD  0x800
#define PT_RXPROD2 0xA00
/* BAR1 (VD) */
#define VD_VRRS 0x00
#define VD_UVRS 0x08
#define VD_DSAL 0x10
#define VD_DSAH 0x18
#define VD_CMD  0x20
#define VD_MACL 0x28
#define VD_MACH 0x30
#define VD_ICR  0x38
#define VD_ECR  0x40

#define CMD_ACTIVATE_DEV      0xCAFE0000u
#define CMD_QUIESCE_DEV       0xCAFE0001u
#define CMD_RESET_DEV         0xCAFE0002u
#define CMD_UPDATE_RX_MODE    0xCAFE0003u
#define CMD_UPDATE_VLAN_FILTERS 0xCAFE0005u
#define CMD_GET_LINK          0xF00D0002u

#define NTX 32
#define NTXC 32
#define NRX 64
#define NRX2 32
#define NRXC (NRX + NRX2)
#define BUFSZ 2048
#define DATA_DESC 128

typedef struct PACKED { uint64_t addr; uint32_t w2, w3; } vdesc_t;         /* tx/rx descriptors and completions: 16 bytes */

typedef struct PACKED {
    /* Vmxnet3_DriverShared */
    uint32_t magic, pad;
    /* devRead.misc */
    uint32_t drv_version, gos, rev_spt, upt_spt;
    uint64_t upt_features, dd_pa, queue_desc_pa;
    uint32_t dd_len, queue_desc_len, mtu;
    uint16_t max_rx_sg; uint8_t num_tx_queues, num_rx_queues;
    uint32_t misc_rsvd[4];
    /* devRead.intrConf */
    uint8_t auto_mask, num_intrs, event_intr_idx, mod_levels[25];
    uint32_t intr_ctrl, intr_rsvd[2];
    /* devRead.rxFilterConf */
    uint32_t rx_mode; uint16_t mf_table_len, pad1; uint64_t mf_table_pa; uint32_t vf_table[128];
    /* devRead.rssConfDesc, pmConfDesc, pluginConfDesc */
    uint32_t var_conf[12];
    uint32_t ecr, reserved;
    uint32_t cmd_info[4];
} shared_t;

typedef struct PACKED {
    /* Vmxnet3_TxQueueDesc: ctrl */
    uint32_t num_deferred, threshold; uint64_t ctrl_rsvd;
    /* conf */
    uint64_t ring_pa, data_ring_pa, comp_ring_pa, dd_pa, rsvd;
    uint32_t ring_size, data_ring_size, comp_ring_size, dd_len;
    uint8_t intr_idx, pad1; uint16_t data_desc_size; uint8_t pad2[4];
    /* status, stats, padding to 256 */
    uint8_t stopped, pad3[3]; uint32_t error;
    uint8_t stats_and_pad[168];
} txqdesc_t;

typedef struct PACKED {
    /* Vmxnet3_RxQueueDesc: ctrl */
    uint8_t update_prod, pad0[7]; uint64_t ctrl_rsvd;
    /* conf */
    uint64_t ring_pa[2], comp_ring_pa, dd_pa, data_ring_pa;
    uint32_t ring_size[2], comp_ring_size, dd_len;
    uint8_t intr_idx, pad1; uint16_t data_desc_size; uint8_t pad2[4];
    uint8_t stopped, pad3[3]; uint32_t error;
    uint8_t stats_and_pad[168];
} rxqdesc_t;

_Static_assert(sizeof(shared_t) == 720, "vmxnet3 DriverShared layout");
_Static_assert(sizeof(txqdesc_t) == 256 && sizeof(rxqdesc_t) == 256, "vmxnet3 queue descriptor layout");

static volatile uint8_t *bar0, *bar1;
static shared_t *sh;
static txqdesc_t *tqd; static rxqdesc_t *rqd;
static vdesc_t *txr, *txc, *rxr, *rxr2, *rxc;
static uint8_t *txbuf, *rxbuf, *txdata;
static uint32_t tx_next, tx_gen, txc_next, txc_gen, tx_inflight;
static uint32_t rx_fill, rx_gen, rxc_next, rxc_gen;

static inline void wr0(uint32_t r, uint32_t v) { *(volatile uint32_t *)(bar0 + r) = v; }
static inline uint32_t rd1(uint32_t r) { return *(volatile uint32_t *)(bar1 + r); }
static inline void wr1(uint32_t r, uint32_t v) { *(volatile uint32_t *)(bar1 + r) = v; }
static uint32_t cmd(uint32_t c) { wr1(VD_CMD, c); return rd1(VD_CMD); }

static void tx_reap(void) {
    for (;;) {
        vdesc_t *c = &txc[txc_next];
        if (((*(volatile uint32_t *)&c->w3) >> 31) != txc_gen) break;
        if (tx_inflight) tx_inflight--;
        if (++txc_next == NTXC) { txc_next = 0; txc_gen ^= 1; }
    }
}

static bool vx_send(const void *frame, uint32_t len) {
    if (len > BUFSZ) return false;
    tx_reap();
    uint32_t t0 = timer_ticks();
    while (tx_inflight >= NTX - 1) { tx_reap(); if (timer_ticks() - t0 > 50) return false; }
    vdesc_t *d = &txr[tx_next];
    memcpy(txbuf + tx_next * BUFSZ, frame, len);
    if (len < 60) { memset(txbuf + tx_next * BUFSZ + len, 0, 60 - len); len = 60; }
    d->addr = dma_addr(txbuf + tx_next * BUFSZ);
    d->w3 = (1u << 12) | (1u << 13);                    /* EOP, CQ: completion wanted */
    __asm__ volatile("mfence" ::: "memory");
    *(volatile uint32_t *)&d->w2 = len | (tx_gen << 14);   /* handing it over: the gen bit goes last */
    __asm__ volatile("mfence" ::: "memory");
    if (++tx_next == NTX) { tx_next = 0; tx_gen ^= 1; }
    tx_inflight++;
    tqd->num_deferred = 0;
    wr0(PT_TXPROD, tx_next);
    return true;
}

static void rx_give(void) {
    vdesc_t *d = &rxr[rx_fill];
    __asm__ volatile("mfence" ::: "memory");
    *(volatile uint32_t *)&d->w2 = BUFSZ | (rx_gen << 31);   /* head buffer, owned by the device */
    if (++rx_fill == NRX) { rx_fill = 0; rx_gen ^= 1; }
}

static void vx_poll(void) {
    bool gave = false;
    for (int n = 0; n < NRX; n++) {
        vdesc_t *c = &rxc[rxc_next];
        uint32_t w3 = *(volatile uint32_t *)&c->w3;
        if ((w3 >> 31) != rxc_gen) break;
        __asm__ volatile("" ::: "memory");
        uint32_t w0 = (uint32_t)c->addr, w2 = c->w2;
        uint32_t idx = w0 & 0xFFF, rq = (w0 >> 16) & 0x3FF, len = w2 & 0x3FFF;
        bool sop = w0 & (1u << 15), eop = w0 & (1u << 14), err = w2 & (1u << 14);
        if (rq == 0 && idx < NRX && sop && eop && !err && len) net_rx(rxbuf + idx * BUFSZ, len);
        else netif.rx_dropped++;
        if (rq == 0) { rx_give(); gave = true; }
        if (++rxc_next == NRXC) { rxc_next = 0; rxc_gen ^= 1; }
    }
    if (gave) wr0(PT_RXPROD, rx_fill);
    tx_reap();
}

static bool vx_link(void) { return cmd(CMD_GET_LINK) & 1; }

bool vmxnet3_probe(nic_ops_t *ops) {
    pci_dev_t *devs; int n = pci_list(&devs);
    for (int i = 0; i < n; i++) {
        if (devs[i].vendor != 0x15AD || devs[i].device != 0x07B0) continue;
        pci_dev_t *p = &devs[i];
        bool io0, io1;
        uintptr_t b0 = pci_bar(p, 0, &io0), b1 = pci_bar(p, 1, &io1);
        if (!b0 || !b1 || io0 || io1) { klog("vmxnet3: BARs not usable"); continue; }
        pci_enable(p, false, true);
        paging_set_uncached(b0); paging_set_uncached(b1);
        bar0 = (volatile uint8_t *)b0; bar1 = (volatile uint8_t *)b1;
        uint32_t vrrs = rd1(VD_VRRS), uvrs = rd1(VD_UVRS);
        if (!(vrrs & 1)) { klog("vmxnet3: device does not offer revision 1 (%08x)", vrrs); continue; }
        wr1(VD_VRRS, 1);
        if (uvrs & 1) wr1(VD_UVRS, 1);
        cmd(CMD_RESET_DEV);
        uint32_t macl = rd1(VD_MACL), mach = rd1(VD_MACH);
        for (int k = 0; k < 4; k++) netif.mac[k] = macl >> (8 * k);
        netif.mac[4] = mach; netif.mac[5] = mach >> 8;
        /* memory the device reads and writes */
        sh = kmalloc_aligned(sizeof *sh, 64); memset(sh, 0, sizeof *sh);
        uint8_t *qd = kmalloc_aligned(512, 128); memset(qd, 0, 512);
        tqd = (txqdesc_t *)qd; rqd = (rxqdesc_t *)(qd + 256);
        txr = kmalloc_aligned(NTX * 16, 512); txc = kmalloc_aligned(NTXC * 16, 512);
        rxr = kmalloc_aligned(NRX * 16, 512); rxr2 = kmalloc_aligned(NRX2 * 16, 512); rxc = kmalloc_aligned(NRXC * 16, 512);
        memset(txr, 0, NTX * 16); memset(txc, 0, NTXC * 16); memset(rxr, 0, NRX * 16); memset(rxr2, 0, NRX2 * 16); memset(rxc, 0, NRXC * 16);
        txbuf = kmalloc_aligned(NTX * BUFSZ, 64); rxbuf = kmalloc_aligned(NRX * BUFSZ, 64);
        txdata = kmalloc_aligned(NTX * DATA_DESC, 512); memset(txdata, 0, NTX * DATA_DESC);
        tx_next = 0; tx_gen = 1; txc_next = 0; txc_gen = 1; tx_inflight = 0;
        rx_fill = 0; rx_gen = 1; rxc_next = 0; rxc_gen = 1;
        for (int k = 0; k < NRX; k++) rxr[k].addr = dma_addr(rxbuf + k * BUFSZ);
        for (int k = 0; k < NRX - 1; k++) rx_give();   /* one slot stays empty so full and empty differ */
        /* queue descriptors */
        tqd->threshold = 1;
        tqd->ring_pa = dma_addr(txr); tqd->data_ring_pa = dma_addr(txdata); tqd->comp_ring_pa = dma_addr(txc);
        tqd->ring_size = NTX; tqd->data_ring_size = NTX; tqd->comp_ring_size = NTXC;
        tqd->data_desc_size = DATA_DESC;
        rqd->ring_pa[0] = dma_addr(rxr); rqd->ring_pa[1] = dma_addr(rxr2); rqd->comp_ring_pa = dma_addr(rxc);
        rqd->ring_size[0] = NRX; rqd->ring_size[1] = NRX2; rqd->comp_ring_size = NRXC;
        /* shared area */
        sh->magic = 0xBABEFEE1;
        sh->drv_version = 0x01000000;
        sh->gos = (sizeof(void *) == 8 ? 2 : 1) | (1u << 2);   /* 32/64-bit, "Linux"-style generic guest */
        sh->rev_spt = 1; sh->upt_spt = 1;
        sh->queue_desc_pa = dma_addr(qd); sh->queue_desc_len = 512;
        sh->mtu = 1500; sh->max_rx_sg = 1;
        sh->num_tx_queues = 1; sh->num_rx_queues = 1;
        sh->auto_mask = 1; sh->num_intrs = 1; sh->intr_ctrl = 1;   /* VMXNET3_IC_DISABLE_ALL: polled */
        sh->rx_mode = 1 | 4 | 8;                        /* unicast, broadcast, all multicast */
        sh->vf_table[0] = 1;                            /* VLAN 0: untagged frames */
        wr1(VD_DSAL, (uint32_t)dma_addr(sh)); wr1(VD_DSAH, (uint32_t)(dma_addr(sh) >> 32));
        uint32_t r = cmd(CMD_ACTIVATE_DEV);
        if (r) { klog("vmxnet3: activation refused (%08x)", r); continue; }
        wr0(PT_IMR, 1);                                 /* keep interrupt 0 masked */
        wr0(PT_RXPROD, rx_fill); wr0(PT_RXPROD2, 0);
        cmd(CMD_UPDATE_RX_MODE); cmd(CMD_UPDATE_VLAN_FILTERS);
        snprintf(netif.driver, sizeof netif.driver, "VMware VMXNET3 Ethernet Adapter");
        ops->name = netif.driver; ops->send = vx_send; ops->poll = vx_poll; ops->link_up = vx_link;
        klog("vmxnet3: at %08x/%08x, MAC %02x:%02x:%02x:%02x:%02x:%02x, link %s", (uint32_t)b0, (uint32_t)b1,
             netif.mac[0], netif.mac[1], netif.mac[2], netif.mac[3], netif.mac[4], netif.mac[5], vx_link() ? "up" : "down");
        return true;
    }
    return false;
}
