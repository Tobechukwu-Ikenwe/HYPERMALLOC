#!/bin/sh
set -eu
cmake --preset release
cmake --build out/release -j 4
out/release/fuzz_ops --seed 1 10 10000
