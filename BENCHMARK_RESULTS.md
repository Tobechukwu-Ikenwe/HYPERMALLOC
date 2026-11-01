# Historical single-thread benchmark results (2026-10-28)

These measurements predate the independent-arena change. See
`optimizations.md` for the current concurrency comparison.

Run `python3 scripts/bench.py` from the repository root to rebuild and repeat
the comparison. The script alternates allocator order, runs each matched
workload seven times, verifies equal checksums, and writes new measurements
and charts to `out/benchmarks/`. The recorded experiment is in
`benchmarks/results/`. The harness uses `CLOCK_MONOTONIC` and
touches the first and last byte of every allocation. These are process wall
times, not isolated allocator call times.

| Workload | Operations | Current libc median | Current HyperMalloc median | Ratio |
|---|---:|---:|---:|---:|
| small | 100,000 | 0.0049 s | 0.0065 s | 1.31× |
| mixed | 100,000 | 0.0098 s | 0.0176 s | 1.80× |
| large | 20,000 | 0.1043 s | 0.3318 s | 3.18× |

The earlier controlled lookup experiment is preserved as `before.csv` and
`after_hash.csv` (with matching SVG charts). It replaced a linear direct-map
registry search with 1,024 hash buckets and constant-time list unlinking.
HyperMalloc's large-workload median fell from 0.8885 s to 0.3317 s in those
runs, a 2.7× improvement. Small and mixed results varied between runs; no
improvement is claimed for those workloads. The `current.*` artifacts include
later fork and chunk-release changes and are a separate measurement.

The CSV files record the machine, CPU, kernel, commit baseline, each repeat,
and checksum. `-dirty` marks measurements taken with uncommitted source edits.
All measurements were collected in the same workspace, with other machine
activity uncontrolled. Test results establish correctness for tested cases,
but these timings are not a general performance guarantee.
