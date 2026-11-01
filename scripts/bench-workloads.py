#!/usr/bin/env python3
"""Run broader matched allocator workloads and retain every raw observation."""
import argparse
import csv
import os
import statistics
import subprocess
from pathlib import Path

CASES = {"small": 50000, "medium": 20000, "large": 500,
         "zero": 50000, "threshold": 500, "batch": 30000,
         "holes": 30000, "aligned": 1000, "records": 20000}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--baseline", type=Path, required=True)
    parser.add_argument("--candidate", type=Path, required=True)
    parser.add_argument("--jemalloc", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--repeats", type=int, default=5)
    parser.add_argument("--workload", action="append", choices=CASES)
    args = parser.parse_args()
    variants = [("glibc", args.baseline, "libc", None),
                ("baseline", args.baseline, "hm", None),
                ("final", args.candidate, "hm", None)]
    if args.jemalloc:
        variants.append(("jemalloc", args.baseline, "libc", args.jemalloc))
    rows = []
    for case in args.workload or CASES:
        operations = CASES[case]
        for repeat in range(args.repeats):
            order = variants[repeat % len(variants):] + variants[:repeat % len(variants)]
            checksums = set()
            for name, binary, mode, preload in order:
                env = os.environ.copy()
                env.pop("LD_PRELOAD", None)
                if preload:
                    env["LD_PRELOAD"] = str(preload.resolve())
                result = subprocess.run(
                    ["/usr/bin/time", "-f", "METRICS %U %S %M",
                     str(binary.resolve()), mode, case, str(operations)],
                    capture_output=True, text=True, env=env, check=True)
                elapsed, checksum, mapped, peak, peak_live, fragmentation = (
                    result.stdout.split())
                metrics = next(line.split() for line in result.stderr.splitlines()
                               if line.startswith("METRICS "))
                checksums.add(checksum)
                rows.append(dict(workload=case, operations=operations,
                                 repeat=repeat, allocator=name,
                                 seconds=float(elapsed), checksum=checksum,
                                 mapped_after_bytes=int(mapped),
                                 peak_mapped_bytes=int(peak),
                                 peak_live_bytes=int(peak_live),
                                 internal_fragmentation_after_bytes=int(fragmentation),
                                 cpu_user_s=float(metrics[1]),
                                 cpu_system_s=float(metrics[2]),
                                 peak_rss_kib=int(metrics[3])))
            if len(checksums) != 1:
                raise RuntimeError(f"checksum mismatch: {case}/{repeat}")
        print(case, {name: round(statistics.median(
            row["seconds"] for row in rows
            if row["workload"] == case and row["allocator"] == name), 6)
            for name, *_ in variants}, flush=True)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with args.output.open("w", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=rows[0].keys())
        writer.writeheader()
        writer.writerows(rows)
    print(args.output)


if __name__ == "__main__":
    main()
