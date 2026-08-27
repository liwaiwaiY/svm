#!/usr/bin/env python3
"""Parse M3 network experiment fio JSON logs into jsonl + summary CSV."""
import json, glob, os, sys, csv, re

RES = "/home/waiai/svm/exp/results/network"
LOG = os.path.join(RES, "logs")
OUT_JSONL = os.path.join(RES, "m3_net.jsonl")
OUT_CSV = os.path.join(RES, "m3_net_summary.csv")

# nominal RTT label -> measured loopback TCP RTT (from rtt_probe calibration)
RTT_MEASURED = {0: 46, 50: 82, 100: 132, 150: 176}

rows = []
with open(OUT_JSONL, "w") as jf:
    for f in sorted(glob.glob(os.path.join(LOG, "m3-*.json"))):
        base = os.path.basename(f)
        # m3-<proto>-rtt<RTT>-r<RUN>.json
        m = re.match(r"m3-(\w+)-rtt(\d+)-r(\d+)\.json", base)
        proto, rtt, run = m.group(1), int(m.group(2)), int(m.group(3))
        d = json.load(open(f))
        j = d["jobs"][0]
        rec = {
            "dev": "blk",
            "exp": "M3",
            "variant": f"{proto}-RTT{rtt}",
            "env": {"netem_delay_us": rtt // 2, "rtt_us_measured": RTT_MEASURED[rtt]},
            "command": "fio --name=M3 --filename=/dev/vdb --rw=randrw --rwmixread=70 --bs=4k --numjobs=1 --iodepth=32 --ioengine=libaio --direct=1 --runtime=60 --time_based --group_reporting --output-format=json",
            "proto": proto,
            "rtt_us": rtt,
            "run": run,
            "read_iops": j["read"]["iops"],
            "write_iops": j["write"]["iops"],
            "read_bw_kib": j["read"]["bw"],
            "write_bw_kib": j["write"]["bw"],
            "read_p50_us": j["read"]["clat_ns"]["percentile"]["50.000000"] / 1e3,
            "read_p99_us": j["read"]["clat_ns"]["percentile"]["99.000000"] / 1e3,
            "write_p50_us": j["write"]["clat_ns"]["percentile"]["50.000000"] / 1e3,
            "write_p99_us": j["write"]["clat_ns"]["percentile"]["99.000000"] / 1e3,
            "read_clat_mean_us": j["read"]["clat_ns"]["mean"] / 1e3,
            "write_clat_mean_us": j["write"]["clat_ns"]["mean"] / 1e3,
            "result": (f"read_iops={j['read']['iops']:.1f},write_iops={j['write']['iops']:.1f},"
                       f"read_p50_us={j['read']['clat_ns']['percentile']['50.000000']/1e3:.1f},"
                       f"read_p99_us={j['read']['clat_ns']['percentile']['99.000000']/1e3:.1f},"
                       f"write_p50_us={j['write']['clat_ns']['percentile']['50.000000']/1e3:.1f},"
                       f"write_p99_us={j['write']['clat_ns']['percentile']['99.000000']/1e3:.1f}"),
        }
        jf.write(json.dumps(rec, ensure_ascii=False) + "\n")
        rows.append(rec)

# CSV summary: mean over runs per (proto, rtt)
with open(OUT_CSV, "w", newline="") as cf:
    w = csv.writer(cf)
    w.writerow(["proto", "rtt_us", "netem_delay_us", "rtt_measured_us",
                "read_iops_avg", "read_iops_std", "write_iops_avg", "write_iops_std",
                "read_p99_us_avg", "write_p99_us_avg"])
    for proto in ["nvmeof", "iscsi"]:
        for rtt in [0, 50, 100, 150]:
            sub = [r for r in rows if r["proto"] == proto and r["rtt_us"] == rtt]
            if not sub:
                continue
            def avg(k):
                return sum(x[k] for x in sub) / len(sub)
            def std(k):
                m = avg(k)
                return (sum((x[k] - m) ** 2 for x in sub) / len(sub)) ** 0.5
            w.writerow([proto, rtt, rtt // 2, RTT_MEASURED[rtt],
                        round(avg("read_iops"), 1), round(std("read_iops"), 1),
                        round(avg("write_iops"), 1), round(std("write_iops"), 1),
                        round(avg("read_p99_us"), 1), round(avg("write_p99_us"), 1)])
print("wrote", OUT_JSONL, "entries:", len(rows))
print(open(OUT_CSV).read())
