#!/usr/bin/env python3
"""Run matched single- and multi-thread allocator workloads."""
import argparse
import csv
import os
import platform
import statistics
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DEFAULT_PROGRAM = ROOT / "out/release/bench_threads"
CASES = {"small": 50000, "mixed": 30000, "direct": 1000,
         "handoff": 32768}
THREADS = (1, 2, 4, 8)


def run(program, allocator, workload, threads, operations, preload=None):
    env = os.environ.copy()
    env.pop("LD_PRELOAD", None)
    if preload:
        env["LD_PRELOAD"] = str(preload.resolve())
    output = subprocess.check_output(
        [str(program), allocator, workload, str(threads), str(operations)],
        text=True, env=env,
    )
    parts = output.split()
    if len(parts) == 2:
        elapsed, checksum = parts
        return float(elapsed), checksum, "", ""
    elapsed, checksum, mapped, peak = parts
    return float(elapsed), checksum, mapped, peak


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--program", type=Path, default=DEFAULT_PROGRAM)
    parser.add_argument("--baseline-program", type=Path)
    parser.add_argument("--variant", action="append", default=[],
                        metavar="NAME=PATH", help="additional hm build")
    parser.add_argument("--preload", action="append", default=[],
                        metavar="NAME=LIBRARY", help="standard malloc via LD_PRELOAD")
    parser.add_argument("--threads", action="append", type=int,
                        help="thread count (default 1,2,4,8; maximum 16)")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--repeats", type=int, default=5)
    parser.add_argument("--workload", action="append", choices=CASES,
                        help="run only selected workload; repeat as needed")
    args = parser.parse_args()
    if args.repeats < 1:
        parser.error("--repeats must be positive")
    programs = {"libc": (args.program, "libc", None),
                "hm": (args.program, "hm", None)}
    if args.baseline_program:
        programs["baseline"] = (args.baseline_program, "hm", None)
    for specification in args.variant:
        if "=" not in specification:
            parser.error("--variant must be NAME=PATH")
        name, path = specification.split("=", 1)
        if not name or name in programs:
            parser.error("variant name is empty or repeated")
        programs[name] = (Path(path), "hm", None)
    for specification in args.preload:
        if "=" not in specification:
            parser.error("--preload must be NAME=LIBRARY")
        name, path = specification.split("=", 1)
        if not name or name in programs:
            parser.error("preload name is empty or repeated")
        programs[name] = (args.program, "libc", Path(path))
    thread_counts = args.threads or THREADS
    if any(count < 1 or count > 16 for count in thread_counts):
        parser.error("thread counts must be between 1 and 16")
    rows = []
    cpu = next((line.split(":", 1)[1].strip()
                for line in Path("/proc/cpuinfo").read_text().splitlines()
                if line.startswith("model name")), platform.processor())
    selected = args.workload or list(CASES)
    for workload in selected:
        operations = CASES[workload]
        for threads in thread_counts:
            names = list(programs)
            for repeat in range(args.repeats):
                # Rotate order so a warm or busy machine does not always favor
                # the same allocator. All variants must produce equal checksums.
                order = names[repeat % len(names):] + names[:repeat % len(names)]
                checksums = set()
                for name in order:
                    program, allocator, preload = programs[name]
                    seconds, checksum, mapped, peak = run(
                        program, allocator, workload, threads, operations,
                        preload)
                    checksums.add(checksum)
                    rows.append({"machine": platform.node(), "cpu": cpu,
                                 "kernel": platform.release(),
                                 "workload": workload, "threads": threads,
                                 "operations_per_thread": operations,
                                 "repeat": repeat, "allocator": name,
                                 "seconds": seconds, "checksum": checksum,
                                 "mapped_after_bytes": mapped,
                                 "peak_mapped_bytes": peak})
                if len(checksums) != 1:
                    raise RuntimeError(f"checksum mismatch: {workload}/{threads}")
            medians = {name: statistics.median(
                row["seconds"] for row in rows if row["workload"] == workload
                and row["threads"] == threads and row["allocator"] == name)
                for name in names}
            print(workload, threads, medians, flush=True)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with args.output.open("w", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=rows[0].keys())
        writer.writeheader()
        writer.writerows(rows)
    print(args.output)


if __name__ == "__main__":
    main()
