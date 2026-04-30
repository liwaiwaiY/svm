// copy from virtio.c
#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qapi/qapi-commands-virtio.h"
#include "trace.h"
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
#include "virtio-qmp.h"

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

#define IO_URING_DEPTH 32 // maximum concurrent reqs

// each vdev has one TCP socket to remote stub
// <K:vdev->name, V:SOCKET_RV*>
GHashTable *gsi_stubs = NULL;
// <K:vdev->name, V:GHashTable*>
GHashTable *gsi_tables = NULL;
// <K:(vq_nr<<16)|index, V:elemt>; need to find first in gsi_tables
// GHashTable *gsi_elems = NULL;

extern VirtQueueElement* virtqueue_split_pop(VirtQueue* vq, size_t sz);
extern VirtQueueElement* virtqueue_packed_pop(VirtQueue* vq, size_t sz);

static struct io_uring *remote_uring = NULL;
static pthread_mutex_t  rw_lock;
static pthread_cond_t   rw_cond;

static int sent, recved;
static bool sending = false;

static VirtQueue *lookup_vq(VirtIODevice *vdev, int vq_nr)
{
    if (vq_nr < 0 || vq_nr >= VIRTIO_QUEUE_MAX) {
        return NULL;
    }
    return &vdev->vq[vq_nr];
}

static gpointer make_elem_key(int vq_nr, unsigned int index)
{
    return GINT_TO_POINTER((vq_nr << 16) | (index & 0xFFFF));
}

int remote_uring_init(void)
{
    pthread_mutex_init(&rw_lock, NULL);
    pthread_cond_init(&rw_cond, NULL);
    int ret = io_uring_queue_init(IO_URING_DEPTH, &remote_uring, 0);
    if (ret < 0) {
        fprintf(stderr, "io_uring init failed\n");
        return -1;
    }

    // initialize GHashTable for virtio-remote socket management
    if (!gsi_stubs) {
        gsi_stubs = g_hash_table_new(g_direct_hash, g_direct_equal);
    }
    if (!gsi_tables) {
        gsi_tables = g_hash_table_new(g_direct_hash, g_direct_equal);
    }
    return 0;
}

// socket reconnect
int reconnect_tcp_socket(int fd)
{
    // cmsvmTODO v2
    return 0;
}

// enalbe socket aliveness in kernel
int enable_tcp_keepalive(int fd)
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

void *remote_stub_virtqueue_pop(VirtQueue *vq, size_t sz)
{
    RemoteVQueueCtx *ctx = vq->remote_ctx;
    return &ctx->elem;
}

void remote_stub_virtqueue_push(VirtQueue *vq, const VirtQueueElement *elem,
                                unsigned int len)
{
    RemoteVQueueCtx *ctx = vq->remote_ctx;
    struct io_uring_sqe *sqe;
    struct io_uring_cqe *cqe;
    int resp_header[3];

    resp_header[0] = ctx->vq_nr;
    resp_header[1] = ctx->elem_index;
    resp_header[2] = len;

    struct iovec resp_iov[2];
    resp_iov[0].iov_base = resp_header;
    resp_iov[0].iov_len = sizeof(resp_header);
    resp_iov[1].iov_base = ctx->in_buf;
    resp_iov[1].iov_len = len;

    sqe = io_uring_get_sqe(remote_uring);
    io_uring_prep_send_zc(sqe, ctx->resp_fd, resp_iov, 2, 0);
    io_uring_submit(remote_uring);
    io_uring_wait_cqe(remote_uring, &cqe);
    io_uring_cqe_seen(remote_uring, cqe);

    g_free(ctx->out_buf);
    g_free(ctx->in_buf);
    memset(ctx, 0, sizeof(*ctx));
}

bool remote_virtio_notify_skip(VirtIODevice *vdev)
{
    int n;
    for (n = 0; n < VIRTIO_QUEUE_MAX; n++) {
        if (!virtio_queue_get_num(vdev, n)) {
            continue;
        }
        if (vdev->vq[n].remote_ctx) {
            return true;
        }
    }
    return false;
}

// cmsvmTODO v2: try decrease the memory region here, maybe flexible array
void init_remote_virtio_device_sockets(VirtIODevice *vdev, const char *ip_port, Error **errp)
{
    char ip[64];
    int port;
    const char *at_pos = strchr(ip_port, '@');
    if (!at_pos) {
        error_setg(errp, "invalid ip_port format, expected ip@port");
        return;
    }

    size_t ip_len = at_pos - ip_port;
    if (ip_len >= sizeof(ip)) {
        error_setg(errp, "ip address too long");
        return;
    }
    memcpy(ip, ip_port, ip_len);
    ip[ip_len] = '\0';
    port = atoi(at_pos + 1);

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        error_setg_errno(errp, errno, "failed to create socket");
        return;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, ip, &addr.sin_addr) <= 0) {
        error_setg(errp, "invalid ip address: %s", ip);
        goto err;
    }

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        error_setg_errno(errp, errno, "failed to connect to %s:%d", ip, port);
        goto err;
    }

    enable_tcp_keepalive(fd);
    *stub = fd;

    if (!gsi_stubs) {
        gsi_stubs = g_hash_table_new(g_direct_hash, g_direct_equal);
    }
    if (!gsi_tables) {
        gsi_tables = g_hash_table_new(g_direct_hash, g_direct_equal);
    }
    g_hash_table_insert(gsi_stubs, vdev->name, GUINT_TO_POINTER(fd));
    return;

err:
    close(fd);
    return;
}

// todocmsvm: out_buf and in_buf will be too large for one buffer?

static void remote_stub_read_handler(void *opaque)
{
    VirtIODevice *vdev = opaque;
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

    vq_nr  = (req_header[0] << 24) | (req_header[1] << 16) |
             (req_header[2] << 8) | req_header[3];
    index  = (req_header[4] << 24) | (req_header[5] << 16) |
             (req_header[6] << 8) | req_header[7];
    out_len = (req_header[8] << 24) | (req_header[9] << 16) |
              (req_header[10] << 8) | req_header[11];
    in_len  = (req_header[12] << 24) | (req_header[13] << 16) |
              (req_header[14] << 8) | req_header[15];

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
        io_uring_cqe_seen(remote_uring, cqe);
    }

    VirtQueue *vq = lookup_vq(vdev, vq_nr);
    if (!vq) {
        g_free(out_buf);
        return;
    }

    RemoteVQueueCtx *ctx = vq->remote_ctx;

    ctx->resp_fd = fd;
    ctx->vq_nr = vq_nr;
    ctx->elem_index = index;
    ctx->out_len = out_len;
    ctx->in_len = in_len;
    ctx->out_buf = out_buf;
    ctx->in_buf = g_new0(uint8_t, in_len);
    if (!ctx->in_buf) {
        g_free(out_buf);
        return;
    }

    ctx->out_sg[0].iov_base = ctx->out_buf;
    ctx->out_sg[0].iov_len = ctx->out_len;
    ctx->in_sg[0].iov_base = ctx->in_buf;
    ctx->in_sg[0].iov_len = ctx->in_len;
    ctx->elem.index = ctx->elem_index;
    ctx->elem.out_num = 1;
    ctx->elem.in_num = 1;
    ctx->elem.out_sg = ctx->out_sg;
    ctx->elem.in_sg = ctx->in_sg;

    vq->handle_output(vdev, vq);

    return;

link_err:
    qemu_set_fd_handler(fd, NULL, NULL, NULL);
    close(fd);
    g_hash_table_remove(gsi_stubs, vdev->name);
}

static void remote_stub_accept_handler(void *opaque)
{
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
    g_hash_table_insert(gsi_stubs, vdev->name, GUINT_TO_POINTER(fd));

    qemu_set_fd_handler(fd, remote_stub_read_handler, NULL, vdev);
}

void init_remote_stub_socket(VirtIODevice *vdev, const char *ip_port, Error **errp)
{
    int port;
    const char *at_pos = strchr(ip_port, '@');
    if (!at_pos) {
        error_setg(errp, "invalid ip_port format, expected ip@port");
        return;
    }
    port = atoi(at_pos + 1);

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

    for (int n = 0; n < VIRTIO_QUEUE_MAX; n++) {
        if (!virtio_queue_get_num(vdev, n)) {
            continue;
        }
        RemoteVQueueCtx *ctx = g_new0(RemoteVQueueCtx, 1);
        vdev->vq[n].remote_ctx = ctx;
    }

    g_hash_table_insert(gsi_stubs, vdev->name, GUINT_TO_POINTER(listen_fd));

    qemu_set_fd_handler(listen_fd, remote_stub_accept_handler, NULL, vdev);
}

void remote_device_clean_up_hash_table(VirtIODevice *vdev)
{
    // gsi_stubs
    g_hash_table_remove(gsi_stubs, vdev->name);
    // gsi_elems
    g_hash_table_destroy(g_hash_table_lookup(gsi_tables, vdev->name));
    // gsi_tables
    g_hash_table_remove(gsi_tables, vdev->name);
}

void close_remote_virtio_device_sockets(VirtIODevice *vdev)
{
    int fd = GPOINTER_TO_UINT(g_hash_table_lookup(gsi_stubs, vdev->name));
    if (fd >= 0) {
        qemu_set_fd_handler(fd, NULL, NULL, NULL);
        close(fd);
    }
    g_hash_table_remove(gsi_stubs, vdev->name);

    for (int n = 0; n < VIRTIO_QUEUE_MAX; n++) {
        if (!virtio_queue_get_num(vdev, n)) {
            continue;
        }
        RemoteVQueueCtx *ctx = vdev->vq[n].remote_ctx;
        if (ctx) {
            g_free(ctx->out_buf);
            g_free(ctx->in_buf);
            g_free(ctx);
            vdev->vq[n].remote_ctx = NULL;
        }
    }
}

/*
 * Single-socket protocol per vdev:
*  req (local->remote): [vq_nr(4B), elem_index(4B), out_len(4B), out_sg_data...]
*  resp (remote->local): [vq_nr(4B), elem_index(4B), data_len(4B), data...]
 */

typedef struct ListenerParam {
    VirtIODevice *vdev;
    SOCKET_RV stub;
} ListenerParam;

void resp_listener(ListenerParam *param)
{
    VirtIODevice *vdev = param->vdev;
    SOCKET_RV stub = param->stub;
    struct io_uring_sqe *sqe;
    struct io_uring_cqe *cqe;
    uint8_t resp_header[3 * sizeof(int)];
    // scoket determined vdev, so we need vq_nr and index to locate desc link
    int vq_nr, index, len;
    int read_cnt, phase;
    VirtQueueElement *elem;
    VirtQueue *vq;
    char *buf;
    GHashTable *gsi_elems = g_hash_table_lookup(gsi_tables, vdev->name);
    if (!gsi_elems) {
        goto hash_err;
    }

    while (sending) {
listen_begin:
        phase = 0;
        pthread_mutex_lock(&rw_lock);
        while (sending && (recved >= sent)) {
            pthread_cond_wait(&rw_cond, &rw_lock);
        }
        pthread_mutex_unlock(&rw_lock);

        if (!sending) {
            break;
        }

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
        vq_nr = (resp_header[0] << 24) | (resp_header[1] << 16) |
                (resp_header[2] << 8) | resp_header[3];
        index = (resp_header[4] << 24) | (resp_header[5] << 16) |
                (resp_header[6] << 8) | resp_header[7];
        len   = (resp_header[8] << 24) | (resp_header[9] << 16) |
                (resp_header[10] << 8) | resp_header[11];

        vq = lookup_vq(vdev, vq_nr);
        if (!vq) {
            goto elem_err;
        }

        elem = g_hash_table_lookup(gsi_elems, make_elem_key(vq_nr, index));
        if (!elem) {
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
            io_uring_cqe_seen(remote_uring, cqe);
        }

        iov_from_buf(elem->in_sg, elem->in_num, 0, buf, len);
        g_free(buf);

        virtqueue_push(vq, elem, elem->len);

        g_hash_table_remove(gsi_elems, make_elem_key(vq_nr, index));
        recved++;
    }

    return;

hash_err:
    return;
link_err:
    if (!reconnect_tcp_socket(stub) && buf) {
        g_free(buf);
        return;
    }
    switch (phase) {
    case 0:
        goto listen_begin;
    case 1:
        goto listen_header;
    case 2:
        goto listen_data;
    default:
        return;
    }
elem_err:
    g_free(buf);
    return;
}

void route_to_remote(VirtQueue *vq, SOCKET_RV stub)
{
    VirtQueueElement* (*remote_virtqueue_pop)(VirtQueue *, size_t);
    VirtQueueElement *elem;
    struct io_uring_sqe *sqe;
    struct io_uring_cqe *cqe;
    GHashTable *gsi_elems = g_hash_table_lookup(gsi_tables, vq->vdev->name);
    int vq_nr = vq - vq->vdev->vq;

    if (virtio_device_disabled(vq->vdev)) {
        return;
    }

    if (virtio_vdev_has_feature(vq->vdev, VIRTIO_F_RING_PACKED)) {
        remote_virtqueue_pop = virtqueue_packed_pop;
    } else {
        remote_virtqueue_pop = virtqueue_split_pop;
    }

    while ((elem = remote_virtqueue_pop(vq, sizeof(VirtQueueElement)))) {
        struct iovec msg_sg[elem->out_num + 1];
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
        msg_sg[0].iov_base = g_malloc(sizeof(header));
        memcpy(msg_sg[0].iov_base, header, sizeof(header));
        msg_sg[0].iov_len = sizeof(header);
        memcpy(msg_sg + 1, elem->out_sg, elem->out_num * sizeof(struct iovec));
        sqe = io_uring_get_sqe(remote_uring);
        io_uring_prep_send_zc(sqe, stub, msg_sg, elem->out_num + 1, 0);
        io_uring_sqe_set_data(sqe, msg_sg[0].iov_base);
        io_uring_submit(remote_uring);
        io_uring_wait_cqe(remote_uring, &cqe);
        g_free(msg_sg[0].iov_base);

        // a new resp is needed
        if (elem->in_num > 0) {
            g_hash_table_insert(gsi_elems, make_elem_key(vq_nr, elem->index), elem);
            sent++;
            pthread_cond_signal(&rw_cond);
        } else {
            virtqueue_push(vq, elem, 0);
        }
    }
}

static void remote_virtio_queue_notify_vq(VirtQueue *vq)
{
    if (vq->vring.desc) {
        sent = 0;
        recved = 0;
        VirtIODevice *vdev = vq->vdev;
        SOCKET_RV stub = (SOCKET_RV)GPOINTER_TO_UINT(
            g_hash_table_lookup(gsi_stubs, vdev->name));

        if (unlikely(vdev->broken)) {
            return;
        }

        if (!remote_uring) {
            remote_uring_init();
        }

        trace_virtio_queue_notify(vdev, vq - vdev->vq, vq);
        sending = true;
        QemuThread listener;
        ListenerParam *param = g_new0(ListenerParam, 1);
        param->vdev = vdev;
        param->stub = stub;
        qemu_thread_create(&listener, "remote_virtqueue_listener",
                           resp_listener, param, QEMU_THREAD_JOINABLE);
        route_to_remote(vq, stub);
        sending = false;
        pthread_cond_signal(&rw_cond);
        qemu_thread_join(&listener);

        if (unlikely(vdev->start_on_kick)) {
            virtio_set_started(vdev, true);
        }
    }
}

// this function is called by the ioeventfd notify
// direction is guest->host : fe->be : local->remote
// so we can left meta data of vring in local machine, send elem to remote
void remote_virtio_queue_host_notifier_read(EventNotifier *n)
{
    VirtQueue *vq = container_of(n, VirtQueue, host_notifier);
    if (event_notifier_test_and_clear(n)) {
        remote_virtio_queue_notify_vq(vq);
    }
}

int remote_virtio_device_start_ioeventfd_impl(VirtioDevice *vdev)
{
    if (!g_hash_table_lookup(gsi_tables, vdev->name)) {
        // vdev impl first time
        GHashTable *ptr = g_hash_table_new(g_direct_hash, g_direct_equal);
        g_hash_table_insert(gsi_tables, vdev->name, ptr);
    }

    VirtioBusState *qbus = VIRTIO_BUS(qdev_get_parent_bus(DEVICE(vdev)));
    int i, n, r, err;

    // cmsvm version1: sockets are created in initialization phase
    // cmsvmTODO v2: lazy binding at ept violation, i.e. here or pci_common_write
    memory_region_transaction_begin();
    for (n = 0; n < VIRTIO_QUEUE_MAX; n++) {
        VirtQueue *vq = &vdev->vq[n];
        if (!virtio_queue_get_num(vdev, n)) {
            continue;
        }
        r = virtio_bus_set_host_notifier(qbus, n, true);
        if (r < 0) {
            err = r;
            goto assign_error;
        }

        event_notifier_set_handler(&vq->host_notifier,
                                   remote_virtio_queue_host_notifier_read);
    }

    for (n = 0; n < VIRTIO_QUEUE_MAX; n++) {
        VirtQueue *vq = &vdev->vq[n];
        if (!vq->vring.num) {
            continue;
        }
        event_notifier_set(&vq->host_notifier);
    }

    memory_region_transaction_commit();
    return 0;

assign_error:
    i = n; /* save n for a second iteration after transaction is committed. */
    while (--n >= 0) {
        VirtQueue *vq = &vdev->vq[n];
        if (!virtio_queue_get_num(vdev, n)) {
            continue;
        }

        event_notifier_set_handler(&vq->host_notifier, NULL);
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
        VirtQueue *vq = &vdev->vq[n];

        if (!virtio_queue_get_num(vdev, n)) {
            continue;
        }
        event_notifier_set_handler(&vq->host_notifier, NULL);
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
    // todocmsvm: need to free heap mem
}

void remote_virtio_pci_notify(DeviceState *dev, uint16_t vector)
{
    // we don't need to write msi-x or irq
    // wait remote_stub_read_handler to send resp
    return;
}