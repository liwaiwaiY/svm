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
 * pool. These workers are unrelated to the device IOThreads. A process is
 * either the local side or the stub side (never both), and both sides use the
 * same two pools: a send pool (outbound) and a recv pool (inbound), each with
 * VIRTIO_REMOTE_WORKERS threads. Local: send pool drains kicks/retry-sends,
 * recv pool parses responses. Stub: send pool does the resp sendmsg, recv
 * pool parses requests and runs handle_output. A vq is hashed onto exactly
 * one worker per pool (vq_nr % VIRTIO_REMOTE_WORKERS), so one vq is handled
 * by two threads that run concurrently and are serialized on the vq by
 * vq_lock (local) or by vq-internal serialization (stub). Several vqs can
 * share one worker. A worker handles one event at a time; extra events are
 * skipped by the distributor and re-armed by the level-triggered epoll.
 */
#define VIRTIO_REMOTE_WORKERS 4

typedef struct RemoteVQueueCtx {
    int resp_fd;
    int vq_nr;
    unsigned int elem_index;
    /* local: kick absorbed by the distributor while the send worker was busy;
     * a redrain of this vq is owed (qatomic, consumed by worker_bh) */
    int kick_pending;
    /* stub: the elem the device currently holds (set by pop(), cleared by
     * its push()); the queue-empty decision is req_count, not this marker */
    void *elem;

    /* stub: requests parsed off the socket and waiting for the device to pop
     * them. Private to the handle worker (the parser enqueues and the device
     * pops on that same worker, so no locking is needed). in_handle is the
     * handle_output re-entry guard (qatomic): set before call_handle_output,
     * cleared by the device's push() on whichever thread it runs, so the IO of
     * one batch overlaps the parsing of the next without two handle_outputs
     * running at once. req_count lets a push (device thread) see at a glance
     * whether requests piled up while the handle worker was blocked. */
    GQueue *req_queue;
    int in_handle;
    int req_count;
    /* stub: push_cond wakes the handle worker blocked on a full in-flight
     * window (backpressure) and a push's drain/teardown wakeups. */
    GMutex push_lock;
    GCond push_cond;
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
     * (resp path). vq_lock serializes every virtqueue access (pop/push/notify);
     * inflight is the lock-free SPSC in-flight
     * window between the two workers (see virtio-remote.c). On the stub side
     * the socket iothread is a pure dispatcher: it hands a vq to its handle
     * worker (which parses the request stream and runs the device) or, for a
     * bare POLLERR (zc completion), to its send worker. The handle worker and
     * the send worker are joined by the same lock-free window (ctx->inflight).
     */
    GMutex vq_lock;               /* serializes vq access (pop/push/notify) */
    void *inflight;               /* the one SPSC in-flight window (Inflight):
                                     local (send worker -> recv worker) holds
                                     VirtQueueElement* + zc bookkeeping; stub
                                     (handle worker -> send worker) holds
                                     StubResp*. Which side owns it is decided
                                     by check_env() (see the stub window init) */
    void *recv;                   /* local: LocalRecvState; stub: StubRecvState */
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

int env_tag;

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

/*
* called in local_Set_remote or remote_set_local
* register necessary data structures
*/
void start_local_env(void);
void start_remote_env(void);


/*
* called by virtio.c / remote_accept_handler: init the per-vq ctx. On the
* local side vring_num is the vq's vring size (the in-flight window =
* pow2ceil(vring_num)). On the stub side only the mutexes/conds are
* initialized here (vring_num is passed as 0); the same window is set up
* later by remote_accept_handler (stub_ctx_init_windows), which also marks
* this TU as the stub side via chenv(VIRTIO_REMOTE_ENV).
*/
void remote_vq_ctx_init(RemoteVQueueCtx *ctx, unsigned int vring_num);

/*
* called at vq teardown (virtio_delete_queue / virtio_device_free_virtqueues /
* the fail rollbacks): releases the inflight window and the vq_lock. Must run
* after the vq's workers have stopped.
*/
void remote_vq_ctx_destroy(RemoteVQueueCtx *ctx);

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
* called by local qemu, registered as the io_read handler of every resp fd:
* receive an fd event, pick the resp worker of the fd's vq and hand the
* response processing over to it. If the worker is already busy, the event is
* skipped (the level-triggered epoll re-arms it for the next round).
*/
void local_response_distributor(void *opaque);

/* -------------- Local QEMU ThreadPool ------------- */

/*
* called by local qemu in local_set_remote after the per-vq ctx is up: tells
* the vq's send worker about it, so the worker can replay the kicks absorbed
* while it was busy (kick_pending) when it goes idle. Must be called during
* machine setup, before any kick can be delivered (the list is read
* lock-free by the worker).
*/
void local_register_vq(VirtQueue *vq);

/* -------------- Remote Stub Forwarding ------------- */
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

/* -------------- Remote Stub Handlers ------------- */

/*
* called by remote stub in socket iothread, registered in accept
* when an req elem arrives, this function is called 
*/
void stub_distributor(void *opaque);


/* -------------- Remote Stub ThreadPool ------------- */

/*
* called in remote stub at remote_accept_handler
* register vq to workers
*/
void stub_register_vq(VirtQueue *vq);

/*
* called in remote stub when conn_err
*/
void stub_teardown_vq(VirtQueue *vq);


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
