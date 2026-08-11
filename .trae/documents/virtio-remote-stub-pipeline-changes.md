# virtio-remote Stub 端流水线化改造 —— 代码改动总结

> 本文档记录本次对话对 virtio-remote 的代码改动（实施后总结）。
> 对应计划文档：[virtio-remote-stub-pipeline-refactor.md](./virtio-remote-stub-pipeline-refactor.md)

## 1. 背景与目标

旧 stub 工作流为**串行**：`req_handler`（socket iothread 上解析协议）→ `handle_output`（处理 elem）→ `push`（发送回复）。虽然每 vq 一条 socket，但所有处理都在 per-vdev 的 aio iothread 上串行执行，socket 线程被设备逻辑阻塞，跨 vq 无法并行。

改造目标：
- **distributor**（socket iothread）只做协议解析，通过 **req_win SPSC 窗口** 将 elem 交给线程池中的 handle worker；
- **handle worker** 调用 `handle_output` 处理设备逻辑，通过 **resp_win SPSC 窗口** 将回复交给 send worker；
- **send worker** 负责 `sendmsg`/MSG_ZEROCOPY/EAGAIN 重试，把 socket 线程从设备逻辑中解放出来，实现**跨 vq 并行**。
- 唯一不变量：**elem 的数量守恒**（每个 req 恰好一次 pop、一次 push）。

并行性结论（已定案）：协议层允许乱序，但 virtio 设备 `handle_output` 的并发安全无普遍保证 → **首版 vq 内串行（每 vq 至多一个 active elem）、跨 vq 并行**。

## 2. 改动文件

| 文件 | 改动 |
| --- | --- |
| `local_qemu/hw/virtio-remote/virtio-remote.c` | 主体改动（见下） |
| `local_qemu/include/hw/virtio-remote/virtio-remote.h` | `RemoteVQueueCtx` 字段增删、注释更新 |

## 3. 架构总览（三段流水线）

```
socket iothread (distributor)         handle worker 池                send worker 池
remote_stub_req_handler               stub_handle_task                stub_send_task
  解析协议 -> 收满一个 req    --req_win-->    pop(active_req)          消费 resp_win head
  inflight_publish(req_win)               virtqueue_call_handle_output  sendmsg/zc/EAGAIN
  stub_dispatch(handle)        <--push_cond--  push() -> 发布 resp_win   drain 后 broadcast
                                                push_done=1, signal
```

- 每 vq 映射到**一个 handle worker + 一个 send worker**（`vq_nr % VIRTIO_REMOTE_WORKERS`，两个池各 4 线程），保证每个窗口严格 SPSC。
- handle worker 在每次 push 完成后 dispatch send worker；`stub_worker_bh` 完成后重扫该 worker 注册的所有 vq，**保证无 lost-wakeup**（stub 侧不采用 local 的 busy-claim/kick_pending 机制）。

## 4. 详细改动

### 4.1 Inflight 窗口参数化与 ZcPending 内嵌（第一阶段微重构）

文件：`virtio-remote.c` L96-243

- `ZcPending` 结构体定义上移到 Inflight 区段之前（原 `inflight_publish` 引用后定义的结构体）。
- `InflightSlot` 内嵌 `ZcPending zc;`（一次结构体拷贝替代散参数），并新增 `is_zc` 标志。
- `inflight_publish(Inflight*, seq, void *elem, ZcPending *zc)`：`elem` 由 `VirtQueueElement *` 泛化为 `void *`，同时服务 local 侧（elem）与 stub 侧（StubReq/StubResp），9 参数精简为 4 参数。
- `inflight_lookup` 返回 `void *`；`inflight_clear`/`inflight_reset`/`inflight_free_bufs` 保持窗口语义不变。
- 窗口语义：`head`/`tail` 既是槽位索引又是 **seq 分配器**（单调递增、永不回绕），容量检查用 `tail - head >= size`；槽位映射 `seq & mask`。

### 4.2 头文件 `RemoteVQueueCtx` 字段变更

文件：`virtio-remote.h` L34-85

新增 stub 字段：
```c
void *req_win;            /* distributor -> handle worker 的 SPSC 窗口 */
void *resp_win;           /* handle worker -> send worker 的 SPSC 窗口 */
void *active_req;         /* 正在被处理的 req（vq 内串行，单槽无竞态） */
GMutex push_lock;
GCond  push_cond;         /* push_done 唤醒 + 背压等待共用 */
bool   push_done;         /* 设备 push() 完成标志（qatomic） */
bool   dead;              /* 连接已断（qatomic），worker 必须退出 */
bool   handle_busy;       /* vq 的 handle worker 正在任务内（qatomic） */
bool   send_busy;         /* vq 的 send worker 正在任务内（qatomic） */
bool   send_writable;     /* 该 vq 的 EAGAIN 驻留 handler 是否已注册 */
```

删除：旧的四个 seq 计数器与 `StubSendQueue` 前向声明。
同步更新 `remote_vq_ctx_init` 的注释（stub 侧传 0 仅初始化锁/条件变量，窗口由 accept handler 的 `stub_ctx_init_windows` 分配）。

### 4.3 ctx 初始化/销毁支持 stub 双窗口

文件：`virtio-remote.c` L1107-1170

- 函数前新增前向声明（`typedef struct StubReq/StubResp` + `stub_req_free`/`stub_resp_free`/`stub_win_release`）。
- `remote_vq_ctx_init`：新增 `push_lock`/`push_cond` 及全部 stub 字段初始化；`vring_num > 0` 才分配 local 的 inflight 窗口。
- `remote_vq_ctx_destroy`：local 逻辑后追加 stub 双窗口释放（`stub_win_release`）与锁/条件变量清理。virtio 侧静态拆除（连接仍存活）时，这里也是窗口的兜底释放点。

### 4.4 StubReq / StubResp

文件：`virtio-remote.c` L1522-1597

- 由匿名 `typedef struct {...}` 改为**可前向声明的普通 struct**（`struct StubReq`/`struct StubResp`），供 `remote_vq_ctx_destroy` 前置使用。
- `struct StubReq` 定义上移到 `remote_stub_virtqueue_pop` 之前（编译修复项）。
- 新增 `stub_req_free(StubReq*, bool free_bufs)`：`free_bufs` 仅对未 pop 的 req 释放 payload（pop 后缓冲归属设备，由 push 释放）。

### 4.5 stub 线程池

文件：`virtio-remote.c` L1599-1718

- 双池 `stub_handle_workers`/`stub_send_workers`（各 `VIRTIO_REMOTE_WORKERS`=4 线程），复用 local 的 `Worker`/bh/`workflow` 机制。
- `stub_start_workers`：单次初始化（`stub_workers_started` 守卫）。
- `stub_worker_for_handle/send`：`vq_nr % VIRTIO_REMOTE_WORKERS` 哈希。
- `stub_register_vq`：accept 阶段把 vq 注册到其 handle/send 两个 worker 的 vqs 列表。
- `stub_worker_bh`：执行 `w->task`，清 busy，**重扫 w 注册的所有 vq**，见 req_win 有货则 dispatch handle、resp_win 有货且未驻留则 dispatch send——这是 stub 侧无 lost-wakeup 的机制核心。
- `stub_dispatch`：记录 `task.vq/fn` + `qemu_bh_schedule`（无 busy-claim，靠重扫兜底）。

### 4.6 pop / push 适配

- **pop**（`remote_stub_virtqueue_pop`，L1530）：从 `ctx->active_req` 构造 `VirtQueueElement`（分配 elem + 内嵌 in/out sg 数组），置 `req->handed = true`、`ctx->elem = ret`。`remote_virtio_queue_empty()` 返回 `ctx->elem != NULL` 防重复 pop。
- **push**（`remote_stub_virtqueue_push`，L2034）：构造 `StubResp`（header `[vq_nr][elem_index][len]` + in_sg iov，末段裁剪到 len）→ 释放 out 缓冲、清 `ctx->elem` → **push_lock 内 dead 复查** → resp_win 满则 `g_cond_wait`（背压安全网）→ `inflight_publish(resp_win)` → `push_done = 1` + signal。
  - dead 分支：就地释放 sr/in 缓冲，绝不触碰窗口（与 teardown 用同一把锁互斥）。

### 4.7 handle worker（`stub_handle_task`，L1859）

1. `handle_busy = 1`；dead/无窗口则直接退。
2. 消费 req_win head 的 req，`push_done = 0`、`active_req = req`。
3. `virtqueue_call_handle_output(vq)`——设备同步 pop/push。
4. 异步设备：push 可能在 handle_output 返回后才到 → `g_cond_wait(push_cond)`（含 dead 检查）。
5. 清 req 槽、`stub_req_free(req, !req->handed)`、dispatch send worker。
6. `handle_busy = 0`。

### 4.8 send worker（`stub_send_task`/`stub_send_writable`/`stub_send_detach`，L1914-2023）

- `stub_send_task`：`send_busy=1` → `stub_zc_drain`（zc 状态单属 send worker）→ 循环消费 resp_win head：
  - `len >= ZC_SEND_MIN` 尝试 `MSG_ZEROCOPY`，ENOBUFS/EINVAL 回退普通 send；
  - EAGAIN：置 `send_writable`，在 **send worker 自己的 aio ctx** 注册 `stub_send_writable` 后退出（EAGAIN 驻留，socket 满背压源头）；
  - zc 成功：把 header/iov/in_bufs 挂入 `st->pending` 等内核完成后再释放；
  - 其余：`stub_resp_free` + `inflight_clear`。
  - 窗口清空后 broadcast `push_cond`（解除 push 背压）。
- `stub_send_writable`（io_write handler）：清驻留 → 若未 dead 重新 dispatch `stub_send_task`。
- `stub_send_detach`：拆除该 vq 驻留的 writable handler（teardown 时从 send worker 或 iothread 调用）。

### 4.9 distributor（`remote_stub_req_handler` 改写，L2156）

- **MSG_PEEK 探针**：`recv(fd, &probe, 1, MSG_PEEK)` 返回 EAGAIN 说明是纯 POLLERR（zc 完成事件）→ dispatch send worker 回收 zc 后 return，与有数据事件分离（避免每次数据事件白 dispatch 一次）。
- 协议解析逻辑（`StubRecvState`：header → lens → out data 三阶段）全部保留。
- 收满一个完整请求：构造 `StubReq`（out/in sg 所有权转移）→ dead 则就地释放 → **窗口满则 return**（保留 rs 解析状态等 level-triggered 重发；按 virtio.c `vq->inuse` 守卫论证实际不可达，仅安全网）→ `inflight_publish(req_win, req_win->tail, req, NULL)` → `stub_dispatch(handle worker)` → 复位解析状态、`continue` 读下一个。
- **conn_err**：摘 iothread fd handler → 释放 rs 部分缓冲 → 从 hash 表移除 → `stub_teardown_vq(vq)`（不再直接 `stub_zc_fd_teardown`/close）。

### 4.10 连接拆除（`stub_teardown_vq` + `stub_ctx_init_windows` + `stub_win_release`，L1747-1849）

`stub_teardown_vq` 顺序（conn_err 与 accept fail 回滚共用）：
1. `dead = 1`（此后进入的 task 全部在碰窗口前退出）；
2. push_lock 内 broadcast `push_cond`（唤醒等待 push 的 handle worker / 等待窗口的 push）；
3. busy-wait `handle_busy`/`send_busy` 清零（`g_usleep(1000)`）；
4. `stub_send_detach`；5. `stub_zc_fd_teardown`；
6. push_lock 内 `stub_win_release` 双窗口并置 NULL（与设备异步 push 的发布互斥，完全定序）；
7. `close(fd)`、`resp_fd = -1`。

`stub_ctx_init_windows(ctx, vring_num)`：req_win + resp_win 各 `pow2ceil(vring_num)` 槽。
`stub_win_release(win, is_req)`：遍历 `[head, tail)`，req 按 `handed` 决定是否释放缓冲，resp 释放所有段，然后 `g_free` 窗口。

### 4.11 accept handler 适配（L2431）

- 每个已接受 vq：`remote_vq_ctx_init(ctx, 0)` + `stub_ctx_init_windows(ctx, virtio_queue_get_num(vdev, n))`（**VirtQueue 是不完整类型，不能用 `vq->vring.num`**，编译修复项）→ `zc_enable` → `stub_register_vq(vq)` → `aio_set_fd_handler(req_handler)`。
- 删除旧 `ctx->send_q` 的创建与回滚销毁。
- fail 回滚：`aio_set_fd_handler` 摘除 → `stub_teardown_vq(vq)` → `remote_vq_ctx_destroy(ctx)` → 置 `remote_ctx = NULL` → `g_free(ctx)`（dispatch 在途时看到 NULL ctx 即退出）。

### 4.12 删除项

- `remote_stub_retry_send`（旧 io_write 重试，引用已删除的 `StubSendQueue`/`ctx->send_q`）。
- `ctx->send_q` 创建/销毁、`StubSendQueue` 相关残留（grep 确认零残留）。

## 5. 并发模型与不变量

| 对象 | 所有者/生产者 | 消费者 | 同步 |
| --- | --- | --- | --- |
| `req_win` | distributor（iothread） | handle worker | SPSC，无锁，release/acquire |
| `resp_win` | 设备 push（任意线程） | send worker | SPSC + push_lock（与 teardown 互斥） |
| `active_req` | handle worker | pop（handle worker 线程内） | vq 内串行保证，无锁 |
| `push_done` | push | handle worker | qatomic + push_cond |
| `zc`（stub 侧） | send worker | send worker | 单线程 |
| `dead` | iothread（teardown） | 所有 worker | qatomic |

关键不变量：
1. **每 vq 至多一个 active elem**（`active_req` 单槽 + `ctx->elem` 守卫），elem 数量守恒：收 1 个 req → 恰好 1 次 pop、1 次 push（或 teardown 兜底释放）。
2. **窗口永不实际打满**：local 侧 `virtqueue_pop` 有 `if (vq->inuse >= vq->vring.num)` 守卫（virtio.c L1985），in-flight 请求 ≤ `vring.num - 1`；stub 双窗口各 `pow2ceil(vring.num)` 槽。"窗口满则 return / push 等待" 仅是安全网。
3. **无 lost-wakeup**：stub 不采用 local 的 busy-claim，`stub_worker_bh` 完成后重扫全部注册 vq。

## 6. 背压链

```
socket 满 → send worker EAGAIN 驻留 writable
         → resp_win 不再消费
         → push 中 g_cond_wait（持 push_lock）
         → 设备逻辑停 → handle 停
         → req_win 满
         → distributor 停止发布（保留 rs，level-triggered epoll 重新触发）
         → TCP 反压 local qemu
```

## 7. 编译修复记录

1. `stub_worker_for_handle/send` 在 `stub_worker_bh` 中先于定义使用 → 补前向声明。
2. `rs->lens = rs->out_sg = rs->in_sg = NULL` 链式赋值把 `struct iovec *` 赋给 `int *lens` → 拆为独立赋值。
3. `remote_accept_handler` 用 `vq->vring.num`（VirtQueue 在 virtio.h 仅为前向声明）→ 改用 `virtio_queue_get_num(vdev, n)`。
4. `struct StubReq` 定义在 `remote_stub_virtqueue_pop` 之后 → 上移定义。

构建通过（`ninja -C build`），`qemu-system-x86_64` 链接成功；剩余警告均为既有问题，与本次改动无关。

## 8. 遗留事项

- stub 侧若发生**纯 virtio teardown（连接仍存活）**，`virtio_device_free_virtqueues` 直接调 `remote_vq_ctx_destroy` + g_free（virtio.c L4222-4239），不经过 `stub_teardown_vq`（无 worker 停机）——计划已接受的静态净约束（设备全部销毁时 worker 也会退）。
- 单 vq 内部 elem 并行：未放开。需要时须按设备逐个确认 `handle_output` 可重入性，并把 `active_req` 单槽改为多槽。
