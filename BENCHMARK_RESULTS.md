# Reproducible benchmark results (2026-10-28)

Run `python3 scripts/bench.py` from the repository root to rebuild and repeat
the comparison. The script alternates allocator order, runs each matched
workload seven times, verifies equal checksums, and writes raw measurements
and charts to `benchmarks/results/`. The harness uses `CLOCK_MONOTONIC` and
touches the first and last byte of every allocation. These are process wall
times, not isolated allocator call times.

| Workload | Operations | libc median | HyperMalloc before | HyperMalloc after |
|---|---:|---:|---:|---:|
| small | 100,000 | 0.0053 / 0.0046 s | 0.0070 s | 0.0059 s |
| mixed | 100,000 | 0.0076 / 0.0126 s | 0.0173 s | 0.0189 s |
| large | 20,000 | 0.1132 / 0.1094 s | 0.8885 s | 0.3317 s |

Each libc cell shows the before and after run medians. The large-workload
change replaced a linear direct-map registry search with 1,024 hash buckets
and constant-time list unlinking. The after median is 2.7 times faster than
the before median for HyperMalloc's large workload, while still about 3.0
times slower than libc in the after run. Small and mixed results varied
between runs; no improvement is claimed for those workloads.

The CSV files record the machine, CPU, kernel, commit baseline, each repeat,
and checksum. The before and after measurements were collected in the same
workspace, with other machine activity uncontrolled. The source change and
test results establish correctness for tested cases, but these timings are
not a general performance guarantee.
