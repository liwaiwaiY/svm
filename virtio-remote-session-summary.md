# virtio-remote 开发会话工作总结

> 项目：`/home/waiai/svm/local_qemu`（QEMU，添加 virtio-remote 扩展）
> 核心文件：`hw/virtio-remote/virtio-remote.c`、`hw/virtio/virtio.c`、`hw/block/virtio-blk.c`、`include/hw/virtio-remote/virtio-remote.h`
> 文档日期：2026-08-17（末次静态检查同日，未执行编译/运行）

---

## 1. 背景与目标

QEMU 的 virtio-remote 让一台 **local qemu**（跑客户机，只做 virtio 前端口）通过 TCP socket
（`ip@port`，每个 vq 一条独立连接，支持 MSG_ZEROCOPY）把 virtio 请求转发给 **remote stub**
进程（跑真正的设备后端，如 virtio-blk）。

本会话的目标：审查并修复 virtio-remote 运行阻塞点，编写一个端到端测试模块验证基本功能。

---

## 2. 已完成的工作

### 2.1 代码审查与清理

- 审查了 `virtio.c` / `virtio-remote.h` / `virtio-remote.c` 三个文件的调用情况，确认调用关系正常。
- 变量重命名：`send_writable` → `send_full`（原命名误导：该标志为 true 时 socket 实际**不可写**）。
- 补充了缺失的 stop 函数 `remote_virtio_device_stop_ioeventfd_impl`（local 侧关闭 ioeventfd 的对称操作）。
- 将 `chenv` / `check_env` 从头文件移入 `.c` 文件（消除 `-Wnested-externs` 警告的根源）。
- 修复两个编译错误：`field 'bh' declared as a function`；`sr->iov` 取成员错误。
- 修复 AVR 交叉编译链接错误：`undefined reference to 'pci_bus_irqs'`。
- 还原 `hw/virtio/vhost.c` 为标准 virtio 实现（用户明确：不考虑 vhost 设备，只考虑标准 virtio）。

### 2.2 阻塞 virtio-remote 运行的 3 个前置缺陷（全部解决）

1. **remote-id 属性未注册**：`qdev-monitor` 曾无条件设置 `remote-id` 属性导致启动即失败。
   该设置代码已从 `system/qdev-monitor.c` 删除。
2. **stub 机器名不匹配**：`machine_class_base_init`（`hw/core/machine.c`）把 QOM 类型名去掉
   `-machine` 后缀作为 CLI 机器名（`x-remote-machine` → `x-remote`）。
   修复：`system/vl.c` 的 `qemu_init_remote_stub()` 注入 `"x-remote"`（L2864）。
3. **`bin/remote-stub` 软链缺失**：`main.c` 通过 `strstr(argv[0], "remote-stub")` 判定 stub 模式。
   **处理方式（按用户要求）**：手动创建，不在编译流程中自动化：

   ```bash
   ln -s ../qemu-system-x86_64 /home/waiai/svm/local_qemu/build/bin/remote-stub
   # 现状：build/bin/remote-stub -> ../qemu-system-x86_64
   ```

### 2.3 运行期 Bug 修复（全部通过 gdb 回溯定位）

1. **local QEMU 段错误（旧二进制 remote-id）**：`qstring_from_str(str=NULL)` 崩溃。根因：旧二进制仍含
   remote-id 设置。修复：重新编译。
2. **`unsupported machine type: x-remote-machine`**：见 2.2 缺陷 2。
3. **local QEMU 段错误（`worker_pool_register_vq`）**：`local_register_vq` 在 `send_pool.workers`
   仍为 NULL 时被调用。修复：`hw/virtio/virtio.c` 的 `local_set_remote` 将 `start_local_env()`
   **前移**到 `local_connect_socket` 成功之后、vq 循环之前（L4421-4423）。
4. **stub 崩溃：`Assertion 'bql_locked()' failed`**：stub 的 handle worker 线程（无 BQL）自动启动
   ioeventfd → `memory_region_transaction_commit`。修复：`hw/block/virtio-blk.c` 的
   `virtio_blk_handle_output`（L1043-1064）对 `virtqueue_get_remote_ctx(vq)` 非空的 vq 跳过
   ioeventfd 自动启动。
5. **stub 崩溃：`Assertion 'bounce->magic == BOUNCE_BUFFER_MAGIC'`**：`virtqueue_push` 的远端路由
   guard 用 `!is_mosaic(vq->vdev)`，在 stub 端不成立（stub 设备 `is_mosaic()==true`），push 误入
   标准 used-ring 路径。**已由用户修复**：guard 改为与 `virtqueue_pop` 一致的
   `check_env(VIRTIO_REMOTE_ENV)`（见 3.1）。

### 2.4 测试模块（已通过：`ok /x86_64/virtio-remote/roundtrip`）

- **文件**：`tests/qtest/virtio-remote-test.c`（新增，约 280 行）
- **注册**：`tests/qtest/meson.build` L94，加入 `qtests_i386` 列表：
  `(config_all_devices.has_key('CONFIG_VIRTIO') ? ['virtio-remote-test'] : [])`
- **测试目标**：端到端验证 local qemu（`virtio-blk-pci` + `remote-machine`）与 remote stub
  （`remote-stub` 软链 + `remote-stub` 属性）之间的数据往返。
- **关键实现**：
  - `get_free_port()`：bind + getsockname 获取空闲 TCP 端口。
  - `create_image()` / `read_file_bytes()`：临时镜像创建与读取。
  - `stub_start(port, img_path)`：私有临时目录创建 `remote-stub` 软链，`g_spawn_async` 启动 stub，
    argv 全部 `g_strdup`（避免 double-free）。
  - `stub_wait_ready()`：轮询 `waitpid(WNOHANG)` 等待 stub 存活（不消耗其监听 socket）。
  - `stub_stop()`：SIGTERM + waitpid。
  - `blk_req()`：构造 16B 头 + 512B sector + 1B status 的 virtio-blk 请求，经 vring 提交并等待完成。
  - `test_remote_roundtrip()`：写 sector → 读回校验 → 检查 stub 磁盘收到数据 → 检查 local 磁盘未被触碰。

### 2.5 测试运行结果与调试结论（读回断言失败 → 已定位并修复）

- **运行输出**：`virtio-remote-test.c:250: assertion failed (rdata == wdata)`，随后
  `remote-stub: local qemu closed vq connection`（Bail out 后 local 退出导致，属正常）。
- **调试中确认的事实**：
  1. L248 `blk_req(VIRTIO_BLK_T_IN)` 返回 status==0（读请求在 stub 端成功完成）；
  2. 残留目录 `/tmp/virtio-remote-test-KDJ3T3/stub.img` 开头含 `hello-remote-virtio`
     → **写路径完全正确**（stub 磁盘收到数据）；
  3. `local.img` 全零 → local 磁盘未被触碰（local 侧只做转发）。
- **根本原因（测试 bug，非 virtio-remote 代码 bug）**：
  `blk_req()` 在读请求完成后只 `qtest_readb` 取回**状态字节**，**从未把 guest RAM
  （`req_addr+16`）中的扇区数据 `qtest_memread` 复制回 `data`（即 `rdata`）**。因此
  `rdata` 保持全 0，与 `wdata` 比较必然失败。读回的数据实际已在 guest 内存里，只是测试没取出来。
- **修复**：`blk_req()` 中，`type == VIRTIO_BLK_T_IN` 时在 `qvirtio_wait_used_elem` 之后
  `qtest_memread(qts, req_addr + 16, data, SECTOR_SIZE)` 再返回。
- **静态核对结论**：stub 读路径（`virtio_blk_handle_request` → `blk_aio_preadv` →
  `in_sg[0]`）、stub 响应（`remote_stub_virtqueue_push` → `[vq_nr][elem_index][len]` +
  512B 数据 + 1B 状态，len=513，`resp_iov` 无裁剪）、local 响应回写（stage1 按 `in_sg`
  逐 iov recv，512B→in_sg[0]，1B→in_sg[1]）、seq 匹配（write=0 / read=1）均正确。
  本次测试数据量 < `ZC_SEND_MIN`（4KB），zc 路径未参与，可排除 zc 释放时序问题。

---

## 3. 静态检查结论（2026-08-17，未编译未运行）

### 3.1 远端路由 guard（已修复 ✓）

`hw/virtio/virtio.c` 中所有远端路由判断已统一为 `check_env(VIRTIO_REMOTE_ENV)`，
与 `virtqueue_pop`（L2104）一致：

| 函数 | 行号 | 行为 |
| --- | --- | --- |
| `virtio_queue_empty` | L843 | stub: `remote_virtio_queue_empty(ctx)` |
| `virtqueue_detach_element` | L945 | stub: 直接 return（不 unmap） |
| `virtqueue_flush` | L1270 | stub: 直接 return |
| `virtqueue_push` | L1292 | stub: `remote_stub_virtqueue_push` |
| `virtio_notify` | L2815 | stub: 跳过通知 |
| `virtio_notify_config` | L2832 | stub: 跳过通知 |

`virtqueue_fill` 无需 guard：`virtqueue_push` 已提前 return，fill 仅被标准路径调用。
local 端 `check_env(REMOTE)==false`，行为与修复前一致，无回归。

### 3.2 数据流核对（关键机制均成立）

- **local 端 ioeventfd 启动**：guest 写 `VIRTIO_CONFIG_S_DRIVER_OK` → virtio-pci
  （`virtio_ioport_write` L473）→ `virtio_bus_start_ioeventfd` → `virtio_blk_start_ioeventfd`
  → `virtio_blk_ioeventfd_attach` → `virtio_queue_aio_attach_host_notifier`（mosaic 分支挂
  `local_notifier_distributor`，L4015-4026）。qtest/TCG 下 `virtio_pci_ioeventfd_assign` 用
  `memory_region_add_eventfd` 模拟（L389-397），无需 KVM。
- **local 端 kick 转发**：`local_notifier_distributor`（L1364）→ `worker_pool_dispatch(send_pool)`
  → send worker → TCP → stub。
- **stub 端接收**：recv worker 解析请求 → handle worker → `virtio_queue_call_handle_output`
  → `virtio_blk_handle_output`（跳过 ioeventfd，无 BQL 断言）→ `virtio_blk_handle_vq`
  → `virtqueue_pop`（`remote_stub_virtqueue_pop`）。
- **stub 端内存**：`remote_stub_virtqueue_pop` 构造 elem 时 `in_addr/out_addr` 保持 0，
  `in_sg/out_sg` 引用 stub 本地接收缓冲区（L2280-2317），stub 无需 guest 内存映射。
- **stub 端完成**：块 IO 完成回调在 stub 主循环（`remote_stub_loop` → `main_loop_wait`，runstate.c
  L957）执行 → `virtio_blk_req_complete` → `virtqueue_push` → `remote_stub_virtqueue_push`
  （L2329，构建响应、入 inflight、send worker 发送）。
- **local 端写回**：`local_response_handler`（L1116）收齐后 `virtqueue_push`（L1241/1250）写 guest
  used ring，`qvirtio_wait_used_elem` 轮询完成。
- **属性设置时机**：qdev-monitor（L721-751）realize 后延迟设置 `remote-machine`/`remote-stub`
  （setter 依赖 realize 后的 `vq->vring.num`）。
- **连接竞态**：`local_connect_socket` 用 `connect_with_retry`（L790）重试，配合
  `stub_wait_ready` 可容忍 stub 启动延迟。

### 3.3 小问题与处理结论

1. **`virtio_blk_handle_output` 的 ioeventfd 自动启动（已按方案 B 修复）**
   - **local 端确认无问题**：即使 guest 在 DRIVER_OK 前提前 kick，handle_output 的
     `virtio_device_start_ioeventfd` 会启动 ioeventfd 成功（TCG 下用 memory eventfd 模拟），随后
     `if (!s->ioeventfd_disabled) return;` 直接返回，不会本地处理请求；后续 kick 由
     `local_notifier_distributor`（经 `virtio_queue_aio_attach_host_notifier`，virtio.c L4015）
     转发到 send worker，socket 在 realize 阶段已就绪。
   - **stub 端必须跳过**：stub 无 guest，status/DRIVER_OK 从不写入、`vmstate_change(running)` 不触发，
     且 realize 时 `virtio_device_ioeventfd_enabled()` 为 true（`USE_IOEVENTFD` 默认置位），
     因此 ioeventfd 只会在 handle_output 首次被调用时自动启动——而该调用发生在 recv worker 线程
     （无 BQL），会触发 `memory_region_transaction_commit()`（memory.c L1143-1148）的
     `assert(bql_locked())` 崩溃（即本会话已修复过的崩溃 #5 的路径）。
   - **结论（方案 B，dev-specific）**：在 `virtio_blk_handle_output` 条件中加入
     `check_env(VIRTIO_LOCAL_ENV)`，仅 local 端自动启动 ioeventfd，stub 端直接落入
     `virtio_blk_handle_vq`。与 `virtio.c` 中 `virtqueue_pop`/`virtqueue_push` 的
     `check_env` 风格一致。最终代码：

     ```c
     if (!s->ioeventfd_disabled && !s->ioeventfd_started
         && check_env(VIRTIO_LOCAL_ENV)) {
         /* Some guests kick before setting VIRTIO_CONFIG_S_DRIVER_OK so start
          * ioeventfd here instead of waiting for .set_status().
          *
          * Only on the local side: on the remote stub the vq is driven over the
          * TCP channel and handle_output runs on a worker thread without the
          * BQL, so the auto-start below would hit
          * memory_region_transaction_commit()'s bql_locked() assertion.
          */
         virtio_device_start_ioeventfd(vdev);
         if (!s->ioeventfd_disabled) {
             return;
         }
     }
     ```

   - **备注**：此前的条件笔误 `s->ioeventfd_disabled`（缺 `!`）已修正为 `!s->ioeventfd_disabled`。
   - **bus 层通用防御（virtio-bus.c `virtio_bus_start_ioeventfd`）**：入口对
     `check_env(VIRTIO_REMOTE_ENV)` 直接返回 `-ENOSYS`，作为兜底，覆盖任何未在 dev 层
     做 `check_env` 判断、仍会尝试启动 ioeventfd 的设备（stub 端无 guest kick、handle_output
     无 BQL，ioeventfd 启动必然触发 `bql_locked()` 断言）。设备侧（如
     `virtio_blk_handle_output`）的 `check_env(VIRTIO_LOCAL_ENV)` 仍负责语义正确与
     `ioeventfd_started/disabled` 状态一致性；bus 层防御不触碰 dev 私有标志位，仅拦截启动。
     最终 bus 层代码：

     ```c
     int virtio_bus_start_ioeventfd(VirtioBusState *bus)
     {
         ...
         if (check_env(VIRTIO_REMOTE_ENV)) {
             /* Remote stub: the vq is driven over the TCP channel, there is no
              * guest to kick or to write DRIVER_OK, and handle_output may run on a
              * worker thread without the BQL. ioeventfd setup would hit
              * memory_region_transaction_commit()'s bql_locked() assertion, so
              * refuse to start; devices fall back to userspace vq processing,
              * which is exactly what the stub wants. This is defensive: device
              * side (e.g. virtio_blk_handle_output) already gates the auto-start
              * on check_env(VIRTIO_LOCAL_ENV).
              */
             return -ENOSYS;
         }
         ...
     }
     ```

     同时 `virtio-bus.c` 显式 `#include "hw/virtio-remote/virtio-remote.h"`（位于
     `hw/virtio/virtio-bus.h` 之前，符合 include 排序）。
   - 已否决的方案：A（stub 端在 `remote_set_server` BQL 上下文走一遍 start，占位无用的
     ioeventfd，语义怪异且让所有 stub 设备强制走启动流程）。C 不再单独采用，但其
     "-ENOSYS 兜底"思路已并入上文的 bus 层防御（无法置位 dev 私有标志位的问题由
     设备侧 B 方案的状态一致性补齐）。

2. **测试清理残留**：`stub_stop` 与测试末尾对含文件的 tmpdir 调 `g_rmdir`（非空目录失败被忽略），
   会残留临时目录/软链。建议先 `unlink` 软链、删除镜像文件再 `g_rmdir`。

---

## 4. 编译与测试流程（手动执行）

> 已确认 `build/bin/remote-stub` 软链存在；以下步骤无需自动化软链。

### 4.1 编译

```bash
cd /home/waiai/svm/local_qemu
ninja -C build qemu-system-x86_64
# 如测试目标未构建，补：
ninja -C build tests/qtest/virtio-remote-test
```

### 4.2 单测运行（直接运行测试二进制）

```bash
cd /home/waiai/svm/local_qemu
timeout 120 env QTEST_QEMU_BINARY=./build/qemu-system-x86_64 \
    ./build/tests/qtest/virtio-remote-test
```

说明：
- `qtest_qemu_binary(NULL)` 读取 `QTEST_QEMU_BINARY`；测试自身在临时目录创建独立
  `remote-stub` 软链并 spawn，不依赖 `build/bin/remote-stub`。
- stub 的 stderr 默认继承到测试终端，崩溃信息直接可见（此前 bounce 断言即由此观察到）。
- 通过：测试打印 `OK` 与 `PASS` 汇总；失败：断言信息 + stub 崩溃栈。

### 4.3 meson test 集成运行（推荐，自动设置二进制路径）

```bash
cd /home/waiai/svm/local_qemu/build
meson test virtio-remote-test --print-errorlogs -v
```

### 4.4 预期与排查

- 预期行为：写 sector → 读回一致 → stub 磁盘含数据 → local 磁盘为全零，4 条断言全过。
- 若 stub 侧再出现 `bounce->magic` 断言：确认 `virtqueue_push` guard 为
  `check_env(VIRTIO_REMOTE_ENV)`（3.1 表）。
- 若 local 侧本地处理（local 磁盘被写）：多为 ioeventfd 未走 notifier 路径，按 3.3 项 1 修改。

---

## 5. 关键机制备忘（后续开发参考）

- **stub 模式触发**：`system/main.c` `strstr(argv[0], "remote-stub")` → `qemu_init_remote_stub()`
  （vl.c L2859，注入 `-machine x-remote`）→ `remote_stub_loop()`。
- **QOM 属性注册**：`virtio.c` `virtio_device_class_init`（L4531 附近）注册 `"remote-machine"`
  （local，setter `local_set_remote`）与 `"remote-stub"`（stub，setter `remote_set_server`）。
- **worker 池**：每进程 send_pool/recv_pool 各 4 个 worker，vq 按
  `virtio_get_queue_index(vq) % 4` 哈希；send worker 负责出站，recv worker 负责入站。
- **stub 端 BQL 约束**：`memory_region_transaction_commit` 断言 `bql_locked()`；stub 的 handle
  worker 线程没有 BQL，凡会触发内存事务/ioeventfd 的路径必须绕过。
