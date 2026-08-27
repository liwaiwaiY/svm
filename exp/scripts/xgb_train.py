#!/usr/bin/env python3
"""XGBoost 训练应用（IO 密集版，合成数据，同 HIGGS schema：无表头 29 列 = 1 标签 + 28 特征）。
xgboost 3.4.1 移除了"文本输入 + external memory"（data.cc:918），且 15M 行整表载入
DMatrix 超 guest 4GB 内存 std::bad_alloc（SparsePage+列式暂态 ~3.5GB+）。
实现（ALS-BIN 同款形态）：合成集 6M 行 1.66GB（10M 行解析时 concatenate+np.save
峰值 ~3.1GB 在 3.4GB guest 上频繁 OOM → 收窄留余量；xgb_parse_seg.py 已改零拼接
预分配，双保险）。每 round scan_full 强制重扫原始 CSV，让 IO 回关键路径：
  build ：pass1 轻量链式行对齐段边界（不整读）+ pass2 subprocess 并行拉起
          xgb_parse_seg.py 分片 pandas 向量化解析全量列 -> float32 numpy ->
          DMatrix（CPU 随 cores 扩展）。不用 multiprocessing：本 guest 上
          Python 3.14 的 Pool/spawn/fork 在任务管道上死锁（worker 收不到任务，
          父进程 futex 空等），subprocess 最底层最可靠。
  train ：每 round 先 scan_full 原始 CSV（顺序读 + fadvise DONTNEED 丢 guest
          页缓存 → 下 round 重新缺页走盘，模拟超内存/分布式每 iter 重扫全量，
          同 ALS-BIN 实证过的强制访存），再做 1 round 内存内训练（hist/depth6）。
用法: xgb_train.py <xgb_synth.csv> <cores> [nrounds=20]
输出: links=... build_s=... train_s=... total_s=...（与 run_app2.sh parse 兼容）
"""
import sys, time, os, subprocess
import numpy as np

CHUNK = 64 * 1024 * 1024   # scan_full 块读
PARSER = os.path.join(os.path.dirname(os.path.abspath(__file__)), "xgb_parse_seg.py")


def _last_nl_before(f, pos):
    """位置 pos 前（不含 pos）最后一个 '\n' 之后的位置；无则 0。回溯块扫。"""
    if pos <= 0:
        return 0
    lo = pos - 1
    while lo >= 0:
        blk_start = max(0, lo - 64 * 1024 + 1)
        f.seek(blk_start)
        data = f.read(lo - blk_start + 1)
        k = data.rfind(b"\n")
        if k >= 0:
            return blk_start + k + 1
        lo = blk_start - 1
    return 0


def aligned_segs(path, nseg):
    """链式行对齐段边界（无表头文件，首行即数据）：
    seg0 从 0 起，各段止于段内最后一个 '\n' 之后；seg_{i} 起于 seg_{i-1} 止。
    返回 [(0, e0), (e0, e1), ..., (e_{n-2}, e_{n-1})]，不重不漏、全完整行。"""
    size = os.path.getsize(path)
    stops = []
    with open(path, "rb") as f:
        for b in [size * i // nseg for i in range(1, nseg)] + [size]:
            stops.append(_last_nl_before(f, b))
    return [(0, stops[0])] + [(stops[i - 1], stops[i]) for i in range(1, nseg)]


def scan_full(path, drop=True):
    """每 iter 强制访存：顺序读全文件 + fadvise DONTNEED 丢 guest 页缓存。
    返回字节校验和（防优化掉读取）。"""
    fd = os.open(path, os.O_RDONLY)
    size = os.path.getsize(path)
    total = 0
    blk = 64 * 1024 * 1024
    off = 0
    while off < size:
        n = min(blk, size - off)
        a = np.frombuffer(os.pread(fd, n, off), dtype=np.uint8)
        total += int(a.sum())
        if drop:
            os.posix_fadvise(fd, off, n, os.POSIX_FADV_DONTNEED)
        off += n
        del a
    os.close(fd)
    return total


def main():
    path = sys.argv[1]
    cores = int(sys.argv[2])
    rounds = int(sys.argv[3]) if len(sys.argv) > 3 else 20
    t0 = time.time()

    # 训练固定单线程 BLAS（同 ALS）：SVM vCPU 竞争下多线程会饿死
    os.environ["OPENBLAS_NUM_THREADS"] = "1"
    os.environ["OMP_NUM_THREADS"] = "1"

    # ---- build：pass1 轻量对齐 + pass2 subprocess 分片 pandas 全量解析建 DMatrix ----
    nseg = max(1, min(cores, 8))
    segs = aligned_segs(path, nseg)
    print(f"[XGB] build: pass1 align {path} ({nseg} segs) ...", flush=True)
    outdir = "/home/wai/xgbparse"   # scratch 放本地系统盘：SVM 远程盘(数据盘)写回风暴会卡死
                                    # stub 队列（guest 需先 journalctl --vacuum-size 腾空间，run_app2.sh 已加）
    os.makedirs(outdir, exist_ok=True)
    for i, (a, b) in enumerate(segs):
        d = os.path.join(outdir, str(i))
        os.makedirs(d, exist_ok=True)
    print(f"[XGB] build: pass2 pandas parse x{nseg} ...", flush=True)
    procs = [
        subprocess.Popen([sys.executable, PARSER, path, str(a), str(b),
                          os.path.join(outdir, str(i))])
        for i, (a, b) in enumerate(segs)
    ]
    rcs = [p.wait() for p in procs]
    if any(r != 0 for r in rcs):
        print(f"[XGB] ERROR parse subprocess rc={rcs}", flush=True)
        sys.exit(1)
    Xs, ys, nrows = [], [], 0
    for i in range(nseg):
        d = os.path.join(outdir, str(i))
        Xs.append(np.load(os.path.join(d, "X.npy")))
        ys.append(np.load(os.path.join(d, "y.npy")))
        nrows += int(open(os.path.join(d, "n.txt")).read())
    X = np.concatenate(Xs).reshape(-1, 28).astype(np.float32)
    y = np.concatenate(ys).astype(np.float32)
    del Xs, ys
    import shutil
    shutil.rmtree(outdir, ignore_errors=True)
    print(f"[XGB] parsing done rows={nrows} X={X.shape}", flush=True)
    # subprocess 已全部退出，此刻才 import xgboost（避免与 multiprocessing 冲突）
    import xgboost as xgb
    dtrain = xgb.DMatrix(X, label=y)
    del X, y
    # build 把 CSV 读进了 guest 页缓存，这里整文件 fadvise 清掉 → 训练每 round 从盘重扫
    fd = os.open(path, os.O_RDONLY)
    os.posix_fadvise(fd, 0, os.path.getsize(path), os.POSIX_FADV_DONTNEED)
    os.close(fd)
    t_build = time.time()
    print(f"[XGB] build {t_build - t0:.2f}s", flush=True)

    # ---- train：每 round 强制重扫全量 CSV（IO 主导）+ 内存内 1 round ----
    print(f"[XGB] training ... cores={cores} rounds={rounds} (per-round full re-scan {path})", flush=True)
    params = {
        "objective": "binary:logistic",
        "max_depth": 6,
        "eta": 0.3,
        "tree_method": "hist",
        "nthread": cores,
        "eval_metric": "auc",
    }
    for r in range(rounds):
        scan_full(path)                     # 每 iter 强制访存全量（IO 主导）
        bst = xgb.train(params, dtrain, num_boost_round=1,
                        evals=[(dtrain, "train")], verbose_eval=False)
    t_train = time.time()
    total = t_train - t0
    print(f"[XGB] links={nrows} build_s={t_build - t0:.3f} train_s={t_train - t_build:.3f} "
          f"total_s={total:.3f}", flush=True)


if __name__ == "__main__":
    main()
