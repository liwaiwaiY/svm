# virtio-remote：stub 端流水线化（distributor + handle/send 双线程池 + 双 inflight SPSC）

## Summary

把 remote stub 的串行请求通路（`req_handler → handle_output → push → send` 全在一个 per-vdev socket iothread 上）改为三段流水线：

1. **distributor**（socket iothread，保留）：只做协议解析，把完整请求包装成 per-请求上下文 `StubReq`，发布进 per-vq 的 **req_win**（inflight SPSC 窗口），dispatch 给 handle worker 池。
2. **handle worker 池**（`stub_handle_workers`，`vq_nr % N` 哈希）：消费 req_win，调用设备 `handle_output`；`push` 生成的 resp 发布进 per-vq 的 **resp_win**，dispatch 给 send worker 池。
3. **send worker 池**（`stub_send_workers`，`vq_nr % M` 哈希）：消费 resp_win，执行 `sendmsg`/MSG_ZEROCOPY/EAGAIN 重试。

效果：socket iothread 不再执行设备逻辑，不同 vq 的请求在 handle/send 池上**跨 vq 并行**。每 vq 的双窗口都是严格 SPSC（producer/consumer 各一），无锁。

## 现状分析（已探明）

- **入口**：`remote_accept_handler`（[L2058](file:///home/waiai/svm/local_qemu/hw/virtio-remote/virtio-remote.c#L2058)）把每个 vq 的 socket 用 `aio_set_fd_handler(sctx->aio_ctx, vq_fd, remote_stub_req_handler, ...)`（[L2181-2182](file:///home/waiai/svm/local_qemu/hw/virtio-remote/virtio-remote.c#L2181-L2182)）注册到 **per-vdev 的 sctx->aio_ctx**。所有 vq 的读/写事件都在这一个 iothread 上被串行处理。
- **req_handler**（[L1806](file:///home/waiai/svm/local_qemu/hw/virtio-remote/virtio-remote.c#L1806)）：增量解析 `[vq_nr][seq][out_num][in_num]` header + lens + out 数据（`StubRecvState`），完整请求填进 `ctx` 单槽字段（`elem_index/out_num/in_num/out_sg/in_sg`，[L1953-1957](file:///home/waiai/svm/local_qemu/hw/virtio-remote/virtio-remote.c#L1953-L1957)），然后 `virtqueue_call_handle_output(vq)` 同步执行设备逻辑，结束后清 ctx 字段。
- **virtio.c 适配点**：`virtqueue_pop` 在 stub 进程走 `remote_stub_virtqueue_pop`（[virtio.c L2108-2112](file:///home/waiai/svm/local_qemu/hw/virtio/virtio.c#L2108-L2112)）；`virtqueue_fill` 对 remote vq 走 `remote_stub_virtqueue_push`（[virtio.c L1129-1136](file:///home/waiai/svm/local_qemu/hw/virtio/virtio.c#L1129-L1136)）。设备 `handle_output` 由 req_handler 直接调用，是 stub 侧唯一触发点。
- **pop**（`remote_stub_virtqueue_pop`，[L1477](file:///home/waiai/svm/local_qemu/hw/virtio-remote/virtio-remote.c#L1477)）：从 `ctx` 单槽字段构造 `VirtQueueElement`（out/in_sg 直接引用解析出的缓冲区）。
- **push**（`remote_stub_virtqueue_push`，[L1681](file:///home/waiai/svm/local_qemu/hw/virtio-remote/virtio-remote.c#L1681)）：构造 `StubResp`（header+iov+in_bufs）→ 加锁入 `send_q->resp_q`（GQueue）→ schedule drain BH；真正 `sendmsg` 在 `stub_drain_send`（[L1582](file:///home/waiai/svm/local_qemu/hw/virtio-remote/virtio-remote.c#L1582)）。`send_q` 已支持跨线程入队（设备异步完成时 push 从设备线程调用），但 drain 仍在 iothread。
- **zc**：stub 侧 `ZcFdState::pending`（GSList）当前只在 iothread 上读写（`stub_drain_send` 写、`stub_zc_drain` 在 req_handler [L1814](file:///home/waiai/svm/local_qemu/hw/virtio-remote/virtio-remote.c#L1814) 读），单线程成立。改造后必须归一到 send worker 单属。
- **本地线程池/窗口可复用**：`Worker`/`worker_bh`/`workflow`（[L493-554](file:///home/waiai/svm/local_qemu/hw/virtio-remote/virtio-remote.c#L493-L554)）与 `Inflight`/`InflightSlot`（[L96-115](file:///home/waiai/svm/local_qemu/hw/virtio-remote/virtio-remote.c#L96-L115)）是通用结构；local 侧已有 `inflight_publish/lookup/clear/reset` 全套。stub 侧直接复用。

## 单 vq 内部 elem 能否并行？（分析结论）

**协议层允许**：
- req 流是自描述字节流（每个请求带长度），stub 解析与顺序无关；
- resp 每个自带 `[vq_nr][seq][len][data]`，可乱序发送，local 侧按 `seq` 匹配 inflight 槽（local 的 SPSC 窗口本身就支持乱序完成）；
- 窗口槽按 `seq & mask` 索引，多请求 in-flight 天然支持。

**设备层是限制**：
- `handle_output` 并发安全没有普遍保证：virtio-blk 等用 `blk_aio_*` 异步设备多数能并发，但控制面类设备（共享状态、SCSI 总线、并发 `virtio_notify`/used ring 写入）未必线程安全；
- 同一 vq 并发时，`virtqueue_pop/push` 的 vq 内部状态（`used_idx` 等）需要串行化，且 `remote_stub_virtqueue_pop` 依赖的 ctx 单槽上下文必须 per-elem 化（本次改造本就要求）。

**决策：首版 vq 内串行（跨 vq 并行）**。理由：① 正确性论证简单——每 vq 至多一个 elem 在处理，`ctx->active_*` 单槽无竞争；② 用户核心痛点是跨 vq 串行，首版即解决；③ 架构不阻碍后续放宽：req_win/resp_win 的 seq 槽位 + 乱序消费已兼容 vq 内并行，届时只需把"每 vq 单 active"放宽为"每 vq 多 active"（pop/push 上下文挂到槽上、push 经 elem→槽映射定位），并确认具体设备的并发安全。

## 改造方案

### 1. `include/hw/virtio-remote/virtio-remote.h` — RemoteVQueueCtx 扩展

stub 侧新增字段（local 侧不用）：

```c
/* stub: 请求窗口 distributor->handle worker、回复窗口 handle->send worker */
void *req_win;                /* Inflight，size = pow2ceil(vring.num) */
void *resp_win;               /* Inflight，size = pow2ceil(vring.num) */
/* stub: 当前正在处理的请求（vq 内串行，handle worker 写、pop 读） */
void *active_req;             /* StubReq * */
/* stub: 等待设备 push 完成的条件变量（异步 handle_output） */
GMutex push_lock;
GCond push_cond;
bool push_done;               /* 当前 active elem 是否已 push（qatomic） */
/* stub: 连接已断开，各 worker 任务需尽快退出 */
bool dead;                    /* qatomic */
```

### 2. stub 线程池（virtio-remote.c）

新增两个全局池，复用 `Worker`/`workflow`/`worker_bh`：

```c
static Worker *stub_handle_workers;   /* handle_output 池 */
static Worker *stub_send_workers;     /* resp 发送池 */
static bool stub_workers_started;
static GMutex stub_workers_lock;      /* 复用 workers_lock 亦可 */
```

- `stub_start_workers()`：两池各 `VIRTIO_REMOTE_WORKERS` 线程，`is_send = false`（不触发 local 的 kick_pending 扫描），线程名 `stub-handle`/`stub-send`。在 `remote_accept_handler` 开头懒初始化。
- `stub_worker_for_handle(vq)` / `stub_worker_for_send(vq)`：`virtio_get_queue_index(vq) % VIRTIO_REMOTE_WORKERS`。每 vq 固定映射到单一 handle worker 与单一 send worker，保证双窗口的 SPSC（producer/consumer 各一）。
- 任务分发用现有机制：`w->task.vq = vq; w->task.fn = stub_handle_task; qemu_bh_schedule(w->bh)`。

### 3. per-vq 双窗口（req_win / resp_win）

- 复用 `Inflight`/`InflightSlot` 结构。`InflightSlot` 现有字段（elem/seq/is_zc/zc）已够用：`elem` 字段存 `StubReq *`（req_win）或 `StubResp *`（resp_win）。
- `remote_vq_ctx_init` 增加 stub 分支：`remote_accept_handler` 调用处改传 `vq->vring.num`（[L2174](file:///home/waiai/svm/local_qemu/hw/virtio-remote/virtio-remote.c#L2174) 现传 0），分配 `ctx->req_win` 与 `ctx->resp_win`，各 `pow2ceil(vring.num)` 槽；初始化 push_lock/push_cond。local 分支不变。
- 窗口容量论证：in-flight 请求 ≤ `vring.num`（distributor 在窗口满时停止发布），每个请求至多一个 resp，故 resp_win 深度 = `vring.num` 恰好容纳，无死锁（背压见 §10）。

### 4. distributor（改造 `remote_stub_req_handler`）

`StubRecvState` 解析逻辑保持不变；`virtqueue_call_handle_output` 一步替换为：

```
StubReq *req = g_new0(StubReq, 1);
req->elem_index = rs->seq;
req->out_num = rs->out_num;  req->in_num = rs->in_num;
req->out_sg = rs->out_sg;    req->in_sg = rs->in_sg;
seq = ctx->req_next_seq++;                    /* 本 vq 自增 */
req_win 满则 return（level-triggered 下次再发，天然背压）;
inflight_publish(req_win, seq, req, NULL);
/* rs 的 iovec 数组所有权移交 req；解析状态复位（同现状） */
schedule(stub_worker_for_handle(vq), stub_handle_task);
```

- `ctx->elem_index/out_num/in_num/out_sg/in_sg` 单槽字段**删除**（不再存在 ctx 上），所有权转给 `StubReq`。`remote_stub_virtqueue_pop` 改读 `ctx->active_req`（见 §6）。
- POLLERR 处理：`stub_zc_drain` 不再在 req_handler 直接执行，改为**通知 send worker** 执行（zc 状态单属 send worker）；req_handler 仅 `schedule(stub_worker_for_send(vq), stub_send_task)`，send worker 内先 `stub_zc_drain(ctx)` 再发。

### 5. handle worker（新 `stub_handle_task(VirtQueue *vq)`）

```c
static void stub_handle_task(VirtQueue *vq) {
    ctx = ...; if (ctx->dead) return;
    req = inflight_lookup(ctx->req_win, ctx->req_head_seq);  /* 取 head */
    if (!req) return;                        /* 空：无请求 */
    qatomic_set(&ctx->push_done, 0);
    ctx->active_req = req;
    virtqueue_call_handle_output(vq);        /* 设备处理，内部 pop/push */
    ctx->active_req = NULL;
    /* 设备异步：push 尚未发生则等待（conn_err 也会唤醒） */
    if (!qatomic_load_acquire(&ctx->push_done)) {
        g_mutex_lock(&ctx->push_lock);
        while (!qatomic_load_acquire(&ctx->push_done) && !ctx->dead)
            g_cond_wait(&ctx->push_cond, &ctx->push_lock);
        g_mutex_unlock(&ctx->push_lock);
    }
    inflight_clear(ctx->req_win, ctx->req_head_seq);  /* 推进 req head */
    ctx->req_head_seq++;
    schedule(stub_worker_for_send(vq), stub_send_task);
}
```

要点：
- **vq 内串行由"每 vq 单 handle worker + 完成当前 elem 才进下一个"保证**；`req_head_seq` 由该 worker 独有，无竞争。
- push 完成标志在 push 中置位（§7），用 acquire/release 无锁同步；`g_cond_wait` 仅用于异步设备挂起，避免忙等。
- 同步设备：`handle_output` 返回前 push 已发生，标志已置，不等待。

### 6. `remote_stub_virtqueue_pop` 适配

数据源从 ctx 单槽改为 `ctx->active_req`（handle worker 在处理前设置的当前 StubReq）：

```c
StubReq *req = ctx->active_req;
if (!req || ctx->elem) return NULL;   /* ctx->elem 守卫保留：防止设备重复 pop */
... 用 req->out_num/in_num/out_sg/in_sg 构造 elem，其余不变 ...
```

`virtio.c` 无需改动（仍调 `remote_stub_virtqueue_pop`）。

### 7. `remote_stub_virtqueue_push` 适配

- 构造 `StubResp` 的逻辑不变（header/iov/in_bufs，含 in_sg 截断与超长丢弃）。
- **删除 send_q 入队**，改为发布到 resp_win：
  - 先释放 out buffers（现状逻辑），`sr` 为 NULL 时走原"无 resp 释放"分支；
  - `resp_publish(vq, sr)`：seq = `ctx->resp_next_seq++`；resp_win 满则 `g_cond_wait`（send worker 消费后 signal，见 §8）——形成背压；
  - `inflight_publish(resp_win, seq, sr, NULL)`；
  - `qatomic_store_release(&ctx->push_done, 1)` + `g_cond_signal(&ctx->push_cond)`（唤醒可能等待的 handle worker；同步设备时无等待者，无害）。
- push 可从任意线程调用（设备异步完成回调），但写的是**当前 active elem 的 push_done**（vq 内串行 → 同一时刻只有一个 active elem，无并发写冲突）。

### 8. send worker（新 `stub_send_task(VirtQueue *vq)`）

把 `stub_drain_send`（[L1582](file:///home/waiai/svm/local_qemu/hw/virtio-remote/virtio-remote.c#L1582)）从"GQueue 消费"改为"resp_win 消费"：

```c
static void stub_send_task(VirtQueue *vq) {
    ctx = ...; if (ctx->dead) return;
    stub_zc_drain(ctx);                        /* zc 完成回收 */
    for (;;) {
        sr = inflight_lookup(resp_win, ctx->resp_head_seq);
        if (!sr) break;
        if (sendmsg(fd, ...) == EAGAIN) {
            /* 保持 head，io_write 注册到 send worker 自己的 aio ctx */
            aio_set_fd_handler(send_w->ctx, fd, NULL, stub_send_writable, NULL, NULL, vq);
            return;
        }
        ... zc 挂 st->pending（原逻辑）; stub_resp_free(...);
        inflight_clear(resp_win, ctx->resp_head_seq);
        ctx->resp_head_seq++;
    }
    g_cond_broadcast(&ctx->push_cond);  /* resp_win 腾出空间，唤醒等待发布的 handle worker */
}
```

- EAGAIN 重试的 `io_write` 注册到 **send worker 自己的 aio ctx**（`w->ctx`），writable 时 `stub_send_writable`（新函数，逻辑同 `remote_stub_retry_send`）调度 `stub_send_task`。
- zc 簿记（`ZcFdState::serial/pending`）单属 send worker：`stub_zc_drain`、发送、EAGAIN 重试都在 send worker 线程，与现状单线程语义一致。
- `StubSendQueue` 及 `stub_send_queue_destroy`/drain BH 删除；`send_q` 字段从 ctx 移除（teardown 一并处理）。

### 9. conn_err / teardown（`remote_stub_req_handler` 的 conn_err 路径 + accept fail 回滚）

顺序固定（防止 worker 仍持窗口/队列对象）：

1. `qatomic_set(&ctx->dead, 1)`；
2. `g_mutex_lock(&ctx->push_lock); g_cond_broadcast(&ctx->push_cond); g_mutex_unlock(...)`（唤醒等待 push 的 handle worker）；
3. 从 iothread 摘除 fd handler（现状已有）；
4. `stub_zc_fd_teardown`（send worker 已不活跃——dead 后 send/handle 任务都会快速退出；如担心任务并发，等待该 vq 两个 worker 的当前任务结束，用简单 per-worker busy 轮询或复用现有 busy 标志）；
5. 清 req_win/resp_win：遍历窗口释放未消费的 `StubReq`（含 out_sg/in_sg 缓冲）与 `StubResp`（`stub_resp_free`），`inflight_reset` 后释放两个窗口；释放 push_lock/cond；
6. 关 fd、`remote_vq_ctx_destroy`（现有点）。

### 10. 背压链（无界积压防护）

`socket 满 → send worker EAGAIN（resp_win 停走）→ handle worker 发布 resp 时 g_cond_wait → handle 停 → req_win 满 → distributor 停止发布（req_handler return，level-triggered 挂起）→ socket 读缓冲满 → TCP 背压 → local 停发`。每级由窗口容量（= vring.num）限界，无死锁（send worker 的 writable 事件与 push_cond broadcast 均能唤醒上家）。

## 假设与决策

1. **首版 vq 内串行、跨 vq 并行**：单 vq 并行需要设备 handle_output 并发安全 + per-elem 上下文，作后续扩展（§现状分析"单 vq 并行"章节）。
2. **每 vq 的 req_win/resp_win 是严格 SPSC**：producer/consumer 各自唯一（distributor/单 handle worker/单 send worker，经哈希固定）。设备异步回调只写"已发布的槽位内容 + push_done"，不发布窗口，SPSC 不破坏。
3. **ctx 单槽 `active_req` 无竞争**：vq 内串行保证同一时刻至多一个 elem 在处理；push 经 push_done 与 handle worker 同步。
4. **窗口大小 = pow2ceil(vring.num)**：in-flight 请求与 resp 均 ≤ vring.num，双窗口各自容纳；resp_win 不会在 handle worker 发布时因容量不足而互相死锁（§3、§10 论证）。
5. **zc 状态单属 send worker**：serial/pending/errqueue drain/EAGAIN 全部在 send worker 线程；POLLERR 由 req_handler 转发通知 send worker。
6. **复用 Inflight/Worker 结构**，不新增平行类型；stub 池 `is_send=false` 不受 local kick 语义影响。
7. **删除 `send_q`**，resp 全部经 resp_win；`StubResp` 结构保留（resp_win 的槽内容）。
8. **handle worker 对异步设备会阻塞等待 push**（g_cond_wait）。共享同一 worker 实例的其他 vq 在此期间挂起——可接受（正确性优先）；若观测到吞吐问题，后续按"每 vq 多 active"扩展。
9. **conn_err 丢弃语义与现状一致**：错误路径下未消费的 req/resp 缓冲由 teardown 统一释放（不再泄漏）。

## 验证

1. 编译：`cd /home/waiai/svm/local_qemu && ninja -C build`（产物 `build/qemu-system-x86_64` 与 `build/remote-stub`）。
2. 静态核对：
   - `ctx->elem_index/out_num/in_num/out_sg/in_sg` 引用全部清除或改读 `active_req`；
   - `send_q`/`stub_drain_send_bh`/`stub_send_queue_destroy` 删除后无残留引用；
   - `remote_stub_virtqueue_pop/push` 的 remote 分支在 virtio.c 中的调用链完整。
3. 冒烟：按 `.vscode/launch.json` 起 local qemu + remote-stub，guest 内设备初始化、IO 正常，无崩溃/断言。
4. 回归关注：
   - 同步设备（如 virtio-balloon 的 balloon 处理）路径：push 在 handle_output 返回前完成，无等待、无卡死；
   - 异步设备（virtio-blk 大 IO）：handle worker 挂起 → push 唤醒 → resp 正常发出；多 vq 同时 IO 时验证跨 vq 并行（互不阻塞）；
   - resp 顺序：vq 内串行下 resp 按 seq 顺序发出，local 侧 seq 匹配正确；
   - 连接断开：conn_err 时 handle/send worker 均能退出当前任务，双窗口/缓冲无泄漏、无 double-free；
   - 背压：`ifconfig lo mtu 300` 之类制造 socket 满，验证 EAGAIN 重试 + 整条背压链无死锁。
