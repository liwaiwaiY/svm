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

#define IO_URING_DEPTH 32 // maximum concurrent reqs

// each vq has a socket, but the resp must be in the same vq with req
// <K:vdev->name, V:socket*> for sender
GHashTable *gsi_stubs = NULL;
// <K:vdev->name, V:GHashTable*> for listener
GHashTable *gsi_tables = NULL;
// <K:elem->index, V:elemt> for listener/recver; need to find in gsi_tables
// GHashTable *gsi_elems = NULL;

extern VirtQueueElement* virtqueue_split_pop(VirtQueue* vq, size_t sz);
extern VirtQueueElement* virtqueue_packed_pop(VirtQueue* vq, size_t sz);

static struct io_uring *remote_uring = NULL;
static pthread_mutex_t  rw_lock;
static pthread_cond_t   rw_cond;

static int sent, recved;
static bool sending = false;

int remote_uring_init(void)
{
    pthread_mutex_init(&rw_lock, NULL);
    pthread_cond_init(&rw_cond, NULL);
    int ret = io_uring_queue_init(IO_URING_DEPTH &remote_uring, 0);
    if (ret < 0) {
        fprintf(stderr, "io_uring init failed\n");
        return -1;
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
    
// cmsvmTODO v2: try decrease the memory region here, maybe flexible array
void init_remote_virtio_device_sockets(VirtIODevice *vdev, const char *ip_port, Error **errp)
{
    // spli ip and port form "ip_port"(xxxx.xxxx.xxxx.xxxx@xxxx)
    char ip[64];
    int port;
    char *at_pos = strchr(ip_port, '@');
    if (at_pos == NULL) {
        return err;
    }
    int ip_len = at_pos - ip_port;
    strncpy(ip, ip_port, ip_len);
    ip[ip_len] = '\0';
    *port = atoi(at_pos + 1);

    // alloc sockets
    SOCKET_RV *stubs = malloc(sizeof(SOCKET_RV) * VIRTIO_QUEUE_MAX);
    for (int n = 0; n < VIRTIO_QUEUE_MAX; n++) {
        if (!virtio_queue_get_num(vdev, n)){
            stubs[n] = -1;
            continue;
        }
        VirtQueue *vq = &vdev->vq[n];
        stubs[n] = socket(AF_INET, SOCK_STREAM, 0);
        if (stubs[n] < 0) {
            goto erro;
        }
        struct sockaddr_in serv_addr;
        memset(&serv_addr, 0, sizeof(serv_addr));
        serv_addr.sin_family = AF_INET;
        serv_addr.sin_port   = htons(PORT);
        if (inet_pton(AF_INET, "127.0.0.1", &serv_addr.sin_addr) <= 0)
        {
            close(sock_fd);
            goto erro;
        }
        if (connect(sock_fd, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) < 0)
        {
            close(sock_fd);
            goto erro;
        }
        enable_tcp_keepalive(stubs[n]);
    }

    if (!gsi_stubs)
        gsi_stubs = g_hash_table_new(g_direct_hash, g_direct_equal);
    if (!gsi_tables)
        gsi_tables = g_hash_table_new(g_direct_hash, g_direct_equal);

    g_hash_table_insert(gsi_stubs, vdev->name, stubs);

    return;

goto erro;
    // write errp
    return;
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
    SOCKET_RV *stubs = g_hash_table_lookup(gsi_stubs, vdev->name);
    for (int n = 0; n < VIRTIO_QUEUE_MAX; n++) {
        if (stubs[n] >= 0)
            close(stubs[n]);
    }
}

/* device usage don't need recover desc meta
*  send: [index, sg1, ...]
*  recv: [index, data]  --> regard elem->out_num = 1; elem->out_sg[0].io_base = data;
*/

/* vq resp need recover desc meta
*  resp: [index, data_len, data]
*  recv: [index, data_len, sg1, ...]
*/

typedef struct ListenerParam {
    VirtQueue *vq;
    SOCKET_RV stub;
} ListenerParam;

void resp_listener(ListenerParam* param)
{
    VirtQueue *vq = param->vq;
    SOCKET_RV stub = param->stub;
    struct io_uring_sqe *sqe;
    struct io_uring_cqe *cqe;
    uint8_t resp_index[sizeof(int)], resp_len[sizeof(int)];
    int index, len;
    int read_cnt, phase;
    VirtQueueElement *elem;
    char* buf;
    GHashTable *gsi_elems = g_hash_table_lookup(gsi_tables, vq->vdev->name);
    if (gsi_elems) {
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

        if (!sending)
            break;
        
        read_cnt = 0;
listen_index:
        phase = 1;
        while (read_cnt < sizeof(int)) { // index
            sqe = io_uring_get_sqe(remote_uring);
            io_uring_prep_recv(sqe, stub, resp_index + read_cnt, 4 - read_cnt);
            io_uring_submit(remote_uring);
            // blocking, do not consume cpu cycles
            io_uring_wait_cqe(remote_uring, &cqe);
            if (cqe->res <= 0) {
                io_uring_cqe_seen(remote_uring, cqe);
                goto link_err;
            }
            read_cnt += cqe->res;
            io_uring_cqe_seen(remote_uring, cqe);
        }
        index = (resp_index[0] << 24) | (resp_index[1] << 16) | (resp_index[2] << 8) | resp_index[3];

        read_cnt = 0;
listen_len:
        phase = 2;
        while(read_cnt < sizeof(int)) { // len
            sqe = io_uring_get_sqe(remote_uring);
            io_uring_prep_recv(sqe, stub, resp_len + read_cnt, 4 - read_cnt);
            io_uring_submit(remote_uring);
            // blocking, do not consume cpu cycles
            io_uring_wait_cqe(remote_uring, &cqe);
            if (cqe->res <= 0) {
                io_uring_cqe_seen(remote_uring, cqe);
                goto link_err;
            }
            read_cnt += cqe->res;
            io_uring_cqe_seen(remote_uring, cqe);
        }
        len = (resp_len[0] << 24) | (resp_len[1] << 16) | (resp_len[2] << 8) | resp_len[3];

        elem = g_hash_table_lookup(gsi_elems, GINT_TO_POINTER(index));
        if (!elem)
            goto elem_err;

        // cmsvmtodo: maybe more efficient way?
        // version1: move socket data to userspace buffer, then recover as sg_table
        buf = (char*)malloc(len);
        read_cnt = 0;
listen_data:
        phase = 3;
        while (read_cnt < len) {
            sqe = io_uring_get_sqe(remote_uring);
            io_uring_prep_recv(sqe, stub, buf + read_cnt, len - read_cnt);
            io_uring_submit();
            // blocking
            io_uring_wait_cqe(remote_uring, &cqe);
            if (cqe->res <= 0) {
                io_uring_cqe_seen(cqe);
                goto link_err;
            }
            read_cnt += cqe->res;
            io_uring_seen(cqe);
        }

        // push to vq
        virtqueue_push(vq, elem, elem->len);

        // clear hash
        g_hash_table_remove(gsi_elems, GINT_TO_POINTER(index));
        recved++;
    }

    return;

// cmsvmtodo: error handling
hash_err:
    return;
link_err:
    if (!reconnect_tcp_socket(param->stub) && buf) {
        // reconnect failing
        free(buf);
        return;
    };
    switch (phase) {
    case 0:
        goto listen_begin;
    case 1:
        goto listen_index;
    case 2:
        goto listen_len;
    case 3:
        goto listen_data;
    default:
        return;
    }
elem_err:
    return;
}

void route_to_remote(VirtQueue *vq, SOCKET_RV stub)
{
    VirtQueueElement* (*remote_virtqueue_pop)(VirtQueue, size_t);
    VirtQueueElement *elem;
    struct io_uring_sqe *sqe;
    struct io_uring_cqe *cqe;
    GHashTable *gsi_elems = g_hash_table_lookup(gsi_tables, vq->vdev->name);

    if (virtio_device_disabled(vq->vdev)) {
        return 0;
    }

    if (virtio_vdev_has_feature(vq->vdev, VIRTIO_F_RING_PACKED)) {
        remote_virtqueue_pop = virtqueue_packed_pop;
    } else {
        remote_virtqueue_pop = virtqueue_split_pop;
    }

    while (elem = remote_virtqueue_pop(vq, sizeof(VirtQueueElement))) {
        // sent to remote
        struct iovec msg_sg[elem->out_num + 1];
        msg_sg[0].iov_base = malloc(sizeof(int));
        memcpy(msg_sg[0].iov_base, &elem->index, sizeof(int));
        msg_sg[0].iov_len = sizeof(int);
        memcpy(msg_sg + 1, elem->out_sg, elem->out_num);
        sqe = io_uring_get_sqe(remote_uring);
        io_uring_prep_send_zc(sqe, stub, msg_sg, elem->out_num + 1, 0);
        io_uring_sqe_set_data(sqe, msg_sg);
        io_uring_submit(remote_uring);
        // synchronous waiting for strong ordering
        io_uring_wait_cqe(remote_uring, &cqe);
        free(msg_sg[0]);

        // a new resp is needed
        if (elem->in_num > 0) {
            // store elem for listener
            g_hash_table_add(gsi_elems, GINT_TO_POINTER(elem->index), elem);
            sent++;
            pthread_cond_signal(&rw_cond);
        } else {
            // handle done
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
        // require vdev to use vq sequentially
        SOCKET_RV remote_stubs[] = g_hash_table_lookup(gsi_rvdevs, vdev->name);

        if (unlikely(vdev->broken)) {
            return;
        }

        if (remote_uring)
            remote_uring_init();

        trace_virtio_queue_notify(vdev, vq - vdev->vq, vq);
        // create a listener
        sending = true;
        QemuThread listener;
        ListenerParam *param = g_new0(ListenerParam, 1);
        param->vq = vq;
        param->stub = remote_stubs[vq - vdev->vq];
        qemu_thread_create(&listener, "remote_virtqueu_listener",
                           resp_listener, param, QEMU_THREAD_JOINABLE);
        // route to remote machine
        route_to_remote(vq, remote_stubs[vq - vdev->vq]);
        // send over, but may not recv all resp
        sending = false;
        pthread_cond_signal(&rw_cond);
        // wait listener exit
        qemu_thread_join(&resp_thread);

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
}