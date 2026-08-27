#!/usr/bin/env python3
"""xgb parse_seg helper（subprocess 版）：解析 [start,end) 行对齐段 -> X.npy / y.npy / n.txt。
由 xgb_train.py 通过 subprocess 并行拉起（规避本 guest 上 Python 3.14 multiprocessing
Pool/spawn/fork 在任务管道上的死锁：worker 收不到任务，父进程 futex 空等）。
内存控制（guest 3.4GB；10M 行时 concatenate+np.save 峰值 ~3.1GB 被 OOM，故数据收窄到
6M 行 + 本文件零拼接预分配）：
  pass0 count_rows：C 级 64MB 块扫 '\n'（纯 IO ~1-2s）得精确 nrows（末行无换行 +1）；
  pass1 预分配 X(nrows,28)/y(nrows) 逐 chunk 填行（无 concatenate 峰值）；
        np.save 写 contiguous 数组（tofile，无整份 tobytes 拷贝）；
  每 chunk 解析完即 fadvise DONTNEED 丢已消费 CSV 页缓存（CS=500K ≈ 138MB 文本）。
用法: xgb_parse_seg.py <csv> <start> <end> <out_dir>
"""
import sys, os, io
import numpy as np

CS = 500_000   # read_csv chunksize（行）
BLK = 64 * 1024 * 1024


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


def count_rows(fh, start, end):
    """段内 '\n' 数；段末字节非 '\n' 则 +1（全文件最后一行无换行符时）。"""
    n = 0
    fh.seek(start)
    pos = start
    while pos < end:
        nxt = min(pos + BLK, end)
        data = fh.read(nxt - pos)
        n += int(np.count_nonzero(np.frombuffer(data, dtype=np.uint8) == 10))
        os.posix_fadvise(fh.fileno(), pos, nxt - pos, os.POSIX_FADV_DONTNEED)
        pos = nxt
    fh.seek(end - 1)
    if fh.read(1) != b"\n":
        n += 1
    return n


def main():
    path, start, end, out = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), sys.argv[4]
    import pandas as pd
    fh = open(path, "rb")
    nrows = count_rows(fh, start, end)
    # count 已逐块丢页缓存；整段再丢一次，parse 从冷缓存重新缺页（防峰值叠加）
    os.posix_fadvise(fh.fileno(), start, end - start, os.POSIX_FADV_DONTNEED)
    lr = LimitedReader(fh, start, end)
    X = np.empty((nrows, 28), np.float32)
    y = np.empty(nrows, np.float32)
    r0, pos = 0, start
    try:
        for chunk in pd.read_csv(lr, chunksize=CS, header=None, dtype=np.float32):
            n = len(chunk)
            X[r0:r0 + n] = chunk.iloc[:, 1:].to_numpy(np.float32)   # 28 特征
            y[r0:r0 + n] = chunk.iloc[:, 0].to_numpy(np.float32)    # 标签列
            r0 += n
            p2 = fh.tell()
            if p2 > pos:
                os.posix_fadvise(fh.fileno(), pos, p2 - pos, os.POSIX_FADV_DONTNEED)
                pos = p2
    finally:
        fh.close()
    if r0 != nrows:
        sys.stderr.write(f"WARN rows parsed {r0} != count {nrows}\n")
    np.save(f"{out}/X.npy", X)
    np.save(f"{out}/y.npy", y)
    with open(f"{out}/n.txt", "w") as f:
        f.write(str(r0))
    del X, y


if __name__ == "__main__":
    main()
