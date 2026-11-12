/* virtio-net: the paravirtual network card of QEMU/KVM, Proxmox, cloud
 * hypervisors and VirtualBox ("Paravirtualized Network"). Modern (virtio 1.0)
 * and legacy PCI transports, one receive and one transmit queue, polled.
 * Every packet is a two-descriptor chain, header then frame, which every
 * device accepts with or without VIRTIO_F_ANY_LAYOUT. */
#include "kernel.h"
#include "net.h"
#include "devmgr.h"
#include "virtio.h"

#define F_MAC    (1u << 5)
#define F_STATUS (1u << 16)
#define NRX 32
#define NTX 32
#define BUFSZ 2048

static vdev_t vn;
static virtq_t rxq, txq;
static uint8_t *rxhdr, *rxbuf, *txhdr, *txbuf;
static uint32_t hdrlen, features;
static uint16_t tx_next, tx_inflight;

static void post_rx(uint16_t slot) {
    virtq_t *q = &rxq;
    uint16_t idx = q->avail[1];
    q->avail[2 + idx % q->size] = slot * 2;
    __asm__ volatile("mfence" ::: "memory");
    q->avail[1] = idx + 1;
}

/* reclaim transmit chains the device has finished with */
static void tx_reap(void) {
    while (*(volatile uint16_t *)(txq.used + 2) != txq.last_used) { txq.last_used++; if (tx_inflight) tx_inflight--; }
}

static bool vn_send(const void *frame, uint32_t len) {
    if (len > BUFSZ) return false;
    tx_reap();
    uint32_t t0 = timer_ticks();
    while (tx_inflight >= NTX) { tx_reap(); if (timer_ticks() - t0 > 50) return false; }
    uint16_t slot = tx_next; tx_next = (tx_next + 1) % NTX;
    memset(txhdr + slot * 16, 0, 16);
    memcpy(txbuf + slot * BUFSZ, frame, len);
    vq_desc_t *d = &txq.desc[slot * 2];
    d[0].addr = dma_addr(txhdr + slot * 16); d[0].len = hdrlen; d[0].flags = VQ_NEXT; d[0].next = slot * 2 + 1;
    d[1].addr = dma_addr(txbuf + slot * BUFSZ); d[1].len = len; d[1].flags = 0; d[1].next = 0;
    uint16_t idx = txq.avail[1];
    txq.avail[2 + idx % txq.size] = slot * 2;
    __asm__ volatile("mfence" ::: "memory");
    txq.avail[1] = idx + 1;
    __asm__ volatile("mfence" ::: "memory");
    tx_inflight++;
    virtio_notify(&vn, &txq);
    return true;
}

static void vn_poll(void) {
    bool posted = false;
    for (int n = 0; n < NRX; n++) {
        uint16_t used_idx = *(volatile uint16_t *)(rxq.used + 2);
        if (used_idx == rxq.last_used) break;
        __asm__ volatile("" ::: "memory");
        volatile uint8_t *e = rxq.used + 4 + 8 * (rxq.last_used % rxq.size);
        uint32_t id = *(volatile uint32_t *)e, len = *(volatile uint32_t *)(e + 4);
        rxq.last_used++;
        uint16_t slot = id / 2;
        if (slot >= NRX) continue;
        if (len > hdrlen && len - hdrlen <= BUFSZ) net_rx(rxbuf + slot * BUFSZ, len - hdrlen);
        else netif.rx_dropped++;
        post_rx(slot); posted = true;
    }
    if (posted) { __asm__ volatile("mfence" ::: "memory"); virtio_notify(&vn, &rxq); }
    tx_reap();
}

static bool vn_link(void) {
    if (!(features & F_STATUS)) return true;
    return virtio_cfg16(&vn, 6) & 1;
}

static bool virtionet_match(const pci_dev_t *p) {
    return p->vendor == 0x1AF4 && (p->device == 0x1000 || p->device == 0x1041);
}
DRIVER_PCI(drv_virtionet, "virtionet", "VirtIO network adapter", DC_NETWORK, virtionet_match);

bool virtionet_probe(nic_ops_t *ops) {
    pci_dev_t *devs; int n = pci_list(&devs);
    for (int i = 0; i < n; i++) {
        if (devs[i].vendor != 0x1AF4 || (devs[i].device != 0x1000 && devs[i].device != 0x1041)) continue;
        pci_dev_t *p = &devs[i];
        memset(&vn, 0, sizeof vn);
        if (!virtio_transport(p, &vn) || !virtio_negotiate(&vn, F_MAC | F_STATUS, &features)) { klog("virtio-net: %02x:%02x.%d: setup failed", p->bus, p->dev, p->fn); continue; }
        hdrlen = vn.modern ? 12 : 10;                 /* num_buffers is always present in virtio 1.0 */
        if (!virtio_queue_setup(&vn, &rxq, 0, 256) || !virtio_queue_setup(&vn, &txq, 1, 256) || rxq.size < NRX * 2 || txq.size < NTX * 2) {
            klog("virtio-net: queues unavailable"); virtio_set_status(&vn, 0x80); continue;
        }
        rxhdr = kmalloc_aligned(NRX * 16, 64); rxbuf = kmalloc_aligned(NRX * BUFSZ, 64);
        txhdr = kmalloc_aligned(NTX * 16, 64); txbuf = kmalloc_aligned(NTX * BUFSZ, 64);
        memset(rxhdr, 0, NRX * 16);
        for (int k = 0; k < NRX; k++) {
            vq_desc_t *d = &rxq.desc[k * 2];
            d[0].addr = dma_addr(rxhdr + k * 16); d[0].len = hdrlen; d[0].flags = VQ_NEXT | VQ_WRITE; d[0].next = k * 2 + 1;
            d[1].addr = dma_addr(rxbuf + k * BUFSZ); d[1].len = BUFSZ; d[1].flags = VQ_WRITE; d[1].next = 0;
            post_rx(k);
        }
        tx_next = tx_inflight = 0;
        if (features & F_MAC) for (int k = 0; k < 6; k++) netif.mac[k] = virtio_cfg8(&vn, k);
        else {                                        /* no MAC from the host: make up a locally administered one */
            uint32_t r = timer_ticks() * 2654435761u ^ (p->bus << 16 | p->dev << 8);
            netif.mac[0] = 0x52; netif.mac[1] = 0x54; netif.mac[2] = 0x00;
            netif.mac[3] = r >> 16; netif.mac[4] = r >> 8; netif.mac[5] = r;
        }
        virtio_driver_ok(&vn);
        virtio_notify(&vn, &rxq);
        snprintf(netif.driver, sizeof netif.driver, "VirtIO Network Adapter");
        ops->name = netif.driver; ops->send = vn_send; ops->poll = vn_poll; ops->link_up = vn_link;
        klog("virtio-net: %s transport, MAC %02x:%02x:%02x:%02x:%02x:%02x, link %s", vn.modern ? "modern" : "legacy",
             netif.mac[0], netif.mac[1], netif.mac[2], netif.mac[3], netif.mac[4], netif.mac[5], vn_link() ? "up" : "down");
        dev_claim(p, &drv_virtionet, netif.driver);
        return true;
    }
    return false;
}
