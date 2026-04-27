#ifndef VIRTIO_REMOTE
#define VIRTIO_REMOTE

#include "hw/virtio/virtio.h"
#include <sys/uio.h>
#include <liburing.h>
#include <glib.h>

#define MAX_FD VIRTIO_QUEUE_MAX
#define SOCKET_RV int

typedef struct RemoteElem {
    unsigned int num;
    struct iovec* sg;
} RemoteElem;

// Struct to hold zero-copy data for cleanup
typedef struct {
    struct iovec *iov;
    int iov_count;
    char *msg_header;
    void *vdev;
} zc_data;

static int remote_virtio_device_start_ioeventfd_impl(VirtioDevice *vdev);
void remote_virtio_queue_host_notifier_read(EventNotifier *n);

#endif