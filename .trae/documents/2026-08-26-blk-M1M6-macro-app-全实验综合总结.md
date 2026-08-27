# blk SVM 全实验综合总结（M1–M6 + Macro + App）

- 综合整理稿，日期：2026-08-26
- 整合来源（按权威度选版）：
  - [2026-08-22-blk-svm实验结果总结.md](./2026-08-22-blk-svm实验结果总结.md)（M2G/M3/M4/M4B/M6/Macro/Macro-B/App）
  - [2026-08-25-blk-m1mem-m5merge实验结果总结.md](./2026-08-25-blk-m1mem-m5merge实验结果总结.md)（M1mem/M5）
  - [2026-08-22-nvmeof-iscsi-m3实验结果总结.md](./2026-08-22-nvmeof-iscsi-m3实验结果总结.md)（NVMe-oF/iSCSI RTT 容忍度早期矩阵）
- **选版原则**：凡 2026-08-24 后重做过的实验（因盘相位 ±55% + page cache 污染问题），一律取重做版为"最合理数据"；8/22 前早期数据（`exp_results/`）与 8/24 前作废版仅作方法学对照，不进入结论。

---

## 0. 方法论铁律（一切结论的前提）

1. **盘相位波动 ±55%**：KINGSTON SNV2S1000G（DRAM-less 入门 NVMe，81% 满）同一配置 randrd 数小时内 119K↔185K（clat 530us↔341us）；写负载还会疲劳盘（KVM randwt 三次 54→41→35K 递减）。
2. **背靠背 + 锚点是唯一可信对比方式**：SVM/KVM/裸盘/多协议对比必须几分钟内交替测，每档前后加 host O_DIRECT 锚点；任何跨天/跨时段数值对比不可信（macro-b 实证：KVM-b 227/315/444 vs 8/24 的 262/371/481，整体低 13%）。
3. **禁 page cache**：一律 O_DIRECT（fio direct=1、virtio-blk `cache=none`、nvmet loop direct-io、LIO block direct-io）+ host 每轮 `drop_caches`。旧 KVM 基线 382K/290K 是 page cache 命中假象（iops_max=459K 超标称 2.3 倍），作废。
4. **fio 口径**：60s 稳态、`time_based`、libaio、1 job（除非注明）；randrw 恒为 70/30、bs=4K。

---

## 1. 实验环境与公共配置

| 项 | 值 |
|---|---|
| SVM | local QEMU + remote stub 双进程，TCP 127.0.0.1:5552 |
| 数据盘 | virtio-blk-pci，vq 数 = `virtio_pci_optimal_num_queues()` = smp.cpus = 4 vq |
| worker pool | vq 按 `vq_nr % workers` 哈希到 worker；inflight_size（I）是窗口，按**请求计数** |
| 档位矩阵 | t0（W=4/I=32/B=16，基准）、t4（W=1）、t5（W=2）、t7/t8（B=1/4，作废） |
| zc | `zc_send_min=1MB` 统一（MSG_ZEROCOPY 仅用于 ≥1MB 响应） |
| RTT 注入 | host lo `tc qdisc netem delay Xus`（单向），RTT = 100 + 2X us；对已建立连接立即生效；**必须在环境全部就绪后注入**（boot/初始化不被 RTT 放大） |

---

## 2. M1：内存开销（2026-08-25，m1mem）——SVM 恒定 +5~10%

### 配置
bs{64K,512K,1M,2M} × {SVM zc=0（全 zc）, SVM zc=4M（4M 以下走 copy）, KVM}；I=256、P=off、**iodepth=1**（避免在途堆积干扰）。口径 = local RSS + stub RSS + skmem。

### 数据（mean kB，total = local+stub+skmem）

| bs | cell | total | vs KVM |
|---|---|---|---|
| 64K | SVM zc=0 | 915721 | **+7.3%** |
| 64K | SVM zc=4M | 942051 | +10.4% |
| 64K | KVM | 853256 | — |
| 512K | SVM zc=0 | 935011 | +6.7% |
| 512K | SVM zc=4M | 954489 | +9.0% |
| 512K | KVM | 875783 | — |
| 1M | SVM zc=0 | 923415 | +5.1% |
| 1M | SVM zc=4M | 946998 | +7.8% |
| 1M | KVM | 878215 | — |
| 2M | SVM zc=0 | 961156 | +8.9% |
| 2M | SVM zc=4M | 954106 | +8.1% |
| 2M | KVM | 882431 | — |

（stub 恒定 34–48MB，skmem KB 级；local RSS 跨 cell 基线波动 ±5%，% 仅作量级参考，同 bs 内 zc/copy 背靠背可比。）

### 结论
1. **iodepth=64 时代的"+14%→+81% 随 bs 涨"是在途堆积假象，作废**；iodepth=1 下 SVM 相对 KVM 恒定 +5~10%，与 bs 无关。
2. 额外内存 = 远端路径在途缓冲 ≈ 在途数 × bs，iodepth 一降即消失。
3. **zc vs copy 结论反转**：iodepth=1 下 zc 的 local 反比 copy 低 2–3%（64K~1M），2M 打平；之前"64K zc 更费内存"是堆积场景页 pin 叠加的产物。
4. 机制：数据必须完整穿过 local→TCP→stub，在途占内存不可避免；zc 只把拷贝从 local 挪到 stub 排队缓冲 + skmem（pin 重叠）。

数据位置：`exp/results/m1mem/`（图 `m1mem_bar_all.png` / `m1mem_pct_kvm.png`）；脚本 `exp/run_m1mem.sh`。

---

## 3. M2：worker pool / inflight 窗口（M2F→M2G 权威）——W=1 唯一瓶颈，唯一真实损失 seqrd -36%

> 演进：M2（numjobs=W 耦合，作废）→ M2F（numjobs=4 固定，2026-08-22）→ **M2G（60s 稳态 + host 锚点 + KVM 禁 cache 复核，2026-08-24，权威）**→ M2FI（窗口模型验证，部分结论被 M2G 修正）。

### M2G（numjobs=4、iodepth=16、60s，仅变 W；KVM 禁 page cache）——平均 IOPS

| 档位 | W | randrd | randwt | seqrd | seqwt |
|---|---|---|---|---|---|
| t4 | 1 | 153.7K | 40.3K | 89.0K | 67.8K |
| t5 | 2 | **199.5K** | 73.5K | 135.9K | **96.6K** |
| t0 | 4 | 175.9K | **94.2K** | **148.3K** | 87.0K |
| KVM 禁 cache（60s×2 区间） | — | 132–199K | 72–96K | **220–233K** | 83–122K |

### M2G 结论（取代 M2F 的 W 效应结论）
- **W=1 是唯一干净瓶颈**：4 负载全部显著低于 W=2/4（4 vq 挤 1 worker 串行化）。
- **W=2 vs W=4 基本持平**（盘相位噪声内）；M2F 时代"W=4 负扩展 -41%/-35%"消失。randwt 是唯一随 W 单调升的负载（40→73→94K）。
- **SVM 相对 KVM 的唯一真实损失 = seqrd -36%**（148.3K vs 220–233K，顺序读对盘相位不敏感）：高 IOPS 顺序读下协议路径每请求开销显著。randrd/randwt/seqwt 均在 KVM 波动范围内，**无净瓶颈**（修正此前"randwt 丢 40–60%"结论，那是盘慢相位/写疲劳数据）。
- 旧 KVM 基线（382K/290K）page cache 污染，全部作废。

### M2FI（W=4 固定，仅变窗口 I）+ 磁盘能力定位

| I | randrd | seqrd |
|---|---|---|
| 32 | 140.4K | 87.3K |
| 64 | 124.5K | **136.4K** |
| 128 | 124.9K | **138.2K** |
| 256 | 111.9K | 133.8K |

- "每 vq 上限 = I/延迟"模型**证伪**：randrd 不随 I 提升；seqrd 在 I=32 被窗口饿着（87K→136K，+56%）；I≥64 收敛于 ~125–138K，实为**盘天花板**（慢相位磁盘能力）。
- 三方同盘态实测（host 裸盘 182–185K / KVM 178K / SVM 160–164K，快相位背靠背）：SVM 读比 KVM/裸盘低 ~10–12%，但主导因素是磁盘。**此"读低 10-12%"已被 M2G 修正为"randrd 无瓶颈，仅 seqrd -36%"**。

数据位置：`exp/results/m2g/`（SVM）+ `exp/results/m2g/kvm_once/`（KVM 禁 cache 复核）；M2FI 在 `exp/results/m2fi/m2fi_svm.jsonl`。

---

## 4. M3：inflight 窗口 × RTT 容忍度——吞吐 = 在途/RTT（Little's law）

### 4.1 M3RTT3：窗口减半 → 全 RTT 域吞吐减半（2026-08-24，m3rtt3/）

负载 randrw 70/30、4K、iodepth=32、1 job。RTT = 100 + 2×netem delay us。

| RTT | SVM-I16 | SVM-I32 | 理论上限 I16/I32 | 效率 I16/I32 | KVM（RTT 无关） |
|---|---|---|---|---|---|
| 0.1ms | 29.2K | 53.5K | 160K/320K | 18%/17% | 69.1K |
| 0.2ms | 24.7K | 47.6K | 80K/160K | 31%/30% | — |
| 0.5ms | 16.3K | 33.9K | 32K/64K | 51%/53% | — |
| 1ms | 9.3K | 23.2K | 16K/32K | 58%/73% | — |
| 2ms | 5.8K | 14.1K | 8K/16K | 72%/88% | 60.3K |

**结论**：
1. 高 RTT 端验证"吞吐 = 在途/RTT"：2ms 时对理论上限效率 72%/88%，RTT 越高效越逼近 100%（Little's law 成立）。
2. **窗口减半 → 全 RTT 域吞吐减半**（I16/I32 ≈ 0.40–0.54），无论 RTT 高低窗口都是第一瓶颈：低 RTT 端限制协议并发，高 RTT 端限制在途。**SVM 吞吐由 inflight 窗口而非 guest iodepth 决定**（iodepth=32 但窗口 16 时在途封顶 16）。
3. SVM-I32 @0.1ms 53.5K vs KVM 69.1K → 混合负载 SVM ~23% 开销（介于 randrd 无瓶颈与 seqrd -36% 之间）；2ms 下 14.1K vs 60.3K——远程存储高 RTT 下因 TCP 在途受限崩塌，本地 KVM 无此问题。

### 4.2 M3 三协议 RTT 对比（2026-08-24，m3net2/，权威版）

三协议统一 **O_DIRECT 直读同一物理盘 raw 文件**（消除 page cache）：SVM `virtio-blk cache=none → fio.raw`；NVMe-oF `nvmet → loop(direct-io) → fio.raw`；iSCSI `LIO block → loop(direct-io) → data_iscsi.raw`。同为 randrw 70/30、4K、1 job、iodepth=32、60s。

| RTT | SVM-I32 | NVMe-oF | iSCSI | KVM | 理论（在途=32） |
|---|---|---|---|---|---|
| 0.1ms | 53.5K | 48.2K | **61.4K** | 69.1K | 320K |
| 0.2ms | 47.6K | 50.2K | 57.4K | — | 160K |
| 0.5ms | 33.9K | 37.4K | 40.3K | — | 64K |
| 1ms | 23.2K | 29.7K | 26.8K | — | 32K |
| 2ms | 14.1K | 15.1K | 13.8K | 60.3K | 16K |

**结论**：
1. **高 RTT 端三协议收敛到同一"吞吐 = 在途/RTT"曲线**（2ms 效率 88%/94%/86%）——高 RTT 下瓶颈是 TCP 在途，与转发路径（emulator 用户态 or host 内核态）无关，所有远程协议等效崩塌。
2. **低 RTT 端每请求开销排序**：iSCSI (61.4K) ≈ KVM (69.1K) 最优（达 KVM 89%），SVM (53.5K) 与 NVMe-oF (48.2K) 相当——SVM 的 emulator 用户态转发相对 nvmet 内核转发**无额外劣势**。
3. RTT 20 倍（0.1→2ms）吞吐跌 SVM 3.8× / NVMe-oF 3.2× / iSCSI 4.4×，全被"在途/RTT"封顶；低 RTT 端三协议差 ≈1.2–1.4×。

### 4.3 早期 NVMe-oF/iSCSI 矩阵（2026-08-22，network/，iSCSI 用 fileio 非直读物理盘，仅趋势参考）

| 协议 | RTT | read IOPS | write IOPS | read p99 |
|---|---|---|---|---|
| NVMe-oF | 0 | 199.5K | 85.5K | 247us |
| | 150us | 88.5K（**-56%**） | 37.9K | 466us |
| iSCSI | 0 | 37.0K | 15.9K | 2597us |
| | 150us | 34.4K（**-7%**） | 14.8K | 2433us |

**结论**：**RTT 容忍度由基线每请求延迟决定**——低延迟协议（NVMe-oF）对 RTT 极敏感（-56% @150us），高开销协议（iSCSI）不敏感（-7%，本就 ~2.4ms 基线被协议/后端开销锁死）。曲线 `exp/results/network/M3-net-RTT-iops.svg`。

数据位置：`exp/results/m3rtt3/`、`exp/results/m3net2/{nvmeof,iscsi}/`、`exp/results/network/`。

---

## 5. M4 + M4B：batch（send 侧）——无净增益 / 二阶优化

### 5.1 M4 重做（2026-08-24，m4new/，I=256、batch_m=512K 消除窗口/字节截断）

> 旧 t7/t8 作废原因：I=32 下 iodepth≥32 在途被窗口封顶；batch_m=64K 只装 ~15 个 4K 请求，batch_n 被字节上限截断。本次 I=256（>最大 iodepth 128）、batch_m=512K，batch_n 为唯一触发条件。

randrw 70/30、4K、1 job、60s；各点 2 次均值（两轮波动 ±40% 为盘相位噪声，B 间差 <20% 无显著性）：

| iodepth | B=1 | B=16 | B=32 | KVM |
|---|---|---|---|---|
| 1 | 6.7K | 7.3K | 7.6K | 8.9K |
| 16 | 55.7K | 58.2K | 49.7K | 45.8K |
| 32 | **65.6K** | 56.7K | 54.2K | 58.4K |
| 64 | **70.6K** | 60.7K | 62.7K | 69.3K |
| 128 | 63.0K | 63.4K | 63.7K | 52.0K |

**结论**：
1. **batch（1/16/32）无净增益**：同 iodepth 下 B 间差异全部落入盘相位噪声，无单调趋势；同 iodepth 下 clat 差 <15%。
2. **机制**：batch 只合并 sendmsg 次数，**不改变在途请求数**；吞吐恒由在途/RTT（Little's law）决定。loopback 上 sendmsg 减少 16–32× 不构成瓶颈（每请求总延迟由 RTT ~100us + 盘延迟 + 排队主导）。
3. SVM vs KVM：d=1 时 KVM 领先 ~20%（单请求路径多一段 TCP hop + emulator 转发）；d≥16 盘相位噪声内持平。

### 5.2 M4B：local send batch 隔离（null_blk，2026-08-24，m4local/）

零盘延迟/零相位环境，只隔离 `local_batch_n`（L）。S=64、I=512、batch_m=512K、W=4；fio randread 4K、1 job。**指标 1/IOPS（µs）**。

| iodepth | L=1 | L=16 | L=32 | clat L1/L16/L32 |
|---|---|---|---|---|
| 1 | 42.5us | 45.9us | 42.3us | 40/43/40us |
| 16 | 8.38us | 8.23us | 8.32us | 132/129/131us |
| 32 | 8.17us | 8.11us | 8.21us | 259/257/260us |
| 64 | 8.13us | 7.52us | 7.60us | 518/479/484us |
| 128 | 8.06us | 7.48us | 7.38us | 1030/954/942us |

**结论**：
- **d=1 三档重合（42–46us）** = loopback RTT 串行基线，batch 无并发点时完全无效（验证口径）。
- d≤32 三档无差异（τ≈8.1–8.4us）；**饱和（d≥64）时 L≥16 把 τ 从 8.06–8.13us 压到 7.38–7.52us（-8%）**，L=16 即饱和。
- **local send batch 是二阶优化**：sendmsg syscall 1–2us 只占 ~140us 全路径的 1%，仅在纯协议瓶颈（nullb 饱和）时露出 ~0.6us 差异。

数据位置：`exp/results/m4new/`、`exp/results/m4local/`。

---

## 6. M5：stub in_sg merge 大块（2026-08-25，m5/）——merge+P 在大块上有收益，带宽占比随 bs 缩

### 配置与新 knob
- 新 knob `stub_merge_m`（M，字节阈值）：>0 且请求 in 总字节 ≤ 阈值时，把单请求内多个 4K in_sg 合并为单个连续大缓冲（in_num=1），设备单次大 aio、响应单 iov；**wire 协议不变（响应只有 data_len），local 端零改动**。代码：virtio-remote.c L489/527/577/3843–3882。
- 矩阵：bs{16K,64K,256K,1M,2M} × {SVM P=off, SVM P=on, KVM}；SVM I=256、**iodepth=1（同步）**、Z=0（全 zc）、M=4M、W=4、B=16。

### 数据（带宽 KiB/s，randrw 7:3 iodepth=1）

| bs | SVM P=off | SVM P=on | KVM | SVM(P=on)/KVM |
|---|---|---|---|---|
| 16K | 328835 | 321272 | 808520 | 39.7% |
| 64K | 886818 | 855455 | 2707041 | 31.6% |
| 256K | 1486048 | 1656444 | 9394375 | 17.6% |
| 1M | 2023118 | **2726309** | 16317408 | 16.7% |
| 2M | 2503597 | **3051009** | 23147499 | 13.2% |

每请求延迟（1/IOPS，µs）：

| bs | SVM P=off | SVM P=on | KVM |
|---|---|---|---|
| 16K | 48.7 | 49.8 | 19.8 |
| 64K | 72.2 | 74.8 | 23.6 |
| 256K | 172.3 | 154.5 | 27.3 |
| 1M | 506.1 | 375.6 | 62.8 |
| 2M | 818.0 | 671.3 | 88.5 |

### 结论
1. **SVM 相对 KVM 带宽占比随 bs 缩（40%→13%）**：bs 16K→2M（128×），KVM 单请求延迟只涨 4.5×（19.8→88.5µs），SVM P=off 涨 16.8×（48.7→818µs）、P=on 涨 13.5×。远程路径每字节成本（local→TCP→stub 拷贝/传输/aio）在大块上线性放大，**即使 iodepth=1 同步模式也躲不掉**。
2. **P=on（buf_pool）大块收益明显**：1M +35%、2M +22%、256K +11%；≤64K 噪声级。机制：merge 后单请求=连续大缓冲，P=off 时每请求 posix_memalign+free 大块（堆抖动），P=on 按页数回收复用。
3. **已知缺口**：矩阵无 M=0（不合并）对照，merge 本身收益未单独量化（P 的收益叠加在 merge 之上）。

数据位置：`exp/results/m5/<tag>.bs.json`（**同目录 q1/q16/q32/q64 是 M6 数据，勿混淆**）。

---

## 7. M6：stub recv batch（2026-08-24，m5/ q1-q64）——决定性因素，10× 跳变

### 配置
新 knob `vr.conf: stub_queue_max`（Q）= req_queue 上限 = 设备 aio 并发上限；**Q=1 即"stub 无批处理"**。I=64（基准）、L=S=32（send 全批排除干扰）、batch_m=512K、W=4。fio randread 4K、1 job、后端 fio.raw cache=none（O_DIRECT），每档前后 host 锚点。

| iodepth | Q=1 | Q=16 | Q=32 | Q=64 (I=64) | Q=64 (I=128) ①/② | host 锚点 |
|---|---|---|---|---|---|---|
| 1 | 5.5K | 5.5K | 5.5K | 5.5K | — | 54/95K |
| 16 | 7.0K | **55.6K** | 55.4K | 55.7K | — | 95/94K |
| 32 | 7.1K | 66.7K | **77.0K** | **77.2K** | — | 64/93K |
| 64 | 7.2K | 71.2K | 66.8K | 65.9K | **92.2K / 79.6K** | 94/94K |

### 结论
1. **Q=1（无 recv batch）= 7.2K 封顶**：stub 同一时刻仅 1 个 aio 在途（clat@d=64=8.9ms 全串行）→ 吞吐被盘串行延迟锁死（1/140us）。**recv batch 决定设备 aio 并发度，是 SVM 能否利用盘并行的开关**。
2. **Q≥16 解锁盘并行 → 55–77K，Q=1→Q=16 是 10× 跳变**；Q16/Q32/Q64 差异在盘相位噪声内，q16 已基本吃满收益。d=1 四档全同（178–180us 串行基线）——与 M4B 的 d=1 重合互为验证。
3. **窗口 slack 是第二独立瓶颈**：同 Q=64、同 iodepth=64，仅把窗口 64→128（加 64 空位当预缓冲），吞吐 **65.9K→92.2K（+40%）**——d=64 塌陷是窗口零 slack（I=64 恰好打满、RTT 全暴露）而非盘。
4. **完整模型（Q/I/iodepth 三旋钮）**：盘深度 D=min(在途,Q)、协议在途 N=min(在途,I)、slack=I−N，有效吞吐 ≈ min(盘(D), 协议(N,slack))。**Q 决定盘并发（Q=1 盘饿死 7K），I 决定隐藏 RTT 的排队冗余（I=Q 零 slack 露 RTT 66K，I≥2Q 满 slack 92K）**，guest iodepth 是需求端。三环最弱者即瓶颈。
5. **send batch vs recv batch 量级**：local send batch（M4B）= -8% 二阶优化；stub recv batch（M6）= **10× 决定性因素**。

数据位置：`exp/results/m5/q1/`、`q16/`、`q32/`、`q64/`、`q64i128/`。

---

## 8. Macro：sysbench TPCC（数据盘 O_DIRECT 重跑 + 四协议同日锚点）

> **8/24 前 Macro 数据全部作废**：MySQL datadir 原在 guest 系统盘 vda，vdb 数据盘从未挂载——之前 SVM/KVM Macro 全压系统盘（本地 virtio-blk），remote stub 完全未参与。重跑条件：datadir 迁 vdb（/mnt/data/mysql，ext4+fstab+AppArmor）、全 O_DIRECT、每轮 drop_caches、`tpcc_run.lua` 修复 delivery `o_c_id=nil` 崩溃、档间优雅关机（先停 MySQL 再 poweroff，避免 InnoDB crash recovery）。

### 8.1 KVM vs SVM（2026-08-24 重跑，macrokvm/ macrosvm/）

| 档位 | threads | txn（3 次） | 相对 KVM | avg / p95 |
|---|---|---|---|---|
| KVM | 1 | 15748 / 15963 / 15139 | — | 3.8–4.0 / 7.7–8.0ms |
| SVM t4（W=1） | 1 | 10782 / 10456 / 11557 | **-32%** | 5.2–5.7 / 9.7–11.0ms |
| KVM | 2 | 19636 / 22307 / 22269 | — | 5.4–6.1 / 10.5–12.1ms |
| SVM t5（W=2） | 2 | 16459 / 9884 / 15706 | **-35%** | 7.3–12.2 / 14.5–38.9ms |
| KVM | 4 | 29464 / 25346 / 28833 | — | 8.1–9.5 / 17.3–24.8ms |
| SVM t0（W=4） | 4 | 22360 / 13769 / 27522 | **-29%** | 8.7–17.4 / 19.0–57.9ms |

**结论**：SVM 相对 KVM 系统性落后 **~30% txn**（-32/-35/-29%），与 W 档位无明显耦合——与 M2G 纯 IO 的 seqrd -36% 量级吻合（TPCC 读多写少把 seqrd 型损失摊到整体）。延迟劣化同向（每事务多一跳 loopback RTT + stub 处理，MySQL 队列化放大）。波动大（SVM t0 三次 229↔459/s 2×，盘相位 + MySQL 调度）；KVM/SVM 非背靠背（22:18 vs 22:41），但三档落后比例一致指示系统性折损。

### 8.2 四协议同日锚点（2026-08-25，macro-b/，权威跨协议对比）

背靠背时间窗：nvmeof 19:38→iscsi 19:49→KVM-b 20:06→SVM-b 20:16（全部同日）。guest 数据盘后端 = host initiator 设备：NVMe-oF `nvmet → loop(direct-io) → fio.raw`；iSCSI `LIO block → loop(direct-io) → data_iscsi.raw`。threads {1,2,4} × 3、60s、20s warmup、cache=none、每轮 drop_caches、优雅关机。表中为 txn/s 中位（3 次）。

| 协议 | threads=1 | vs KVM | threads=2 | vs KVM | threads=4 | vs KVM |
|---|---|---|---|---|---|---|
| KVM-b | 227.2 | — | 314.7 | — | 444.0 | — |
| SVM-b | 183.9（t4） | **-19%** | 197.1（t5） | **-37%** | 275.8（t0） | **-38%** |
| NVMe-oF | 158.3 | -30% | 217.6 | -31% | 273.8 | -38% |
| iSCSI | 119.4 | -47% | 216.9 | -31% | 249.5 | -44% |

**结论**：
1. **今日盘相位整体偏低，坐实锚点必要**：KVM-b（227/315/444）全面低于 8/24（262/371/481），跨日数值直接比较不可靠。
2. **SVM 相对 KVM：t1 -19% / t2 -37% / t4 -38%**：与 8/24 均匀 -30% 不同——低盘相位下 t1 折损收窄（t1 近盘上限、远程开销被掩盖），高并发放大到 ~-38%。"约三成"结论仍成立，量级随盘相位在 20–40% 间漂移。
3. **SVM ≈ NVMe-oF，iSCSI 恒最低**：virtio-remote 直连 stub aio 与 nvmet（NVMe 控制器+传输层+loop 块层）在 TPCC 下无实质差距；iSCSI 的 SCSI/TCP 固定开销使 t1 差 -25%、t4 差 -9%。
4. 波动仍是第一噪声：每协议总有 1 轮下探（SVM t4-r2 122、nvmeof t1-r3 111、iscsi t2-r1 172），中位数代表稳态，单轮差值不可解读。

数据位置：8/24 → `exp/results/macrokvm/`、`exp/results/macrosvm/`；8/25 → `exp/results/macro-b/{kvm,svm}/logs/`（汇总 `macro_b_summary.txt`）、`exp/results/macronet/{nvmeof,iscsi}/logs/`（汇总 `macronet_summary.txt`）。

---

## 9. App：Spark pagerank（四协议重做 + @150us + SVM RTT 卡死阈值）

### 9.1 四协议（2026-08-25，app-net/，同日背靠背，总耗时 s）

SVM 固定 **W=4 I=256 B=16**（用户指定，不再与 cores 耦合）；cores {1,2,4}、10 迭代、每轮 drop_caches、优雅关机。

| 协议 | c1 | c2 | c4 |
|---|---|---|---|
| KVM | 38.44 | 24.68 | 20.75 |
| SVM | 38.26 | 24.87 | 21.62 |
| NVMe-oF | 39.08 | 24.47 | 20.73 |
| iSCSI | 38.23 | 24.53 | 21.28 |

**结论**：四协议几乎无差距（最大差 = c4 SVM vs nvmeof 4%，其余 <2%）。App 并非没有磁盘 IO——实测 shuffle 确认写盘：读峰 111MB/s（JAR/wiki 加载）、写峰 120/101MB/s（shuffle write），总 IO ~400–500MB/20.9s ≈ 平均 20MB/s，集中在 6–8 个 1s 突发秒。它是**"总量小 + 顺序大块（~110KB/IO）+ 突发 + 与计算重叠 + 延迟不敏感"**型负载，盘远未饱和 → remote 路径每请求开销被淹没。与 Macro -19~-38% 对照自洽：**SVM 折损只在 IO 密集且延迟敏感负载下显形**。dd 路径实证：KVM 1.8GB/s > nvmeof 1.4GB/s > SVM 1.2GB/s > iscsi 455MB/s。

### 9.2 @ RTT≈150us（2026-08-26，app-2/）

RTT 注入时机修正：guest 0 RTT 正常 boot + 数据盘就绪后，才在 host lo 注入 `netem delay 50us`（等效 RTT ≈ 50 + 2×50 ≈ **150us**），对已建立连接立即生效。

| 协议 | c1 | c2 | c4 |
|---|---|---|---|
| KVM | 39.56 | 27.27 | 23.03 |
| SVM | 39.18 | 24.94 | 21.67 |
| NVMe-oF | 38.24 | 24.25 | 20.85 |
| iSCSI | 38.31 | 24.64 | 21.05 |

**结论**：
1. **150us RTT 下四协议几乎无差距**（同日背靠背 nvmeof vs iscsi +0.2/+1.6/+1.0%；SVM vs KVM c2/c4 -8.5/-5.9% 全落在 iter 纯 CPU 阶段、KVM 那轮逐轮偏慢，属逐轮噪声）；与无 RTT 基线几乎重合（最大差 ~3%）→ 150us 对 App 无影响。
2. **KVM 完全不受 lo netem 影响**（数据面本地 virtio 不走 lo；KVM @300us 与基线 38.38/24.21/20.66 一致）→ 坐实此前 150us "KVM 变慢"是盘相位噪声。
3. **SVM pagerank 在 RTT ≥200us 确定性卡死**（200/300us 各复现：Java CPU 冻结不增长、IO 不返回、guest load 堆积），**阈值在 150~200us 之间**；fio 探针排除 IO 层退化（psync 单延迟随 RTT 线性 96→258→360us、libaio 并发 91K IOPS 正常）——**卡死根因未定位**（按用户指令不修复）。**150us 是当前 SVM App 可用的最大 RTT 档**。

数据位置：`exp/results/app-net/{kvm,svm,nvmeof,iscsi}/logs/`（汇总 `app_net_summary.txt`）、`exp/results/app-2/{...}/logs/`（汇总 `summary.txt`）。

### 9.3 ALS（ml-25m，2026-08-26，app2/als-*，四协议同日背靠背）

- 数据：`ratings_big.csv`（200M 行 / 5.4GB，ml-25m 放大 8 倍，超 guest 4GB 内存），O_DIRECT 后端双盘预置（fio.raw + data_iscsi.raw）。
- 应用：`als_train.py`（9.3 逐行版，现备份为 `als_train_v1.py`）——pass1 全量并行分片扫行数 + pass2 采样 500 万建 CSR（130 万用户 × 64.6 万物品）；训练 20 iter × 128 factors **单线程 BLAS**（规避 SVM vCPU 竞争下 OpenBLAS 死锁）；每 5 iter **O_DIRECT** 写 V checkpoint 330MB（×4 = 1.3GB 写）。**cores 参数 = pass 阶段多进程分片读的并行度**（c1/c2/c4 → 1/2/4 进程）。
- SVM 固定 W=4 I=256 B=16；每轮 host+guest 双 drop_caches。

| 协议 | c1 total (build/train) | c2 total (build/train) | c4 total (build/train) |
|---|---|---|---|
| KVM | 107.1 (81.6/25.5) | 68.9 (43.2/25.6) | 49.5 (23.6/25.9) |
| SVM | 109.2 (82.5/26.6) | 68.6 (41.5/27.0) | 49.3 (23.1/26.2) |
| NVMe-oF | 108.2 (82.1/26.1) | 68.3 (42.3/26.1) | 49.8 (23.6/26.1) |
| iSCSI | 107.4 (80.1/27.3) | 72.5 (44.3/28.2) | 58.9 (23.1/35.8) |

**结论**：
1. **四协议几乎无差距**（SVM vs KVM total：+2.0%/+0.4%/+0.4%）；iSCSI c4 +19% 是 checkpoint O_DIRECT 写在 iSCSI 慢写路径的波动。SVM 折损 ≈ 0 —— 与 App pagerank（计算主导）一致。
2. **pass/build 是 Python 逐行解析的 CPU 瓶颈，非 IO**：KVM 本地盘读 5.4GB 仅 ~5s，但 build 81.6s → 解析 200M 行 ×2 遍占 ~95%；cores 区分度 = 多进程并行解析（近线性 1→2→4），IO 差异被完全掩盖（四协议 build 几乎重合）。
3. **train 恒定 ~26s**（单线程 BLAS 与 cores 解耦；4 次 O_DIRECT 写 1.3GB 在 iscsi 略慢）。
4. 对照：**ALS（解析主导）与 App（计算主导）都无折损，Macro TPCC（IO 密集+延迟敏感）-19~-38%** —— 第三次印证 **SVM 折损只在 IO 密集且延迟敏感负载下显形**；当前 ALS 实现测的是 CPU 并行扩展而非 IO。要让它 IO 密集需 numpy 向量化解析（read_csv chunksize），属可选改进。

**踩坑记录（方法学）**：
- **每 iter np.save 330MB（page cache 写）在 SVM 远程盘上触发写回风暴 → virtio-blk 卡死**（80 在途永不完成、jbd2/kworker D 态、stub 空闲 0.7%、guest 瘫痪 SSH 无响应）：近满盘（旧版每 iter 新建 U_<i>.npy 残留 10×333MB 占 93%）必现。解法：清残留（双盘 rm als_ckpt）→ checkpoint 降频（每 5 iter）→ **O_DIRECT 直落盘**（mmap 匿名页对齐缓冲分块写，`save_v_direct`）。
- **`/tmp/app2-guest.log` 残留 → qemu serial 打开 Permission denied**：sudo qemu（root）与 waiai 进程对 /tmp 残留文件属主不对付（沙箱/属主组合），serial 统一改 `$H/guest.log` + 启动前 sudo rm。
- **qemu 直通块设备必须 sudo**：/dev/nvme1n1、/dev/sda 被 TRAE 沙箱拦截非特权访问；且 `echo pw | sudo -S` 的管道内**不能含 `< /dev/null`**（同 fd 显式重定向覆盖管道 → sudo 读到 EOF "no password was provided"），`< /dev/null` 必须放 bash -c 外部。
- 数据：`exp/results/app2/als-{kvm,svm,nvmeof,iscsi}/logs/*.log`；脚本 `exp/als_train.py`、`exp/run_app2.sh <als> <proto>`、`exp/prep_data.sh als`（ml-25m）。

### 9.4 ALS-IO（ml-25m 向量化解析版，2026-08-26，app2/als-*，四协议同日背靠背）

- **动机**：9.3 的 pass/build 是 Python 逐行解析（CPU 瓶颈，build 81.6s 中解析占 ~95%），IO 差异被完全掩盖 → 改用 **pandas C 解析器向量化**（read_csv chunksize），让 5.4GB ×2 遍的数据加载真正 IO 主导。
- **实现**（`als_train.py` IO 密集版，9.3 版备份 `als_train_v1.py`）：
  - pass1 多进程 **C 级块扫数行**（64MB 块 `bytes.count(b'\n')`，纯 IO）+ 收集**行对齐链式段边界**（不重不漏，links 精确 = 200000760）；
  - pass2 多进程 **pandas `read_csv` chunksize=8M 行**分块解析 + 采样 500 万（`LimitedReader` 精确截断段边界）；
  - 训练/checkpoint 与 9.3 相同（单线程 BLAS + 每 5 iter O_DIRECT 写 330MB）。
- 验证：KVM c1 build 从 9.3 的 **81.6s → 28.6s**（解析瓶颈消除，加载 IO + C 解析各占一半左右）。

| 协议 | c1 total (build/train) | c2 total (build/train) | c4 total (build/train) |
|---|---|---|---|
| KVM | 56.4 (28.6/27.9) | 51.9 (14.5/37.4) | 41.9 (10.1/31.8) |
| SVM | 54.4 (27.5/26.9) | 43.4 (16.2/27.2) | 37.6 (10.3/27.3) |
| NVMe-oF | 53.8 (27.3/26.6) | 42.9 (16.3/26.6) | 36.7 (9.9/26.8) |
| iSCSI | 61.2 (34.6/26.6) | 48.5 (21.9/26.6) | 53.1 (26.5/26.6) |

**结论**：
1. **SVM/NVMe-oF 在 IO 密集加载段追平 KVM**（build：SVM vs KVM c1 -4% / c2 +12% / c4 +2%，噪声内；NVMe-oF 同）；**iSCSI 显形**：build c1 +21% / c2 +51% / c4 **+162%**（26.5 vs 10.1s）——iSCSI 533MB/s（dd 实测）带宽瓶颈在**并行分片读**（c4 四流打满盘）下被放大。
2. **SVM 顺序大块读（64MB/96MB 块）带宽 ≈ KVM**：与 M2G seqrd -36%（iodepth=16、随机/混合队列场景）不矛盾——ALS 加载是**纯顺序大块 + 高并行（4 worker 4 路读）**，SVM 远程路径在顺序大块下追平；**第四次印证 SVM 折损只在随机 IO + 延迟敏感负载显形，且这次是真实 IO 密集场景**（非解析/计算主导）。
3. **train 恒定 ~26.6s**（单线程 BLAS 与 cores/协议解耦；KVM c2/c4 的 37.4/31.8s 是 checkpoint O_DIRECT 写盘遇盘相位/调度波动，非 CPU 计算差异）。
4. **c4 是 IO 区分度最强档**（并行读最满）：KVM/SVM/NVMe-oF 10.0–10.3s vs iSCSI 26.5s——**协议层带宽瓶颈（而非 SVM 远程开销）在 IO 密集应用中的真实量级**：SVM ≈ KVM，iSCSI 是唯一明显短板。

**踩坑（方法学）**：
- **ml-25m movieId 是稀疏编号**（最大 209171，非 prep 注释的 62423）→ 放大后全文件 max_i=646139（实测），shape 硬编码 (1300336, 646140)；user 侧 8×162542 = 1300336 与公式一致。
- **pass1 按字节边界数行会丢跨段行**（每段边界 1 行，4 段丢 3）→ links 改用 pass2 链式边界的实际解析行数（精确 200000760）。
- **脚本进程启动竞态**：前一个协议脚本的 `shutdown_guest`（sleep 20 后 `pkill -9`）尚未结束就启动下一个 → 残留 pkill 误杀新 qemu/stub；需等上一脚本完全退出（OK 打印/进程消失）再启动。

数据：`exp/results/app2/als-{kvm,svm,nvmeof,iscsi}/logs/*.log`（9.3 旧日志备份 `/tmp/als93-backup/`）；脚本 `exp/als_train.py`（IO 密集版）、`exp/als_train_v1.py`（9.3 逐行版）、`exp/run_app2.sh <als> <proto>`。

### 9.5 ALS-BIN（紧凑二进制 + 每 iter 强制访存，2026-08-26 14:02–14:20，app2/als-bin-*，四协议同日背靠背）

- **动机**：9.3/9.4 的 build 是一次性加载（每配置只冷读 1 次），IO 被均摊；本轮改为**每训练 iter 强制从盘重读全量**，模拟分布式 ALS 每轮迭代扫全量文件的形态。
- **实现**（`als_train_bin.py`）：
  - 数据 `ratings_big.bin` = **200M 条 int32/int32/float32（12B/条，2.4GB），CSV 直转零放大**（`gen_als_bin.py` 只做格式转换、行数不变，**不扩大数据集**）；双盘预置 `/mnt/data/ratings_big.bin`。
  - build：**多进程分片读**（cores=分片流数，每段独立 rng）整块顺序读（96MB 块）max + 采样 500 万建 CSR，零解析（KVM c1 3.0s / c4 2.6s 冷读 2.4GB；重跑后见下表）。
  - train：每 iter 顺序读全文件 + 每块 `posix_fadvise(POSIX_FADV_DONTNEED)` 丢 guest 页缓存 → 下 iter 重新缺页走盘（**强制访存**）；再做 1 次交替最小二乘（单线程 BLAS）+ 每 5 iter O_DIRECT 写 checkpoint。
- **实证每 iter 真实读盘**：冒烟（build+4 iter）guest vdb 读扇区增量 23.1M ≈ 11.8GB ≈ 5×2.4GB（丢页失效则只有 ~2.4GB）——fadvise 强制访存在四条协议路径均生效。
- **每 iter ≈ scan(IO 主导) + compute(~0.4s)**：KVM scan ~1.8s/iter（占 78%）、iSCSI ~5.3s/iter（占 93%）→ 真正 IO 主导，协议差异显形。

**并行 build 重跑（2026-08-26 14:33–14:38，四协议同日背靠背，`ITERS=0` 只跑 build；build 改为多进程分片读、cores=分片流数；train 段未重跑，沿用旧 20 iter 值）**

| 协议 | c1 build (s) | c2 build (s) | c4 build (s) | c4/c1 核扩展 |
|---|---|---|---|---|
| KVM | 3.01 | 3.07 | 2.55 | 0.85×（本地盘带宽近上限，持平） |
| SVM | 3.43 | 2.59 | 1.70 | 0.50× |
| NVMe-oF | 4.44 | 3.67 | 1.40 | **0.31×** |
| iSCSI | 7.47 | 7.68 | 11.47 | **1.54× 负扩展** |

（每档前 host+guest drop_caches，全冷读；train_s 恒定 ~1.8s 为 CSR 构造/进程启动固定开销，非训练。旧 20 iter 全量参考——train 段代码未改动，仍有效：）

| 协议 | c1 total (build/train, 旧单线程 build) | c2 total | c4 total |
|---|---|---|---|
| KVM | 47.5 (1.8/45.7) | 47.1 (1.8/45.3) | 47.9 (1.8/46.1) |
| SVM | 51.7 (2.3/49.4) | 56.6 (2.2/54.4) | 58.7 (2.3/56.4) |
| NVMe-oF | 53.6 (2.0/51.6) | 52.6 (2.0/50.6) | 52.9 (2.1/50.8) |
| iSCSI | 122.4 (5.4/117.0) | 124.1 (5.6/118.5) | 123.9 (5.2/118.7) |

**结论（build 阶段，并行读）**：
1. **cores 恢复为真实并行度**：build 从单线程改多进程分片读后，c1/c2/c4 出现明确区分度（旧版 cores 仅日志、三档全同，无法绘图）。
2. **远程协议并行读扩展性反超 KVM**：SVM c4/c1=0.50×（2 倍加速）、NVMe-oF 0.31×（3.2 倍），c4 绝对时间 1.70s/1.40s 反而低于 KVM 2.55s（-33%/-45%）——SVM stub 4 worker（W=4）与 NVMe-oF loop 多流把 2.4GB 冷读拉满盘带宽；KVM 0.85× 基本持平（virtio-blk cache=none 单进程 4 vq，本地盘 ~940MB/s 近上限）。
3. **iSCSI 负扩展是最大短板放大器**：c4 多流 → 11.47s（比 c1 7.47s 还慢 54%），vs KVM c4 2.55s 差 4.5×；LIO block 单队列带宽瓶颈在并行流下被打爆——**并行度越高 iSCSI 越吃亏**（绘图区分度最大的一列）。
4. **SVM 折损不来自 build**：build 阶段 SVM ≥ KVM（并行扩展好）；折损集中于 train 段每 iter 强制重扫（旧 20 iter：49.4~56.4 vs 45.3~46.1，+9~22%）——逐 iter 缺页 → 远程往返的延迟成本在串行单流 train 上逐轮累加，与 9.4 对照成立。

**踩坑（方法学）**：
- **`madvise(MADV_DONTNEED)` 在 guest 内核 7.0.0-30 对只读 MAP_SHARED 文件映射返回 EINVAL**（映射类型限制）→ 改用 `posix_fadvise(POSIX_FADV_DONTNEED)`（只丢页缓存、不依赖映射类型），语义等价（下 iter 重新缺页走盘）；build 后也 fadvise 一次，iter 1 同样冷读。
- **多进程分片读必须 `os.lseek(fd, start, SEEK_SET)` 定位段起点**：fd 默认从 0 读，不 seek 会各 worker 读同一段 → max_u/max_i 错乱（c4 曾得 max_u=325083 而非 1300335）；已修并全档验证四协议 12 日志 max 均正确。
- 数据集**不扩大**：删 4.8GB 双倍放大 bin（400M 条），保留 200M 条 2.4GB 版本；`gen_als_bin.py` 纯格式转换零放大。

数据：`exp/results/app2/als-{kvm,svm,nvmeof,iscsi}/logs/*.log`（build-only 重跑 14:33–14:38）+ `als-bin-{kvm,svm,nvmeof,iscsi}/logs/*.log`（旧 20 iter 全量）；脚本 `exp/als_train_bin.py`、`exp/gen_als_bin.py`、`exp/run_app2.sh <als> <proto>`（`ITERS=0` 只跑 build）。

**并行 build 重跑 #2（CSV 直读原始数据，2026-08-26 14:51–14:59，四协议同日背靠背，`ITERS=0`）**

- **动机**：#1 二进制 build 是零解析纯 IO，2.4GB 读太快 → KVM c1/c2/c4 平线（3.01/3.07/2.55s），cores 区分度弱；改 **build 直读原始 `ratings_big.csv`（5.4GB，200M 行）**——pass1 多进程 C 级块扫数行（纯 IO）+ pass2 多进程 pandas 向量化解析采样（CPU 随 cores 扩展），IO+解析叠加后 cores 1/2/4 恢复真实区分度（`als_train_bin.py` 按扩展名分流：`.csv` 走此路径，仅支持 build-only，训练需先 `gen_als_bin.py` 转 bin）。
- 校验：12 日志全档 links=200000760、max_u=1300335 / max_i=646139（与二进制扫描一致）、sampled≈500 万；train_s=0（build-only 无训练，CSR 构造已计入 build）。

| 协议 | c1 build (s) | c2 build (s) | c4 build (s) | c4/c1 核扩展 |
|---|---|---|---|---|
| KVM | 28.11 | 16.80 | 11.65 | 0.41×（~2.4× 加速） |
| SVM | 30.32 | 15.30 | 9.38 | **0.31×（~3.2×，c4 四协议最快）** |
| NVMe-oF | 27.24 | 16.57 | 11.50 | 0.42× |
| iSCSI | 29.47 | 21.12 | 25.58 | **0.87× 负扩展** |

**结论（CSV 直读 build）**：
1. **cores 区分度恢复**：CSV 直读 = IO + 解析（CPU 随 cores 扩展），KVM 28.1→16.8→11.7s（c4/c1=0.41× ≈2.4× 加速）；对比二进制零解析 build 的平线（KVM 3.0/3.1/2.6s）——**解析成本正是 cores 1/2/4 区分度的来源**，二进制 build 只剩 IO 带宽上限测不了核扩展。
2. **SVM c4 绝对时间四协议最低**（9.38s，c4/c1=0.31×），与二进制 build 的 SVM c4 反超 KVM 一致（stub 4 worker 拉满盘带宽 + 解析 4 流并行）；但 **SVM c1 单流最慢**（30.32s vs KVM 28.11s，+8%）——单流时远程路径逐块往返的延迟成本显形，与 9.4 结论（并行大块追平、串行单流落后）自洽。
3. **iSCSI 负扩展依旧显形**：c2 21.12s / c4 25.58s（c4 比 c2 还慢 21%），vs KVM c4 11.65s 差 **2.2×**——LIO 单队列在并行解析读下打爆；与 9.4（c4 26.5s，+162%）量级一致，跨 run 可复现。
4. **与 9.4 ALS-IO（同日 CSV 直读）对照**：KVM c1 28.1 vs 28.6s、iSCSI c4 25.6 vs 26.5s 吻合——CSV 直读 build 解析主导，结果不随盘相位漂移（对比二进制 IO 主导 build 对盘相位敏感）；SVM c1 今日 30.3 vs 9.4 27.5s 属盘相位/调度噪声。

数据：`exp/results/app2/als-bin-csv/{kvm,svm,nvmeof,iscsi}/logs/*.log`（CSV 直读 build-only，14:51–14:59）+ `als-bin-csv-4proto.log`（四协议汇总）；原始 CSV 已拷回双盘（`/mnt/data/ratings_big.csv`，5.4GB），`ratings_big.bin` 已从双盘删除（`gen_als_bin.py` 可随时再生）；脚本 `exp/als_train_bin.py`（`<.csv>` 输入直读原始数据，仅 build-only）。

### 9.6 xgb（合成数据集，强制重扫 IO 密集版，2026-08-26 16:44–17:04，app2/xgb-synth-6m/，四协议同日背靠背）

**先导实验**（15:08–15:16，app2/xgb-synth/，5M 行 1.4GB，DMatrix 流式 CSV 解析 + 20 rounds）：解析/计算主导——1.4GB 首次读入后全在 guest 页缓存，训练循环不碰盘，四协议收敛（KVM/SVM/nvmeof/iscsi c4 ≈ 12.7~15.3s，SVM 仅 c4 train +21%），IO 不进关键路径，复现 pagerank 结论。→ 判定"数据集必须强制重扫才能显形 SVM 折损"。

**本版改动（目标：让 IO 回关键路径）**：
- 数据集 10M 行 2.76GB → **6M 行 1.66GB**（`head -n 6000000` 截取；10M 行解析时 `concatenate+np.save` 峰值 ~3.1GB 在 guest 3.4GB 上频繁 OOM → 收窄留余量；`xgb_parse_seg.py` 改**零拼接预分配**：pass0 C 级数行得精确 nrows + 预分配 X/y 逐 chunk 填行 + 每 chunk fadvise DONTNEED 丢页缓存，双保险）
- 重写 `exp/xgb_train.py`：xgboost 3.4.1 已移除文本流式输入（data.cc:918 移除 CSV 文本 + external memory），改 **subprocess 分片 pandas 向量化解析建 DMatrix**（**不用 multiprocessing**：guest Python 3.14 的 Pool/spawn/fork 在任务管道上死锁——worker 收不到任务、父进程 futex 空等，30MB 小文件正常、真实数据盘必现；subprocess 最底层最可靠）
- **train 每 round 强制重扫**：`scan_full` 顺序读全量 1.66GB + fadvise DONTNEED 丢页缓存 → 下 round 重新缺页走盘（同 ALS-BIN 强制访存形态），再做 1 round 内存内 xgb.train（hist/depth6/eta0.3/auc）——**16 rounds × 1.66GB = 26.6GB 每次运行从协议盘重拉**
- **坑：SVM 远程盘写回风暴卡死 stub 队列**（ALS-BIN 已知，当时用 O_DIRECT 绕开）：build 的 npy scratch 原放数据盘 /mnt/data，`np.save` 672MB 缓冲写触发 guest 内 kworker/jbd2 **永久挂死**（>614s；读 3.44GB 全部正常、第一个写就挂）→ scratch 移到**本地系统盘 `/home/wai/xgbparse`**（guest / 只剩 520M，先 `journalctl --vacuum-size=100M` 腾 ~700M，run_app2.sh 已内置），远程盘全程只读 → 不再挂

**结果**（total_s，括号 build/train；16 rounds 强制重扫；每档 host+guest drop_caches + 128MB dd 预热）：

| 协议 | c1 total (build/train) | c2 total (build/train) | c4 total (build/train) | vs KVM |
|---|---|---|---|---|
| KVM | 55.37 (11.24/44.13) | 50.60 (6.81/43.79) | 49.37 (5.04/44.34) | — |
| SVM | 60.00 (11.76/48.24) | 57.66 (7.29/50.37) | 57.24 (6.70/50.54) | +8.4/+13.9/+15.9% |
| NVMe-oF | 58.05 (10.91/47.15) | 53.67 (7.08/46.59) | 51.59 (5.26/46.34) | +4.8/+6.1/+4.5% |
| iSCSI | 96.84 (13.18/83.67) | 94.91 (9.21/85.70) | 93.59 (9.50/84.09) | +74.9/+87.6/+89.6% |

（links 全部 6000000；build 随 cores 扩展：subprocess 并行解析 11.2→5.0s（KVM）；train 协议内各 cores 基本平、四协议间明显分层。）

**结论**：
1. **强制重扫让 xgb 变真 IO 密集，四协议分层显形**：train 段 KVM 44.1s < NVMe-oF 46.6s < SVM 49.6s < iSCSI 84.5s（各 cores 中位）——26.6GB 重扫占每轮 ~40%（KVM）~80%（iSCSI）；SVM 折损从"收敛"变为 **+8~16%**、iSCSI **+75~90%**。与 ALS-BIN"每 iter 强制重扫 +9~22%"同一机制，量级自洽。
2. **SVM 折损集中在 train（远程逐轮重扫）而非 build**：build 四协议差异小（并行读+解析，SVM c1 11.8s vs KVM 11.2s）；train SVM 比 KVM 慢 4~6s（远程 seqrd 带宽 + stub 与训练抢 CPU），c2/c4 略高于 c1。
3. **iSCSI 恒为最大短板**：带宽 ~400–530MB/s，26.6GB 重扫直接吃满——train 84s ≈ 26.6GB/320MB/s，与 dd 预热 412MB/s 量级吻合。
4. 旧 15:08 版（解析主导、IO 不进关键路径）四协议收敛 → 本版（强制重扫）分层——**IO 密集形态的关键是"每 iter 强制重扫（缺页→远程往返逐轮累加）"，数据集大小只是次因**：1.66GB 全装进页缓存也可用 fadvise 逐轮驱逐（本版 1.66GB 仍远超 guest 页缓存容量差，重扫真实走盘）。

数据：`exp/results/app2/xgb-synth-6m/logs/*.log`（16:44–17:04 背靠背）；脚本 `exp/xgb_train.py`（subprocess 解析 + 每 round scan_full 强制重扫）、`exp/xgb_parse_seg.py`（分片解析 worker，零拼接预分配）、`exp/gen_synth_xgb.py`（10M 行生成，已截 6M）；数据 `exp/data/xgb_synth.csv`（6M 行 1.66GB，双盘 `/mnt/data/xgb_synth.csv`）；旧版数据 `exp/results/app2/xgb-synth/`（先导，15:08）。

---

## 10. 综合结论（跨实验）

1. **SVM 相对 KVM 的折损集中在"IO 密集 + 延迟敏感"负载**：纯 IO 测到唯一真实瓶颈 seqrd -36%（M2G）；TPCC 混合负载 -19~-38%（Macro）；App pagerank 无差距（IO 太小被计算淹没）；**ALS-IO（真实 IO 密集、一次性顺序大块加载）SVM 追平 KVM**（9.4），**ALS-BIN（每 iter 强制重扫全量）SVM total +9~23%**（9.5；折损集中在 train 段逐 iter 重扫 +9~22%，build 阶段并行读 SVM c4/c1=0.50× 扩展反超 KVM、iSCSI c4 负扩展 1.54×；CSV 直读 build 复现：SVM c4/c1=0.31× 且 c4 绝对时间四协议最低 9.38s、iSCSI c4 负扩展 0.87× 差 KVM 2.2×）——**SVM 折损的充分条件是"延迟敏感"，顺序大块读本身不是：一次性冷读不显形，一旦每轮 iter 强制重扫（缺页 → 远程往返逐轮累加）就显形**；iSCSI 带宽瓶颈（450~533MB/s）在各 IO 密集场景恒为最大短板（9.4 c4 +162%、9.5 全档 +158~163%、并行 build c4 差 4.5×）。**xgb 合成数据先导版（1.4GB，解析+计算主导、IO 不进关键路径）四协议收敛、SVM 仅 c4 train +21%**（9.6，复现 pagerank 结论）；**强制重扫版（6M 行 1.66GB，16 rounds × 1.66GB 逐轮重扫）SVM total +8~16%、iSCSI +75~90%，train 段分层 KVM 44s < NVMe-oF 47s < SVM 50s < iSCSI 84s**（9.6）——**再次坐实"SVM 折损的充分条件 = 延迟敏感 × 每 iter 强制重扫"，与 ALS-BIN 同一机制**。多档量级自洽。
2. **吞吐统一由"在途 / RTT"决定（Little's law）**：inflight 窗口（I）是 SVM 吞吐的第一旋钮——M3 窗口减半全 RTT 域吞吐减半；batch（send）不改变在途故无净增益（M4/M4B）；高 RTT 下所有远程协议（SVM/NVMe-oF/iSCSI）收敛到同一曲线（M3 三协议）。
3. **stub recv batch（Q）是盘并行的开关（10×），local send batch（L）是二阶优化（-8%）**：Q=1 盘饿死 7K，Q≥16 解锁 55–77K；完整模型 吞吐 ≈ min(盘(min(在途,Q)), 协议(min(在途,I), slack))。
4. **merge（M5）+ buf_pool（P）在大块上有效**：P=on 给 1M/2M +22~35%；但 SVM 带宽占比仍随 bs 缩小（40%→13%），大块远程路径每字节成本线性放大，iodepth=1 也躲不掉。
5. **内存开销恒定 +5~10%**（M1，iodepth=1 权威版）：在途缓冲 = 在途 × bs，非结构性问题；zc 只移拷贝不灭在途。
6. **RTT 容忍度由协议基线延迟决定**（网络存储 RTT 矩阵）：低延迟协议（NVMe-oF/SVM）对 RTT 敏感，高开销协议（iSCSI）不敏感。
7. **SVM 在 RTT ≥200us 下 App 级负载确定性卡死**，根因未定位，150us 为当前可用上限。
8. **方法学**：盘相位 ±55% + page cache 污染是历史结论错误的两大来源；所有可引用的结论均基于 8/24 后"O_DIRECT + drop_caches + 背靠背锚点"重做版。

---

## 11. 数据位置索引（`exp/results/`）

| 实验 | 目录 | 脚本 |
|---|---|---|
| M1 内存 | `m1mem/` | `exp/run_m1mem.sh`、`exp/guest_m1mem.sh`、`exp/plot_m1mem.py` |
| M2G（权威） | `m2g/`、`m2g/kvm_once/` | `exp/run_m2g.sh`、`exp/run_m2g_rest.sh`、`exp/host_anchor.sh`、`exp/run_kvm_m2g.sh` |
| M2FI | `m2fi/m2fi_svm.jsonl` | — |
| M3 RTT×窗口 | `m3rtt3/` | `exp/run_m3rtt3.sh` |
| M3 三协议 | `m3net2/{nvmeof,iscsi}/` | `exp/run_m3_net2.sh <nvmeof\|iscsi>` |
| NVMe-oF/iSCSI 早期矩阵 | `network/` | `exp/run_m3_net.sh` |
| M4 batch 重做 | `m4new/` | — |
| M4B send batch | `m4local/` | — |
| M5 merge | `m5/`（顶层 bs.json；q1/q16/q32/q64 是 M6） | `exp/run_m5.sh`、`exp/host_switch_blk_null.sh` |
| M6 recv batch | `m5/q1~q64/`、`m5/q64i128/` | `exp/run_m5v.sh` |
| Macro 8/24 | `macrokvm/`、`macrosvm/` | `exp/run_macro_kvm.sh`、`exp/run_macro_svm.sh`、`exp/run_macro_svm_fix.sh` |
| Macro 四协议 8/25 | `macro-b/{kvm,svm}/`、`macronet/{nvmeof,iscsi}/` | `exp/run_macro_b_{kvm,svm}.sh`、`exp/run_macro_net.sh <nvmeof\|iscsi>` |
| App 四协议 | `app-net/{kvm,svm,nvmeof,iscsi}/` | `exp/run_app_{kvm,svm,net}.sh` |
| App @150us | `app-2/{kvm,svm,nvmeof,iscsi}/` | `exp/run_app2.sh <xgb\|als> <proto>`（新算法框架） |
| App2 ALS | `app2/als-{kvm,svm,nvmeof,iscsi}/` | `exp/run_app2.sh als <proto>`、`exp/als_train.py`、`exp/prep_data.sh als`（ml-25m） |
| App2 ALS-BIN | `app2/als-bin-{kvm,svm,nvmeof,iscsi}/`（旧 20 iter）、`app2/als-{kvm,svm,nvmeof,iscsi}/`（bin build-only）、`app2/als-bin-csv/`（CSV 直读 build-only） | `exp/als_train_bin.py`（`.csv` 输入直读原始数据仅 build-only）、`exp/gen_als_bin.py` |
| App2 xgb（合成） | `app2/xgb-synth/{kvm,svm,nvmeof,iscsi}/` | `exp/run_app2.sh xgb <proto>`（`APP_DATA=/mnt/data/xgb_synth.csv`）、`exp/xgb_train.py`、`exp/gen_synth_xgb.py` |
