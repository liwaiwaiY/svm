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

/* -------------- Zero Copy Infra ------------- */

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

typedef struct ZcFdState {
    bool enabled;             /* SO_ZEROCOPY accepted on this socket */
    uint32_t serial;          /* kernel serial expected for the next zc send */
    GSList *pending;          /* ZcPending, matched by serial */
} ZcFdState;

/* per-vq in-flight element record (local side), matched by the resp seq */
typedef struct PendingElem {
    unsigned int seq;
    VirtQueueElement *elem;
} PendingElem;

/*
 * The per-vq state (RemoteVQueueCtx) carries the resp recv state, the
 * in-flight elem queue and the MSG_ZEROCOPY state instead of the old global
 * hash tables: the distributor looks them up once and hands them to the resp
 * worker, so no cross-vq table races exist.
 */

/* look up an in-flight elem by seq; caller holds ctx->lock */
static VirtQueueElement *pending_lookup(RemoteVQueueCtx *ctx, unsigned int seq)
{
    for (GList *it = ctx->pending.head; it; it = it->next) {
        PendingElem *pe = it->data;
        if (pe->seq == seq) {
            return pe->elem;
        }
    }
    return NULL;
}

/* remove an in-flight elem by seq; caller holds ctx->lock */
static void pending_remove(RemoteVQueueCtx *ctx, unsigned int seq)
{
    for (GList *it = ctx->pending.head; it; it = it->next) {
        PendingElem *pe = it->data;
        if (pe->seq == seq) {
            g_queue_delete_link(&ctx->pending, it);
            g_free(pe);
            return;
        }
    }
}

/*
 * Enable MSG_ZEROCOPY on fd and return the per-vq zc state. Not registered in
 * any global table: the state is owned by the vq's RemoteVQueueCtx, so the
 * socket iothread (sender) and the resp worker (zc completions) are the only
 * two owners and are serialized by ctx->lock.
 */
static ZcFdState *zc_enable(int fd)
{
    ZcFdState *st = g_new0(ZcFdState, 1);
    int one = 1;
    st->enabled = setsockopt(fd, SOL_SOCKET, SO_ZEROCOPY, &one, sizeof(one)) == 0;
    return st;
}

static bool zc_pending_has_elem(RemoteVQueueCtx *ctx, VirtQueueElement *elem)
{
    ZcFdState *st = ctx->zc;
    if (!st) {
        return false;
    }
    for (GSList *it = st->pending; it; it = it->next) {
        if (((ZcPending *)it->data)->elem == elem) {
            return true;
        }
    }
    return false;
}

/*
* local and remote use the same function
* caller holds ctx->lock
*/
static bool zc_complete_one(RemoteVQueueCtx *ctx, ZcPending *zp)
{
    // free buffers for the first time
    if (zp->bufs) {
        for (unsigned int i = 0; i < zp->n_bufs; i++) {
            if (zp->bufs[i]) {
                free(zp->bufs[i]);
            }
        }
        g_free(zp->bufs);
        zp->bufs = NULL;
    }

    // local: in-flight elem, successing send, waiting resp
    if (zp->elem && !zp->len_known) {
        return false;
    }

    // local: resp arrives
    if (zp->elem) {
        VirtQueue *vq = zp->vq;
        virtqueue_push(vq, zp->elem, zp->push_len);
        pending_remove(ctx, zp->seq);
        virtio_notify(virtqueue_get_vdev(vq), vq);
        g_free(zp->elem);
    }

    // local, remote: free zp
    g_free(zp);
    return true;
}

/*
 * drain the socket error queue: each entry is a completion notification for
 * the zc sends whose serial is in [ee_info, ee_data] (inclusive, may wrap)
 */
static void zc_drain(RemoteVQueueCtx *ctx)
{
    ZcFdState *st = ctx->zc;
    if (!st) {
        return;
    }
    g_mutex_lock(&ctx->lock);
    char ctrl[CMSG_SPACE(sizeof(struct sock_extended_err)) * 16];
    char data[128];
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
                    /* delete from the list, but only keep it deleted if
                     * zc_complete_one consumed zp; when it returns false the
                     * local-side response is still in flight and the response
                     * handler needs to find zp to finish the push */
                    st->pending = g_slist_delete_link(st->pending, it);
                    if (!zc_complete_one(ctx, zp)) {
                        st->pending = g_slist_prepend(st->pending, zp);
                    }
                }
                it = next;
            }
        }
    }
    g_mutex_unlock(&ctx->lock);
}

/* connection torn down: release every zc send still in flight */
static void zc_fd_teardown(RemoteVQueueCtx *ctx)
{
    ZcFdState *st = ctx->zc;
    if (!st) {
        return;
    }
    g_mutex_lock(&ctx->lock);
    GSList *list = st->pending;
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
        if (zp->elem) {
            pending_remove(ctx, zp->seq);
            g_free(zp->elem);
        }
        g_free(zp);
    }
    g_slist_free(list);
    g_mutex_unlock(&ctx->lock);
    g_free(st);
    ctx->zc = NULL;
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
 * Shared kick/resp dispatcher core (defined with the worker pool below).
 * Hands a vq task to the single worker that owns the vq; returns false if the
 * worker is busy (the caller must then leave the event source untouched so the
 * level-triggered epoll re-arms it).
 */
static bool dispatcher_enqueue(VirtQueue *vq, void (*fn)(void *opaque));

/* worker task: retry the parked elem, then pop and send the vring */
static void local_worker_output(void *opaque);

/*
* called by io_write event, registered in local_send_msg
* when the kernel refuses to send because of lack of buffers, this handler is
* invoked once the socket becomes writable. The vring must only be touched by
* the vq's worker thread, so this iothread-side handler only hands the vq back
* to its worker (which retries the parked elem and drains the vring inside
* local_handle_output). If the worker is busy the io_write handler is kept, so
* the level-triggered G_IO_OUT re-fires once the socket is writable again.
*/
static void local_retry_send(void *opaque)
{
    VirtQueue *vq = opaque;
    RemoteVQueueCtx *ctx = virtqueue_get_remote_ctx(vq);

    if (!dispatcher_enqueue(vq, local_worker_output)) {
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
* called by local qemu local_handle_output()
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
         * the push is deferred until the completion (see zc_complete_one /
         * the response handler), which is exactly the ack from the stub.
         */
        void **bufs = g_new(void *, 3);
        bufs[0] = header;
        bufs[1] = lens;
        bufs[2] = msg_sg;
        ZcPending *zp = g_new0(ZcPending, 1);
        zp->bufs = bufs;
        zp->n_bufs = 3;
        zp->elem = elem;
        zp->vq = vq;
        zp->seq = seq;
        if (elem->in_num == 0) {
            /* no response comes back: push the used ring when the NIC
             * completion arrives */
            zp->len_known = true;
            zp->push_len = 0;
        }
        g_mutex_lock(&ctx->lock);
        zp->serial = st->serial++;
        st->pending = g_slist_prepend(st->pending, zp);
        if (elem->in_num > 0) {
            /* track the in-flight elem so its response can be matched by seq */
            PendingElem *pe = g_new0(PendingElem, 1);
            pe->seq = seq;
            pe->elem = elem;
            g_queue_push_tail(&ctx->pending, pe);
        }
        g_mutex_unlock(&ctx->lock);
        return true;
    }

    g_free(lens);
    g_free(header);
    g_free(msg_sg);

    if (ret < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            /* send buffer full: hang the elem on the vq and retry when the
             * socket becomes writable, instead of dropping the request. The
             * aio loop keeps running; only this vq's sending is stalled. */
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
        virtqueue_push(vq, elem, 0);
        virtio_notify(virtqueue_get_vdev(vq), vq);
        g_free(elem);
        return true;
    }

    /* track the in-flight elem so its response can be matched by seq */
    g_mutex_lock(&ctx->lock);
    PendingElem *pe = g_new0(PendingElem, 1);
    pe->seq = seq;
    pe->elem = elem;
    g_queue_push_tail(&ctx->pending, pe);
    g_mutex_unlock(&ctx->lock);
    return true;
}

/* -------------- Local QEMU Virtio Interception ------------- */

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

void local_distributor(EventNotifier *n)
{
    
}

void local_handle_output(VirtQueue *vq, RemoteVQueueCtx *ctx)
{
    if (!ctx) {
        return;
    }
    /* an elem is parked on the socket being full: retry it before popping
     * more, otherwise the vring would be drained past the parked elem */
    if (ctx->pending_elem) {
        VirtQueueElement *elem = ctx->pending_elem;
        ctx->pending_elem = NULL;
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
        VirtQueueElement *elem = virtqueue_pop(vq, sizeof(VirtQueueElement));
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

void local_response_handler(void *opaque)
{
    VirtQueue *vq = opaque;
    RemoteVQueueCtx *ctx = virtqueue_get_remote_ctx(vq);
    int fd = ctx->resp_fd;

    /* drain MSG_ZEROCOPY completions: each one may release a deferred
     * used-ring push (POLLERR on this fd wakes this handler) */
    zc_drain(ctx);

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
            g_mutex_lock(&ctx->lock);
            rs->cur = pending_lookup(ctx, seq);
            g_mutex_unlock(&ctx->lock);
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
                ZcFdState *st = ctx->zc;
                ZcPending *zp = NULL;
                if (st) { // fd has used zc
                    /* the socket iothread may concurrently prepend a new zc
                     * send, so the pending list walk must take ctx->lock */
                    g_mutex_lock(&ctx->lock);
                    for (GSList *it = st->pending; it; it = it->next) {
                        ZcPending *p = it->data;
                        if (p->elem == elem) {
                            zp = p;
                            break;
                        }
                    }
                    g_mutex_unlock(&ctx->lock);
                }
                if (zp) { // this elem sused zc
                    zp->push_len = rs->need_len;
                    zp->len_known = true;
                    if (!zp->bufs) { // zc has completed by zc_complete_one()
                        g_mutex_lock(&ctx->lock);
                        st->pending = g_slist_remove(st->pending, zp);
                        pending_remove(ctx, rs->cur_seq);
                        g_mutex_unlock(&ctx->lock);
                        virtqueue_push(vq, elem, rs->need_len);
                        virtio_notify(virtqueue_get_vdev(vq), vq);
                        g_free(elem);
                        g_free(zp);
                    } /* else {} wait zc_complete_one() to handle */
                } else { // this elem is sent by copy
                    g_mutex_lock(&ctx->lock);
                    pending_remove(ctx, rs->cur_seq);
                    g_mutex_unlock(&ctx->lock);
                    virtqueue_push(vq, elem, rs->need_len);
                    virtio_notify(virtqueue_get_vdev(vq), vq);
                    g_free(elem);
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
     * zc_fd_teardown() owns (frees) every elem still tracked by the zc
     * layer, so only free rs->cur here if it is not one of them. */
    bool cur_is_zc = rs->cur && zc_pending_has_elem(ctx, rs->cur);
    zc_fd_teardown(ctx);
    if (rs->cur) {
        if (!cur_is_zc) {
            g_mutex_lock(&ctx->lock);
            pending_remove(ctx, rs->cur_seq);
            g_mutex_unlock(&ctx->lock);
            g_free(rs->cur);
        }
        rs->cur = NULL;
    }
    /* a send-retry elem never made it onto the wire; release it too */
    if (ctx->pending_elem) {
        g_free(ctx->pending_elem);
        ctx->pending_elem = NULL;
    }
    rs->stage = 0;
    rs->hdr_off = 0;
    rs->cur_off = 0;
    rs->need_len = 0;
}

/* -------------- Local QEMU Resp Distributor ------------- */

/*
 * Internal per-vq processing thread pool, unrelated to the device IOThreads.
 * Both event sources of a vq - the host-notifier kick (local_notifier_
 * distributor) and the resp socket (local_response_distributor) - are hashed
 * onto the SAME worker (vq_nr % VIRTIO_REMOTE_RESP_WORKERS) by one shared
 * dispatcher core, so one vq is always processed by exactly one worker thread
 * and its virtqueue_pop()/virtqueue_push() can never run concurrently. The
 * worker runs the handed-over task (local_handle_output for a kick,
 * local_response_handler for a resp) on its own thread. One worker handles one
 * task at a time: when the worker is busy the distributor skips the event and
 * the level-triggered epoll re-arms it on a later round. The per-vq state in
 * RemoteVQueueCtx is therefore only touched by this one worker thread, plus
 * the distributor thread for the handoff (guarded by the busy claim).
 */
typedef struct RemoteWorkerTask {
    VirtQueue *vq;            /* the vq to process (written by distributor) */
    void (*fn)(void *opaque); /* task to run on the worker for this event */
} RemoteWorkerTask;

typedef struct RemoteRespWorker {
    AioContext *ctx;          /* this worker's event loop */
    QEMUBH *bh;               /* distributor -> worker handoff */
    QemuThread thread;
    RemoteWorkerTask task;    /* handoff, written by distributor, read by BH */
    int busy;                 /* 1 while a task is running (qatomic) */
} RemoteRespWorker;

static RemoteRespWorker *resp_workers;
static GMutex resp_workers_lock;
static bool resp_workers_started;

static void resp_worker_bh(void *opaque);

static void *resp_worker_thread(void *opaque)
{
    RemoteRespWorker *w = opaque;

    rcu_register_thread();
    qemu_set_current_aio_context(w->ctx);
    while (true) {
        aio_poll(w->ctx, true);
    }
    return NULL;
}

static void resp_workers_ensure(void)
{
    g_mutex_lock(&resp_workers_lock);
    if (!resp_workers_started) {
        resp_workers = g_new0(RemoteRespWorker, VIRTIO_REMOTE_RESP_WORKERS);
        for (int i = 0; i < VIRTIO_REMOTE_RESP_WORKERS; i++) {
            RemoteRespWorker *w = &resp_workers[i];
            w->ctx = aio_context_new(&error_abort);
            aio_context_set_poll_params(w->ctx, 0, 0, 0, &error_abort);
            w->busy = 0;
            w->bh = aio_bh_new(w->ctx, resp_worker_bh, w);
            qemu_thread_create(&w->thread, "virtio-remote-resp",
                               resp_worker_thread, w, QEMU_THREAD_JOINABLE);
        }
        resp_workers_started = true;
    }
    g_mutex_unlock(&resp_workers_lock);
}

static RemoteRespWorker *resp_worker_for_vq(VirtQueue *vq)
{
    resp_workers_ensure();
    return &resp_workers[virtio_get_queue_index(vq) % VIRTIO_REMOTE_RESP_WORKERS];
}

/*
 * Runs on a worker thread. Executes the task handed over by the distributor
 * (response processing or vq output draining), then releases the worker for
 * the next event.
 */
static void resp_worker_bh(void *opaque)
{
    RemoteRespWorker *w = opaque;

    w->task.fn(w->task.vq);
    qatomic_set(&w->busy, 0);
}

/*
 * Shared distributor core for both event sources of a vq: the host-notifier
 * kick and the resp socket. Resolves the vq's worker with the same hash for
 * both sources, claims it, and schedules the task on its thread. Returns false
 * when the worker is already busy: the caller must then leave the event source
 * untouched (eventfd not cleared / socket not drained), so the level-triggered
 * epoll re-arms it on a later round. Runs on the socket iothread.
 */
static bool dispatcher_enqueue(VirtQueue *vq, void (*fn)(void *opaque))
{
    RemoteVQueueCtx *ctx = virtqueue_get_remote_ctx(vq);
    RemoteRespWorker *w;

    if (!ctx) {
        return false;
    }
    w = resp_worker_for_vq(vq);
    if (qatomic_cmpxchg(&w->busy, 0, 1) != 0) {
        return false; /* worker busy: skip this event, it will be re-armed */
    }
    w->task.vq = vq;
    w->task.fn = fn;
    qemu_bh_schedule(w->bh);
    return true;
}

/*
 * Worker task of the kick path. The kick eventfd is only cleared here, at the
 * start of the drain that consumes the kicked requests. Clearing it on the
 * iothread would race with a kick arriving after this drain already popped the
 * ring: the clear would consume that kick while its vring entries are left
 * unpopped and the guest (waiting for the completion) kicks no more. Any kick
 * that reaches the eventfd after this clear leaves the counter non-zero, so
 * the level-triggered epoll re-fires and the vq is drained again.
 */
static void local_worker_output(void *opaque)
{
    VirtQueue *vq = opaque;

    event_notifier_test_and_clear(virtqueue_get_host_notifier(vq));
    local_handle_output(vq, virtqueue_get_remote_ctx(vq));
}

/*
 * io_read handler of every host notifier (mosaic devices, runs on the device
 * iothread). A guest kick must pop the vring on the vq's worker thread, so the
 * kick is handed to the same worker that owns the resp socket of this vq -
 * never processed inline here. The eventfd is left untouched: it is cleared by
 * the worker task that drains the vring, so a kick that arrives while the
 * worker is busy is re-armed by the level-triggered epoll instead of being
 * dropped.
 */
void local_notifier_distributor(EventNotifier *n)
{
    VirtQueue *vq = host_notifier_to_vq(n);

    dispatcher_enqueue(vq, local_worker_output);
}

/*
 * io_read handler of every local resp fd. Instead of processing the response
 * inline, it hands the work to the worker that owns the fd's vq: the response
 * bytes are parsed (and the used ring pushed) on the worker's thread, so
 * several vqs can progress in parallel. A busy worker is skipped - the fd
 * stays level-triggered and the next epoll round retries.
 */
void local_response_distributor(void *opaque)
{
    VirtQueue *vq = opaque;

    dispatcher_enqueue(vq, local_response_handler);
}

// to review
void remote_virtio_queue_host_notifier_read(EventNotifier *n)
{
    VirtQueue *vq = host_notifier_to_vq(n);
    if (event_notifier_test_and_clear(n)) {
        /* stub side: request processing is driven by the vq TCP socket
         * (remote_stub_req_handler), so a host-notifier kick has no guest
         * kick to forward; just drain the eventfd */
        (void)vq;
    }
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

void *remote_stub_virtqueue_pop(VirtQueue *vq, size_t sz)
{
    RemoteVQueueCtx *ctx = virtqueue_get_remote_ctx(vq);
    if (!ctx || ctx->elem) {
        return NULL;
    }

    int out_num = ctx->out_num, in_num = ctx->in_num;
    VirtQueueElement *ret = remote_stub_virtqueue_alloc_element(sz, out_num, in_num);
    ret->index = ctx->elem_index;
    ret->ndescs = 1;
    ret->in_order_filled = false;
    ret->len = 0;
    for (int i = 0; i < out_num; i++) {
        ret->out_addr[i] = 0;
        ret->out_sg[i] = ctx->out_sg[i];
    }
    for (int i = 0; i < in_num; i++) {
        ret->in_addr[i] = 0;
        ret->in_sg[i] = ctx->in_sg[i];
    }
    ctx->elem = (void *)ret;
    return ret;
}

/*
 * a response queued on the stub side. The header + iov describe the resp
 * [vq_nr(4B)][elem_index(4B)][data_len(4B)][data...]; the in_sg payload
 * buffers are owned here until the send (or the zc completion) happens.
 */
typedef struct StubResp {
    int *header;          /* resp header, owned until sent */
    struct iovec *iov;    /* resp_iov: [header, in_sg...], owned until sent */
    int iov_cnt;
    unsigned int len;     /* resp payload length (zc threshold) */
    void **in_bufs;       /* bases of the in_sg payload buffers */
    unsigned int n_in_sg;
} StubResp;

/*
 * Per-vq response send queue: the single cross-thread structure between the
 * device completion thread(s) (producer) and the socket iothread (consumer).
 * remote_stub_virtqueue_push() only enqueues + schedules the drain BH; every
 * sendmsg()/MSG_ZEROCOPY bookkeeping and the deferred retry happen in
 * stub_drain_send() on the socket iothread, so the rest of the stub-side
 * state stays single-threaded no matter where the device completes.
 */
typedef struct StubSendQueue {
    GMutex lock;              /* protects resp_q / bh_scheduled */
    GQueue resp_q;            /* queued StubResp, drained by the iothread */
    QEMUBH *bh;               /* drain BH running on the socket iothread */
    bool bh_scheduled;        /* a drain BH is already pending */
    bool writable_handler;    /* io_write handler registered (socket full) */
} StubSendQueue;

/* retry the queue head once the socket is writable again */
static void remote_stub_retry_send(void *opaque);

/* stub-side request receiver; forward decl (defined after this function) */
static void remote_stub_req_handler(void *opaque);

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

/* drop every still-queued response and the send queue itself */
static void stub_send_queue_destroy(StubSendQueue *q)
{
    StubResp *sr;
    g_mutex_lock(&q->lock);
    while ((sr = g_queue_pop_head(&q->resp_q))) {
        g_mutex_unlock(&q->lock);
        stub_resp_free(sr, sr->iov_cnt - 1, false);
        g_mutex_lock(&q->lock);
    }
    g_mutex_unlock(&q->lock);
    qemu_bh_delete(q->bh);
    g_mutex_clear(&q->lock);
    g_free(q);
}

/*
 * Send every queued response. Runs on the socket iothread (from the drain BH
 * or the writable handler), which is the single owner of the send path: the
 * zc serial/pending list and the EAGAIN retry all live here, so they never
 * race with remote_stub_virtqueue_push().
 * Returns false if the socket is full and the head is parked on a writable
 * event; true once the queue is empty.
 */
static bool stub_drain_send(VirtQueue *vq)
{
    RemoteVQueueCtx *ctx = virtqueue_get_remote_ctx(vq);
    StubSendQueue *q = ctx->send_q;
    AioContext *ctx_aio = vq_get_aio_ctx(vq);
    int fd = ctx->resp_fd;

    for (;;) {
        g_mutex_lock(&q->lock);
        StubResp *sr = g_queue_peek_head(&q->resp_q);
        g_mutex_unlock(&q->lock);
        if (!sr) {
            return true;
        }
        if (q->writable_handler) {
            /* socket still full: the writable handler retries the head */
            return false;
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
            /* send buffer full: keep sr at the head and retry on writable.
             * retry goes in the io_write slot: with polling disabled on the
             * stub iothread an io_poll_ready handler would never fire. */
            q->writable_handler = true;
            if (ctx_aio) {
                aio_set_fd_handler(ctx_aio, fd,
                                   remote_stub_req_handler,
                                   remote_stub_retry_send, NULL, NULL, vq);
            }
            return false;
        }
        if (ret < 0) {
            error_report("remote stub: sendmsg resp failed: %s", strerror(errno));
        }

        g_mutex_lock(&q->lock);
        g_queue_pop_head(&q->resp_q);
        g_mutex_unlock(&q->lock);

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
    }
}

/* drain BH: scheduled by remote_stub_virtqueue_push() from any thread */
static void stub_drain_send_bh(void *opaque)
{
    VirtQueue *vq = opaque;
    RemoteVQueueCtx *ctx = virtqueue_get_remote_ctx(vq);
    StubSendQueue *q = ctx->send_q;

    if (!q) {
        /* the connection was torn down before this BH ran */
        return;
    }
    g_mutex_lock(&q->lock);
    q->bh_scheduled = false;
    g_mutex_unlock(&q->lock);

    stub_drain_send(vq);
}

/*
 * called by virtio.c through virtqueue_push()/virtqueue_fill() when
 * vq->remote_ctx is set (remote stub side). Sends the response back to local
 * qemu as [vq_nr(4B)][elem_index(4B)][data_len(4B)][data...] and releases the
 * out/in buffers that were allocated by remote_stub_req_handler.
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

    /* the out buffers are always released here */
    for (unsigned int i = 0; i < elem->out_num && elem->out_sg[i].iov_base; i++) {
        g_free(elem->out_sg[i].iov_base);
    }
    if (!sr) {
        /* no (or dropped) resp: the in buffers are released right away */
        for (unsigned int i = 0; i < elem->in_num && elem->in_sg[i].iov_base; i++) {
            free(elem->in_sg[i].iov_base);
        }
        return;
    }

    /*
     * Hand the resp to the socket iothread: this thread only enqueues and
     * schedules the drain BH. The actual sendmsg()/MSG_ZEROCOPY bookkeeping
     * and the EAGAIN retry happen in stub_drain_send() on the iothread, so
     * those structures stay single-owner no matter which thread completed
     * this elem.
     */
    StubSendQueue *q = ctx->send_q;
    if (!q) {
        /* the connection was already torn down: drop the resp */
        stub_resp_free(sr, sr->iov_cnt - 1, false);
        return;
    }
    g_mutex_lock(&q->lock);
    bool was_empty = g_queue_is_empty(&q->resp_q);
    g_queue_push_tail(&q->resp_q, sr);
    if (was_empty && !q->bh_scheduled) {
        q->bh_scheduled = true;
        qemu_bh_schedule(q->bh);
    }
    g_mutex_unlock(&q->lock);
}

/* io_write handler: the socket has send buffer space again, so retry the
 * stalled head of the send queue. Once the queue drains, the write handler
 * is dropped and request delivery resumes. */
static void remote_stub_retry_send(void *opaque)
{
    VirtQueue *vq = opaque;
    RemoteVQueueCtx *ctx = virtqueue_get_remote_ctx(vq);
    StubSendQueue *q = ctx->send_q;
    AioContext *ctx_aio = vq_get_aio_ctx(vq);
    if (!q) {
        return;
    }

    q->writable_handler = false;
    if (ctx_aio) {
        aio_set_fd_handler(ctx_aio, ctx->resp_fd,
                           remote_stub_req_handler, NULL, NULL, NULL, vq);
    }
    stub_drain_send(vq);
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
 * aio fd handler on the stub side for one vq socket. Incrementally receives
 * one request, then hands it to the device through the vq's RemoteVQueueCtx;
 * the device drains it with virtqueue_pop() (remote_stub_virtqueue_pop) and
 * replies via virtqueue_push() (remote_stub_virtqueue_push).
 */
static void remote_stub_req_handler(void *opaque)
{
    VirtQueue *vq = opaque;
    RemoteVQueueCtx *ctx = virtqueue_get_remote_ctx(vq);
    int fd = ctx->resp_fd;

    /* drain MSG_ZEROCOPY completions so deferred response buffers can be
     * released (POLLERR on this fd wakes this handler) */
    zc_drain(ctx);

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

            /* full request received: deliver it to the device */
            if (ctx->send_q && ctx->send_q->writable_handler) {
                /* the socket is still full: the drain is parked on a writable
                 * event. Deferring delivery keeps the queue from growing
                 * unboundedly; remote_stub_retry_send resumes processing
                 * once the socket drains. */
                return;
            }
            ctx->elem_index = rs->seq;
            ctx->out_num = rs->out_num;
            ctx->in_num = rs->in_num;
            ctx->out_sg = rs->out_sg;
            ctx->in_sg = rs->in_sg;
            virtqueue_call_handle_output(vq);
            /* pop()/push() consumed the sg buffers; the iovec arrays are ours */
            ctx->elem = NULL;
            ctx->out_sg = NULL;
            ctx->in_sg = NULL;
            g_free(rs->lens);
            g_free(rs->out_sg);
            g_free(rs->in_sg);
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
    zc_fd_teardown(ctx);
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
    /* drop every response still queued for the dead connection */
    if (ctx->send_q) {
        stub_send_queue_destroy(ctx->send_q);
        ctx->send_q = NULL;
    }
    g_hash_table_remove(stub_recv_states, GINT_TO_POINTER(fd));
    g_free(rs);
    AioContext *ctx_aio = vq_get_aio_ctx(vq);
    if (ctx_aio) {
        aio_set_fd_handler(ctx_aio, fd, NULL, NULL, NULL, NULL, NULL);
    }
    close(fd);
    ctx->resp_fd = -1;
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
        g_mutex_init(&ctx->lock);
        g_queue_init(&ctx->pending);
        ctx->zc = zc_enable(vq_fd);
        /* send queue drained on this iothread via the drain BH */
        ctx->send_q = g_new0(StubSendQueue, 1);
        g_mutex_init(&ctx->send_q->lock);
        g_queue_init(&ctx->send_q->resp_q);
        ctx->send_q->bh = aio_bh_new(sctx->aio_ctx, stub_drain_send_bh, vq);
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
    /* roll back the vq connections already registered */
    for (int n = 0; n < VIRTIO_QUEUE_MAX; n++) {
        VirtQueue *vq = virtio_get_queue(sctx->vdev, n);
        RemoteVQueueCtx *ctx = virtqueue_get_remote_ctx(vq);
        if (ctx && ctx->resp_fd > 0) {
            aio_set_fd_handler(sctx->aio_ctx, ctx->resp_fd,
                               NULL, NULL, NULL, NULL, NULL);
            close(ctx->resp_fd);
            if (ctx->send_q) {
                stub_send_queue_destroy(ctx->send_q);
                ctx->send_q = NULL;
            }
            if (ctx->zc) {
                g_free(ctx->zc);
            }
            g_mutex_clear(&ctx->lock);
            virtqueue_set_remote_ctx(vq, NULL);
            g_free(ctx);
        }
    }
    if (ctl_fd >= 0) {
        close(ctl_fd);
    }
}
