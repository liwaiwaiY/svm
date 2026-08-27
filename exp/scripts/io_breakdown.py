#!/usr/bin/env python3
"""分解 ALS-IO build 的 IO vs CPU 占比（KVM guest，本地盘最快参考）。"""
import time, subprocess, numpy as np, pandas as pd
from multiprocessing import Pool

path = "/mnt/data/ratings_big.csv"
CH = 64 * 1024 * 1024

def drop():
    subprocess.run("sync; echo 3 | sudo tee /proc/sys/vm/drop_caches >/dev/null", shell=True)

# 1. 纯读（IO 下限）
drop(); t = time.time(); n = 0
with open(path, "rb") as f:
    while True:
        d = f.read(CH)
        if not d: break
        n += len(d)
ti = time.time() - t
print(f"pure-read  : {ti:.2f}s  {n/1e9/ti:.2f} GB/s", flush=True)

# 2. pass1 式 read+count
drop(); t = time.time(); nn = 0
with open(path, "rb") as f:
    while True:
        d = f.read(CH)
        if not d: break
        nn += d.count(b"\n")
tc = time.time() - t
print(f"read+count : {tc:.2f}s  ({nn} lines)", flush=True)

# 3. pass2 式 pandas 单进程（c1 场景）
drop(); t = time.time(); tot = 0
for c in pd.read_csv(path, chunksize=8000000, usecols=[0, 1, 2],
                     dtype={0: np.int32, 1: np.int32, 2: np.float32}):
    tot += len(c)
tp = time.time() - t
print(f"pandas 1p  : {tp:.2f}s  ({tot} rows, parse-overhead~{tp - ti:.2f}s)", flush=True)

# 4. 4 进程并行 pandas 全扫（模拟 c4 的解析部分，非分片）
drop(); t = time.time()
def f(_):
    x = 0
    for c in pd.read_csv(path, chunksize=8000000, usecols=[0, 1, 2],
                         dtype={0: np.int32, 1: np.int32, 2: np.float32}):
        x += len(c)
    return x
with Pool(4) as p:
    tot = sum(p.map(f, range(4)))
t4 = time.time() - t
print(f"pandas 4p  : {t4:.2f}s  ({tot} rows, 4x full scan)", flush=True)
