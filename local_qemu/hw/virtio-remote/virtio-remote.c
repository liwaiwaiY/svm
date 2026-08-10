// copy from virtio.c
#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qapi/qapi-commands-virtio.h"
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
#include "qemu/aio.h"
#include "qemu/host-utils.h"
#include "qemu/thread.h"
#include "qemu/rcu.h"

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

#include "hw/virtio-remote/virtio-remote.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <linux/errqueue.h>
#include <stdbool.h>

/* -------------- Device States ------------- */

static GHashTable *mosaic;

void register_mosaic(VirtIODevice *vdev)
{
    if (!mosaic)
        mosaic = g_hash_table_new(g_direct_hash, g_direct_equal);
    g_hash_table_insert(mosaic, vdev, GINT_TO_POINTER(1));
}

bool is_mosaic(VirtIODevice *vdev)
{
    return vdev && mosaic &&
           g_hash_table_lookup(mosaic, vdev);
}

/* -------------- Aio Contexts ------------- */

static GHashTable *aio_ctxs;

void register_aio_ctx(VirtIODevice *vdev, AioContext *aio_ctx)
{
    if (!aio_ctxs)
        aio_ctxs = g_hash_table_new(g_direct_hash, g_direct_equal);
    g_hash_table_insert(aio_ctxs, vdev, aio_ctx);
}

AioContext * local_search_aio_ctx(VirtIODevice *vdev)
{
    return aio_ctxs ? g_hash_table_lookup(aio_ctxs, vdev) : NULL;
}

static AioContext *vq_get_aio_ctx(VirtQueue *vq)
{
    return local_search_aio_ctx(virtqueue_get_vdev(vq));
}

/* -------------- Local QEMU Pipelining ------------- */

/*
 * Lock-free SPSC in-flight window types. The zc section below is laid out
 * before the functions that own the window ("Local QEMU Pipelining"), so the
 * structs are defined here and the function prototypes declared here too.
 */
typedef struct ZcPending {
    uint32_t serial;          /* kernel zc serial of this send */
    /* buffers the network stack may still reference (both sides) */
    void **bufs;
    unsigned int n_bufs;
    /* local side: elem whose used-ring push must wait for the completion */
    VirtQueueElement *elem;
    VirtQueue *vq;
    unsigned int seq;
    unsigned int push_len;    /* resp len; valid once len_known */
    bool len_known;
} ZcPending;

typedef struct InflightSlot {
    /* NULL = slot free. Local side: VirtQueueElement *; stub side: StubReq *
     * (req_win) or StubResp * (resp_win). */
    void *elem;
    unsigned int seq;
    bool is_zc;               /* sent with MSG_ZEROCOPY */
    ZcPending zc;             /* zc bookkeeping, copied in on publish */
} InflightSlot;

typedef struct Inflight {
    uint32_t size;            /* power of 2 >= vring.num */
    uint32_t mask;
    InflightSlot *slots;
    uint32_t head;            /* consumer-owned, advanced past cleared slots */
    uint32_t tail;            /* producer-owned */
} Inflight;

/*
 * Lock-free SPSC in-flight window.
 *
 * Local side: producer = the vq's send worker, consumer = the vq's recv
 * worker. Slots are indexed by seq & mask with size = pow2ceil(vring.num);
 * in-flight elems (popped but not pushed) are bounded by vring.num - 1
 * (virtqueue_pop refuses to pop once vq->inuse >= vring.num), so a slot is
 * never overwritten before its response is consumed. The producer writes a
 * slot fully and publishes it with qatomic_store_release(&tail); the consumer
 * snapshots [head, tail) once per task with acquire loads, clears slots as
 * responses complete and advances head past contiguous free slots.
 *
 * Stub side: two windows per vq - req_win (distributor -> handle worker) and
 * resp_win (handle worker's push -> send worker), both sized pow2ceil
 * (vring.num). head/tail are used directly as the seq allocator/consumer:
 * the producer publishes at its own tail, the consumer clears the head slot.
 *
 * The per-vq state is carried by RemoteVQueueCtx, so the distributor hands it
 * to the workers without any cross-vq table races.
 */

/* slot of seq (seq and seq - size collide only when the window is full) */
static inline InflightSlot *inflight_slot(Inflight *inf, unsigned int seq)
{
    return &inf->slots[seq & inf->mask];
}

/* look up an in-flight elem by seq (recv/handle/send worker, lock-free).
 * inf may be NULL (a ctx on the other side that never allocates the window). */
static void *inflight_lookup(Inflight *inf, unsigned int seq)
{
    InflightSlot *slot;

    if (!inf) {
        return NULL;
    }
    /* acquire on tail: makes the producer's slot writes visible */
    (void)qatomic_load_acquire(&inf->tail);
    slot = inflight_slot(inf, seq);
    return (slot->seq == seq) ? slot->elem : NULL;
}

/* publish an elem into the window (producer; caller owns the slot).
 * zc == NULL: plain send, no zc bookkeeping (slot->is_zc = false). */
static void inflight_publish(Inflight *inf, unsigned int seq, void *elem,
                             ZcPending *zc)
{
    InflightSlot *slot;

    assert(inf && seq - qatomic_load_acquire(&inf->head) < inf->size);
    slot = inflight_slot(inf, seq);
    slot->elem = elem;
    slot->seq = seq;
    slot->is_zc = zc != NULL;
    if (zc) {
        slot->zc = *zc;
    }
    qatomic_store_release(&inf->tail, seq + 1);
}

/* free the bufs kept alive for a zc completion */
static void inflight_free_bufs(InflightSlot *slot)
{
    if (slot->zc.bufs) {
        for (unsigned int i = 0; i < slot->zc.n_bufs; i++) {
            if (slot->zc.bufs[i]) {
                free(slot->zc.bufs[i]);
            }
        }
        g_free(slot->zc.bufs);
        slot->zc.bufs = NULL;
    }
}

/* clear a consumed slot and advance head past the contiguous free run */
static void inflight_clear(Inflight *inf, unsigned int seq)
{
    InflightSlot *slot = inflight_slot(inf, seq);
    uint32_t head, tail;

    if (!slot->elem) {
        return; /* already cleared by inflight_reset */
    }
    inflight_free_bufs(slot);
    slot->elem = NULL;
    slot->is_zc = false;

    tail = qatomic_load_acquire(&inf->tail);
    head = qatomic_load_acquire(&inf->head);
    while (head < tail && inflight_slot(inf, head)->elem == NULL) {
        head++;
    }
    qatomic_store_release(&inf->head, head);
}

/* teardown: release every still-in-flight elem/buf and reset the window.
 * caller owns the window (serialized against publish/clear). */
static void inflight_reset(Inflight *inf)
{
    uint32_t head, tail;

    if (!inf) {
        return;
    }
    head = qatomic_load_acquire(&inf->head);
    tail = qatomic_load_acquire(&inf->tail);
    while (head < tail) {
        InflightSlot *slot = inflight_slot(inf, head);
        inflight_free_bufs(slot);
        if (slot->elem) {
            g_free(slot->elem);
            slot->elem = NULL;
        }
        slot->is_zc = false;
        head++;
    }
    qatomic_store_release(&inf->head, tail);
}

/* -------------- Zero Copy ------------- */

/* stub-side per-connection incremental request receive state, keyed by fd */
static GHashTable *stub_recv_states;

/*
 * MSG_ZEROCOPY support (send side only).
 *
 * With SO_ZEROCOPY + MSG_ZEROCOPY the NIC DMA's the payload straight from
 * the (pinned) user pages, skipping the copy into the kernel skb - i.e.
 * the NIC reads directly from CPU memory. The kernel is free to fall back
 * to copying for segments it cannot pin (unaligned / partial pages); in
 * that case the send still succeeds and a completion is still queued, just
 * with SO_EE_CODE_ZEROCOPY_COPIED set. A completion is guaranteed for
 * every successful (ret > 0) zc send, and the sent buffers are referenced
 * by the network stack until that completion arrives, so we must not free
 * them (or let the guest reuse them) beforehand.
 *
 * The receive side cannot be zero-copy'd on TCP: the NIC DMA's into kernel
 * pages and recv() copies them into the user buffer. Only the send side
 * gets the "NIC <-> user memory" zero-copy.
 */
#define ZC_SEND_MIN (4 * 1024) /* only try zc for payloads >= this */

typedef struct ZcFdState {
    bool enabled;             /* SO_ZEROCOPY accepted on this socket */
    uint32_t serial;          /* kernel serial expected for the next zc send */
    GSList *pending;          /* ZcPending, matched by serial */
} ZcFdState;

/*
 * Enable MSG_ZEROCOPY on fd and return the per-vq zc state. Not registered in
 * any global table: the state is owned by the vq's RemoteVQueueCtx. On the
 * local side it is created/enabled by the send worker and released by the recv
 * worker's teardown, both under vq_lock; on the stub side it is owned by the
 * single socket iothread.
 */
static ZcFdState *zc_enable(int fd)
{
    ZcFdState *st = g_new0(ZcFdState, 1);
    int one = 1;
    st->enabled = setsockopt(fd, SOL_SOCKET, SO_ZEROCOPY, &one, sizeof(one)) == 0;
    return st;
}

/*
 * local: complete a zc slot whose kernel completion has arrived. Frees the
 * deferred bufs; if the response has not arrived yet (len_known false) the
 * slot is kept for the response handler, otherwise the used-ring push happens
 * now. Runs on the recv worker, lock-free except for the push itself.
 */
static bool local_zc_complete_one(RemoteVQueueCtx *ctx, VirtQueue *vq,
                                  InflightSlot *slot)
{
    VirtQueueElement *elem = slot->elem;

    /* free the buffers the network stack no longer references */
    inflight_free_bufs(slot);

    /* resp has not arrived yet: keep the slot for the response handler */
    if (!slot->zc.len_known) {
        return false;
    }

    /* resp arrived earlier: push the used ring entry now */
    g_mutex_lock(&ctx->vq_lock);
    virtqueue_push(vq, elem, slot->zc.push_len);
    virtio_notify(virtqueue_get_vdev(vq), vq);
    g_mutex_unlock(&ctx->vq_lock);
    g_free(elem);
    inflight_clear(ctx->inflight, slot->seq);
    return true;
}

/*
 * drain the socket error queue on the local side: each entry is a completion
 * notification for the zc sends whose serial is in [ee_info, ee_data]
 * (inclusive, may wrap). The completions are matched against the inflight
 * window slots (single consumer = recv worker, lock-free).
 */
static void local_zc_drain(RemoteVQueueCtx *ctx, VirtQueue *vq)
{
    ZcFdState *st = ctx->zc;
    Inflight *inf = ctx->inflight;
    char ctrl[CMSG_SPACE(sizeof(struct sock_extended_err)) * 16];
    char data[128];

    if (!st || !inf) {
        return;
    }
    for (;;) {
        struct iovec iov = { data, sizeof(data) };
        struct msghdr m = { 0 };
        m.msg_iov = &iov;
        m.msg_iovlen = 1;
        m.msg_control = ctrl;
        m.msg_controllen = sizeof(ctrl);
        ssize_t n = recvmsg(ctx->resp_fd, &m, MSG_ERRQUEUE);
        if (n < 0) {
            break; /* EAGAIN: error queue drained */
        }
        struct cmsghdr *cm;
        for (cm = CMSG_FIRSTHDR(&m); cm; cm = CMSG_NXTHDR(&m, cm)) {
            struct sock_extended_err *serr = (void *)CMSG_DATA(cm);
            if (serr->ee_origin != SO_EE_ORIGIN_ZEROCOPY) {
                continue;
            }
            uint32_t first = serr->ee_info;
            uint32_t last = serr->ee_data;
            /* snapshot the window once per completion */
            uint32_t head = qatomic_load_acquire(&inf->head);
            uint32_t tail = qatomic_load_acquire(&inf->tail);
            for (uint32_t seq = head; seq < tail; seq++) {
                InflightSlot *slot = inflight_slot(inf, seq);
                bool hit;
                if (!slot->is_zc) {
                    continue;
                }
                if (first <= last) {
                    hit = slot->zc.serial >= first && slot->zc.serial <= last;
                } else {
                    /* serial counter wrapped */
                    hit = slot->zc.serial >= first || slot->zc.serial <= last;
                }
                if (hit) {
                    local_zc_complete_one(ctx, vq, slot);
                }
            }
        }
    }
}

/*
 * drain the socket error queue on the stub side: matches the completions
 * against st->pending (the stub is single-threaded per vq, no lock needed).
 */
static void stub_zc_drain(RemoteVQueueCtx *ctx)
{
    ZcFdState *st = ctx->zc;
    char ctrl[CMSG_SPACE(sizeof(struct sock_extended_err)) * 16];
    char data[128];

    if (!st) {
        return;
    }
    for (;;) {
        struct iovec iov = { data, sizeof(data) };
        struct msghdr m = { 0 };
        m.msg_iov = &iov;
        m.msg_iovlen = 1;
        m.msg_control = ctrl;
        m.msg_controllen = sizeof(ctrl);
        ssize_t n = recvmsg(ctx->resp_fd, &m, MSG_ERRQUEUE);
        if (n < 0) {
            break; /* EAGAIN: error queue drained */
        }
        struct cmsghdr *cm;
        for (cm = CMSG_FIRSTHDR(&m); cm; cm = CMSG_NXTHDR(&m, cm)) {
            struct sock_extended_err *serr = (void *)CMSG_DATA(cm);
            if (serr->ee_origin != SO_EE_ORIGIN_ZEROCOPY) {
                continue;
            }
            uint32_t first = serr->ee_info;
            uint32_t last = serr->ee_data;
            GSList *it = st->pending;
            while (it) {
                ZcPending *zp = it->data;
                GSList *next = it->next;
                bool hit;
                if (first <= last) {
                    hit = zp->serial >= first && zp->serial <= last;
                } else {
                    /* serial counter wrapped */
                    hit = zp->serial >= first || zp->serial <= last;
                }
                if (hit) {
                    /* stub: no elem, just release the deferred bufs */
                    st->pending = g_slist_delete_link(st->pending, it);
                    if (zp->bufs) {
                        for (unsigned int i = 0; i < zp->n_bufs; i++) {
                            if (zp->bufs[i]) {
                                free(zp->bufs[i]);
                            }
                        }
                        g_free(zp->bufs);
                    }
                    g_free(zp);
                }
                it = next;
            }
        }
    }
}

/* local: connection torn down. Release every still-in-flight elem/buf under
 * vq_lock (serialized against the send worker's pop/publish/zc enable) and
 * drop the zc state. */
static void local_zc_fd_teardown(RemoteVQueueCtx *ctx)
{
    g_mutex_lock(&ctx->vq_lock);
    inflight_reset(ctx->inflight);
    g_free(ctx->zc);
    ctx->zc = NULL;
    g_mutex_unlock(&ctx->vq_lock);
}

/* stub: connection torn down; release every zc send still in flight */
static void stub_zc_fd_teardown(RemoteVQueueCtx *ctx)
{
    ZcFdState *st = ctx->zc;
    GSList *list;

    if (!st) {
        return;
    }
    list = st->pending;
    st->pending = NULL;
    for (GSList *it = list; it; it = it->next) {
        ZcPending *zp = it->data;
        if (zp->bufs) {
            for (unsigned int i = 0; i < zp->n_bufs; i++) {
                if (zp->bufs[i]) {
                    free(zp->bufs[i]);
                }
            }
            g_free(zp->bufs);
        }
        g_free(zp);
    }
    g_slist_free(list);
    g_free(st);
    ctx->zc = NULL;
}

/* -------------- Thread Pooling  ------------- */

/*
 * Internal per-vq processing thread pool, unrelated to the device IOThreads.
 * Every vq owns TWO workers: a send worker (kick path, pool send_workers) and
 * a recv worker (resp path, pool recv_workers), each hashed by vq_nr %
 * VIRTIO_REMOTE_WORKERS. A kick is always handed to the vq's send worker and
 * every resp to its recv worker, so the two may run concurrently; they are
 * serialized on the vq by vq_lock (virtqueue_pop/push/notify) and communicate
 * through the lock-free in-flight window. One worker handles
 * one task at a time: when it is busy the distributor absorbs the event and
 * records an owed redrain on the vq, which the worker replays once idle, so
 * the level-triggered source is consumed right away (no repeated wakeups).
 */
typedef struct Task {
    VirtQueue *vq;            /* the vq to process (written by distributor) */
    void (*fn)(void *opaque); /* task to run on the worker for this event */
} Task;

typedef struct Worker {
    AioContext *ctx;          /* this worker's event loop */
    QEMUBH *bh;               /* distributor -> worker handoff */
    QemuThread thread;
    Task task;    /* handoff, written by distributor, read by BH */
    int busy;                 /* 1 while a task is running (qatomic) */
    bool is_send;             /* send-pool worker: replays kick_pending */
    /*
     * vqs that route kicks to this worker. Built once during machine setup
     * (remote_worker_register_vq), before any kick can be delivered, so
     * worker_bh scans it lock-free; entries whose ctx is gone are skipped.
     */
    VirtQueue **vqs;
    unsigned int n_vqs;
    unsigned int vqs_cap;
} Worker;

static Worker *send_workers;
static Worker *recv_workers;
static GMutex workers_lock;
static bool workers_started;

static void local_send_handler(void *opaque);
static void local_response_handler(void *opaque);

// process a task by calling functions
static void worker_bh(void *opaque)
{
    Worker *w = opaque;

    if (w->task.fn) {
        w->task.fn(w->task.vq);
    }
    /* go idle first, then replay the kicks absorbed while busy: the
     * distributor records kick_pending *before* its failed claim, so every
     * pending stored before this busy=0 is visible to the scan below (seq-cst
     * atomics); a kick absorbed after busy=0 instead wins the claim and is
     * replayed by its own scheduled task. */
    qatomic_set(&w->busy, 0);
    if (w->is_send) {
        for (unsigned int i = 0; i < w->n_vqs; i++) {
            VirtQueue *vq = w->vqs[i];
            RemoteVQueueCtx *ctx = virtqueue_get_remote_ctx(vq);
            if (ctx && qatomic_xchg(&ctx->kick_pending, 0)) {
                local_send_handler(vq); /* drain the owed kick */
            }
        }
    }
}

// worker's workflow with aio and qemu bh framework
static void *workflow(void *opaque)
{
    Worker *w = opaque;

    rcu_register_thread();
    qemu_set_current_aio_context(w->ctx);
    while (true) {
        aio_poll(w->ctx, true);
    }
    return NULL;
}

// mosaic todo: move this to intialization to remove locks
static void start_workers(void)
{
    g_mutex_lock(&workers_lock);
    if (!workers_started) {
        send_workers = g_new0(Worker, VIRTIO_REMOTE_WORKERS);
        recv_workers = g_new0(Worker, VIRTIO_REMOTE_WORKERS);
        for (int i = 0; i < VIRTIO_REMOTE_WORKERS; i++) {
            Worker *w = &send_workers[i];
            w->ctx = aio_context_new(&error_abort);
            aio_context_set_poll_params(w->ctx, 0, 0, 0, &error_abort);
            w->busy = 0;
            w->is_send = true;
            w->bh = aio_bh_new(w->ctx, worker_bh, w);
            qemu_thread_create(&w->thread, "vremote-send",
                               workflow, w, QEMU_THREAD_JOINABLE);

            w = &recv_workers[i];
            w->ctx = aio_context_new(&error_abort);
            aio_context_set_poll_params(w->ctx, 0, 0, 0, &error_abort);
            w->busy = 0;
            w->is_send = false;
            w->bh = aio_bh_new(w->ctx, worker_bh, w);
            qemu_thread_create(&w->thread, "vremote-recv",
                               workflow, w, QEMU_THREAD_JOINABLE);
        }
        workers_started = true;
    }
    g_mutex_unlock(&workers_lock);
}

// lookup the send/recv worker thread of a vq by hash mapping
// mosaic: vq_nr may be the same for different vdevs, hash confict!
static Worker *worker_for_vq_send(VirtQueue *vq)
{
    start_workers();
    return &send_workers[virtio_get_queue_index(vq) % VIRTIO_REMOTE_WORKERS];
}

static Worker *worker_for_vq_recv(VirtQueue *vq)
{
    start_workers();
    return &recv_workers[virtio_get_queue_index(vq) % VIRTIO_REMOTE_WORKERS];
}

/* called at machine setup (local_set_remote): tell the vq's send worker about
 * it so worker_bh can replay kicks absorbed while busy. Set-up time only, so
 * the list is append-only before any kick can arrive; the worker scans it
 * lock-free (see the Worker::vqs comment). */
void remote_worker_register_vq(VirtQueue *vq)
{
    Worker *w = worker_for_vq_send(vq);

    g_mutex_lock(&workers_lock);
    for (unsigned int i = 0; i < w->n_vqs; i++) {
        if (w->vqs[i] == vq) {
            g_mutex_unlock(&workers_lock);
            return; /* already registered */
        }
    }
    if (w->n_vqs == w->vqs_cap) {
        w->vqs_cap = w->vqs_cap ? w->vqs_cap * 2 : 4;
        w->vqs = g_renew(VirtQueue *, w->vqs, w->vqs_cap);
    }
    w->vqs[w->n_vqs++] = vq;
    g_mutex_unlock(&workers_lock);
}

/*
 * Submit one event to the vq's worker. If clear_kick (kick path) the host
 * notifier is consumed unconditionally: on a successful claim the claimed
 * task's drain covers it; on a busy worker the kick is recorded as an owed
 * redrain (kick_pending) that worker_bh replays once it goes idle. Returns
 * true if the worker accepted the task (claimed), false if it was busy (the
 * event has been absorbed/recorded, or - for the recv/io_write paths where
 * clear_kick is false - left for the level-triggered source to re-arm).
 */
static bool pool_dispatch(VirtQueue *vq, bool is_recv,
                               void (*fn)(void *opaque), bool clear_kick)
{
    RemoteVQueueCtx *ctx = virtqueue_get_remote_ctx(vq);
    Worker *w;

    if (!ctx) {
        return false;
    }
    w = is_recv ? worker_for_vq_recv(vq) : worker_for_vq_send(vq);
    if (clear_kick) {
        /* absorb the kick before the claim: a failed claim leaves the owed
         * drain visible to the worker's post-idle scan (seq-cst ordering) */
        event_notifier_test_and_clear(virtqueue_get_host_notifier(vq));
        qatomic_set(&ctx->kick_pending, 1);
    }
    if (qatomic_cmpxchg(&w->busy, 0, 1) != 0) {
        return false; /* busy: the owed redrain (or re-arm) covers the event */
    }
    if (clear_kick) {
        /* we own the claim: our own drain covers the kick we just absorbed */
        qatomic_set(&ctx->kick_pending, 0);
    }
    w->task.vq = vq;
    w->task.fn = fn;
    qemu_bh_schedule(w->bh);
    return true;
}

/* -------------- Local QEMU Forwarding ------------- */

// enable socket keepalive in kernel, ~55s maximum link down detection
static void enable_tcp_keepalive(int fd)
{
    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &opt, sizeof(opt));
    int keep_idle = 30;
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &keep_idle, sizeof(keep_idle));
    int keep_intvl = 5;
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &keep_intvl, sizeof(keep_intvl));
    int keep_cnt = 5;
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &keep_cnt, sizeof(keep_cnt));
}

// connection retry parameters for the remote stub
#define CONNECT_RETRY_TIMES 3
#define CONNECT_RETRY_DELAY_MS 500
#define CONNECT_TIMEOUT_SEC 1

// control-plane message magic exchanged during port negotiation
#define LOCAL_NEGO_MAGIC 0x4c52434e /* "LRCN" */

// write all bytes to fd, or -1 on error (retries EINTR)
static int write_all(int fd, const void *buf, size_t len)
{
    size_t off = 0;
    while (off < len) {
        ssize_t n = write(fd, (const char *)buf + off, len - off);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (n == 0) {
            errno = EPIPE;
            return -1;
        }
        off += n;
    }
    return 0;
}

// read exactly len bytes from fd, or -1 on error / premature close
static int read_all(int fd, void *buf, size_t len)
{
    size_t off = 0;
    while (off < len) {
        ssize_t n = read(fd, (char *)buf + off, len - off);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (n == 0) {
            errno = ECONNRESET;
            return -1;
        }
        off += n;
    }
    return 0;
}

/*
 * Try to establish a TCP connection to addr with retries, bounding each
 * attempt with SO_SNDTIMEO. If recreate is true, a fresh socket is created
 * after each failed attempt (a failed connect() leaves the old fd unusable)
 * and the fd is owned by this function; otherwise the caller's fd is reused
 * as-is and left open on failure, so the caller keeps ownership.
 * Returns 0 on success, -1 on failure (errp set, *fdp closed if recreate).
 */
static int connect_with_retry(int *fdp, const struct sockaddr_in *addr,
                              bool recreate, Error **errp)
{
    int last_errno = 0;

    for (int attempt = 0; attempt < CONNECT_RETRY_TIMES; attempt++) {
        struct timeval tv = { .tv_sec = CONNECT_TIMEOUT_SEC };
        setsockopt(*fdp, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

        if (connect(*fdp, (struct sockaddr *)addr, sizeof(*addr)) == 0) {
            return 0;
        }
        last_errno = errno;

        if (attempt + 1 < CONNECT_RETRY_TIMES) {
            if (recreate) {
                close(*fdp);
                *fdp = socket(AF_INET, SOCK_STREAM, 0);
                if (*fdp < 0) {
                    last_errno = errno;
                    break;
                }
            }
            g_usleep(CONNECT_RETRY_DELAY_MS * 1000);
        }
    }

    if (recreate) {
        close(*fdp);
    }
    char ipstr[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &addr->sin_addr, ipstr, sizeof(ipstr));
    error_setg_errno(errp, last_errno,
                     "failed to connect to %s:%d after %d attempts",
                     ipstr, ntohs(addr->sin_port), CONNECT_RETRY_TIMES);
    return -1;
}

int local_connect_socket(const char *ip_port, int vq_nt, int *sockets,
                         struct sockaddr_in *dst, Error **errp)
{
    // parse ip and port
    char ip[64];
    int port;
    const char *at_pos = strchr(ip_port, '@');
    if (!at_pos) {
        error_setg(errp, "invalid ip_port format, expected ip@port");
        return -1;
    }
    size_t ip_len = at_pos - ip_port;
    if (ip_len >= sizeof(ip)) {
        error_setg(errp, "ip address too long");
        return -1;
    }
    memcpy(ip, ip_port, ip_len);
    ip[ip_len] = '\0';
    port = atoi(at_pos + 1);

    // configure ip and port
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, ip, &addr.sin_addr) <= 0) {
        error_setg(errp, "invalid ip address: %s", ip);
        return -1;
    }

    /*
     * The remote stub is assumed to be up before local qemu starts, but it
     * may take a moment to bind/listen, so retry a few times.
     */
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        error_setg_errno(errp, errno, "failed to create socket");
        return -1;
    }
    /* control connection; a failed connect() leaves the fd unusable, so let
     * the helper recreate the socket on each retry */
    if (connect_with_retry(&fd, &addr, true, errp) < 0) {
        return -1;
    }

    /* One TCP connection per vq. */
    int last_errno = 0;
    if (vq_nt > VIRTIO_QUEUE_MAX) {
        last_errno = EINVAL;
        goto err;
    }

    /* size the buffers by the actual vq count, not VIRTIO_QUEUE_MAX */
    g_autofree uint16_t *src_ports = g_new(uint16_t, vq_nt);
    g_autofree uint16_t *dst_ports = g_new(uint16_t, vq_nt);
    g_autofree char *msg = g_new(char, 8 + vq_nt * 2);
    int sock_count = 0;

    /* 1. ask the local kernel for vq_nt free source ports */
    for (int i = 0; i < vq_nt; i++) {
        int s = socket(AF_INET, SOCK_STREAM, 0);
        if (s < 0) {
            last_errno = errno;
            goto neg_fail;
        }
        struct sockaddr_in sin = { 0 };
        sin.sin_family = AF_INET;
        sin.sin_addr.s_addr = htonl(INADDR_ANY);
        sin.sin_port = 0; /* kernel picks a free port */
        if (bind(s, (struct sockaddr *)&sin, sizeof(sin)) < 0) {
            last_errno = errno;
            close(s);
            goto neg_fail;
        }
        socklen_t slen = sizeof(sin);
        if (getsockname(s, (struct sockaddr *)&sin, &slen) < 0) {
            last_errno = errno;
            close(s);
            goto neg_fail;
        }
        sockets[sock_count++] = s;
        src_ports[i] = ntohs(sin.sin_port);
    }

    /* 2. send the local ports to the remote stub over the control fd */
    uint32_t magic = htonl(LOCAL_NEGO_MAGIC);
    uint32_t cnt = htonl(vq_nt);
    memcpy(msg, &magic, 4);
    memcpy(msg + 4, &cnt, 4);
    for (int i = 0; i < vq_nt; i++) {
        uint16_t p = htons(src_ports[i]);
        memcpy(msg + 8 + i * 2, &p, 2);
    }
    if (write_all(fd, msg, 8 + vq_nt * 2) < 0) {
        last_errno = errno;
        goto neg_fail;
    }

    /* 3. receive the ports allocated by the remote stub */
    if (read_all(fd, msg, 8) < 0) {
        last_errno = errno;
        goto neg_fail;
    }
    memcpy(&magic, msg, 4);
    memcpy(&cnt, msg + 4, 4);
    if (ntohl(magic) != LOCAL_NEGO_MAGIC || ntohl(cnt) != vq_nt) {
        last_errno = EPROTO;
        goto neg_fail;
    }
    if (read_all(fd, msg, vq_nt * 2) < 0) {
        last_errno = errno;
        goto neg_fail;
    }
    for (int i = 0; i < vq_nt; i++) {
        uint16_t p;
        memcpy(&p, msg + i * 2, 2);
        dst_ports[i] = ntohs(p);
    }

    /* 4. hand out the stub-side destination addresses; the caller connects */
    for (int i = 0; i < vq_nt; i++) {
        dst[i] = addr; /* stub ip already set in addr */
        dst[i].sin_port = htons(dst_ports[i]);
    }

    return fd;

neg_fail:
    /* the control connection is unusable after a failed negotiation */
    close(fd);
    fd = -1;
    for (int i = 0; i < sock_count; i++) {
        close(sockets[i]);
        sockets[i] = -1;
    }
    /* fall through to err */

err:
    if (fd >= 0) {
        close(fd);
    }
    error_setg_errno(errp, last_errno,
                     "failed to set up %d remote connections to %s:%d",
                     vq_nt, ip, port);
    return -1;
}

bool local_connect_vq(int socket, const struct sockaddr_in *addr, Error **errp)
{
    if (connect_with_retry(&socket, addr, false, errp) < 0) {
        return false;
    }
    enable_tcp_keepalive(socket);
    /* MSG_ZEROCOPY is enabled lazily on the first send (see local_send_msg):
     * it must run on the socket iothread, which owns the zc state */
    return true;
}

// pre-definition
static bool local_send_msg(VirtQueue *vq, VirtQueueElement *elem);

/*
* called by io_write event, registered in local_send_msg
* when the kernel refuses to send because of lack of buffers, this handler is
* invoked once the socket becomes writable. The vring must only be touched by
* the vq's send worker thread, so this iothread-side handler only hands the vq
* back to its send worker (which retries the parked elem and drains the vring
* inside local_send_handler). If the worker is busy the io_write handler is
* kept, so the level-triggered G_IO_OUT re-fires once the socket is writable
* again.
*/
static void local_retry_send(void *opaque)
{
    VirtQueue *vq = opaque;
    RemoteVQueueCtx *ctx = virtqueue_get_remote_ctx(vq);

    if (!pool_dispatch(vq, false, local_send_handler, false)) {
        /* worker busy: keep io_write registered; it will re-fire */
        return;
    }
    /* the worker retries the parked elem; drop the writable handler */
    AioContext *ctx_aio = vq_get_aio_ctx(vq);
    if (ctx_aio) {
        aio_set_fd_handler(ctx_aio, ctx->resp_fd,
                           local_response_distributor, NULL, NULL, NULL, vq);
    }
}

/*
* called by local qemu local_send_handler()
* send pending elems in vq to remote stub
*/
static bool local_send_msg(VirtQueue *vq, VirtQueueElement *elem)
{
    RemoteVQueueCtx *ctx = virtqueue_get_remote_ctx(vq);
    if (!ctx) {
        return false;
    }
    int vq_nr = virtio_get_queue_index(vq);
    int seq = ctx->elem_index++;

    iovec *msg_sg = g_new0(iovec, elem->out_num + 2);
    int *header = g_new0(int, 4);
    header[0] = vq_nr;
    header[1] = seq;
    header[2] = elem->out_num;
    header[3] = elem->in_num;
    ssize_t total = 0;

    int *lens = g_new0(int, elem->out_num + elem->in_num);
    for (unsigned int i = 0; i < elem->out_num; i++) {
        lens[i] = elem->out_sg[i].iov_len;
        total += lens[i];
    }
    for (unsigned int i = 0; i < elem->in_num; i++) {
        lens[i + elem->out_num] = elem->in_sg[i].iov_len;
    }

    msg_sg[0].iov_base = header;
    msg_sg[0].iov_len = 4 * sizeof(int);
    msg_sg[1].iov_base = lens;
    msg_sg[1].iov_len = (elem->out_num + elem->in_num) * sizeof(int);
    memcpy(msg_sg + 2, elem->out_sg, elem->out_num * sizeof(iovec));
    total += msg_sg[0].iov_len + msg_sg[1].iov_len;

    struct msghdr msg = {
        .msg_iov = msg_sg,
        .msg_iovlen = elem->out_num + 2,
    };

    /*
     * Try MSG_ZEROCOPY for large requests: the NIC DMA's the out buffers
     * straight from the guest RAM pages (no skb copy). The kernel decides
     * per segment - anything it cannot pin is copied and a completion is
     * still queued, so the bookkeeping below is always correct.
     */
    if (!ctx->zc) {
        /* first send on this vq: enable MSG_ZEROCOPY. Runs on the vq's
         * worker thread, the same thread that owns the zc state otherwise. */
        ctx->zc = zc_enable(ctx->resp_fd);
    }
    ZcFdState *st = ctx->zc;
    bool used_zc = false;
    ssize_t ret;
    if (st && st->enabled && total >= ZC_SEND_MIN) {
        ret = sendmsg(ctx->resp_fd, &msg, MSG_ZEROCOPY | MSG_NOSIGNAL);
        if (ret < 0 && (errno == ENOBUFS || errno == EINVAL)) {
            /* kernel refuses zc for this call: fall back to a copy send */
            ret = sendmsg(ctx->resp_fd, &msg, MSG_NOSIGNAL);
        } else if (ret > 0) {
            used_zc = true;
        }
    } else {
        ret = sendmsg(ctx->resp_fd, &msg, MSG_NOSIGNAL);
    }

    if (used_zc) {
        /*
         * The kernel may still reference the header/lens and the guest's
         * out buffers until the completion arrives, so keep them alive. The
         * guest cannot reuse its out buffers until the used-ring push, and
         * the push is deferred until the completion (see
         * local_zc_complete_one / the response handler), which is exactly
         * the ack from the stub. The elem is published into the lock-free
         * in-flight window so the recv worker can match its response by seq.
         */
        void **bufs = g_new(void *, 3);
        bufs[0] = header;
        bufs[1] = lens;
        bufs[2] = msg_sg;
        ZcPending zc = {
            .serial = st->serial++,
            .bufs = bufs,
            .n_bufs = 3,
            .push_len = 0,
            .len_known = elem->in_num == 0,
        };
        g_mutex_lock(&ctx->vq_lock);
        inflight_publish(ctx->inflight, seq, elem, &zc);
        g_mutex_unlock(&ctx->vq_lock);
        return true;
    }
    g_free(lens);
    g_free(header);
    g_free(msg_sg);

    if (ret < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            /* send buffer full: hang the elem on the vq and retry when the
             * socket becomes writable, instead of dropping the request. The
             * aio loop keeps running; only this vq's sending is stalled. The
             * park needs no vq_lock: pending_elem is owned by this send
             * worker alone. */
            ctx->pending_elem = elem;
            AioContext *ctx_aio = vq_get_aio_ctx(vq);
            if (ctx_aio) {
                aio_set_fd_handler(ctx_aio, ctx->resp_fd,
                                   local_response_distributor, NULL, NULL,
                                   local_retry_send, vq);
            }
            return false;
        }
        error_report("local qemu: sendmsg to stub failed: %s", strerror(errno));
        return false;
    }

    if (elem->in_num == 0) {
        /* no in-buffers: the stub sends no resp for this request, so
         * complete the used-ring entry right away */
        g_mutex_lock(&ctx->vq_lock);
        virtqueue_push(vq, elem, 0);
        virtio_notify(virtqueue_get_vdev(vq), vq);
        g_mutex_unlock(&ctx->vq_lock);
        g_free(elem);
        return true;
    }

    /* track the in-flight elem so its response can be matched by seq */
    g_mutex_lock(&ctx->vq_lock);
    inflight_publish(ctx->inflight, seq, elem, NULL);
    g_mutex_unlock(&ctx->vq_lock);
    return true;
}

/*
 * forward decls of the stub window teardown helpers (defined in the stub
 * section below): remote_vq_ctx_destroy releases the stub req/resp windows
 * the same way the connection teardown does.
 */
typedef struct StubReq StubReq;
typedef struct StubResp StubResp;
static void stub_req_free(StubReq *req, bool free_bufs);
static void stub_resp_free(StubResp *sr, unsigned int sent_sgs,
                           bool zc_deferred);
static void stub_win_release(Inflight *win, bool is_req);

void remote_vq_ctx_init(RemoteVQueueCtx *ctx, unsigned int vring_num)
{
    g_mutex_init(&ctx->vq_lock);
    g_mutex_init(&ctx->push_lock);
    g_cond_init(&ctx->push_cond);
    ctx->inflight = NULL;
    ctx->req_win = NULL;
    ctx->resp_win = NULL;
    ctx->push_done = 0;
    ctx->dead = 0;
    ctx->handle_busy = 0;
    ctx->send_busy = 0;
    ctx->send_writable = false;
    if (vring_num > 0) {
        Inflight *inf = g_new0(Inflight, 1);
        inf->size = pow2ceil(vring_num);
        inf->mask = inf->size - 1;
        inf->slots = g_new0(InflightSlot, inf->size);
        ctx->inflight = inf;
    }
}

/* vq-level destroy: every ctx free site goes through here. pending_elem is
 * parked by the send worker and released only at this vq level, never by the
 * workers. Must run after the vq's workers have stopped: pending_elem is
 * freed without vq_lock (send-worker-owned), the inflight window is torn down
 * under vq_lock (serialized against a racing local_zc_fd_teardown). On the
 * stub side the req/resp windows are normally already released by the
 * connection teardown (stub_teardown_vq), but a plain virtio teardown with a
 * still-live connection reaches this as the first (and only) release site. */
void remote_vq_ctx_destroy(RemoteVQueueCtx *ctx)
{
    if (ctx->pending_elem) {
        g_free(ctx->pending_elem);
        ctx->pending_elem = NULL;
    }
    g_mutex_lock(&ctx->vq_lock);
    if (ctx->inflight) {
        Inflight *inf = ctx->inflight;
        inflight_reset(inf);
        g_free(inf->slots);
        g_free(inf);
        ctx->inflight = NULL;
    }
    g_mutex_unlock(&ctx->vq_lock);
    if (ctx->req_win) {
        stub_win_release(ctx->req_win, true);
        ctx->req_win = NULL;
    }
    if (ctx->resp_win) {
        stub_win_release(ctx->resp_win, false);
        ctx->resp_win = NULL;
    }
    g_mutex_clear(&ctx->push_lock);
    g_cond_clear(&ctx->push_cond);
    g_mutex_clear(&ctx->vq_lock);
}

/* -------------- Local QEMU Handlers ------------- */

int virtio_device_start_ioeventfd_impl_local(VirtIODevice *vdev, AioContext *aio_ctx)
{
    VirtioBusState *qbus = VIRTIO_BUS(qdev_get_parent_bus(DEVICE(vdev)));
    int i, r;
    memory_region_transaction_begin();
    for (i = 0; i < VIRTIO_QUEUE_MAX; i++) {
        if (!virtio_queue_get_num(vdev, i))
            continue;
        r = virtio_bus_set_host_notifier(qbus, i, true);
        if (r != 0) {
            fprintf(stderr, "virtio-blk failed to set host notifier (%d)\n", r);
            goto assign_err;
        }
    }
    memory_region_transaction_commit();
    for (i = 0; i < VIRTIO_QUEUE_MAX; i++) {
        VirtQueue *vq = virtio_get_queue(vdev, i);
        virtio_queue_aio_attach_host_notifier(vq, aio_ctx);
    }

    return 0;

assign_err:
    memory_region_transaction_commit();
    for (i = 0; i < VIRTIO_QUEUE_MAX; i++) {
        if (!virtio_queue_get_num(vdev, i))
            continue;
        virtio_bus_set_host_notifier(qbus, i, false);
    }
    memory_region_transaction_commit();
    for (i = 0; i < VIRTIO_QUEUE_MAX; i++) {
        if (!virtio_queue_get_num(vdev, i))
            continue;
        virtio_bus_cleanup_host_notifier(qbus, i);
    }
    return 0;
}

void local_notifier_distributor(EventNotifier *n)
{
    VirtQueue *vq = host_notifier_to_vq(n);

    /* always consume the kick (success or busy): a successful claim's drain
     * covers it, a busy worker replays it via kick_pending once idle, so the
     * level-triggered eventfd does not keep waking the iothread */
    pool_dispatch(vq, false, local_send_handler, true);
}

void local_response_distributor(void *opaque)
{
    VirtQueue *vq = opaque;

    pool_dispatch(vq, true, local_response_handler, false);
}

static void local_send_handler(void *opaque)
{
    VirtQueue *vq = opaque;
    RemoteVQueueCtx *ctx = virtqueue_get_remote_ctx(vq);
    VirtQueueElement *elem;

    if (!ctx) {
        return;
    }
    /* an elem is parked on the socket being full: retry it before popping
     * more, otherwise the vring would be drained past the parked elem. The
     * park is owned by this send worker alone (the recv worker never touches
     * it), so it needs no vq_lock; a successful retry publishes the elem into
     * the in-flight window like any other send. */
    elem = ctx->pending_elem;
    ctx->pending_elem = NULL;
    if (elem) {
        if (!local_send_msg(vq, elem)) {
            /* socket still full: re-park; the writable handler re-arms us */
            ctx->pending_elem = elem;
            return;
        }
    }
    /* drain the vring: pop and submit as many elems as possible without
     * blocking the aio loop */
    // to review: the same fd event will be lift up, may causing HoL, think about multiple aio iothread
    while (true) {
        g_mutex_lock(&ctx->vq_lock);
        elem = virtqueue_pop(vq, sizeof(VirtQueueElement));
        g_mutex_unlock(&ctx->vq_lock);
        if (!elem) {
            break;
        }
        if (!local_send_msg(vq, elem)) { // send false, wait for the next time
            break;
        }
    }
}


/* true if any active vq of vdev carries a remote ctx: this process is the
 * remote stub for vdev (the local qemu side is marked by register_mosaic) */
static bool stub_vdev_has_remote_vq(VirtIODevice *vdev)
{
    for (int n = 0; n < VIRTIO_QUEUE_MAX; n++) {
        VirtQueue *vq = virtio_get_queue(vdev, n);
        if (virtio_queue_get_num(vdev, n) && virtqueue_get_remote_ctx(vq)) {
            return true;
        }
    }
    return false;
}

bool remote_virtio_queue_empty(void *opaque)
{
    RemoteVQueueCtx *ctx = opaque;
    return ctx->elem != NULL; /* popped once but not pushed yet */
}

bool remote_virtio_notify_skip(VirtIODevice *vdev)
{
    /* the stub process has no guest to deliver the config interrupt to */
    return !is_mosaic(vdev) && stub_vdev_has_remote_vq(vdev);
}

bool check_virtio_device_remote(VirtIODevice *vdev)
{
    /* the local qemu side is marked by register_mosaic; only the stub
     * process (no mosaic marking) dispatches through the remote stubs */
    return !is_mosaic(vdev) && stub_vdev_has_remote_vq(vdev);
}

bool check_origin_qemu_in_iothread(VirtIODevice *vdev)
{
    return local_search_aio_ctx(vdev) != NULL;
}

void remote_virtio_device_stop_ioeventfd_impl(VirtIODevice *vdev)
{
}






/*
 * per-connection receive state for incremental, non-blocking reads of the
 * resp stream [vq_nr(4B)][elem_index(4B)][data_len(4B)][data...]
 */
typedef struct LocalRecvState {
    int stage;            /* 0 = reading header, 1 = reading data */
    unsigned int hdr_off; /* header bytes read so far */
    uint8_t hdr[12];      /* partial header */
    VirtQueueElement *cur;    /* elem whose data is being received */
    unsigned int cur_seq; /* seq of the current response */
    unsigned int cur_off; /* data bytes already written into in_sg */
    unsigned int need_len;    /* total data bytes expected */
} LocalRecvState;

static void local_response_handler(void *opaque)
{
    VirtQueue *vq = opaque;
    RemoteVQueueCtx *ctx = virtqueue_get_remote_ctx(vq);
    int fd = ctx->resp_fd;

    /* drain MSG_ZEROCOPY completions: each one may release a deferred
     * used-ring push (POLLERR on this fd wakes this handler) */
    local_zc_drain(ctx, vq);

    LocalRecvState *rs = ctx->recv;
    if (!rs) {
        rs = g_new0(LocalRecvState, 1);
        ctx->recv = rs;
    }

    while (true) {
        if (rs->stage == 0) {
            /* resp header: [vq_nr][elem_index][data_len], native endian */
            ssize_t n = recv(fd, rs->hdr + rs->hdr_off,
                             sizeof(rs->hdr) - rs->hdr_off, 0);
            if (n < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    return; /* wait for the next readable event */
                }
                error_report("local qemu: recv resp header failed: %s",
                             strerror(errno));
                goto conn_err;
            }
            if (n == 0) {
                error_report("local qemu: stub closed vq connection");
                goto conn_err;
            }
            rs->hdr_off += n;
            if (rs->hdr_off < sizeof(rs->hdr)) {
                return; /* header incomplete, wait for more */
            }
            int vq_nr, seq, len;
            memcpy(&vq_nr, rs->hdr, 4);
            memcpy(&seq, rs->hdr + 4, 4);
            memcpy(&len, rs->hdr + 8, 4);
            if (vq_nr != virtio_get_queue_index(vq)) {
                error_report("local qemu: resp vq_nr %d, expected %d",
                             vq_nr, virtio_get_queue_index(vq));
                goto conn_err;
            }
            /* match the seq against the lock-free in-flight window: the slot
             * was published by the send worker and is consumed by this recv
             * worker, so no lock is needed */
            rs->cur = inflight_lookup(ctx->inflight, seq);
            if (!rs->cur) {
                error_report("local qemu: no pending elem for seq %d", seq);
                goto conn_err;
            }
            rs->cur_seq = seq;
            rs->need_len = len;
            rs->cur_off = 0;
            rs->stage = 1;
        }

        if (rs->stage == 1) {
            VirtQueueElement *elem = rs->cur;
            if (rs->cur_off >= rs->need_len) {
                Inflight *inf = ctx->inflight;
                InflightSlot *slot = inf ? inflight_slot(inf, rs->cur_seq) : NULL;
                if (slot && slot->is_zc) {
                    /* this elem was sent with MSG_ZEROCOPY: record the resp
                     * len and let the kernel completion (local_zc_drain) own
                     * the used-ring push, which releases the guest's out
                     * buffers once the NIC is done with them */
                    slot->zc.push_len = rs->need_len;
                    slot->zc.len_known = true;
                    if (!slot->zc.bufs) {
                        /* zc completed before the resp: push now */
                        g_mutex_lock(&ctx->vq_lock);
                        virtqueue_push(vq, elem, rs->need_len);
                        virtio_notify(virtqueue_get_vdev(vq), vq);
                        g_mutex_unlock(&ctx->vq_lock);
                        g_free(elem);
                        inflight_clear(ctx->inflight, rs->cur_seq);
                    } /* else {} wait local_zc_complete_one() to handle */
                } else {
                    /* copy-sent elem: push right away and clear the slot */
                    g_mutex_lock(&ctx->vq_lock);
                    virtqueue_push(vq, elem, rs->need_len);
                    virtio_notify(virtqueue_get_vdev(vq), vq);
                    g_mutex_unlock(&ctx->vq_lock);
                    g_free(elem);
                    inflight_clear(ctx->inflight, rs->cur_seq);
                }
                rs->stage = 0;
                rs->hdr_off = 0;
                rs->cur = NULL;
                rs->cur_off = 0;
                rs->need_len = 0;
                continue; /* try to read the next response */
            }
            /* write the response data into the elem's in_sg buffers */
            unsigned int off = 0;
            unsigned int i;
            for (i = 0; i < elem->in_num; i++) {
                if (rs->cur_off < off + elem->in_sg[i].iov_len) {
                    break;
                }
                off += elem->in_sg[i].iov_len;
            }
            ssize_t n;
            if (i >= elem->in_num) {
                /* stub sent more data than the in-buffers can hold:
                 * drain and discard the excess to keep the stream in sync */
                unsigned int want = rs->need_len - rs->cur_off;
                char drop[256];
                n = recv(fd, drop, MIN(sizeof(drop), want), 0);
            } else {
                unsigned int iov_off = rs->cur_off - off;
                unsigned int want = MIN(elem->in_sg[i].iov_len - iov_off,
                                        rs->need_len - rs->cur_off);
                n = recv(fd, (char *)elem->in_sg[i].iov_base + iov_off,
                         want, 0);
            }
            if (n < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    return;
                }
                error_report("local qemu: recv resp data failed: %s",
                             strerror(errno));
                goto conn_err;
            }
            if (n == 0) {
                error_report("local qemu: stub closed vq connection mid-data");
                goto conn_err;
            }
            rs->cur_off += n;
        }
    }

conn_err:
    /* the connection is unusable; drop the in-flight elem and reset state.
     * local_zc_fd_teardown() owns (frees) every elem still in the in-flight
     * window, so only free rs->cur here if it is not one of them. The parked
     * send-retry elem is left alone: it is owned by the send worker and
     * released only at the vq-level remote_vq_ctx_destroy. */
    Inflight *inf = ctx->inflight;
    InflightSlot *slot = inf ? inflight_slot(inf, rs->cur_seq) : NULL;
    bool cur_in_flight = slot && slot->elem == rs->cur;
    local_zc_fd_teardown(ctx);
    if (rs->cur) {
        if (!cur_in_flight) {
            g_free(rs->cur);
        }
        rs->cur = NULL;
    }
    rs->stage = 0;
    rs->hdr_off = 0;
    rs->cur_off = 0;
    rs->need_len = 0;
}

/* -------------- Remote Stub ------------- */

/*
 * allocate a VirtQueueElement with room for out_num/in_num sg entries.
 * out_addr/in_addr are kept zeroed so the stub never tries to dma_unmap them.
 */
static VirtQueueElement *remote_stub_virtqueue_alloc_element(size_t sz,
                                                             unsigned int out_num,
                                                             unsigned int in_num)
{
    VirtQueueElement *elem;
    size_t in_addr_ofs = QEMU_ALIGN_UP(sz, __alignof__(elem->in_addr[0]));
    size_t out_addr_ofs = in_addr_ofs + in_num * sizeof(elem->in_addr[0]);
    size_t out_addr_end = out_addr_ofs + out_num * sizeof(elem->out_addr[0]);
    size_t in_sg_ofs = QEMU_ALIGN_UP(out_addr_end, __alignof__(elem->in_sg[0]));
    size_t out_sg_ofs = in_sg_ofs + in_num * sizeof(elem->in_sg[0]);
    size_t out_sg_end = out_sg_ofs + out_num * sizeof(elem->out_sg[0]);

    assert(sz >= sizeof(VirtQueueElement));
    elem = g_malloc(out_sg_end);
    elem->out_num = out_num;
    elem->in_num = in_num;
    elem->in_addr = (void *)elem + in_addr_ofs;
    elem->out_addr = (void *)elem + out_addr_ofs;
    elem->in_sg = (void *)elem + in_sg_ofs;
    elem->out_sg = (void *)elem + out_sg_ofs;
    return elem;
}

/*
 * A fully received request handed from the distributor to the handle worker
 * through the req_win SPSC window. Owns the out/in iovec arrays; the payload
 * buffers they describe are released by the device's push() once the req has
 * been popped (handed = true), otherwise by the window teardown.
 */
struct StubReq {
    unsigned int elem_index;    /* seq echoed back in the resp */
    unsigned int out_num, in_num;
    struct iovec *out_sg;       /* out_num entries, buffers allocated */
    struct iovec *in_sg;        /* in_num entries, buffers allocated */
    bool handed;                /* pop() gave the buffers to the device */
};

void *remote_stub_virtqueue_pop(VirtQueue *vq, size_t sz)
{
    RemoteVQueueCtx *ctx = virtqueue_get_remote_ctx(vq);
    StubReq *req;
    VirtQueueElement *ret;
    int out_num, in_num;

    if (!ctx || ctx->elem) {
        return NULL;
    }
    /* the request currently being handled (set by stub_handle_task before
     * calling handle_output); vq-internal serialization guarantees this is
     * the only pop outstanding for the vq */
    req = ctx->active_req;
    if (!req) {
        return NULL;
    }
    out_num = req->out_num;
    in_num = req->in_num;
    ret = remote_stub_virtqueue_alloc_element(sz, out_num, in_num);
    ret->index = req->elem_index;
    ret->ndescs = 1;
    ret->in_order_filled = false;
    ret->len = 0;
    for (int i = 0; i < out_num; i++) {
        ret->out_addr[i] = 0;
        ret->out_sg[i] = req->out_sg[i];
    }
    for (int i = 0; i < in_num; i++) {
        ret->in_addr[i] = 0;
        ret->in_sg[i] = req->in_sg[i];
    }
    /* the request buffers now belong to the device until its push() */
    req->handed = true;
    ctx->elem = (void *)ret;
    return ret;
}

/*
 * a response queued on the stub side. The header + iov describe the resp
 * [vq_nr(4B)][elem_index(4B)][data_len(4B)][data...]; the in_sg payload
 * buffers are owned here until the send (or the zc completion) happens.
 */
struct StubResp {
    int *header;          /* resp header, owned until sent */
    struct iovec *iov;    /* resp_iov: [header, in_sg...], owned until sent */
    int iov_cnt;
    unsigned int len;     /* resp payload length (zc threshold) */
    void **in_bufs;       /* bases of the in_sg payload buffers */
    unsigned int n_in_sg;
};

/* release a StubReq. free_bufs also releases the payload buffers: only valid
 * for reqs never popped (once handed, the device's push() owns the buffers). */
static void stub_req_free(StubReq *req, bool free_bufs)
{
    if (free_bufs) {
        for (unsigned int i = 0; i < req->out_num; i++) {
            g_free(req->out_sg[i].iov_base);
        }
        for (unsigned int i = 0; i < req->in_num; i++) {
            free(req->in_sg[i].iov_base);   /* posix_memalign'd */
        }
    }
    g_free(req->out_sg);
    g_free(req->in_sg);
    g_free(req);
}

/* stub-side task / worker-pool plumbing. The handle pool runs the device
 * handle_output, the send pool runs sendmsg()/MSG_ZEROCOPY. A vq maps to one
 * handle worker and one send worker (vq_nr % VIRTIO_REMOTE_WORKERS), so every
 * req_win/resp_win stays strict SPSC. Tasks are dispatched with the same
 * Worker/bh mechanism as the local pools (is_send=false: no kick replay). */
static void stub_handle_task(void *opaque);
static void stub_send_task(void *opaque);
static void stub_send_writable(void *opaque);
static void stub_send_detach(VirtQueue *vq);
static Worker *stub_worker_for_handle(VirtQueue *vq);
static Worker *stub_worker_for_send(VirtQueue *vq);

static Worker *stub_handle_workers;
static Worker *stub_send_workers;
static bool stub_workers_started;

/* dispatch a task to a stub worker: record the vq + fn and schedule the bh.
 * No busy-claim: a dispatch to a busy worker overwrites the pending task, but
 * stub_worker_bh re-scans every registered vq afterwards, so no published
 * work is ever lost. The distributor (socket iothread) and the handle worker
 * (after each push) are the usual callers. */
static void stub_dispatch(Worker *w, VirtQueue *vq, void (*fn)(void *opaque))
{
    w->task.vq = vq;
    w->task.fn = fn;
    qemu_bh_schedule(w->bh);
}

/* stub pool bh: run the dispatched task, then re-scan every registered vq so
 * requests/responses published while the worker was busy are picked up (a
 * dropped dispatch is never a lost wakeup). Dead vqs are skipped. */
static void stub_worker_bh(void *opaque)
{
    Worker *w = opaque;

    if (w->task.fn) {
        w->task.fn(w->task.vq);
    }
    qatomic_set(&w->busy, 0);
    for (unsigned int i = 0; i < w->n_vqs; i++) {
        VirtQueue *vq = w->vqs[i];
        RemoteVQueueCtx *ctx = virtqueue_get_remote_ctx(vq);
        Inflight *req_win, *resp_win;
        if (!ctx || qatomic_load_acquire(&ctx->dead)) {
            continue;
        }
        req_win = ctx->req_win;
        resp_win = ctx->resp_win;
        if (req_win &&
            qatomic_load_acquire(&req_win->head) < qatomic_load_acquire(&req_win->tail)) {
            stub_dispatch(stub_worker_for_handle(vq), vq, stub_handle_task);
        }
        if (resp_win && !ctx->send_writable &&
            qatomic_load_acquire(&resp_win->head) < qatomic_load_acquire(&resp_win->tail)) {
            stub_dispatch(stub_worker_for_send(vq), vq, stub_send_task);
        }
    }
}

static void stub_start_workers(void)
{
    g_mutex_lock(&workers_lock);
    if (!stub_workers_started) {
        stub_handle_workers = g_new0(Worker, VIRTIO_REMOTE_WORKERS);
        stub_send_workers = g_new0(Worker, VIRTIO_REMOTE_WORKERS);
        for (int i = 0; i < VIRTIO_REMOTE_WORKERS; i++) {
            Worker *w = &stub_handle_workers[i];
            w->ctx = aio_context_new(&error_abort);
            aio_context_set_poll_params(w->ctx, 0, 0, 0, &error_abort);
            w->busy = 0;
            w->is_send = false;
            w->bh = aio_bh_new(w->ctx, stub_worker_bh, w);
            qemu_thread_create(&w->thread, "stub-handle",
                               workflow, w, QEMU_THREAD_JOINABLE);

            w = &stub_send_workers[i];
            w->ctx = aio_context_new(&error_abort);
            aio_context_set_poll_params(w->ctx, 0, 0, 0, &error_abort);
            w->busy = 0;
            w->is_send = false;
            w->bh = aio_bh_new(w->ctx, stub_worker_bh, w);
            qemu_thread_create(&w->thread, "stub-send",
                               workflow, w, QEMU_THREAD_JOINABLE);
        }
        stub_workers_started = true;
    }
    g_mutex_unlock(&workers_lock);
}

static Worker *stub_worker_for_handle(VirtQueue *vq)
{
    stub_start_workers();
    return &stub_handle_workers[virtio_get_queue_index(vq) % VIRTIO_REMOTE_WORKERS];
}

static Worker *stub_worker_for_send(VirtQueue *vq)
{
    stub_start_workers();
    return &stub_send_workers[virtio_get_queue_index(vq) % VIRTIO_REMOTE_WORKERS];
}

/* register a stub vq with its handle and send workers (called by the accept
 * handler once per accepted vq). Setup-time append, read lock-free by
 * stub_worker_bh, same pattern as remote_worker_register_vq. */
static void stub_register_vq(VirtQueue *vq)
{
    Worker *hw = stub_worker_for_handle(vq);
    Worker *sw = stub_worker_for_send(vq);

    g_mutex_lock(&workers_lock);
    if (hw->n_vqs == hw->vqs_cap) {
        hw->vqs_cap = hw->vqs_cap ? hw->vqs_cap * 2 : 4;
        hw->vqs = g_renew(VirtQueue *, hw->vqs, hw->vqs_cap);
    }
    hw->vqs[hw->n_vqs++] = vq;
    if (sw->n_vqs == sw->vqs_cap) {
        sw->vqs_cap = sw->vqs_cap ? sw->vqs_cap * 2 : 4;
        sw->vqs = g_renew(VirtQueue *, sw->vqs, sw->vqs_cap);
    }
    sw->vqs[sw->n_vqs++] = vq;
    g_mutex_unlock(&workers_lock);
}

/* release a StubResp and its payload buffers. zc_deferred keeps the header,
 * iov and the first sent_sgs in buffers alive for a zc completion. */
static void stub_resp_free(StubResp *sr, unsigned int sent_sgs,
                           bool zc_deferred)
{
    if (zc_deferred) {
        for (unsigned int i = sent_sgs; i < sr->n_in_sg; i++) {
            free(sr->in_bufs[i]);
        }
    } else {
        g_free(sr->header);
        g_free(sr->iov);
        for (unsigned int i = 0; i < sr->n_in_sg; i++) {
            free(sr->in_bufs[i]);
        }
    }
    g_free(sr->in_bufs);
    g_free(sr);
}

/* allocate the per-vq stub windows (distributor->handle req_win and
 * handle->send resp_win), sized to the vring: in-flight requests are bounded
 * by vring.num - 1 on the local side (virtqueue_pop's inuse guard), so one
 * slot per request never wraps. Called by the accept handler right after
 * remote_vq_ctx_init. */
static void stub_ctx_init_windows(RemoteVQueueCtx *ctx, unsigned int vring_num)
{
    uint32_t size = pow2ceil(vring_num);
    Inflight *req_win = g_new0(Inflight, 1);
    req_win->size = size;
    req_win->mask = size - 1;
    req_win->slots = g_new0(InflightSlot, size);
    ctx->req_win = req_win;

    Inflight *resp_win = g_new0(Inflight, 1);
    resp_win->size = size;
    resp_win->mask = size - 1;
    resp_win->slots = g_new0(InflightSlot, size);
    ctx->resp_win = resp_win;
}

/* release every element still in a stub window (connection teardown) and free
 * the window itself. is_req selects the StubReq vs StubResp release; a popped
 * req (handed) already gave its buffers to the device's elem, which the
 * device releases on its (dead) push, so only un-popped reqs free them. */
static void stub_win_release(Inflight *win, bool is_req)
{
    uint32_t head = qatomic_load_acquire(&win->head);
    uint32_t tail = qatomic_load_acquire(&win->tail);

    for (uint32_t seq = head; seq < tail; seq++) {
        InflightSlot *slot = inflight_slot(win, seq);
        if (!slot->elem) {
            continue;
        }
        if (is_req) {
            StubReq *req = slot->elem;
            stub_req_free(req, !req->handed);
        } else {
            StubResp *sr = slot->elem;
            stub_resp_free(sr, sr->iov_cnt - 1, false);
        }
        slot->elem = NULL;
    }
    g_free(win->slots);
    g_free(win);
}

/* drop a parked send-worker writable handler for a vq (connection teardown).
 * Runs from the send worker (dead path) or the iothread (conn_err), both
 * before the fd is closed. */
static void stub_send_detach(VirtQueue *vq)
{
    RemoteVQueueCtx *ctx = virtqueue_get_remote_ctx(vq);
    Worker *w = stub_worker_for_send(vq);

    if (ctx && ctx->send_writable && ctx->resp_fd >= 0) {
        aio_set_fd_handler(w->ctx, ctx->resp_fd, NULL, NULL, NULL, NULL, NULL);
        ctx->send_writable = false;
    }
}

/*
 * connection teardown shared by conn_err (req_handler) and the accept-handler
 * fail rollback. Runs on the socket iothread; after it returns no worker may
 * touch the vq's windows again:
 *   1. dead -> every task still entering for this vq bails out before windows
 *   2. broadcast -> a handle worker parked on push_done wakes and exits
 *   3. busy-wait -> both workers have left their current tasks (a dispatch
 *      still in flight when the teardown started sees dead on entry)
 *   4. detach/zc/windows/fd -> released on the now-quiet vq. The windows are
 *      torn down under push_lock so a device async push racing the teardown
 *      (it publishes under the same lock) is fully ordered.
 */
static void stub_teardown_vq(VirtQueue *vq)
{
    RemoteVQueueCtx *ctx = virtqueue_get_remote_ctx(vq);
    int fd;

    if (!ctx) {
        return;
    }
    fd = ctx->resp_fd;
    qatomic_set(&ctx->dead, 1);
    g_mutex_lock(&ctx->push_lock);
    g_cond_broadcast(&ctx->push_cond);
    g_mutex_unlock(&ctx->push_lock);
    while (qatomic_load_acquire(&ctx->handle_busy) ||
           qatomic_load_acquire(&ctx->send_busy)) {
        g_usleep(1000); /* workers exit at the next safe point */
    }
    stub_send_detach(vq);
    stub_zc_fd_teardown(ctx);
    g_mutex_lock(&ctx->push_lock);
    if (ctx->req_win) {
        stub_win_release(ctx->req_win, true);
        ctx->req_win = NULL;
    }
    if (ctx->resp_win) {
        stub_win_release(ctx->resp_win, false);
        ctx->resp_win = NULL;
    }
    g_mutex_unlock(&ctx->push_lock);
    if (fd >= 0) {
        close(fd);
        ctx->resp_fd = -1;
    }
}

/*
 * handle worker task for a vq: consume the request at the req_win head and
 * run the device handle_output. vq-internal serialization guarantees at most
 * one active elem, so ctx->active_req/push_done need no extra locking. Once
 * the device pushed (push_done), release the req slot and hand the resp to
 * the send worker. Async devices block here until the push (or the teardown
 * broadcast) wakes them.
 */
static void stub_handle_task(void *opaque)
{
    VirtQueue *vq = opaque;
    RemoteVQueueCtx *ctx = virtqueue_get_remote_ctx(vq);
    Inflight *req_win;
    StubReq *req;
    unsigned int head;

    if (!ctx) {
        return; /* a dispatch that outlived the ctx teardown */
    }
    qatomic_set(&ctx->handle_busy, 1);
    if (qatomic_load_acquire(&ctx->dead) || !(req_win = ctx->req_win)) {
        goto out;
    }
    head = qatomic_load_acquire(&req_win->head);
    req = inflight_lookup(req_win, head);
    if (!req) {
        goto out;
    }
    qatomic_set(&ctx->push_done, 0);
    ctx->active_req = req;
    virtqueue_call_handle_output(vq);          /* device pops and pushes */
    ctx->active_req = NULL;

    /* async device: the push may arrive after handle_output returned */
    if (!qatomic_load_acquire(&ctx->push_done)) {
        g_mutex_lock(&ctx->push_lock);
        while (!qatomic_load_acquire(&ctx->push_done) &&
               !qatomic_load_acquire(&ctx->dead)) {
            g_cond_wait(&ctx->push_cond, &ctx->push_lock);
        }
        g_mutex_unlock(&ctx->push_lock);
    }
    if (qatomic_load_acquire(&ctx->dead)) {
        /* connection gone: the buffers are the device's once handed (freed by
         * its dropped push); release the rest and leave the teardown the rest */
        inflight_clear(req_win, head);
        stub_req_free(req, !req->handed);
        goto out;
    }
    inflight_clear(req_win, head);
    stub_req_free(req, false);
    stub_dispatch(stub_worker_for_send(vq), vq, stub_send_task);
out:
    qatomic_set(&ctx->handle_busy, 0);
}

/*
 * send worker task for a vq: drain the resp_win head, doing the
 * sendmsg()/MSG_ZEROCOPY and the EAGAIN retry. The zc serial/pending list is
 * owned by this worker alone (the single consumer of resp_win). When the
 * window is fully drained, broadcasts push_cond so a push blocked on a full
 * resp_win (backpressure) can proceed.
 */
static void stub_send_task(void *opaque)
{
    VirtQueue *vq = opaque;
    RemoteVQueueCtx *ctx = virtqueue_get_remote_ctx(vq);
    Worker *w = stub_worker_for_send(vq);
    Inflight *resp_win;
    int fd;

    if (!ctx) {
        return; /* a dispatch that outlived the ctx teardown */
    }
    qatomic_set(&ctx->send_busy, 1);
    if (qatomic_load_acquire(&ctx->dead)) {
        stub_send_detach(vq);
        goto out;
    }
    /* zc completions for this vq belong to this worker */
    stub_zc_drain(ctx);

    resp_win = ctx->resp_win;
    fd = ctx->resp_fd;
    if (!resp_win || fd < 0) {
        goto out;
    }
    for (;;) {
        unsigned int seq = qatomic_load_acquire(&resp_win->head);
        StubResp *sr = inflight_lookup(resp_win, seq);
        if (!sr) {
            break;
        }
        if (ctx->send_writable) {
            /* socket still full: the parked writable handler retries */
            goto out;
        }

        struct msghdr msg = { .msg_iov = sr->iov, .msg_iovlen = sr->iov_cnt };
        ZcFdState *st = ctx->zc;
        bool used_zc = false;
        ssize_t ret;
        if (st && st->enabled && sr->len >= ZC_SEND_MIN) {
            ret = sendmsg(fd, &msg, MSG_ZEROCOPY | MSG_NOSIGNAL);
            if (ret < 0 && (errno == ENOBUFS || errno == EINVAL)) {
                /* kernel refuses zc for this call: fall back to a copy send */
                ret = sendmsg(fd, &msg, MSG_NOSIGNAL);
            } else if (ret > 0) {
                used_zc = true;
            }
        } else {
            ret = sendmsg(fd, &msg, MSG_NOSIGNAL);
        }
        if (ret < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            /* send buffer full: keep the head parked and retry on writable.
             * The writable handler is registered on this worker's own aio
             * context, so the retry stays on the send worker. */
            ctx->send_writable = true;
            aio_set_fd_handler(w->ctx, fd, NULL, stub_send_writable,
                               NULL, NULL, vq);
            goto out;
        }
        if (ret < 0) {
            error_report("remote stub: sendmsg resp failed: %s", strerror(errno));
        }

        unsigned int sgs = sr->iov_cnt - 1;
        if (used_zc) {
            /* release the header, iov and the sent in buffers on the zc
             * completion, not now */
            void **bufs = g_new(void *, 2 + sgs);
            unsigned int n = 0;
            bufs[n++] = sr->header;
            bufs[n++] = sr->iov;
            for (unsigned int i = 0; i < sgs; i++) {
                bufs[n++] = sr->in_bufs[i];
            }
            ZcPending *zp = g_new0(ZcPending, 1);
            zp->serial = st->serial++;
            zp->bufs = bufs;
            zp->n_bufs = n;
            st->pending = g_slist_prepend(st->pending, zp);
        }
        stub_resp_free(sr, sgs, used_zc);
        inflight_clear(resp_win, seq);
    }
    /* the window has space again: wake a push blocked on backpressure (the
     * handle worker waiting on push_done uses the same cond) */
    g_mutex_lock(&ctx->push_lock);
    g_cond_broadcast(&ctx->push_cond);
    g_mutex_unlock(&ctx->push_lock);
out:
    qatomic_set(&ctx->send_busy, 0);
}

/* io_write handler on the send worker's aio context: the socket has send
 * buffer space again, so resume draining the resp_win head. */
static void stub_send_writable(void *opaque)
{
    VirtQueue *vq = opaque;
    RemoteVQueueCtx *ctx = virtqueue_get_remote_ctx(vq);
    Worker *w = stub_worker_for_send(vq);

    if (!ctx) {
        return;
    }
    ctx->send_writable = false;
    aio_set_fd_handler(w->ctx, ctx->resp_fd, NULL, NULL, NULL, NULL, NULL);
    if (qatomic_load_acquire(&ctx->dead)) {
        return;
    }
    stub_dispatch(w, vq, stub_send_task);
}

/*
 * called by virtio.c through virtqueue_push()/virtqueue_fill() when
 * vq->remote_ctx is set (remote stub side). Builds the response
 * [vq_nr(4B)][elem_index(4B)][data_len(4B)][data...], publishes it into the
 * vq's resp_win for the send worker and signals push_done. May run on any
 * thread (device async completions): the resp_win publish is serialized with
 * the teardown by push_lock, and vq-internal serialization keeps the pushes
 * one at a time.
 */
void remote_stub_virtqueue_push(VirtQueue *vq, const VirtQueueElement *elem,
                                unsigned int len)
{
    RemoteVQueueCtx *ctx = virtqueue_get_remote_ctx(vq);
    StubResp *sr = NULL;

    if (ctx && elem->in_num > 0) {
        int *resp_header = g_new0(int, 3);
        resp_header[0] = ctx->vq_nr;
        resp_header[1] = elem->index;
        resp_header[2] = len;

        /* gather the in-buffers, trimming the last one to exactly len */
        unsigned int sgs = 0, cnt = 0;
        while (cnt < len && sgs < elem->in_num) {
            cnt += elem->in_sg[sgs++].iov_len;
        }
        if (cnt < len) {
            /* in-buffers are smaller than the response: sending a truncated
             * resp would desync the byte stream, so drop it */
            error_report("remote stub: resp len %u exceeds in_sg capacity %u",
                         len, cnt);
            g_free(resp_header);
        } else {
            iovec *resp_iov = g_new0(iovec, sgs + 1);
            resp_iov[0].iov_base = resp_header;
            resp_iov[0].iov_len = 3 * sizeof(int);
            memcpy(resp_iov + 1, elem->in_sg, sizeof(iovec) * sgs);
            resp_iov[sgs].iov_len -= (cnt - len);

            sr = g_new0(StubResp, 1);
            sr->header = resp_header;
            sr->iov = resp_iov;
            sr->iov_cnt = sgs + 1;
            sr->len = len;
            sr->n_in_sg = elem->in_num;
            sr->in_bufs = g_new(void *, elem->in_num);
            for (unsigned int i = 0; i < elem->in_num; i++) {
                sr->in_bufs[i] = elem->in_sg[i].iov_base;
            }
        }
    }

    /* the out buffers are always released here; the elem is now complete */
    for (unsigned int i = 0; i < elem->out_num && elem->out_sg[i].iov_base; i++) {
        g_free(elem->out_sg[i].iov_base);
    }
    ctx->elem = NULL;

    /* publish under push_lock: the re-check of dead closes the race with
     * conn_err tearing the windows down (it takes the same lock). */
    g_mutex_lock(&ctx->push_lock);
    if (qatomic_load_acquire(&ctx->dead)) {
        g_mutex_unlock(&ctx->push_lock);
        if (sr) {
            stub_resp_free(sr, sr->iov_cnt - 1, false);
        } else {
            for (unsigned int i = 0; i < elem->in_num && elem->in_sg[i].iov_base; i++) {
                free(elem->in_sg[i].iov_base);
            }
        }
        return;
    }
    if (sr) {
        Inflight *resp_win = ctx->resp_win;
        if (!resp_win) {
            g_mutex_unlock(&ctx->push_lock);
            stub_resp_free(sr, sr->iov_cnt - 1, false);
            return;
        }
        /* backpressure: the window is sized for vring.num in-flight requests
         * (one resp each), so this wait is a pure safety net; the send worker
         * broadcasts push_cond once it drains the window */
        while (resp_win->tail - qatomic_load_acquire(&resp_win->head) >=
               resp_win->size) {
            g_cond_wait(&ctx->push_cond, &ctx->push_lock);
            if (qatomic_load_acquire(&ctx->dead)) {
                g_mutex_unlock(&ctx->push_lock);
                stub_resp_free(sr, sr->iov_cnt - 1, false);
                return;
            }
        }
        inflight_publish(resp_win, resp_win->tail, sr, NULL);
    } else {
        /* no (or dropped) resp: the in buffers are released right away */
        for (unsigned int i = 0; i < elem->in_num && elem->in_sg[i].iov_base; i++) {
            free(elem->in_sg[i].iov_base);
        }
    }
    qatomic_store_release(&ctx->push_done, 1);
    g_cond_signal(&ctx->push_cond);
    g_mutex_unlock(&ctx->push_lock);
}

/*
 * per-connection receive state on the stub side for incremental, non-blocking
 * reads of the req stream
 * [vq_nr(4B)][seq(4B)][out_num(4B)][in_num(4B)]
 * [lens: (out_num+in_num) x 4B][out_sg data...]
 */
typedef struct StubRecvState {
    int stage;                /* 0 = header, 1 = lens, 2 = out data */
    unsigned int hdr_off;     /* header bytes read so far */
    uint8_t hdr[16];          /* [vq_nr][seq][out_num][in_num] */
    unsigned int seq;         /* elem_index echoed back in the resp */
    unsigned int out_num, in_num;
    unsigned int lens_off;    /* lens bytes read so far */
    int *lens;                /* (out_num + in_num) native-endian lens */
    unsigned int out_total;   /* sum of out lens */
    unsigned int data_off;    /* out data bytes read so far */
    struct iovec *out_sg;     /* out_num entries, buffers allocated */
    struct iovec *in_sg;      /* in_num entries, buffers allocated */
} StubRecvState;

/*
 * aio fd handler on the stub side for one vq socket, i.e. the distributor:
 * incrementally receives requests and publishes each complete one into the
 * vq's req_win for the handle worker, which runs the device (pop via
 * remote_stub_virtqueue_pop, reply via remote_stub_virtqueue_push). A bare
 * POLLERR wakeup (MSG_ZEROCOPY completion) carries no request data; it is
 * forwarded to the send worker, the sole owner of the zc state.
 */
static void remote_stub_req_handler(void *opaque)
{
    VirtQueue *vq = opaque;
    RemoteVQueueCtx *ctx = virtqueue_get_remote_ctx(vq);
    int fd = ctx->resp_fd;

    /* a zc completion is signalled as POLLERR and wakes this read handler
     * without any request data. Peek one byte to tell the two apart: a data
     * event is parsed below (its send kick comes from the handle worker after
     * each push), a bare POLLERR event only reaps the zc state, which belongs
     * to the send worker. */
    char probe;
    if (recv(fd, &probe, 1, MSG_PEEK) < 0 &&
        (errno == EAGAIN || errno == EWOULDBLOCK)) {
        if (!qatomic_load_acquire(&ctx->dead)) {
            stub_dispatch(stub_worker_for_send(vq), vq, stub_send_task);
        }
        return;
    }

    if (!stub_recv_states) {
        stub_recv_states = g_hash_table_new(g_direct_hash, g_direct_equal);
    }
    StubRecvState *rs = g_hash_table_lookup(stub_recv_states, GINT_TO_POINTER(fd));
    if (!rs) {
        rs = g_new0(StubRecvState, 1);
        g_hash_table_insert(stub_recv_states, GINT_TO_POINTER(fd), rs);
    }

    while (true) {
        if (rs->stage == 0) {
            /* req header: [vq_nr][seq][out_num][in_num], native endian */
            ssize_t n = recv(fd, rs->hdr + rs->hdr_off,
                             sizeof(rs->hdr) - rs->hdr_off, 0);
            if (n < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    return; /* wait for the next readable event */
                }
                error_report("remote stub: recv req header failed: %s",
                             strerror(errno));
                goto conn_err;
            }
            if (n == 0) {
                error_report("remote stub: local qemu closed vq connection");
                goto conn_err;
            }
            rs->hdr_off += n;
            if (rs->hdr_off < sizeof(rs->hdr)) {
                return; /* header incomplete, wait for more */
            }
            int vq_nr;
            memcpy(&vq_nr, rs->hdr, 4);
            memcpy(&rs->seq, rs->hdr + 4, 4);
            memcpy(&rs->out_num, rs->hdr + 8, 4);
            memcpy(&rs->in_num, rs->hdr + 12, 4);
            if (vq_nr != virtio_get_queue_index(vq)) {
                error_report("remote stub: req vq_nr %d, expected %d",
                             vq_nr, virtio_get_queue_index(vq));
                goto conn_err;
            }
            if (rs->out_num > VIRTQUEUE_MAX_SIZE ||
                rs->in_num > VIRTQUEUE_MAX_SIZE) {
                error_report("remote stub: bogus out_num %u in_num %u",
                             rs->out_num, rs->in_num);
                goto conn_err;
            }
            rs->lens = g_new0(int, rs->out_num + rs->in_num);
            rs->out_sg = g_new0(struct iovec, rs->out_num);
            rs->in_sg = g_new0(struct iovec, rs->in_num);
            rs->stage = 1;
        }

        if (rs->stage == 1) {
            size_t lens_bytes = (rs->out_num + rs->in_num) * sizeof(int);
            ssize_t n = recv(fd, (char *)rs->lens + rs->lens_off,
                             lens_bytes - rs->lens_off, 0);
            if (n < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    return;
                }
                error_report("remote stub: recv req lens failed: %s",
                             strerror(errno));
                goto conn_err;
            }
            if (n == 0) {
                error_report("remote stub: local qemu closed vq mid-lens");
                goto conn_err;
            }
            rs->lens_off += n;
            if (rs->lens_off < lens_bytes) {
                return;
            }
            /* allocate the out/in buffers from the received lens */
            rs->out_total = 0;
            for (unsigned int i = 0; i < rs->out_num; i++) {
                rs->out_sg[i].iov_len = rs->lens[i];
                rs->out_sg[i].iov_base = g_new(char, rs->out_sg[i].iov_len);
                rs->out_total += rs->out_sg[i].iov_len;
            }
            for (unsigned int i = 0; i < rs->in_num; i++) {
                rs->in_sg[i].iov_len = rs->lens[rs->out_num + i];
                /* page-aligned (and page-multiple sized) so the response
                 * send can use MSG_ZEROCOPY and let the NIC DMA straight
                 * from these buffers; freed with free() */
                if (rs->in_sg[i].iov_len == 0) {
                    rs->in_sg[i].iov_base = NULL;
                } else if (posix_memalign(&rs->in_sg[i].iov_base, getpagesize(),
                                          QEMU_ALIGN_UP(rs->in_sg[i].iov_len,
                                                        getpagesize())) != 0) {
                    error_report("remote stub: posix_memalign failed: %s",
                                 strerror(errno));
                    rs->in_sg[i].iov_base = NULL;
                    goto conn_err;
                }
            }
            rs->stage = 2;
        }

        if (rs->stage == 2) {
            if (rs->data_off < rs->out_total) {
                /* find the out_sg iov containing data_off, recv into it */
                unsigned int off = 0, i;
                for (i = 0; i < rs->out_num; i++) {
                    if (rs->data_off < off + rs->out_sg[i].iov_len) {
                        break;
                    }
                    off += rs->out_sg[i].iov_len;
                } 
                unsigned int iov_off = rs->data_off - off;
                ssize_t n = recv(fd, (char *)rs->out_sg[i].iov_base + iov_off,
                                 rs->out_sg[i].iov_len - iov_off, 0);
                if (n < 0) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK) {
                        return;
                    }
                    error_report("remote stub: recv req data failed: %s",
                                 strerror(errno));
                    goto conn_err;
                }
                if (n == 0) {
                    error_report("remote stub: local qemu closed vq mid-data");
                    goto conn_err;
                }
                rs->data_off += n;
                if (rs->data_off < rs->out_total) {
                    continue; /* keep filling the out buffers */
                }
            }

            /* full request received: publish it into the req_win for the
             * handle worker, then try to read the next one */
            Inflight *req_win = ctx->req_win;
            StubReq *req;
            if (qatomic_load_acquire(&ctx->dead) || !req_win) {
                /* the connection is being torn down: drop the request */
                for (unsigned int i = 0; i < rs->out_num; i++) {
                    g_free(rs->out_sg[i].iov_base);
                }
                for (unsigned int i = 0; i < rs->in_num; i++) {
                    free(rs->in_sg[i].iov_base);
                }
                g_free(rs->lens);
                g_free(rs->out_sg);
                g_free(rs->in_sg);
                rs->lens = NULL;
                rs->out_sg = NULL;
                rs->in_sg = NULL;
                rs->stage = 0;
                rs->out_num = rs->in_num = 0;
                return;
            }
            if (req_win->tail - qatomic_load_acquire(&req_win->head) >=
                req_win->size) {
                /* no slot: keep the parsed request in rs and let the
                 * level-triggered epoll re-fire this handler once the handle
                 * worker drains. Sizing keeps this unreachable in practice
                 * (in-flight requests <= vring.num - 1 < window size). */
                return;
            }
            req = g_new0(StubReq, 1);
            req->elem_index = rs->seq;
            req->out_num = rs->out_num;
            req->in_num = rs->in_num;
            req->out_sg = rs->out_sg;   /* ownership transfers to the req */
            req->in_sg = rs->in_sg;
            inflight_publish(req_win, req_win->tail, req, NULL);
            stub_dispatch(stub_worker_for_handle(vq), vq, stub_handle_task);
            /* the sg arrays now belong to the req; reset the parsing state */
            g_free(rs->lens);
            rs->lens = NULL;
            rs->out_sg = NULL;
            rs->in_sg = NULL;
            rs->stage = 0;
            rs->hdr_off = 0;
            rs->lens_off = 0;
            rs->data_off = 0;
            rs->out_total = 0;
            rs->out_num = rs->in_num = 0;
            continue; /* try to read the next request */
        }
    }

conn_err:
    /* the connection is unusable: release partial buffers, then drop it */
    AioContext *ctx_aio = vq_get_aio_ctx(vq);
    if (ctx_aio) {
        aio_set_fd_handler(ctx_aio, fd, NULL, NULL, NULL, NULL, NULL);
    }
    if (rs->lens) {
        g_free(rs->lens);
    }
    if (rs->out_sg) {
        for (unsigned int i = 0; i < rs->out_num && rs->out_sg[i].iov_base; i++) {
            g_free(rs->out_sg[i].iov_base);
        }
        g_free(rs->out_sg);
    }
    if (rs->in_sg) {
        for (unsigned int i = 0; i < rs->in_num && rs->in_sg[i].iov_base; i++) {
            free(rs->in_sg[i].iov_base); /* posix_memalign'd */
        }
        g_free(rs->in_sg);
    }
    g_hash_table_remove(stub_recv_states, GINT_TO_POINTER(fd));
    g_free(rs);
    /* dead + wake + wait the workers, then release windows/zc and close fd */
    stub_teardown_vq(vq);
}

int remote_accept(const char *ip_port, Error **errp)
{
    // parse "ip@port"
    char ip[64];
    int port;
    const char *at_pos = strchr(ip_port, '@');
    if (!at_pos) {
        error_setg(errp, "invalid ip_port format, expected ip@port");
        return -1;
    }
    size_t ip_len = at_pos - ip_port;
    if (ip_len >= sizeof(ip)) {
        error_setg(errp, "ip address too long");
        return -1;
    }
    memcpy(ip, ip_port, ip_len);
    ip[ip_len] = '\0';
    port = atoi(at_pos + 1);

    // create socket with ip and port
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);

    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        error_setg_errno(errp, errno, "failed to create listen socket");
        return -1;
    }
    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        error_setg_errno(errp, errno, "failed to bind %s:%d", ip, port);
        close(listen_fd);
        return -1;
    }
    if (listen(listen_fd, 8) < 0) {
        error_setg_errno(errp, errno, "failed to listen on %s:%d", ip, port);
        close(listen_fd);
        return -1;
    }
    return listen_fd;
}

void remote_accept_handler(void *opaque)
{
    RemoteAccept *sctx = opaque;
    int *vq_listen = NULL;
    uint16_t *dst_ports = NULL;
    char *msg = NULL;
    int vq_nt = 0;
    int ctl_fd = -1;

    /* 1. accept the control connection from local qemu */
    ctl_fd = accept(sctx->listen_fd, NULL, NULL);
    if (ctl_fd < 0) {
        error_report("remote stub: accept control connection failed: %s",
                     strerror(errno));
        goto fail;
    }

    /* 2. read local's negotiation header: [magic:4][vq_nt:4], then the
     *    src_port list [src_port:2*vq_nt]. vq_nt is only known after the
     *    header, so the buffers are sized by vq_nt, not VIRTIO_QUEUE_MAX */
    char hdr[8];
    if (read_all(ctl_fd, hdr, sizeof(hdr)) < 0) {
        goto fail;
    }
    uint32_t magic;
    uint32_t cnt;
    memcpy(&magic, hdr, 4);
    memcpy(&cnt, hdr + 4, 4);
    magic = ntohl(magic);
    cnt = ntohl(cnt);
    if (magic != LOCAL_NEGO_MAGIC || cnt == 0 || cnt > VIRTIO_QUEUE_MAX) {
        error_report("remote stub: bad negotiation header (magic %x, vq %u)",
                     magic, cnt);
        goto fail;
    }
    vq_nt = cnt;
    msg = g_new(char, 8 + vq_nt * 2);
    if (read_all(ctl_fd, msg, vq_nt * 2) < 0) {
        goto fail;
    }
    /* the source ports are kept for identity check if needed later */

    /* 3. allocate vq_nt listening ports on the stub side, reply to local */
    vq_listen = g_new(int, vq_nt);
    dst_ports = g_new(uint16_t, vq_nt);
    for (int i = 0; i < vq_nt; i++) {
        vq_listen[i] = -1;
    }
    int opt = 1;
    for (int i = 0; i < vq_nt; i++) {
        struct sockaddr_in vaddr;
        memset(&vaddr, 0, sizeof(vaddr));
        vaddr.sin_family = AF_INET;
        vaddr.sin_addr.s_addr = htonl(INADDR_ANY);
        vaddr.sin_port = 0; /* kernel picks a free port */
        vq_listen[i] = socket(AF_INET, SOCK_STREAM, 0);
        if (vq_listen[i] < 0 ||
            setsockopt(vq_listen[i], SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0 ||
            bind(vq_listen[i], (struct sockaddr *)&vaddr, sizeof(vaddr)) < 0 ||
            listen(vq_listen[i], 1) < 0) {
            error_report("remote stub: allocate vq listen socket failed: %s",
                         strerror(errno));
            goto fail;
        }
        socklen_t slen = sizeof(vaddr);
        if (getsockname(vq_listen[i], (struct sockaddr *)&vaddr, &slen) < 0) {
            error_report("remote stub: getsockname failed: %s", strerror(errno));
            goto fail;
        }
        dst_ports[i] = ntohs(vaddr.sin_port);
    }

    uint32_t rep_magic = htonl(LOCAL_NEGO_MAGIC);
    uint32_t rep_cnt = htonl(vq_nt);
    memcpy(msg, &rep_magic, 4);
    memcpy(msg + 4, &rep_cnt, 4);
    for (int i = 0; i < vq_nt; i++) {
        uint16_t p = htons(dst_ports[i]);
        memcpy(msg + 8 + i * 2, &p, 2);
    }
    if (write_all(ctl_fd, msg, 8 + vq_nt * 2) < 0) {
        goto fail;
    }

    /* 4. accept the vq connections from local qemu and register them.
     *    local qemu's sockets arrive in its vq enumeration order, so map the
     *    i-th active vq (both sides enumerate by vring.num set at realize) */
    int vq_idx = 0;
    for (int n = 0; n < VIRTIO_QUEUE_MAX; n++) {
        VirtQueue *vq = virtio_get_queue(sctx->vdev, n);
        if (!virtio_queue_get_num(sctx->vdev, n)) {
            continue;
        }
        if (vq_idx >= vq_nt) {
            error_report("remote stub: more active vqs than negotiated");
            goto fail;
        }
        int vq_fd = accept(vq_listen[vq_idx], NULL, NULL);
        close(vq_listen[vq_idx]);
        vq_listen[vq_idx] = -1;
        if (vq_fd < 0) {
            error_report("remote stub: accept vq connection failed: %s",
                         strerror(errno));
            goto fail;
        }
        int flags = fcntl(vq_fd, F_GETFL, 0);
        if (flags >= 0) {
            fcntl(vq_fd, F_SETFL, flags | O_NONBLOCK);
        }
        RemoteVQueueCtx *ctx = virtqueue_get_remote_ctx(vq);
        if (!ctx) {
            ctx = g_new0(RemoteVQueueCtx, 1);
            virtqueue_set_remote_ctx(vq, ctx);
        }
        ctx->resp_fd = vq_fd;
        ctx->vq_nr = n;
        remote_vq_ctx_init(ctx, 0); /* stub: vq_lock + push_lock/cond only */
        stub_ctx_init_windows(ctx, virtio_queue_get_num(sctx->vdev, n));
        ctx->zc = zc_enable(vq_fd);
        /* the handle/send workers must know this vq for their re-scans */
        stub_register_vq(vq);
        aio_set_fd_handler(sctx->aio_ctx, vq_fd,
                           remote_stub_req_handler, NULL, NULL, NULL, vq);
        vq_idx++;
    }
    if (vq_idx != vq_nt) {
        error_report("remote stub: negotiated %d vqs, accepted %d",
                     vq_nt, vq_idx);
        goto fail;
    }

    close(ctl_fd);
    return;

fail:
    if (vq_listen) {
        for (int i = 0; i < vq_nt; i++) {
            if (vq_listen[i] >= 0) {
                close(vq_listen[i]);
            }
        }
    }
    /* roll back the vq connections already registered. stub_teardown_vq
     * parks the workers first (dead + busy-wait), so the ctx can be freed
     * right after; a dispatch still in flight sees remote_ctx == NULL and
     * bails out. */
    for (int n = 0; n < VIRTIO_QUEUE_MAX; n++) {
        VirtQueue *vq = virtio_get_queue(sctx->vdev, n);
        RemoteVQueueCtx *ctx = virtqueue_get_remote_ctx(vq);
        if (ctx && ctx->resp_fd > 0) {
            aio_set_fd_handler(sctx->aio_ctx, ctx->resp_fd,
                               NULL, NULL, NULL, NULL, NULL);
            stub_teardown_vq(vq); /* dead, wake, wait, free windows/zc, close */
            remote_vq_ctx_destroy(ctx);
            virtqueue_set_remote_ctx(vq, NULL);
            g_free(ctx);
        }
    }
    if (ctl_fd >= 0) {
        close(ctl_fd);
    }
}
