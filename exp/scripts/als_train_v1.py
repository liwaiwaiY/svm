#!/usr/bin/env python3
"""ALS 训练应用（自实现显式矩阵分解，numpy/scipy BLAS，无 implicit 依赖）。
数据文件 5.4GB 超 guest 4GB 内存：全量流式读 2 遍（真实 IO），采样构建 CSR。
cores 参数控制 pass 阶段的多进程并行分片读（IO 并行度，对 IO 密集应用有真实区分度）；
训练阶段固定单线程 BLAS（SVM vCPU 竞争下 OpenBLAS 多线程会饿死/死锁，KVM 正常）。
每迭代写 checkpoint 到数据盘（模拟分布式中间结果交换，制造写 IO；写 V 330MB 单次不触发脏页风暴）。
用法: als_train.py <ratings_big.csv> <cores> [iters=20] [factors=128]
输出: links=... build_s=... train_s=... total_s=...
"""
import sys, time, random, os

def seg_bounds(path, nseg):
    """返回各段 [start,end) 字节区间；每段 start 对齐到行首。"""
    size = os.path.getsize(path)
    bounds = []
    for seg in range(nseg):
        start = seg * (size // nseg)
        end = size if seg == nseg - 1 else (seg + 1) * (size // nseg)
        bounds.append((start, end))
    return bounds

def scan_seg(args):
    """读 [start,end) 段：mode=count 数行；mode=sample 随机采样解析。返回 (n 或 (rows,cols,vals,max_u,max_i))"""
    path, start, end, mode, keep, seed = args
    n = 0
    rows, cols, vals = [], [], []
    mx_u = mx_i = 0
    rng = random.Random(seed)
    with open(path, "rb") as f:
        if start > 0:
            f.seek(start)
            f.readline()  # 丢弃半行，落到行首
        else:
            f.readline()  # seg 0 跳过表头行
        f.seek(f.tell())
        while True:
            pos = f.tell()
            if pos >= end:
                break
            line = f.readline()
            if not line:
                break
            if mode == "count":
                n += 1
            else:
                if rng.random() > keep:
                    continue
                p = line.split(b",")
                if len(p) < 3:
                    continue
                u = int(p[0]); i = int(p[1]); v = float(p[2])
                if u > mx_u: mx_u = u
                if i > mx_i: mx_i = i
                rows.append(u); cols.append(i); vals.append(v)
    if mode == "count":
        return n
    # 转 numpy 再回传（python 列表 pickle 慢 6×）
    import numpy as np
    return (np.array(rows, dtype=np.int32), np.array(cols, dtype=np.int32),
            np.array(vals, dtype=np.float32), mx_u, mx_i)

def save_v_direct(path, V):
    """O_DIRECT 同步写 V 到盘：绕开 guest 页缓存回写（SVM 远程盘上 330MB 写回风暴会把
    virtio-blk 队列卡死、训练挂起数分钟）；直接落盘制造真实的写 IO，写完成才返回。
    mmap 匿名页对齐缓冲满足 O_DIRECT 对齐要求。"""
    import mmap
    import numpy as np
    data = np.ascontiguousarray(V, dtype=np.float32).tobytes()
    n = len(data)
    chunk = 8 * 1024 * 1024
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_TRUNC | os.O_DIRECT)
    try:
        pos = 0
        while pos < n:
            blk = min(chunk, n - pos)          # 本块实际数据字节
            pad = (-blk) % 4096                # pad 到 4K 倍数
            with mmap.mmap(-1, blk + pad) as m:
                m[:blk] = data[pos:pos + blk]
                os.write(fd, m)
            pos += blk
    finally:
        os.close(fd)

def main():
    path = sys.argv[1]
    cores = int(sys.argv[2])
    iters = int(sys.argv[3]) if len(sys.argv) > 3 else 20
    factors = int(sys.argv[4]) if len(sys.argv) > 4 else 128
    t0 = time.time()

    # 训练固定单线程 BLAS：SVM vCPU 竞争下 OpenBLAS 多线程会饿死/死锁（KVM 正常）
    os.environ["OPENBLAS_NUM_THREADS"] = "1"
    os.environ["OMP_NUM_THREADS"] = "1"

    import multiprocessing as mp
    import numpy as np
    from scipy.sparse import csr_matrix

    nseg = max(1, min(cores, 8))
    bounds = seg_bounds(path, nseg)
    print(f"[ALS] pass1: parallel scan {path} ({nseg} segs) ...", flush=True)

    # ---- pass1：全量并行分片读，统计行数（真实 IO：扫 5.4GB）----
    with mp.Pool(nseg) as pool:
        counts = pool.map(scan_seg, [(path, s, e, "count", 0, 0) for s, e in bounds])
    nnz = sum(counts)
    if nnz == 0:
        print("ERROR: empty ratings"); sys.exit(1)
    print(f"[ALS] total rows={nnz}", flush=True)

    # ---- pass2：并行分片采样 ~500 万，统计维度 ----
    SAMPLE = 5_000_000
    keep = min(1.0, SAMPLE / nnz)
    with mp.Pool(nseg) as pool:
        res = pool.map(scan_seg, [(path, s, e, "sample", keep, 42 + i) for i, (s, e) in enumerate(bounds)])
    rows, cols, vals, max_u, max_i = [], [], [], 0, 0
    for r in res:
        rows += r[0].tolist(); cols += r[1].tolist(); vals += r[2].tolist()
        if r[3] > max_u: max_u = r[3]
        if r[4] > max_i: max_i = r[4]
    n_users, n_items = max_u + 1, max_i + 1
    n_sampled = len(rows)
    print(f"[ALS] sampled={n_sampled} users={n_users} items={n_items}", flush=True)

    # ---- build：COO -> CSR ----
    R = csr_matrix((np.array(vals, dtype=np.float32),
                    (np.array(rows, dtype=np.int32),
                     np.array(cols, dtype=np.int32))),
                   shape=(n_users, n_items)).tocsr()
    del rows, cols, vals
    t_build = time.time()
    print(f"[ALS] build done {t_build - t0:.2f}s", flush=True)

    # ---- train：交替最小二乘 R ~= U @ V.T（单线程 BLAS）----
    lam = 0.1
    rng2 = np.random.default_rng(42)
    U = rng2.standard_normal((n_users, factors), dtype=np.float32) * 0.01
    V = rng2.standard_normal((n_items, factors), dtype=np.float32) * 0.01
    ckpt_dir = "/mnt/data/als_ckpt"
    os.makedirs(ckpt_dir, exist_ok=True)
    print(f"[ALS] training ... cores={cores} iters={iters} factors={factors}", flush=True)
    for it in range(iters):
        # 固定 V 解 U: U = R V (V^T V + λI)^-1
        VtV = V.T @ V
        A_u = VtV + lam * np.eye(factors, dtype=np.float32)
        U = R @ V @ np.linalg.inv(A_u)
        # 固定 U 解 V: V = R^T U (U^T U + λI)^-1
        UtU = U.T @ U
        A_v = UtU + lam * np.eye(factors, dtype=np.float32)
        V = R.T @ U @ np.linalg.inv(A_v)
        # checkpoint 覆盖写盘（模拟分布式中间结果交换，制造写 IO）
        # 每 5 iter 一次（20 iter 共 4 次 × 330MB = 1.3GB 写）；O_DIRECT 直落盘，
        # 避免 page cache 回写在 SVM 远程盘上形成写回风暴卡死 virtio-blk
        if (it + 1) % 5 == 0:
            save_v_direct(f"{ckpt_dir}/V_latest.npy", V)
        if (it + 1) % 2 == 0 or it + 1 == iters:
            print(f"[ALS]   iter {it+1}/{iters} done", flush=True)
    t_train = time.time()
    print(f"[ALS] links={nnz} build_s={t_build - t0:.3f} train_s={t_train - t_build:.3f} "
          f"total_s={t_train - t0:.3f}", flush=True)

if __name__ == "__main__":
    main()
