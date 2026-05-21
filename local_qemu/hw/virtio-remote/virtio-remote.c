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
#define RING_SIZE (IO_URING_DEPTH * 4)

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

// static void ensure_log_dir(const char *dir)
// {
//     g_mkdir_with_parents(dir, 0755);
// }

// static void log_hex_dump(const char *filepath, const char *prefix,
//                          int seq, const uint8_t *data, int len)
// {
//     FILE *f = fopen(filepath, "a");
//     if (!f) return;

//     fprintf(f, "[%s #%d] len=%d:\n", prefix, seq, len);
//     for (int off = 0; off < len; off += 32) {
//         int n = len - off < 32 ? len - off : 32;
//         for (int i = 0; i < n; i++)
//             fprintf(f, "%02x ", data[off + i]);
//         fprintf(f, "\n");
//     }
//     fflush(f);
//     fclose(f);
// }

// static void log_hex_dump_iov(const char *filepath, const char *prefix,
//                              int seq, struct iovec *iov, int iov_cnt)
// {
//     FILE *f = fopen(filepath, "a");
//     int total = 0, i = 0, off = 0;
//     if (!f) return;

//     for (int k = 0; k < iov_cnt; k++)
//         total += iov[k].iov_len;

//     fprintf(f, "[%s #%d] iov_cnt=%d total_len=%d:\n", prefix, seq, iov_cnt, total);
//     while (i < iov_cnt) {
//         for (int col = 0; col < 32 && i < iov_cnt; col++) {
//             uint8_t *p = (uint8_t *)iov[i].iov_base;
//             fprintf(f, "%02x ", p[off]);
//             off++;
//             if (off >= (int)iov[i].iov_len) { i++; off = 0; }
//         }
//         fprintf(f, "\n");
//     }
//     fflush(f);
//     fclose(f);
// }

// static atomic_int local_send_seq;
// static atomic_int local_recv_seq;
// static atomic_int remote_recv_seq;
// static atomic_int remote_send_seq;

#define LOCAL_LOG_DIR  "/home/waiai/SvmExp/local/svm/log"
#define REMOTE_LOG_DIR "/home/waiai/SvmExp/remote/log"

// static void log_init_local(void)
// {
//     ensure_log_dir(LOCAL_LOG_DIR);
// }

// static void log_init_remote(void)
// {
//     ensure_log_dir(REMOTE_LOG_DIR);
// }

/*
*  format: <K:DEVICE(vdev)->id, V:int>
*  local_qemu: a link head of sockets of each vq
*  remote_stub: a socket of listening
*/
GHashTable *gsi_stubs = NULL;

/*
*  format: <K:DEVICE(vdev)->id, V:CommCTX*>
*  local_qemu: a hash table of sent elements
*  remote_qemu: none
*/
// 
GHashTable *gsi_ctxes = NULL;

/*
*  format: <K:DEVICE(vdev)->id, V:Bool>
*  local_qemu: aio list
*  remote_stub: none
*/
GHashTable *set_aio = NULL;

/*
*  format: <K:DEVICE(vdev)->id, GINT_TO_POINTER(0)>
*  local_qemu: remote devices list
*  remote_stub: remote devices list
*/
GHashTable* ids = NULL;

static struct io_uring send_uring_data;
static struct io_uring *send_uring = &send_uring_data;

static struct io_uring resp_uring_data;
static struct io_uring *resp_uring = &resp_uring_data;

/*
*  decoupling I/O with strong ordering
*/
typedef struct CommCTX {
    bool used;
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
    if (!gsi_stubs) // none remote devices
        return false;

    if (!DEVICE(vdev)->id) // no id
        return false;

    return g_hash_table_contains(ids, DEVICE(vdev)->id);
}

bool check_origin_qemu_in_iothread(VirtIODevice *vdev)
{
    if (!set_aio)
        return false;
    return g_hash_table_contains(set_aio, DEVICE(vdev)->id);
}

void remote_virtio_register_aio(VirtIODevice *vdev)
{
    if (!set_aio)
        set_aio = g_hash_table_new(g_str_hash, g_str_equal);
    g_hash_table_insert(set_aio, DEVICE(vdev)->id, GINT_TO_POINTER(0));
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
    if (gsi_stubs)
        return 0;
    // force_printf("[remote_uring_init] for %s", remote_stub ? "remote_stub" : "local_qemu");

    gsi_stubs = g_hash_table_new(g_str_hash, g_str_equal);

    if (!remote_stub) { // local_qemu
        // log_init_local();
        gsi_ctxes = g_hash_table_new(g_str_hash, g_str_equal);
        int ret = io_uring_queue_init(IO_URING_DEPTH, send_uring, 0);
        if (ret < 0) {
            fprintf(stderr, "send_uring init failed\n");
            return -1;
        }
        ret = io_uring_queue_init(IO_URING_DEPTH, resp_uring, 0);
        if (ret < 0) {
            fprintf(stderr, "resp_uring init failed\n");
            io_uring_queue_exit(send_uring);
            return -1;
        }
        
    } else {
        // log_init_remote();
        int ret = io_uring_queue_init(IO_URING_DEPTH, send_uring, 0);
        if (ret < 0) {
            fprintf(stderr, "send_uring init failed\n");
            return -1;
        }
        ret = io_uring_queue_init(IO_URING_DEPTH, resp_uring, 0);
        if (ret < 0) {
            fprintf(stderr, "resp_uring init failed\n");
            io_uring_queue_exit(send_uring);
            return -1;
        }
    }
    return 0;
}


static int seq = 0;

void remote_register_id(Object *obj, const char *id, Error **errp)
{
    // force_printf("[remote_register_id] regiserint id");
    if (!ids)
        ids = g_hash_table_new(g_str_hash, g_str_equal);
    if (g_hash_table_lookup(ids, id)) { // duplicate
        error_setg(errp, "Duplicate device ID '%s'", id);
        return;
    } else if (id) {
        DEVICE(obj)->id = g_strdup(id);
        // force_printf("[remote_register_id] register id [%s]", DEVICE(obj)->id);
    } else { // allocate one
        DEVICE(obj)->id = g_strdup_printf("remote%d", seq++);
        // force_printf("[remote_register_id] register id [%s]", DEVICE(obj)->id);
    }
    g_hash_table_insert(ids, DEVICE(obj)->id, GINT_TO_POINTER(0));
}

// socket reconnect
static int reconnect_tcp_socket(int fd)
{
    // cmsvmTODO v2
    return 0;
}

// enalbe socket aliveness in kernel. 55s maximum link down
static int enable_tcp_keepalive(int fd)
{
    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &opt, sizeof(opt));
    int keep_idle = 30;
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &keep_idle, sizeof(keep_idle));
    int keep_intvl = 5;
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &keep_intvl, sizeof(keep_intvl));
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

// remote_ctx has to be existing
bool remote_virtio_queue_empty(void *opaque)
{
    RemoteVQueueCtx *remote_ctx = (RemoteVQueueCtx *)opaque;
    return remote_ctx->elem; // popped once from the ctx
}

void *remote_stub_virtqueue_pop(VirtQueue *vq, size_t sz)
{
    RemoteVQueueCtx *ctx = virtqueue_get_remote_ctx(vq);
    if (ctx->elem) { // poped once
        // force_printf("[remote_stub_virtqueue_pop] empty as poped once from ctx");
        return NULL;
    }

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
        ret->in_addr[i] = 0;
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
    // force_printf("[remote_stub_virtqueue_push] send resp for vdev %s", virtqueue_get_vdev_id(vq));
    if (len == 0) {
        // force_printf("[remote_stub_virtqueue_push] len == 0, don't send");
        return;
    }

    RemoteVQueueCtx *ctx = virtqueue_get_remote_ctx(vq);
    struct io_uring_sqe *sqe;
    struct io_uring_cqe *cqe;
    int resp_header[3];

    resp_header[0] = ctx->vq_nr;
    resp_header[1] = elem->index;
    resp_header[2] = len;

    struct iovec resp_iov[2] = {
        { .iov_base = resp_header,     .iov_len = sizeof(resp_header) },
        { .iov_base = elem->in_sg[0].iov_base,      .iov_len = len },
    };
    struct msghdr msg = {
        .msg_iov = resp_iov,
        .msg_iovlen = 2,
    };
    force_printf("[remote_stub_virtqueue_push] send resp at [vq_nr:%d, offset:%d, len: %d]",
                 resp_header[0], resp_header[1], resp_header[2]);

    // log_hex_dump_iov(REMOTE_LOG_DIR "/remote-send.log", "SEND_HDR",
    //                  atomic_fetch_add(&remote_send_seq, 1) + 1,
    //                  resp_iov, 1);
    // log_hex_dump_iov(REMOTE_LOG_DIR "/remote-send.log", "SEND",
    //                  atomic_fetch_add(&remote_send_seq, 1) + 1,
    //                  resp_iov + 1, 1);

    sqe = io_uring_get_sqe(send_uring);
    io_uring_prep_sendmsg_zc(sqe, ctx->resp_fd, &msg, 0);
    sqe->ioprio |= IORING_SEND_ZC_REPORT_USAGE;
    io_uring_submit(send_uring);

    io_uring_wait_cqe(send_uring, &cqe);
    io_uring_cqe_seen(send_uring, cqe);

    if (cqe->flags & IORING_CQE_F_MORE) {
        io_uring_wait_cqe(send_uring, &cqe);
        io_uring_cqe_seen(send_uring, cqe);
    }

    g_free(elem->out_sg[0].iov_base);
    g_free(elem->in_sg[0].iov_base);
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
    // force_printf("[init_remote_virtio_device_sockets] for vdev %s to connect %s", DEVICE(vdev)->id, ip_port);

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
    g_hash_table_insert(gsi_stubs, DEVICE(vdev)->id, GUINT_TO_POINTER(fd));
    CommCTX *comm_ctx = g_new0(CommCTX, 1);
    g_hash_table_insert(gsi_ctxes, (gpointer)DEVICE(vdev)->id, comm_ctx);

    return;

err_connect:
    // force_printf("failed to connect to %s:%d", ip, port);
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
    // force_printf("[remote_stub_read_handler] for vdev %s", DEVICE(vdev)->id);

    int fd = GPOINTER_TO_UINT(g_hash_table_lookup(gsi_stubs, DEVICE(vdev)->id));
    if (fd < 0) {
        return;
    }

    struct io_uring_sqe *sqe;
    struct io_uring_cqe *cqe;
    uint8_t req_header[4 * sizeof(int)];
    uint8_t *out_buf;
    int read_cnt, vq_nr, index, out_len, in_len;

    if (!resp_uring) {
        return;
    }

    read_cnt = 0;
    while (read_cnt < (int)sizeof(req_header)) {
        sqe = io_uring_get_sqe(resp_uring);
        io_uring_prep_recv(sqe, fd, req_header + read_cnt,
                           sizeof(req_header) - read_cnt, 0);
        io_uring_submit(resp_uring);
        io_uring_wait_cqe(resp_uring, &cqe);
        if (cqe->res <= 0) {
            io_uring_cqe_seen(resp_uring, cqe);
            goto link_err;
        }
        read_cnt += cqe->res;
        io_uring_cqe_seen(resp_uring, cqe);
    }

    // log_hex_dump(REMOTE_LOG_DIR "/remote-recv.log", "RECV_HDR",
    //              atomic_fetch_add(&remote_recv_seq, 1) + 1,
    //              req_header, sizeof(req_header));

    vq_nr  = req_header[0] | (req_header[1] << 8) |
             (req_header[2] << 16) | (req_header[3] << 24);
    index  = req_header[4] | (req_header[5] << 8) |
             (req_header[6] << 16) | (req_header[7] << 24);
    out_len = req_header[8] | (req_header[9] << 8) |
              (req_header[10] << 16) | (req_header[11] << 24);
    in_len  = req_header[12] | (req_header[13] << 8) |
              (req_header[14] << 16) | (req_header[15] << 24);
    force_printf("[remote_stub_read_handler] recv header [vq_nr:%d, offset:%d, out_len:%d, in_len:%d]",
                 vq_nr, index, out_len, in_len);

    out_buf = g_new0(uint8_t, out_len);
    if (!out_buf) {
        return;
    }

    read_cnt = 0;
    while (read_cnt < out_len) {
        sqe = io_uring_get_sqe(resp_uring);
        io_uring_prep_recv(sqe, fd, out_buf + read_cnt,
                           out_len - read_cnt, 0);
        io_uring_submit(resp_uring);
        io_uring_wait_cqe(resp_uring, &cqe);
        if (cqe->res <= 0) {
            io_uring_cqe_seen(resp_uring, cqe);
            g_free(out_buf);
            goto link_err;
        }
        read_cnt += cqe->res;
        // force_printf("[remote_stub_read_handler] recv data at [cqe->res:%d, read_cnt:%d, need:%d]",
        //              cqe->res, read_cnt, out_len);
        io_uring_cqe_seen(resp_uring, cqe);
    }

    // log_hex_dump(REMOTE_LOG_DIR "/remote-recv.log", "RECV",
    //              atomic_fetch_add(&remote_recv_seq, 1) + 1,
    //              out_buf, out_len);

    VirtQueue *vq = lookup_vq(vdev, vq_nr);
    if (!vq) {
        // force_printf("[remote_stub_read_handler] cannot found vq_nr:%d, vdev:%s",
        //              vq_nr, DEVICE(vdev)->id);
        g_free(out_buf);
        return;
    }
    // force_printf("[remote_stub_read_handler] found vq");

    RemoteVQueueCtx *ctx = virtqueue_get_remote_ctx(vq);
    if (!ctx) {
        // force_printf("[remote_stub_read_handler] unexpected empty remote_ctx");
        exit(0);
    }
    if (!ctx->resp_fd) { // first called
        ctx->resp_fd = fd;
        ctx->vq_nr = vq_nr;
    }

    // elem-specific
    ctx->elem_index = index;
    ctx->out_len = out_len;
    ctx->in_len = in_len;
    ctx->out_buf = out_buf;
    ctx->in_buf = g_new0(uint8_t, in_len);
    if (in_len > 0 && !ctx->in_buf) {
        // force_printf("[remote_stub_read_handler] unexpected allocation error");
        g_free(out_buf);
        return;
    }

    ctx->out_sg[0].iov_base = ctx->out_buf;
    ctx->out_sg[0].iov_len = ctx->out_len;
    ctx->in_sg[0].iov_base = ctx->in_buf;
    ctx->in_sg[0].iov_len = ctx->in_len;
    // force_printf("[remote_stub_read_handler] wrapper ctx [vq_nr:%d, index:%d, out_len:%d, in_len:%d]",
    //              vq_nr, index, out_len, in_len);

    /*
    *  basic handle_output framework:
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

    /*
    *  asynchronous IO framework:
    *  handle_ouput: defer_call(...) -> return
    *  backend: call push later
    */

    // force_printf("[remote_stub_read_handler] call handle_output at[%p]", virtqueue_get_handle_output(vq));
    virtqueue_call_handle_output(vq);
    // force_printf("[remote_stub_read_handler] call bh to handle");
    aio_bh_poll(qemu_get_aio_context());

    // test to free in push
    // early free
    // g_free(out_buf);
    // g_free(ctx->in_buf);
    // reset remote_ctx
    ctx->elem = NULL;
    ctx->elem_index = 0;
    ctx->out_len = 0;
    ctx->in_len = 0;
    ctx->out_buf = NULL;
    ctx->in_buf = NULL;
    ctx->out_sg[0].iov_base = NULL;
    ctx->out_sg[0].iov_len = 0;
    ctx->in_sg[0].iov_base = NULL;
    ctx->in_sg[0].iov_len = 0;

    // force_printf("[remote_stub_read_handler] return");
    return;

link_err:
    qemu_set_fd_handler(fd, NULL, NULL, NULL);
    close(fd);
    g_hash_table_remove(gsi_stubs, DEVICE(vdev)->id);
}

static void remote_stub_accept_handler(void *opaque)
{
    // force_printf("[remote_stub_accept_handler] Begin to connect ...");

    VirtIODevice *vdev = opaque;
    int listen_fd = GPOINTER_TO_UINT(g_hash_table_lookup(gsi_stubs, DEVICE(vdev)->id));

    if (listen_fd < 0) {
        return;
    }

    int fd = accept(listen_fd, NULL, NULL);
    if (fd < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            error_report("remote stub accept failed for vdev %s: %s",
                         DEVICE(vdev)->id, strerror(errno));
        }
        return;
    }

    qemu_set_fd_handler(listen_fd, NULL, NULL, NULL);
    g_hash_table_remove(gsi_stubs, DEVICE(vdev)->id);
    close(listen_fd);
    enable_tcp_keepalive(fd);
    g_hash_table_insert(gsi_stubs, DEVICE(vdev)->id, GUINT_TO_POINTER(fd));

    qemu_set_fd_handler(fd, remote_stub_read_handler, NULL, vdev);
    // force_printf("connected for dev %s", DEVICE(vdev)->id);
}

void init_remote_stub_socket(VirtIODevice *vdev, const char *str_port, Error **errp)
{
    // force_printf("[init_remote_stub_socket] for vdev %s to listen in port %s", DEVICE(vdev)->id, str_port);

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
        // force_printf("[init_remote_stub_socket] set remote ctx at [%p] for vq [%d]", ctx, n);
    }

    g_hash_table_insert(gsi_stubs, DEVICE(vdev)->id, GUINT_TO_POINTER(listen_fd));
    /*
    *  we think it is okay to put the listen resp in main-loop, as the local_qemu can only
    *  connect server after remote_stub is started.
    */
    qemu_set_fd_handler(listen_fd, remote_stub_accept_handler, NULL, vdev);
}

static void remote_device_clean_up_hash_table(VirtIODevice *vdev)
{
    // gsi_stubs
    if (g_hash_table_lookup(gsi_stubs, DEVICE(vdev)->id)) {
        g_hash_table_remove(gsi_stubs, DEVICE(vdev)->id);
    }
    // gsi_elems + gsi_ctxes
    if (g_hash_table_lookup(gsi_ctxes, DEVICE(vdev)->id)) {
        g_hash_table_remove(gsi_ctxes, DEVICE(vdev)->id);
    }
    // set_aio
    if (set_aio && g_hash_table_contains(set_aio, DEVICE(vdev)->id))
        g_hash_table_remove(set_aio, DEVICE(vdev)->id);
}

static void close_remote_virtio_device_sockets(VirtIODevice *vdev)
{
    int fd = GPOINTER_TO_UINT(g_hash_table_lookup(gsi_stubs, DEVICE(vdev)->id));
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
    int vq_nr, index, len, tmp_recved;
    int read_cnt, phase; // WARN: phase is not reliable code
    VirtQueueElement *elem;
    char *buf = NULL;
    int mapping[RING_SIZE]; // record mapping of switch for aio
    memset(mapping, 0, RING_SIZE);

    // force_printf("[resp_listener] to listener resps for vdev %s", DEVICE(vdev)->id);

    while (true) {
listen_begin:
        phase = 0;

        while (qatomic_read(&comm_ctx->sending) && (qatomic_read(&comm_ctx->recved) >= qatomic_read(&comm_ctx->sent)))
            sem_wait(&comm_ctx->sem1);
        if (!qatomic_read(&comm_ctx->sending) && (qatomic_read(&comm_ctx->recved) >= qatomic_read(&comm_ctx->sent)))
            break;

        tmp_recved = qatomic_read(&comm_ctx->recved);
        elem = comm_ctx->ring[tmp_recved % RING_SIZE];
        if (elem->in_num == 0) {
            elem->len = 0;
            force_printf("[resp_listener] skip one elem at [%p]\n", elem);
            goto done;
        }

        // force_printf("[resp_listener] recv a resp, begin to read header");
        read_cnt = 0;
listen_header:
        phase = 1;
        while (read_cnt < (int)sizeof(resp_header)) {
            sqe = io_uring_get_sqe(resp_uring);
            io_uring_prep_recv(sqe, stub, resp_header + read_cnt,
                               sizeof(resp_header) - read_cnt, 0);
            io_uring_submit(resp_uring);
            io_uring_wait_cqe(resp_uring, &cqe);
            if (cqe->res <= 0) {
                io_uring_cqe_seen(resp_uring, cqe);
                goto link_err;
            }
            read_cnt += cqe->res;
            io_uring_cqe_seen(resp_uring, cqe);
        }
        // log_hex_dump(LOCAL_LOG_DIR "/local-recv.log", "RECV_HDR",
        //              atomic_fetch_add(&local_recv_seq, 1) + 1,
        //              resp_header, sizeof(resp_header));
        vq_nr = resp_header[0] | (resp_header[1] << 8) |
                (resp_header[2] << 16) | (resp_header[3] << 24);
        index = resp_header[4] | (resp_header[5] << 8) |
                (resp_header[6] << 16) | (resp_header[7] << 24);
        len   = resp_header[8] | (resp_header[9] << 8) |
                (resp_header[10] << 16) | (resp_header[11] << 24);

        // index is original offset, maybe switched
        // only smaller offset can be switched to larger one (0 is unswitched)
        index = mapping[index] == 0 ? index : mapping[index];
        elem = comm_ctx->ring[index];
        force_printf("[resp_listener] fetch elem at [offset:%d,addr:%p]",
                     index, elem);

        vq = lookup_vq(vdev, vq_nr);
        if (!vq) {
            // force_printf("[resp_listener] vq [%d] cannot found.", vq_nr);
            goto elem_err;
        }

        buf = g_new0(char, len);
        read_cnt = 0;
listen_data:
        phase = 2;
        while (read_cnt < len) {
            sqe = io_uring_get_sqe(resp_uring);
            io_uring_prep_recv(sqe, stub, buf + read_cnt, len - read_cnt, 0);
            io_uring_submit(resp_uring);
            io_uring_wait_cqe(resp_uring, &cqe);
            if (cqe->res <= 0) {
                io_uring_cqe_seen(resp_uring, cqe);
                g_free(buf);
                goto link_err;
            }
            read_cnt += cqe->res;
            // force_printf("[resp_listener] recv data at [cqe->res: %d, read_cnt: %d, need: %d]",
            //               cqe->res, read_cnt, len);
            io_uring_cqe_seen(resp_uring, cqe);
        }
        // log_hex_dump(LOCAL_LOG_DIR "/local-recv.log", "RECV",
        //              atomic_fetch_add(&local_recv_seq, 1) + 1,
        //              (uint8_t *)buf, len);
        // iov_from_buf(elem->in_sg, elem->in_num, 0, buf, len);
        // we copy ourselves
        int copied = 0;
        for (int i = 0; i < elem->in_num && copied < len; i++) {
            int delta = MIN(elem->in_sg[i].iov_len, len - copied);
            memcpy(elem->in_sg[i].iov_base, buf + copied, delta);
            // log_hex_dump_iov(LOCAL_LOG_DIR "/iov-from-buf.log", "RECV", 0,
            //                  elem->in_sg + i, 1);
            // log_hex_dump(LOCAL_LOG_DIR "/iov-from-buf.log", "RECV", 0,
            //              (uint8_t *)(buf + copied), delta);
            copied += delta;
        }

        elem->len = len;
        g_free(buf);
        // force_printf("[resp_listener] push elem [%d] to vq [%d] with len [%d]", index, vq_nr, len);

        // switch offset [index] to recv
        if (index != (tmp_recved % RING_SIZE)) {
            VirtQueueElement *tmp = comm_ctx->ring[index];
            comm_ctx->ring[index] = comm_ctx->ring[tmp_recved % RING_SIZE];
            comm_ctx->ring[tmp_recved % RING_SIZE] = tmp;
            mapping[tmp_recved] = index;
            force_printf("[resp_listener] switch elem at [offset:%d] to [recved:%d], recording [mapping[recved]:%d]",
                         index, tmp_recved % RING_SIZE, mapping[tmp_recved]);
        }

done:
        virtqueue_push(vq, elem, elem->len);
        qatomic_fetch_inc(&comm_ctx->recved);
        sem_post(&comm_ctx->sem2);
    }

    // force_printf("[resp_listener] return");
    qatomic_set(&comm_ctx->recving, false);
    sem_post(&comm_ctx->sem2);
    return NULL;

link_err:
    if (!reconnect_tcp_socket(stub)) {
        if (buf)
            g_free(buf);
        // force_printf("[resp_listener] reconnection error");
        exit(0);
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

    // force_printf("[route_to_remote] for vdev %s", virtqueue_get_vdev_id(vq));

    while ((elem = virtqueue_pop(vq, sizeof(VirtQueueElement)))) {
        // send data as [vq_nr, index, out_len, in_len, out_data]
        struct iovec *msg_sg = g_new0(struct iovec, elem->out_num + 1);
        int tmp_sent = qatomic_read(&comm_ctx->sent);
        int header[4];
        header[0] = vq_nr;
        // header[1] = elem->index;
        header[1] = tmp_sent % RING_SIZE;
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
        sqe = io_uring_get_sqe(send_uring);
        io_uring_prep_sendmsg_zc(sqe, stub, &msg, 0);
        io_uring_sqe_set_data(sqe, msg_sg[0].iov_base);
        io_uring_submit(send_uring);

        force_printf("[route_to_remote] sent header [vq_nr:%d, index:%d, out_len:%d, in_len:%d] with [out_num:%d in_num:%d]",
                     header[0], header[1], header[2], header[3], elem->out_num, elem->in_num);

        // log_hex_dump_iov(LOCAL_LOG_DIR "/local-send.log", "SEND_HDR",
        //                  atomic_fetch_add(&local_send_seq, 1) + 1,
        //                  msg_sg, 1);
        // log_hex_dump_iov(LOCAL_LOG_DIR "/local-send.log", "SEND",
        //                  atomic_fetch_add(&local_send_seq, 1) + 1,
        //                  msg_sg + 1, elem->out_num);

        io_uring_wait_cqe(send_uring, &cqe);
        io_uring_cqe_seen(send_uring, cqe);

        if (cqe->flags & IORING_CQE_F_MORE) {
            io_uring_wait_cqe(send_uring, &cqe);
            io_uring_cqe_seen(send_uring, cqe);
        }

        g_free(msg_sg);

        // force_printf("[route_to_remote] sent success");

        // neglect full first
        force_printf("[route_to_remote] send elem at offset [%d]", header[1]);
        comm_ctx->ring[tmp_sent % RING_SIZE] = elem;
        qatomic_fetch_inc(&comm_ctx->sent);
        // force_printf("[route_to_remote] comm_ctx->sent turns to be [%d]", qatomic_read(&comm_ctx->sent));
        sem_post(&comm_ctx->sem1);
    }

    qatomic_set(&comm_ctx->sending, false);
    sem_post(&comm_ctx->sem1);

    return NULL;
}

static void remote_virtio_queue_notify_vq(VirtQueue *vq)
{
    // force_printf("[remote_virtio_queue_notify_vq] vq[%d] of vdev[%s] has been notified",
    //              virtio_get_queue_index(vq), virtqueue_get_vdev_id(vq));
    if (virtqueue_get_vring_desc(vq)) {
        VirtIODevice *vdev = virtqueue_get_vdev(vq);
        int stub = GPOINTER_TO_UINT(g_hash_table_lookup(gsi_stubs, DEVICE(vdev)->id));

        if (unlikely(vdev->broken)) {
            return;
        }

        // trace_virtio_queue_notify(vdev, vq - vdev->vq, vq);
        CommCTX *comm_ctx = g_hash_table_lookup(gsi_ctxes, DEVICE(vdev)->id);

        if (qatomic_cmpxchg(&comm_ctx->used, false, true)) {
            // force_printf("[remote_virtio_qeueue_notify_vq] comm_ctx is using");
            return;
        }

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
            g_free(comm_ctx->ring[comm_ctx->notified % RING_SIZE]);

            comm_ctx->notified++;
        }

        sem_destroy(&comm_ctx->sem1);
        sem_destroy(&comm_ctx->sem2);
        g_free(comm_ctx->ring);
        comm_ctx->ring = NULL;
        qatomic_set(&comm_ctx->used, false);

        if (unlikely(vdev->start_on_kick)) {
            virtio_set_started(vdev, true);
        }
    }
    // force_printf("[remote_virtio_queue_notify_vq] return");
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
    // force_printf("[remote_virtio_device_start_ioeventfd_impl] for vdev:%s", DEVICE(vdev)->id);

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
