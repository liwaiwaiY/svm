# virtio-remote 开发会话工作总结

> 项目：`/home/waiai/svm/local_qemu`（QEMU，添加 virtio-remote 扩展）
> 核心文件：`hw/virtio-remote/virtio-remote.c`、`hw/virtio/virtio.c`、`include/hw/virtio-remote/virtio-remote.h`
> 文档日期：2026-08-17

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
- 修复两个编译错误：
  - `field 'bh' declared as a function`
  - `sr->iov`：`struct msghdr msg = { .msg_iov = sr->iov, ... }` 中的取成员错误。
- 修复 AVR 交叉编译链接错误：`undefined reference to 'pci_bus_irqs'`。
- 还原 `hw/virtio/vhost.c` 为标准 virtio 实现（用户明确：不考虑 vhost 设备，只考虑标准 virtio）。

### 2.2 阻塞 virtio-remote 运行的 3 个前置缺陷（全部解决）

1. **remote-id 属性未注册**：`qdev-monitor` 曾无条件设置 `remote-id` 属性导致启动即失败。
   该设置代码已由用户从 `system/qdev-monitor.c` 删除，源码中已不存在。
2. **stub 机器名不匹配**：QEMU 的 `machine_class_base_init`（`hw/core/machine.c`）会把 QOM 类型名
   去掉 `-machine` 后缀作为 CLI 机器名，即 `x-remote-machine` → `x-remote`。
   修复：`system/vl.c` 的 `qemu_init_remote_stub()` 中注入的机器名由 `"x-remote-machine"` 改为
   `"x-remote"`（L2864），与 `hw/remote/machine.h` 的 `TYPE_REMOTE_MACHINE` 匹配。
3. **`bin/remote-stub` 软链缺失**：`main.c` 通过 `strstr(argv[0], "remote-stub")` 判定 stub 模式，
   需要软链指向被测二进制。
   **处理方式（按用户要求）**：手动创建软链，不在编译流程中自动化。

   ```bash
   ln -s ../qemu-system-x86_64 /home/waiai/svm/local_qemu/build/bin/remote-stub
   # 现状：build/bin/remote-stub -> ../qemu-system-x86_64
   ```

### 2.3 运行期 Bug 修复（全部通过 gdb 回溯定位）

1. **local QEMU 段错误（旧二进制 remote-id）**：`qstring_from_str(str=NULL)` 崩溃。
   根因：所用二进制为旧版本，仍含已删除的 `remote-id` 设置。修复：重新编译。
2. **`unsupported machine type: x-remote-machine`**：见 2.2 缺陷 2，`-machine help` 确认 CLI 名是 `x-remote`。
3. **local QEMU 段错误（`worker_pool_register_vq`）**：`local_register_vq` 在 `send_pool.workers`
   仍为 NULL 时被调用（`worker_pool_register_vq` 解引用 worker 数组）。
   修复：`hw/virtio/virtio.c` 的 `local_set_remote` 将 `start_local_env()` **前移**到
   `local_connect_socket` 成功之后、vq 循环之前（L4421-4423）。
4. **stub 崩溃：`Assertion 'bql_locked()' failed`**（`memory.c`）：stub 的 handle worker 线程
   （无 BQL）调用 `virtio_blk_handle_output` → 自动启动 ioeventfd → `memory_region_transaction_commit`。
   修复：`hw/block/virtio-blk.c` 的 `virtio_blk_handle_output`（L1043-1064）对
   `virtqueue_get_remote_ctx(vq)` 非空的 vq 跳过 ioeventfd 自动启动，直接处理 vq。

### 2.4 测试模块（已编写，尚未跑通）

- **文件**：`tests/qtest/virtio-remote-test.c`（新增，约 280 行）
- **注册**：`tests/qtest/meson.build` L94，加入 `qtests_i386` 列表：
  `(config_all_devices.has_key('CONFIG_VIRTIO') ? ['virtio-remote-test'] : [])`
- **测试目标**：端到端验证 local qemu（`virtio-blk-pci` + `remote-machine`）与 remote stub
  （`remote-stub` 软链 + `remote-stub` 属性）之间的数据往返。
- **关键实现**：
  - `get_free_port()`：bind + getsockname 获取空闲 TCP 端口。
  - `create_image()` / `read_file_bytes()`：临时镜像创建与读取。
  - `stub_start(port, img_path)`：在私有临时目录创建 `remote-stub` 软链，用 `g_spawn_async`
    启动 stub，argv 全部 `g_strdup`（避免 double-free）。
  - `stub_wait_ready()`：轮询 `waitpid(WNOHANG)` 等待 stub 存活（不消耗其监听 socket）。
  - `stub_stop()`：SIGTERM + waitpid。
  - `blk_req()`：构造 16B 头 + 512B sector + 1B status 的 virtio-blk 请求，经 vring 提交并等待完成。
  - `test_remote_roundtrip()`：写 sector → 读回校验 → 检查 stub 磁盘收到数据 → 检查 local 磁盘未被触碰。
- **使用的 libqos API**：`qtest_initf`、`pc_alloc_init`、`qpci_init_pc`、`virtio_pci_init`、
  `qvirtio_start_device`、`qvirtio_set_features`、`qvirtqueue_setup`、`qvirtqueue_kick`、
  `qvirtio_wait_used_elem`。
- **运行方式**：

  ```bash
  ninja -C build qemu-system-x86_64
  timeout 90 env QTEST_QEMU_BINARY=./build/qemu-system-x86_64 \
      QTEST_QEMU_IMG= ./build/tests/qtest/virtio-remote-test
  ```

---

## 3. 遗留问题（未解决）

### 3.1 virtqueue_push / fill / flush / empty 的远端路由 guard 在 stub 端失效

`hw/virtio/virtio.c` 中 5 处远端路由判断仍使用：

```c
if (vq->remote_ctx && !is_mosaic(vq->vdev)) {
    remote_stub_virtqueue_push(...);   // 或 return;
}
```

涉及位置：

| 函数 | 行号 |
| --- | --- |
| `virtio_queue_empty` | L843 |
| `virtqueue_unmap_sg` 附近（远端判定） | L945 |
| `virtqueue_fill` | L1133 |
| `virtqueue_flush` | L1276 |
| `virtqueue_push` | L1298 |

**问题**：stub 端设备调用 `register_mosaic()`（`remote_set_server` 中），`is_mosaic()` 为 **true**，
`!is_mosaic` 为 false → 远端路由不生效，`virtqueue_push` 落入标准 used-ring 路径，导致：
`physmem.c:3799 address_space_unmap: Assertion 'bounce->magic == BOUNCE_BUFFER_MAGIC' failed`，
随后 `qvirtio_wait_used_elem` 超时。

**修复方向**（已定位，未实施）：与 `virtqueue_pop`（L2110）保持一致，改用环境判断：

```c
if (vq->remote_ctx && check_env(VIRTIO_REMOTE_ENV)) {
    remote_stub_virtqueue_push(vq, elem, len);
    return;
}
```

- stub 端：`remote_set_server`（virtio.c L4530）先 `chenv(VIRTIO_REMOTE_ENV)`，
  accept 后再设 `remote_ctx`，因此 push 时 `check_env(REMOTE)` 为 true → 正确走远端。
- local 端：`local_set_remote`（L4461）`chenv(VIRTIO_LOCAL_ENV)`，`check_env(REMOTE)` 为 false，
  行为与现状一致，无回归。
- 同一 guard 模式也需同步修改 `virtio_queue_empty` / `virtqueue_fill` / `virtqueue_flush` / L945 处。
- `is_mosaic()` 在 `virtio.c` L4021/4065/4268/4339 的 ioeventfd/aio 判定中仍被使用，修复后不可删除。

### 3.2 测试尚未跑通

`virtio-remote-test` 因 3.1 的 stub 崩溃未通过；修复 3.1 后需重新编译并运行测试，验证数据往返断言。

---

## 4. 关键机制备忘（后续开发参考）

- **stub 模式触发**：`system/main.c` 用 `strstr(argv[0], "remote-stub")` 判断，走
  `qemu_init_remote_stub()`（vl.c L2859，注入 `-machine x-remote`），再进 `remote_stub_loop()`。
- **QOM 属性注册**：`virtio.c` `virtio_device_class_init`（L4531 附近）注册
  `"remote-machine"`（local，setter `local_set_remote`）与 `"remote-stub"`（stub，setter `remote_set_server`）。
- **qdev-monitor 延迟设属性**：remote 属性在 realize 之后才设置（setter 依赖 realize 后的 `vq->vring.num`）。
- **worker 池**：每进程两个池（send_pool/recv_pool），每池 4 个 worker，
  vq 按 `virtio_get_queue_index(vq) % 4` 哈希到固定 worker。
- **stub 端 BQL 约束**：`memory_region_transaction_commit` 断言 `bql_locked()`；
  stub 的 handle worker 线程没有 BQL，凡会触发内存事务/ioeventfd 的路径必须绕过。
