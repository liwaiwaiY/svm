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
typedef struct StubSendQueue StubSendQueue;
typedef struct iovec iovec;

/*
 * Number of resp-processing worker threads in the virtio-remote internal
 * pool. These workers are unrelated to the device IOThreads: resp fds are
 * hashed onto them (vq_nr % VIRTIO_REMOTE_RESP_WORKERS), so several vqs can
 * share one worker. A worker handles one fd at a time; extra events are
 * skipped by the distributor and re-armed by the level-triggered epoll.
 */
#define VIRTIO_REMOTE_RESP_WORKERS 4

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
    /* stub: response send queue, drained by the socket iothread */
    StubSendQueue *send_q;

    /*
     * cmsvm: per-vq resp-processing state. These are the per-vq counterparts
     * of the old global hash tables; the distributor hands them to the resp
     * worker together with the vq, so each vq's entries are only ever touched
     * by that vq's owner threads. lock serializes the two cross-thread owners
     * of the same vq: the socket iothread (send side) and the resp worker
     * (response side).
     */
    GMutex lock;                  /* guards pending and zc->pending */
    GQueue pending;               /* local: in-flight elems, seq-ordered */
    void *recv;                   /* local: LocalRecvState of the resp stream */
    void *zc;                     /* local/stub: MSG_ZEROCOPY state (ZcFdState) */
} RemoteVQueueCtx;

/* -------------- Device States ------------- */

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

/* -------------- Aio Contexts ------------- */

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

/* -------------- Environments ------------- */

#define VIRTIO_LOCAL_ENV 0
#define VIRTIO_REMOTE_ENV 1

static int env_tag;

/*
* called in local qemu and remote stub
* to change value of env
*/
static inline void chenv(int new_env)
{
    if (new_env != VIRTIO_LOCAL_ENV && new_env != VIRTIO_REMOTE_ENV)
        return;
    env_tag = new_env;
}

/*
* called in local qemu and remote stub
* to check the environment
*/
static inline int check_env(int tar_env)
{
    return env_tag == tar_env;
}


/* -------------- Local QEMU Forwarding ------------- */


/* -------------- Remote Stub Forwarding ------------- */

/* -------------- Local QEMU Handlers ------------- */

/*
* called by local qemu in ioeventfd_impl
* this will be called only by vdev which originally use main loop
* vdev that originally use aio will use aio_attach in virtio.c
*/
int virtio_device_start_ioeventfd_impl_local(VirtIODevice *vdev, AioContext *ctx);

/*
* called by local qemu in aio iothread
* all events of host notifiers and socket fds will be sent to this
* this distributor needs to allocate worker and functions
*/
void local_distributor(EventNotifier *n);

/*
* called by local qemu in virtio_queue_notify_vq
* handle notifier kick
*/
void local_handle_output(VirtQueue *vq, RemoteVQueueCtx *ctx);

/*
* called by local qemu in local_set_remote (and aio handlers):
* handle a response arriving on a per-vq socket
*/
void local_response_handler(void *opaque);

/*
* called by local qemu, registered as the io_read handler of every resp fd:
* receive an fd event, pick the resp worker of the fd's vq and hand the
* response processing over to it. If the worker is already busy, the event is
* skipped (the level-triggered epoll re-arms it for the next round).
*/
void local_response_distributor(void *opaque);

/* -------------- Remote Stub Handlers ------------- */

/*
* called by virtio.c on the stub side (virtio_queue_aio_attach_host_notifier_no_poll):
* the stub has no guest, so a host-notifier kick has nothing to forward
*/
void remote_virtio_queue_host_notifier_read(EventNotifier *n);


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

#endif /* VIRTIO_REMOTE */
