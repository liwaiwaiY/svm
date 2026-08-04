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
#include <stdbool.h>

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

static GHashTable *aio_ctxs;

void local_register_aio_ctx(VirtIODevice *vdev, AioContext *aio_ctx)
{
    if (!aio_ctxs)
        aio_ctxs = g_hash_table_new(g_direct_hash, g_direct_equal);
    g_hash_table_insert(aio_ctxs, vdev, aio_ctx);
}

AioContext * local_search_aio_ctx(VirtIODevice *vdev)
{
    return aio_ctxs ? g_hash_table_lookup(aio_ctxs, vdev) : NULL;
}

/* true if any active vq of vdev carries a remote ctx: this process is the
 * remote stub for vdev (the local qemu side is marked by register_mosaic) */
static bool stub_vdev_has_remote_vq(VirtIODevice *vdev)
{
    for (int n = 0; n < VIRTIO_QUEUE_MAX; n++) {
        VirtQueue *vq = virtio_get_queue(vdev, n);
        if (virtio_queue_get_num(vdev, n) && vq->remote_ctx) {
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

/*
 * legacy property "remote-id": keep the device id; per-device remote marking
 * now lives in register_mosaic()
 */
void remote_register_id(Object *obj, const char *id, Error **errp)
{
    if (id) {
        DEVICE(obj)->id = g_strdup(id);
    } else if (!DEVICE(obj)->id) {
        static int remote_seq;
        DEVICE(obj)->id = g_strdup_printf("remote%d", remote_seq++);
    }
}

/*
 * legacy: the stub keeps no guest, so its vq host notifiers are never kicked;
 * the request path is driven entirely by the per-vq TCP sockets
 */
void remote_virtio_register_aio(VirtIODevice *vdev)
{
}

void remote_virtio_queue_host_notifier_read(EventNotifier *n)
{
    event_notifier_test_and_clear(n);
}

void remote_virtio_queue_host_notifier_aio_poll_ready(EventNotifier *n)
{
}

/*
 * called by virtio_device_stop_ioeventfd_impl on the stub side: release the
 * vq host notifiers (never kicked by a guest, but must be cleaned up)
 */
void remote_virtio_device_stop_ioeventfd_impl(VirtIODevice *vdev)
{
    VirtioBusState *qbus = VIRTIO_BUS(qdev_get_parent_bus(DEVICE(vdev)));
    memory_region_transaction_begin();
    for (int n = 0; n < VIRTIO_QUEUE_MAX; n++) {
        if (!virtio_queue_get_num(vdev, n)) {
            continue;
        }
        VirtQueue *vq = virtio_get_queue(vdev, n);
        event_notifier_set_handler(virtqueue_get_host_notifier(vq), NULL);
        virtio_bus_set_host_notifier(qbus, n, false);
    }
    memory_region_transaction_commit();
    for (int n = 0; n < VIRTIO_QUEUE_MAX; n++) {
        if (!virtio_queue_get_num(vdev, n)) {
            continue;
        }
        virtio_bus_cleanup_host_notifier(qbus, n);
    }
}

/* in-flight elems awaiting their responses, keyed by (resp_fd, seq) */
static GHashTable *pending_elems;

/* per-connection incremental receive state, keyed by resp_fd */
static GHashTable *recv_states;

/* stub-side per-connection incremental request receive state, keyed by fd */
static GHashTable *stub_recv_states;

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

/*
 * called by local qemu: negotiate per-vq ports with the remote stub.
 * Only negotiation here: the vq sockets are created and bound to a free
 * local source port, and the stub-side destination addresses are handed
 * out via dst[]. No connection is established yet - the caller connects
 * each socket with local_connect_vq().
 *
 * on success the control connection fd is returned (or -1 on error, errp set)
 */
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
     * may take a moment to bind/listen, so retry a few times. Each attempt is
     * bounded by SO_SNDTIMEO, otherwise an unreachable host would stall the
     * main thread for the whole TCP SYN retry period (minutes).
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

    /*
     * One TCP connection per vq. Here we only negotiate the ports and pin
     * the four-tuple (src_ip, src_port, dst_ip, dst_port) for each vq:
     * 1. create vq_nt sockets and bind them to free local source ports
     *    (kernel picks via port 0) - this fixes the source side.
     * 2. send the source ports to the remote stub over the control fd.
     * 3. receive the stub-side ports; dst[] records the destination side.
     * No connect() happens here: local_connect_vq() establishes the actual
     * connection on these pinned sockets afterwards.
     */
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

/*
 * called by local qemu: establish the connection on a vq socket that was
 * already created and source-pinned during negotiation (local_connect_socket),
 * connecting it to the stub-side address in addr. The fd is reused across
 * retries because recreating it would change the source port and break the
 * negotiated four-tuple.
 */
bool local_connect_vq(int socket, const struct sockaddr_in *addr, Error **errp)
{
    if (connect_with_retry(&socket, addr, false, errp) < 0) {
        return false;
    }
    enable_tcp_keepalive(socket);
    return true;
}

int virtio_device_start_ioeventfd_impl_local(VirtIODevice *vdev, AioContext *ctx)
{
    /* attach every active vq's host notifier (guest kick) to the iothread's
     * aio ctx, so kicks are drained on the iothread instead of the main loop */
    for (int n = 0; n < VIRTIO_QUEUE_MAX; n++) {
        VirtQueue *vq = &vdev->vq[n];
        if (!vq->vring.num) {
            continue;
        }
        aio_set_event_notifier(ctx, &vq->host_notifier,
                               local_host_notifier_read, // read
                               NULL, NULL); // poll, poll_ready
        /*
         * We will have ignored notifications about new requests from the guest
         * while no notifiers were attached, so "kick" the virt queue to process
         * those requests now.
         */
        event_notifier_set(&vq->host_notifier);
    }
    return 0;
}

/* key for an in-flight element: (resp_fd, seq) -> VirtQueueElement */
static gpointer pending_key(int resp_fd, unsigned int seq)
{
    return (gpointer)(uintptr_t)(((guint64)(unsigned)resp_fd << 32) | seq);
}

/*
 * deferred notification: defer_call() coalesces a batch of completed
 * responses into a single virtio_notify per vq per aio event
 */
static void local_virtio_notify(void *opaque)
{
    VirtQueue *vq = opaque;
    virtio_notify(vq->vdev, vq);
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

/*
 * called by local qemu: submit one already-popped element to the stub as
 * soon as possible. Non-blocking sendmsg only - never waits for a response,
 * so the aio loop is never stalled. The element is handed over to the
 * response path which will virtqueue_push() it.
 * Returns false if the socket send buffer is full (caller stops draining).
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

    int *lens = g_new0(int, elem->out_num + elem->in_num);
    for (unsigned int i = 0; i < elem->out_num; i++) {
        lens[i] = elem->out_sg[i].iov_len;
    }
    for (unsigned int i = 0; i < elem->in_num; i++) {
        lens[i + elem->out_num] = elem->in_sg[i].iov_len;
    }

    msg_sg[0].iov_base = header;
    msg_sg[0].iov_len = 4 * sizeof(int);
    msg_sg[1].iov_base = lens;
    msg_sg[1].iov_len = (elem->out_num + elem->in_num) * sizeof(int);
    memcpy(msg_sg + 2, elem->out_sg, elem->out_num * sizeof(iovec));

    struct msghdr msg = {
        .msg_iov = msg_sg,
        .msg_iovlen = elem->out_num + 2,
    };

    ssize_t ret = sendmsg(ctx->resp_fd, &msg, MSG_NOSIGNAL);
    g_free(lens);
    g_free(header);
    g_free(msg_sg);

    if (ret < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            /* send buffer full; stop draining, retry on the next kick.
             * TODO: keep the elem and retry via an io_write handler */
            return false;
        }
        error_report("local qemu: sendmsg to stub failed: %s", strerror(errno));
        return false;
    }

    if (elem->in_num == 0) {
        /* no in-buffers: the stub sends no resp for this request, so
         * complete the used-ring entry right away */
        virtqueue_push(vq, elem, 0);
        defer_call(local_virtio_notify, vq);
        g_free(elem);
        return true;
    }

    /* track the in-flight elem so its response can be matched by seq */
    if (!pending_elems) {
        pending_elems = g_hash_table_new(g_direct_hash, g_direct_equal);
    }
    g_hash_table_insert(pending_elems, pending_key(ctx->resp_fd, seq), elem);
    return true;
}

static void local_handle_output(VirtQueue *vq)
{
    /* drain the vring: pop and submit as many elems as possible without
     * blocking the aio loop */
    while (true) {
        VirtQueueElement *elem = virtqueue_pop(vq, sizeof(VirtQueueElement));
        if (!elem) {
            break;
        }
        if (!local_send_msg(vq, elem)) {
            break;
        }
    }
}

void local_host_notifier_read(EventNotifier *n)
{
    VirtQueue *vq = host_notifier_to_vq(n);
    if (!event_notifier_test_and_clear(n)) {
        return;
    }
    VirtIODevice *vdev = vq->vdev;
    if (unlikely(vdev->broken)) {
        return;
    }
    if (!vq->vring.desc || !vq->handle_output) {
        return;
    }
    local_handle_output(vq);
    if (unlikely(vdev->start_on_kick)) {
        virtio_set_started(vdev, true);
    }
}

void local_response_handler(void *opaque)
{
    VirtQueue *vq = opaque;
    RemoteVQueueCtx *ctx = virtqueue_get_remote_ctx(vq);
    int fd = ctx->resp_fd;

    if (!recv_states) {
        recv_states = g_hash_table_new(g_direct_hash, g_direct_equal);
    }
    LocalRecvState *rs = g_hash_table_lookup(recv_states, GINT_TO_POINTER(fd));
    if (!rs) {
        rs = g_new0(LocalRecvState, 1);
        g_hash_table_insert(recv_states, GINT_TO_POINTER(fd), rs);
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
            rs->cur = g_hash_table_lookup(pending_elems, pending_key(fd, seq));
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
                /* response complete: return the elem to the used ring */
                virtqueue_push(vq, elem, rs->need_len);
                g_hash_table_remove(pending_elems,
                                    pending_key(fd, rs->cur_seq));
                defer_call(local_virtio_notify, vq);
                g_free(elem);
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
    /* the connection is unusable; drop the in-flight elem and reset state */
    if (rs->cur) {
        g_hash_table_remove(pending_elems, pending_key(fd, rs->cur_seq));
        g_free(rs->cur);
        rs->cur = NULL;
    }
    rs->stage = 0;
    rs->hdr_off = 0;
    rs->cur_off = 0;
    rs->need_len = 0;
}

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
 * called by virtio.c through virtqueue_pop() when vq->remote_ctx is set
 * (remote stub side). Reconstructs one VirtQueueElement from the request
 * buffered in the ctx by remote_stub_req_handler. Exactly one element is
 * delivered per received request, so a second pop returns NULL.
 */
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
 * called by virtio.c through virtqueue_push()/virtqueue_fill() when
 * vq->remote_ctx is set (remote stub side). Sends the response back to local
 * qemu as [vq_nr(4B)][elem_index(4B)][data_len(4B)][data...] and releases the
 * out/in buffers that were allocated by remote_stub_req_handler.
 */
void remote_stub_virtqueue_push(VirtQueue *vq, const VirtQueueElement *elem,
                                unsigned int len)
{
    RemoteVQueueCtx *ctx = virtqueue_get_remote_ctx(vq);

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
            goto free_bufs;
        }
        iovec *resp_iov = g_new0(iovec, sgs + 1);
        resp_iov[0].iov_base = resp_header;
        resp_iov[0].iov_len = 3 * sizeof(int);
        memcpy(resp_iov + 1, elem->in_sg, sizeof(iovec) * sgs);
        resp_iov[sgs].iov_len -= (cnt - len);

        struct msghdr msg = {
            .msg_iov = resp_iov,
            .msg_iovlen = sgs + 1,
        };
        ssize_t ret = sendmsg(ctx->resp_fd, &msg, MSG_NOSIGNAL);
        if (ret < 0) {
            error_report("remote stub: sendmsg resp failed: %s", strerror(errno));
        }
        g_free(resp_header);
        g_free(resp_iov);
    }

free_bufs:
    for (unsigned int i = 0; i < elem->out_num && elem->out_sg[i].iov_base; i++) {
        g_free(elem->out_sg[i].iov_base);
    }
    for (unsigned int i = 0; i < elem->in_num && elem->in_sg[i].iov_base; i++) {
        g_free(elem->in_sg[i].iov_base);
    }
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
                rs->in_sg[i].iov_base = g_new(char, rs->in_sg[i].iov_len);
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
            g_free(rs->in_sg[i].iov_base);
        }
        g_free(rs->in_sg);
    }
    g_hash_table_remove(stub_recv_states, GINT_TO_POINTER(fd));
    g_free(rs);
    if (ctx->aio_ctx) {
        aio_set_fd_handler(ctx->aio_ctx, fd, NULL, NULL, NULL, NULL, NULL);
    }
    close(fd);
    ctx->resp_fd = -1;
}

/*
 * called by remote stub: parse ip@port, create a listening socket and return
 * its fd. The actual accept() is done asynchronously in remote_accept_handler
 * once the fd becomes readable (i.e. local qemu connects).
 */
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

/*
 * aio fd handler on the stub side: runs on the iothread when the listen fd
 * becomes readable. Accepts the control connection from local qemu, then
 * negotiates the per-vq ports with it (message format aligned with
 * local_connect_socket), finally accepts and registers the vq connections.
 *
 * Note: this runs once per device during initialization, so the blocking
 * reads/accepts here are acceptable; the data path itself stays async.
 */
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
        ctx->aio_ctx = sctx->aio_ctx;
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
            virtqueue_set_remote_ctx(vq, NULL);
            g_free(ctx);
        }
    }
    if (ctl_fd >= 0) {
        close(ctl_fd);
    }
}
