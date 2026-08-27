#!/usr/bin/env python3
"""PageRank over a wiki link graph (edge list, one "src dst" pair per line).

Usage:
    spark-submit pagerank.py <edge_file> [iterations] [cores]

Reads a whitespace-separated directed edge list (SNAP wiki format), runs
iterative PageRank with damping factor 0.85, and prints per-phase timings plus
the top-10 ranked pages.  Used by the App experiment (pyspark + wiki +
pagerank) for both the KVM baseline and SVM runs.
"""
import sys
import time
from operator import add

from pyspark.sql import SparkSession


def parse_neighbors(line):
    if line.startswith('#'):
        return None
    parts = line.strip().split()
    if len(parts) < 2:
        return None
    return parts[0], parts[1]


def compute_contribs(urls, rank):
    num_urls = len(urls)
    for url in urls:
        yield (url, rank / num_urls)


if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("usage: pagerank.py <edge_file> [iterations] [cores]", file=sys.stderr)
        sys.exit(1)

    edge_file = sys.argv[1]
    iterations = int(sys.argv[2]) if len(sys.argv) > 2 else 10
    cores = int(sys.argv[3]) if len(sys.argv) > 3 else 2

    spark = (SparkSession.builder
             .appName("WikiPageRank")
             .master("local[%d]" % cores)
             .config("spark.local.dir", "/mnt/data/spark-tmp")
             .config("spark.driver.memory", "2g")
             .config("spark.shuffle.file.buffer", "64k")
             .getOrCreate())
    spark.sparkContext.setLogLevel("WARN")

    # Build the cached (url -> neighbor iterable) link graph.
    t0 = time.time()
    lines = spark.read.text(edge_file).rdd.map(lambda r: r[0])
    links = (lines.map(parse_neighbors)
             .filter(lambda x: x is not None)
             .distinct()
             .groupByKey()
             .cache())
    n_links = links.count()          # force cache / materialize
    t_build = time.time() - t0

    # Initialize ranks to 1.0 for every page.
    ranks = links.map(lambda u_neighbors: (u_neighbors[0], 1.0))

    # Iterative PageRank: contribution = rank / out-degree, damping 0.85.
    t_iters = []
    for _ in range(iterations):
        t0 = time.time()
        contribs = (links.join(ranks)
                    .flatMap(lambda u_lr: compute_contribs(u_lr[1][0], u_lr[1][1])))
        ranks = contribs.reduceByKey(add).mapValues(lambda r: r * 0.85 + 0.15)
        ranks.count()                # force the job so iteration time is real
        t_iters.append(time.time() - t0)

    t0 = time.time()
    top = ranks.sortBy(lambda x: x[1], ascending=False).take(10)
    t_sort = time.time() - t0

    total = t_build + t_sort + sum(t_iters)
    print("=== PAGERANK SUMMARY ===")
    print("edge_file=%s" % edge_file)
    print("iterations=%d cores=%d links=%d" % (iterations, cores, n_links))
    print("build_s=%.3f sort_s=%.3f" % (t_build, t_sort))
    print("iter_s=" + ",".join("%.3f" % t for t in t_iters))
    print("total_s=%.3f" % total)
    for link, rank in top:
        print("%s rank=%.6f" % (link, rank))

    spark.stop()
