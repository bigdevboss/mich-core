#ifndef VIRTIO_BLK_H
#define VIRTIO_BLK_H

#include "types.h"
#include "object.h"

#define VIRTIO_BLK_F_RO 5
#define VIRTIO_BLK_F_BLK_SIZE 6
#define VIRTIO_BLK_F_FLUSH 9

#define VIRTIO_BLK_T_IN 0
#define VIRTIO_BLK_T_OUT 1

#define VIRTIO_BLK_S_OK 0
#define VIRTIO_BLK_S_IOERR 1
#define VIRTIO_BLK_S_UNSUPP 2

struct kernel_object *virtio_blk_open(struct kernel_object *pci);

#endif
