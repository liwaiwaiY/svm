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
    /* local: VirtQueueElement deferred for a send retry (socket was full) */
    void *pending_elem;
    /* stub: StubResp deferred for a response send retry (socket was full) */
    void *pending_resp;
} RemoteVQueueCtx;

/*
* called in local qemu in local_set_remote
* register a vdev is mosaic-based
*/
void register_mosaic(VirtIODevice *vdev);

/*
* called by local qemu or remote stub
* check the vdev is a mosaic device
*/
bool is_mosaic(VirtIODevice *vdev);

/*
* called in local qemu and remote stub in property setter
* register aio context without modifyling vdev structure
*/
void register_aio_ctx(VirtIODevice *vdev, AioContext *aio_ctx);

/*
* called in local qemu
* search registered aio_ctx for vdev
*/
AioContext * local_search_aio_ctx(VirtIODevice *vdev);

/*
* called by local qemu in property setter ("remote-machine")
* negotiate per-vq ports with the remote stub over a control connection.
* Only negotiation here: sockets[] gets vq_nt sockets bound to free local
* source ports (not connected yet), dst[] gets the stub-side destination
* addresses. The caller connects each socket with local_connect_vq().
* On success the control connection fd is returned (or -1 on error).
*/
int local_connect_socket(const char *ip_port, int vq_nt, int *sockets,
                         struct sockaddr_in *dst, Error **errp);

/*
* called by local qemu in peoperty setter ("remote-machine")
* local qemu connect an already-created vq socket (pinned source port)
* to the stub-side address in addr
*/
bool local_connect_vq(int socket, const struct sockaddr_in *addr, Error **errp);

/*
* called by local qemu in ioeventfd_impl
* this will be called only by vdev which originally use main loop
* vdev that originally use aio will use aio_attach in virtio.c
*/
int virtio_device_start_ioeventfd_impl_local(VirtIODevice *vdev, AioContext *ctx);

/*
* called by local qemu in ioevent, registed in ioeventfd_impl
* handle msg in the vq, and send it to remote
*/
void local_host_notifier_read(EventNotifier *n);

/*
* called by local qemu in aio iothread, registerd in property setter ("remote-machine")
* local qemu handle responses with opaque as VirtQueue *
*/
void local_response_handler(void *opaque);

/*
* used in remote stub in accept handler
* to pass essential params
*/
typedef struct RemoteAccept {
    int listen_fd;
    AioContext *aio_ctx;
    VirtIODevice *vdev;
} RemoteAccept;

/*
* called by remote stub in property setter ("remote-stub")
* listen on ip@port and accept one connection from local qemu,
* returns the connected fd or -1 on error (errp set)
*/
int remote_accept(const char *ip_port, Error **errp);

/*
* called by remote stub in aio iothread, registered in property setter
* main function of server, handle accept from local qemu
* coordinate sockets for each vq
*/
void remote_accept_handler(void *opaque);

/*
* called by virtio.c on the remote stub side (vq->remote_ctx set):
* reconstruct a VirtQueueElement from the request buffered in the vq's
* RemoteVQueueCtx, exactly one element per received request
*/
void *remote_stub_virtqueue_pop(VirtQueue *vq, size_t sz);

/*
* called by virtio.c on the remote stub side: send the response back to
* local qemu and release the request buffers
*/
void remote_stub_virtqueue_push(VirtQueue *vq, const VirtQueueElement *elem,
                                unsigned int len);

/*
* called by virtio.c on the remote stub side: 1 if the buffered request of
* this vq is still in flight (popped but not pushed)
*/
bool remote_virtio_queue_empty(void *opaque);

/*
* called by virtio.c: on the remote stub side the config interrupt has no
* guest to deliver to, so it must be skipped
*/
bool remote_virtio_notify_skip(VirtIODevice *vdev);

/*
* called by virtio.c: true if this process is the remote stub for vdev
* (any active vq carries a remote ctx and vdev is not a mosaic device)
*/
bool check_virtio_device_remote(VirtIODevice *vdev);

/*
* called by virtio.c: true if vdev's responses are processed on an iothread
*/
bool check_origin_qemu_in_iothread(VirtIODevice *vdev);

/*
* legacy property "remote-id": keep the device id
*/
void remote_register_id(Object *obj, const char *id, Error **errp);

/*
* legacy stub-side host notifier hooks: the stub has no guest to kick its
* vqs, so these are inert
*/
void remote_virtio_register_aio(VirtIODevice *vdev);
void remote_virtio_queue_host_notifier_read(EventNotifier *n);
void remote_virtio_queue_host_notifier_aio_poll_ready(EventNotifier *n);
void remote_virtio_device_stop_ioeventfd_impl(VirtIODevice *vdev);


#endif /* VIRTIO_REMOTE */
