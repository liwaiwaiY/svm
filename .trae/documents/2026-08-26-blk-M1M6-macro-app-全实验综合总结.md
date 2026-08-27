# blk SVM 全实验综合总结（M1–M6 + Macro + App）

- 综合整理稿，日期：2026-08-26
- 整合来源（按权威度选版）：
  - [2026-08-22-blk-svm实验结果总结.md](./2026-08-22-blk-svm实验结果总结.md)（M2G/M3/M4/M4B/M6/Macro/Macro-B/App）
  - [2026-08-25-blk-m1mem-m5merge实验结果总结.md](./2026-08-25-blk-m1mem-m5merge实验结果总结.md)（M1mem/M5）
  - [2026-08-22-nvmeof-iscsi-m3实验结果总结.md](./2026-08-22-nvmeof-iscsi-m3实验结果总结.md)（NVMe-oF/iSCSI RTT 容忍度早期矩阵）
- **选版原则**：凡 2026-08-24 后重做过的实验（因盘相位 ±55% + page cache 污染问题），一律取重做版为"最合理数据"；8/22 前早期数据（`exp_results/`）与 8/24 前作废版仅作方法学对照，不进入结论。

***

## 0. 方法论铁律（一切结论的前提）

1. **盘相位波动 ±55%**：KINGSTON SNV2S1000G（DRAM-less 入门 NVMe，81% 满）同一配置 randrd 数小时内 119K↔185K（clat 530us↔341us）；写负载还会疲劳盘（KVM randwt 三次 54→41→35K 递减）。
2. **背靠背 + 锚点是唯一可信对比方式**：SVM/KVM/裸盘/多协议对比必须几分钟内交替测，每档前后加 host O\_DIRECT 锚点；任何跨天/跨时段数值对比不可信（macro-b 实证：KVM-b 227/315/444 vs 8/24 的 262/371/481，整体低 13%）。
3. **禁 page cache**：一律 O\_DIRECT（fio direct=1、virtio-blk `cache=none`、nvmet loop direct-io、LIO block direct-io）+ host 每轮 `drop_caches`。旧 KVM 基线 382K/290K 是 page cache 命中假象（iops\_max=459K 超标称 2.3 倍），作废。
4. **fio 口径**：60s 稳态、`time_based`、libaio、1 job（除非注明）；randrw 恒为 70/30、bs=4K。

***

## 1. 实验环境与公共配置

| 项           | 值                                                                                                       |
| ----------- | ------------------------------------------------------------------------------------------------------- |
| SVM         | local QEMU + remote stub 双进程，TCP 127.0.0.1:5552                                                         |
| 数据盘         | virtio-blk-pci，vq 数 = `virtio_pci_optimal_num_queues()` = smp.cpus = 4 vq                               |
| worker pool | vq 按 `vq_nr % workers` 哈希到 worker；inflight\_size（I）是窗口，按**请求计数**                                        |
| 档位矩阵        | t0（W=4/I=32/B=16，基准）、t4（W=1）、t5（W=2）、t7/t8（B=1/4，作废）                                                    |
| zc          | `zc_send_min=1MB` 统一（MSG\_ZEROCOPY 仅用于 ≥1MB 响应）                                                         |
| RTT 注入      | host lo `tc qdisc netem delay Xus`（单向），RTT = 100 + 2X us；对已建立连接立即生效；**必须在环境全部就绪后注入**（boot/初始化不被 RTT 放大） |

***

## 2. M1：内存开销（2026-08-25，m1mem）——SVM 恒定 +5\~10%

### 配置

bs{64K,512K,1M,2M} × {SVM zc=0（全 zc）, SVM zc=4M（4M 以下走 copy）, KVM}；I=256、P=off、**iodepth=1**（避免在途堆积干扰）。口径 = local RSS + stub RSS + skmem。

### 数据（mean kB，total = local+stub+skmem）

<br />

| bs   | cell            | local  | stub  | total  | vs KVM |
| :--- | :-------------- | :----- | :---- | :----- | :----- |
| 64K  | SVM zc=0        | 881357 | 34326 | 915721 | +7.3%  |
| 64K  | SVM zc=4M(copy) | 907998 | 34029 | 942051 | +10.4% |
| 64K  | KVM             | 853256 | —     | 853256 | —      |
| 512K | SVM zc=0        | 899083 | 35745 | 935011 | +6.7%  |
| 512K | SVM zc=4M(copy) | 918365 | 36009 | 954489 | +9.0%  |
| 512K | KVM             | 875783 | —     | 875783 | —      |
| 1M   | SVM zc=0        | 885613 | 37530 | 923415 | +5.1%  |
| 1M   | SVM zc=4M(copy) | 907761 | 38969 | 946998 | +7.8%  |
| 1M   | KVM             | 878215 | —     | 878215 | —      |
| 2M   | SVM zc=0        | 913590 | 47139 | 961156 | +8.9%  |
| 2M   | SVM zc=4M(copy) | 905434 | 48166 | 954106 | +8.1%  |
| 2M   | KVM             | 882431 | —     | 882431 | —      |

（stub 恒定 34–48MB，skmem KB 级；local RSS 跨 cell 基线波动 ±5%，% 仅作量级参考，同 bs 内 zc/copy 背靠背可比。）

### 结论

1. **iodepth=64 时代的"+14%→+81% 随 bs 涨"是在途堆积假象，作废**；iodepth=1 下 SVM 相对 KVM 恒定 +5\~10%，与 bs 无关。
2. 额外内存 = 远端路径在途缓冲 ≈ 在途数 × bs，iodepth 一降即消失。
3. **zc vs copy 结论反转**：iodepth=1 下 zc 的 local 反比 copy 低 2–3%（64K\~1M），2M 打平；之前"64K zc 更费内存"是堆积场景页 pin 叠加的产物。
4. 机制：数据必须完整穿过 local→TCP→stub，在途占内存不可避免；zc 只把拷贝从 local 挪到 stub 排队缓冲 + skmem（pin 重叠）。

数据位置：`exp/results/m1mem/`（图 `m1mem_bar_all.png` / `m1mem_pct_kvm.png`）；脚本 `exp/run_m1mem.sh`。

***

## 3. M2：worker pool / inflight 窗口（M2F→M2G 权威）——W=1 唯一瓶颈，唯一真实损失 seqrd -36%

> 演进：M2（numjobs=W 耦合，作废）→ M2F（numjobs=4 固定，2026-08-22）→ **M2G（60s 稳态 + host 锚点 + KVM 禁 cache 复核，2026-08-24，权威）**→ M2FI（窗口模型验证，部分结论被 M2G 修正）。

### M2G（numjobs=4、iodepth=16、60s，仅变 W；KVM 禁 page cache）——平均 IOPS

| 档位  | W | randrd | randwt | seqrd  | seqwt |
| --- | - | ------ | ------ | ------ | ----- |
| t4  | 1 | 153.7K | 40.3K  | 89.0K  | 67.8K |
| t5  | 2 | 175.9K | 73.5K  | 135.9K | 87K   |
| t0  | 4 | 189.5K | 94.2K  | 148.3K | 96.6K |
| KVM | — | 199K   | 96K    | 220K   | 122K  |

### M2G 结论

- **W=1 是唯一干净瓶颈**：4 负载全部显著低于 W=2/4（4 vq 挤 1 worker 串行化）。
- **W=2 vs W=4 基本持平**（盘相位噪声内）；M2F 时代"W=4 负扩展 -41%/-35%"消失。randwt 是唯一随 W 单调升的负载（40→73→94K）。
- **SVM 相对 KVM 的唯一真实损失 = seqrd -36%**（148.3K vs 220–233K，顺序读对盘相位不敏感）：高 IOPS 顺序读下协议路径每请求开销显著。randrd/randwt/seqwt 均在 KVM 波动范围内，**无净瓶颈**（修正此前"randwt 丢 40–60%"结论，那是盘慢相位/写疲劳数据）。
- 旧 KVM 基线（382K/290K）page cache 污染，全部作废。

***

## 4. M3：inflight 窗口 × RTT 容忍度

### 4.1 M3RTT3：窗口减半 → 全 RTT 域吞吐减半（2026-08-24，m3rtt3/）

负载 randrw 70/30、4K、iodepth=32、1 job。RTT = 100 + 2×netem delay us。

| RTT   | SVM-I16 | SVM-I32 | NVMe-oF | iSCSI | KVM   |
| ----- | ------- | ------- | :------ | :---- | ----- |
| 0.1ms | 29.2K   | 53.5K   | 48.2K   | 61.4K | 65.1K |
| 0.2ms | 24.7K   | 47.6K   | 50.2K   | 57.4K | 65.1K |
| 0.5ms | 16.3K   | 33.9K   | 37.4K   | 40.3K | 65.1K |
| 1ms   | 9.3K    | 23.2K   | 29.7K   | 26.8K | 65.1K |
| 2ms   | 5.8K    | 14.1K   | 15.1K   | 13.8K | 65.1K |

**结论**：

1. 高 RTT 端验证"吞吐 = 在途/RTT"：2ms 时对理论上限效率 72%/88%，RTT 越高效越逼近 100%（Little's law 成立）。
2. **窗口减半 → 全 RTT 域吞吐减半**（I16/I32 ≈ 0.40–0.54），无论 RTT 高低窗口都是第一瓶颈：低 RTT 端限制协议并发，高 RTT 端限制在途。**SVM 吞吐由 inflight 窗口而非 guest iodepth 决定**（iodepth=32 但窗口 16 时在途封顶 16）。
3. SVM-I32 @0.1ms 53.5K vs KVM 69.1K → 混合负载 SVM \~23% 开销（介于 randrd 无瓶颈与 seqrd -36% 之间）；2ms 下 14.1K vs 60.3K——远程存储高 RTT 下因 TCP 在途受限崩塌，本地 KVM 无此问题。

**结论**：

1. **高 RTT 端三协议收敛到同一"吞吐 = 在途/RTT"曲线**（2ms 效率 88%/94%/86%）——高 RTT 下瓶颈是 TCP 在途，与转发路径（emulator 用户态 or host 内核态）无关，所有远程协议等效崩塌。
2. **低 RTT 端每请求开销排序**：iSCSI (61.4K) ≈ KVM (69.1K) 最优（达 KVM 89%），SVM (53.5K) 与 NVMe-oF (48.2K) 相当——SVM 的 emulator 用户态转发相对 nvmet 内核转发**无额外劣势**。
3. RTT 20 倍（0.1→2ms）吞吐跌 SVM 3.8× / NVMe-oF 3.2× / iSCSI 4.4×，全被"在途/RTT"封顶；低 RTT 端三协议差 ≈1.2–1.4×。

***

## 5. M5：stub in\_sg merge 大块（2026-08-25，m5/）——merge+P 在大块上有收益

### 配置与新 knob

- 新 knob `stub_merge_m`（M，字节阈值）：>0 且请求 in 总字节 ≤ 阈值时，把单请求内多个 4K in\_sg 合并为单个连续大缓冲（in\_num=1），设备单次大 aio、响应单 iov；**wire 协议不变（响应只有 data\_len），local 端零改动**。代码：virtio-remote.c L489/527/577/3843–3882。
- 矩阵：bs{16K,64K,256K,1M,2M} × {SVM P=off, SVM P=on, KVM}；SVM I=256、**iodepth=1（同步）**、Z=0（全 zc）、M=4M、W=4、B=16。

每请求延迟（1/IOPS，µs）：

| bs   | SVM P=off | SVM P=on | KVM  |
| ---- | --------- | -------- | ---- |
| 16K  | 48.7      | 49.8     | 19.8 |
| 64K  | 72.2      | 74.8     | 23.6 |
| 256K | 172.3     | 154.5    | 27.3 |
| 1M   | 506.1     | 375.6    | 62.8 |
| 2M   | 818.0     | 671.3    | 88.5 |

### 结论

1. **SVM 相对 KVM 带宽占比随 bs 缩（40%→13%）**：bs 16K→2M（128×），KVM 单请求延迟只涨 4.5×（19.8→88.5µs），SVM P=off 涨 16.8×（48.7→818µs）、P=on 涨 13.5×。远程路径每字节成本（local→TCP→stub 拷贝/传输/aio）在大块上线性放大，**即使 iodepth=1 同步模式也躲不掉**。
2. **P=on（buf\_pool）大块收益明显**：1M +35%、2M +22%、256K +11%；≤64K 噪声级。机制：merge 后单请求=连续大缓冲，P=off 时每请求 posix\_memalign+free 大块（堆抖动），P=on 按页数回收复用。
3. **已知缺口**：矩阵无 M=0（不合并）对照，merge 本身收益未单独量化（P 的收益叠加在 merge 之上）。

数据位置：`exp/results/m5/<tag>.bs.json`（**同目录 q1/q16/q32/q64 是 M6 数据，勿混淆**）。

***

## 7. M6：stub recv batch（2026-08-24，m5/ q1-q64）——决定性因素，10× 跳变

### 配置

新 knob `vr.conf: stub_queue_max`（Q）= req\_queue 上限 = 设备 aio 并发上限；**Q=1 即"stub 无批处理"**。I=64（基准）、L=S=32（send 全批排除干扰）、batch\_m=512K、W=4。fio randread 4K、1 job、后端 fio.raw cache=none（O\_DIRECT），每档前后 host 锚点。

| iodepth | Q=1  | Q=16      | Q=32      | Q=64 (I=128)  | KVM-等效 |
| ------- | ---- | --------- | --------- | ------------- | :----- |
| 1       | 5.5K | 5.5K      | 5.5K      | 5.5K          | 7.47K  |
| 16      | 7.0K | **55.6K** | 55.4K     | 55.7K         | 65.3K  |
| 32      | 7.1K | 66.7K     | **77.0K** | 77.2K         | 88.5K  |
| 64      | 7.2K | 71.2K     | 66.8K     | **92.2K**     | 101.4K |

### 结论

1. **Q=1（无 recv batch）= 7.2K 封顶**：stub 同一时刻仅 1 个 aio 在途（clat\@d=64=8.9ms 全串行）→ 吞吐被盘串行延迟锁死（1/140us）。**recv batch 决定设备 aio 并发度，是 SVM 能否利用盘并行的开关**。
2. **Q≥16 解锁盘并行 → 55–77K，Q=1→Q=16 是 10× 跳变**；Q16/Q32/Q64 差异在盘相位噪声内，q16 已基本吃满收益。d=1 四档全同（178–180us 串行基线）——与 M4B 的 d=1 重合互为验证。
3. **窗口 slack 是第二独立瓶颈**：同 Q=64、同 iodepth=64，仅把窗口 64→128（加 64 空位当预缓冲），吞吐 **65.9K→92.2K（+40%）**——d=64 塌陷是窗口零 slack（I=64 恰好打满、RTT 全暴露）而非盘。
4. **完整模型（Q/I/iodepth 三旋钮）**：盘深度 D=min(在途,Q)、协议在途 N=min(在途,I)、slack=I−N，有效吞吐 ≈ min(盘(D), 协议(N,slack))。**Q 决定盘并发（Q=1 盘饿死 7K），I 决定隐藏 RTT 的排队冗余（I=Q 零 slack 露 RTT 66K，I≥2Q 满 slack 92K）**，guest iodepth 是需求端。三环最弱者即瓶颈。
5. **send batch vs recv batch 量级**：local send batch（M4B）= -8% 二阶优化；stub recv batch（M6）= **10× 决定性因素**。

数据位置：`exp/results/m5/q1/`、`q16/`、`q32/`、`q64/`、`q64i128/`。

***

## 8. Macro：sysbench TPCC（数据盘 O\_DIRECT 重跑 + 四协议同日锚点）

### 8.1 四协议同日锚点（2026-08-25，macro-b/，权威跨协议对比）

背靠背时间窗：nvmeof 19:38→iscsi 19:49→KVM-b 20:06→SVM-b 20:16（全部同日）。guest 数据盘后端 = host initiator 设备：NVMe-oF `nvmet → loop(direct-io) → fio.raw`；iSCSI `LIO block → loop(direct-io) → data_iscsi.raw`。threads {1,2,4} × 3、60s、20s warmup、cache=none、每轮 drop\_caches、优雅关机。**下表仅保留 round 1（r1）结果，vs KVM 相对同 run 的 KVM-b 计算**（SVM 档位：t1→t4、t2→t5、t4→t0；r2/r3 原始值与 max 见 `exp/results/macro-b/{kvm,svm}/logs/` 与 `macronet/{nvmeof,iscsi}/logs/`）。

**txns/s（r1）**：

| 协议      | threads=1 | vs KVM | threads=2 | vs KVM | threads=4 | vs KVM |
| ------- | --------- | ------ | --------- | ------ | --------- | ------ |
| KVM-b   | 240.0     | —      | 298.7     | —      | 446.3     | —      |
| SVM-b   | 185.6     | -23%   | 252.2     | -16%   | 248.5     | -44%   |
| NVMe-oF | 158.3     | -34%   | 221.5     | -26%   | 273.8     | -39%   |
| iSCSI   | 109.3     | -54%   | 171.6     | -43%   | 249.5     | -44%   |

**avg 事务延迟（ms，r1；sysbench 默认只报 95th，未采集 p99）**：

| 协议      | threads=1 | vs KVM | threads=2 | vs KVM | threads=4 | vs KVM |
| ------- | --------- | ------ | --------- | ------ | --------- | ------ |
| KVM-b   | 4.17      | —      | 6.69      | —      | 8.96      | —      |
| SVM-b   | 5.39      | +29%   | 7.93      | +19%   | 16.09     | +80%   |
| NVMe-oF | 6.32      | +52%   | 9.03      | +35%   | 14.61     | +63%   |
| iSCSI   | 9.15      | +119%  | 11.65     | +74%   | 16.03     | +79%   |

**p95 事务延迟（ms，r1）**：

| 协议      | threads=1 | vs KVM | threads=2 | vs KVM | threads=4 | vs KVM |
| ------- | --------- | ------ | --------- | ------ | --------- | ------ |
| KVM-b   | 8.13      | —      | 13.70     | —      | 20.74     | —      |
| SVM-b   | 10.65     | +31%   | 17.63     | +29%   | 44.17     | +113%  |
| NVMe-oF | 13.22     | +63%   | 20.74     | +51%   | 37.56     | +81%   |
| iSCSI   | 30.26     | +272%  | 38.94     | +184%  | 45.79     | +121%  |

（备注：① vs KVM 相对同 run KVM-b 逐对计算。② p95 劣化幅度系统性大于吞吐劣化（如 iSCSI t2：txn -43% 但 p95 +184%）——尾部延迟对远程排队/RTT 更敏感。③ t1 排序 KVM 240.0 > SVM 185.6 > NVMe-oF 158.3 > iSCSI 109.3（p95 8.13 < 10.65 < 13.22 < 30.26 同向），与结论 5 的固定开销机制一致。④ 结论 1-5 的百分比仍以 3 轮中位为准，与 r1 表数值存在轮次差异。）

**结论**：

1. **今日盘相位整体偏低，坐实锚点必要**：KVM-b（227/315/444）全面低于 8/24（262/371/481），跨日数值直接比较不可靠。
2. **SVM 相对 KVM：t1 -19% / t2 -37% / t4 -38%**：与 8/24 均匀 -30% 不同——低盘相位下 t1 折损收窄（t1 近盘上限、远程开销被掩盖），高并发放大到 \~-38%。"约三成"结论仍成立，量级随盘相位在 20–40% 间漂移。
3. **SVM ≈ NVMe-oF，iSCSI 恒最低**：virtio-remote 直连 stub aio 与 nvmet（NVMe 控制器+传输层+loop 块层）在 TPCC 下无实质差距；iSCSI 的 SCSI/TCP 固定开销使 t1 差 -25%、t4 差 -9%。
4. 波动仍是第一噪声：每协议总有 1 轮下探（SVM t4-r2 122、nvmeof t1-r3 111、iscsi t2-r1 172），中位数代表稳态，单轮差值不可解读。
5. **t1 排序（SVM 183.9 > NVMe-oF 158.3 > iSCSI 119.4）机制 = 低并发下每 I/O 固定协议开销直接暴露**：threads=1 时事务串行、在途≈1、盘空闲，吞吐≈1/事务延迟；四协议事务形态一致（queries/txn≈28），差距纯来自每 I/O 端到端延迟。每事务延迟增量（vs KVM-b avg 4.40ms）：SVM +1.03ms / NVMe-oF +1.92ms / iSCSI +3.97ms（avg 5.43/6.32/8.37ms，p95 10.65/13.22/22.69ms），比例 ≈1:1.9:3.9。对应每 IO 固定成本排序：**virtio-blk 自定义 TCP 转发（guest 最简驱动 + 用户态直连 stub aio，最轻）< nvme-tcp（NVMe 栈 + 胶囊/PDU 封装 + 内核 nvmet + loop 块层）< iSCSI（SCSI 命令层 + BHS + 用户态 iscsid + LIO，最重）**。高并发下排序反转（t2/t4 差距收窄、M3 d=32 时 iSCSI 反超 61K）：固定开销被流水线摊薄，瓶颈转为在途窗口 × 盘并行，SVM 的 I=32 窗口成为自己的天花板——**低并发看固定开销（SVM 胜），高并发看在途能力（SVM 受限）**，与完整模型 min(盘, 协议(在途, slack)) 自洽。

数据位置：8/24 → `exp/results/macrokvm/`、`exp/results/macrosvm/`；8/25 → `exp/results/macro-b/{kvm,svm}/logs/`（汇总 `macro_b_summary.txt`）、`exp/results/macronet/{nvmeof,iscsi}/logs/`（汇总 `macronet_summary.txt`）。

***

## 9. App

### 9.1 PySpkark+PageRank+widi dump @ RTT≈150us（2026-08-26，app-2/）

RTT 注入时机修正：guest 0 RTT 正常 boot + 数据盘就绪后，才在 host lo 注入 `netem delay 50us`（等效 RTT ≈ 50 + 2×50 ≈ **150us**），对已建立连接立即生效。

| 协议      | c1    | vs KVM | c2    | vs KVM | c4    | vs KVM |
| ------- | ----- | :----- | ----- | :----- | ----- | :----- |
| KVM     | 39.56 | —      | 27.27 | —      | 23.03 | —      |
| SVM     | 39.18 | -1.0%  | 24.94 | -8.5%  | 21.67 | -5.9%  |
| NVMe-oF | 38.24 | -3.3%  | 24.25 | -11.1% | 20.85 | -9.5%  |
| iSCSI   | 38.31 | -3.2%  | 24.64 | -9.6%  | 21.05 | -8.6%  |

**结论**：

1. **150us RTT 下四协议几乎无差距**（同日背靠背 nvmeof vs iscsi +0.2/+1.6/+1.0%；SVM vs KVM c2/c4 -8.5/-5.9% 全落在 iter 纯 CPU 阶段、KVM 那轮逐轮偏慢，属逐轮噪声）；与无 RTT 基线几乎重合（最大差 \~3%）→ 150us 对 App 无影响。
2. **KVM 完全不受 lo netem 影响**（数据面本地 virtio 不走 lo；KVM @300us 与基线 38.38/24.21/20.66 一致）→ 坐实此前 150us "KVM 变慢"是盘相位噪声。
3. **SVM pagerank 在 RTT ≥200us 确定性卡死**（200/300us 各复现：Java CPU 冻结不增长、IO 不返回、guest load 堆积），**阈值在 150\~200us 之间**；fio 探针排除 IO 层退化（psync 单延迟随 RTT 线性 96→258→360us、libaio 并发 91K IOPS 正常）——**卡死根因未定位**（按用户指令不修复）。**150us 是当前 SVM App 可用的最大 RTT 档**。

数据位置：`exp/results/app-net/{kvm,svm,nvmeof,iscsi}/logs/`（汇总 `app_net_summary.txt`）、`exp/results/app-2/{...}/logs/`（汇总 `summary.txt`）。

### 9.2 ALS-IO + movielens（每 iter 强制访存，2026-08-26 14:02–14:20，app2/als-bin-\*，四协议同日背靠背）

**合并总表（新 CSV 直读并行 build + 旧 20 iter train；total = build+train 合成值）**：

| 协议      | c1 total (build/train) | vs KVM 总比 (build/train) | c2 total (build/train) | vs KVM 总比 (build/train) | c4 total (build/train) | vs KVM 总比 (build/train) |
| ------- | ---------------------- | :------------------- | ---------------------- | :------------------- | ---------------------- | :------------------- |
| KVM     | 73.8 (28.11/45.7)      | —                    | 62.1 (16.80/45.3)      | —                    | 57.8 (11.65/46.1)      | —                    |
| SVM     | 79.7 (30.32/49.4)      | +8.0 (+7.9/+8.1)      | 69.7 (15.30/54.4)      | +12.2 (-8.9/+20.1)     | 65.8 (9.38/56.4)       | +13.8 (-19.5/+22.3)  |
| NVMe-oF | 78.8 (27.24/51.6)      | +6.8 (-3.1/+12.9)     | 67.2 (16.57/50.6)      | +8.2 (-1.4/+11.7)      | 62.3 (11.50/50.8)      | +7.8 (-1.3/+10.2)    |
| iSCSI   | 146.5 (29.47/117.0)    | +98.5 (+4.8/+156.0)   | 139.6 (21.12/118.5)    | +124.8 (+25.7/+161.6)  | 144.3 (25.58/118.7)    | +149.7 (+119.6/+157.5) |

（build = 新 CSV 直读并行 build（14:51–14:59）；train = 旧 20 iter 全量（train 段代码未改动，仍有效）；**total 为合成值非直接测量**。train 不随 cores 扩展（单线程 BLAS），total 的核扩展完全来自 build 段。）

**结论（CSV 直读 build）**：

1. **cores 区分度恢复**：CSV 直读 = IO + 解析（CPU 随 cores 扩展），KVM 28.1→16.8→11.7s（c4/c1=0.41× ≈2.4× 加速）；对比二进制零解析 build 的平线（KVM 3.0/3.1/2.6s）——**解析成本正是 cores 1/2/4 区分度的来源**，二进制 build 只剩 IO 带宽上限测不了核扩展。
2. **SVM c4 绝对时间四协议最低**（9.38s，c4/c1=0.31×），与二进制 build 的 SVM c4 反超 KVM 一致（stub 4 worker 拉满盘带宽 + 解析 4 流并行）；但 **SVM c1 单流最慢**（30.32s vs KVM 28.11s，+8%）——单流时远程路径逐块往返的延迟成本显形，与 9.4 结论（并行大块追平、串行单流落后）自洽。
3. **iSCSI 负扩展依旧显形**：c2 21.12s / c4 25.58s（c4 比 c2 还慢 21%），vs KVM c4 11.65s 差 **2.2×**——LIO 单队列在并行解析读下打爆；与 9.4（c4 26.5s，+162%）量级一致，跨 run 可复现。
4. **与 9.4 ALS-IO（同日 CSV 直读）对照**：KVM c1 28.1 vs 28.6s、iSCSI c4 25.6 vs 26.5s 吻合——CSV 直读 build 解析主导，结果不随盘相位漂移（对比二进制 IO 主导 build 对盘相位敏感）；SVM c1 今日 30.3 vs 9.4 27.5s 属盘相位/调度噪声。

数据：`exp/results/app2/als-bin-csv/{kvm,svm,nvmeof,iscsi}/logs/*.log`（CSV 直读 build-only，14:51–14:59）+ `als-bin-csv-4proto.log`（四协议汇总）；原始 CSV 已拷回双盘（`/mnt/data/ratings_big.csv`，5.4GB），`ratings_big.bin` 已从双盘删除（`gen_als_bin.py` 可随时再生）；脚本 `exp/als_train_bin.py`（`<.csv>` 输入直读原始数据，仅 build-only）。

### 9.6 xgb（合成数据集，强制重扫 IO 密集版，2026-08-26 16:44–17:04，app2/xgb-synth-6m/，四协议同日背靠背）

**先导实验**（15:08–15:16，app2/xgb-synth/，5M 行 1.4GB，DMatrix 流式 CSV 解析 + 20 rounds）：解析/计算主导——1.4GB 首次读入后全在 guest 页缓存，训练循环不碰盘，四协议收敛（KVM/SVM/nvmeof/iscsi c4 ≈ 12.7\~15.3s，SVM 仅 c4 train +21%），IO 不进关键路径，复现 pagerank 结论。→ 判定"数据集必须强制重扫才能显形 SVM 折损"。

**本版改动（目标：让 IO 回关键路径）**：

- 数据集 10M 行 2.76GB → **6M 行 1.66GB**（`head -n 6000000` 截取；10M 行解析时 `concatenate+np.save` 峰值 \~3.1GB 在 guest 3.4GB 上频繁 OOM → 收窄留余量；`xgb_parse_seg.py` 改**零拼接预分配**：pass0 C 级数行得精确 nrows + 预分配 X/y 逐 chunk 填行 + 每 chunk fadvise DONTNEED 丢页缓存，双保险）
- 重写 `exp/xgb_train.py`：xgboost 3.4.1 已移除文本流式输入（data.cc:918 移除 CSV 文本 + external memory），改 **subprocess 分片 pandas 向量化解析建 DMatrix**（**不用 multiprocessing**：guest Python 3.14 的 Pool/spawn/fork 在任务管道上死锁——worker 收不到任务、父进程 futex 空等，30MB 小文件正常、真实数据盘必现；subprocess 最底层最可靠）
- **train 每 round 强制重扫**：`scan_full` 顺序读全量 1.66GB + fadvise DONTNEED 丢页缓存 → 下 round 重新缺页走盘（同 ALS-BIN 强制访存形态），再做 1 round 内存内 xgb.train（hist/depth6/eta0.3/auc）——**16 rounds × 1.66GB = 26.6GB 每次运行从协议盘重拉**
- **坑：SVM 远程盘写回风暴卡死 stub 队列**（ALS-BIN 已知，当时用 O\_DIRECT 绕开）：build 的 npy scratch 原放数据盘 /mnt/data，`np.save` 672MB 缓冲写触发 guest 内 kworker/jbd2 **永久挂死**（>614s；读 3.44GB 全部正常、第一个写就挂）→ scratch 移到**本地系统盘** **`/home/wai/xgbparse`**（guest / 只剩 520M，先 `journalctl --vacuum-size=100M` 腾 \~700M，run\_app2.sh 已内置），远程盘全程只读 → 不再挂

**结果**（total\_s，括号 build/train；16 rounds 强制重扫；每档 host+guest drop\_caches + 128MB dd 预热）：

| 协议      | c1 total (build/train) | vs KVM c1 (build/train) | c2 total (build/train) | vs KVM c2 (build/train) | c4 total (build/train) | vs KVM c4 (build/train) |
| ------- | ---------------------- | ---------------------- | ---------------------- | ---------------------- | ---------------------- | ---------------------- |
| KVM     | 55.37 (11.24/44.13)    | —                      | 50.60 (6.81/43.79)     | —                      | 49.37 (5.04/44.34)     | —                      |
| SVM     | 60.00 (11.76/48.24)    | +8.4 (+4.6/+9.3)       | 57.66 (7.29/50.37)     | +13.9 (+7.0/+15.0)     | 57.24 (6.70/50.54)     | +15.9 (+32.9/+14.0)    |
| NVMe-oF | 58.05 (10.91/47.15)    | +4.8 (-2.9/+6.8)       | 53.67 (7.08/46.59)     | +6.1 (+4.0/+6.4)       | 51.59 (5.26/46.34)     | +4.5 (+4.4/+4.5)       |
| iSCSI   | 96.84 (13.18/83.67)    | +74.9 (+17.3/+89.6)    | 94.91 (9.21/85.70)     | +87.6 (+35.2/+95.7)    | 93.59 (9.50/84.09)     | +89.6 (+88.5/+89.6)    |

（links 全部 6000000；build 随 cores 扩展：subprocess 并行解析 11.2→5.0s（KVM）；train 协议内各 cores 基本平、四协议间明显分层。）

**结论**：

1. **强制重扫让 xgb 变真 IO 密集，四协议分层显形**：train 段 KVM 44.1s < NVMe-oF 46.6s < SVM 49.6s < iSCSI 84.5s（各 cores 中位）——26.6GB 重扫占每轮 \~40%（KVM）\~80%（iSCSI）；SVM 折损从"收敛"变为 **+8\~16%**、iSCSI **+75\~90%**。与 ALS-BIN"每 iter 强制重扫 +9\~22%"同一机制，量级自洽。
2. **SVM 折损集中在 train（远程逐轮重扫）而非 build**：build 四协议差异小（并行读+解析，SVM c1 11.8s vs KVM 11.2s）；train SVM 比 KVM 慢 4\~6s（远程 seqrd 带宽 + stub 与训练抢 CPU），c2/c4 略高于 c1。
3. **iSCSI 恒为最大短板**：带宽 \~400–530MB/s，26.6GB 重扫直接吃满——train 84s ≈ 26.6GB/320MB/s，与 dd 预热 412MB/s 量级吻合。
4. 旧 15:08 版（解析主导、IO 不进关键路径）四协议收敛 → 本版（强制重扫）分层——**IO 密集形态的关键是"每 iter 强制重扫（缺页→远程往返逐轮累加）"，数据集大小只是次因**：1.66GB 全装进页缓存也可用 fadvise 逐轮驱逐（本版 1.66GB 仍远超 guest 页缓存容量差，重扫真实走盘）。

数据：`exp/results/app2/xgb-synth-6m/logs/*.log`（16:44–17:04 背靠背）；脚本 `exp/xgb_train.py`（subprocess 解析 + 每 round scan\_full 强制重扫）、`exp/xgb_parse_seg.py`（分片解析 worker，零拼接预分配）、`exp/gen_synth_xgb.py`（10M 行生成，已截 6M）；数据 `exp/data/xgb_synth.csv`（6M 行 1.66GB，双盘 `/mnt/data/xgb_synth.csv`）；旧版数据 `exp/results/app2/xgb-synth/`（先导，15:08）。

***

## 10. 综合结论（跨实验）

1. **SVM 相对 KVM 的折损集中在"IO 密集 + 延迟敏感"负载**：纯 IO 测到唯一真实瓶颈 seqrd -36%（M2G）；TPCC 混合负载 -19\~-38%（Macro）；App pagerank 无差距（IO 太小被计算淹没）；**ALS-IO（真实 IO 密集、一次性顺序大块加载）SVM 追平 KVM**（9.4），**ALS-BIN（每 iter 强制重扫全量）SVM total +9\~23%**（9.5；折损集中在 train 段逐 iter 重扫 +9\~22%，build 阶段并行读 SVM c4/c1=0.50× 扩展反超 KVM、iSCSI c4 负扩展 1.54×；CSV 直读 build 复现：SVM c4/c1=0.31× 且 c4 绝对时间四协议最低 9.38s、iSCSI c4 负扩展 0.87× 差 KVM 2.2×）——**SVM 折损的充分条件是"延迟敏感"，顺序大块读本身不是：一次性冷读不显形，一旦每轮 iter 强制重扫（缺页 → 远程往返逐轮累加）就显形**；iSCSI 带宽瓶颈（450\~533MB/s）在各 IO 密集场景恒为最大短板（9.4 c4 +162%、9.5 全档 +158\~163%、并行 build c4 差 4.5×）。**xgb 合成数据先导版（1.4GB，解析+计算主导、IO 不进关键路径）四协议收敛、SVM 仅 c4 train +21%**（9.6，复现 pagerank 结论）；**强制重扫版（6M 行 1.66GB，16 rounds × 1.66GB 逐轮重扫）SVM total +8\~16%、iSCSI +75\~90%，train 段分层 KVM 44s < NVMe-oF 47s < SVM 50s < iSCSI 84s**（9.6）——**再次坐实"SVM 折损的充分条件 = 延迟敏感 × 每 iter 强制重扫"，与 ALS-BIN 同一机制**。多档量级自洽。
2. **吞吐统一由"在途 / RTT"决定（Little's law）**：inflight 窗口（I）是 SVM 吞吐的第一旋钮——M3 窗口减半全 RTT 域吞吐减半；batch（send）不改变在途故无净增益（M4/M4B）；高 RTT 下所有远程协议（SVM/NVMe-oF/iSCSI）收敛到同一曲线（M3 三协议）。
3. **stub recv batch（Q）是盘并行的开关（10×），local send batch（L）是二阶优化（-8%）**：Q=1 盘饿死 7K，Q≥16 解锁 55–77K；完整模型 吞吐 ≈ min(盘(min(在途,Q)), 协议(min(在途,I), slack))。
4. **merge（M5）+ buf\_pool（P）在大块上有效**：P=on 给 1M/2M +22\~35%；但 SVM 带宽占比仍随 bs 缩小（40%→13%），大块远程路径每字节成本线性放大，iodepth=1 也躲不掉。
5. **内存开销恒定 +5\~10%**（M1，iodepth=1 权威版）：在途缓冲 = 在途 × bs，非结构性问题；zc 只移拷贝不灭在途。
6. **RTT 容忍度由协议基线延迟决定**（网络存储 RTT 矩阵）：低延迟协议（NVMe-oF/SVM）对 RTT 敏感，高开销协议（iSCSI）不敏感。
7. **SVM 在 RTT ≥200us 下 App 级负载确定性卡死**，根因未定位，150us 为当前可用上限。
8. **方法学**：盘相位 ±55% + page cache 污染是历史结论错误的两大来源；所有可引用的结论均基于 8/24 后"O\_DIRECT + drop\_caches + 背靠背锚点"重做版。

***

## 11. 数据位置索引（`exp/results/`）

| 实验                 | 目录                                                                                                                                          | 脚本                                                                                                          |
| ------------------ | ------------------------------------------------------------------------------------------------------------------------------------------- | ----------------------------------------------------------------------------------------------------------- |
| M1 内存              | `m1mem/`                                                                                                                                    | `exp/run_m1mem.sh`、`exp/guest_m1mem.sh`、`exp/plot_m1mem.py`                                                 |
| M2G（权威）            | `m2g/`、`m2g/kvm_once/`                                                                                                                      | `exp/run_m2g.sh`、`exp/run_m2g_rest.sh`、`exp/host_anchor.sh`、`exp/run_kvm_m2g.sh`                            |
| M2FI               | `m2fi/m2fi_svm.jsonl`                                                                                                                       | —                                                                                                           |
| M3 RTT×窗口          | `m3rtt3/`                                                                                                                                   | `exp/run_m3rtt3.sh`                                                                                         |
| M3 三协议             | `m3net2/{nvmeof,iscsi}/`                                                                                                                    | `exp/run_m3_net2.sh <nvmeof\|iscsi>`                                                                        |
| NVMe-oF/iSCSI 早期矩阵 | `network/`                                                                                                                                  | `exp/run_m3_net.sh`                                                                                         |
| M4 batch 重做        | `m4new/`                                                                                                                                    | —                                                                                                           |
| M4B send batch     | `m4local/`                                                                                                                                  | —                                                                                                           |
| M5 merge           | `m5/`（顶层 bs.json；q1/q16/q32/q64 是 M6）                                                                                                       | `exp/run_m5.sh`、`exp/host_switch_blk_null.sh`                                                               |
| M6 recv batch      | `m5/q1~q64/`、`m5/q64i128/`                                                                                                                  | `exp/run_m5v.sh`                                                                                            |
| Macro 8/24         | `macrokvm/`、`macrosvm/`                                                                                                                     | `exp/run_macro_kvm.sh`、`exp/run_macro_svm.sh`、`exp/run_macro_svm_fix.sh`                                    |
| Macro 四协议 8/25     | `macro-b/{kvm,svm}/`、`macronet/{nvmeof,iscsi}/`                                                                                             | `exp/run_macro_b_{kvm,svm}.sh`、`exp/run_macro_net.sh <nvmeof\|iscsi>`                                       |
| App 四协议            | `app-net/{kvm,svm,nvmeof,iscsi}/`                                                                                                           | `exp/run_app_{kvm,svm,net}.sh`                                                                              |
| App @150us         | `app-2/{kvm,svm,nvmeof,iscsi}/`                                                                                                             | `exp/run_app2.sh <xgb\|als> <proto>`（新算法框架）                                                                 |
| App2 ALS           | `app2/als-{kvm,svm,nvmeof,iscsi}/`                                                                                                          | `exp/run_app2.sh als <proto>`、`exp/als_train.py`、`exp/prep_data.sh als`（ml-25m）                             |
| App2 ALS-BIN       | `app2/als-bin-{kvm,svm,nvmeof,iscsi}/`（旧 20 iter）、`app2/als-{kvm,svm,nvmeof,iscsi}/`（bin build-only）、`app2/als-bin-csv/`（CSV 直读 build-only） | `exp/als_train_bin.py`（`.csv` 输入直读原始数据仅 build-only）、`exp/gen_als_bin.py`                                    |
| App2 xgb（合成）       | `app2/xgb-synth/{kvm,svm,nvmeof,iscsi}/`                                                                                                    | `exp/run_app2.sh xgb <proto>`（`APP_DATA=/mnt/data/xgb_synth.csv`）、`exp/xgb_train.py`、`exp/gen_synth_xgb.py` |

