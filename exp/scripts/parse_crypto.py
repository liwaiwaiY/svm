#!/usr/bin/env python3
"""parse_crypto.py <log> <dev> <exp> <variant> <env_json>
Parse openssl speed afalg aes-128-cbc tables from a bench log into jsonl lines
(appended to exp_results/crypto/<exp>.jsonl). One object per run, result holds
the blocksize->MBps table plus the raw ops/s values.
"""
import json
import re
import sys

log_path, dev, exp, variant, env_json = sys.argv[1:6]
env = json.loads(env_json) if env_json else {}
blk_sizes = [16, 64, 256, 1024, 8192, 16384]
runs = []
speed_re = re.compile(r"^aes-128-cbc\s+(.*)$")
ops_re = re.compile(r"^(?:Doing )?aes-128-cbc ops for \d+s on (\d+) size blocks: (\d+)")
current_ops = {}
for line in open(log_path, encoding="utf-8", errors="replace"):
    m = speed_re.match(line.strip())
    if m:
        vals = [float(x.rstrip("k")) for x in m.group(1).split()]
        if len(vals) == 6:
            runs.append({"ops_s": dict(current_ops), "kbps": dict(zip(blk_sizes, vals))})
        continue
    m = ops_re.match(line.strip())
    if m:
        current_ops[int(m.group(1))] = int(m.group(2))

out = "/home/waiai/svm/exp/results/crypto/%s.jsonl" % exp
with open(out, "a", encoding="utf-8") as f:
    for i, r in enumerate(runs, 1):
        rec = {
            "dev": dev,
            "exp": exp,
            "variant": variant,
            "env": env,
            "command": "openssl speed -engine afalg -elapsed aes-128-cbc",
            "run": i,
            "result": {
                bs: round(r["kbps"][bs] / 1000.0, 3) for bs in blk_sizes
            },
            "ops_s": r["ops_s"],
        }
        f.write(json.dumps(rec) + "\n")
print("appended %d runs -> %s" % (len(runs), out))
