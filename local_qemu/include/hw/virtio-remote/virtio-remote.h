#ifndef VIRTIO_REMOTE
#define VIRTIO_REMOTE

#include "hw/virtio/virtio.h"
#include "hw/virtio/vhost.h"
#include "qemu/osdep.h"
#include <sys/uio.h>
#include <liburing.h>
#include <glib.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <string.h>
#include <stdlib.h>

typedef struct VirtQueue VirtQueue;
typedef struct VirtIODevice VirtIODevice;
typedef struct VirtQueueElement VirtQueueElement;
typedef struct iovec iovec;

typedef struct RemoteVQueueCtx {
    int resp_fd;
    int vq_nr;
    unsigned int elem_index;
    unsigned int out_num;
    unsigned int in_num;
    struct iovec *out_sg;
    struct iovec *in_sg;
    // VirtQueueElement elem;
    void *elem;
} RemoteVQueueCtx;

/*
* called by local qemu or remote stub
* check the vdev is a mosaic device
*/
bool is_mosaic(VirtIODevice *vdev);

/*
* called by local qemu in property setter ("remote-machine")
* local qemu coordinate with remote stub to create vq_nt ports
* on success, sockets[] holds vq_nt connected fds
*/
int local_connect_socket(const char *ip_port, int vq_nt, int *sockets, Error **errp);

/*
* called by local qemu in peoperty setter ("remote-machine")
* local qemu connect sockets for each vq
*/
bool local_connect_vq(int socket, Error **errp);

/*
* called by local qemu in aio iothread, registerd in property setter ("remote-machine")
* local qemu handle responses with opaque as VirtIODevice *
*/
void local_response_handler(void *opaque);

/*
* called by local qemu in ioevent, registed in ioeventfd_impl
* handle msg in the vq, and send it to remote
*/
int local_virtio_queue_host_notifier_read(EventNotifier *n);

/*
* called by remote stub in property setter ("remote-stub")
* listen on ip@port and accept one connection from local qemu,
* returns the connected fd or -1 on error (errp set)
*/
int remote_accept(const char *ip_port, Error **errp);

typedef struct RemoteAccept {
    int listen_fd;
    AioContext *aio_ctx;
    VirtIODevice *vdev;
} RemoteAccept;

void remote_accept_handler(void *opaque);


#endif /* VIRTIO_REMOTE */
