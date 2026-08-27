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

/* the single definition backing the header's extern (see virtio-remote.h) */
int env_tag;

void chenv(int new_env)
{
    if (new_env != VIRTIO_LOCAL_ENV && new_env != VIRTIO_REMOTE_ENV)
        return;
    env_tag = new_env;
}

int check_env(int tar_env)
{
    return env_tag == tar_env;
}

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

/* -------------- Pipelining ------------- */
/*
* Local QEMU and remote stub has different pipelining policies. Thereby, their
* zero copy is different as well. Local QEMU is pop-send-resp-push. Inflight is
* used in (pop-send)-inflight-(resp-push).-'h
*/

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
    /* local side: resp len recorded by the response handler while the slot
     * waits for the completion. Copied in on publish (slot->zc = *zc), which
     * both seeds len_known and resets the slot on reuse. */
    unsigned int push_len;    /* resp len; valid once len_known */
    bool len_known;
} ZcPending;

typedef struct InflightSlot {
    /* NULL = slot free. Local side: VirtQueueElement *; stub side: StubResp *
     * (the stub's window). */
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
    /* local-side backpressure: when the window is full the send worker (the
     * sole producer) waits on full_cond (holding full_lock = vq_lock) before
     * publishing; the recv worker signals from inflight_clear after advancing
     * head. NULL on the stub - its push path has its own push_lock/push_cond
     * wait, and the M3 windows are only configured on the local side. */
    GCond *full_cond;
    GMutex *full_lock;
} Inflight;

/* ---- diagnostic ring: the last clear() calls, for freeze forensics.
 * Written lock-free (each consumer writes one slot); dump with gdb after a
 * freeze, or abort() fires when a clear observes head >= tail (the tail read
 * returned a stale value from the middle of a publish). */
#define DBG_RING_N 65536
typedef struct {
    uint32_t seq, ohead, otail, newhead, tail_after;
    unsigned long tid;
} DbgClear;
static DbgClear dbg_ring[DBG_RING_N];
static int dbg_idx;

/* ---- diagnostic event ring (freeze forensics). Lock-free: each writer
 * claims a slot with an atomic bump of the position counter, then fills it.
 * Dump the tail with gdb after a freeze (local/stub both build this). */
#define VR_EV_N 262144
typedef struct {
    int64_t ts;            /* g_get_monotonic_time(), us */
    uint16_t type;         /* VR_EV_* */
    uint16_t vq;           /* virtio queue index, 0xffff if none */
    uint32_t a, b, c;
} VrEv;
static VrEv vr_ev[VR_EV_N];
static uint64_t vr_ev_pos; /* qatomic */
/* event types (VR_EV_*) are declared in virtio-remote.h so virtio.c and
 * virtio-blk.c can log into the same ring via vr_ev_log_ext() */
static inline void vr_ev_log(uint16_t type, VirtQueue *vq,
                             uint32_t a, uint32_t b, uint32_t c)
{
    unsigned int p = (unsigned int)qatomic_fetch_add(&vr_ev_pos, 1) % VR_EV_N;
    VrEv *e = &vr_ev[p];
    e->ts = g_get_monotonic_time();
    e->type = type;
    e->vq = vq ? (uint16_t)virtio_get_queue_index(vq) : 0xffff;
    e->a = a; e->b = b; e->c = c;
}

/* exported wrapper for virtio.c / virtio-blk.c (attach/detach/drain events) */
void vr_ev_log_ext(uint16_t type, VirtQueue *vq,
                   uint32_t a, uint32_t b, uint32_t c)
{
    vr_ev_log(type, vq, a, b, c);
}

/* SIGUSR1: dump the tail of the event ring to a file (no gdb/root needed).
 * Registered once in vr_config_init. */
static GMutex vr_ev_dump_lock;
static void vr_ev_dump_handler(int sig)
{
    /* several vqs tear down concurrently and each calls this dump; serialize
     * the non-signal (crash/conn-lost) path so the O_TRUNC writers do not
     * corrupt each other's output (observed: torn mid-line entries) */
    bool from_thread = (sig == 0);
    if (from_thread) {
        g_mutex_lock(&vr_ev_dump_lock);
    }
    const char *path = check_env(VIRTIO_REMOTE_ENV)
                       ? "/tmp/vr_ring_dump_stub.txt"
                       : "/tmp/vr_ring_dump_local.txt";
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        if (from_thread) {
            g_mutex_unlock(&vr_ev_dump_lock);
        }
        return;
    }
    char buf[256];
    uint64_t pos = vr_ev_pos;
    int n = snprintf(buf, sizeof(buf), "vr_ev_pos=%llu\n",
                     (unsigned long long)pos);
    write(fd, buf, n);
    unsigned int maxn = 131072;
    uint64_t start = pos > maxn ? pos - maxn : 0;
    for (unsigned int k = 0; k < maxn; k++) {
        VrEv *e = &vr_ev[(unsigned int)((start + k) % VR_EV_N)];
        if (!e->type) {
            continue;
        }
        n = snprintf(buf, sizeof(buf),
                     "E type=%u vq=%u a=%u b=%u c=%u ts=%ld\n",
                     e->type, e->vq, e->a, e->b, e->c, (long)e->ts);
        write(fd, buf, n);
    }
    /* second pass: every LHDR/SHDR anywhere in the ring (ring order =
     * chronological), so a wrapped ring still yields the full resp-header
     * chain for reconciliation against the stub's SHDR/SSEND accounting */
    for (unsigned int k = 0; k < VR_EV_N; k++) {
        VrEv *e = &vr_ev[(unsigned int)((pos + k) % VR_EV_N)];
        uint16_t t = e->type;
        if (t != VR_EV_LHDR && t != VR_EV_SHDR) {
            continue;
        }
        n = snprintf(buf, sizeof(buf),
                     "H type=%u vq=%u a=%u b=%u c=%u ts=%ld\n",
                     t, e->vq, e->a, e->b, e->c, (long)e->ts);
        write(fd, buf, n);
    }
    close(fd);
    if (from_thread) {
        g_mutex_unlock(&vr_ev_dump_lock);
    }
}

/* print the last n resp-header parses (VR_EV_LHDR) to stderr: the crash-site
 * chain must survive even if the ring dump races the event flood */
static void vr_ev_dump_lhdr_stderr(unsigned int n)
{
    uint64_t pos = vr_ev_pos;
    unsigned int found = 0;
    for (unsigned int k = 1; k <= VR_EV_N && found < n; k++) {
        VrEv *e = &vr_ev[(unsigned int)((pos - k) % VR_EV_N)];
        if (!e->type || e->type != VR_EV_LHDR) {
            continue;
        }
        error_report("last-LHDR[%u]: vq_nr=%u seq=%u len=%u ts=%ld",
                     found, e->a, e->b, e->c, (long)e->ts);
        found++;
    }
}

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
 * Stub side: the same window type, one per vq (handle worker's push -> send
 * worker), sized pow2ceil(vring.num); the request side is window-less (the
 * handle worker parses and processes inline). head/tail are used directly as
 * the seq allocator/consumer: the producer publishes at its own tail, the
 * consumer clears the head slot.
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
 * inf may be NULL (a ctx on the other side that never allocates the window).
 *
 * The tail snapshot is NOT merely a memory barrier: a consumer must never
 * treat a slot as published until it has observed tail > seq. acquire is
 * one-way - observing a stale tail does NOT prove the slot writes are
 * invisible, so a publish that has written elem/seq but not yet advanced
 * tail must not be consumable. seq >= tail means the publish is incomplete:
 * consuming it lets a later inflight_clear() see the stale tail and stop its
 * hole-skip on the just-cleared slot, wedging head on a NULL hole (freeze). */
static void *inflight_lookup(Inflight *inf, unsigned int seq)
{
    InflightSlot *slot;

    if (!inf) {
        return NULL;
    }
    uint32_t tail = qatomic_load_acquire(&inf->tail);
    if (seq >= tail) {
        return NULL; /* not fully published yet (tail not advanced past it) */
    }
    slot = inflight_slot(inf, seq);
    return (slot->seq == seq) ? slot->elem : NULL;
}

/* publish an elem into the window (producer; caller owns the slot).
 * zc == NULL: plain send, no zc bookkeeping (slot->is_zc = false). */
static void inflight_publish(Inflight *inf, unsigned int seq, void *elem,
                             ZcPending *zc)
{
    InflightSlot *slot;

    vr_debug("vremote: inflight pub seq=%u head=%u tail=%u",
             seq, qatomic_load_acquire(&inf->head),
             qatomic_load_acquire(&inf->tail));
    /* local-side backpressure (full_cond is bound only on the local side,
     * see remote_vq_ctx_init): the sole producer (the vq's send worker)
     * blocks here while the target slot is still occupied, instead of
     * overflowing the window and clobbering a live entry. The wait is on the
     * *slot* (not on seq - head) because responses arrive in disk-completion
     * order: while a slow request keeps head stuck, its cleared holes are
     * reused by newer seqs, so seq - head alone no longer tells whether the
     * window is really full - a head-relative test would stall the whole
     * pipeline on a single late completion (40ms tail on the slow disk
     * request). The recv worker signals from inflight_clear after clearing a
     * slot. The caller holds full_lock (== vq_lock), which g_cond_wait()
     * releases while blocked, so the recv worker can still take vq_lock to
     * push used-ring entries and clear slots. */
    if (inf->full_cond) {
        while (inflight_slot(inf, seq)->elem != NULL) {
            g_cond_wait(inf->full_cond, inf->full_lock);
        }
    }
    assert(inf && inflight_slot(inf, seq)->elem == NULL);
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
    uint32_t ohead = head, otail = tail;
    /* advance past every seq that is no longer in flight. The stop test is
     * "slot of seq=head still holds seq=head", NOT "slot->elem != NULL":
     * free-slot publishing reuses a cleared slot for a *newer* seq while
     * head waits on a slow (out-of-order) completion, so the head slot may
     * be occupied by an elem whose seq != head. seq=head is in flight iff
     * its slot holds elem with seq == head (a cleared slot keeps a stale seq
     * but NULLs elem, and a reused slot overwrites seq). */
    while (head < tail) {
        InflightSlot *s = inflight_slot(inf, head);
        if (s->elem != NULL && s->seq == head) {
            break; /* seq=head still in flight */
        }
        head++; /* seq=head completed (slot cleared and maybe reused) */
    }
    int di = qatomic_fetch_add(&dbg_idx, 1) & (DBG_RING_N - 1);
    dbg_ring[di].seq = seq;
    dbg_ring[di].ohead = ohead;
    dbg_ring[di].otail = otail;
    dbg_ring[di].newhead = head;
    qatomic_store_release(&inf->head, head);
    dbg_ring[di].tail_after = qatomic_load_acquire(&inf->tail);
    dbg_ring[di].tid = (unsigned long)pthread_self();
    if (inf->full_cond) {
        /* local backpressure: a send worker blocked on the full M3 window
         * may now have room (head advanced); wake it. The signal MUST be
         * issued while holding full_lock (== vq_lock): a send worker that
         * has checked the window-full condition but not yet registered as a
         * waiter would otherwise miss the wakeup and block forever, even
         * though head has advanced. Every local-side caller holds vq_lock. */
        g_cond_signal(inf->full_cond);
    }
    vr_debug("vremote: inflight clear seq=%u head->%u tail=%u", seq, head,
             tail);
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

/* -------------- Runtime config (experiment knobs) --------------
 * All experiment parameters are read from the environment once at first use
 * (VR_*). This keeps a single build: each experiment varies one knob at a
 * time (env var) without recompiling. Defaults match the old compile-time
 * values. local qemu and the stub are the same binary and both read the same
 * variables, so a driver script must export the same VR_* for both.
 */
static uint32_t vr_zc_send_min;
static unsigned vr_workers;
static unsigned vr_inflight_size;
static unsigned vr_local_batch_n;
static unsigned vr_local_batch_m;
static unsigned vr_stub_batch_n;
static unsigned vr_stub_batch_m;
static unsigned vr_stub_queue_max; /* recv-batch cap: max reqs queued between
                                    * recv and handle before parsing pauses */
static unsigned vr_stub_merge_m;   /* M5: merge one req's in_sg entries into a
                                    * single buffer when total in-bytes <= this;
                                    * 0 = keep per-sg buffers (original) */
static bool vr_buf_pool_on;

static void buf_pool_init(void);

/* runtime config file, next to this source file (write once, no portability
 * concerns). Missing file or missing keys fall back to the defaults below. */
#define VR_CONFIG_PATH "/home/waiai/svm/local_qemu/hw/virtio-remote/vr.conf"

static void vr_config_init(void)
{
    static bool inited;
    FILE *fp;
    char line[256];

    if (inited) {
        return;
    }
    inited = true;
    signal(SIGUSR1, vr_ev_dump_handler);
    if (check_env(VIRTIO_REMOTE_ENV)) {
        /* The stub has no vCPU threads and blocks SIGUSR1 (SIG_IPI) in the
         * main thread, so SIGUSR1 can never be delivered there. SIGUSR2 is
         * unblocked in the stub's main thread and unused by the ucontext
         * coroutine backend, so it is a reliable dump trigger on the stub. */
        signal(SIGUSR2, vr_ev_dump_handler);
    }
    /* defaults (same as the pre-config behavior) */
    vr_zc_send_min   = 4 * 1024;
    vr_workers       = 4;
    vr_inflight_size = 0;
    vr_local_batch_n = 4;
    vr_local_batch_m = 64 * 1024;
    vr_stub_batch_n  = 4;
    vr_stub_batch_m  = 64 * 1024;
    vr_stub_queue_max = 0; /* 0 = unlimited (current behavior) */
    vr_stub_merge_m   = 0; /* 0 = per-sg in buffers (original behavior) */
    vr_buf_pool_on   = false;

    fp = fopen(VR_CONFIG_PATH, "r");
    if (!fp) {
        return;    /* no config file: keep defaults */
    }
    while (fgets(line, sizeof(line), fp)) {
        char *eq, *key, *val, *e;

        key = line;
        while (*key == ' ' || *key == '\t') {
            key++;
        }
        if (*key == '#' || *key == '\n' || *key == '\0') {
            continue;
        }
        eq = strchr(key, '=');
        if (!eq) {
            continue;
        }
        *eq = '\0';
        val = eq + 1;
        /* trim key/value trailing whitespace */
        for (e = key + strlen(key); e > key &&
             (e[-1] == ' ' || e[-1] == '\t'); e--) {
            *e = '\0';
        }
        for (e = val + strlen(val); e > val &&
             (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\n' ||
              e[-1] == '\r'); e--) {
            *e = '\0';
        }

        if (strcmp(key, "zc_send_min") == 0) {
            vr_zc_send_min = strtoul(val, NULL, 0);
        } else if (strcmp(key, "workers") == 0) {
            vr_workers = strtoul(val, NULL, 0);
        } else if (strcmp(key, "inflight_size") == 0) {
            vr_inflight_size = strtoul(val, NULL, 0);
        } else if (strcmp(key, "local_batch_n") == 0) {
            vr_local_batch_n = strtoul(val, NULL, 0);
        } else if (strcmp(key, "local_batch_m") == 0) {
            vr_local_batch_m = strtoul(val, NULL, 0);
        } else if (strcmp(key, "stub_batch_n") == 0) {
            vr_stub_batch_n = strtoul(val, NULL, 0);
        } else if (strcmp(key, "stub_batch_m") == 0) {
            vr_stub_batch_m = strtoul(val, NULL, 0);
        } else if (strcmp(key, "stub_queue_max") == 0) {
            vr_stub_queue_max = strtoul(val, NULL, 0);
        } else if (strcmp(key, "stub_merge_m") == 0) {
            vr_stub_merge_m = strtoul(val, NULL, 0);
        } else if (strcmp(key, "buf_pool") == 0) {
            vr_buf_pool_on = (*val == '1' ||
                              g_ascii_strncasecmp(val, "on", 2) == 0 ||
                              g_ascii_strncasecmp(val, "yes", 3) == 0 ||
                              g_ascii_strncasecmp(val, "true", 4) == 0);
        }
    }
    fclose(fp);

    if (vr_workers == 0) {
        vr_workers = 1;
    }
    if (vr_local_batch_n > VR_BATCH_MAX) {
        vr_local_batch_n = VR_BATCH_MAX;
    }
    if (vr_stub_batch_n > VR_BATCH_MAX) {
        vr_stub_batch_n = VR_BATCH_MAX;
    }
    buf_pool_init();
}

/* -------------- M5: stub resp-buffer pool --------------
 * The stub posix_memalign()s one page-aligned buffer per in_sg entry for
 * every request and frees it once the response is sent (or its zc completion
 * arrives). Under heavy small-request traffic (openssl speed, small blk
 * requests) that churns the heap. VR_BUF_POOL=1 makes the stub recycle those
 * buffers from a size-keyed pool instead, keeping the page-aligned pointers
 * alive across requests. Only the stub-side resp buffers are pooled (they are
 * the posix_memalign'd in_sg buffers); the local side uses g_malloc/g_free.
 * The pool is a single global instance guarded by one mutex: allocs happen on
 * the handle workers, frees on the send workers and device threads.
 */
#define VR_BUF_POOL_PGMAX    512     /* 512 pages = 2MB, bigger bufs share the last bucket */
#define VR_BUF_POOL_MAXFREE  1024    /* cap pooled buffers to bound memory */

typedef struct BufPool {
    GMutex lock;
    GSList *free[VR_BUF_POOL_PGMAX]; /* bucket b = buffers of (b+1) pages */
    unsigned int n_free;
    GHashTable *pages;               /* live ptr -> page count (bucket+1) */
    uint64_t hits, allocs, releases, reallocs;
} BufPool;

static BufPool *buf_pool;

static void buf_pool_init(void)
{
    if (!vr_buf_pool_on || buf_pool) {
        return;
    }
    buf_pool = g_new0(BufPool, 1);
    g_mutex_init(&buf_pool->lock);
    buf_pool->pages = g_hash_table_new(g_direct_hash, g_direct_equal);
}

/* page-aligned, page-multiple-sized buffer; recycled from the pool when on */
static void *buf_pool_alloc(size_t len)
{
    void *p;
    unsigned int pages;

    if (len == 0) {
        return NULL;
    }
    pages = QEMU_ALIGN_UP(len, getpagesize()) / getpagesize();
    if (pages > VR_BUF_POOL_PGMAX) {
        pages = VR_BUF_POOL_PGMAX;
    }
    if (buf_pool) {
        uint64_t total;
        g_mutex_lock(&buf_pool->lock);
        GSList *l = buf_pool->free[pages - 1];
        if (l) {
            buf_pool->free[pages - 1] = l->next;
            p = l->data;
            g_slist_free_1(l);
            buf_pool->n_free--;
            /* hand it out again: track it so buf_pool_free can route it back */
            g_hash_table_insert(buf_pool->pages, p, GINT_TO_POINTER(pages));
            buf_pool->hits++;
            total = buf_pool->hits + buf_pool->allocs;
            if (total % 65536 == 0) {
                vr_debug("buf pool: hits=%llu allocs=%llu releases=%llu "
                         "reallocs=%llu n_free=%u",
                         (unsigned long long)buf_pool->hits,
                         (unsigned long long)buf_pool->allocs,
                         (unsigned long long)buf_pool->releases,
                         (unsigned long long)buf_pool->reallocs,
                         buf_pool->n_free);
            }
            g_mutex_unlock(&buf_pool->lock);
            return p;
        }
        buf_pool->allocs++;
        total = buf_pool->hits + buf_pool->allocs;
        if (total % 65536 == 0) {
            vr_debug("buf pool: hits=%llu allocs=%llu releases=%llu "
                     "reallocs=%llu n_free=%u",
                     (unsigned long long)buf_pool->hits,
                     (unsigned long long)buf_pool->allocs,
                     (unsigned long long)buf_pool->releases,
                     (unsigned long long)buf_pool->reallocs,
                     buf_pool->n_free);
        }
        g_mutex_unlock(&buf_pool->lock);
    }
    if (posix_memalign(&p, getpagesize(),
                       QEMU_ALIGN_UP(len, getpagesize())) != 0) {
        return NULL;
    }
    if (buf_pool) {
        g_mutex_lock(&buf_pool->lock);
        g_hash_table_insert(buf_pool->pages, p, GINT_TO_POINTER(pages));
        g_mutex_unlock(&buf_pool->lock);
    }
    return p;
}

/* recycle or release a buffer. Non-pooled pointers (g_malloc'd resp headers,
 * iovs etc.) are just free()d - the pages hash lookup misses on them. */
static void buf_pool_free(void *p)
{
    gpointer pages_key;

    if (!p) {
        return;
    }
    if (!buf_pool) {
        free(p);
        return;
    }
    g_mutex_lock(&buf_pool->lock);
    pages_key = g_hash_table_lookup(buf_pool->pages, p);
    if (!pages_key) {
        g_mutex_unlock(&buf_pool->lock);
        free(p);
        return;
    }
    unsigned int pages = GPOINTER_TO_INT(pages_key);
    g_hash_table_remove(buf_pool->pages, p);
    if (buf_pool->n_free < VR_BUF_POOL_MAXFREE) {
        buf_pool->free[pages - 1] =
            g_slist_prepend(buf_pool->free[pages - 1], p);
        buf_pool->n_free++;
        buf_pool->releases++;
    } else {
        free(p);
        buf_pool->reallocs++;
    }
    g_mutex_unlock(&buf_pool->lock);
}

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
/* window space frees up on the recv side: wake the vq's send worker to drain
 * the vring again (defined after local_send_handler) */
static void local_drain_wake(VirtQueue *vq);

static bool local_zc_complete_one(RemoteVQueueCtx *ctx, VirtQueue *vq,
                                  InflightSlot *slot)
{
    /* serialize against the send worker's zc sendmsg + short-write tail
     * snapshot (local_send_msg holds zc_lock across both): the completion
     * frees the slot's header/lens/msg_sg and, for in_num == 0 requests,
     * pushes + frees the elem, so it must never run in the middle of a
     * resume entry being built from those originals. Lock order
     * zc_lock -> vq_lock (see the field comment in virtio-remote.h). */
    g_mutex_lock(&ctx->zc_lock);
    VirtQueueElement *elem = slot->elem;

    /* free the buffers the network stack no longer references */
    inflight_free_bufs(slot);

    /* resp has not arrived yet: keep the slot for the response handler */
    if (!slot->zc.len_known) {
        g_mutex_unlock(&ctx->zc_lock);
        return false;
    }

    /* resp arrived earlier: push the used ring entry now. inflight_clear
     * must run under vq_lock so its full_cond signal cannot be lost by a
     * send worker that is about to enter g_cond_wait on the full window. */
    g_mutex_lock(&ctx->vq_lock);
    virtqueue_push(vq, elem, slot->zc.push_len);
    virtio_notify(virtqueue_get_vdev(vq), vq);
    inflight_clear(ctx->inflight, slot->seq);
    g_mutex_unlock(&ctx->vq_lock);
    g_mutex_unlock(&ctx->zc_lock);
    g_free(elem);
    vr_ev_log(VR_EV_ZC, vq, slot->seq, 0, 0);
    local_drain_wake(vq);
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
            vr_debug("vremote: zc comp first=%u last=%u scan head=%u tail=%u",
                     first, last, head, tail);
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
static void stub_zc_drain(RemoteVQueueCtx *ctx, VirtQueue *vq)
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
            vr_ev_log(VR_EV_SZC, vq, first, last, 0);
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
                                buf_pool_free(zp->bufs[i]);
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
                    buf_pool_free(zp->bufs[i]);
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

typedef struct WorkerPool WorkerPool;

/* per-pool dispatch policy: record the task and wake the worker's bh. Local
 * uses a busy-claim + kick_pending accounting (opaque = clear_kick flag), the
 * stub an overwriting dispatch (opaque unused), so every caller goes through
 * the same worker_pool_dispatch() with a pool-specific opaque. */
typedef bool (*PoolDispatch)(WorkerPool *pool, VirtQueue *vq,
                             void (*fn)(void *opaque), void *opaque);

typedef struct Worker {
    AioContext *ctx;          /* this worker's event loop */
    QEMUBH *bh;               /* distributor -> worker handoff */
    QemuThread thread;
    Task task;    /* handoff, written by distributor, read by BH */
    int busy;                 /* 1 while a task is running (qatomic) */
    WorkerPool *pool;         /* owning pool (its flag drives kick replay) */
    /*
     * vqs that route kicks to this worker. Built once during machine setup
     * (worker_pool_register_vq), before any kick can be delivered, so the
     * pool's bh scans it lock-free; entries whose ctx is gone are skipped.
     */
    VirtQueue **vqs;
    unsigned int n_vqs;
    unsigned int vqs_cap;
} Worker;

/* common thread pool shared by both sides: a fixed number of worker threads,
 * each with its own aio context + bh. The dispatch policy and the worker bh
 * are registered per pool at setup and are invoked
 * through the common worker_pool_* entry points, so the local/stub sides only
 * supply the two callbacks (and a per-pool opaque at dispatch time). */
struct WorkerPool {
    Worker *workers;          /* pool of VIRTIO_REMOTE_WORKERS workers */
    bool is_send;             /* pool flag: send-pool kick replay policy */
    PoolDispatch dispatch;    /* registered dispatch policy */
    QEMUBHFunc *bh;           /* registered worker bh */
    const char *name;         /* thread name prefix */
};

static WorkerPool send_pool;
static WorkerPool recv_pool;

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

/* create worker threads. This is called only once, so we do not need locks and flags */
static void worker_pool_start(WorkerPool *pool)
{
    vr_config_init();
    pool->workers = g_new0(Worker, vr_workers);
    for (int i = 0; i < vr_workers; i++) {
        Worker *w = &pool->workers[i];
        w->ctx = aio_context_new(&error_abort);
        aio_context_set_poll_params(w->ctx, 0, 0, 0, &error_abort);
        w->busy = 0;
        w->pool = pool;
        w->bh = aio_bh_new(w->ctx, pool->bh, w);
        qemu_thread_create(&w->thread, pool->name, workflow, w, QEMU_THREAD_JOINABLE);
    }
}

/* register a pool's policy callbacks: thread name, pool flag, dispatch policy
 * and worker bh. Called once per pool at machine setup
 * (idempotent). */
static inline void worker_pool_init(WorkerPool *pool, const char *name, bool is_send,
                             PoolDispatch dispatch, QEMUBHFunc bh)
{
    pool->name = name;
    pool->is_send = is_send;
    pool->dispatch = dispatch;
    pool->bh = bh;
    worker_pool_start(pool);
}

/* lookup the worker thread of a vq in a pool by hash mapping
 * mosaic: vq_nr may be the same for different vdevs, hash confict! */
static inline Worker *worker_pool_worker(WorkerPool *pool, VirtQueue *vq)
{
    return &pool->workers[virtio_get_queue_index(vq) % vr_workers];
}

/* append a vq to its worker's re-scan list. Setup-time only, so the list is
 * append-only before any kick can arrive; the pool's bh scans it lock-free
 * (see the Worker::vqs comment). */
static void worker_pool_register_vq(WorkerPool *pool, VirtQueue *vq)
{
    Worker *w = worker_pool_worker(pool, vq);

    for (unsigned int i = 0; i < w->n_vqs; i++) {
        if (w->vqs[i] == vq) {
            return; /* already registered */
        }
    }
    if (w->n_vqs == w->vqs_cap) {
        w->vqs_cap = w->vqs_cap ? w->vqs_cap * 2 : 4;
        w->vqs = g_renew(VirtQueue *, w->vqs, w->vqs_cap);
    }
    w->vqs[w->n_vqs++] = vq;
}

/* common dispatch entry: submit one event to the vq's worker of the given
 * pool, routed through the pool's registered dispatch policy. Per-pool extra
 * arguments ride in opaque (local: clear_kick flag; stub: unused). */
static bool worker_pool_dispatch(WorkerPool *pool, VirtQueue *vq,
                                 void (*fn)(void *opaque), void *opaque)
{
    RemoteVQueueCtx *ctx = virtqueue_get_remote_ctx(vq);

    if (!ctx) {
        return false;
    }
    return pool->dispatch(pool, vq, fn, opaque);
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

/*
 * Disable Nagle on the per-vq data connections. Without this, a small send
 * (a request or a resp, both < MSS on loopback) is held until the previous
 * segment is ACKed; the receiver's delayed ACK (40ms) can then stall the
 * send for up to 40ms. This shows up as a ~40ms latency plateau on ~0.5% of
 * requests. The data plane is request/response (latency-sensitive), so the
 * micro-batching that Nagle would give is not wanted; the code already
 * batches multiple resps into a single sendmsg (stub_send_batch).
 */
static void enable_tcp_nodelay(int fd)
{
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
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
    enable_tcp_nodelay(socket);
    /* MSG_ZEROCOPY is enabled lazily on the first send (see local_send_msg):
     * it must run on the socket iothread, which owns the zc state */
    return true;
}

static void local_send_handler(void *opaque);
/* defined after LocalSendResume; parks a partially-queued zc elem's tail in
 * the resume state (the zc slot owns header/lens until the completion) */
static bool local_send_resume_park_zc(VirtQueue *vq, VirtQueueElement *elem,
                                      int *header, int *lens, uint32_t seq,
                                      unsigned int off, ssize_t total);
/* defined after LocalSendResume; parks a partially-queued copy elem's tail
 * (the zc fallback copy send) with copy semantics */
static bool local_send_resume_park_copy(VirtQueue *vq, VirtQueueElement *elem,
                                        int *header, int *lens, uint32_t seq,
                                        unsigned int off, ssize_t total,
                                        bool no_in);
/*
* called by io_write event, registered in local_send_msg
* when the kernel refuses to send because of lack of buffers, this handler is
* invoked once the socket becomes writable. The vring must only be touched by
* the vq's send worker thread, so this iothread-side handler only hands the vq
* back to its send worker (which re-drains the vring inside local_send_handler;
* the unpopped elem is popped again). If the worker is busy the io_write
* handler is kept, so the level-triggered G_IO_OUT re-fires once the socket
* is writable again.
*/
static void local_retry_send(void *opaque)
{
    VirtQueue *vq = opaque;
    RemoteVQueueCtx *ctx = virtqueue_get_remote_ctx(vq);

    if (ctx && qatomic_load_acquire(&ctx->dead)) {
        /* the stub is gone: never re-register the resp fd handler here - the
         * conn_err BH detaches (and closes) the fd */
        return;
    }
    if (!worker_pool_dispatch(&send_pool, vq, local_send_handler, NULL)) {
        /* worker busy: keep io_write registered; it will re-fire */
        return;
    }
    /* the worker re-drains the vring; drop the writable handler */
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
    vr_debug("vremote: local send vq %d seq %d out=%u in=%u", vq_nr, seq,
                 elem->out_num, elem->in_num);
    /* capture before the send: a resp-tracked elem (in_num > 0) is freed by
     * the recv worker as soon as the stub responds, so reading elem->in_num
     * after sendmsg() (the "no in-buffers" completion below) is a
     * use-after-free that pushed a stale/freed elem -> corrupted out_num ->
     * virtqueue_unmap_sg walked garbage sgs and segfaulted. in_num == 0
     * elems are send-worker-owned (the stub sends no resp), so the captured
     * flag is the only read we are allowed to make post-send. */
    bool no_in_bufs = (elem->in_num == 0);

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
    ssize_t ret;
    bool used_zc = false;
    bool zc_eligible = st && st->enabled && total >= vr_zc_send_min;
    uint32_t zc_serial = 0;

    /*
     * Publish the slot BEFORE the send, on every path. The stub can respond
     * as soon as the request bytes hit the socket, and a zc completion can
     * be drained as soon as sendmsg() returns; publishing only afterwards
     * lets the recv worker match a resp/completion against a window that
     * does not contain this seq yet, losing it forever (the slot never
     * completes -> head stalls -> window overflow -> "no pending elem" /
     * publish assert). With the slot published first the recv worker always
     * finds it. On a failed send the slot is left as a hole below (head
     * skips it), and the zc serial is handed back.
     */
    if (zc_eligible) {
        void **zc_bufs = g_new(void *, 3);
        zc_bufs[0] = header;
        zc_bufs[1] = lens;
        zc_bufs[2] = msg_sg;
        ZcPending zc = {
            .bufs = zc_bufs,
            .n_bufs = 3,
            .push_len = 0,
            .len_known = elem->in_num == 0,
        };
        g_mutex_lock(&ctx->vq_lock);
        zc.serial = st->serial++;
        inflight_publish(ctx->inflight, seq, elem, &zc);
        g_mutex_unlock(&ctx->vq_lock);
        zc_serial = zc.serial;

        /* zc_lock spans the sendmsg + the short-write tail snapshot: the zc
         * completion (drained by the recv worker, see local_zc_complete_one)
         * frees the slot's header/lens/msg_sg and, for in_num == 0 requests,
         * pushes + frees the elem as soon as sendmsg() returns, so it must
         * not run in the middle of the resume entry being built from those
         * originals. */
        g_mutex_lock(&ctx->zc_lock);
        ret = sendmsg(ctx->resp_fd, &msg, MSG_ZEROCOPY | MSG_NOSIGNAL);
        if (ret < 0 && (errno == ENOBUFS || errno == EINVAL)) {
            /* kernel refuses zc for this call: fall back to a copy send.
             * No completion for zc_serial will ever be queued; the
             * pre-published slot is converted to a plain one below. */
            ret = sendmsg(ctx->resp_fd, &msg, MSG_NOSIGNAL);
        } else if (ret > 0) {
            used_zc = true;
        }
        if (used_zc && ret > 0 && (unsigned int)ret < (unsigned int)total) {
            /* zc short write: the kernel queued only [0, ret) (a completion
             * is pending for it) and the slot keeps header/lens/msg_sg + the
             * elem alive. The unsent tail must go out as a plain copy send -
             * treating the short write as success would truncate the request
             * and leave the stub blocking on a full one. The tail is
             * snapshotted into the resume entry here (under zc_lock, so the
             * pending completion cannot free the originals first); the
             * writable handler re-sends it from the snapshot without ever
             * touching the slot's buffers (the resume must not release them
             * either, hence the zc-park entry). */
            if (!local_send_resume_park_zc(vq, elem, header, lens, seq,
                                           (unsigned int)ret, total)) {
                error_report("local qemu: zc short-write seq %u: resume state "
                             "full, request truncated", seq);
            }
            g_mutex_unlock(&ctx->zc_lock);
            AioContext *ctx_aio = vq_get_aio_ctx(vq);
            if (ctx_aio) {
                aio_set_fd_handler(ctx_aio, ctx->resp_fd,
                                   local_response_distributor, NULL, NULL,
                                   local_retry_send, vq);
            }
            return false;
        }
        if (!used_zc && ret > 0 && (unsigned int)ret < (unsigned int)total) {
            /* the zc fallback copy send short-wrote: same truncation hazard.
             * Convert the pre-published zc slot to a plain one (no
             * completion for zc_serial will ever be queued) and park the
             * tail with copy semantics - the header/lens are ours and the
             * elem is send-worker-owned until the tail is queued, so the
             * resume completes it exactly like a batch copy entry. */
            g_mutex_lock(&ctx->vq_lock);
            InflightSlot *s = inflight_slot(ctx->inflight, seq);
            inflight_free_bufs(s);
            s->zc = (ZcPending){0};
            s->is_zc = false;
            st->serial = zc_serial;     /* serial was never used */
            g_mutex_unlock(&ctx->vq_lock);
            if (!local_send_resume_park_copy(vq, elem, header, lens, seq,
                                             (unsigned int)ret, total,
                                             no_in_bufs)) {
                error_report("local qemu: copy short-write seq %u: resume "
                             "state full, request truncated", seq);
            }
            g_mutex_unlock(&ctx->zc_lock);
            AioContext *ctx_aio = vq_get_aio_ctx(vq);
            if (ctx_aio) {
                aio_set_fd_handler(ctx_aio, ctx->resp_fd,
                                   local_response_distributor, NULL, NULL,
                                   local_retry_send, vq);
            }
            return false;
        }
        g_mutex_unlock(&ctx->zc_lock);
    } else {
        /* plain copy send: publish first, for the same reason */
        g_mutex_lock(&ctx->vq_lock);
        inflight_publish(ctx->inflight, seq, elem, NULL);
        g_mutex_unlock(&ctx->vq_lock);

        ret = sendmsg(ctx->resp_fd, &msg, MSG_NOSIGNAL);
    }

    vr_debug("vremote: local sent vq %d seq %d total=%zd ret=%zd %s",
                 vq_nr, seq, total, ret, used_zc ? "zc" : "copy");

    if (ret < 0) {
        /* roll back the pre-published slot: nothing was sent, so neither a
         * resp nor a completion can reference this seq. Leaving it as a
         * NULL hole (not rewinding tail) keeps head <= tail under all
         * interleavings; the hole-advance in inflight_clear skips it. */
        g_mutex_lock(&ctx->vq_lock);
        InflightSlot *s = inflight_slot(ctx->inflight, seq);
        inflight_free_bufs(s);
        s->elem = NULL;
        s->seq = 0;
        s->is_zc = false;
        s->zc = (ZcPending){0};
        if (zc_eligible) {
            st->serial = zc_serial; /* serial was never used */
        }
        g_mutex_unlock(&ctx->vq_lock);

        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            /* send buffer full: give the elem back to the vring (rewind the
             * avail index + detach, under vq_lock like pop) and re-drain when
             * the socket becomes writable, instead of dropping the request.
             * The aio loop keeps running; only this vq's sending is stalled.
             * The elem is freed here (unpop does not free it) and the next
             * drain pops a fresh one. */
            g_mutex_lock(&ctx->vq_lock);
            virtqueue_unpop(vq, elem, 0);
            g_mutex_unlock(&ctx->vq_lock);
            g_free(elem);
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

    if (used_zc) {
        /* full zc send: the slot is already published (pre-send); the kernel
         * holds the header/lens/msg_sg and the guest's out buffers until the
         * completion, which either pushes the used-ring entry now (resp
         * already here, see the response handler) or defers the push until
         * the resp arrives (see local_zc_complete_one). */
        return true;
    }

    /* the copy send succeeded */
    if (zc_eligible) {
        /* the zc send was refused and the copy send took over: convert the
         * pre-published zc slot to a plain one (no completion for zc_serial
         * will ever be queued, and the header/lens/msg_sg are ours again) */
        g_mutex_lock(&ctx->vq_lock);
        InflightSlot *s = inflight_slot(ctx->inflight, seq);
        inflight_free_bufs(s);
        s->zc = (ZcPending){0};
        s->is_zc = false;
        st->serial = zc_serial;     /* serial was never used */
        g_mutex_unlock(&ctx->vq_lock);
    } else {
        g_free(lens);
        g_free(header);
        g_free(msg_sg);
    }

    if (no_in_bufs) {
        /* no in-buffers: the stub sends no resp for this request, so
         * complete the used-ring entry right away. Uses the flag captured
         * before the send: a resp-tracked elem may already have been freed
         * by the recv worker (resp matched against the pre-published slot),
         * so elem->in_num is not readable anymore here. */
        g_mutex_lock(&ctx->vq_lock);
        virtqueue_push(vq, elem, 0);
        virtio_notify(virtqueue_get_vdev(vq), vq);
        inflight_clear(ctx->inflight, seq);
        g_mutex_unlock(&ctx->vq_lock);
        g_free(elem);
        return true;
    }

    /* the pre-published slot stays for the response to match by seq */
    return true;
}

/* wire bytes of a request (the zc threshold total): 16B header + the lens
 * array + the out payload */
static ssize_t local_elem_total(const VirtQueueElement *elem)
{
    ssize_t total = 4 * sizeof(int) +
                    (elem->out_num + elem->in_num) * sizeof(int);
    for (unsigned int i = 0; i < elem->out_num; i++) {
        total += elem->out_sg[i].iov_len;
    }
    return total;
}

/* a request batch that a non-blocking sendmsg() only partially queued
 * (short write): the fully-queued elems are handled right away, the rest
 * keep their scratch buffers here until the vq's send worker re-sends the
 * unsent tails from the recorded byte offsets. Every elem still has its
 * in-flight slot published (the publish happens before the send). */
typedef struct LocalSendResume {
    VirtQueueElement *elems[VR_BATCH_MAX];
    int *headers[VR_BATCH_MAX];
    int *lens[VR_BATCH_MAX];
    uint32_t seqs[VR_BATCH_MAX];
    unsigned int off[VR_BATCH_MAX]; /* bytes of elems[i] already queued */
    bool zc[VR_BATCH_MAX];   /* zc elems: the inflight slot owns the header/
                                lens (freed by the zc completion), so the
                                resume must not push or release them */
    bool no_in[VR_BATCH_MAX]; /* in_num == 0 captured pre-send: the resp-tracked
                                 elems may already be freed (by the recv worker
                                 matching the just-sent tail) when the resume
                                 finishes them, so their fields are not
                                 readable anymore */
    /* zc entries only: snapshot of the unsent tail [off, total) copied at
     * park time under zc_lock. The zc completion may fire at any moment
     * after the original sendmsg and free the slot's header/lens/msg_sg and,
     * for in_num == 0 requests, push + free the elem (the guest then reuses
     * the out pages), so the resume sends the tail from this private buffer
     * and never touches the originals. */
    void *tail[VR_BATCH_MAX];
    size_t tail_len[VR_BATCH_MAX]; /* bytes in tail[] (== payload at park) */
    ssize_t total[VR_BATCH_MAX];   /* full wire size, captured pre-send */
    unsigned int n;
} LocalSendResume;

/* release the resume state (used by the teardown; elems stay in the window
 * and are freed by inflight_reset) */
static void local_send_resume_free(RemoteVQueueCtx *ctx)
{
    LocalSendResume *r = ctx->send_resume;

    if (!r) {
        return;
    }
    for (unsigned int i = 0; i < r->n; i++) {
        if (r->zc[i]) {
            g_free(r->tail[i]); /* zc entries: private tail snapshot; the slot
                                   owns header/lens, released by inflight_reset
                                   on teardown */
        } else {
            g_free(r->headers[i]);
            g_free(r->lens[i]);
        }
    }
    g_free(r);
    ctx->send_resume = NULL;
}

/* re-send the unsent tails of a short-written request batch. Runs on the vq's
 * send worker (reached from local_retry_send via local_send_handler); every
 * elem already has its in-flight slot published, so finishing one is exactly
 * the fully-sent path of local_send_batch (in_num == 0 elems push right away,
 * the others keep their slot for the resp). Returns true when the resume
 * state is fully drained (the vring may then be re-drained). */
static bool local_send_resume(VirtQueue *vq)
{
    RemoteVQueueCtx *ctx = virtqueue_get_remote_ctx(vq);
    LocalSendResume *r = ctx->send_resume;
    int vq_nr = virtio_get_queue_index(vq);

    if (!r || r->n == 0) {
        return true;
    }
    while (r->n > 0) {
        unsigned int off = r->off[0];
        ssize_t total = r->total[0];
        ssize_t payload = total - off;
        struct iovec *sg;
        unsigned int k;
        if (r->zc[0]) {
            /* zc entry: send the snapshotted tail only - the zc completion
             * may already have freed the slot's header/lens/msg_sg and the
             * elem (with the guest reusing the out pages), so nothing of
             * the original send state is readable anymore. */
            size_t base = (size_t)total - r->tail_len[0];
            size_t idx = (size_t)off - base;
            sg = g_new(struct iovec, 1);
            sg[0].iov_base = (char *)r->tail[0] + idx;
            sg[0].iov_len = r->tail_len[0] - idx;
            k = 1;
        } else {
            VirtQueueElement *e = r->elems[0];
            unsigned int outn = e->out_num, inn = e->in_num;
            /* build the iov for the unsent tail [off, total): header, lens,
             * out data, each clipped to the segment the offset falls into.
             * Copy elems are send-worker-owned until their own tail is fully
             * queued (the stub cannot respond to a partial request), so the
             * elem fields are still valid here. */
            sg = g_new(struct iovec, 2 + outn);
            k = 0;
            unsigned int seg_off = 0;
            size_t hlen = 4 * sizeof(int);
            if (off < seg_off + hlen) {
                size_t in_seg = (off > seg_off) ? (off - seg_off) : 0;
                sg[k].iov_base = (char *)r->headers[0] + in_seg;
                sg[k].iov_len = hlen - in_seg;
                k++;
            }
            seg_off += hlen;
            size_t llen = (outn + inn) * sizeof(int);
            if (off < seg_off + llen) {
                size_t in_seg = (off > seg_off) ? (off - seg_off) : 0;
                sg[k].iov_base = (char *)r->lens[0] + in_seg;
                sg[k].iov_len = llen - in_seg;
                k++;
            }
            seg_off += llen;
            for (unsigned int i = 0; i < outn; i++) {
                size_t olen = e->out_sg[i].iov_len;
                if (off < seg_off + olen) {
                    /* clip only if off falls inside this segment; segments
                     * fully before off are skipped, segments after the one
                     * containing off are sent whole (in_seg == 0). Without
                     * the off >= seg_off guard a later segment passes the
                     * end-bound check, and (off - seg_off) wraps unsigned
                     * -> huge iov_len -> sendmsg EINVAL -> dropped elem ->
                     * hung IO (spark's 20+ segment requests hit this). */
                    size_t in_seg = (off > seg_off) ? (off - seg_off) : 0;
                    sg[k].iov_base = (char *)e->out_sg[i].iov_base + in_seg;
                    sg[k].iov_len = olen - in_seg;
                    k++;
                }
                seg_off += olen;
            }
        }
        struct msghdr msg = { .msg_iov = sg, .msg_iovlen = k };
        ssize_t ret = sendmsg(ctx->resp_fd, &msg, MSG_NOSIGNAL);
        if (ret < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                g_free(sg);
                /* send buffer full again: keep the resume state; the writable
                 * handler re-enters this function */
                AioContext *ctx_aio = vq_get_aio_ctx(vq);
                if (ctx_aio) {
                    aio_set_fd_handler(ctx_aio, ctx->resp_fd,
                                       local_response_distributor, NULL, NULL,
                                       local_retry_send, vq);
                }
                return false;
            }
            error_report("local qemu: sendmsg resume failed: %s "
                         "vq=%d off=%u total=%zd k=%u zc=%d payload=%zd",
                         strerror(errno), vq_nr, off, total, k, r->zc[0],
                         payload);
            for (unsigned int d = 0; d < k && d < 64; d++) {
                error_report("  resume iov[%u] len=%zu", d, sg[d].iov_len);
            }
            g_free(sg);
            local_send_resume_free(ctx); /* the teardown frees the elems */
            return true;
        }
        g_free(sg);
        if (ret < payload) {
            /* short again: keep the rest for the next writable callback */
            r->off[0] = off + (unsigned int)ret;
            AioContext *ctx_aio = vq_get_aio_ctx(vq);
            if (ctx_aio) {
                aio_set_fd_handler(ctx_aio, ctx->resp_fd,
                                   local_response_distributor, NULL, NULL,
                                   local_retry_send, vq);
            }
            return false;
        }
        vr_debug("vremote: local resume vq %d seq %u", vq_nr, r->seqs[0]);
        if (r->zc[0]) {
            /* zc elems: the tail is fully queued, done here - the slot stays
             * published and the zc completion (or the resp) drives the push
             * and releases the header/lens/elem; only the private tail
             * snapshot is released */
            g_free(r->tail[0]);
        } else {
            /* copy elems complete the used ring right here (in_num == 0
             * elems) and release the scratch header/lens. Uses the flag
             * captured before the original send: the just-sent tail lets the
             * recv worker free a resp-tracked elem before this runs. */
            if (r->no_in[0]) {
                g_mutex_lock(&ctx->vq_lock);
                virtqueue_push(vq, r->elems[0], 0);
                virtio_notify(virtqueue_get_vdev(vq), vq);
                inflight_clear(ctx->inflight, r->seqs[0]);
                g_mutex_unlock(&ctx->vq_lock);
                g_free(r->elems[0]);
            }
            g_free(r->headers[0]);
            g_free(r->lens[0]);
        }
        for (unsigned int i = 1; i < r->n; i++) {
            r->elems[i - 1] = r->elems[i];
            r->headers[i - 1] = r->headers[i];
            r->lens[i - 1] = r->lens[i];
            r->seqs[i - 1] = r->seqs[i];
            r->off[i - 1] = r->off[i];
            r->zc[i - 1] = r->zc[i];
            r->no_in[i - 1] = r->no_in[i];
            r->tail[i - 1] = r->tail[i];
            r->tail_len[i - 1] = r->tail_len[i];
            r->total[i - 1] = r->total[i];
        }
        r->n--;
    }
    return true;
}

/* park a zc elem whose send short-wrote in the resume state. The elem's slot
 * is already published with is_zc (the slot owns header/lens/msg_sg, freed by
 * the zc completion), so the resume entry records the byte offset of the
 * unsent tail AND a private snapshot of those bytes; on completion
 * local_send_resume sends the snapshot and drops the entry without touching
 * the slot. Called with zc_lock held (local_send_msg holds it across the zc
 * sendmsg), so the completion cannot have freed the header/lens/elem between
 * the send and this copy. Returns false when the resume state is full (the
 * caller then stops draining; the writable handler re-tries). */
static bool local_send_resume_park_zc(VirtQueue *vq, VirtQueueElement *elem,
                                      int *header, int *lens, uint32_t seq,
                                      unsigned int off, ssize_t total)
{
    RemoteVQueueCtx *ctx = virtqueue_get_remote_ctx(vq);
    LocalSendResume *r = ctx->send_resume;

    if (!r) {
        r = g_new0(LocalSendResume, 1);
        ctx->send_resume = r;
    }
    if (r->n >= VR_BATCH_MAX) {
        return false;
    }
    /* snapshot the unsent tail [off, total) into the entry's own buffer:
     * the zc completion may fire as soon as zc_lock is released and free the
     * slot's header/lens/msg_sg and, for in_num == 0 requests, push + free
     * the elem (the guest then reuses the out pages), so the resume must
     * never read the originals. */
    size_t hlen = 4 * sizeof(int);
    size_t llen = ((size_t)elem->out_num + (size_t)elem->in_num) * sizeof(int);
    size_t tail_len = (size_t)total - off;
    char *tail = g_malloc(tail_len);
    size_t w = 0;
    size_t seg_off = 0;
    if (off < seg_off + hlen) {
        size_t c = MIN(hlen - off, tail_len);
        memcpy(tail, (char *)header + (off - seg_off), c);
        w += c;
    }
    seg_off += hlen;
    if (off < seg_off + llen) {
        size_t l_off = off > seg_off ? off - seg_off : 0;
        size_t c = MIN(seg_off + llen - off, tail_len - w);
        memcpy(tail + w, (char *)lens + l_off, c);
        w += c;
    }
    seg_off += llen;
    for (unsigned int i = 0; i < elem->out_num && w < tail_len; i++) {
        size_t olen = elem->out_sg[i].iov_len;
        if (off < seg_off + olen) {
            size_t o_off = off > seg_off ? off - seg_off : 0;
            size_t c = MIN(seg_off + olen - off, tail_len - w);
            memcpy(tail + w, (char *)elem->out_sg[i].iov_base + o_off, c);
            w += c;
        }
        seg_off += olen;
    }
    r->elems[r->n] = elem;
    r->headers[r->n] = header;
    r->lens[r->n] = lens;
    r->seqs[r->n] = seq;
    r->off[r->n] = off;
    r->zc[r->n] = true;
    r->no_in[r->n] = false; /* zc entries never push here; set for symmetry */
    r->tail[r->n] = tail;
    r->tail_len[r->n] = tail_len;
    r->total[r->n] = total;
    r->n++;
    return true;
}

/* park a single copy-sent elem whose send short-wrote (the zc fallback copy
 * send in local_send_msg). Unlike the zc park, the header/lens/msg_sg are
 * owned by the caller (no completion is pending) and the elem stays
 * send-worker-owned until its own tail is fully queued, so the resume
 * completes it exactly like a batch copy entry. */
static bool local_send_resume_park_copy(VirtQueue *vq, VirtQueueElement *elem,
                                        int *header, int *lens, uint32_t seq,
                                        unsigned int off, ssize_t total,
                                        bool no_in)
{
    RemoteVQueueCtx *ctx = virtqueue_get_remote_ctx(vq);
    LocalSendResume *r = ctx->send_resume;

    if (!r) {
        r = g_new0(LocalSendResume, 1);
        ctx->send_resume = r;
    }
    if (r->n >= VR_BATCH_MAX) {
        return false;
    }
    r->elems[r->n] = elem;
    r->headers[r->n] = header;
    r->lens[r->n] = lens;
    r->seqs[r->n] = seq;
    r->off[r->n] = off;
    r->zc[r->n] = false;
    r->no_in[r->n] = no_in;
    r->tail[r->n] = NULL;
    r->tail_len[r->n] = 0;
    r->total[r->n] = total;
    r->n++;
    return true;
}

/*
 * send a batch of n copy elems with one sendmsg(): the per-elem
 * [header][lens][out_sg...] records are concatenated into one flat iovec
 * array, every slot is published before the send (the recv worker must always
 * find any seq the stub may respond to - see local_send_msg), then a single
 * sendmsg. On EAGAIN the whole batch is rolled back (published slots are
 * turned into holes, every elem is given back to the vring in reverse order
 * so the next drain re-sends FIFO) and the writable handler is armed, exactly
 * like the single-elem path. On a non-blocking SHORT write (ret >= 0 but less
 * than the batch size) the fully-queued elems are completed normally and the
 * rest (with the partially-queued head) go to ctx->send_resume to be re-sent
 * from their byte offsets on writable - a partial batch must never be rolled
 * back or dropped, it would desync the request stream. Elems with in_num == 0
 * complete their used-ring entry right away (the stub sends no resp for them).
 */
static bool local_send_batch(VirtQueue *vq, VirtQueueElement **elems,
                             unsigned int n)
{
    RemoteVQueueCtx *ctx = virtqueue_get_remote_ctx(vq);
    int vq_nr = virtio_get_queue_index(vq);
    unsigned int n_iov = 0;
    ssize_t total = 0;
    int **headers, **lens_arr;
    uint32_t *seqs;
    struct iovec *msg_sg;
    unsigned int k = 0;
    ssize_t ret;

    if (!ctx) {
        return false;
    }
    /* elem fields are only readable before the send: the recv worker can
     * match a resp against a pre-published slot and free the elem while the
     * send worker is still walking the batch. Capture everything the
     * post-send paths need (the wire total for short-write accounting and
     * the no-in-buffers flag for the immediate-push decision). */
    ssize_t *elen_arr = g_new(ssize_t, n);
    bool *no_in = g_new(bool, n);
    for (unsigned int i = 0; i < n; i++) {
        VirtQueueElement *e = elems[i];
        n_iov += 2 + e->out_num;
        elen_arr[i] = local_elem_total(e);
        no_in[i] = (e->in_num == 0);
        total += elen_arr[i];
    }

    msg_sg = g_new(struct iovec, n_iov);
    headers = g_new(int *, n);
    lens_arr = g_new(int *, n);
    seqs = g_new(uint32_t, n);

    for (unsigned int i = 0; i < n; i++) {
        VirtQueueElement *e = elems[i];
        int *header = g_new0(int, 4);
        int *lens;
        uint32_t seq = ctx->elem_index++;

        header[0] = vq_nr;
        header[1] = seq;
        header[2] = e->out_num;
        header[3] = e->in_num;
        headers[i] = header;

        lens = g_new0(int, e->out_num + e->in_num);
        for (unsigned int j = 0; j < e->out_num; j++) {
            lens[j] = e->out_sg[j].iov_len;
        }
        for (unsigned int j = 0; j < e->in_num; j++) {
            lens[e->out_num + j] = e->in_sg[j].iov_len;
        }
        lens_arr[i] = lens;
        seqs[i] = seq;

        msg_sg[k].iov_base = header;
        msg_sg[k].iov_len = 4 * sizeof(int);
        k++;
        msg_sg[k].iov_base = lens;
        msg_sg[k].iov_len = (e->out_num + e->in_num) * sizeof(int);
        k++;
        memcpy(msg_sg + k, e->out_sg, e->out_num * sizeof(iovec));
        k += e->out_num;

        vr_debug("vremote: local send vq %d seq %u out=%u in=%u", vq_nr, seq,
                 e->out_num, e->in_num);

        /* publish before the send, on every path (see local_send_msg) */
        g_mutex_lock(&ctx->vq_lock);
        inflight_publish(ctx->inflight, seq, e, NULL);
        g_mutex_unlock(&ctx->vq_lock);
    }

    struct msghdr msg = {
        .msg_iov = msg_sg,
        .msg_iovlen = n_iov,
    };
    ret = sendmsg(ctx->resp_fd, &msg, MSG_NOSIGNAL);

    if (ret < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            /* send buffer full: nothing of the batch was accepted, so turn
             * the pre-published slots into holes and give every elem back to
             * the vring (reverse order so the next drain re-sends FIFO). The
             * writable handler is armed like the single-elem path. */
            g_mutex_lock(&ctx->vq_lock);
            for (unsigned int i = 0; i < n; i++) {
                InflightSlot *s = inflight_slot(ctx->inflight, seqs[i]);
                inflight_free_bufs(s);
                s->elem = NULL;
                s->seq = 0;
                s->is_zc = false;
                s->zc = (ZcPending){0};
            }
            for (unsigned int i = n; i > 0; i--) {
                virtqueue_unpop(vq, elems[i - 1], 0);
            }
            g_mutex_unlock(&ctx->vq_lock);
            for (unsigned int i = 0; i < n; i++) {
                g_free(elems[i]);
            }
            AioContext *ctx_aio = vq_get_aio_ctx(vq);
            if (ctx_aio) {
                aio_set_fd_handler(ctx_aio, ctx->resp_fd,
                                   local_response_distributor, NULL, NULL,
                                   local_retry_send, vq);
            }
            goto out_free;
        }
        error_report("local qemu: sendmsg batch to stub failed: %s",
                     strerror(errno));
        goto out_free;
    }
    if (ret < total) {
        /* non-blocking short write: the send buffer filled mid-batch.
         * Fully-queued elems are completed like the success path; the rest
         * (with the partially-queued head) go to ctx->send_resume and are
         * re-sent from their recorded byte offsets once the socket is
         * writable - rolling them back or dropping them would desync the
         * request stream, and their published slots would never match a
         * response. */
        unsigned int abs = (unsigned int)ret;
        unsigned int cum = 0;
        unsigned int i;
        for (i = 0; i < n; i++) {
            ssize_t elen = elen_arr[i];
            if (cum + elen <= abs) {
                VirtQueueElement *e = elems[i];
                if (no_in[i]) {
                    g_mutex_lock(&ctx->vq_lock);
                    virtqueue_push(vq, e, 0);
                    virtio_notify(virtqueue_get_vdev(vq), vq);
                    inflight_clear(ctx->inflight, seqs[i]);
                    g_mutex_unlock(&ctx->vq_lock);
                    g_free(e);
                }
                cum += elen;
            } else {
                break;
            }
        }
        LocalSendResume *r = ctx->send_resume;
        if (!r) {
            r = g_new0(LocalSendResume, 1);
            ctx->send_resume = r;
        }
        for (unsigned int j = i; j < n; j++) {
            unsigned int rj = j - i;
            r->elems[rj] = elems[j];
            r->headers[rj] = headers[j];
            r->lens[rj] = lens_arr[j];
            r->seqs[rj] = seqs[j];
            r->off[rj] = (j == i) ? abs - cum : 0;
            r->zc[rj] = false;
            r->no_in[rj] = no_in[j];
            /* copy entries own their header/lens; no tail snapshot (the
             * elems are alive until their own tail is queued). Fields set
             * explicitly: a drained resume state may leave stale zc/tail
             * values in these slots from an earlier zc batch. */
            r->tail[rj] = NULL;
            r->tail_len[rj] = 0;
            r->total[rj] = elen_arr[j];
            r->n = rj + 1;
        }
        AioContext *ctx_aio = vq_get_aio_ctx(vq);
        if (ctx_aio) {
            aio_set_fd_handler(ctx_aio, ctx->resp_fd,
                               local_response_distributor, NULL, NULL,
                               local_retry_send, vq);
        }
        /* release the scratch buffers of the fully-queued elems only; the
         * kept ones were moved to the resume state */
        for (unsigned int k = 0; k < i; k++) {
            g_free(headers[k]);
            g_free(lens_arr[k]);
        }
        g_free(headers);
        g_free(lens_arr);
        g_free(seqs);
        g_free(msg_sg);
        g_free(elen_arr);
        g_free(no_in);
        return false;
    }

    /* success: elems with no in-buffers complete right away (no resp is
     * coming); the others keep their published slots for the resp match.
     * Uses the pre-captured flag: a resp-tracked elem may already have been
     * freed by the recv worker, so elem fields are not readable here. */
    for (unsigned int i = 0; i < n; i++) {
        VirtQueueElement *e = elems[i];
        if (no_in[i]) {
            g_mutex_lock(&ctx->vq_lock);
            virtqueue_push(vq, e, 0);
            virtio_notify(virtqueue_get_vdev(vq), vq);
            inflight_clear(ctx->inflight, seqs[i]);
            g_mutex_unlock(&ctx->vq_lock);
            g_free(e);
        }
    }

out_free:
    for (unsigned int i = 0; i < n; i++) {
        g_free(headers[i]);
        g_free(lens_arr[i]);
    }
    g_free(headers);
    g_free(lens_arr);
    g_free(seqs);
    g_free(msg_sg);
    g_free(elen_arr);
    g_free(no_in);
    return ret >= 0;
}

static void local_send_handler(void *opaque)
{
    VirtQueue *vq = opaque;
    RemoteVQueueCtx *ctx = virtqueue_get_remote_ctx(vq);
    VirtQueueElement *elem;
    /* copy elems are accumulated and sent as one batch (VR_LOCAL_BATCH_*);
     * zc-eligible elems flush the pending batch and go through local_send_msg
     * (MSG_ZEROCOPY) individually, their bookkeeping must stay per-elem */
    VirtQueueElement *batch[VR_BATCH_MAX];
    unsigned int n_batch = 0;
    ssize_t batch_bytes = 0;
    unsigned int pops = 0;

    if (!ctx) {
        return;
    }
    if (qatomic_load_acquire(&ctx->dead)) {
        return; /* the stub is gone */
    }
    vr_ev_log(VR_EV_DRAIN, vq, 0, 0, 0);
    if (ctx->send_resume) {
        /* a short-write tail is pending: it must go out before any further
         * request, else the stub parses a corrupt stream. Drain it first
         * (the writable handler stays armed on EAGAIN). */
        if (!local_send_resume(vq)) {
            return;
        }
    }
    if (!ctx->zc) {
        /* first send on this vq: enable MSG_ZEROCOPY (runs on the vq's send
         * worker, the same thread that owns the zc state otherwise) */
        ctx->zc = zc_enable(ctx->resp_fd);
    }
    ZcFdState *st = ctx->zc;

    /* drain the vring: pop and submit as many elems as possible without
     * blocking the aio loop. When the socket is full local_send_batch gives
     * the batch back to the vring (virtqueue_unpop) and arms the writable
     * handler; the next drain pops it again in FIFO order. When the in-flight
     * window is full we stop popping: the recv worker wakes this handler
     * again once a response clears a slot (local_drain_wake), so the guest
     * can never push more requests than the window can hold. */
    // to review: the same fd event will be lift up, may causing HoL, think about multiple aio iothread
    while (true) {
        Inflight *inf = ctx->inflight;
        if (inf && inflight_slot(inf, ctx->elem_index)->elem != NULL) {
            /* the next seq's slot is still occupied: a response has not yet
             * cleared it. Same free-slot semantics as inflight_publish: an
             * out-of-order completion may reuse other slots while the slow
             * one that owns this slot index is pending, so the window is not
             * "full" just because tail - head reached the window size. */
            vr_ev_log(VR_EV_BREAK, vq, 1 /*window full*/, pops,
                      qatomic_load_acquire(&inf->head));
            break; /* wait for a resp to clear the slot */
        }
        g_mutex_lock(&ctx->vq_lock);
        elem = virtqueue_pop(vq, sizeof(VirtQueueElement));
        g_mutex_unlock(&ctx->vq_lock);
        if (!elem) {
            vr_ev_log(VR_EV_BREAK, vq, 0 /*vring empty*/, pops,
                      virtio_queue_get_last_avail_idx(virtqueue_get_vdev(vq),
                                                      virtio_get_queue_index(vq)));
            break;
        }
        pops++;
        vr_ev_log(VR_EV_POP, vq,
                  virtio_queue_get_last_avail_idx(virtqueue_get_vdev(vq),
                                                  virtio_get_queue_index(vq)),
                  inf ? (uint32_t)(qatomic_load_acquire(&inf->tail) -
                                   qatomic_load_acquire(&inf->head)) : 0,
                  0);
        if (st && st->enabled &&
            local_elem_total(elem) >= vr_zc_send_min) {
            /* flush the pending copy batch, then send this zc elem alone */
            if (n_batch) {
                if (!local_send_batch(vq, batch, n_batch)) {
                    n_batch = 0;
                    batch_bytes = 0;
                    /* the batch is back in the vring (EAGAIN) or the conn is
                     * dying; give this zc elem back too so the retry (or the
                     * teardown) re-sends everything in FIFO order */
                    g_mutex_lock(&ctx->vq_lock);
                    virtqueue_unpop(vq, elem, 0);
                    g_mutex_unlock(&ctx->vq_lock);
                    g_free(elem);
                    break;
                }
                n_batch = 0;
                batch_bytes = 0;
            }
            if (!local_send_msg(vq, elem)) { // send false, wait for the next time
                vr_ev_log(VR_EV_BREAK, vq, 2 /*zc send fail*/, pops, 0);
                break;
            }
            continue;
        }
        batch[n_batch++] = elem;
        batch_bytes += local_elem_total(elem);
        if (n_batch >= vr_local_batch_n || batch_bytes >= vr_local_batch_m) {
            if (!local_send_batch(vq, batch, n_batch)) {
                n_batch = 0;
                batch_bytes = 0;
                vr_ev_log(VR_EV_BREAK, vq, 3 /*batch fail*/, pops, 0);
                break;
            }
            n_batch = 0;
            batch_bytes = 0;
        }
    }
    if (n_batch) {
        local_send_batch(vq, batch, n_batch);
    }
    if (pops == 0) {
        vr_ev_log(VR_EV_BREAK, vq, 4 /*no pops*/, pops, 0);
    }
}

/*
 * The in-flight window has space again (the recv worker consumed a resp or a
 * zc completion that cleared a slot): re-drain the vring so requests parked
 * behind a full window get sent. If the send worker is busy it is already
 * draining and re-checks the window itself, so a lost claim here is safe.
 */
static void local_drain_wake(VirtQueue *vq)
{
    bool ok = worker_pool_dispatch(&send_pool, vq, local_send_handler, NULL);
    vr_ev_log(VR_EV_WAKE, vq, ok ? 1 : 0, 0, 0);
}

/*
 * per-connection receive state for incremental, non-blocking reads of the
 * resp stream [vq_nr(4B)][elem_index(4B)][data_len(4B)][data...]
 */
typedef struct LocalRecvState {
    int stage;            /* 0 = header, 1 = data, 2 = finish */
    unsigned int hdr_off; /* header bytes read so far */
    uint8_t hdr[12];      /* partial header */
    VirtQueueElement *cur;    /* elem whose data is being received */
    unsigned int cur_seq; /* seq of the current response */
    unsigned int cur_off; /* data bytes already written into in_sg */
    unsigned int need_len;    /* total data bytes expected */
} LocalRecvState;

/* -------------- Local Connection Loss ------------- */

/* the stub is gone: the guest has no backend anymore, exit it. Runs on the
 * main loop via a one-shot BH. */
static void local_shutdown_bh(void *opaque)
{
    qemu_system_shutdown_request(SHUTDOWN_CAUSE_HOST_ERROR);
}

/* detach the vq's resp fd handler and close the fd. Runs on the iothread that
 * owns the aio handler via a one-shot BH: aio_set_fd_handler() must not be
 * called from the recv worker that detected the EOF. */
static void local_conn_lost_bh(void *opaque)
{
    VirtQueue *vq = opaque;
    RemoteVQueueCtx *ctx = virtqueue_get_remote_ctx(vq);

    if (ctx && ctx->resp_fd >= 0) {
        AioContext *aio_ctx = vq_get_aio_ctx(vq);
        if (aio_ctx) {
            aio_set_fd_handler(aio_ctx, ctx->resp_fd,
                               NULL, NULL, NULL, NULL, NULL);
        }
        close(ctx->resp_fd);
        ctx->resp_fd = -1;
    }
}

/* called by the recv worker from conn_err when the stub closed the connection:
 * mark the vq dead (stops further dispatch work), detach the fd on the iothread
 * and exit the guest. Without the detach the level-triggered EOF would keep
 * re-awakening local_response_handler forever. */
static void local_conn_lost(VirtQueue *vq)
{
    RemoteVQueueCtx *ctx = virtqueue_get_remote_ctx(vq);

    if (!ctx) {
        return;
    }
    if (qatomic_xchg(&ctx->dead, 1)) {
        return; /* already tearing down this vq */
    }
    /* cmsvm: the recv worker's resp-header parses (VR_EV_LHDR) are the crash
     * chain - print the last few to stderr (survives the ring dump racing the
     * event flood) and dump the ring with the full LHDR scan */
    vr_ev_dump_lhdr_stderr(8);
    vr_ev_dump_handler(0);
    AioContext *aio_ctx = vq_get_aio_ctx(vq);
    if (aio_ctx) {
        aio_bh_schedule_oneshot(aio_ctx, local_conn_lost_bh, vq);
    }
    aio_bh_schedule_oneshot(qemu_get_aio_context(), local_shutdown_bh, NULL);
}

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
        if (qatomic_load_acquire(&ctx->dead)) {
            return; /* the stub is gone; conn_err already handled the teardown */
        }
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
            vr_ev_log(VR_EV_LHDRR, vq, rs->hdr_off - n, n, rs->hdr_off);
            if (rs->hdr_off < sizeof(rs->hdr)) {
                return; /* header incomplete, wait for more */
            }
            int vq_nr, seq, len;
            memcpy(&vq_nr, rs->hdr, 4);
            memcpy(&seq, rs->hdr + 4, 4);
            memcpy(&len, rs->hdr + 8, 4);
            vr_ev_log(VR_EV_LHDR, vq, vq_nr, seq, len);
            if (vq_nr != virtio_get_queue_index(vq)) {
                error_report("local qemu: resp vq_nr %d, expected %d "
                             "(hdr=[%d %d %d] hdr_off=%d stage=%d cur_seq=%d "
                             "cur_off=%d need=%d fd=%d)",
                             vq_nr, virtio_get_queue_index(vq),
                             ((int *)rs->hdr)[0], ((int *)rs->hdr)[1],
                             ((int *)rs->hdr)[2], rs->hdr_off, rs->stage,
                             rs->cur_seq, rs->cur_off, rs->need_len, fd);
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
            if (seq == 0) {
                vr_debug("vremote: vq %d first resp len=%d", vq_nr, len);
            }
        }

        if (rs->stage == 1) {
            /* stage 1: receive the response data into the elem's in_sg.
             * Only read while data is still missing; once cur_off reaches
             * need_len (a zero-length resp completes immediately) move to
             * stage 2 to finish this response. */
            VirtQueueElement *elem = rs->cur;
            if (rs->cur_off < rs->need_len) {
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
                vr_ev_log(VR_EV_LDATA, vq, rs->cur_seq, rs->cur_off - n, n);
            }
            if (rs->cur_off >= rs->need_len) {
                /* all data received: finish this response in stage 2 */
                rs->stage = 2;
            }
        }

        if (rs->stage == 2) {
            /* stage 2: finish the response. Decide the used-ring push timing:
             * for MSG_ZEROCOPY elems the kernel completion owns the push
             * (local_zc_complete_one) so the guest's out buffers are not
             * reused before the NIC is done with them; copy-sent elems push
             * right away. Either way clear the in-flight slot afterwards. */
            VirtQueueElement *elem = rs->cur;
            vr_debug("vremote: local resp vq %d seq %d len=%d",
                         virtio_get_queue_index(vq), rs->cur_seq, rs->need_len);
            Inflight *inf = ctx->inflight;
            InflightSlot *slot = inf ? inflight_slot(inf, rs->cur_seq) : NULL;
            if (slot && slot->is_zc) {
                slot->zc.push_len = rs->need_len;
                slot->zc.len_known = true;
                if (!slot->zc.bufs) {
                    /* zc completed before the resp: push now. Same locking
                     * note as local_zc_complete_one: inflight_clear must run
                     * under vq_lock for a non-lost full_cond signal. */
                    g_mutex_lock(&ctx->vq_lock);
                    virtqueue_push(vq, elem, rs->need_len);
                    virtio_notify(virtqueue_get_vdev(vq), vq);
                    inflight_clear(ctx->inflight, rs->cur_seq);
                    g_mutex_unlock(&ctx->vq_lock);
                    g_free(elem);
                    local_drain_wake(vq);
                } /* else {} wait local_zc_complete_one() to handle */
            } else {
                /* copy-sent elem: push right away and clear the slot */
                g_mutex_lock(&ctx->vq_lock);
                virtqueue_push(vq, elem, rs->need_len);
                virtio_notify(virtqueue_get_vdev(vq), vq);
                inflight_clear(ctx->inflight, rs->cur_seq);
                g_mutex_unlock(&ctx->vq_lock);
                g_free(elem);
                vr_ev_log(VR_EV_RESP, vq, rs->cur_seq, 0, 0);
                local_drain_wake(vq);
            }
            rs->stage = 0;
            rs->hdr_off = 0;
            rs->cur = NULL;
            rs->cur_off = 0;
            rs->need_len = 0;
        }
    }

conn_err:
    /* the connection is unusable; drop the in-flight elem and reset state.
     * local_zc_fd_teardown() owns (frees) every elem still in the in-flight
     * window, so only free rs->cur here if it is not one of them. */
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
    /* the stub is gone: exit the guest instead of looping on the EOF */
    local_conn_lost(vq);
}

/* -------------- Local QEMU Handlers ------------- */

int virtio_device_start_ioeventfd_impl_local(VirtIODevice *vdev, AioContext *aio_ctx)
{
    VirtioBusState *qbus = VIRTIO_BUS(qdev_get_parent_bus(DEVICE(vdev)));
    int i, r;
    vr_ev_log_ext(VR_EV_ISTART, virtio_get_queue(vdev, 0), 1, 0, 0);
    vr_debug("vremote: start_ioeventfd_impl_local");
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
        if (!virtio_queue_get_num(vdev, i))
            continue;
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

void remote_virtio_device_stop_ioeventfd_impl(VirtIODevice *vdev)
{
    VirtioBusState *qbus = VIRTIO_BUS(qdev_get_parent_bus(DEVICE(vdev)));
    AioContext *aio_ctx = local_search_aio_ctx(vdev);
    int n, r;

    vr_ev_log_ext(VR_EV_ISTOP, virtio_get_queue(vdev, 0), 1, 0, 0);

    for (n = 0; n < VIRTIO_QUEUE_MAX; n++) {
        VirtQueue *vq = virtio_get_queue(vdev, n);
        if (!virtio_queue_get_num(vdev, n)) {
            continue;
        }
        virtio_queue_aio_detach_host_notifier(vq, aio_ctx);
    }

    /*
     * Batch all the host notifiers in a single transaction to avoid
     * quadratic time complexity in address_space_update_ioeventfds().
     */
    memory_region_transaction_begin();
    for (n = 0; n < VIRTIO_QUEUE_MAX; n++) {
        if (!virtio_queue_get_num(vdev, n)) {
            continue;
        }
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
}

void local_notifier_distributor(EventNotifier *n)
{
    VirtQueue *vq = host_notifier_to_vq(n);
    RemoteVQueueCtx *ctx = virtqueue_get_remote_ctx(vq);

    if (ctx && qatomic_load_acquire(&ctx->dead)) {
        /* the stub is gone: consume the kick so the level-triggered eventfd
         * does not spin the iothread until the guest exits */
        event_notifier_test_and_clear(n);
        return;
    }
    /* always consume the kick (success or busy): a successful claim's drain
     * covers it, a busy worker replays it via kick_pending once idle, so the
     * level-triggered eventfd does not keep waking the iothread */
    /* opaque = clear_kick: consume the notifier even if the claim fails */
    vr_ev_log(VR_EV_KICK, vq, ctx ? (uint32_t)ctx->kick_pending : 0, 0,
              (uint32_t)event_notifier_get_fd(n));
    vr_debug("vremote: kick vq %d", virtio_get_queue_index(vq));
    worker_pool_dispatch(&send_pool, vq, local_send_handler,
                         GINT_TO_POINTER(1));
}

void local_response_distributor(void *opaque)
{
    VirtQueue *vq = opaque;

    worker_pool_dispatch(&recv_pool, vq, local_response_handler, NULL);
}

/* -------------- Local QEMU ThreadPool ------------- */

// process a task by calling functions
static void local_worker_bh(void *opaque)
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
    if (w->pool->is_send) {
        for (unsigned int i = 0; i < w->n_vqs; i++) {
            VirtQueue *vq = w->vqs[i];
            RemoteVQueueCtx *ctx = virtqueue_get_remote_ctx(vq);
            if (ctx && qatomic_xchg(&ctx->kick_pending, 0)) {
                vr_ev_log(VR_EV_REPLAY, vq, 1, 0, 0);
                w->task.fn(vq); /* drain the owed kick */
            }
        }
    }
}

/* local dispatch policy: busy-claim the worker. If opaque is set (clear_kick,
 * the kick path) the host notifier is consumed unconditionally: on a
 * successful claim the claimed task's drain covers it; on a busy worker the
 * kick is recorded as an owed redrain (kick_pending) that local_worker_bh replays
 * once it goes idle. Returns true if the worker accepted the task (claimed),
 * false if it was busy (the event has been absorbed/recorded, or - for the
 * recv/io_write paths where clear_kick is false - left for the level-triggered
 * source to re-arm). */
static bool local_dispatch(WorkerPool *pool, VirtQueue *vq,
                           void (*fn)(void *opaque), void *opaque)
{
    bool clear_kick = opaque != NULL;
    RemoteVQueueCtx *ctx = virtqueue_get_remote_ctx(vq);
    Worker *w = worker_pool_worker(pool, vq);

    if (clear_kick) {
        /* absorb the kick before the claim: a failed claim leaves the owed
         * drain visible to the worker's post-idle scan (seq-cst ordering) */
        event_notifier_test_and_clear(virtqueue_get_host_notifier(vq));
        qatomic_set(&ctx->kick_pending, 1);
    }
    if (qatomic_cmpxchg(&w->busy, 0, 1) != 0) {
        /* busy: the owed redrain (or re-arm) covers the event. (The old
         * VR_EV_CLAIM busy log is deliberately gone: under a blocking resp
         * socket the recv worker never goes idle, so the level-triggered
         * iothread re-dispatch floods the event ring and overwrites the
         * VR_EV_LHDR crash chain.) */
        if (pool->is_send) {
            /* Any lost claim on a send pool is an owed re-drain. The kick
             * path already set kick_pending above; the drain_wake path must
             * too, else a window slot freed by the recv worker can be lost
             * forever when this worker goes idle right after the failed
             * claim (the recv worker's wake raced the busy window). */
            qatomic_set(&ctx->kick_pending, 1);
        }
        return false; /* busy: the owed redrain (or re-arm) covers the event */
    }
    if (clear_kick) {
        /* we own the claim: our own drain covers the kick we just absorbed */
        qatomic_set(&ctx->kick_pending, 0);
    }
    w->task.vq = vq;
    w->task.fn = fn;
    vr_ev_log(VR_EV_CLAIM, vq, clear_kick ? 1 : 0, 1, 0); /* claimed */
    qemu_bh_schedule(w->bh);
    return true;
}

/* called at machine setup (local_set_remote): tell the vq's send pool about
 * it so local_worker_bh can replay kicks absorbed while busy. */
void local_register_vq(VirtQueue *vq)
{
    worker_pool_register_vq(&send_pool, vq);
}

/* -------------- Remote Stub Forwarding  ------------- */

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
        error_report("remote stub: read_all ctl header failed (errno=%d %s)",
                     errno, strerror(errno));
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
        error_report("remote stub: read_all ctl src_ports failed (errno=%d %s)",
                     errno, strerror(errno));
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
        error_report("remote stub: write_all ctl reply failed (errno=%d %s)",
                     errno, strerror(errno));
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
        enable_tcp_nodelay(vq_fd);
        RemoteVQueueCtx *ctx = virtqueue_get_remote_ctx(vq);
        if (!ctx) {
            ctx = g_new0(RemoteVQueueCtx, 1);
            virtqueue_set_remote_ctx(vq, ctx);
        }
        ctx->resp_fd = vq_fd;
        ctx->vq_nr = n;
        remote_vq_ctx_init(ctx, virtio_queue_get_num(sctx->vdev, n), false);
        ctx->zc = zc_enable(vq_fd);
        /* the workers must know this vq for their re-scans */
        stub_register_vq(vq);
        aio_set_fd_handler(sctx->aio_ctx, vq_fd,
                           stub_distributor, NULL, NULL, NULL, vq);
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
            stub_teardown_vq(vq); /* dead, wake, wait, free resp win/zc, close */
            remote_vq_ctx_destroy(ctx);
            virtqueue_set_remote_ctx(vq, NULL);
            g_free(ctx);
        }
    }
    if (ctl_fd >= 0) {
        close(ctl_fd);
    }
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
    unsigned int sent;    /* bytes of this resp already queued to the socket
                             (non-blocking short-write resume point; 0 = the
                             send of this resp has not started) */
    bool zc_deferred;     /* a zc short-write handed header/iov/in bufs to the
                             zc completion tracker; the copy resume of the
                             tail only borrows them and must not release them */
    unsigned int total;   /* full wire length (header + data), precomputed at
                             push so a resume never re-reads sr->iov (a later
                             stub_zc_drain may already have released it) */
    void *tail;           /* snapshot of the unsent wire tail taken at the zc
                             short write; the parked copy resume sends from
                             here, never from sr->iov */
    unsigned int tail_base; /* sr->sent value when tail was snapshotted */
} StubResp;

/* release a StubResp and its payload buffers. zc_deferred keeps the header,
 * iov and the first sent_sgs in buffers alive for a zc completion. */
static void stub_resp_free(StubResp *sr, unsigned int sent_sgs,
                           bool zc_deferred)
{
    g_free(sr->tail);
    if (zc_deferred) {
        for (unsigned int i = sent_sgs; i < sr->n_in_sg; i++) {
            buf_pool_free(sr->in_bufs[i]); /* pooled resp buffers */
        }
    } else {
        g_free(sr->header);
        g_free(sr->iov);
        for (unsigned int i = 0; i < sr->n_in_sg; i++) {
            buf_pool_free(sr->in_bufs[i]); /* pooled resp buffers */
        }
    }
    g_free(sr->in_bufs);
    g_free(sr);
}

/*
 * A fully parsed request produced by the vq's handle worker (stub_recv_req)
 * and consumed by the device via remote_stub_virtqueue_pop. Owns the out/in
 * iovec arrays; the payload buffers they describe are released by the device's
 * push() once the req has been popped (handed = true), otherwise by the
 * handle worker's teardown path.
 */
typedef struct StubReq {
    unsigned int elem_index;    /* seq echoed back in the resp */
    unsigned int out_num, in_num;
    struct iovec *out_sg;       /* out_num entries, buffers allocated */
    struct iovec *in_sg;        /* in_num entries, buffers allocated */
    bool handed;                /* pop() gave the buffers to the device */
} StubReq;

/* release a StubReq. free_bufs also releases the payload buffers: only valid
 * for reqs never popped (once handed, the device's push() owns the buffers). */
static void stub_req_free(StubReq *req, bool free_bufs)
{
    if (free_bufs) {
        for (unsigned int i = 0; i < req->out_num; i++) {
            g_free(req->out_sg[i].iov_base);
        }
        for (unsigned int i = 0; i < req->in_num; i++) {
            buf_pool_free(req->in_sg[i].iov_base); /* pooled resp buffers */
        }
    }
    g_free(req->out_sg);
    g_free(req->in_sg);
    g_free(req);
}

/* drop a parked send-worker writable handler for a vq (connection teardown).
 * Runs from the send worker (dead path) or the iothread (conn_err), both
 * before the fd is closed. */
static void stub_send_detach(VirtQueue *vq)
{
    RemoteVQueueCtx *ctx = virtqueue_get_remote_ctx(vq);

    if (ctx && ctx->send_full && ctx->resp_fd >= 0) {
        /* the writable handler was registered on the stub's main aio ctx
         * (qemu polls one fd on ONE aio context only), so detach it there */
        AioContext *ctx_aio = vq_get_aio_ctx(vq);
        if (ctx_aio) {
            aio_set_fd_handler(ctx_aio, ctx->resp_fd, NULL, NULL,
                               NULL, NULL, NULL);
        }
        ctx->send_full = false;
    }
}

static void stub_send_writable(void *opaque);

/* total wire bytes of a response (header + payload iovs) */
static unsigned int stub_resp_len(const StubResp *sr)
{
    unsigned int total = 0;

    for (unsigned int i = 0; i < sr->iov_cnt; i++) {
        total += sr->iov[i].iov_len;
    }
    return total;
}

/* fill out[] (capacity cap) with the iov tail of sr starting at the byte
 * offset `from`; returns the number of iovs filled. `from` must lie inside
 * the response. Used to resume a non-blocking short write. */
static unsigned int stub_resp_iov_tail(const StubResp *sr, unsigned int from,
                                       struct iovec *out, unsigned int cap)
{
    unsigned int n = 0, off = 0;
    bool started = false;

    for (unsigned int i = 0; i < sr->iov_cnt && n < cap; i++) {
        unsigned int len = sr->iov[i].iov_len;
        if (from >= off + len) {
            off += len;
            continue;
        }
        if (!started) {
            /* the iov that contains the resume byte: partial from there */
            out[n].iov_base = (char *)sr->iov[i].iov_base + (from - off);
            out[n].iov_len = len - (from - off);
            started = true;
        } else {
            /* everything after the resume point goes out whole */
            out[n].iov_base = sr->iov[i].iov_base;
            out[n].iov_len = len;
        }
        n++;
        off += len;
    }
    return n;
}

/* copy n bytes of sr starting at the wire byte offset `from` into dst. Used
 * to snapshot the unsent tail of a zc short write while sr->iov is still
 * valid (the parked resume runs after a stub_zc_drain may have freed it).
 * Returns the number of bytes actually copied (may be < n if the iovs run
 * out before n bytes are covered - the caller logs this to catch a
 * short-fill that would leave uninitialized snapshot bytes on the wire).
 * Wire position tracks `from + done`; `off` is the wire start of each iov,
 * so the copy is a straight linear walk (an earlier bug advanced both
 * `from` and `off` by `take`, misplacing every segment after the first). */
static unsigned int stub_resp_copy_range(const StubResp *sr, unsigned int from,
                                         void *dst, unsigned int n)
{
    unsigned int off = 0, done = 0;
    char *out = dst;

    for (unsigned int i = 0; i < sr->iov_cnt && done < n; i++) {
        unsigned int len = sr->iov[i].iov_len;
        if (from + done >= off + len) {
            off += len;
            continue;
        }
        unsigned int start = (from + done) - off;
        unsigned int take = MIN(len - start, n - done);
        memcpy(out + done, (char *)sr->iov[i].iov_base + start, take);
        done += take;
        off += len;
    }
    return done;
}

/*
 * send a batch of copy responses with one sendmsg(): the resp iovs of
 * [0, n) are gathered into one flat array. On EAGAIN nothing is consumed:
 * the window head stays and the writable handler is parked, so the next
 * drain re-sends the same batch. On a non-blocking SHORT write (ret >= 0 but
 * less than the requested bytes) only the fully-queued responses are
 * consumed (cleared + freed); the first partially-queued response records
 * its resume point (sr->sent) and, together with everything after it, stays
 * in the window to be resumed on writable. Only when the whole batch is
 * queued are all slots cleared in order (head advances past them).
 */
static bool stub_send_batch(int fd, StubResp **batch, unsigned int n,
                            Inflight *win, VirtQueue *vq, Worker *w)
{
    RemoteVQueueCtx *ctx = virtqueue_get_remote_ctx(vq);
    unsigned int seq0 = qatomic_load_acquire(&win->head);
    unsigned int n_iov = 0, k = 0;
    struct iovec *msg_sg;
    unsigned int total = 0;
    ssize_t ret;
    unsigned int i;

    for (i = 0; i < n; i++) {
        total += stub_resp_len(batch[i]);
        n_iov += batch[i]->iov_cnt;
    }
    msg_sg = g_new(struct iovec, n_iov);
    for (i = 0; i < n; i++) {
        if (i == 0 && batch[0]->sent > 0) {
            /* resume of a short-write head: queue only its unsent tail */
            k += stub_resp_iov_tail(batch[0], batch[0]->sent, msg_sg + k,
                                    n_iov - k);
        } else {
            memcpy(msg_sg + k, batch[i]->iov,
                   batch[i]->iov_cnt * sizeof(struct iovec));
            k += batch[i]->iov_cnt;
        }
    }
    struct msghdr msg = { .msg_iov = msg_sg, .msg_iovlen = k };
    /* cmsvm: if the batch head's header is all-zero, the wire is about to
     * carry 12 zero bytes as a response header - the local side's exact
     * crash signature (resp vq_nr 0). Dump the header memory to stderr. */
    if (k > 0 && batch[0]->header[0] == 0 && batch[0]->header[1] == 0 &&
        batch[0]->header[2] == 0) {
        error_report("remote stub: BATCH HEAD ZERO HDR seq0=%u sent=%u "
                     "iov_base=%p total=%u", seq0, batch[0]->sent,
                     batch[0]->iov[0].iov_base, batch[0]->total);
        for (unsigned int h = 0; h < 4 && h < k; h++) {
            unsigned char *p = msg_sg[h].iov_base;
            error_report("  iov[%u] base=%p len=%llu: %02x %02x %02x %02x "
                         "%02x %02x %02x %02x %02x %02x %02x %02x",
                         h, p, (unsigned long long)msg_sg[h].iov_len,
                         p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7],
                         p[8], p[9], p[10], p[11]);
        }
    }
    ret = sendmsg(fd, &msg, MSG_NOSIGNAL);
    g_free(msg_sg);

    if (ret < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        /* send buffer full: nothing of the batch was accepted; park the
         * writable handler (on the stub's main aio ctx, next to the POLLIN
         * stub_distributor - qemu polls one fd on ONE aio context only, so
         * registering POLLOUT on the worker's private ctx here never fired
         * and the EAGAIN retry stalled forever on 4MB zc resp short-writes;
         * the local side already registers both handlers on the iothread) */
        ctx->send_full = true;
        aio_set_fd_handler(vq_get_aio_ctx(vq), fd,
                           stub_distributor, stub_send_writable,
                           NULL, NULL, vq);
        return false;
    }
    if (ret < 0) {
        /* hard error: keep the slots; the connection teardown releases them */
        static unsigned int efa_probe;
        if (efa_probe++ < 3) {
            error_report("remote stub: sendmsg resp batch failed: %s "
                         "(n=%u k=%u total=%u seq0=%u)",
                         strerror(errno), n, k, total, seq0);
            for (unsigned int j = 0; j < n && j < 8; j++) {
                error_report("  batch[%u]: seq=%u sent=%u iov_cnt=%u total=%u "
                             "header=%p iov=%p", j, seq0 + j, batch[j]->sent,
                             batch[j]->iov_cnt, batch[j]->total, batch[j]->header,
                             batch[j]->iov);
                for (unsigned int v = 0; v < batch[j]->iov_cnt; v++) {
                    error_report("    iov[%u]: base=%p len=%llu",
                                 v, batch[j]->iov[v].iov_base,
                                 (unsigned long long)batch[j]->iov[v].iov_len);
                }
            }
        }
        return false;
    }

    /* absolute offset of the queued bytes within the batch (batch[0]->sent
     * bytes were already queued by an earlier short write) */
    unsigned int abs = batch[0]->sent + (unsigned int)ret;
    unsigned int cum = 0;          /* wire bytes of batch[0..i-1] */
    for (i = 0; i < n; i++) {
        unsigned int blen = stub_resp_len(batch[i]);
        if (cum + blen <= abs) {
            stub_resp_free(batch[i], batch[i]->iov_cnt - 1, false);
            inflight_clear(win, seq0 + i);
            cum += blen;
        } else {
            break;
        }
    }
    if (i < n) {
        /* the send buffer filled mid-batch: batch[i] is (partially) unsent.
         * Keep it and everything after it in the window and resume on
         * writable; the window head is now the first kept response. */
        batch[i]->sent = abs - cum; /* < blen; 0 = not started at all */
        ctx->send_full = true;
        aio_set_fd_handler(vq_get_aio_ctx(vq), fd,
                           stub_distributor, stub_send_writable,
                           NULL, NULL, vq);
        return false;
    }
    return true;
}

/*
 * send worker task for a vq: drain the in-flight window head. Consecutive
 * copy responses are merged into one sendmsg() (VR_STUB_BATCH_*); zc
 * responses flush the pending batch and go through MSG_ZEROCOPY alone. The
 * zc serial/pending list is owned by this worker alone (the single consumer
 * of the window). When the window is fully drained, broadcasts push_cond so
 * a push blocked on a full window (backpressure) can proceed.
 */

/* a hard sendmsg error cannot be recovered: the head slot never drains and no
 * socket event reaches the iothread (the error is local to the send), so the
 * send worker would otherwise re-dispatch the stuck head slot in an endless
 * loop. Defer the connection teardown onto the socket iothread - it must not
 * run here, the teardown parks the workers including this one. */
static void stub_send_err_teardown(void *opaque)
{
    VirtQueue *vq = opaque;
    RemoteVQueueCtx *ctx = virtqueue_get_remote_ctx(vq);

    if (!ctx) {
        return;
    }
    AioContext *ctx_aio = vq_get_aio_ctx(vq);
    if (ctx_aio && ctx->resp_fd >= 0) {
        aio_set_fd_handler(ctx_aio, ctx->resp_fd, NULL, NULL, NULL, NULL, NULL);
    }
    stub_teardown_vq(vq);
}

/* cmsvm: pre-sendmsg probe - record the exact iov handed to sendmsg and the
 * first 8 wire bytes about to go out. SRES: (a=seq, b=sr->sent, c=total iov
 * bytes); SRDMP: (a=seq, b=first 4B, c=next 4B). Used to reconcile the
 * "ret=128 but only 63 requested" anomaly on the last zc-resume drip. */
static void stub_log_send_probe(VirtQueue *vq, unsigned int seq,
                                unsigned int sent, const struct msghdr *msg)
{
    unsigned int req = 0;
    for (unsigned int i = 0; i < msg->msg_iovlen; i++) {
        req += msg->msg_iov[i].iov_len;
    }
    vr_ev_log(VR_EV_SRES, vq, seq, sent, req);
    if (msg->msg_iovlen > 0 && msg->msg_iov[0].iov_base) {
        const unsigned char *p = msg->msg_iov[0].iov_base;
        uint32_t b0, b1;
        memcpy(&b0, p, 4);
        memcpy(&b1, p + 4, 4);
        vr_ev_log(VR_EV_SRDMP, vq, seq, b0, b1);
    }
}

static void stub_send_task(void *opaque)
{
    VirtQueue *vq = opaque;
    RemoteVQueueCtx *ctx = virtqueue_get_remote_ctx(vq);
    Worker *w = worker_pool_worker(&send_pool, vq);
    Inflight *win;
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
    stub_zc_drain(ctx, vq);
    vr_ev_log(VR_EV_SDRAIN, vq, 0, 0, 0);

    win = ctx->inflight;
    fd = ctx->resp_fd;
    if (!win || fd < 0) {
        goto out;
    }
    for (;;) {
        StubResp *batch[VR_BATCH_MAX];
        unsigned int n = 0;
        ssize_t bytes = 0;
        ZcFdState *st = ctx->zc;

        /* gather the consecutive copy responses at the window head */
        while (n < vr_stub_batch_n && bytes < vr_stub_batch_m) {
            unsigned int seq = qatomic_load_acquire(&win->head) + n;
            StubResp *sr = inflight_lookup(win, seq);
            if (!sr) {
                break; /* window drained (or the head resp is mid-publish) */
            }
            if (ctx->send_full) {
                break; /* socket still full: the parked handler retries */
            }
            if (st && st->enabled && sr->len >= vr_zc_send_min) {
                break; /* zc resp: send it alone below */
            }
            batch[n++] = sr;
            bytes += sr->len;
        }
        if (n > 0) {
            if (ctx->send_full) {
                goto out;
            }
            /* capture the batch head seq before the send: stub_send_batch
             * frees the sent responses, so the log must not touch batch[] */
            unsigned int seq0 = qatomic_load_acquire(&win->head);
            /* cmsvm: capture the batch-head header bytes that are about to go
             * on the wire. If the batch head was corrupted (e.g. a freed /
             * zeroed header), a=0 b=0 c=0 here is the smoking gun that matches
             * the local side's zero-LHDR crash */
            vr_ev_log(VR_EV_SBHD, vq, batch[0]->header[0], batch[0]->header[1],
                      batch[0]->header[2]);
            if (!stub_send_batch(fd, batch, n, win, vq, w)) {
                goto out; /* EAGAIN parked the writable handler */
            }
            vr_ev_log(VR_EV_SSEND, vq, seq0, n, 0);
            continue;
        }

        /* nothing copy-eligible gathered: drained, parked, or a zc resp at
         * the head */
        unsigned int seq = qatomic_load_acquire(&win->head);
        StubResp *sr = inflight_lookup(win, seq);
        if (!sr) {
            break; /* window drained (or the head resp is mid-publish) */
        }
        vr_ev_log(VR_EV_SRPT, vq, seq, (uint32_t)(uintptr_t)sr, sr->total);
        if (ctx->send_full) {
            goto out;
        }
        /* log the header bytes going on the wire (first send of this resp
         * only, not the 128B drip chunks) */
        if (sr->sent == 0) {
            vr_ev_log(VR_EV_SHDR, vq, sr->header[0], sr->header[1],
                      sr->header[2]);
        }

        struct iovec *tail_sg = NULL;
        struct msghdr msg;
        bool used_zc = false;
        ssize_t ret;
        if (sr->sent > 0) {
            /* short-write resume: queue only the unsent tail, as a copy
             * send - a zc part (if any) was already queued and must not be
             * sent again */
            if (sr->tail) {
                /* zc short write: a stub_zc_drain may already have released
                 * sr->iov (and the header), so send from the snapshot taken
                 * when the short write happened */
                struct iovec tail_io = {
                    .iov_base = (char *)sr->tail + (sr->sent - sr->tail_base),
                    .iov_len = sr->total - sr->sent,
                };
                msg = (struct msghdr){ .msg_iov = &tail_io, .msg_iovlen = 1 };
                vr_ev_log(VR_EV_SRBT, vq, seq, 1, msg.msg_iovlen);
                vr_ev_log(VR_EV_SRTA, vq, seq,
                          (uint32_t)(uintptr_t)sr->tail, sr->tail_base);
            } else {
                tail_sg = g_new(struct iovec, sr->iov_cnt);
                unsigned int n_tail = stub_resp_iov_tail(sr, sr->sent, tail_sg,
                                                         sr->iov_cnt);
                msg = (struct msghdr){ .msg_iov = tail_sg, .msg_iovlen = n_tail };
                vr_ev_log(VR_EV_SRBT, vq, seq, 0, msg.msg_iovlen);
            }
            stub_log_send_probe(vq, seq, sr->sent, &msg);
            ret = sendmsg(fd, &msg, MSG_NOSIGNAL);
        } else if (st && st->enabled && sr->len >= vr_zc_send_min) {
            msg = (struct msghdr){ .msg_iov = sr->iov,
                                   .msg_iovlen = sr->iov_cnt };
            stub_log_send_probe(vq, seq, sr->sent, &msg);
            ret = sendmsg(fd, &msg, MSG_ZEROCOPY | MSG_NOSIGNAL);
            if (ret < 0 && (errno == ENOBUFS || errno == EINVAL)) {
                /* kernel refuses zc for this call: fall back to a copy send */
                ret = sendmsg(fd, &msg, MSG_NOSIGNAL);
            } else if (ret > 0) {
                used_zc = true;
            }
        } else {
            msg = (struct msghdr){ .msg_iov = sr->iov,
                                   .msg_iovlen = sr->iov_cnt };
            stub_log_send_probe(vq, seq, sr->sent, &msg);
            ret = sendmsg(fd, &msg, MSG_NOSIGNAL);
        }
        g_free(tail_sg);
        if (ret < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            /* send buffer full: keep the head parked and retry on writable.
             * The writable handler is registered on the stub's main aio ctx
             * next to the POLLIN stub_distributor (one fd -> one aio ctx),
             * so the retry lands on the send worker via stub_send_writable. */
            ctx->send_full = true;
            aio_set_fd_handler(vq_get_aio_ctx(vq), fd,
                               stub_distributor, stub_send_writable,
                               NULL, NULL, vq);
            goto out;
        }
        if (ret < 0) {
            /* hard error: keep the slot; the connection teardown releases it.
             * The error is local to the sendmsg - no socket event fires to
             * trigger conn_err - so tear the connection down now (deferred
             * off this worker), or the worker bh would keep re-dispatching
             * the stuck head slot in an endless loop. */
            error_report("remote stub: sendmsg resp failed: %s",
                         strerror(errno));
            AioContext *ctx_aio = vq_get_aio_ctx(vq);
            if (ctx_aio) {
                aio_bh_schedule_oneshot(ctx_aio, stub_send_err_teardown, vq);
            }
            goto out;
        }
        vr_debug("vremote: stub send vq %d seq %u len %u",
                     virtio_get_queue_index(vq), seq, sr->len);
        vr_ev_log(VR_EV_SSEND, vq, seq, 1, 0);
        /* cmsvm: exact wire accounting for this send (a=seq, b=bytes queued,
         * c=iov bytes requested) */
        vr_ev_log(VR_EV_SSND, vq, seq, (unsigned int)ret,
                  sr->total - sr->sent);

        unsigned int payload = sr->total - sr->sent;
        if ((unsigned int)ret < payload) {
            /* short write: the send buffer filled mid-response. Record the
             * resume point and retry the tail on writable. If the queued
             * part went out with MSG_ZEROCOPY the kernel has allocated a zc
             * serial for it (and will queue a completion), so register a
             * placeholder to keep the serial counters in sync - the resp
             * bufs themselves stay owned by sr until the resume completes. */
            sr->sent += (unsigned int)ret;
            vr_ev_log(VR_EV_SSWR, vq, seq, sr->sent, sr->total - sr->sent);
            if (used_zc) {
                /* zc short-write: the kernel zc'd [0, ret) and still holds
                 * page references to the header/iov/in bufs until its
                 * completion arrives. Snapshot the unsent tail NOW - the copy
                 * resume below runs in a later task whose entry drain may
                 * already have released sr->iov/header, so the resume must
                 * not touch them. Then hand those bufs to the completion
                 * tracker (returning them to buf_pool early would let a new
                 * resp overwrite pages the NIC is still DMA-ing). */
                unsigned int tail_len = sr->total - sr->sent;
                sr->tail = g_malloc(tail_len);
                sr->tail_base = sr->sent;
                unsigned int copied = stub_resp_copy_range(sr, sr->sent,
                                                           sr->tail, tail_len);
                vr_ev_log(VR_EV_SRSN, vq, seq, tail_len, copied);
                unsigned int sgs = sr->iov_cnt - 1;
                void **bufs = g_new(void *, 2 + sgs);
                unsigned int m = 0;
                bufs[m++] = sr->header;
                bufs[m++] = sr->iov;
                for (unsigned int i = 0; i < sgs; i++) {
                    bufs[m++] = sr->in_bufs[i];
                }
                ZcPending *zp = g_new0(ZcPending, 1);
                zp->serial = st->serial++;
                zp->bufs = bufs;
                zp->n_bufs = m;
                st->pending = g_slist_prepend(st->pending, zp);
                sr->zc_deferred = true;
            }
            ctx->send_full = true;
            aio_set_fd_handler(w->ctx, fd, NULL, stub_send_writable,
                               NULL, NULL, vq);
            goto out;
        }

        unsigned int sgs = sr->iov_cnt - 1;
        if (used_zc) {
            /* release the header, iov and the sent in buffers on the zc
             * completion, not now */
            void **bufs = g_new(void *, 2 + sgs);
            unsigned int m = 0;
            bufs[m++] = sr->header;
            bufs[m++] = sr->iov;
            for (unsigned int i = 0; i < sgs; i++) {
                bufs[m++] = sr->in_bufs[i];
            }
            ZcPending *zp = g_new0(ZcPending, 1);
            zp->serial = st->serial++;
            zp->bufs = bufs;
            zp->n_bufs = m;
            st->pending = g_slist_prepend(st->pending, zp);
        }
        if (sr->zc_deferred) {
            /* the header/iov/in bufs were handed to the zc completion
             * tracker by the earlier short write; this copy resume of the
             * tail sent from the snapshot, so release the snapshot, the
             * arrays and the struct */
            g_free(sr->tail);
            g_free(sr->in_bufs);
            g_free(sr);
        } else {
            stub_resp_free(sr, sgs, used_zc);
        }
        inflight_clear(win, seq);
    }
    /* the window has space again: wake a push blocked on backpressure */
    g_mutex_lock(&ctx->push_lock);
    g_cond_broadcast(&ctx->push_cond);
    g_mutex_unlock(&ctx->push_lock);
out:
    qatomic_set(&ctx->send_busy, 0);
}

/* io_write handler on the stub's main aio ctx (registered together with the
 * POLLIN stub_distributor - qemu polls one fd on ONE aio context only): the
 * socket has send buffer space again, so resume draining the in-flight
 * window head. */
static void stub_send_writable(void *opaque)
{
    VirtQueue *vq = opaque;
    RemoteVQueueCtx *ctx = virtqueue_get_remote_ctx(vq);

    if (!ctx) {
        return;
    }
    ctx->send_full = false;
    /* detach the writable handler registered on the stub's main aio ctx and
     * restore the POLLIN stub_distributor read handler for the fd */
    AioContext *ctx_aio = vq_get_aio_ctx(vq);
    if (ctx_aio && ctx->resp_fd >= 0) {
        aio_set_fd_handler(ctx_aio, ctx->resp_fd,
                           stub_distributor, NULL, NULL, NULL, vq);
    }
    if (qatomic_load_acquire(&ctx->dead)) {
        return;
    }
    worker_pool_dispatch(&send_pool, vq, stub_send_task, NULL);
}

/*
 * per-vq receive state on the stub side for incremental, non-blocking reads
 * of the req stream
 * [vq_nr(4B)][seq(4B)][out_num(4B)][in_num(4B)]
 * [lens: (out_num+in_num) x 4B][out_sg data...]
 * Owned by the vq's handle worker (the single consumer of the socket), stored
 * in ctx->recv; partial reads resume across dispatches.
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

/* release any partially-parsed request buffers of rs and reset the parsing
 * state (idempotent). Called by the handle worker on a parse error / EOF and
 * by the teardown paths; the sg arrays may already have been handed to a
 * StubReq (then they are NULL here, so their buffers are not touched). */
static void stub_recv_state_reset(StubRecvState *rs)
{
    if (!rs) {
        return;
    }
    g_free(rs->lens);
    rs->lens = NULL;
    if (rs->out_sg) {
        for (unsigned int i = 0; i < rs->out_num && rs->out_sg[i].iov_base; i++) {
            g_free(rs->out_sg[i].iov_base);
        }
        g_free(rs->out_sg);
        rs->out_sg = NULL;
    }
    if (rs->in_sg) {
        for (unsigned int i = 0; i < rs->in_num && rs->in_sg[i].iov_base; i++) {
            buf_pool_free(rs->in_sg[i].iov_base); /* pooled resp buffers */
        }
        g_free(rs->in_sg);
        rs->in_sg = NULL;
    }
    rs->stage = 0;
    rs->hdr_off = 0;
    rs->lens_off = 0;
    rs->data_off = 0;
    rs->out_total = 0;
    rs->out_num = 0;
    rs->in_num = 0;
    rs->seq = 0;
}

static void stub_recv_state_free(StubRecvState *rs)
{
    if (!rs) {
        return;
    }
    stub_recv_state_reset(rs);
    g_free(rs);
}

/*
 * incrementally receive and parse one request from the vq socket, resuming
 * from the partial state in ctx->recv. Returns a complete StubReq (ownership
 * of the out/in sg arrays transfers to it) or NULL on EAGAIN / a dead or
 * errored connection. On a hard error or EOF the partial buffers are dropped
 * and NULL is returned; the socket iothread observes the error (its peek
 * returns 0/error) and runs the connection teardown.
 */
static StubReq *stub_recv_req(RemoteVQueueCtx *ctx, int fd)
{
    StubRecvState *rs = ctx->recv;

    if (!rs) {
        rs = g_new0(StubRecvState, 1);
        ctx->recv = rs;
    }

    while (true) {
        if (rs->stage == 0) {
            /* req header: [vq_nr][seq][out_num][in_num], native endian */
            ssize_t n = recv(fd, rs->hdr + rs->hdr_off,
                             sizeof(rs->hdr) - rs->hdr_off, 0);
            if (n < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    return NULL; /* wait for the next readable event */
                }
                error_report("remote stub: recv req header failed: %s",
                             strerror(errno));
                stub_recv_state_reset(rs);
                return NULL;
            }
            if (n == 0) {
                error_report("remote stub: local qemu closed vq connection");
                stub_recv_state_reset(rs);
                return NULL;
            }
            rs->hdr_off += n;
            if (rs->hdr_off < sizeof(rs->hdr)) {
                return NULL; /* header incomplete, wait for more */
            }
            int vq_nr;
            memcpy(&vq_nr, rs->hdr, 4);
            memcpy(&rs->seq, rs->hdr + 4, 4);
            memcpy(&rs->out_num, rs->hdr + 8, 4);
            memcpy(&rs->in_num, rs->hdr + 12, 4);
            if (vq_nr != ctx->vq_nr) {
                error_report("remote stub: req vq_nr %d, expected %d",
                             vq_nr, ctx->vq_nr);
                stub_recv_state_reset(rs);
                return NULL;
            }
            if (rs->out_num > VIRTQUEUE_MAX_SIZE ||
                rs->in_num > VIRTQUEUE_MAX_SIZE) {
                error_report("remote stub: bogus out_num %u in_num %u",
                             rs->out_num, rs->in_num);
                stub_recv_state_reset(rs);
                return NULL;
            }
            rs->lens = g_new0(int, rs->out_num + rs->in_num);
            rs->out_sg = g_new0(struct iovec, rs->out_num);
            rs->in_sg = g_new0(struct iovec, rs->in_num);
            rs->stage = 1;
            vr_debug("vremote: stub recv vq %d seq %d out=%u in=%u",
                         vq_nr, rs->seq, rs->out_num, rs->in_num);
        }

        if (rs->stage == 1) {
            size_t lens_bytes = (rs->out_num + rs->in_num) * sizeof(int);
            unsigned int in_total = 0;
            ssize_t n = recv(fd, (char *)rs->lens + rs->lens_off,
                             lens_bytes - rs->lens_off, 0);
            if (n < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    return NULL;
                }
                error_report("remote stub: recv req lens failed: %s",
                             strerror(errno));
                stub_recv_state_reset(rs);
                return NULL;
            }
            if (n == 0) {
                error_report("remote stub: local qemu closed vq mid-lens");
                stub_recv_state_reset(rs);
                return NULL;
            }
            rs->lens_off += n;
            if (rs->lens_off < lens_bytes) {
                return NULL;
            }
            /* allocate the out/in buffers from the received lens */
            rs->out_total = 0;
            for (unsigned int i = 0; i < rs->out_num; i++) {
                rs->out_sg[i].iov_len = rs->lens[i];
                rs->out_sg[i].iov_base = g_new(char, rs->out_sg[i].iov_len);
                rs->out_total += rs->out_sg[i].iov_len;
            }
            /* M5: stub_merge_m > 0 merges all in_sg entries of one request
             * (one virtio elem) into a single contiguous buffer: the device
             * then runs one large aio instead of in_num page-sized aios and
             * the response is one iov. The wire resp is unchanged (it carries
             * only data_len), and the merged buffer is page-aligned so the
             * zc resp path still applies. */
            for (unsigned int i = 0; i < rs->in_num; i++) {
                in_total += rs->lens[rs->out_num + i];
            }
            if (vr_stub_merge_m > 0 && rs->in_num > 1 &&
                in_total > 0 && in_total <= vr_stub_merge_m) {
                rs->in_sg[0].iov_len = in_total;
                rs->in_sg[0].iov_base = buf_pool_alloc(in_total);
                if (!rs->in_sg[0].iov_base) {
                    error_report("remote stub: buf_pool_alloc(%u) failed: %s",
                                 in_total, strerror(errno));
                    stub_recv_state_reset(rs);
                    return NULL;
                }
                rs->in_num = 1;
            } else {
                for (unsigned int i = 0; i < rs->in_num; i++) {
                    rs->in_sg[i].iov_len = rs->lens[rs->out_num + i];
                    /* page-aligned (and page-multiple sized) so the response
                     * send can use MSG_ZEROCOPY and let the NIC DMA straight
                     * from these buffers; recycled from the M5 pool when on */
                    if (rs->in_sg[i].iov_len == 0) {
                        rs->in_sg[i].iov_base = NULL;
                    } else {
                        rs->in_sg[i].iov_base = buf_pool_alloc(rs->in_sg[i].iov_len);
                        if (!rs->in_sg[i].iov_base) {
                            error_report("remote stub: buf_pool_alloc failed: %s",
                                         strerror(errno));
                            stub_recv_state_reset(rs);
                            return NULL;
                        }
                    }
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
                        return NULL;
                    }
                    error_report("remote stub: recv req data failed: %s",
                                 strerror(errno));
                    stub_recv_state_reset(rs);
                    return NULL;
                }
                if (n == 0) {
                    error_report("remote stub: local qemu closed vq mid-data");
                    stub_recv_state_reset(rs);
                    return NULL;
                }
                rs->data_off += n;
                if (rs->data_off < rs->out_total) {
                    continue; /* keep filling the out buffers */
                }
            }

            /* full request received: hand the sg arrays to a StubReq */
            StubReq *req = g_new0(StubReq, 1);
            req->elem_index = rs->seq;
            req->out_num = rs->out_num;
            req->in_num = rs->in_num;
            req->out_sg = rs->out_sg;   /* ownership transfers to the req */
            req->in_sg = rs->in_sg;
            rs->out_sg = NULL;
            rs->in_sg = NULL;
            vr_debug("vremote: stub req complete vq %d seq %d out=%u in=%u",
                         ctx->vq_nr, rs->seq, req->out_num, req->in_num);
            stub_recv_state_reset(rs);
            return req;
        }
    }
}

/*
 * handle worker task for a vq: parse the request stream and run the device
 * handle_output. The socket iothread only hands the vq over on readability,
 * so a partial parse returns here (EAGAIN) and resumes on the next dispatch;
 * the level-triggered epoll keeps re-firing as long as data remains, so no
 * readable event is ever lost.
 *
 * Requests are parsed off the socket and queued in ctx->req_queue (private
 * to this worker: the parser enqueues and pop() dequeues on this same
 * thread, so no locking is needed). in_handle is the handle_output re-entry
 * guard: it is set before call_handle_output and cleared by the device's
 * push() on whichever thread that runs, so the device can consume the whole
 * queue as one batch (while (pop()) in its handle_output) and the IO of one
 * batch overlaps the parsing of the next, without two handle_outputs for the
 * vq ever being dispatched at once. Sync devices are unaffected: their push
 * runs inside handle_output, so in_handle is already cleared when the call
 * returns. If reqs pile up while a batch is still in flight, the push that
 * ends it re-dispatches this task.
 */
/* take the handle_output slot (in_handle must be 0) and drive the device
 * through one batch of queued requests. Only this worker ever sets in_handle
 * (a push only clears it), so the take-over cannot race another caller; a
 * stale push clearing the flag mid-batch is impossible for sync devices and
 * only softens the guard for async ones, which is safe because handle_output
 * is serialized on this worker. pop() frees each req shell as the device
 * dequeues it, so the loop just keeps driving the device until the queue is
 * drained. A sync device's push clears in_handle inside the call, so after
 * the batch the parse loop can take the slot again; an async device leaves
 * it set and the pushes that end the batch re-dispatch this task if reqs
 * piled up. */
static void stub_drive_queue(VirtQueue *vq, RemoteVQueueCtx *ctx)
{
    int before;

    qatomic_set(&ctx->in_handle, 1);
    while ((before = qatomic_load_acquire(&ctx->req_count)) > 0) {
        virtqueue_call_handle_output(vq); /* device pops (and pushes) */
        if (qatomic_load_acquire(&ctx->dead)) {
            break; /* teardown: stop feeding the device */
        }
        if (qatomic_load_acquire(&ctx->req_count) >= before) {
            break; /* the device consumed nothing: stop driving it */
        }
    }
}

static void stub_handle_task(void *opaque)
{
    VirtQueue *vq = opaque;
    RemoteVQueueCtx *ctx = virtqueue_get_remote_ctx(vq);
    int fd;

    if (!ctx) {
        return; /* a dispatch that outlived the ctx teardown */
    }
    qatomic_set(&ctx->handle_busy, 1);
    if (qatomic_load_acquire(&ctx->dead)) {
        goto out;
    }
    fd = ctx->resp_fd;
    if (fd < 0) {
        goto out;
    }
    for (;;) {
        if (vr_stub_queue_max > 0 &&
            qatomic_load_acquire(&ctx->req_count) >= vr_stub_queue_max &&
            qatomic_load_acquire(&ctx->in_handle)) {
            /* recv-batch cap reached while the device is mid-batch: stop
             * parsing; the push that ends the batch drains the queue and
             * re-dispatches this task. When the device is idle the queue is
             * driven below regardless (a full idle queue would otherwise
             * strand until the next POLLIN - which never comes once the
             * local window is exhausted). */
            break;
        }
        /* drain the queue before parsing: a re-dispatch (from the device's
         * push) can arrive with requests already queued but nothing left on
         * the socket, and those reqs must still be handed to the device.
         * Otherwise the task would take the EAGAIN path below and strand
         * them until the next readable event never comes. */
        if (qatomic_load_acquire(&ctx->req_count) > 0 &&
            !qatomic_load_acquire(&ctx->in_handle)) {
            stub_drive_queue(vq, ctx);
            if (qatomic_load_acquire(&ctx->dead)) {
                break;
            }
        }
        StubReq *req = stub_recv_req(ctx, fd);
        if (!req) {
            break; /* EAGAIN (wait for the next readable event) or conn err */
        }
        if (qatomic_load_acquire(&ctx->dead)) {
            stub_req_free(req, true);
            break;
        }
        vr_ev_log(VR_EV_SREQ, vq, req->elem_index, 0, 0);
        g_queue_push_tail(ctx->req_queue, req);
        qatomic_store_release(&ctx->req_count,
                              g_queue_get_length(ctx->req_queue));
        if (qatomic_load_acquire(&ctx->in_handle)) {
            /* the device is still finishing the previous batch (async push
             * pending): keep the req queued; the push that ends the batch
             * re-dispatches this task if reqs piled up */
            continue;
        }
        stub_drive_queue(vq, ctx);
        if (qatomic_load_acquire(&ctx->dead)) {
            break;
        }
    }
    if (qatomic_load_acquire(&ctx->dead)) {
        /* connection gone: release the reqs the device never popped. Once
         * handed, the buffers are the device's (its dropped push freed
         * them), so only the never-popped reqs free their payloads. */
        StubReq *r;
        while ((r = g_queue_pop_head(ctx->req_queue))) {
            stub_req_free(r, !r->handed);
        }
        qatomic_store_release(&ctx->req_count, 0);
        qatomic_store_release(&ctx->in_handle, 0);
    }
out:
    qatomic_set(&ctx->handle_busy, 0);
}

/* -------------- Remote Stub Handlers  ------------- */

/*
 * aio fd handler on the stub side for one vq socket - a pure dispatcher. The
 * protocol parsing lives in the vq's handle worker (stub_handle_task), which
 * also runs the device; this handler only tells the two event kinds apart and
 * hands the vq to the right worker:
 *   - data ready -> the vq's handle worker (parses and processes inline)
 *   - bare POLLERR -> a MSG_ZEROCOPY completion; the send worker is the sole
 *     owner of the zc state, so it is dispatched to reap it.
 * The recv worker is fixed per vq (worker_pool_worker hashes the vq onto
 * exactly one recv-pool worker), so no second worker is ever assigned to
 * the same vq.
 */
void stub_distributor(void *opaque)
{
    VirtQueue *vq = opaque;
    RemoteVQueueCtx *ctx = virtqueue_get_remote_ctx(vq);
    int fd = ctx->resp_fd;
    char probe;

    /* a zc completion is signalled as POLLERR and wakes this read handler
     * without any request data. Peek one byte to tell the two apart: a data
     * event goes to the handle worker, a bare POLLERR event only reaps the zc
     * state, which belongs to the send worker. */
    ssize_t n = recv(fd, &probe, 1, MSG_PEEK);
    if (n == 0) {
        error_report("remote stub: local qemu closed vq connection");
        goto conn_err;
    }
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            /* bare POLLERR: a MSG_ZEROCOPY completion, no request data */
            vr_ev_log(VR_EV_SDISP, vq, 1, 0, 0);
            if (!qatomic_load_acquire(&ctx->dead)) {
                worker_pool_dispatch(&send_pool, vq, stub_send_task, NULL);
            }
            return;
        }
        error_report("remote stub: recv req probe failed: %s", strerror(errno));
        goto conn_err;
    }
    /* data ready: hand the vq to its handle worker for parsing + processing.
     * The dispatch is overwriting (no busy-claim): if the worker is busy the
     * event is queued and the partial parse resumes when it runs, and the
     * level-triggered epoll keeps re-firing while data remains, so no readable
     * event is ever lost. When the recv-batch cap is reached while the device
     * is mid-batch, skip the dispatch: the push that ends the batch re-arms
     * it, so this keeps the re-firing epoll from spinning a parse that would
     * only hit the cap again. */
    vr_ev_log(VR_EV_SDISP, vq, 0, 0, 0);
    if (!qatomic_load_acquire(&ctx->dead) &&
        !(vr_stub_queue_max > 0 &&
          qatomic_load_acquire(&ctx->req_count) >= vr_stub_queue_max &&
          qatomic_load_acquire(&ctx->in_handle))) {
        worker_pool_dispatch(&recv_pool, vq, stub_handle_task, NULL);
    }
    return;

conn_err:
    /* the connection is unusable: drop the fd handler, then tear down.
     * stub_teardown_vq parks the workers, then releases the half-parsed recv
     * state, the resp window, the zc state and closes the fd. */
    AioContext *ctx_aio = vq_get_aio_ctx(vq);
    if (ctx_aio) {
        aio_set_fd_handler(ctx_aio, fd, NULL, NULL, NULL, NULL, NULL);
    }
    stub_teardown_vq(vq);
}

bool remote_virtio_queue_empty(void *opaque)
{
    RemoteVQueueCtx *ctx = opaque;
    /* nothing left for the device to pop: the req queue is drained. The
     * elem marker is not consulted: for an async device it stays set after
     * a drain until the pushes land, but that must still read as empty. */
    return qatomic_load_acquire(&ctx->req_count) == 0;
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

void *remote_stub_virtqueue_pop(VirtQueue *vq, size_t sz)
{
    RemoteVQueueCtx *ctx = virtqueue_get_remote_ctx(vq);
    StubReq *req;
    VirtQueueElement *ret;
    int out_num, in_num;

    if (!ctx) {
        return NULL;
    }
    /* one pop = one request off the handle worker's queue (filled on the
     * same worker thread, so no locking is needed here) */
    req = g_queue_pop_head(ctx->req_queue);
    if (!req) {
        return NULL;
    }
    qatomic_store_release(&ctx->req_count, g_queue_get_length(ctx->req_queue));
    out_num = req->out_num;
    in_num = req->in_num;
    ret = remote_stub_virtqueue_alloc_element(sz, out_num, in_num);
    ret->index = req->elem_index;
    vr_ev_log(VR_EV_SPOP, vq, req->elem_index, out_num, in_num);
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
    /* the payload buffers now belong to the device (the elem references
     * them, and its push() releases them); the iovec entries are copied
     * into the elem above, so the req shell can go right away */
    stub_req_free(req, false);
    return ret;
}

/*
 * called by virtio.c through virtqueue_push()/virtqueue_fill() when
 * vq->remote_ctx is set (remote stub side). Builds the response
 * [vq_nr(4B)][elem_index(4B)][data_len(4B)][data...], publishes it into the
 * vq's in-flight window (ctx->inflight) for the send worker and clears the
 * in_handle re-entry guard. May run on any
 * thread (device async completions): the publish is serialized with
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
            sr->total = resp_iov[0].iov_len + len;
            sr->n_in_sg = elem->in_num;
            sr->in_bufs = g_new(void *, elem->in_num);
            for (unsigned int i = 0; i < elem->in_num; i++) {
                sr->in_bufs[i] = elem->in_sg[i].iov_base;
            }
            vr_debug("vremote: stub push vq %d elem %u len %u",
                         ctx->vq_nr, elem->index, len);
        }
    }

    /* the out buffers are always released here; the elem is now complete */
    for (unsigned int i = 0; i < elem->out_num && elem->out_sg[i].iov_base; i++) {
        g_free(elem->out_sg[i].iov_base);
    }

    /* publish under push_lock: the re-check of dead closes the race with
     * conn_err tearing the windows down (it takes the same lock). */
    g_mutex_lock(&ctx->push_lock);
    if (qatomic_load_acquire(&ctx->dead)) {
        g_mutex_unlock(&ctx->push_lock);
        if (sr) {
            stub_resp_free(sr, sr->iov_cnt - 1, false);
        } else {
            for (unsigned int i = 0; i < elem->in_num && elem->in_sg[i].iov_base; i++) {
                buf_pool_free(elem->in_sg[i].iov_base);
            }
        }
        return;
    }
    if (sr) {
        Inflight *win = ctx->inflight;
        if (!win) {
            g_mutex_unlock(&ctx->push_lock);
            stub_resp_free(sr, sr->iov_cnt - 1, false);
            return;
        }
        /* backpressure: the window is sized for vring.num in-flight requests
         * (one resp each), so this wait is a pure safety net; the send worker
         * broadcasts push_cond once it drains the window */
        while (win->tail - qatomic_load_acquire(&win->head) >=
               win->size) {
            vr_ev_log(VR_EV_SPUSHW, vq, win->tail, win->head, 0);
            g_cond_wait(&ctx->push_cond, &ctx->push_lock);
            if (qatomic_load_acquire(&ctx->dead)) {
                g_mutex_unlock(&ctx->push_lock);
                stub_resp_free(sr, sr->iov_cnt - 1, false);
                return;
            }
        }
        inflight_publish(win, win->tail, sr, NULL);
        vr_ev_log(VR_EV_SPUSH, vq, win->tail - 1, 0, 0);
    } else {
        /* no (or dropped) resp: the in buffers are released right away */
        for (unsigned int i = 0; i < elem->in_num && elem->in_sg[i].iov_base; i++) {
            buf_pool_free(elem->in_sg[i].iov_base);
        }
    }
    /* end of a handle_output batch (or of one of its async pushes): clear
     * the re-entry guard. If requests piled up while the batch was in
     * flight, hand the queue back to the handle worker - the push that
     * clears in_handle last (i.e. the final push of the batch) is the
     * handoff point, so a busy worker is safe to re-dispatch (its task
     * overwrite is recovered by stub_worker_bh's re-scan). The resp is
     * handed to the send worker here too: the push may run on a device
     * thread with no worker task in flight, so no re-scan would pick the
     * window up. The window is sampled under push_lock (where the teardown
     * frees it), so a racing teardown cannot free it mid-read. */
    qatomic_store_release(&ctx->in_handle, 0);
    Inflight *win = ctx->inflight;
    bool want_send = win && !ctx->send_full &&
                     qatomic_load_acquire(&win->head) <
                         qatomic_load_acquire(&win->tail);
    g_mutex_unlock(&ctx->push_lock);
    if (qatomic_load_acquire(&ctx->req_count) > 0) {
        worker_pool_dispatch(&recv_pool, vq, stub_handle_task, NULL);
    }
    if (want_send) {
        worker_pool_dispatch(&send_pool, vq, stub_send_task, NULL);
    }
}

/* -------------- Remote Stub ThreadPool ------------- */

/* stub pool bh: run the dispatched task, then re-scan every registered vq so
 * responses published while the worker was busy are picked up (a dropped
 * dispatch is never a lost wakeup). The send side re-dispatches when the
 * window holds a published response. The handle side re-dispatches when a
 * parsed request is queued with the device idle: its only other wakeup is
 * the push() re-dispatch, and a dispatch overwrite while the worker is busy
 * can lose it (with vr_workers=1 every vq shares worker 0), stranding the
 * req forever once the socket is quiet and no device IO is in flight. The
 * !in_handle check keeps a device that is mid-batch (or cannot consume) from
 * being re-driven into a spin. Dead vqs are skipped. */
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
        Inflight *win;
        if (!ctx || qatomic_load_acquire(&ctx->dead)) {
            continue;
        }
        win = ctx->inflight;
        if (win && !ctx->send_full &&
            qatomic_load_acquire(&win->head) <
                qatomic_load_acquire(&win->tail)) {
            /* Only re-dispatch when the window head slot actually holds a
             * response. A NULL head (a hole) means the consumer cannot make
             * progress - re-dispatching would only spin the send worker at
             * 100% CPU (the window is corrupt; the connection teardown is
             * the recovery path). */
            uint32_t h = qatomic_load_acquire(&win->head);
            if (inflight_slot(win, h)->elem != NULL) {
                worker_pool_dispatch(&send_pool, vq, stub_send_task, NULL);
            }
        }
        if (!w->pool->is_send &&
            qatomic_load_acquire(&ctx->req_count) > 0 &&
            !qatomic_load_acquire(&ctx->in_handle)) {
            /* queued reqs with the device idle: drive them again (see the
             * comment above; a push's handle re-dispatch can be overwritten
             * by another vq's dispatch while this worker was busy) */
            worker_pool_dispatch(&recv_pool, vq, stub_handle_task, NULL);
        }
    }
}

/* stub dispatch policy: overwrite the worker's pending task and wake its bh.
 * No busy-claim: a dispatch to a busy worker overwrites the pending task, but
 * stub_worker_bh re-scans every registered vq afterwards, so no published
 * work is ever lost. The socket iothread (pure dispatcher) and the handle
 * worker (after each push) are the usual callers. opaque unused. */
static bool stub_dispatch(WorkerPool *pool, VirtQueue *vq,
                          void (*fn)(void *opaque), void *opaque)
{
    Worker *w = worker_pool_worker(pool, vq);

    w->task.vq = vq;
    w->task.fn = fn;
    qemu_bh_schedule(w->bh);
    return true;
}

/* register a stub vq with its recv and send pools (called by the accept
 * handler once per accepted vq). Setup-time append, read lock-free by
 * stub_worker_bh, same pattern as local_register_vq. */
void stub_register_vq(VirtQueue *vq)
{
    worker_pool_register_vq(&recv_pool, vq);
    worker_pool_register_vq(&send_pool, vq);
}

/* release every element still in the in-flight window (connection teardown)
 * and free the window itself. */
static void stub_win_release(Inflight *win)
{
    uint32_t head = qatomic_load_acquire(&win->head);
    uint32_t tail = qatomic_load_acquire(&win->tail);

    for (uint32_t seq = head; seq < tail; seq++) {
        InflightSlot *slot = inflight_slot(win, seq);
        if (!slot->elem) {
            continue;
        }
        StubResp *sr = slot->elem;
        /* a zc_deferred resp's header/iov/in bufs are owned by its zc
         * completion entry, which stub_teardown_vq releases first via
         * stub_zc_fd_teardown; only release what is still ours */
        stub_resp_free(sr, sr->iov_cnt - 1, sr->zc_deferred);
        slot->elem = NULL;
    }
    g_free(win->slots);
    g_free(win);
}

/*
 * connection teardown shared by conn_err (iothread) and the accept-handler
 * fail rollback. Runs on the socket iothread; after it returns no worker may
 * touch the vq's state again:
 *   1. dead -> every task still entering for this vq bails out before work
 *   2. broadcast -> a push blocked on a full resp window wakes and exits
 *   3. busy-wait -> both workers have left their current tasks (a dispatch
 *      still in flight when the teardown started sees dead on entry)
 *   4. req queue -> freed here too, so a pile-up left behind when the handle
 *      worker exited (async pushes still pending) cannot leak
 *   5. detach/zc/recv/windows/fd -> released on the now-quiet vq. The resp
 *      window is torn down under push_lock so a device async push racing the
 *      teardown (it publishes under the same lock) is fully ordered.
 */
void stub_teardown_vq(VirtQueue *vq)
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
    /* no handle worker is running now: release the reqs that never reached
     * the device (never-popped ones still own their payload buffers) */
    {
        StubReq *r;
        while ((r = g_queue_pop_head(ctx->req_queue))) {
            stub_req_free(r, !r->handed);
        }
        qatomic_store_release(&ctx->req_count, 0);
    }
    /* handle worker parked: the half-parsed request state is now private */
    stub_recv_state_free(ctx->recv);
    ctx->recv = NULL;
    g_mutex_lock(&ctx->push_lock);
    if (ctx->inflight) {
        stub_win_release(ctx->inflight);
        ctx->inflight = NULL;
    }
    g_mutex_unlock(&ctx->push_lock);
    if (fd >= 0) {
        close(fd);
        ctx->resp_fd = -1;
    }
}

/* -------------- Environments ------------- */

/* called by local_set_remote / remote_set_server (the virtio.c property
 * setters): spin up the two worker pools with the side-specific dispatch
 * policy and worker bh. Setup-time only, single-threaded. */
void start_local_env(void)
{
    worker_pool_init(&send_pool, "v-send", true, local_dispatch, local_worker_bh);
    worker_pool_init(&recv_pool, "v-recv", false, local_dispatch, local_worker_bh);
}

void start_remote_env(void)
{
    worker_pool_init(&send_pool, "s-send", false, stub_dispatch, stub_worker_bh);
    worker_pool_init(&recv_pool, "s-recv", false, stub_dispatch, stub_worker_bh);
}


void remote_vq_ctx_init(RemoteVQueueCtx *ctx, unsigned int vring_num,
                        bool local_side)
{
    vr_config_init();
    g_mutex_init(&ctx->vq_lock);
    g_mutex_init(&ctx->zc_lock);
    g_mutex_init(&ctx->push_lock);
    g_cond_init(&ctx->push_cond);
    g_cond_init(&ctx->inflight_cond);
    ctx->inflight = NULL;
    ctx->req_queue = g_queue_new();
    ctx->in_handle = 0;
    ctx->req_count = 0;
    ctx->dead = 0;
    ctx->handle_busy = 0;
    ctx->send_busy = 0;
    ctx->send_full = false;
    if (vring_num > 0) {
        Inflight *inf = g_new0(Inflight, 1);
        inf->size = vr_inflight_size ? pow2ceil(vr_inflight_size)
                                     : pow2ceil(vring_num);
        inf->mask = inf->size - 1;
        inf->slots = g_new0(InflightSlot, inf->size);
        ctx->inflight = inf;
        if (local_side) {
            /* local-side backpressure binding: the send worker waits on
             * inflight_cond (holding vq_lock) when the M3 window is full.
             * The stub's window keeps full_cond NULL - its push path has
             * its own push_lock/push_cond backpressure. local_side is an
             * explicit parameter because this runs before chenv() on the
             * local side (local_set_remote calls it mid-setup). */
            inf->full_cond = &ctx->inflight_cond;
            inf->full_lock = &ctx->vq_lock;
        }
    }
}

/* vq-level destroy: every ctx free site goes through here. Must run after the
 * vq's workers have stopped: the inflight window is torn down under vq_lock
 * (serialized against a racing local_zc_fd_teardown). On the
 * stub side the in-flight window and the half-parsed recv state are normally
 * already released by the connection teardown (stub_teardown_vq), but a plain
 * virtio teardown with a still-live connection reaches this as the first (and
 * only) release site. */
void remote_vq_ctx_destroy(RemoteVQueueCtx *ctx)
{
    g_mutex_lock(&ctx->vq_lock);
    if (check_env(VIRTIO_LOCAL_ENV) && ctx->inflight) {
        /* local side: slots hold VirtQueueElement* (released via
         * inflight_reset + the zc bufs) */
        Inflight *inf = ctx->inflight;
        inflight_reset(inf);
        g_free(inf->slots);
        g_free(inf);
        ctx->inflight = NULL;
    }
    /* local side: drop the half-parsed resp state and the zc state (both are
     * plain heap objects; the pending zc bufs were drained by
     * local_zc_fd_teardown on conn_err, or are freed by inflight_reset) */
    if (check_env(VIRTIO_LOCAL_ENV)) {
        g_free(ctx->recv);
        ctx->recv = NULL;
        g_free(ctx->zc);
        ctx->zc = NULL;
        /* any short-send resume state: its elems were freed by inflight_reset
         * above, the scratch headers/lens are released here */
        local_send_resume_free(ctx);
    }
    g_mutex_unlock(&ctx->vq_lock);
    /* stub side only (local leaves inflight/recv untouched): the slots hold
     * StubResp* (released with stub_resp_free) and recv holds the half-parsed
     * StubRecvState. check_env() is reliable here because the stub side has set
     * VIRTIO_REMOTE_ENV (remote_set_server) before any teardown. */
    if (check_env(VIRTIO_REMOTE_ENV)) {
        stub_recv_state_free(ctx->recv);
        ctx->recv = NULL;
        if (ctx->inflight) {
            stub_win_release(ctx->inflight);
            ctx->inflight = NULL;
        }
        /* release any queued-but-unpopped requests (a plain virtio teardown
         * with a live connection reaches this without stub_teardown_vq) */
        StubReq *r;
        while ((r = g_queue_pop_head(ctx->req_queue))) {
            stub_req_free(r, !r->handed);
        }
        qatomic_store_release(&ctx->req_count, 0);
    }
    g_queue_free(ctx->req_queue);
    ctx->req_queue = NULL;
    g_mutex_clear(&ctx->push_lock);
    g_cond_clear(&ctx->push_cond);
    g_cond_clear(&ctx->inflight_cond);
    g_mutex_clear(&ctx->zc_lock);
    g_mutex_clear(&ctx->vq_lock);
}