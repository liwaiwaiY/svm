#ifndef VIRTIO_REMOTE
#define VIRTIO_REMOTE

#include "hw/virtio/virtio.h"

static int virtio_device_start_ioeventfd_impl_cmsvm(VirtioDevice *vdev);
void virtio_queue_host_notifier_read_cmsvm(EventNotifier *n);
#endif