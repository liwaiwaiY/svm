// copy from virtio.c
#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qapi/qapi-commands-virtio.h"
#include "trace.h"
#include "qemu/defer-call.h"
#include "qemu/error-report.h"
#include "qemu/log.h"
#include "qemu/main-loop.h"
#include "qemu/module.h"
#include "qemu/target-info.h"
#include "qom/object_interfaces.h"
#include "hw/core/cpu.h"
#include "hw/virtio/virtio.h"
#include "hw/virtio/vhost.h"
#include "migration/qemu-file-types.h"
#include "qemu/atomic.h"
#include "hw/virtio/virtio-bus.h"
#include "hw/core/qdev-properties.h"
#include "hw/virtio/virtio-access.h"
#include "system/dma.h"
#include "system/iothread.h"
#include "system/memory.h"
#include "system/runstate.h"
#include "virtio-qmp.h"

#include "standard-headers/linux/virtio_ids.h"
#include "standard-headers/linux/vhost_types.h"
#include "standard-headers/linux/virtio_blk.h"
#include "standard-headers/linux/virtio_console.h"
#include "standard-headers/linux/virtio_gpu.h"
#include "standard-headers/linux/virtio_net.h"
#include "standard-headers/linux/virtio_scsi.h"
#include "standard-headers/linux/virtio_i2c.h"
#include "standard-headers/linux/virtio_balloon.h"
#include "standard-headers/linux/virtio_iommu.h"
#include "standard-headers/linux/virtio_mem.h"
#include "standard-headers/linux/virtio_vsock.h"
#include "standard-headers/linux/virtio_spi.h"

// cmsvm
#include "hw/virtio-remote/virtio-remote.h"
#include <sys/socket.h>
#include <sys/uio.h>


#define MAX_FD VIRTIO_QUEUE_MAX
// <K:name, V:socket*>
GHashTable *gsi_stubs;
// <K:socket, V:vq_nr>
GHashTable *gst_reverse;

void route_to_remote(VirtIODevice *vdev, VirtQueue *vq, int stub)
{
    // TODO
    const struct msghdr *send_buf = vq_to_msghdr(vq);
    sendmsg(stub, send_buf, NULL);
}

void complete_consumption(VirtIODevice *vdev, int stubs[])
{
    // TODO: learn main-loop.c
    struct pollfd pfd[MAX_FD];
    int i, ret;

    for (i = 0; i < MAX_FD; i++) {
        if (!virtio_queue_get_num(vdev, i)) {
            continue;
        }
        pfd[i].fd = stubs[i];
        pdf[i].events = POLLIN;
        pfd[i].revents = 0;
    }

    ret = poll(pfd, );
}

static void virtio_queue_notify_vq_cmsvm(VirtQueue *vq)
{
    if (vq->vring.desc) {
        VirtIODevice *vdev = vq->vdev;
        int remote_stubs[] = g_hash_table_lookup(gsi_rvdevs, vdev->name);

        if (unlikely(vdev->broken)) {
            return;
        }

        trace_virtio_queue_notify(vdev, vq - vdev->vq, vq);
        // rerouter
        route_to_remote(vdev, vq, remote_stub[vq - vdev->vq]);
        // wait resp
        complete_consumption(vdev, remote_stubs);

        if (unlikely(vdev->start_on_kick)) {
            virtio_set_started(vdev, true);
        }
        free(stub_key);
    }
}

void virtio_queue_host_notifier_read_cmsvm(EventNotifier *n)
{
    VirtQueue *vq = container_of(n, VirtQueue, host_notifier);
    if (event_notifier_test_and_clear(n)) {
        virtio_queue_notify_vq_cmsvm(vq);
    }
}

static int virtio_device_start_ioeventfd_impl_cmsvm(VirtioDevice *vdev)
{
    VirtioBusState *qbus = VIRTIO_BUS(qdev_get_parent_bus(DEVICE(vdev)));
    int i, n, r, err;

    memory_region_transaction_begin();
    for (n = 0; n < VIRTIO_QUEUE_MAX; n++) {
        VirtQueue *vq = &vdev->vq[n];
        if (!virtio_queue_get_num(vdev, n)) {
            continue;
        }
        r = virtio_bus_set_host_notifier(qbus, n, true);
        if (r < 0) {
            err = r;
            goto assign_error;
        }

        event_notifier_set_handler(&vq->host_notifier,
                                   virtio_queue_host_notifier_read_cmsvm);
    }

    /* TODO: decide to kick or not
    for (n = 0; n < VIRTIO_QUEUE_MAX; n++) {
        VirtQueue *vq = &vdev->vq[n];
        if (!vq->vring.num) {
            continue;
        }
        event_notifier_set(&vq->host_notifier);
    }
    */

    memory_region_transaction_commit();
    return 0;

assign_error:
    i = n; /* save n for a second iteration after transaction is committed. */
    while (--n >= 0) {
        VirtQueue *vq = &vdev->vq[n];
        if (!virtio_queue_get_num(vdev, n)) {
            continue;
        }

        event_notifier_set_handler(&vq->host_notifier, NULL);
        r = virtio_bus_set_host_notifier(qbus, n, false);
        assert(r >= 0);
    }
    /*
     * The transaction expects the ioeventfds to be open when it
     * commits. Do it now, before the cleanup loop.
     */
    memory_region_transaction_commit();

    while (--i >= 0) {
        if (!virtio_queue_get_num(vdev, i)) {
            continue;
        }
        virtio_bus_cleanup_host_notifier(qbus, i);
    }
    return err;
}