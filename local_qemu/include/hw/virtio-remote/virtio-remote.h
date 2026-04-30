#ifndef VIRTIO_REMOTE
#define VIRTIO_REMOTE

#include "hw/virtio/virtio.h"
#include "qemu/qemu.h"
#include "qemu/iov.h"
#include <sys/uio.h>
#include <liburing.h>
#include <glib.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <string.h>
#include <stdlib.h>

#define MAX_FD VIRTIO_QUEUE_MAX
#define SOCKET_RV int

typedef struct {
    int resp_fd;
    int vq_nr;
    unsigned int elem_index;
    unsigned int out_len;
    unsigned int in_len;
    uint8_t *out_buf;
    uint8_t *in_buf;
    struct iovec out_sg[1];
    struct iovec in_sg[1];
    VirtQueueElement elem;
} RemoteVQueueCtx;

// Struct to hold zero-copy data for cleanup
typedef struct {
    struct iovec *iov;
    int iov_count;
    char *msg_header;
    void *vdev;
} zc_data;

int remote_virtio_device_start_ioeventfd_impl(VirtioDevice *vdev);
void remote_virtio_device_stop_ioeventfd_impl(VirtIODevice *vdev);
void remote_virtio_queue_host_notifier_read(EventNotifier *n);
void init_remote_virtio_device_sockets(VirtIODevice *vdev, const char *ip_port, Error **errp);
void init_remote_stub_socket(VirtIODevice *vdev, const char *ip_port,Error **errp);
int remote_uring_init(void);
void remote_virtio_pci_notify(DeviceState *dev, uint16_t vector);

void *remote_stub_virtqueue_pop(VirtQueue *vq, size_t sz);
void remote_stub_virtqueue_push(VirtQueue *vq, const VirtQueueElement *elem,
                                unsigned int len);
bool remote_virtio_notify_skip(VirtIODevice *vdev);

#endif