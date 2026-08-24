# crypto SVM 实验结果总结（virtio-remote）

- 日期：2026-08-22
- 实验对象：远程 virtio-crypto SVM（local QEMU + remote stub，TCP 127.0.0.1:5553，`cryptodev-backend-builtin`）
- 依据：`/home/waiai/svm/.trae/documents/2026-08-19-实验矩阵.md`（M1 zc 阈值、M5 buffer 池；crypto 不涉及 M2/M3/M4/Macro/App）
- 负载：`openssl speed -engine afalg -elapsed aes-128-cbc`（16/64/256/1K/8K/16K blocksize），每变体点 3 次
- 前置：guest 卸载 `aesni_intel`（已 rmmod + `/etc/modprobe.d/blacklist-aesni.conf`），使 `cbc(aes)` 解析到 `virtio_crypto_aes_cbc`（priority 150 > aes-lib 100）
- 配置档位（vr.conf，Z/W/I/B 其余固定 W=4/I=32/B=16）：

| 档位 | zc_send_min (Z) | buf_pool (P) | 含义 |
|---|---|---|---|
| 0 | 4K | off | 基准（≥4K 走 zc） |
| 1 | 0 | off | 全 zc |
| 2 | 64K | off | 全 copy（≤16K 响应均 < 64K） |
| 3 | 512K | off | 全 copy |
| 9 | 4K | **on** | M5：stub 响应缓冲池 |

---

## 一、覆盖情况（全部跑完）

| 实验 | 变体 | 次数 | 状态 |
|---|---|---|---|
| M1 KVM（本地后端） | — | 3 | ✅ |
| M1 SVM | 档 0/1/2/3（Z 扫描） | 各 3 | ✅ |
| M5 SVM | 档 0（P=off，复用 M1 档0）/ 档 9（P=on） | 各 3 | ✅ |

结果文件：宿主 `/home/waiai/svm/exp/results/crypto/`（jsonl + 原始日志 + CSV 汇总 + SVG 曲线，见第七节）。
每个 SVM 变体均经 stub 日志确认 dataq 流量（handle_dataq 数十万次），数据真实走远程 stub。

---

## 二、M1 zc 阈值：blocksize-throughput（aes-128-cbc，均值 MB/s）

| variant | 16B | 64B | 256B | 1K | 8K | 16K |
|---|---|---|---|---|---|---|
| **KVM**（本地后端） | 0.59 | 2.40 | 9.56 | 37.86 | 249.75 | 427.61 |
| SVM-档0（Z=4K） | 0.27 | 1.01 | 4.16 | 16.11 | 103.25 | 169.49 |
| SVM-档1（Z=0，全 zc） | 0.26 | 1.04 | 4.14 | 17.30 | 112.22 | 173.21 |
| SVM-档2（Z=64K） | 0.25 | 1.07 | 4.26 | 17.10 | 117.24 | 166.33 |
| SVM-档3（Z=512K，全 copy） | 0.27 | 1.06 | 4.10 | 16.53 | 118.85 | 163.06 |

单次波动参考（MB/s @16K）：KVM 393/465/425；档0 175/170/163；档1 179/162/178；档2 168/164/167；档3 157/169/164。
曲线图：`exp/results/crypto/M1-M1.svg`。

**观察**：
1. **ZC 阈值对 crypto 无可见影响**：档 0/1/2/3 吞吐几乎重合（16B 0.25-0.27，16K 163-173）。crypto 响应即加密结果（≤16K），档1（全 zc）与档3（全 copy）的差异 <6%，落在 3 次运行波动内。小响应下 zc 的页注册/释放开销与 copy 的 memcpy 开销相当。
2. **KVM 明显高于 SVM**：所有 blocksize 下 KVM ≈ SVM × 2.1-2.5。SVM 吞吐约为 KVM 的 40-48%，开销来自请求/响应的 TCP 转发往返 + local/stub 双 worker 池调度。

## 三、M5 buffer 池：P off vs on（均值 MB/s）

| variant | 16B | 64B | 256B | 1K | 8K | 16K |
|---|---|---|---|---|---|---|
| SVM-档0（P=off） | 0.27 | 1.01 | 4.16 | 16.11 | 103.25 | 169.49 |
| SVM-档9（P=on） | 0.27 | 1.10 | 4.27 | 17.15 | 116.58 | 183.79 |

**观察**：P=on 全面略优，16K 最明显（169.5→183.8，**+8.4%**），小请求（16B）无差异（0.27 vs 0.27）。stub 响应缓冲池在小请求下命中收益被网络往返延迟掩盖，块越大（分配/释放量越大）收益越可见。

---

## 四、结论

1. **crypto 设备 SVM 化后的绝对开销**：相对本地后端 KVM，SVM 吞吐约降一半（40-48%），与 blk SVM 的幅度一致（网络转发主导）。
2. **zc 阈值在本实验范围内不影响 crypto 性能**（M1 四档重合），crypto 的 zc 优化价值有限；这与 blk 中 zc 主要影响 ≥1MB 大响应不同，crypto 请求/响应天然很小。
3. **buffer 池是小而稳的正向优化**（M5，16K 时 +8.4%），值得保留（P=on 无副作用）。
4. 全矩阵运行**无崩溃**，SVM 链路在修复中断投递后稳定。

---

## 五、过程问题与修复（影响实验结论的环境因素）

| 问题 | 根因 | 影响 | 处理 |
|---|---|---|---|
| guest 中断丢失，SSH 起不来 | local 的 recv worker 线程在 IOThread 上下文调 `virtio_irq`，走 irqfd 分支但该设备未调 `set_guest_notifiers`，`guest_notifier` eventfd 从未初始化（fd=0）→ 中断写进无效 fd，guest 停在等待 | crypto 远程模式完全不可用（blk 因设备调过 set_guest_notifiers 不受影响） | `virtio_irq` 增加 `event_notifier_get_fd(&vq->guest_notifier) > 0` 检查，无效时回退 `virtio_notify_vector`（msix_notify，线程安全） |
| dataq 请求被吞 | `virtio_crypto_handle_dataq_bh` 中 `if (!vdev->vm_running) return;` 在 remote stub 侧 `vm_running` 恒 0，所有 dataq 请求被丢弃 | stub 侧 0 次 dataq | remote_ctx 存在时内联直接处理（`virtio_crypto_handle_dataq`），跳过 vm_running/BH 延迟 |
| RSA 负载不可用 | guest 内核 AF_ALG 不支持 akcipher（ENOENT），用户态无法驱动 virtio-crypto RSA | 无法用 RSA 做可控负载 | 改用对称负载 cbc(aes)；RSA 仅在开机模块签名时被动触发 |
| AES 不走 virtio | `aesni_intel` 优先级 300 > virtio 150，`cbc(aes)` 解析到 aesni | 加密负载未经过 SVM | guest `modprobe -r aesni_intel` + `blacklist-aesni.conf`，强制走 `virtio_crypto_aes_cbc`；bench 脚本每次启动前复检 |
| 实验脚本 launch 自杀 | runner 里 `pkill -f crypto_bench.sh` 会匹配 remote shell 命令行自身（含 `nohup ... crypto_bench.sh`），把 shell 杀掉，rm/bench 全未执行 | 采集到残留旧日志（KVM 数据误标 SVM） | 去掉 launch 中的 pkill；采集前 `rm -f` 目标日志 |

---

## 六、数据来源与复现清单

| 项 | 路径 |
|---|---|
| 原始 bench 日志（KVM + SVM 档0/1/2/3/9） | `exp/results/crypto/logs/crypto-{kvm,svm}-{0,1,2,3,9}.log` |
| 解析结果 M1（KVM+SVM 档0-3，各 3 次） | `exp/results/crypto/M1.jsonl` |
| 解析结果 M5（档0/档9，各 3 次） | `exp/results/crypto/M5.jsonl` |
| CSV 汇总 | `exp/results/crypto/M1_summary.csv`、`M5_summary.csv` |
| 曲线图（SVG） | `exp/results/crypto/M1-M1.svg`、`M5-M5.svg` |
| guest 侧基准脚本（已保存） | `exp/scripts/crypto_bench.sh`（部署到 guest `/home/wai/`） |
| guest 侧解析/绘图脚本 | `exp/scripts/parse_crypto.py`、`gen_crypto_chart.py` |
| 环境切换/运行编排脚本 | `exp/host_switch_crypto.sh`、`exp/run_crypto_tier.sh` |
| 补充负载工具（未入正式矩阵） | `exp/rsa_sign.c`（AF_ALG akcipher，因内核不支持未采用）、`exp/aes_bench.c`（AF_ALG AES 延迟基准，备用） |
| vr.conf 每档参数 | `local_qemu/hw/virtio-remote/vr.conf`（切换时由 host_switch_crypto.sh 重写） |

复现命令（任一变体）：
```bash
bash exp/host_switch_crypto.sh <tier>        # 0/1/2/3/9；kvm 用 exp/run_crypto_tier.sh kvm 0
bash exp/run_crypto_tier.sh svm <tier>       # 自动切档 + 跑 3 次 + 采集
python3 exp/scripts/parse_crypto.py <log> crypto <M1|M5> <variant> '<env_json>'
```
