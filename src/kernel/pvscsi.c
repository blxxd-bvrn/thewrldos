/* VMware Paravirtual SCSI (PVSCSI) host adapter, also emulated by QEMU.
 * One request ring page and one completion ring page, polled. */
#include "kernel.h"
#include "disk.h"
#include "devmgr.h"

#define REG_COMMAND 0x0
#define REG_COMMAND_DATA 0x4
#define REG_COMMAND_STATUS 0x8
#define REG_INTR_MASK 0x2010
#define REG_KICK_NON_RW 0x3014
#define REG_KICK_RW 0x4018
#define CMD_ADAPTER_RESET 1
#define CMD_SETUP_RINGS 3
#define FLAG_DIR_NONE (1u << 2)
#define FLAG_DIR_TOHOST (1u << 3)
#define FLAG_DIR_TODEVICE (1u << 4)

typedef struct {
    uint32_t reqProdIdx, reqConsIdx, reqNumEntriesLog2;
    uint32_t cmpProdIdx, cmpConsIdx, cmpNumEntriesLog2;
    uint8_t pad[104];
    uint32_t msgProdIdx, msgConsIdx, msgNumEntriesLog2;
} __attribute__((packed)) rings_state_t;

typedef struct {
    uint64_t context, dataAddr, dataLen, senseAddr;
    uint32_t senseLen, flags;
    uint8_t cdb[16];
    uint8_t cdbLen;
    uint8_t lun[8];
    uint8_t tag, bus, target, vcpuHint;
    uint8_t unused[59];
} __attribute__((packed)) req_desc_t;               /* 128 bytes */

typedef struct {
    uint64_t context, dataLen;
    uint32_t senseLen;
    uint16_t hostStatus, scsiStatus;
    uint32_t pad[2];
} __attribute__((packed)) cmp_desc_t;               /* 32 bytes */

typedef struct {
    volatile uint8_t *regs;
    volatile rings_state_t *state;
    req_desc_t *req;
    cmp_desc_t *cmp;
    uint8_t *sense, *buf;
    uint64_t ctx;
    scsi_host_t host;
} pvscsi_t;

static inline void wr(pvscsi_t *p, int off, uint32_t v) { *(volatile uint32_t *)(p->regs + off) = v; }
static inline uint32_t rd(pvscsi_t *p, int off) { return *(volatile uint32_t *)(p->regs + off); }

static int pv_exec(scsi_host_t *h, int target, int lun, const uint8_t *cdb, int cdblen, void *buf, uint32_t len, int dir) {
    pvscsi_t *p = h->priv;
    if (len > 65536 || cdblen > 16) return -1;
    uint32_t mask = (1u << p->state->reqNumEntriesLog2) - 1;
    req_desc_t *r = &p->req[p->state->reqProdIdx & mask];
    memset(r, 0, sizeof *r);
    r->context = ++p->ctx;
    r->dataAddr = dir ? dma_addr(p->buf) : 0;
    r->dataLen = dir ? len : 0;
    r->senseAddr = dma_addr(p->sense); r->senseLen = 32;
    r->flags = dir == 1 ? FLAG_DIR_TOHOST : dir == 2 ? FLAG_DIR_TODEVICE : FLAG_DIR_NONE;
    memcpy(r->cdb, cdb, cdblen); r->cdbLen = cdblen;
    r->lun[1] = lun; r->target = target; r->bus = 0;
    if (dir == 2) memcpy(p->buf, buf, len);
    __asm__ volatile("" ::: "memory");
    p->state->reqProdIdx++;
    bool rwcmd = cdb[0] == 0x28 || cdb[0] == 0x2A || cdb[0] == 0x88 || cdb[0] == 0x8A;
    wr(p, rwcmd ? REG_KICK_RW : REG_KICK_NON_RW, 0);
    uint32_t t0 = timer_ticks();
    for (;;) {
        while (p->state->cmpConsIdx == p->state->cmpProdIdx)
            if (timer_ticks() - t0 > 10000) { klog("pvscsi: command %02x to target %d timed out", cdb[0], target); return -1; }
        uint32_t cmask = (1u << p->state->cmpNumEntriesLog2) - 1;
        cmp_desc_t c = p->cmp[p->state->cmpConsIdx & cmask];
        p->state->cmpConsIdx++;
        if (c.context != p->ctx) continue;
        if (c.hostStatus == 0x11 || c.hostStatus == 0x21) return -1;      /* selection timeout: no device */
        if (c.hostStatus && c.hostStatus != 0x0C && c.hostStatus != 0x12 && c.hostStatus != 0x0A && c.hostStatus != 0x0B) return -1;
        if (dir == 1 && !c.scsiStatus) memcpy(buf, p->buf, len);
        return c.scsiStatus;
    }
}

static int nhost;
static bool pvscsi_match(const pci_dev_t *p) { return p->vendor == 0x15AD && p->device == 0x07C0; }
DRIVER_PCI(drv_pvscsi, "pvscsi", "VMware Paravirtual SCSI controller", DC_STORAGE, pvscsi_match);
static void pv_init(pci_dev_t *pd) {
    uintptr_t base = pci_bar(pd, 0, NULL);
    if (!base) { klog("pvscsi: BAR0 unusable"); return; }
    pci_enable(pd, false, true);
    pvscsi_t *p = kzalloc(sizeof *p);
    p->regs = (volatile uint8_t *)base;
    wr(p, REG_COMMAND, CMD_ADAPTER_RESET);
    wr(p, REG_INTR_MASK, 0);                               /* we poll */
    p->state = kmalloc_aligned(4096, 4096); memset((void *)p->state, 0, 4096);
    p->req = kmalloc_aligned(4096, 4096); memset(p->req, 0, 4096);
    p->cmp = kmalloc_aligned(4096, 4096); memset(p->cmp, 0, 4096);
    p->sense = kmalloc_aligned(64, 64);
    p->buf = kmalloc_aligned(65536, 4096);
    /* setup rings: reqRingNumPages, cmpRingNumPages, ringsStatePPN, reqRingPPNs[32], cmpRingPPNs[32] */
    uint32_t desc[4 + 2 + 64 + 64] = { 0 };
    desc[0] = 1; desc[1] = 1;
    uint64_t sp = dma_addr((void *)p->state) >> 12, rp = dma_addr(p->req) >> 12, cp = dma_addr(p->cmp) >> 12;
    desc[2] = (uint32_t)sp; desc[3] = (uint32_t)(sp >> 32);
    desc[4] = (uint32_t)rp; desc[5] = (uint32_t)(rp >> 32);
    desc[4 + 64] = (uint32_t)cp; desc[5 + 64] = (uint32_t)(cp >> 32);
    wr(p, REG_COMMAND, CMD_SETUP_RINGS);
    for (unsigned i = 0; i < 4 + 64 + 64; i++) wr(p, REG_COMMAND_DATA, desc[i]);
    if (rd(p, REG_COMMAND_STATUS) != 0 || !p->state->reqNumEntriesLog2) { klog("pvscsi: ring setup failed"); return; }
    p->host.exec = pv_exec; p->host.priv = p;
    p->host.driver = "pvscsi";
    p->host.busno = nhost++;
    p->host.max_target = 63;
    p->host.max_lun = 7;
    p->host.max_xfer = 65536;
    klog("pvscsi: VMware PVSCSI at %x, %u request slots", (uint32_t)base, 1u << p->state->reqNumEntriesLog2);
    dev_claim(pd, &drv_pvscsi, "VMware PVSCSI controller");
    scsi_scan(&p->host);
}

void pvscsi_probe(void) {
    pci_dev_t *devs; int n = pci_list(&devs);
    for (int i = 0; i < n; i++)
        if (pvscsi_match(&devs[i])) pv_init(&devs[i]);
}
