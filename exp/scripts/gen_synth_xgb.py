#!/usr/bin/env python3
"""gen_synth_xgb.py <N> [out.csv]
合成 xgboost 数据集（替代 7.5GB HIGGS——真实 HIGGS 需下载、训练太慢）。
schema 与 HIGGS 完全一致：无表头，29 列 = 1 标签(col 0, 0/1) + 28 特征(float)，
xgb_train.py（label_column=0）直接可用。
label = (f0 + 0.5*f1 + 0.3*f2 + 噪声 > 0)，非纯噪声，训练有真实信号。
rng seed 固定 42；分块生成控制峰值内存。默认 N=5_000_000（~1.4GB）。
用法: gen_synth_xgb.py [N] [out]
"""
import sys, os, time
import numpy as np

def main():
    N = int(sys.argv[1]) if len(sys.argv) > 1 else 5_000_000
    out = sys.argv[2] if len(sys.argv) > 2 else "xgb_synth.csv"
    rng = np.random.default_rng(42)
    CH = 500_000
    t0 = time.time()
    done = 0
    with open(out, "w") as f:
        while done < N:
            n = min(CH, N - done)
            X = rng.standard_normal((n, 28))
            y = (X[:, 0] + 0.5 * X[:, 1] + 0.3 * X[:, 2] + rng.standard_normal(n) > 0).astype(np.int32)
            a = np.hstack([y.reshape(-1, 1), X])
            np.savetxt(f, a, fmt="%.6f", delimiter=",")
            done += n
            if done % 1_000_000 == 0 or done == N:
                print(f"  {done/1e6:.1f}M rows, {time.time()-t0:.0f}s", flush=True)
    print(f"done: {done} rows, {os.path.getsize(out)/1e9:.2f}GB, {time.time()-t0:.0f}s")

if __name__ == "__main__":
    main()
