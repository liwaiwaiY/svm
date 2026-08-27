# blk M1(mem) + M5(merge) 实验总结（2026-08-25）

> 日期：2026-08-25（18:40-19:08 本会话完成）
> 环境：SVM（local qemu + remote stub，null_blk /dev/nullb0），KVM 对照，4 vCPU，W=4，B=16
> 负载：fio randrw 7:3、direct=1、libaio、1 job
> 前置文档：[2026-08-22-blk-svm实验结果总结.md](./2026-08-22-blk-svm实验结果总结.md)（M2/M3/M4/M4B/M6）

---

## 0. 两个实验的共性方法

- **M1（内存）**：host 侧直接观察进程——`/proc/<pid>/status` VmRSS（local qemu + remote stub 各自）+ `ss -m` 的 `skmem:(r=rmem_alloc,w=wmem_queued)` 累加。fio 启动时 guest 写 `BS_START`/`ALL_DONE` 到 `/tmp/m15/progress.log`，host 轮询同步后每 0.5s 采一点，共 60 点（30s 窗口）。
- **M5（合并）**：新增 vr.conf knob `stub_merge_m`（字节阈值）——>0 且请求 in 总字节数 ≤ 阈值时，把**一个请求（elem）内部的多个 4K in_sg 合并成一个连续大缓冲**（in_num 折叠为 1），设备做单次大 aio、响应为单个 iov；阈值 0 = 保持逐 sg 原行为。响应 wire 格式只有 `[vq_nr][elem_index][data_len]`，不含 in_num，故协议不变、local 端无需改动。

---

## 1. M1 内存研究（m1mem）

### 1.1 配置
- 矩阵：bs{64K,512K,1M,2M} × {SVM zc=0（全 zc）, SVM zc=4M（4M 以下走 copy）, KVM}
- I=256、P=off、**iodepth=1**（M1 微基准，避免在途堆积干扰；此前 iodepth=64 的数据作废，理由见结论）
- 结果口径：virtio-remote 总占用 = local RSS + stub RSS + skmem（skmem 从 B 换算成 kB）

### 1.2 结果（mean，kB；total = local+stub+skmem，括号内为相对 KVM 增量）

| bs | cell | local | stub | total | vs KVM |
|---|---|---|---|---|---|
| 64K | SVM zc=0 | 881357 | 34326 | 915721 | **+7.3%** |
| 64K | SVM zc=4M(copy) | 907998 | 34029 | 942051 | +10.4% |
| 64K | KVM | 853256 | — | 853256 | — |
| 512K | SVM zc=0 | 899083 | 35745 | 935011 | +6.7% |
| 512K | SVM zc=4M(copy) | 918365 | 36009 | 954489 | +9.0% |
| 512K | KVM | 875783 | — | 875783 | — |
| 1M | SVM zc=0 | 885613 | 37530 | 923415 | +5.1% |
| 1M | SVM zc=4M(copy) | 907761 | 38969 | 946998 | +7.8% |
| 1M | KVM | 878215 | — | 878215 | — |
| 2M | SVM zc=0 | 913590 | 47139 | 961156 | +8.9% |
| 2M | SVM zc=4M(copy) | 905434 | 48166 | 954106 | +8.1% |
| 2M | KVM | 882431 | — | 882431 | — |

skmem（内核 socket 缓冲，中位数 B）：64K zc 13248 vs copy 1064；512K 26496 vs 2909；1M 158528 vs 134778；2M 383552 vs 277731。**zc 恒高于 copy**（页 pin 使 wmem 与 rmem 重叠窗口更长），但都是 KB 级。

### 1.3 关键结论

1. **之前 iodepth=64 的"+14%→+81% 随 bs 增长"是在途堆积的假象**。iodepth=1 重跑后：stub 恒定 34-48MB（不再随 bs 涨）、skmem 降到 KB 级、**SVM 相对 KVM 恒定 +5~10%**，与 bs 基本无关。SVM 的额外内存 = 远端路径上的在途缓冲 ≈ 在途数 × bs，iodepth 一降就消失。
2. **zc vs copy 结论反转**：iodepth=64 时"64K 走 zc 更费内存"（skmem 高、stub 高）；iodepth=1 下 zc 的 local 反而比 copy 低 2-3%（64K~1M，zc 不分配拷贝缓冲），2M 打平（±1% 噪声内）。之前的小块 zc 吃亏是堆积场景下页 pin 叠加的产物。
3. **机制**：virtio-remote 数据必须完整穿过 local→TCP→stub 全路径，在途数据占内存不可避免；zc 只是把"拷贝"从 local 挪走（省 local），同时转加 stub 排队缓冲与 skmem（pin 重叠）。要真正降内存只能降在途（牺牲吞吐）或减 bs。
4. 注意：iodepth=1 下各 cell 数值都很小，跨 cell 的 local RSS 基线波动就有 ±5%（853-882K），% 数字只作量级参考；同 bs 内 zc/copy 是背靠背跑的，相对可比。

### 1.4 位置

- 脚本：`exp/run_m1mem.sh`（iodepth=1、FORCE=1 强制重跑）、`exp/guest_m1mem.sh`（fio 侧）、`exp/plot_m1mem.py`（绘图）
- 图：`exp/results/m1mem/m1mem_bar_all.png`（4 面板堆叠柱，local 蓝底+stub 橙顶）、`exp/results/m1mem/m1mem_pct_kvm.png`（8 根柱，相对 KVM %）
- 数据：`exp/results/m1mem/`（`<tag>.samples` 60 点原始采样、`<tag>.baseline`、`<tag>.bs.json`、`points.csv`、`m1mem_summary.txt`）

---

## 2. M5 stub in_sg merge（大块同步读写）

### 2.1 新 knob

- vr.conf 新增 `stub_merge_m`（字节阈值，0=关闭）。`host_switch_blk_null.sh` 增加第 4 参 `M`（默认 0），其他实验不受影响。
- 代码（`local_qemu/hw/virtio-remote/virtio-remote.c`）：声明 L489-491；默认 L527；解析 L577-578；合并逻辑在 `stub_recv_req` stage1（L3843-3882）：`in_num>1 && in_total<=stub_merge_m` 时 `buf_pool_alloc(in_total)` 单缓冲 + `rs->in_num=1`。释放路径（stub_recv_state_reset / stub_resp_free / push dead 分支）按 in_num 计数，合并后均为 1，无泄漏。

### 2.2 配置与矩阵

- SVM：I=256（可正常启动）、**iodepth=1（同步，无排队）**、Z=0（响应全走 zc）、**M=4M**（合并）、W=4、B=16
- 矩阵：bs{16K,64K,256K,1M,2M} × {SVM P=off, SVM P=on, KVM}
- 指标：带宽（KiB/s）+ 每请求延迟 1/IOPS（µs）

### 2.3 结果

带宽（KiB/s，randrw 7:3 iodepth=1）：

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

### 2.4 关键结论

1. **SVM 相对 KVM 的带宽占比随 bs 缩小（40%→13%）**：bs 16K→2M（128×），KVM 单请求延迟只涨 4.5×（19.8→88.5µs），SVM P=off 涨 16.8×（48.7→818µs）、P=on 涨 13.5×。远程路径的每字节成本（local→TCP→stub 的拷贝/传输/aio）在大块上线性放大，**即使 iodepth=1 同步模式也躲不掉**。
2. **P=on（buf_pool）在大块上有明显收益**：1M +35%、2M +22%、256K +11%；16K/64K 反而略差（-2~3%，噪声级）。机制：merge 让每个请求变成一个连续大缓冲（256K/512 页），P=off 时每请求 posix_memalign+free 大块（堆抖动），P=on 时按页数回收复用。
3. **已知缺口**：矩阵内无 M=0（不合并）对照，merge 本身的收益尚未单独量化（P 的收益是叠加在 merge 之上的）。如需拆分，补 M=0 × {P=off 1M/2M, P=on 2M} 即可（每档 ~45s）。

### 2.5 位置

- 脚本：`exp/run_m5.sh`（全矩阵自动跑 + 汇总）、`exp/host_switch_blk_null.sh <Z> <I> <P> [<M>]`、`exp/guest_m1mem.sh`（复用）
- 数据：`exp/results/m5/<tag>.bs.json`（tag = svm_poff_* / svm_pon_* / kvm_*，15 组）。**注意：同目录下 `q1/q16/q32/q64` 子目录是 8/24 的 M6 recv batch 数据，勿混淆**
- 图：暂无（当前以表格对比）
- 代码：`local_qemu/hw/virtio-remote/virtio-remote.c`（`stub_merge_m`）

---

## 3. 备注 / 待办

- `exp/results/m5/` 与 `exp/results/m1m5/`（今天 14:24-15:18 的旧 M1/M5 大块扫描，无 merge knob）编号并存，引用时注意区分。
- 建议后续：①补 M5 的 M=0 对照；②如需正式汇报可出带宽/延迟随 bs 的折线图。
