#ifndef VIRTIO_H
#define VIRTIO_H
/* virtio PCI transport (modern capabilities or legacy I/O ports), shared by
 * the virtio disk drivers (kernel/virtio.c) and virtio-net (net/virtionet.c). */
#include "kernel.h"

typedef struct { uint64_t addr; uint32_t len; uint16_t flags, next; } __attribute__((packed)) vq_desc_t;
#define VQ_NEXT 1
#define VQ_WRITE 2

typedef struct {                                   /* one split virtqueue */
    uint16_t index, size, last_used, notify_off;
    vq_desc_t *desc;
    volatile uint16_t *avail;                      /* flags, idx, ring[] */
    volatile uint8_t *used;                        /* flags, idx, ring[] of {id, len} */
} virtq_t;

typedef struct {
    bool modern;
    uint16_t io;                                   /* legacy BAR0 */
    volatile uint8_t *common, *notify, *devcfg;    /* modern */
    uint32_t notify_mult;
    virtq_t q;                                     /* the queue the disk drivers use */
} vdev_t;

bool virtio_transport(pci_dev_t *pd, vdev_t *v);
bool virtio_negotiate(vdev_t *v, uint32_t want, uint32_t *got);   /* low 32 feature bits; VERSION_1 on modern */
bool virtio_queue_setup(vdev_t *v, virtq_t *q, uint16_t index, uint16_t max);
void virtio_notify(vdev_t *v, virtq_t *q);
void virtio_driver_ok(vdev_t *v);
uint8_t virtio_get_status(vdev_t *v);
void virtio_set_status(vdev_t *v, uint8_t s);
uint8_t virtio_cfg8(vdev_t *v, int off);
uint16_t virtio_cfg16(vdev_t *v, int off);
uint32_t virtio_cfg32(vdev_t *v, int off);
#endif
