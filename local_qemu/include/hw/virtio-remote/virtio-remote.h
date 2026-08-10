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

/*
 * Number of per-vq processing worker threads in the virtio-remote internal
 * pool. These workers are unrelated to the device IOThreads: there are two
 * pools (a send pool and a recv pool), each with VIRTIO_REMOTE_WORKERS
 * threads. A vq's host-notifier kick is hashed onto its send worker and every
 * resp is hashed onto its recv worker (vq_nr % VIRTIO_REMOTE_WORKERS), so one
 * vq is handled by two threads that run concurrently and are serialized on the
 * vq by vq_lock. Several vqs can share one worker. A worker handles one event
 * at a time; extra events are skipped by the distributor and re-armed by the
 * level-triggered epoll.
 */
#define VIRTIO_REMOTE_WORKERS 4

typedef struct RemoteVQueueCtx {
    int resp_fd;
    int vq_nr;
    unsigned int elem_index;
    /* local: VirtQueueElement deferred for a send retry (socket was full) */
    void *pending_elem;
    /* local: kick absorbed by the distributor while the send worker was busy;
     * a redrain of this vq is owed (qatomic, consumed by worker_bh) */
    int kick_pending;
    /* local: the elem currently popped but not yet pushed (empty check) */
    void *elem;

    /* stub: request window (distributor -> handle worker) and response window
     * (handle worker -> send worker), both Inflight sized pow2ceil(vring.num).
     * The Inflight head/tail are the seq allocator/consumer directly. */
    void *req_win;
    void *resp_win;
    /* stub: per-request context of the elem currently being handled. vq-internal
     * serialization guarantees at most one active elem at a time, so this single
     * slot has no race (handle worker writes, pop reads). */
    void *active_req;
    /* stub: waits for the device's push() of the active elem (async
     * handle_output). push_done is qatomic; push_cond wakes the handle worker
     * and a push blocked on a full resp_win (backpressure). */
    GMutex push_lock;
    GCond push_cond;
    bool push_done;
    /* stub: the connection is gone; workers must exit their current task and
     * never touch the windows again (qatomic) */
    bool dead;
    /* stub: the vq's handle/send worker is inside a task for this vq (qatomic).
     * conn_err waits for both to clear before tearing down shared state. */
    bool handle_busy;
    bool send_busy;
    /* stub: a send worker writable handler is registered for this vq (the
     * socket is full). Owned by the vq's send worker; cleared on detach. */
    bool send_writable;

    /*
     * cmsvm: per-vq processing state. On the local side a vq is owned by two
     * concurrent workers - the send worker (kick path) and the recv worker
     * (resp path). vq_lock serializes every virtqueue access (pop/push/notify)
     * and the shared pending_elem; inflight is the lock-free SPSC in-flight
     * window between the two workers (see virtio-remote.c). On the stub side
     * the distributor (socket iothread), the handle worker and the send worker
     * form a three-stage pipeline joined by the req_win/resp_win SPSC windows.
     */
    GMutex vq_lock;               /* serializes vq access and pending_elem */
    void *inflight;               /* local: SPSC in-flight window (Inflight) */
    void *recv;                   /* local: LocalRecvState of the resp stream */
    void *zc;                     /* local/stub: MSG_ZEROCOPY state (ZcFdState) */
} RemoteVQueueCtx;

/*
* called by virtio.c / remote_accept_handler: init the per-vq ctx. On the
* local side vring_num is the vq's vring size (inflight window =
* pow2ceil(vring_num)). On the stub side the distributor/send workers and the
* req_win/resp_win SPSC windows are set up later by remote_accept_handler
* (stub_ctx_init_windows), so vring_num is passed as 0 here and only the
* mutexes/conds are initialized.
*/
void remote_vq_ctx_init(RemoteVQueueCtx *ctx, unsigned int vring_num);

/*
* called at vq teardown (virtio_delete_queue / virtio_device_free_virtqueues /
* the fail rollbacks): releases the parked pending_elem, the inflight window
* and the vq_lock. Must run after the vq's workers have stopped.
*/
void remote_vq_ctx_destroy(RemoteVQueueCtx *ctx);

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

/* -------------- Remote Stub Forwarding ------------- */

/* -------------- Local QEMU Handlers ------------- */

/*
* called by local qemu in ioeventfd_impl
* this will be called only by vdev which originally use main loop
* vdev that originally use aio will use aio_attach in virtio.c
*/
int virtio_device_start_ioeventfd_impl_local(VirtIODevice *vdev, AioContext *ctx);

/*
* called by local qemu, registered as the io_read handler of every host
* notifier (mosaic devices): a kick is dispatched to the worker that owns the
* vq (the same worker as its resp socket), never processed on the iothread
*/
void local_notifier_distributor(EventNotifier *n);

/*
* called by local qemu in local_set_remote after the per-vq ctx is up: tells
* the vq's send worker about it, so the worker can replay the kicks absorbed
* while it was busy (kick_pending) when it goes idle. Must be called during
* machine setup, before any kick can be delivered (the list is read
* lock-free by the worker).
*/
void remote_worker_register_vq(VirtQueue *vq);

/*
* called by local qemu, registered as the io_read handler of every resp fd:
* receive an fd event, pick the resp worker of the fd's vq and hand the
* response processing over to it. If the worker is already busy, the event is
* skipped (the level-triggered epoll re-arms it for the next round).
*/
void local_response_distributor(void *opaque);

/* -------------- Remote Stub Handlers ------------- */


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
