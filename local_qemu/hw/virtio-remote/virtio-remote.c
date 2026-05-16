// copy from virtio.c
#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qapi/qapi-commands-virtio.h"
// #include "trace.h"
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
// #include "virtio-qmp.h"

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

// cmsvm
#include "hw/virtio-remote/virtio-remote.h"
#include <sys/socket.h>
#include <sys/uio.h>
#include <poll.h>
#include <liburing.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdatomic.h>

/*
*  Transport Protocol:
*  req (local->remote): [vq_nr(4B), elem_index(4B), out_len(4B), in_len(4B), out_sg_data...]
*  resp (remote->local): [vq_nr(4B), elem_index(4B), data_len(4B), data...]
*/

#define IO_URING_DEPTH 32 // maximum concurrent reqs
#define RING_SIZE IO_URING_DEPTH

__attribute__((format(printf, 1, 2)))
void force_printf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
    fflush(stdout);
}

/*
*  format: <K:vdev->name, V:int>
*  local_qemu: a link head of sockets of each vq
*  remote_stub: a socket of listening
*/
GHashTable *gsi_stubs = NULL;

/*
*  format: <K:vdev->name, V:CommCTX*>
*  local_qemu: a hash table of sent elements
*  remote_qemu: none
*/
// 
GHashTable *gsi_ctxes = NULL;

/*
*  format: <K:vdev->name, V:Bool>
*  local_qemu: aio list
*  remote_stub: none
*/
GHashTable *set_aio = NULL;

static struct io_uring remote_uring_data;
static struct io_uring *remote_uring = &remote_uring_data;

/*
*  decoupling I/O with strong ordering
*/
typedef struct CommCTX {
    bool sending;
    bool recving;
    int sent;
    int recved;
    int notified;
    sem_t sem1;
    sem_t sem2;
    VirtQueueElement **ring;
} CommCTX;

bool check_virtio_device_remote(VirtIODevice *vdev)
{
    if (!gsi_stubs)
        return false;
    return g_hash_table_contains(gsi_stubs, vdev->name);
}

bool check_origin_qemu_in_iothread(VirtIODevice *vdev)
{
    if (!set_aio)
        return false;
    return g_hash_table_contains(set_aio, vdev->name);
}

void remote_virtio_register_aio(VirtIODevice *vdev)
{
    if (!set_aio)
        set_aio = g_hash_table_new(g_str_hash, g_str_equal);
    g_hash_table_insert(set_aio, (gpointer)(vdev->name), GINT_TO_POINTER(0));
}

static VirtQueue *lookup_vq(VirtIODevice *vdev, int vq_nr)
{
    if (vq_nr < 0 || vq_nr >= VIRTIO_QUEUE_MAX) {
        return NULL;
    }
    return virtio_get_queue(vdev, vq_nr);
}

int remote_uring_init(bool remote_stub)
{
    force_printf("[remote_uring_init] for %s", remote_stub ? "remote_stub" : "local_qemu");

    if (!remote_stub) { // local_qemu
        gsi_ctxes = g_hash_table_new(g_str_hash, g_str_equal);
    }
    gsi_stubs = g_hash_table_new(g_str_hash, g_str_equal);
    int ret = io_uring_queue_init(IO_URING_DEPTH, remote_uring, 0);
    if (ret < 0) {
        fprintf(stderr, "io_uring init failed\n");
        return -1;
    }
    return 0;
}

// socket reconnect
static int reconnect_tcp_socket(int fd)
{
    // cmsvmTODO v2
    return 0;
}

// enalbe socket aliveness in kernel
static int enable_tcp_keepalive(int fd)
{
    // need kernel to keep socket alive (this will not effect cqe&sqe)
    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &opt, sizeof(opt));
    // set idle limit (seconds)
    int keep_idle = 30;
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &keep_idle, sizeof(keep_idle));
    // set options for heartbeat packet (seconds)
    int keep_intvl = 5;
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &keep_intvl, sizeof(keep_intvl));
    // set options for retring (seconds)
    int keep_cnt = 5;
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &keep_cnt, sizeof(keep_cnt));
    return 0;
}

static void *remote_stub_virtqueue_alloc_element(size_t sz, unsigned out_num, unsigned in_num)
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
    // trace_virtqueue_alloc_element(elem, sz, in_num, out_num);
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
    if (ctx->elem) // handled once
        return NULL;

    // int out_num = ctx->out_num, in_num = ctx->in_num;
    int out_num = 1, in_num = 1; // todocmsvm v2: add sg_table
    int i = 0;
    VirtQueueElement *ret = remote_stub_virtqueue_alloc_element(sz, out_num, in_num);
    ret->index = ctx->elem_index;
    ret->ndescs = 1;
    ret->in_order_filled = false;
    ret->len = 0;
    // out_addr and in_addr fields are filled to prevent escape
    // need to handle in migration
    for (i = 0; i < out_num; i++) {
        ret->out_addr[i] = 0;
        ret->out_sg[i] = ctx->out_sg[i];
    }
    for (i = 0; i < in_num; i++) {
        ret->out_addr[i] = 0;
        ret->in_sg[i] = ctx->in_sg[i];
    }
    // record element
    ctx->elem = (void *)ret;
    return ret;
}

/*
*  need to send resp back as [vq_nr, index, in_len, in_data]
*/
void remote_stub_virtqueue_push(VirtQueue *vq, const VirtQueueElement *elem, unsigned int len)
{
    force_printf("[remote_stub_virtqueue_push] send resp for vdev %s", virtqueue_get_vdev_name(vq));
    if (len == 0)
        return;

    RemoteVQueueCtx *ctx = virtqueue_get_remote_ctx(vq);
    struct io_uring_sqe *sqe;
    struct io_uring_cqe *cqe;
    int resp_header[3];

    resp_header[0] = ctx->vq_nr;
    resp_header[1] = ctx->elem_index;
    resp_header[2] = len;

    struct iovec resp_iov[2] = {
        { .iov_base = resp_header,     .iov_len = sizeof(resp_header) },
        { .iov_base = ctx->in_buf,      .iov_len = len },
    };
    struct msghdr msg = {
        .msg_iov = resp_iov,
        .msg_iovlen = 2,
    };

    sqe = io_uring_get_sqe(remote_uring);
    io_uring_prep_sendmsg_zc(sqe, ctx->resp_fd, &msg, 0);
    sqe->ioprio |= IORING_SEND_ZC_REPORT_USAGE;
    io_uring_submit(remote_uring);

    io_uring_wait_cqe(remote_uring, &cqe);
    io_uring_cqe_seen(remote_uring, cqe);

    if (cqe->flags & IORING_CQE_F_MORE) {
        io_uring_wait_cqe(remote_uring, &cqe);
        io_uring_cqe_seen(remote_uring, cqe);
    }
}

bool remote_virtio_notify_skip(VirtIODevice *vdev)
{
    int n;
    for (n = 0; n < VIRTIO_QUEUE_MAX; n++) {
        if (!virtio_queue_get_num(vdev, n)) {
            continue;
        }
        if (virtqueue_get_remote_ctx(virtio_get_queue(vdev, n))) {
            return true;
        }
    }
    return false;
}

void init_remote_virtio_device_sockets(VirtIODevice *vdev, const char *ip_port, Error **errp)
{
    force_printf("[init_remote_virtio_device_sockets] for vdev %s to connect %s", vdev->name, ip_port);

    // get ip and port
    char ip[64];
    int port;
    const char *at_pos = strchr(ip_port, '@');
    if (!at_pos) {
        error_setg(errp, "invalid ip_port format, expected ip@port");
        goto err_option;
    }
    size_t ip_len = at_pos - ip_port;
    if (ip_len >= sizeof(ip)) {
        error_setg(errp, "ip address too long");
        goto err_option;
    }
    memcpy(ip, ip_port, ip_len);
    ip[ip_len] = '\0';
    port = atoi(at_pos + 1);
    // open a socket
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        error_setg_errno(errp, errno, "failed to create socket");
        goto err_sock;
    }
    // configure ip and port
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, ip, &addr.sin_addr) <= 0) {
        error_setg(errp, "invalid ip address: %s", ip);
        goto err_sock2;
    }
    /* we assume that the remote_stub is already initialized before local_qemu starts, therefore
    *  directly connecting is reasonable.
    */
    // todocmsvm: do we need retry?
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        error_setg_errno(errp, errno, "failed to connect to %s:%d", ip, port);
        goto err_connect;
    }
    // long-term usage (kernel automatically send hearbeats)
    enable_tcp_keepalive(fd);
    g_hash_table_insert(gsi_stubs, (gpointer)vdev->name, GUINT_TO_POINTER(fd));
    return;

err_connect:
    force_printf("failed to connect to %s:%d", ip, port);
err_sock2:
    close(fd);
err_option:
err_sock:
    return;
}

// todocmsvm: out_buf and in_buf will be too large for one buffer?
/*
*  this function will be called when remote_stub recieves an elem as [vq_nr, index, out_len, in_len, out_data]
*  it needs to recieve the correct data, prepare remote_ctx, call vq->handle_output, and send back resp
*  resp as [vq_nr, index, in_len, in_data] (in_len is the true len)
*/
static void remote_stub_read_handler(void *opaque)
{
    VirtIODevice *vdev = opaque;
    force_printf("[remote_stub_read_handler] for vdev %s", vdev->name);

    int fd = GPOINTER_TO_UINT(g_hash_table_lookup(gsi_stubs, vdev->name));
    if (fd < 0) {
        return;
    }

    struct io_uring_sqe *sqe;
    struct io_uring_cqe *cqe;
    uint8_t req_header[4 * sizeof(int)];
    uint8_t *out_buf;
    int read_cnt, vq_nr, index, out_len, in_len;

    if (!remote_uring) {
        return;
    }

    read_cnt = 0;
    while (read_cnt < (int)sizeof(req_header)) {
        sqe = io_uring_get_sqe(remote_uring);
        io_uring_prep_recv(sqe, fd, req_header + read_cnt,
                           sizeof(req_header) - read_cnt, 0);
        io_uring_submit(remote_uring);
        io_uring_wait_cqe(remote_uring, &cqe);
        if (cqe->res <= 0) {
            io_uring_cqe_seen(remote_uring, cqe);
            goto link_err;
        }
        read_cnt += cqe->res;
        io_uring_cqe_seen(remote_uring, cqe);
    }

    vq_nr  = req_header[0] | (req_header[1] << 8) |
             (req_header[2] << 16) | (req_header[3] << 24);
    index  = req_header[4] | (req_header[5] << 8) |
             (req_header[6] << 16) | (req_header[7] << 24);
    out_len = req_header[8] | (req_header[9] << 8) |
              (req_header[10] << 16) | (req_header[11] << 24);
    in_len  = req_header[12] | (req_header[13] << 8) |
              (req_header[14] << 16) | (req_header[15] << 24);
    force_printf("[remote_stub_read_handler] recv header [vq_nr:%d, index:%d, out_len:%d, in_len:%d]",
                 vq_nr, index, out_len, in_len);

    out_buf = g_new0(uint8_t, out_len);
    if (!out_buf) {
        return;
    }

    read_cnt = 0;
    while (read_cnt < out_len) {
        sqe = io_uring_get_sqe(remote_uring);
        io_uring_prep_recv(sqe, fd, out_buf + read_cnt,
                           out_len - read_cnt, 0);
        io_uring_submit(remote_uring);
        io_uring_wait_cqe(remote_uring, &cqe);
        if (cqe->res <= 0) {
            io_uring_cqe_seen(remote_uring, cqe);
            g_free(out_buf);
            goto link_err;
        }
        read_cnt += cqe->res;
        force_printf("[remote_stub_read_handler] recv data at [cqe->res:%d, read_cnt:%d, need:%d]",
                     cqe->res, read_cnt, out_len);
        io_uring_cqe_seen(remote_uring, cqe);
    }

    VirtQueue *vq = lookup_vq(vdev, vq_nr);
    if (!vq) {
        force_printf("[remote_stub_read_handler] cannot found vq_nr:%d, vdev:%s",
                     vq_nr, vdev->name);
        g_free(out_buf);
        return;
    }
    force_printf("[remote_stub_read_handler] found vq");

    RemoteVQueueCtx *ctx = virtqueue_get_remote_ctx(vq);

    ctx->resp_fd = fd;
    ctx->vq_nr = vq_nr;
    ctx->elem_index = index;
    ctx->out_len = out_len;
    ctx->in_len = in_len;
    ctx->out_buf = out_buf;
    ctx->in_buf = g_new0(uint8_t, in_len);
    if (in_len > 0 && !ctx->in_buf) {
        g_free(out_buf);
        return;
    }

    ctx->out_sg[0].iov_base = ctx->out_buf;
    ctx->out_sg[0].iov_len = ctx->out_len;
    ctx->in_sg[0].iov_base = ctx->in_buf;
    ctx->in_sg[0].iov_len = ctx->in_len;
    force_printf("[remote_stub_read_handler] wrapper ctx [vq_nr:%d, index:%d, out_len:%d, in_len:%d]",
                 vq_nr, index, out_len, in_len);

    /*
    *  basic handle_output framework: (aio is the same)
    *  while (elem = virtqueue_pop(vq, sizeof(VirtQueueElement))) {
    *      read(elem->out_sg);
    *      ....
    *      write(elem->in_sg);
    *  }
    *  virtqueue_push(...);    or     virtqueue_fill(); virtqueue_flush();
    *  g_free(elem);
    */

    /*
    *  top-bottom half framework:
    *  notifier -> vq->handle_output (top half): forbidden notification and qemu_bh_schedule();
    *           +> reoute_to_remote --> remote_stub_read_handler
    *                                           -> remain elem and free in push
    */

    force_printf("[remote_stub_read_handler] call handle_output...");
    virtqueue_call_handle_output(vq);
    force_printf("[remote_stub_read_handler] call bh to handle");
    aio_bh_poll(qemu_get_aio_context());

    // early free
    g_free(out_buf);
    // g_free(msg_sg[0]);
    g_free(ctx->in_buf);
    // reset remote_ctx
    memset(ctx, 0, sizeof(*ctx));

    force_printf("[remote_stub_read_handler] return");
    return;

link_err:
    qemu_set_fd_handler(fd, NULL, NULL, NULL);
    close(fd);
    g_hash_table_remove(gsi_stubs, vdev->name);
}

static void remote_stub_accept_handler(void *opaque)
{
    force_printf("[remote_stub_accept_handler] Begin to connect ...");

    VirtIODevice *vdev = opaque;
    int listen_fd = GPOINTER_TO_UINT(g_hash_table_lookup(gsi_stubs, vdev->name));

    if (listen_fd < 0) {
        return;
    }

    int fd = accept(listen_fd, NULL, NULL);
    if (fd < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            error_report("remote stub accept failed for vdev %s: %s",
                         vdev->name, strerror(errno));
        }
        return;
    }

    qemu_set_fd_handler(listen_fd, NULL, NULL, NULL);
    g_hash_table_remove(gsi_stubs, vdev->name);
    close(listen_fd);
    enable_tcp_keepalive(fd);
    g_hash_table_insert(gsi_stubs, (gpointer)vdev->name, GUINT_TO_POINTER(fd));

    qemu_set_fd_handler(fd, remote_stub_read_handler, NULL, vdev);
    force_printf("connected for dev %s", vdev->name);
}

void init_remote_stub_socket(VirtIODevice *vdev, const char *str_port, Error **errp)
{
    force_printf("[init_remote_stub_socket] for vdev %s to listen in port %s", vdev->name, str_port);

    int port = atoi(str_port);

    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        error_setg_errno(errp, errno, "failed to create listen socket");
        return;
    }

    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        error_setg_errno(errp, errno, "failed to bind listen socket port %d", port);
        close(listen_fd);
        return;
    }

    if (listen(listen_fd, 1) < 0) {
        error_setg_errno(errp, errno, "failed to listen on socket");
        close(listen_fd);
        return;
    }

    int flags = fcntl(listen_fd, F_GETFL, 0);
    if (flags >= 0) {
        fcntl(listen_fd, F_SETFL, flags | O_NONBLOCK);
    }

    // tag vq is a remote_stub
    for (int n = 0; n < VIRTIO_QUEUE_MAX; n++) {
        if (!virtio_queue_get_num(vdev, n)) {
            continue;
        }
        RemoteVQueueCtx *ctx = g_new0(RemoteVQueueCtx, 1);
        virtqueue_set_remote_ctx(virtio_get_queue(vdev, n), ctx);
    }

    g_hash_table_insert(gsi_stubs, (gpointer)vdev->name, GUINT_TO_POINTER(listen_fd));
    /*
    *  we think it is okay to put the listen resp in main-loop, as the local_qemu can only
    *  connect server after remote_stub is started.
    */
    qemu_set_fd_handler(listen_fd, remote_stub_accept_handler, NULL, vdev);
}

static void remote_device_clean_up_hash_table(VirtIODevice *vdev)
{
    // gsi_stubs
    if (g_hash_table_lookup(gsi_stubs, vdev->name)) {
        g_hash_table_remove(gsi_stubs, vdev->name);
    }
    // gsi_elems + gsi_ctxes
    if (g_hash_table_lookup(gsi_ctxes, vdev->name)) {
        g_hash_table_remove(gsi_ctxes, vdev->name);
    }
    // set_aio
    if (set_aio && g_hash_table_contains(set_aio, vdev->name))
        g_hash_table_remove(set_aio, vdev->name);
}

static void close_remote_virtio_device_sockets(VirtIODevice *vdev)
{
    int fd = GPOINTER_TO_UINT(g_hash_table_lookup(gsi_stubs, vdev->name));
    if (fd >= 0) {
        qemu_set_fd_handler(fd, NULL, NULL, NULL);
        close(fd);
    }

    for (int n = 0; n < VIRTIO_QUEUE_MAX; n++) {
        if (!virtio_queue_get_num(vdev, n)) {
            continue;
        }
        RemoteVQueueCtx *remote_ctx = virtqueue_get_remote_ctx(virtio_get_queue(vdev, n));
        if (remote_ctx) {
            g_free(remote_ctx);
            virtqueue_set_remote_ctx(virtio_get_queue(vdev, n), NULL);
        }
    }
}

typedef struct ListenerParam {
    VirtIODevice *vdev;
    VirtQueue *vq;
    int stub;
    CommCTX *comm_ctx;
} ListenerParam;

static void* resp_listener(void *opaque)
{
    ListenerParam *param = (ListenerParam *)opaque;
    VirtIODevice *vdev = param->vdev;
    VirtQueue *vq = param->vq;
    int stub = param->stub;
    CommCTX *comm_ctx = param->comm_ctx;
    g_free(opaque);
    struct io_uring_sqe *sqe;
    struct io_uring_cqe *cqe;
    uint8_t resp_header[3 * sizeof(int)];
    int vq_nr, index, len;
    int read_cnt, phase; // WARN: phase is not reliable code
    VirtQueueElement *elem;
    char *buf = NULL;

    force_printf("[resp_listener] to listener resps for vdev %s", vdev->name);

    while (true) {
listen_begin:
        phase = 0;

        while (qatomic_read(&comm_ctx->sending) && (qatomic_read(&comm_ctx->recved) >= qatomic_read(&comm_ctx->sent)))
            sem_wait(&comm_ctx->sem1);
        if (!qatomic_read(&comm_ctx->sending) && (qatomic_read(&comm_ctx->recved) >= qatomic_read(&comm_ctx->sent)))
            break;

        elem = comm_ctx->ring[comm_ctx->recved % RING_SIZE];
        if (elem->in_num == 0) {
            elem->len = 0;
            goto done;
        }

        force_printf("[resp_listener] recv a resp, begin to read header");
        read_cnt = 0;
listen_header:
        phase = 1;
        while (read_cnt < (int)sizeof(resp_header)) {
            sqe = io_uring_get_sqe(remote_uring);
            io_uring_prep_recv(sqe, stub, resp_header + read_cnt,
                               sizeof(resp_header) - read_cnt, 0);
            io_uring_submit(remote_uring);
            io_uring_wait_cqe(remote_uring, &cqe);
            if (cqe->res <= 0) {
                io_uring_cqe_seen(remote_uring, cqe);
                goto link_err;
            }
            read_cnt += cqe->res;
            io_uring_cqe_seen(remote_uring, cqe);
        }
        vq_nr = resp_header[0] | (resp_header[1] << 8) |
                (resp_header[2] << 16) | (resp_header[3] << 24);
        index = resp_header[4] | (resp_header[5] << 8) |
                (resp_header[6] << 16) | (resp_header[7] << 24);
        len   = resp_header[8] | (resp_header[9] << 8) |
                (resp_header[10] << 16) | (resp_header[11] << 24);

        force_printf("[resp_listener] get header as [vq_nr: %d, index: %d, len: %d]", vq_nr, index, len);

        vq = lookup_vq(vdev, vq_nr);
        if (!vq) {
            force_printf("[resp_listener] vq [%d] cannot found.", vq_nr);
            goto elem_err;
        }

        buf = g_new0(char, len);
        read_cnt = 0;
listen_data:
        phase = 2;
        while (read_cnt < len) {
            sqe = io_uring_get_sqe(remote_uring);
            io_uring_prep_recv(sqe, stub, buf + read_cnt, len - read_cnt, 0);
            io_uring_submit(remote_uring);
            io_uring_wait_cqe(remote_uring, &cqe);
            if (cqe->res <= 0) {
                io_uring_cqe_seen(remote_uring, cqe);
                g_free(buf);
                goto link_err;
            }
            read_cnt += cqe->res;
            force_printf("[resp_listener] recv data at [cqe->res: %d, read_cnt: %d, need: %d]", cqe->res, read_cnt, len);
            io_uring_cqe_seen(remote_uring, cqe);
        }
        // write resp to in_sg
        iov_from_buf(elem->in_sg, elem->in_num, 0, buf, len);
        g_free(buf);
        force_printf("[resp_listener] push elem [%d] to vq [%d] with len [%d]", index, vq_nr, len);

done:
        virtqueue_push(vq, elem, elem->len);
        qatomic_fetch_inc(&comm_ctx->recved);
        sem_post(&comm_ctx->sem2);
    }

    force_printf("[resp_listener] return");
    qatomic_set(&comm_ctx->recving, false);
    sem_post(&comm_ctx->sem2);
    return NULL;

link_err:
    if (!reconnect_tcp_socket(stub)) {
        if (buf)
            g_free(buf);
        return NULL;
    }
    switch (phase) {
    case 0:
        goto listen_begin;
    case 1:
        goto listen_header;
    case 2:
        goto listen_data;
    default:
        return NULL;
    }
elem_err:
    g_free(buf);
    return NULL;
}

typedef struct SenderParam {
    VirtQueue *vq;
    int stub;
    CommCTX *comm_ctx;
} SenderParam;

static void* route_to_remote(void *opaque)
{
    SenderParam *param = (SenderParam*) opaque;
    VirtQueue *vq = param->vq;
    int stub = param->stub;
    CommCTX *comm_ctx = param->comm_ctx;
    g_free(opaque);
    VirtQueueElement *elem;
    struct io_uring_sqe *sqe;
    struct io_uring_cqe *cqe;
    int vq_nr = virtio_get_queue_index(vq);

    force_printf("[route_to_remote] for vdev %s", virtqueue_get_vdev_name(vq));

    while ((elem = virtqueue_pop(vq, sizeof(VirtQueueElement)))) {
        // send data as [vq_nr, index, out_len, in_len, out_data]
        struct iovec *msg_sg = g_new0(struct iovec, elem->out_num + 1);
        int header[4];
        header[0] = vq_nr;
        header[1] = elem->index;
        header[2] = 0;
        for (int i = 0; i < elem->out_num; i++) {
            header[2] += elem->out_sg[i].iov_len;
        }
        header[3] = 0;
        for (int i = 0; i < elem->in_num; i++) {
            header[3] += elem->in_sg[i].iov_len;
        }
        msg_sg[0].iov_base = header;
        msg_sg[0].iov_len = sizeof(header);
        memcpy(msg_sg + 1, elem->out_sg, elem->out_num * sizeof(struct iovec));
        struct msghdr msg = {
            .msg_iov = msg_sg,
            .msg_iovlen = elem->out_num + 1,
        };
        sqe = io_uring_get_sqe(remote_uring);
        io_uring_prep_sendmsg_zc(sqe, stub, &msg, 0);
        io_uring_sqe_set_data(sqe, msg_sg[0].iov_base);
        io_uring_submit(remote_uring);

        force_printf("[route_to_remote] sent header [vq_nr:%d, index:%d, out_len:%d, in_len:%d]",
                     header[0], header[1], header[2], header[3]);
        force_printf("[route_to_remote] sent a msg with out_num: %d, in_num: %d", elem->out_num, elem->in_num);

        io_uring_wait_cqe(remote_uring, &cqe);
        io_uring_cqe_seen(remote_uring, cqe);

        if (cqe->flags & IORING_CQE_F_MORE) {
            io_uring_wait_cqe(remote_uring, &cqe);
            io_uring_cqe_seen(remote_uring, cqe);
        }

        g_free(msg_sg);

        force_printf("[route_to_remote] sent success");

        // neglect full first
        comm_ctx->ring[comm_ctx->sent % RING_SIZE] = elem;
        qatomic_fetch_inc(&comm_ctx->sent);
        sem_post(&comm_ctx->sem1);
    }

    qatomic_set(&comm_ctx->sending, false);
    sem_post(&comm_ctx->sem1);

    return NULL;
}

static void remote_virtio_queue_notify_vq(VirtQueue *vq)
{
    force_printf("[remote_virtio_queue_notify_vq] vq[%d] of vdev[%s] has been notified",
                 virtio_get_queue_index(vq), virtqueue_get_vdev_name(vq));
    if (virtqueue_get_vring_desc(vq)) {
        VirtIODevice *vdev = virtqueue_get_vdev(vq);
        int stub = GPOINTER_TO_UINT(g_hash_table_lookup(gsi_stubs, vdev->name));

        if (unlikely(vdev->broken)) {
            return;
        }

        // trace_virtio_queue_notify(vdev, vq - vdev->vq, vq);
        CommCTX *comm_ctx = g_hash_table_lookup(gsi_ctxes, vdev->name);
        comm_ctx->ring = g_new0(VirtQueueElement *, RING_SIZE);
        // reset
        comm_ctx->sending = comm_ctx->recving = true;
        comm_ctx->sent = comm_ctx->recved  = comm_ctx->notified = 0;
        sem_init(&comm_ctx->sem1, 0, 0); sem_init(&comm_ctx->sem2, 0, 0);

        QemuThread listener;
        ListenerParam *listen_param = g_new0(ListenerParam, 1);
        listen_param->vdev = vdev;
        listen_param->stub = stub;
        listen_param->comm_ctx = comm_ctx;

        QemuThread sender;
        SenderParam *sender_param = g_new0(SenderParam, 1);
        sender_param->vq = vq;
        sender_param->stub = stub;
        sender_param->comm_ctx = comm_ctx;

        qemu_thread_create(&listener, "remote_virtqueue_listener",
                           resp_listener, listen_param, QEMU_THREAD_DETACHED);
        qemu_thread_create(&sender, "remote_virtqueue_sender",
                           route_to_remote, sender_param, QEMU_THREAD_DETACHED);

        while (true) {
            while (qatomic_read(&comm_ctx->recving) && (comm_ctx->notified >= qatomic_read(&comm_ctx->recved))) {
                sem_wait(&comm_ctx->sem2);
            }
            if (!qatomic_read(&comm_ctx->recving) && (comm_ctx->notified >= qatomic_read(&comm_ctx->recved)))
                break;
            // handle a notification
            virtio_notify(virtqueue_get_vdev(vq), vq);
            comm_ctx->notified++;
        }

        sem_destroy(&comm_ctx->sem1);
        sem_destroy(&comm_ctx->sem2);
        g_free(comm_ctx->ring);
        comm_ctx->ring = NULL;

        if (unlikely(vdev->start_on_kick)) {
            virtio_set_started(vdev, true);
        }
    }
    force_printf("[remote_virtio_queue_notify_vq] return");
}

// this function is called by the ioeventfd notify
// direction is guest->host : fe->be : local->remote
// so we can left meta data of vring in local machine, send elem to remote
void remote_virtio_queue_host_notifier_read(EventNotifier *n)
{
    VirtQueue *vq = host_notifier_to_vq(n);
    if (event_notifier_test_and_clear(n)) {
        remote_virtio_queue_notify_vq(vq);
    }
}

void remote_virtio_queue_host_notifier_aio_poll_ready(EventNotifier *n)
{
    VirtQueue *vq = host_notifier_to_vq(n);

    remote_virtio_queue_notify_vq(vq);
}

int remote_virtio_device_start_ioeventfd_impl(VirtIODevice *vdev)
{
    force_printf("[remote_virtio_device_start_ioeventfd_impl] for vdev:%s", vdev->name);
    if (!g_hash_table_lookup(gsi_ctxes, vdev->name)) {
        CommCTX *comm_ctx = g_new0(CommCTX, 1);
        g_hash_table_insert(gsi_ctxes, (gpointer)vdev->name, comm_ctx);
    }

    VirtioBusState *qbus = VIRTIO_BUS(qdev_get_parent_bus(DEVICE(vdev)));
    int i, n, r, err;

    memory_region_transaction_begin();
    for (n = 0; n < VIRTIO_QUEUE_MAX; n++) {
        VirtQueue *vq = virtio_get_queue(vdev, n);
        if (!virtio_queue_get_num(vdev, n)) {
            continue;
        }
        r = virtio_bus_set_host_notifier(qbus, n, true);
        if (r < 0) {
            err = r;
            goto assign_error;
        }
        // avoid wild pointer
        virtqueue_set_remote_ctx(vq, NULL);
        event_notifier_set_handler(virtqueue_get_host_notifier(vq),
                                   remote_virtio_queue_host_notifier_read);
    }

    for (n = 0; n < VIRTIO_QUEUE_MAX; n++) {
        VirtQueue *vq = virtio_get_queue(vdev, n);
        if (!virtio_queue_get_num(vdev, n)) {
            continue;
        }
        event_notifier_set(virtqueue_get_host_notifier(vq));
    }

    memory_region_transaction_commit();
    return 0;

assign_error:
    i = n; /* save n for a second iteration after transaction is committed. */
    while (--n >= 0) {
        VirtQueue *vq = virtio_get_queue(vdev, n);
        if (!virtio_queue_get_num(vdev, n)) {
            continue;
        }

        event_notifier_set_handler(virtqueue_get_host_notifier(vq), NULL);
        r = virtio_bus_set_host_notifier(qbus, n, false);
        assert(r >= 0);
    }
    /*
     * The transaction expects the ioeventfds to be open when it
     * commits. Do it now, before the cleanup loop.
     */
    memory_region_transaction_commit();

    while (--i >= 0) {
        if (!virtio_queue_get_num(vdev, i)) {
            continue;
        }
        virtio_bus_cleanup_host_notifier(qbus, i);
    }
    return err;
}

void remote_virtio_device_stop_ioeventfd_impl(VirtIODevice *vdev)
{
    VirtioBusState *qbus = VIRTIO_BUS(qdev_get_parent_bus(DEVICE(vdev)));
    int n, r;

    /*
     * Batch all the host notifiers in a single transaction to avoid
     * quadratic time complexity in address_space_update_ioeventfds().
     */
    memory_region_transaction_begin();
    for (n = 0; n < VIRTIO_QUEUE_MAX; n++) {
        VirtQueue *vq = virtio_get_queue(vdev, n);

        if (!virtio_queue_get_num(vdev, n)) {
            continue;
        }
        event_notifier_set_handler(virtqueue_get_host_notifier(vq), NULL);
        r = virtio_bus_set_host_notifier(qbus, n, false);
        assert(r >= 0);
    }
    /*
     * The transaction expects the ioeventfds to be open when it
     * commits. Do it now, before the cleanup loop.
     */
    memory_region_transaction_commit();

    for (n = 0; n < VIRTIO_QUEUE_MAX; n++) {
        if (!virtio_queue_get_num(vdev, n)) {
            continue;
        }
        virtio_bus_cleanup_host_notifier(qbus, n);
    }

    // need to close sockets
    close_remote_virtio_device_sockets(vdev);
    remote_device_clean_up_hash_table(vdev);
}

// -------------- vhost --------------
