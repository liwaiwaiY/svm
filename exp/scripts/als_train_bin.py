#!/usr/bin/env python3
"""ALS 训练应用（真 IO 密集版，10.1 ALS-BIN）：紧凑二进制 + 每 iter 强制访存全量重扫。
数据 ratings_big.bin（200M 条 int32,int32,float32，12B/条，2.4GB，CSV 直转零放大）。
build：多进程分片顺序读全文件（cores=分片流数；max + 随机采样 500 万建 CSR，零解析）。
CSV 输入（build-only，ITERS=0）：直读原始 ratings_big.csv（5.4GB，200M 行）——pass1
多进程 C 级块扫数行（纯 IO）+ pass2 多进程 pandas 向量化解析采样（CPU 随 cores 扩展，
cores 1/2/4 有真实区分度）；训练需先 gen_als_bin.py 转二进制。
train：每 iter 全量重扫 —— 顺序读 + 每块读完 posix_fadvise(POSIX_FADV_DONTNEED)
丢 guest 页缓存，下 iter 重新缺页走盘（强制访存，不扩大数据集；等价每轮迭代从
远端盘冷缓存重拉全量数据，模拟分布式 ALS 每轮迭代扫全量文件的形态）。
再做 1 次交替最小二乘（单线程 BLAS）。
每 5 iter O_DIRECT 写 V checkpoint（绕开 page cache 写回风暴）。
用法: als_train_bin.py <ratings_big.bin> <cores> [iters=20] [factors=128]
输出: links=... build_s=... train_s=... total_s=...
"""
import sys, time, os, io, mmap
import numpy as np

REC = 12  # 每条 12B: int32 u + int32 i + float32 v
BLOCK = 8_000_000  # 条/块（96MB，页对齐）
DT = np.dtype([("u", "<i4"), ("i", "<i4"), ("v", "<f4")])
CHUNK = 64 * 1024 * 1024   # CSV pass1 块读（C 级 count）
CS = 8 * 1024 * 1024       # CSV pass2 read_csv chunksize（行）


def seg_bounds(size, nseg, rec):
    """按 REC 对齐的字节段边界 [start,end)，供多进程分片读。"""
    if nseg <= 1:
        return [(0, size)]
    base = (size // nseg) // rec * rec
    bounds = []
    start = 0
    for k in range(nseg):
        end = size if k == nseg - 1 else start + base
        bounds.append((start, end))
        start = end
    return bounds


def scan_seg(args):
    """多进程 worker：扫描 [start,end) 字节段，返回 (max_u, max_i, sampled_u, sampled_i, sampled_v)。
    每段独立 rng（seed 按段序），keep 全文件一致 → 总采样 ≈ SAMPLE。"""
    path, start, end, keep, seed = args
    rng = np.random.default_rng(seed)
    fd = os.open(path, os.O_RDONLY)
    os.lseek(fd, start, os.SEEK_SET)   # 定位到段起点（fd 默认从 0 读，必须 seek）
    mx_u = mx_i = 0
    uu_l, ii_l, vv_l = [], [], []
    off = start
    while off < end:
        blk = min(BLOCK * REC, end - off)
        data = os.read(fd, blk)
        a = np.frombuffer(data, dtype=DT)
        u = a["u"]
        i = a["i"]
        v = a["v"]
        mu, mi = int(u.max()), int(i.max())
        if mu > mx_u:
            mx_u = mu
        if mi > mx_i:
            mx_i = mi
        m = rng.random(len(a)) < keep
        if m.any():
            uu_l.append(u[m])
            ii_l.append(i[m])
            vv_l.append(v[m])
        off += blk
        del a, u, i, v, data
    os.close(fd)
    u = np.concatenate(uu_l) if uu_l else np.empty(0, np.int32)
    i = np.concatenate(ii_l) if ii_l else np.empty(0, np.int32)
    v = np.concatenate(vv_l) if vv_l else np.empty(0, np.float32)
    return mx_u, mx_i, u, i, v


def save_v_direct(path, V):
    """O_DIRECT 同步写 V 到盘：绕开 guest 页缓存回写（SVM 远程盘上 330MB 写回风暴会把
    virtio-blk 队列卡死、训练挂起数分钟）；直接落盘制造真实的写 IO，写完成才返回。"""
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


def scan_full(path, dt, drop=False):
    """全量扫描：返回 sum(v) + max_u + max_i。drop=True 时每块读完后
    posix_fadvise(DONTNEED) 丢 guest 页缓存 → 下 iter 重新缺页走盘（强制访存）。
    不用 mmap+madvise(DONTNEED)：guest 内核（7.0.0-30）对只读 MAP_SHARED 文件映射
    返回 EINVAL；fadvise 只丢页缓存、不依赖映射类型，语义等价。"""
    fd = os.open(path, os.O_RDONLY)
    size = os.fstat(fd).st_size
    total = 0.0
    mx_u = mx_i = 0
    off = 0
    while off < size:
        blk = min(BLOCK * REC, size - off)
        data = os.read(fd, blk)                  # 整块读（走 page cache，零解析）
        a = np.frombuffer(data, dtype=dt)        # bytes → 只读结构化视图
        total += float(a["v"].sum())
        uu, ii = int(a["u"].max()), int(a["i"].max())
        if uu > mx_u:
            mx_u = uu
        if ii > mx_i:
            mx_i = ii
        if drop:
            os.posix_fadvise(fd, off, blk, os.POSIX_FADV_DONTNEED)
        off += blk
        del a, data
    os.close(fd)
    return total, mx_u, mx_i


# ---- CSV（原始数据）build-only 路径：pass1 C 级数行 + pass2 pandas 向量化解析采样 ----
# 5.4GB 超 guest 4GB 内存，分片段内流式处理；解析成本随 cores 扩展（1/2/4 区分度）。


def seg_bounds_csv(path, nseg):
    """按字节均分的段边界 [start,end)。"""
    size = os.path.getsize(path)
    return [(i * (size // nseg), size if i == nseg - 1 else (i + 1) * (size // nseg))
            for i in range(nseg)]


def count_seg(args):
    """C 级块扫 [start,end) 段：返回 (段内完整行数, 段首对齐偏移, 段末最后完整行尾偏移)。
    段 0 丢弃表头行；非 0 段丢弃 start 前的半行（seek+readline 对齐行首）。
    stop 为段内最后一个完整行尾（\\n 之后），供 pass2 链式边界使用，保证不重不漏。"""
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
    """pandas 向量化解析 [start,end) 段（行对齐），按 keep 随机采样前三列 + 全段 max。"""
    import pandas as pd
    path, start, end, keep, seed = args
    rng = np.random.default_rng(seed)
    fh = open(path, "rb")
    lr = LimitedReader(fh, start, end)
    uu, ii, vv = [], [], []
    mx_u = mx_i = 0
    n = 0
    try:
        for chunk in pd.read_csv(lr, chunksize=CS, usecols=[0, 1, 2],
                                 dtype={0: np.int32, 1: np.int32, 2: np.float32},
                                 header=None):
            n += len(chunk)
            c0 = chunk[0].to_numpy(np.int32)
            c1 = chunk[1].to_numpy(np.int32)
            c2v = chunk[2].to_numpy(np.float32)
            m0, m1 = int(c0.max()), int(c1.max())
            if m0 > mx_u:
                mx_u = m0
            if m1 > mx_i:
                mx_i = m1
            m = rng.random(len(chunk)) < keep
            if m.any():
                uu.append(c0[m])
                ii.append(c1[m])
                vv.append(c2v[m])
    finally:
        fh.close()
    u = np.concatenate(uu) if uu else np.empty(0, np.int32)
    i = np.concatenate(ii) if ii else np.empty(0, np.int32)
    v = np.concatenate(vv) if vv else np.empty(0, np.float32)
    return u, i, v, n, mx_u, mx_i


def build_csv(path, cores):
    """CSV build-only：pass1 多进程 C 级块扫数行（纯 IO）+ pass2 多进程 pandas
    解析采样（CPU 随 cores 扩展）。返回 (nnz, mx_u, mx_i, n_sampled, R)。"""
    import multiprocessing as mp
    from scipy.sparse import csr_matrix
    nseg = max(1, min(cores, 8))
    bounds = seg_bounds_csv(path, nseg)
    print(f"[ALS-BIN] build(csv): pass1 C-scan {path} ({nseg} segs) ...", flush=True)
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
    print(f"[ALS-BIN] total rows={nnz}", flush=True)
    SAMPLE = 5_000_000
    keep = min(1.0, SAMPLE / nnz)
    print(f"[ALS-BIN] build(csv): pass2 pandas parse (keep={keep:.4f}) ...", flush=True)
    with mp.Pool(nseg) as pool:
        res2 = pool.map(sample_seg, [(path, a, b, keep, 42 + i) for i, (a, b) in enumerate(segs)])
    # pass2 链式边界实际解析行数作为精确 links（pass1 按字节边界会丢跨边界行）
    nnz = sum(r[3] for r in res2)
    rows = np.concatenate([r[0] for r in res2])
    cols = np.concatenate([r[1] for r in res2])
    vals = np.concatenate([r[2] for r in res2])
    mx_u = max(r[4] for r in res2)
    mx_i = max(r[5] for r in res2)
    n_users, n_items = mx_u + 1, mx_i + 1
    n_sampled = len(rows)
    print(f"[ALS-BIN] max_u={mx_u} max_i={mx_i} sampled={n_sampled} "
          f"users={n_users} items={n_items}", flush=True)
    R = csr_matrix((vals, (rows, cols)), shape=(n_users, n_items)).tocsr()
    del rows, cols, vals
    return nnz, mx_u, mx_i, n_sampled, R


def main():
    path = sys.argv[1]
    cores = int(sys.argv[2])
    iters = int(sys.argv[3]) if len(sys.argv) > 3 else 20
    factors = int(sys.argv[4]) if len(sys.argv) > 4 else 128
    t0 = time.time()

    # 训练固定单线程 BLAS：SVM vCPU 竞争下 OpenBLAS 多线程会饿死/死锁（KVM 正常）
    os.environ["OPENBLAS_NUM_THREADS"] = "1"
    os.environ["OMP_NUM_THREADS"] = "1"

    from scipy.sparse import csr_matrix
    np.seterr(over="ignore", invalid="ignore")   # 采样归约含 float32 溢出提示，忽略

    dt = DT
    # ---- build：CSV 直读原始数据（pass1+pass2，CPU 随 cores 扩展）或二进制零解析分片读 ----
    if path.endswith(".csv"):
        if iters > 0:
            print("ERROR: CSV 输入仅支持 build-only（ITERS=0）；训练请先 gen_als_bin.py 转 bin")
            sys.exit(1)
        nnz, mx_u, mx_i, n_sampled, R = build_csv(path, cores)
        del R
        t_build = time.time()
        t_train = t_build
        print(f"[ALS-BIN] build done {t_build - t0:.2f}s", flush=True)
        print(f"[ALS-BIN] links={nnz} build_s={t_build - t0:.3f} train_s={t_train - t_build:.3f} "
              f"total_s={t_train - t0:.3f}", flush=True)
        return
    size = os.path.getsize(path)
    assert size % REC == 0, f"bad bin size {size} % {REC} != 0"
    nnz = size // REC
    print(f"[ALS-BIN] build: scan {path} ({nnz/1e6:.0f}M recs, {size/1e9:.2f}GB) ...", flush=True)

    # ---- build：多进程分片读全量扫描（cores=分片流数，max + 随机采样 500 万建 CSR，零解析）----
    SAMPLE = 5_000_000
    keep = min(1.0, SAMPLE / nnz)
    import multiprocessing as mp
    segs = seg_bounds(size, cores, REC)
    with mp.Pool(cores) as pool:
        res = pool.map(scan_seg, [(path, s, e, keep, 42 + r) for r, (s, e) in enumerate(segs)])
    mx_u = max(r[0] for r in res)
    mx_i = max(r[1] for r in res)
    rows = np.concatenate([r[2] for r in res])
    cols = np.concatenate([r[3] for r in res])
    vals = np.concatenate([r[4] for r in res])
    # build 后丢页缓存 → iter1 也冷读
    fd = os.open(path, os.O_RDONLY)
    os.posix_fadvise(fd, 0, size, os.POSIX_FADV_DONTNEED)
    os.close(fd)
    n_users, n_items = mx_u + 1, mx_i + 1
    n_sampled = len(rows)
    print(f"[ALS-BIN] max_u={mx_u} max_i={mx_i} sampled={n_sampled} "
          f"users={n_users} items={n_items}", flush=True)

    R = csr_matrix((vals, (rows, cols)), shape=(n_users, n_items)).tocsr()
    del rows, cols, vals
    t_build = time.time()
    print(f"[ALS-BIN] build done {t_build - t0:.2f}s", flush=True)

    # ---- train：每 iter 强制访存全量重扫（IO）+ 1 次交替最小二乘（单线程 BLAS）----
    lam = 0.1
    rng2 = np.random.default_rng(42)
    U = rng2.standard_normal((n_users, factors), dtype=np.float32) * 0.01
    V = rng2.standard_normal((n_items, factors), dtype=np.float32) * 0.01
    ckpt_dir = "/mnt/data/als_ckpt"
    os.makedirs(ckpt_dir, exist_ok=True)
    print(f"[ALS-BIN] training ... cores={cores} iters={iters} factors={factors}", flush=True)
    for it in range(iters):
        # 强制访存全量重扫：posix_fadvise(DONTNEED) 丢页，2.4GB 必然走盘
        scan_sum, _, _ = scan_full(path, dt, drop=True)
        # 固定 V 解 U: U = R V (V^T V + λI)^-1（先算小矩阵 V@invA 降低峰值内存）
        VtV = V.T @ V
        A_u = VtV + lam * np.eye(factors, dtype=np.float32)
        U = R @ (V @ np.linalg.inv(A_u))
        # 固定 U 解 V: V = R^T U (U^T U + λI)^-1
        UtU = U.T @ U
        A_v = UtU + lam * np.eye(factors, dtype=np.float32)
        V = R.T @ (U @ np.linalg.inv(A_v))
        # checkpoint 覆盖写盘（模拟分布式中间结果交换，制造写 IO）；O_DIRECT 直落盘
        if (it + 1) % 5 == 0:
            save_v_direct(f"{ckpt_dir}/V_latest.npy", V)
        if (it + 1) % 2 == 0 or it + 1 == iters:
            print(f"[ALS-BIN]   iter {it+1}/{iters} done (scan_sum={scan_sum:.0f})", flush=True)
    t_train = time.time()
    print(f"[ALS-BIN] links={nnz} build_s={t_build - t0:.3f} train_s={t_train - t_build:.3f} "
          f"total_s={t_train - t0:.3f}", flush=True)


if __name__ == "__main__":
    main()
