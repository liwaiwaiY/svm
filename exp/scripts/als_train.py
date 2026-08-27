#!/usr/bin/env python3
"""ALS 训练应用（IO 密集版，9.4 ALS-IO）：pandas C 解析器向量化读 CSV。
数据 5.4GB 超 guest 4GB 内存：pass1 多进程 C 级块扫数行（纯 IO，bytes.count）；
pass2 多进程 pandas read_csv 分块解析采样（C 向量化，解析不再是瓶颈，IO 主导）。
cores = pass 阶段多进程分片读并行度（IO 并行度区分度）；训练固定单线程 BLAS
（SVM vCPU 竞争下 OpenBLAS 多线程会饿死/死锁）。
每 5 iter 写 O_DIRECT checkpoint 到数据盘（制造真实写 IO，绕开 page cache 写回风暴）。
用法: als_train.py <ratings_big.csv> <cores> [iters=20] [factors=128]
输出: links=... build_s=... train_s=... total_s=...
"""
import sys, time, os, io

CHUNK = 64 * 1024 * 1024   # pass1 块读（C 级 count）
CS = 8 * 1024 * 1024       # pass2 read_csv chunksize（行）


def seg_bounds(path, nseg):
    """按字节均分的段边界 [start,end)。"""
    size = os.path.getsize(path)
    return [(i * (size // nseg), size if i == nseg - 1 else (i + 1) * (size // nseg))
            for i in range(nseg)]


def count_seg(args):
    """C 级块扫 [start,end) 段：返回 (段内完整行数, 段首对齐偏移, 段末最后完整行尾偏移)。
    段 0 丢弃表头行；非 0 段丢弃 start 前的半行（seek+readline 对齐行首）。
    stop 为段内最后一个完整行尾（\n 之后），供 pass2 链式边界使用，保证不重不漏。"""
    path, start, end = args
    n = 0
    with open(path, "rb") as f:
        f.seek(start)
        f.readline()                     # 丢弃半行（或表头）
        pos = f.tell()                   # 对齐行首 / 表头行尾
        start_aligned = pos
        last_nl_end = pos
        while pos < end:
            data = f.read(CHUNK)
            if not data:
                break
            if pos + len(data) > end:
                data = data[:end - pos]
                k = data.rfind(b"\n")
                if k >= 0:
                    n += data[:k + 1].count(b"\n")
                    last_nl_end = pos + k + 1
                break
            n += data.count(b"\n")
            last_nl_end = pos + len(data)
            pos = f.tell()
    return n, start_aligned, last_nl_end


class LimitedReader(io.RawIOBase):
    """只暴露 [start,end) 字节的只读视图（行对齐区间）；pandas C 解析器读到 end 精确停止。"""

    def __init__(self, fh, start, end):
        fh.seek(start)
        self.fh, self.end = fh, end

    def readable(self):
        return True

    def seekable(self):
        return False

    def read(self, n=-1):
        rem = self.end - self.fh.tell()
        if rem <= 0:
            return b""
        if n < 0 or n > rem:
            n = rem
        return self.fh.read(n)

    def readinto(self, b):
        rem = self.end - self.fh.tell()
        if rem <= 0:
            return 0
        if len(b) > rem:
            b = memoryview(b)[:rem]
        return self.fh.readinto(b)


def sample_seg(args):
    """pandas 向量化解析 [start,end) 段（行对齐），按 keep 随机采样前三列。"""
    path, start, end, keep, seed = args
    import numpy as np
    import pandas as pd
    rng = np.random.default_rng(seed)
    fh = open(path, "rb")
    lr = LimitedReader(fh, start, end)
    uu, ii, vv = [], [], []
    n = 0
    try:
        for chunk in pd.read_csv(lr, chunksize=CS, usecols=[0, 1, 2],
                                 dtype={0: np.int32, 1: np.int32, 2: np.float32},
                                 header=None):
            n += len(chunk)
            m = rng.random(len(chunk)) < keep
            c = chunk[m]
            if len(c):
                uu.append(c[0].to_numpy(np.int32))
                ii.append(c[1].to_numpy(np.int32))
                vv.append(c[2].to_numpy(np.float32))
    finally:
        fh.close()
    u = np.concatenate(uu) if uu else np.empty(0, np.int32)
    i = np.concatenate(ii) if ii else np.empty(0, np.int32)
    v = np.concatenate(vv) if vv else np.empty(0, np.float32)
    return u, i, v, n


def save_v_direct(path, V):
    """O_DIRECT 同步写 V 到盘：绕开 guest 页缓存回写（SVM 远程盘上 330MB 写回风暴会把
    virtio-blk 队列卡死、训练挂起数分钟）；直接落盘制造真实的写 IO，写完成才返回。"""
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
    print(f"[ALS-IO] pass1: parallel C-scan {path} ({nseg} segs) ...", flush=True)

    # ---- pass1：多进程 C 级块扫数行 + 收集行对齐段边界（纯 IO）----
    with mp.Pool(nseg) as pool:
        res = pool.map(count_seg, [(path, s, e) for s, e in bounds])
    counts = [r[0] for r in res]
    starts = [r[1] for r in res]
    stops = [r[2] for r in res]
    nnz = sum(counts)
    if nnz == 0:
        print("ERROR: empty ratings")
        sys.exit(1)
    # 链式行对齐段边界：段 0 从表头行尾起，段 i>0 从上一段末完整行尾起（不重不漏）
    segs = [(starts[0], stops[0])] + [(stops[i - 1], stops[i]) for i in range(1, nseg)]
    print(f"[ALS-IO] total rows={nnz} segs={[(a, b) for a, b in segs]}", flush=True)

    SAMPLE = 5_000_000
    keep = min(1.0, SAMPLE / nnz)
    print(f"[ALS-IO] pass2: parallel pandas parse (keep={keep:.4f}) ...", flush=True)

    # ---- pass2：多进程 pandas C 解析分块采样（IO 主导）----
    with mp.Pool(nseg) as pool:
        res2 = pool.map(sample_seg, [(path, a, b, keep, 42 + i) for i, (a, b) in enumerate(segs)])
    # 用 pass2 链式边界的实际解析行数作为精确 links（pass1 按字节边界会丢跨边界行）
    nnz = sum(r[3] for r in res2)
    rows = np.concatenate([r[0] for r in res2])
    cols = np.concatenate([r[1] for r in res2])
    vals = np.concatenate([r[2] for r in res2])
    # ml-25m ×8 放大（prep_data.sh：off_u=k*162542, off_i=k*62424, k=0..7）
    # 全文件上界（2026-08-26 实测）：userId 1..1300335, movieId 1..646139
    # （ml-25m 原始 movieId 稀疏编号最大 209171；采样未必覆盖全 id，故用固定 shape）
    n_users, n_items = 1300336, 646140
    n_sampled = len(rows)
    print(f"[ALS-IO] sampled={n_sampled} users={n_users} items={n_items}", flush=True)

    # ---- build：COO -> CSR ----
    R = csr_matrix((vals, (rows, cols)), shape=(n_users, n_items)).tocsr()
    del rows, cols, vals
    t_build = time.time()
    print(f"[ALS-IO] build done {t_build - t0:.2f}s", flush=True)

    # ---- train：交替最小二乘 R ~= U @ V.T（单线程 BLAS）----
    lam = 0.1
    rng2 = np.random.default_rng(42)
    U = rng2.standard_normal((n_users, factors), dtype=np.float32) * 0.01
    V = rng2.standard_normal((n_items, factors), dtype=np.float32) * 0.01
    ckpt_dir = "/mnt/data/als_ckpt"
    os.makedirs(ckpt_dir, exist_ok=True)
    print(f"[ALS-IO] training ... cores={cores} iters={iters} factors={factors}", flush=True)
    for it in range(iters):
        # 固定 V 解 U: U = R V (V^T V + λI)^-1
        VtV = V.T @ V
        A_u = VtV + lam * np.eye(factors, dtype=np.float32)
        U = R @ V @ np.linalg.inv(A_u)
        # 固定 U 解 V: V = R^T U (U^T U + λI)^-1
        UtU = U.T @ U
        A_v = UtU + lam * np.eye(factors, dtype=np.float32)
        V = R.T @ U @ np.linalg.inv(A_v)
        # checkpoint 覆盖写盘（模拟分布式中间结果交换，制造写 IO）；O_DIRECT 直落盘
        if (it + 1) % 5 == 0:
            save_v_direct(f"{ckpt_dir}/V_latest.npy", V)
        if (it + 1) % 2 == 0 or it + 1 == iters:
            print(f"[ALS-IO]   iter {it+1}/{iters} done", flush=True)
    t_train = time.time()
    print(f"[ALS-IO] links={nnz} build_s={t_build - t0:.3f} train_s={t_train - t_build:.3f} "
          f"total_s={t_train - t0:.3f}", flush=True)


if __name__ == "__main__":
    main()
