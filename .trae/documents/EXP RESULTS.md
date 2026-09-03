## M1. zero copy

目的是验证zero copy的有效性。微基准测试的fio在blk设备上的性能。检测的是local side和remote side的内存开销。所以fio的配置是iodepth=1，模拟同步；bs={64K,512K,1M}，观察mem变化。

实验figure数据，按照 (Mosaic-Local - KVM-local) / (Mosaic-local + Mosaic-remote - KVM-local - KVM-remote)计算local和remote分别增加的开销。因为是采样的，所以最终按照平均值比较，但标注采样过程的最大值。最终的数据为：

\| bs | cell | local | remote | total| max |
\| 64K | zc | 3.3 | 4.0 | 7.3 | 7.4 |
\| 64K | copy | 6.4 | 4.0 | 10.4 | 10.5 |
\| 512K | zc | 2.7 | 4.1 | 6.8 | 7.0 |
\| 512K | copy | 4.9 | 4.1 | 9.0 | 9.1 |
\| 1m | zc | 0.8 | 4.3 | 5.1 | 5.4 |
\| 1m | copy | 3.4 | 4.4 | 7.8 | 8.1 |

## M1. worker pool

目的是验证event驱动的 vq 隔离的有效性。微基准测试的fio在blk设备上的iops。fio配置为numjobs=4，触发多队列。

实验figure按照fio提供的iops进行绘制，最终数据为：

\| W | randrd | randwt | seqrd | seqwt |
\| 1 | 154 | 40 | 89 | 68 |
\| 2 | 176 | 74 | 136 | 87 |
\| 4 | 190 | 94 | 148 | 97 |
\| KVM | 199 | 96 | 220 | 122 |

## M3. inflight

目的是验证local端的send和recv之间的inflight 窗口的有效性。微基准测试fio在blk的性能，负载时randrw 70/30、bs=4K、iodepth=32、1job。调整RTT，观察iops效果。

最终数据为：

| RTT   | I16 | I32 | NVMe-oF | iSCSI | KVM   |
| ----- | --- | --- | :------ | :---- | ----- |
| 0.1ms | 30K | 54K | 61K     | 48K   | 65.1K |
| 0.2ms | 25K | 50K | 57K     | 44K   | 65.1K |
| 0.5ms | 16K | 37K | 40K     | 34K   | 65.1K |
| 1ms   | 9K  | 27K | 30K     | 23K   | 65.1K |
| 2ms   | 6K  | 14K | 15K     | 14K   | 65.1K |

## M5. buffer pool

remote side会重建request提供的sg table，如果反复在heap分配/释放，会有额外开销，我们用buffer pool实现userspace的缓冲区自管理。调整每个request的请求大小来验证性能。

实验figure数据选自每请求延时，换算 ( Mosaic-P=on - Mosaic-P=off ) / KVM.

\| req | value |
\| 16K | -2.3 |
\| 64K | -3.6 |
\| 256K| +10.3 |
\| 1M | +25.8 |
\| 2M | +17.9 |

## M6. remote batch

remote端会将recv和device invoke拆分开，充分利用device本身的批性能。当remote发现device处于handle状态，就会跳过invoke，把req放到lock-free queue中，继续recv。等待device idle，一并把queue的req上交给device处理。从而实现自适应的批处理，即下一批的数量，取决于上一批执行过程中接收的请求数。

实验figure的数据通过测试fio的来，调整iodepth观察并发数对于queue side的影响。最终按照iops进行绘制。数据为：

\| iodepth | Q=1 | Q=16 | Q=32 | Q=64 | KVM |
\| 1       | 5.5K | 5.5K      | 5.5K      | 5.5K          | 7.47K  |
\| 16      | 7.0K | 55.6K | 55.4K     | 55.7K         | 65.3K  |
\| 32      | 7.1K | 66.7K     | 77.0K | 77.2K         | 88.5K  |
\| 64      | 7.2K | 71.2K     | 82.8K     | 92.2K     | 101.4K |

## Macro sysbench tpc-c

跑的是mysql数据库，用sysbench跑tpc-c标准的基准测试。有两个折线图，p95的延迟和txn。数据如下：

| 协议      | threads=1 | threads=2 | threads=4 |
| ------- | --------- | --------- | --------- |
| KV      | 240.0     | 298.7     | 446.3     |
| Mosaic  | 185.6     | 252.2     | 248.5     |
| NVMe-oF | 158.3     | 221.5     | 273.8     |
| iSCSI   | 109.3     | 171.6     | 249.5     |

| 协议      | threads=1 | threads=2 | threads=4 |
| ------- | --------- | --------- | --------- |
| KVM     | 8.13      | 13.70     | 20.74     |
| Mosaic  | 10.65     | 17.63     | 44.17     |
| NVMe-oF | 13.22     | 20.74     | 37.56     |
| iSCSI   | 30.26     | 38.94     | 45.79     |

## Application

### PySpark + PageRank + wiki dump

利用pyspark跑pagerank算法，数据集来自wiki dump出来的1 million的数据。

| 协议      | c1   | c2   | c4   |
| ------- | ---- | ---- | ---- |
| KVM     | 38.2 | 24.2 | 20.9 |
| SVM     | 39.2 | 24.9 | 21.7 |
| NVMe-oF | 38.3 | 24.6 | 21.1 |
| iSCSI   | 39.6 | 27.3 | 23.0 |

### ALS + movielens

跑ALS算法，跑的数据集是movielens，200M行数据，大小为5.4GB，每次tier强制扫数据。

| 协议      | c1 total (build/train) | c2 total (build/train) | c4 total (build/train) |
| :------ | :--------------------- | :--------------------- | :--------------------- |
| KVM     | 73.8 (27.2/46.6)      | 62.1 (14.8/47.3)      | 57.8 (11.5/46.3)      |
| SVM     | 79.7 (30.3/49.4)      | 69.7 (15.3/54.4)      | 65.8 (18.4/47.4)       |
| NVMe-oF | 78.8 (28.1/50.7)      | 67.2 (16.6/50.6)      | 62.3 (11.6/50.7)      |
| iSCSI   | 146.5 (30.2/116.3)    | 139.6 (21.1/118.5)    | 144.3 (25.58/118.7)    |

### XGBoost + Higgs-sampled

跑xgboost算法，数据集是采样过后的higgs，大小为1.66GB，跑16轮。每轮都强制访盘。

| 协议      | c1 total (build/train) | c2 total (build/train) | c4 total (build/train) |
| :------ | :--------------------- | :--------------------- | :--------------------- |
| KVM     | 55.37 (10.24/45.13)    | 50.60 (6.81/43.79)     | 49.37 (5.04/44.34)     |
| SVM     | 60.00 (11.76/48.24)    | 57.66 (7.29/50.37)     | 57.24 (6.70/50.54)     |
| NVMe-oF | 58.05 (10.91/47.15)    | 53.67 (7.08/46.59)     | 51.59 (5.26/46.34)     |
| iSCSI   | 96.84 (13.18/83.67)    | 94.91 (9.21/85.70)     | 93.59 (9.50/84.09)     |

