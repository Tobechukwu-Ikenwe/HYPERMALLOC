#!/usr/bin/env python3
"""Matched large-realloc runs; preserve each timing and its checksum."""
import argparse
import csv
import os
import statistics
import subprocess
from pathlib import Path


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--baseline", type=Path, required=True)
    parser.add_argument("--candidate", type=Path)
    parser.add_argument("--jemalloc", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--repeats", type=int, default=7)
    parser.add_argument("--operations", type=int, default=2000)
    parser.add_argument("--latency", action="store_true",
                        help="sample each realloc call and record p50/p95/p99 ns")
    args = parser.parse_args()
    variants = [("glibc", args.baseline, "libc", None),
                ("baseline", args.baseline, "hm", None)]
    if args.candidate:
        variants.append(("candidate", args.candidate, "hm", None))
    if args.jemalloc:
        variants.append(("jemalloc", args.baseline, "libc", args.jemalloc))
    rows = []
    for repeat in range(args.repeats):
        checksums = set()
        order = variants[repeat % len(variants):] + variants[:repeat % len(variants)]
        for name, binary, mode, preload in order:
            env = os.environ.copy()
            env.pop("LD_PRELOAD", None)
            if preload:
                env["LD_PRELOAD"] = str(preload.resolve())
            command = ["/usr/bin/time", "-f", "METRICS %U %S %M",
                       str(binary.resolve()), mode, str(args.operations)]
            if args.latency:
                command.append("latency")
            result = subprocess.run(
                command,
                text=True, capture_output=True, env=env, check=True)
            fields = result.stdout.split()
            elapsed, checksum = fields[:2]
            if len(fields) != (5 if args.latency else 2):
                raise RuntimeError(f"unexpected output from {name}: {result.stdout}")
            metrics = next(line.split() for line in result.stderr.splitlines()
                           if line.startswith("METRICS "))
            checksums.add(checksum)
            rows.append(dict(repeat=repeat, allocator=name,
                             operations=args.operations, seconds=float(elapsed),
                             cpu_user_s=float(metrics[1]), cpu_system_s=float(metrics[2]),
                             peak_rss_kib=int(metrics[3]), checksum=checksum,
                             realloc_p50_ns=fields[2] if args.latency else "",
                             realloc_p95_ns=fields[3] if args.latency else "",
                             realloc_p99_ns=fields[4] if args.latency else ""))
        if len(checksums) != 1:
            raise RuntimeError(f"checksum mismatch in repeat {repeat}")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with args.output.open("w", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=rows[0].keys())
        writer.writeheader()
        writer.writerows(rows)
    for name, *_ in variants:
        values = [row["seconds"] for row in rows if row["allocator"] == name]
        print(name, "median_s=", statistics.median(values),
              "range_s=", (min(values), max(values)))
    print(args.output)


if __name__ == "__main__":
    main()
