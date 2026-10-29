#!/usr/bin/env python3
"""Run matched allocator workloads and write raw results plus a median plot."""
import csv
import os
import platform
import statistics
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
PROGRAM = ROOT / "out/release/bench_compare"
RESULTS = ROOT / "benchmarks/results"
WORKLOADS = {"small": 100000, "mixed": 100000, "large": 20000}
REPEATS = 7


def run(allocator, workload, operations):
    output = subprocess.check_output(
        [str(PROGRAM), allocator, workload, str(operations)], text=True
    )
    elapsed, checksum = output.split()
    return float(elapsed), checksum


def main():
    subprocess.run(["cmake", "--preset", "release"], cwd=ROOT, check=True)
    subprocess.run(["cmake", "--build", "out/release", "-j", "4"],
                   cwd=ROOT, check=True)
    commit = subprocess.check_output(
        ["git", "rev-parse", "--short", "HEAD"], cwd=ROOT, text=True
    ).strip()
    cpu = next((line.split(":", 1)[1].strip()
                for line in Path("/proc/cpuinfo").read_text().splitlines()
                if line.startswith("model name")), platform.processor())
    RESULTS.mkdir(parents=True, exist_ok=True)
    rows = []
    for workload, operations in WORKLOADS.items():
        for repeat in range(REPEATS):
            pair = {}
            order = ("libc", "hm") if repeat % 2 == 0 else ("hm", "libc")
            for allocator in order:
                elapsed, checksum = run(allocator, workload, operations)
                pair[allocator] = checksum
                rows.append({"commit": commit, "machine": platform.node(),
                             "cpu": cpu, "kernel": platform.release(),
                             "workload": workload, "operations": operations,
                             "repeat": repeat, "allocator": allocator,
                             "seconds": elapsed, "checksum": checksum})
            if pair["libc"] != pair["hm"]:
                raise RuntimeError(f"checksum mismatch for {workload} repeat {repeat}")
    path = RESULTS / "current.csv"
    with path.open("w", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=rows[0].keys(),
                                lineterminator="\n")
        writer.writeheader()
        writer.writerows(rows)

    os.environ.setdefault("MPLCONFIGDIR", "/tmp/hypermalloc-matplotlib")
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    fig, ax = plt.subplots(figsize=(7, 3.5))
    names = list(WORKLOADS)
    for index, allocator in enumerate(("libc", "hm")):
        medians = [statistics.median(
            row["seconds"] for row in rows
            if row["workload"] == name and row["allocator"] == allocator
        ) for name in names]
        ax.bar([number + (index - .5) * .36 for number in range(len(names))],
               medians, width=.36, label=allocator)
    ax.set_xticks(range(len(names)), names)
    ax.set_ylabel("Median wall time (seconds, lower is better)")
    ax.set_title("Matched allocator workloads (7 runs each)")
    ax.legend()
    fig.tight_layout()
    svg_path = RESULTS / "current.svg"
    fig.savefig(svg_path)
    svg_path.write_text("\n".join(line.rstrip() for line in
                                  svg_path.read_text().splitlines()) + "\n")
    for name in names:
        libc = statistics.median(row["seconds"] for row in rows
                                 if row["workload"] == name and row["allocator"] == "libc")
        hm = statistics.median(row["seconds"] for row in rows
                               if row["workload"] == name and row["allocator"] == "hm")
        print(f"{name}: libc={libc:.6f}s hm={hm:.6f}s ratio={hm/libc:.2f}x")
    print(path)


if __name__ == "__main__":
    main()
