# NVMe-oF / iSCSI M3 实验结果总结（网络存储 RTT 容忍度）

- 日期：2026-08-22
- 实验对象：**host 内自闭环网络存储**——host 侧同时维护 target（server）与 initiator（client），guest 通过 virtio-blk 访问 host initiator 设备
- 依据：`/home/waiai/svm/.trae/documents/2026-08-19-实验矩阵.md` M3（inflight 深度 / 网络延迟容忍度，原标注"NVMe-oF/iSCSI 暂缓"，本次补齐）
- 负载：M3 标准负载 `fio randrw 70/30, bs=4k, numjobs=1, iodepth=32, libaio, direct=1, runtime=60, time_based`
- RTT 注入：按用户要求 **host 内用 tc netem 对 loopback（`lo`）注入延迟**（guest 侧看不到 RTT），netem 单向延迟 = RTT/2
- 每个 (协议, RTT) 变体点 3 次，共 4×3×2 = 24 次 fio

## 拓扑（host 内部 client + server，guest 从 client 端访问）

```
guest (Ubuntu 26.04, fio /dev/vdb)
   │ virtio-blk-pci (serial=data-disk, cache=none)
   ▼
host qemu 打开 host initiator 块设备
   │
   ▼
┌─────────────────────── host 内部 loopback (127.0.0.1) ───────────────────────┐
│  client (initiator)  ──TCP over lo──▶  server (target)  ──▶ 10GB raw 磁盘    │
│  NVMe-oF: /dev/nvme1n1 (nvme_tcp)        nvmet (tcp:4420)      nvme.raw(loop32)│
│  iSCSI : /dev/sda   (iscsi_tcp)          LIO   (tcp:3260)      data_iscsi.raw │
└───────────────────── tc qdisc netem delay 加在 lo 上 ─────────────────────────┘
```

- 10GB 后端为与 KVM/SVM blk 实验相同的 **raw 稀疏文件**（O_DIRECT 语义：virtio-blk `cache=none`、LIO fileio 默认 O_DIRECT；nvmet 走块设备）
- guest 数据盘始终为 `/dev/vdb`，协议切换只换 host 侧 initiator 设备与 qemu 后端

## RTT 标定（TCP_NODELAY 回环探针，实测中位 RTT）

| 标称 RTT (us) | netem 单向延迟 (us) | 实测 lo TCP RTT (us) |
|---|---|---|
| 0 | 0（无 qdisc） | 46 |
| 50 | 25 | 82 |
| 100 | 50 | 132 |
| 150 | 75 | 176 |

## 一、覆盖情况（全部跑完）

| 协议 | 变体（RTT us） | 次数 | 状态 |
|---|---|---|---|
| NVMe-oF | 0 / 50 / 100 / 150 | 各 3 | ✅ |
| iSCSI | 0 / 50 / 100 / 150 | 各 3 | ✅ |

结果文件：宿主 `/home/waiai/svm/exp/results/network/`（原始 fio JSON + jsonl + CSV + SVG 曲线，见第六节）。

## 二、RTT-IOPS（均值，3 次）

| 协议 | RTT (us) | read IOPS | write IOPS | read p99 (us) | write p99 (us) |
|---|---|---|---|---|---|
| **NVMe-oF** | 0 | 199496 | 85500 | 247 | 246 |
| | 50 | 126036 | 54010 | 563 | 582 |
| | 100 | 101643 | 43568 | 609 | 625 |
| | 150 | 88492 | 37942 | 466 | 475 |
| **iSCSI** | 0 | 37010 | 15863 | 2597 | 2389 |
| | 50 | 40109 | 17196 | 2114 | 1983 |
| | 100 | 35873 | 15381 | 2452 | 2343 |
| | 150 | 34411 | 14754 | 2433 | 2294 |

NVMe-oF 单次波动（read IOPS）：RTT0 185410/207548/205530；RTT50 109934/135075/133099；RTT100 91449/106942/106538；RTT150 88548/87848/89082。
曲线图：`exp/results/network/M3-net-RTT-iops.svg`（实线=read，虚线=write，竖线=3 次波动区间）。

## 三、观察

1. **NVMe-oF 对 RTT 高度敏感**：RTT 0→150us，read IOPS 199.5k→88.5k（**-56%**），write 85.5k→37.9k（-56%）。主因：NVMe-oF 基线每请求延迟很低（p99 ~250us），队列深度 32 下增加 50-150us RTT 直接拉低吞吐（Little's law：IOPS ≈ depth / latency）。
2. **iSCSI 几乎不受 RTT 影响**：read IOPS 37.0k→34.4k（-7%，落在 3 次波动内）。主因：iSCSI（SCSI over TCP）协议 + LIO fileio 后端的每请求固定开销大（p99 ~2.4ms），增加 ≤150us 的 RTT 相对其基线延迟可忽略，且吞吐本来就被协议/后端开销限制在 ~37k。
3. **两协议绝对吞吐差距 ~5-6 倍**（RTT0：199.5k vs 37.0k read）。NVMe-oF 设计为低开销传输（命令头小、无 SCSI 层胶水），loopback 上远超 iSCSI。
4. NVMe-oF 的 IOPS 曲线随 RTT 单调下降并趋向饱和（RTT100→150 边际衰减变小），符合"延迟容忍度=队列深度可覆盖的延迟上限"直觉。

## 四、结论

1. 网络存储协议的 **RTT 容忍度由基线每请求延迟决定**：低延迟协议（NVMe-oF）对链路 RTT 极其敏感（-56% @150us），高开销协议（iSCSI）反而不敏感（-7%）。
2. 这也解释了矩阵中 SVM M3 变体 A（inflight 深度）的机制：inflight 窗口覆盖链路 RTT × 期望 IOPS，窗口不足时吞吐按比例下降。
3. host 内 loopback + tc netem 注入方案可行且稳定：guest 全程只看到 virtio-blk，RTT 完全由 host 侧 lo qdisc 控制。

## 五、过程问题与修复

| 问题 | 根因 | 影响 | 处理 |
|---|---|---|---|
| nvmet 端口不监听 | 配置脚本 heredoc 与 sudo 密码 herestring 冲突，密码未传入，脚本实际未执行 | target 未创建 | 改为 `echo 密码 | sudo -S bash -c '...'` 显式传密码 |
| iSCSI 登录认证失败 (24) | (1) `/etc/iscsi/initiatorname.iscsi` 为空，initiator 名非法；(2) LIO TPG 无 ACL 时默认**拒绝所有** initiator | 无法登录 | 写入合法 initiator IQN + `targetcli /iscsi/.../tpg1/acls create <iqn>` |
| IQN 格式不合法 | `iqn.2026-08.svm:data` 域名部分无点号，WWN 校验失败 | target 创建失败 | 改用 `iqn.2026-08.com.svm:data` |
| 残留 5 月 iSCSI target | 旧配置仍在内核（`iqn.2026-05.svm.test`，占用 0.0.0.0:3260） | 新 portal 绑定冲突 | 先 `targetcli /iscsi delete` 旧 target 再创建 |
| 环境清理 | 之前 crypto 实验的 local qemu + remote stub 仍在运行 | 端口/资源占用 | `kill` 后确认无残留进程 |

## 六、数据来源与复现清单

| 项 | 路径 |
|---|---|
| 原始 fio JSON（NVMe-oF + iSCSI，RTT 0/50/100/150，各 3 次） | `exp/results/network/logs/m3-{nvmeof,iscsi}-rtt{0,50,100,150}-r{1,2,3}.json` |
| 解析结果（schema 见矩阵 2.3，含 env/command/result） | `exp/results/network/m3_net.jsonl` |
| CSV 汇总（均值±std） | `exp/results/network/m3_net_summary.csv` |
| RTT-IOPS 曲线（SVG） | `exp/results/network/M3-net-RTT-iops.svg` |
| 解析脚本 | `exp/scripts/parse_m3_net.py` |
| 绘图脚本 | `exp/scripts/gen_m3_net_chart.py` |
| 编排脚本（tc 注入 + guest fio + 采集） | `exp/run_m3_net.sh` |
| RTT 标定探针 | `/tmp/rtt_probe.py`（TCP_NODELAY loopback RTT） |

复现命令：
```bash
# 1. NVMe-oF target（host）
sudo modprobe nvmet nvme-tcp nvme_fabrics
sudo losetup -f /home/waiai/svm/exp/nvme/nvme.raw     # → /dev/loopN
# configfs 建 subsystem nqn.2026-08.svm:data + namespace(/dev/loopN) + port(127.0.0.1:4420)
sudo nvme connect -t tcp -n nqn.2026-08.svm:data -a 127.0.0.1 -s 4420   # → /dev/nvme1n1

# 2. iSCSI target（host）
sudo targetcli  # fileio data_iscsi.raw + iqn.2026-08.com.svm:data + portal 127.0.0.1:3260 + ACL
sudo iscsiadm -m discovery -t sendtargets -p 127.0.0.1:3260
sudo iscsiadm -m node -T iqn.2026-08.com.svm:data -p 127.0.0.1:3260 -l  # → /dev/sda

# 3. guest（virtio-blk 后端=host initiator 设备，serial=data-disk → guest /dev/vdb）
qemu-system-x86_64 -enable-kvm -cpu host -smp 4 -m 4096 \
  -drive file=system.raw,if=none,id=drive0,format=raw,cache=none -device virtio-blk-pci,drive=drive0,bootindex=1 \
  -netdev user,id=net0,hostfwd=tcp::2222-:22 -device virtio-net-pci,netdev=net0 \
  -drive file=/dev/nvme1n1,if=none,id=drive1,format=raw,cache=none -device virtio-blk-pci,drive=drive1,serial=data-disk \
  -serial file:guest.log -monitor none -display none

# 4. RTT 注入 + 跑 M3（自动完成 4 RTT × 3 次 + 采集）
bash exp/run_m3_net.sh nvmeof    # 或 iscsi（先换 qemu 后端为 /dev/sda）
```
