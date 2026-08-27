#!/usr/bin/env python3
"""gen_als_bin.py <ratings_big.csv> <out.bin>
CSV（200M 行，5.7GB，ml-25m 的 8 倍）→ 紧凑二进制（200M 条 int32,int32,float32，
12B/条，2.4GB）。行数零放大（只做格式转换），供 ALS-BIN 零解析加载；
train 每 iter 用 posix_fadvise(DONTNEED) 强制访存（丢 guest 页缓存 → 下 iter 从
远端盘重拉全量，不扩大数据集，10.1 节）。
全文件上界（实测）：max_u=1300335, max_i=646139 → shape (1300336, 646140)。
"""
import sys, time
import numpy as np
import pandas as pd

def main():
    src, dst = sys.argv[1], sys.argv[2]
    CS = 8 * 1024 * 1024        # read_csv chunksize（行）
    t0 = time.time()
    n = 0
    row = np.dtype([("u", "<i4"), ("i", "<i4"), ("v", "<f4")])
    with open(dst, "wb") as out:
        for chunk in pd.read_csv(src, chunksize=CS, usecols=[0, 1, 2],
                                 dtype={0: np.int32, 1: np.int32, 2: np.float32},
                                 header=None, skiprows=1):
            u = chunk[0].to_numpy(np.int32)
            i = chunk[1].to_numpy(np.int32)
            v = chunk[2].to_numpy(np.float32)
            a = np.empty(len(u), dtype=row)
            a["u"] = u
            a["i"] = i
            a["v"] = v
            out.write(a.tobytes())          # 每条 12B 交错，不能 np.stack（会提 dtype）
            n += len(u)
            if n % 50_000_000 == 0:
                print(f"  {n/1e6:.0f}M recs", flush=True)
    # 校验 max（memmap 结构化 dtype，不驻留内存）
    size = n * 12
    arr = np.memmap(dst, dtype=row, mode="r")
    mx_u, mx_i = int(arr["u"].max()), int(arr["i"].max())
    del arr
    print(f"  done: {n} recs, {size/1e9:.2f}GB, max_u={mx_u} max_i={mx_i} "
          f"(expect 1300335 / 646139), {time.time()-t0:.0f}s")

if __name__ == "__main__":
    main()
