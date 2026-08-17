# virtio.c / virtio-remote.h / virtio-remote.c 审查与修复计划

## 1. 概述（Summary）

对 `hw/virtio/virtio.c`、`include/hw/virtio-remote/virtio-remote.h`、`hw/virtio-remote/virtio-remote.c` 三个文件做全面交叉审查，发现：

- **2 处编译错误**（`unlikeli` 拼写、`pool->lock` 引用不存在的成员）→ **当前代码无法编译**
- **2 处链接级问题**（`check_virtio_device_remote` 无定义且 vhost.c 缺少头文件 include、头文件 `env_tag` 多重定义）→ **即使编译通过也无法链接**
- **1 处内存泄漏**（local 侧 `remote_vq_ctx_destroy` 不释放 `ctx->recv`/`ctx->zc`）
- **1 个死字段**（`ctx->elem` 只写不读）+ **1 处过时注释**
- **调用关系核查结论**：virtio.c 中全部 remote 调用点与头文件声明一致；virtio-remote.c 内 42 个 static 函数无死函数；头文件所有导出 API 均有使用者。

本计划按"先修编译/链接阻塞 → 再清理死代码 → 再修泄漏 → 验证"的顺序实施。

## 2. 当前状态分析（Current State Analysis）

### 2.1 调用情况核查结论（确保调用正常）

virtio.c 中共 18 处 remote 相关调用，**全部与头文件签名一致，无参数/返回类型错配**：

| 调用点 | 函数 | 位置 |
|---|---|---|
| `remote_virtio_queue_empty(void*)` | virtio_queue_empty | virtio.c L843-844 |
| `remote_stub_virtqueue_push(vq, elem, len)` | virtqueue_push / virtqueue_fill | virtio.c L1133-1134, L1298-1299 |
| `remote_stub_virtqueue_pop(vq, sz)` | virtqueue_pop | virtio.c L2111 |
| `remote_vq_ctx_destroy(ctx)` | 3 处 teardown | virtio.c L2669, L4237, L4476 |
| `virtqueue_set_remote_ctx` | rollback | virtio.c L4477 |
| `local_register_vq(vq)` | local_set_remote | virtio.c L4440 |
| `register_mosaic` | 属性 setter | virtio.c L4455, L4525 |
| `register_aio_ctx` / `local_search_aio_ctx` | 属性 setter | virtio.c L4455-4456, L4268 |
| `start_local_env` / `start_remote_env` | 属性 setter | virtio.c L4457, L4528 |
| `chenv` | 属性 setter | virtio.c L4456 |
| `virtio_device_start_ioeventfd_impl_local` | start_ioeventfd | virtio.c L4268 |
| `remote_virtio_device_stop_ioeventfd_impl` | stop_ioeventfd | virtio.c L4340 |
| `local_notifier_distributor` | aio attach handler | virtio.c L4029, L4066 |
| `local_response_distributor` | resp_fd handler | virtio.c L4449 |
| `remote_accept` / `remote_accept_handler` | remote_set_server | virtio.c L4508, L4521 |

virtio-remote.h 导出 API：`register_mosaic`/`is_mosaic`/`register_aio_ctx`/`local_search_aio_ctx`/`chenv`/`check_env`/`start_*_env`/`remote_vq_ctx_init`/`remote_vq_ctx_destroy`/`local_connect_*`/`local_notifier_distributor`/`local_response_distributor`/`local_register_vq`/`RemoteAccept`/`remote_accept*`/`stub_distributor`/`remote_virtio_queue_empty`/`remote_stub_virtqueue_*`/`stub_register_vq`/`stub_teardown_vq`/`virtio_device_start_ioeventfd_impl_local`/`remote_virtio_device_stop_ioeventfd_impl` —— **均有调用方**。仅 `check_virtio_device_remote` 声明了但新 .c 无定义（见 2.2 问题 3）。

virtio-remote.c 内 42 个 static 函数全部可达，无死函数。

### 2.2 发现的问题清单

| # | 严重度 | 文件 | 位置 | 问题 |
|---|---|---|---|---|
| A1 | **编译错误** | virtio.c | L4339 | `if (unlikeli(is_mosaic(vdev)))` —— `unlikeli` 是 `unlikely` 的拼写笔误，未定义标识符。全树仅此一处（由 "lock free pool; stub pipeline" 提交引入） |
| A2 | **编译错误** | virtio-remote.c | L633/636/645 | `worker_pool_register_vq` 引用 `pool->lock`，但 `struct WorkerPool`（L567-573）无 `lock` 成员 |
| A3 | **编译+链接错误** | vhost.c + virtio-remote.c | vhost.c L1822 | `check_virtio_device_remote(vdev)` 被调用，但：① vhost.c 未 include `virtio-remote.h`（隐式声明）；② 新 virtio-remote.c 无定义（唯一定义在未参与编译的 virtio-remote-old.c L186） |
| A4 | **链接错误** | virtio-remote.h | L129 | `int env_tag;` 是头文件内的非 extern 全局定义；QEMU 全局 `-fno-common`（meson.build L361），多个 TU 包含该头文件会多重定义 |
| B1 | 死代码 | virtio-remote.h + virtio-remote.c | 头 L44-46；c L2340, L2401 | `ctx->elem` 字段只写不读（两处赋值均无读取方），头文件注释自述"queue-empty 判断用 req_count，不用此字段" |
| B2 | 注释过时 | virtio-remote.h | L160-166 | 注释提到不存在的 `stub_ctx_init_windows`；实际初始化在 `remote_vq_ctx_init` |
| C1 | **内存泄漏** | virtio-remote.c | L2609-2620 | `remote_vq_ctx_destroy` 的 local 分支只释放 inflight 窗口，不释放 `ctx->recv`（LocalRecvState，L1150-1153 分配）与 `ctx->zc`（ZcFdState，L1007-1012/L1640 分配） |

### 2.3 virtio 流程审查结论（功能缺失）

梳理 local 与 stub 两侧完整数据通路后，**主流程没有结构性缺失**：

- **local kick 路径**：guest 踢 → host_notifier → `local_notifier_distributor`（aio attach 时注册为 mosaic 的 io_read，virtio.c L4029/L4066）→ `worker_pool_dispatch(&send_pool,…)` → send worker 持 `vq_lock` pop/sendmsg。
- **local resp 路径**：stub 响应 → `local_response_distributor`（resp_fd 的 io_read，virtio.c L4449）→ recv worker → `local_response_handler` 解析 → 写 in_sg → 推 used ring。
- **stub req 路径**：`remote_accept_handler`（virtio.c L4521 注册）→ `stub_distributor`（L1644 注册为 fd handler）→ handle worker 解析 → `remote_stub_virtqueue_pop` → 设备 handle_output → `remote_stub_virtqueue_push`（L1134/L1299 由 virtqueue_push/fill 转发）→ send worker。
- **ioeventfd**：start（L4268 mosaic 分支）与 stop（L4340，已补 `remote_virtio_device_stop_ioeventfd_impl`）镜像对称，顺序正确。
- **teardown**：`remote_vq_ctx_destroy` 覆盖所有释放点；stub 侧另有 `stub_teardown_vq`（L2557）。

**阻塞 remote-virtio 正常执行的根因是编译/链接问题 A1-A4**（代码根本编译不过）；C1 为长期运行下的泄漏，B1/B2 为清理项。

## 3. 修改方案（Proposed Changes）

### 3.1 `hw/virtio/virtio.c`

- **A1**（L4339）：`if (unlikeli(is_mosaic(vdev)))` → `if (unlikely(is_mosaic(vdev)))`。
- 其余 18 处 remote 调用点核查无误，**不做改动**。

### 3.2 `include/hw/virtio-remote/virtio-remote.h`

- **A4**（L129）：`int env_tag;` → `extern int env_tag;`（定义移到 virtio-remote.c）。
- **B1**（L44-46）：删除 `void *elem;` 字段及其上方注释（"stub: the elem the device currently holds …"整块）。
- **B2**（L160-166）：更新注释——删除 `stub_ctx_init_windows` 引用，改为"stub 侧窗口由 `remote_accept_handler` 在 `remote_vq_ctx_init` 之后另行初始化"。
- **A3**（L314）：保留 `check_virtio_device_remote` 声明（定义见 3.3）。

### 3.3 `hw/virtio-remote/virtio-remote.c`

- **A4**：在文件靠前位置（如 `mosaic` 表附近）增加 `int env_tag;` 定义。
- **A2**（L629-646）：`worker_pool_register_vq` 删除 `g_mutex_lock(&pool->lock)` / `g_mutex_unlock(&pool->lock)` 三处调用。依据：函数注释（L626-628）明确声明"setup 期 append-only、bh 无锁扫描"，且两个调用方（`local_register_vq` ← virtio.c L4440 主线程；`stub_register_vq` ← remote_accept_handler L1642 单线程 accept）均为 setup 期单线程，无并发。修改后函数体为纯无锁链表操作。
- **A3**：实现 `check_virtio_device_remote`（语义严格遵循头文件 L311-313 注释：*true if this process is the remote stub for vdev*）：

```c
bool check_virtio_device_remote(VirtIODevice *vdev)
{
    if (!vdev || is_mosaic(vdev))
        return false;
    for (int n = 0; n < VIRTIO_QUEUE_MAX; n++) {
        if (virtqueue_get_remote_ctx(&vdev->vq[n]))
            return true;
    }
    return false;
}
```

（旧实现依赖已不存在的 `gsi_stubs`/`ids` 哈希，不可移植，需按新架构重新实现。）

- **B1**：删除 L2340 `ctx->elem = (void *)ret;` 与 L2401 `ctx->elem = NULL;` 两处赋值。
- **C1**（L2609-2620）：local 分支在 `vq_lock` 内、释放 inflight 之后补：

```c
g_free(ctx->recv);
ctx->recv = NULL;
g_free(ctx->zc);
ctx->zc = NULL;
```

（与 `local_zc_fd_teardown` L484-485 的释放方式一致；LocalRecvState/ZcFdState 均为纯 `g_new0` 堆对象。）

### 3.4 `hw/virtio/vhost.c`

- **A3**：顶部 include 列表追加 `#include "hw/virtio-remote/virtio-remote.h"`，使 L1822 的 `check_virtio_device_remote` 获得声明（消除隐式声明编译错误）。

### 3.5 不动的部分

- `virtio-remote-old.c/h`：未参与编译（meson.build 仅编译 virtio-remote.c），其中的 `check_virtio_device_remote` 定义在新实现落地后不再有符号缺口，保留作参考，不删除。
- `send_full` 重命名、`remote_virtio_device_stop_ioeventfd_impl` 实现、头文件 L210 声明：上一轮已完成，本轮仅核验。

## 4. 假设与决策（Assumptions & Decisions）

- **A2 采用"删锁"而非"加锁"**：函数注释与调用方时序都证明 setup 期无并发，删锁与既有设计意图一致，且避免为 Pool 引入无用的 GMutex（也免去 init/clear）。
- **A3 语义以头文件注释为准**：`check_virtio_device_remote` 返回"本进程是 vdev 的 remote stub"（有 remote_ctx 且非 mosaic）。vhost_virtqueue_mask 用它在 stub 侧跳过 vhost 屏蔽，把通知让给 remote-virtio 通路。
- **`ctx->elem` 直接删除**：无任何读取方，注释自认不用，删除零风险。
- **env_tag 定义为 `extern`**：QEMU 规范做法，一处定义于 virtio-remote.c。
- **不删除 old.c/h**：不在本次"删除不必要变量/函数"范围内（未编译、无副作用），避免过度清理。

## 5. 验证步骤（Verification）

1. `git diff` 复核：仅改动计划列出的 4 个文件、共 8 处修改点。
2. 编译器级验证：
   - 方案一（若有 build 目录）：`ninja` 增量编译，确认 `hw/virtio-remote/virtio-remote.c.o`、`hw/virtio/virtio.c.o`、`hw/virtio/vhost.c.o` 通过；
   - 方案二：`make -j` 顶层编译验证；
   - 若树内无 build 配置，则用 `GetDiagnostics` 对三个改动文件做静态检查，确认无 `unlikeli`/`pool->lock`/隐式声明类报错。
3. 全局符号核对：`grep -rn "check_virtio_device_remote"` 确认声明（头）+ 定义（virtio-remote.c）+ 调用（vhost.c）三者齐备；`grep -rn "env_tag"` 确认仅一处定义、头文件为 `extern`。
4. 死代码核对：`grep -n "ctx->elem"` 确认零命中。
5. 功能验证（用户侧）：local qemu + remote stub 启动 remote-virtio，跑通读写后正常关停，观察无泄漏告警、ioeventfd 正常 detach。

## 6. 最终交付

修复完成后，向用户输出**审查报告**，包含：
1. 调用核查结论（表 2.1 摘要）；
2. 函数/结构体清理结论（B1/B2、42 个 static 函数均可达、头文件导出 API 全部在用）；
3. 功能缺失与修复清单（A1-A4、C1 逐项说明）；
4. 修改文件与行号汇总。
