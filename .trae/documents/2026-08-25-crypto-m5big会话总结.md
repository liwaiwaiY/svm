# 2026-08-25 Crypto M5big 会话总结（blk Macro 已完成部分 + crypto 大块验证被阻断）

> 会话时间：2026-08-24 23:4x ~ 2026-08-25 00:20
> 目的：完成 crypto M5 大块验证（M5big），把实验结果更新到 crypto 文档。
> 状态：**实验 0 轮结果，被 crypto guest 无法启动阻断**。blk 环境正常，问题已隔离到 crypto。

---

## 1. 用户指令（本会话最新）

- "各2次就行。把实验结果更新到文档。"
  → 运行 `exp/run_crypto_m5big.sh`：P=off（tier0）vs P=on（tier9）× {32K,128K,512K,1M} × 各 2 次
  → 结果（MBps）追加到 `.trae/documents/2026-08-22-crypto-svm实验结果总结.md` 的 M5 buffer pool 小节（L52-57）之后
- 之后用户询问："你在干什么"、"blk是否能正常启动guest？" → 均已答复
- 最后：用户要求总结当前对话到 .trae（本文档）

## 2. 实验脚本（已就绪，未跑通）

- `exp/run_crypto_m5big.sh`：每档先 `host_switch_crypto.sh <tier>` → scp aes_bench.c → gcc 编译 → `modprobe -r aesni_intel`（确保 cbc(aes) 走 virtio_crypto）→ 逐块跑 2 次：
  - bs→iters：32K→2000、128K→500、512K→200、1M→100，threads=1
  - 输出 `exp/results/crypto/aesbench/{off,on}/bs${bs}-r${r}.log`，末尾 awk 汇总 MBps 均值
  - 跑完 `host_switch_crypto.sh 0` 恢复
- `exp/aes_bench.c`：AF_ALG AES-256-CBC，`./aes_bench <bs> <iters> <threads> [warmup]`，输出 `RESULT bs=... MBps=...` 行
- `exp/host_switch_crypto.sh <tier>`：写 vr.conf（tier0: Z=4096 W=4 I=32 B=16 P=0；tier9: P=1），起 stub(5553) + local qemu，等 SSH 60×3s

## 3. 阻断问题：crypto guest 无法正常启动（SSH 起不来）

### 诊断结论（已排除 / 已确认）

1. **blk-only SVM 环境 guest 完全正常**：`bash exp/host_switch.sh a 0` → "guest SSH up after 8 tries"，load 0.04。
   → **问题隔离到 crypto 设备/路径，非 guest 系统盘、非 host 级问题**。
2. **system.raw 的 p2（ext4, /boot 分区）确有文件系统错误**：`e2fsck -fn` 报 "Feature orphan_present is set but orphan file is clean" → 已用 `e2fsck -fy` 修复（loop 挂载后 detach）。但修复后 crypto 仍不行 → 不是根因（blk 本身就能启动，说明系统盘可正常引导）。
3. **crypto 环境下**：
   - stub 监听 5553 正常；local qemu 与 stub 的 **2 条 vq 连接已 ESTABLISH**（qemu fd54/fd55 ↔ stub，控制连接协商完成后关闭，属正常）→ **remote 连接本身没问题**
   - guest 内核已 boot 到 idle：4 个 vCPU 全部 `kvm_vcpu_block`/HLT（QMP `info registers` 显示 HLT=1）
   - 但 **sshd 不应答**：SSH 表现从 "banner exchange 超时" → "Connection reset" → "Connection closed"（guest 的 TCP 能接受连接，但 sshd 不完成握手）
   - 有一次观察到 vCPU0 99.8% 自旋（后一次又回到 idle）——说明 guest 内部不稳定，可能 crypto 驱动在 probe/中断路径上挂住或忙等
   - `guest.log` 0 字节 = 正常（该 guest 无 serial console 输出）
   - stub 0% CPU、qemu 4.8% CPU，无报错日志

### 最可疑根因

**今天（8/24）对共享代码的未提交改动破坏了 crypto 路径**：
- 上次 crypto 实验成功：`exp/results/crypto/logs/crypto-svm-9.log` 时间戳 **8/22 15:17**
- 8/22 至今的未提交 diff：
  - `hw/virtio/virtio-crypto.c`（mtime 8/24 10:29，+11 行）：`virtio_crypto_handle_dataq_bh` 开头新增 —— 若 `virtqueue_get_remote_ctx(vq)` 则 inline 调 `virtio_crypto_handle_dataq(vdev, vq)` 直接处理（stub 侧 SPSC/BH 修复）
  - `hw/virtio-remote/virtio-remote.c`（mtime 8/24 20:22，+32 行）：主要是 error_report 打印
  - `hw/virtio/virtio.c`、`hw/virtio-remote/vr.conf`
- qemu/stub 二进制 build 于 8/24 20:26（含以上改动）
- **下一步优先验证**：`git stash` 或手动回滚这几处后重建，重启 crypto 环境，若 SSH 恢复即确认回归点。

## 4. 已准备的诊断工具/素材（新会话可直接复用）

- `/tmp/qmp_regs.py`：QMP 辅助脚本。用法 `python3 /tmp/qmp_regs.py "info registers"` / `"info cpus"`（本 qemu 不支持 `-c N`）
- `/tmp/kallsyms`：guest 内核 7.0.0-30-generic 的完整符号表（blk 环境 sudo cat /proc/kallsyms 拉取）。**注意有 KASLR 偏移**，跨 boot 不能直接对地址，需先算出 _text 基址偏移（blk 环境 _text=ffffffff8a800000）
- QMP socket：`/tmp/qmp.sock`（qemu 运行时有）
- 关键命令速查：
  - 起 blk：`bash exp/host_switch.sh a 0`（fio.raw 数据盘在 `/home/waiai/svm/exp/remote/fio.raw`；local 侧用 dummy.raw）
  - 起 crypto：`bash exp/host_switch_crypto.sh 0|9`
  - 杀进程：`pkill -f qemu-system-x86_64; pkill -f remote-stub`
  - 查连接：`ss -tnp | grep -E "qemu-system|remote-stub"`
  - vCPU 状态：`ps -L -p $(pgrep -f qemu-system|head -1) -o lwp,wchan:28,pcpu`

## 5. 当前环境状态（会话结束时）

- crypto tier0 环境正在运行但 guest 仍挂（stub pid 3063203、qemu pid 3063205）
- vr.conf = crypto tier0（buf_pool=0, inflight=32, local_batch_n=16, stub_batch_n=16, zc_send_min=4096, workers=4）
- `exp/results/crypto/aesbench/` 目录已建（off/on 子目录），无任何结果

## 6. 下一步（新会话从这继续）

1. **定位 crypto 回归**：diff 8/22 成功版本 vs 当前工作区的 virtio-crypto.c / virtio-remote.c / virtio.c 改动；优先回滚测试（重建 qemu 二进制约需几分钟）
2. crypto 恢复后：`bash exp/run_crypto_m5big.sh`（全自动，~10 分钟），产物在 `exp/results/crypto/aesbench/{off,on}/`
3. 把 MBps 均值表追加到 `2026-08-22-crypto-svm实验结果总结.md` M5 小节后，回答"buf_pool 收益是否随块增大"
4. 备选（未执行）：blk Macro 盘疲劳恢复后的 KVM↔SVM 交替重跑仍待定

## 7. 环境凭据（备忘）

- host sudo：`echo dxeqqghk | sudo -S ...`；guest(wai)：密码 `wai`
- SSH：`DISPLAY=:0 SSH_ASKPASS_REQUIRE=force SSH_ASKPASS=/tmp/askpass.sh ssh -o StrictHostKeyChecking=no -p 2222 wai@127.0.0.1`
- 数据盘：/dev/nvme1n1（盘相位波动大 ±55%，跨时段比较需背靠背锚点）
