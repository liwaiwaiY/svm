# virtio-remote：zc 无锁化 + vq 锁 + send/recv 双 worker + distributor 事件清除

## Summary

对 local qemu 侧 virtio-remote 数据通路做四项改造：

1. **zc/pending 状态无锁化**：用 SPSC 语义的 seq 索引槽位数组（Inflight）替换 `ctx->lock` + `GQueue pending` + zc 的 `GSList st->pending`，send worker（生产者）与 recv worker（消费者）之间零锁交接。
2. **新增 vq 锁**：`RemoteVQueueCtx` 增加 `GMutex vq_lock`，所有 virtqueue 访问（`virtqueue_pop`/`virtqueue_push`/`virtio_notify`）及共享的 `pending_elem` 必须先持有该锁。
3. **线程池拆分**：每个 vq 同时拥有 send worker 与 recv worker（两个独立池，按 `vq_nr % VIRTIO_REMOTE_WORKERS` 哈希），kick 只路由到 send worker，resp 只路由到 recv worker，两者可并发，靠 vq 锁串行化 pop/push。
4. **distributor 事件清除（仅 kick）**：`local_notifier_distributor` 抢占到 send worker 后立即清 eventfd，避免 kick 反复触发 iothread；**resp 侧不做任何清除/预读**（socket 维持 level-triggered 重触发），`local_response_handler` 继续在 recv worker 内直接 recv() 解析。

stub 侧（remote_stub_*）保持单线程每 vq 的既有语义，仅删除对 `ctx->lock` 的引用并适配共享函数拆分。

## 现状分析（已探明的关键事实）

- 数据路径入口：
  - kick：`local_notifier_distributor`（iothread）→ `dispatcher_enqueue(vq, local_worker_output)` → 单 worker 池 `workers[vq_nr % VIRTIO_REMOTE_WORKERS]`。
  - resp：`local_response_distributor`（iothread，resp_fd 的 io_read 槽）→ `dispatcher_enqueue(vq, local_response_handler)` → 同一 worker。
  - 写重试：`local_retry_send`（iothread，io_write 槽）→ `dispatcher_enqueue(vq, local_worker_output)`。
- `RemoteVQueueCtx` 定义于 `include/hw/virtio-remote/virtio-remote.h`，含 `GMutex lock`、`GQueue pending`、`void *recv`、`void *zc`。
- ctx 初始化点：
  - local 侧：`hw/virtio/virtio.c` L4435-4439（`g_mutex_init(&ctx->lock); g_queue_init(&ctx->pending);`），此处 `vq->vring.num` 可用；清理在 L4467-4483。
  - stub 侧：`virtio-remote.c` `remote_accept_handler` L1947-1948 初始化、L1991 清理。
- 共享状态与当前锁的使用：
  - `ctx->pending`（GQueue）：send worker 写（L726-736 zc、L772-777 普通），recv worker 读/删（resp handler、conn_err），全部在 `ctx->lock` 下。local 侧单 worker 时其实同线程，`ctx->lock` 现在处于"防御性存在"状态。
  - zc `st->pending`（GSList）：send worker prepend（`local_send_msg` L728，持锁）、recv worker 遍历/删除（`zc_drain` L243 持锁、resp handler L1118-1126 持锁）。
  - `stub_drain_send` L1424 对 stub 侧 `st->pending` prepend **不加锁**（stub 侧 zc 只有 socket iothread 一个 owner，单线程成立）。
- `zc_complete_one`（L201-231）被 local 与 stub 共用：local 分支 `zp->elem`（push + pending_remove + notify），stub 分支 `zp->elem == NULL`（仅释放 bufs 与 zp）。
- `zc_drain`（L237-291）同样共用：local（resp handler L1059）与 stub（remote_stub_req_handler L1587）各调用一次。
- `zc_fd_teardown`（L294-323）由 local conn_err（L1200）与 stub conn_err（L1754）调用。
- `local_response_handler`（L1051-1219）直接从 socket `recv()` 增量解析（header 12B / data），维护 `LocalRecvState`。
- 线程池：`worker_bh`（L813）、`workflow`（L822，线程入口）、`start_workers`（L836）、`worker_for_vq`（L857）、`dispatcher_enqueue`（L873）。worker 一次一个任务，busy 用 `qatomic_cmpxchg` 抢占，忙时 distributor 返回 false 且**不清事件源**（level-triggered 重触发，iothread 空转）。
- kick 的 eventfd 目前在 worker 任务 `local_worker_output`（L899-905）开头清除（`event_notifier_test_and_clear`）。
- `ctx->pending_elem` 生命周期现状：send worker 停泊（`local_send_msg` L749，EAGAIN）、重试（`local_handle_output` L967-973）；recv worker 的 conn_err 当前释放（L1211-1214，**本方案移除**）。`remote_virtio_device_stop_ioeventfd_impl`（L1028）当前为空，且 local（mosaic）侧根本不经过它——local 侧 ctx 释放点只有 virtio.c L4480（fail 回滚）；stub 侧 ctx 释放点在 virtio-remote.c L1991 与 virtio.c L2669/L4244（local/stub 共用，device unplug/销毁路径）。
- 构建目录：`local_qemu/build/`（launch.json 指向 `build/qemu-system-x86_64` 与 `build/remote-stub`）。当前 /home/waiai/svm 下未见 build 目录。

## 改动方案

### 文件 1：`include/hw/virtio-remote/virtio-remote.h`

**RemoteVQueueCtx 结构**（L33-60）：

```c
typedef struct RemoteVQueueCtx {
    int resp_fd;
    int vq_nr;
    unsigned int elem_index;
    unsigned int out_num;
    unsigned int in_num;
    struct iovec *out_sg;
    struct iovec *in_sg;
    void *elem;
    /* local: VirtQueueElement deferred for a send retry (socket was full) */
    void *pending_elem;
    /* stub: response send queue, drained by the socket iothread */
    StubSendQueue *send_q;
    /* serializes every virtqueue access (pop/push/notify) and pending_elem */
    GMutex vq_lock;
    /* local: lock-free SPSC in-flight window (Inflight), see virtio-remote.c */
    void *inflight;
    /* local: LocalRecvState of the resp stream */
    void *recv;
    /* local/stub: MSG_ZEROCOPY state (ZcFdState) */
    void *zc;
} RemoteVQueueCtx;
```

- 删除 `GMutex lock;` 与 `GQueue pending;`（zc/pending 改为无锁 SPSC，vq 访问改由 `vq_lock` 保护）。
- 更新 L48-55 注释：说明 `vq_lock` 守护 vq 访问，`inflight` 为 send/recv 双 worker 间的 SPSC 槽位数组。
- 更新 L22-31 `VIRTIO_REMOTE_WORKERS` 注释：现在两个池（send 池 + recv 池）各 `VIRTIO_REMOTE_WORKERS` 个线程。
- 新增声明：
  ```c
  void remote_vq_ctx_init(RemoteVQueueCtx *ctx, unsigned int vring_num);
  void remote_vq_ctx_destroy(RemoteVQueueCtx *ctx);
  ```

### 文件 2：`hw/virtio/virtio.c`（local 侧 ctx 初始化；local/stub 共用的 ctx 销毁点）

- L4437-4438：`g_mutex_init(&ctx->lock); g_queue_init(&ctx->pending);` → `remote_vq_ctx_init(ctx, vq->vring.num);`
- L4480：`g_mutex_clear(&ctx->lock);` → `remote_vq_ctx_destroy(ctx);`（放在 `virtqueue_set_remote_ctx(vq, NULL)` 与 `g_free(ctx)` 之前）。
- L2669（`virtio_delete_queue`）与 L4244（`virtio_device_free_virtqueues`）：裸 `g_free(vq->remote_ctx)` → `remote_vq_ctx_destroy(ctx); g_free(ctx);`。这两个点 local/stub 共用，是 unplug/销毁时释放 `pending_elem` 与 Inflight 的收口（否则 `g_mutex_clear` 与 Inflight 内存会漏）。
- 其余不动。

### 文件 3：`hw/virtio-remote/virtio-remote.c`（主体）

#### 3.1 Inflight 无锁槽位数组（替代 pending + local zc 列表）

新增（放在原 `PendingElem`/`pending_lookup` 区域）：

```c
/* Lock-free SPSC in-flight window (local side only).
 * Producer = the vq's send worker, consumer = the vq's recv worker.
 * slots are indexed by seq & mask; size = pow2(vring.num) >= max in-flight
 * (in-flight = popped-but-not-pushed elems <= vring.num). The producer
 * publishes a slot by writing it fully, then qatomic_store_release(&tail).
 * The consumer snapshots [head, tail) once per task with acquire loads,
 * clears slots as responses complete, and advances head past contiguous
 * free slots. The producer reuses slot for seq only when seq - head < size
 * (guaranteed by the vring bound, asserted defensively). */
typedef struct InflightSlot {
    VirtQueueElement *elem;   /* NULL = slot free */
    unsigned int seq;
    bool zc;                  /* sent with MSG_ZEROCOPY */
    void **bufs;              /* zc: kept alive until the completion */
    unsigned int n_bufs;
    uint32_t serial;          /* zc: kernel serial */
    unsigned int push_len;    /* zc: resp len once known */
    bool len_known;
} InflightSlot;

typedef struct Inflight {
    uint32_t size;            /* power of 2 >= vring.num */
    uint32_t mask;
    InflightSlot *slots;
    uint32_t head;            /* consumer-owned, advanced past cleared slots */
    uint32_t tail;            /* producer-owned */
} Inflight;
```

辅助函数：
- `static InflightSlot *inflight_slot(Inflight *inf, unsigned int seq)`：`&inf->slots[seq & inf->mask]`。
- `static VirtQueueElement *inflight_lookup(RemoteVQueueCtx *ctx, unsigned int seq)`：取代 `pending_lookup`，`slot->elem`（校验 `slot->seq == seq`）。
- `static void inflight_publish(RemoteVQueueCtx *ctx, unsigned int seq, VirtQueueElement *elem, bool zc, ...)`：生产者写入槽位并 `qatomic_store_release(&inf->tail, seq + 1)`；写入前 `assert(seq - qatomic_load_acquire(&inf->head) < inf->size)`。**在 vq_lock 内调用**（与 teardown 互斥，见 3.3）。
- `static void inflight_clear(RemoteVQueueCtx *ctx, unsigned int seq)`：消费者清槽（`elem=NULL`、`bufs` 释放置 NULL、`zc=false`），随后推进 head：`while (slots[head & mask].elem == NULL && head < tail_snap) head++;`，`qatomic_store_release(&inf->head, head)`。
- `static void inflight_reset(RemoteVQueueCtx *ctx)`：teardown 用，遍历 [head,tail) 释放 bufs/elem 后重置 head=tail、槽全清。

`remote_vq_ctx_init(ctx, vring_num)`：`g_mutex_init(&ctx->vq_lock)`；若 `vring_num > 0`，分配 `Inflight`（`size = pow2ceil(vring_num)`，用 QEMU 的 `pow2ceil`），`ctx->inflight` 指向之；stub 侧传 0 则不分配。

`remote_vq_ctx_destroy(ctx)`（**vq 级销毁，所有 ctx 释放点统一走这里**）：
- `g_mutex_lock(&ctx->vq_lock)`：若 `ctx->pending_elem` 非空，`g_free` 并置 NULL（**pending_elem 的唯一释放点**，见 3.3/3.4）；若 `ctx->inflight` 非空，`inflight_reset` 后释放数组。
- 解锁后 `g_mutex_clear(&ctx->vq_lock)`。
- 调用点：local 侧 virtio.c L4480（fail 回滚）、virtio.c L2669/L4244（unplug/销毁，local/stub 共用）；stub 侧 virtio-remote.c L1991。前提：销毁前该 vq 的 worker 已停止（L4480 在 realize 期无 worker 运行；L2669/L4244 在设备销毁期，事件源已摘除），vq_lock 与停泊/发布临界区互斥保证无双重释放。

删除 `pending_lookup`/`pending_remove`/`GQueue pending` 相关代码。

#### 3.2 zc 函数拆分（无锁）

- `ZcFdState` 保持 `{ bool enabled; uint32_t serial; GSList *pending; }`；local 侧 `st->pending` 不再使用（local zc 项并入 Inflight 槽），stub 侧维持原用法（stub 单线程，无需锁）。
- `zc_complete_one(ctx, ZcPending *zp)`（共用函数）删除，拆为：
  - `static bool local_zc_complete_one(RemoteVQueueCtx *ctx, InflightSlot *slot)`：释放 `slot->bufs`；若 `!slot->len_known` 返回 false（resp 未到，槽保留）；否则 `g_mutex_lock(&ctx->vq_lock)` → `virtqueue_push` + `virtio_notify` → 解锁 → `inflight_clear(ctx, slot->seq)` → 返回 true。
  - stub 路径内联进 `stub_zc_drain`（仅释放 bufs + `g_free(zp)`）。
- `zc_drain(ctx)` 拆为：
  - `static void local_zc_drain(RemoteVQueueCtx *ctx)`：去掉 `ctx->lock`；快照 `[head, tail)`（acquire 读各一次），遍历窗口，对 `slot->zc` 且 serial 命中 `[first,last]`（含回绕逻辑，逻辑不变）的槽调用 `local_zc_complete_one`。
  - `static void stub_zc_drain(RemoteVQueueCtx *ctx)`：保留原遍历 `st->pending` 的逻辑，去掉 `ctx->lock`，命中即 `g_slist_delete_link` + 释放 bufs + `g_free(zp)`（stub 无 elem、无 pending 队列）。
  - 调用点更新：`local_response_handler` L1059 → `local_zc_drain`；`remote_stub_req_handler` L1587 → `stub_zc_drain`。
- `zc_pending_has_elem(ctx, elem)` 删除；conn_err（L1199）改用 `inflight_slot(ctx->inflight, rs->cur_seq)->zc && slot->elem == rs->cur` 判断。
- `zc_fd_teardown(ctx)` 拆为：
  - `static void local_zc_fd_teardown(RemoteVQueueCtx *ctx)`：`g_mutex_lock(&ctx->vq_lock)` 内 `inflight_reset`，解锁；释放 `ctx->zc` 并置 NULL。（vq_lock 保证与 send worker 的 pop/publish 互斥。）
  - `static void stub_zc_fd_teardown(RemoteVQueueCtx *ctx)`：原逻辑去掉 `ctx->lock`（stub 单线程）。
  - 调用点：L1200 → local 版；L1754 → stub 版。

#### 3.3 vq 锁接入（send 侧）

- `local_worker_output`（L899-905）：**删除** `event_notifier_test_and_clear`（移入 distributor），仅 `local_handle_output(vq, ctx)`。
- `local_handle_output`（L960-989）：
  - `pending_elem` 重试：`g_mutex_lock(&vq_lock)` 取走并置 NULL，解锁；`local_send_msg` 失败再 `g_mutex_lock(&vq_lock)` 停泊回 `ctx->pending_elem`，解锁，return。**重试成功即由 `local_send_msg` 发布进 Inflight（zc/普通路径与其它 elem 完全一致），pending_elem 指针随后归属 Inflight 槽位**。
  - drain 循环改为每 elem 一次 vq_lock：
    ```c
    while (true) {
        g_mutex_lock(&ctx->vq_lock);
        VirtQueueElement *elem = virtqueue_pop(vq, sizeof(VirtQueueElement));
        g_mutex_unlock(&ctx->vq_lock);
        if (!elem) break;
        if (!local_send_msg(vq, elem)) break;
    }
    ```
- `local_send_msg`（L638-779）：
  - sendmsg 相关（msg 构造、zc 尝试、EAGAIN 处理）**不加锁**（放锁外）。
  - zc 发布（原 L726-736）：`g_mutex_lock(&vq_lock)` → `zp->serial = st->serial++; inflight_publish(...)` → 解锁。`ZcPending` 结构被 `InflightSlot` 取代，相关字段直接写槽。
  - 普通 in_num>0 发布（原 L772-777）：`g_mutex_lock(&vq_lock)` → `inflight_publish` → 解锁。
  - in_num==0 立即完成（L765-766）：`g_mutex_lock(&vq_lock)` → `virtqueue_push` + `virtio_notify` → 解锁。
  - EAGAIN 停泊（L749）：`g_mutex_lock(&vq_lock)` → `ctx->pending_elem = elem` → 解锁；writable handler 注册逻辑不变。

#### 3.4 vq 锁接入（recv 侧）

- `local_response_handler`（L1051-1219）改造（无锁部分用 Inflight 槽位替换锁内扫描）：
  - `local_zc_drain(ctx)`（无锁）。
  - header 解析后：`rs->cur = inflight_lookup(ctx, seq)`（无锁）。
  - 完成路径：非 zc → `inflight_clear(ctx, rs->cur_seq)`（无锁）→ `g_mutex_lock(&vq_lock)` → `virtqueue_push` + `virtio_notify` → 解锁 → `g_free(elem)`。zc → 直接查 `inflight_slot(ctx->inflight, rs->cur_seq)`：置 `push_len`/`len_known`；若 `slot->bufs == NULL`（zc 已完成）→ 锁内 push+notify → `inflight_clear` → free elem；否则等待 `local_zc_drain`。
  - conn_err（L1195-1219）：`rs->cur` 是否 zc 用槽位判断；`local_zc_fd_teardown`（锁内 reset）。**删除 L1211-1214 对 `ctx->pending_elem` 的释放**：pending_elem 是 send worker 停泊的 elem，recv worker 不拥有它；连接死亡后它保持停泊（指针稳定、不再被并发触碰），其释放统一收口到 vdev/vq 级 `remote_vq_ctx_destroy`（见 3.1）。

#### 3.5 distributor 事件清除（仅 kick 侧；socket 侧不做清除/预读）

- **不改动 `LocalRecvState`（L1041-1049）**：不引入 recv 缓冲区（无 GByteArray/pos）；`local_response_handler` 维持现状，在 recv worker 任务内直接从 socket `recv()` 增量解析（三处 recv 原样保留）。
- `local_notifier_distributor`（L947-952）：
  ```c
  void local_notifier_distributor(EventNotifier *n)
  {
      VirtQueue *vq = host_notifier_to_vq(n);
      Worker *w = dispatcher_claim(vq, false);   /* send worker */
      if (!w) return;                            /* busy: 不清 eventfd，level-triggered 重触发 */
      event_notifier_test_and_clear(virtqueue_get_host_notifier(vq));
      dispatcher_commit(w, vq, local_worker_output);
  }
  ```
  （先抢占成功再清，保证清掉的 kick 其 vring 项必被 worker 的完整 drain 消费；清后到达的 kick 计数非零会再次触发。）
- `local_response_distributor`（L954-959）——**纯 claim→commit，不做任何 socket 操作**：
  ```c
  void local_response_distributor(void *opaque)
  {
      VirtQueue *vq = opaque;
      Worker *w = dispatcher_claim(vq, true);    /* recv worker */
      if (!w) return;                            /* busy: 不读 socket，可读事件保持 level-triggered 重触发 */
      dispatcher_commit(w, vq, local_response_handler);
  }
  ```
  忙窗口内 socket 保持可读、重触发 iothread（与 kick 忙窗口同性质，属既有可接受行为）；读入与解析全部留在 recv worker 的任务内完成，不做任何 socket 侧清除/预读优化。
- `local_retry_send`（L617-632）：改用 `dispatcher_claim(vq, false)` + `dispatcher_commit(w, vq, local_worker_output)`；失败则保留 writable handler，成功才摘除。

#### 3.6 线程池拆分

- 全局：
  ```c
  static Worker *send_workers;
  static Worker *recv_workers;
  static GMutex workers_lock;   /* 保护两个池与 started */
  static bool workers_started;
  ```
- `start_workers`：分配并启动两个池（各 `VIRTIO_REMOTE_WORKERS`），线程名 "vremote-send"/"vremote-recv"，其余字段与现有一致（AioContext、BH、busy、task）。
- `worker_for_vq` 拆为 `worker_for_vq_send(vq)` / `worker_for_vq_recv(vq)`（`vq_nr % VIRTIO_REMOTE_WORKERS`）。
- dispatcher 拆为两段（解决"claim 成功后才清事件"的顺序）：
  ```c
  static Worker *dispatcher_claim(VirtQueue *vq, bool is_recv); /* NULL=busy/无ctx */
  static void dispatcher_commit(Worker *w, VirtQueue *vq, void (*fn)(void *));
  ```
  `dispatcher_claim`：`ctx` 检查 + `qatomic_cmpxchg(&w->busy, 0, 1)`。`dispatcher_commit`：写 `task.vq/fn` + `qemu_bh_schedule(w->bh)`。
  保留一个 `dispatcher_enqueue(vq, fn, is_recv)` 封装（claim+commit）供 `local_retry_send` 使用（若保留）。
- `worker_bh`、`workflow` 不变。

#### 3.7 stub 侧适配

- `remote_accept_handler` L1947-1948：`g_mutex_init(&ctx->lock); g_queue_init(&ctx->pending);` → `remote_vq_ctx_init(ctx, 0)`（只 init vq_lock，inflight 不分配）。
- L1991：`g_mutex_clear(&ctx->lock);` → `remote_vq_ctx_destroy(ctx);`。
- `remote_stub_req_handler`：`zc_drain` → `stub_zc_drain`；conn_err 处 `zc_fd_teardown` → `stub_zc_fd_teardown`。
- `stub_drain_send` 对 `st->pending` 的 prepend 不变（stub 单线程）。

## 假设与决策

1. **SPSC 成立的前提**：local 侧 `ctx->pending`/zc 的全部写只来自 send worker、全部读/删只来自 recv worker——已核对代码成立（`local_send_msg` 唯一写者；resp handler / `local_zc_drain` / conn_err 唯一读者）。stub 侧 zc 单线程，无需改造。
2. **槽位窗口大小 = pow2ceil(vring.num)**：in-flight（popped 未 pushed）≤ vring.num，故生产者永不溢出；仍保留防御性 `assert(seq - head < size)`。
3. **teardown 与 send worker 并发**：`local_zc_fd_teardown` 在 vq_lock 内 `inflight_reset`，send worker 的 pop/publish 也在 vq_lock 内，互斥成立。连接死亡路径的请求丢弃行为与现状一致（错误路径本就 leak/drop）。
4. **vq_lock 只在 local 侧使用**：stub 侧单线程，不取该锁（取与不取等价，为最小改动不取）。
5. **socket 侧不做 distributor 清除**：resp 路径不引入 recv 缓冲区、不在 iothread 上预读，`local_response_handler` 维持直接在 socket 上 recv() 的现状；只保留 kick 侧 eventfd 清除。
6. **`ctx->lock` 全量删除**：所有原 `g_mutex_lock(&ctx->lock)` 站点（L243/290/300/320/726/736/772/777/1097/1099/1118/1126/1132/1135/1142/1144/1203/1205）逐一替换为无锁 Inflight 或 vq_lock；`workers_lock` 与 stub 的 `send_q->lock` 保留。
7. **distributor 清除的顺序语义**：kick=先 claim 成功再清 eventfd，忙时不操作事件源（level-triggered 重触发）；resp=纯 claim→commit，**不对 socket 做任何清除/预读**。忙窗口的 iothread 空转属于既有可接受行为，未引入额外的 handler 摘挂复杂度。
8. **pending_elem 归属（vdev/vq 级）**：parked elem 由 send worker 持有（vq_lock 下停泊/重试）；重试成功即经 `local_send_msg` 发布进 Inflight（zc/普通，与其它 elem 同路径），失败继续停泊；唯一释放点在 vdev/vq 级 `remote_vq_ctx_destroy`。recv worker / conn_err 一律不触碰该指针——连接死亡后 pending_elem 保持停泊（指针稳定），直至设备销毁统一释放。

## 验证

1. 编译：`cd /home/waiai/svm/local_qemu && ninja -C build`（若 build 目录不存在，先按项目既有方式 configure；目标产物 `build/qemu-system-x86_64` 与 `build/remote-stub`）。修复所有编译错误。
2. 静态检查：`git diff` 复核每个 `ctx->lock` 站点已被替换；确认 `virtqueue_pop/push/virtio_notify` 均处于 vq_lock 临界区。
3. 冒烟：按 `.vscode/launch.json` 配置起 local qemu（`-device virtio-balloon-pci,remote-machine=127.0.0.1@8080`）与 remote-stub（`remote-stub=5553`），确认 guest 内设备初始化、IO 正常，无崩溃/断言。
4. 回归关注点：
   - kick 丢失（eventfd 在 distributor 清除后是否出现卡死——预期：drain 为完整 while(true)，无丢失）。
   - zc 路径（大包 + MSG_ZEROCOPY）完成与 resp 先后两种顺序下的 used-ring push 正确性。
   - 连接断开（conn_err）路径不 double-free。
