#!/bin/sh
set -eu
case "${1:-release}" in
    release|debug|asan|tsan|valgrind) preset=${1:-release} ;;
    *) echo "usage: $0 [release|debug|asan|tsan|valgrind]" >&2; exit 2 ;;
esac
cmake --preset "$preset"
cmake --build "out/$preset" -j 4
if [ "$preset" = asan ]; then
    ASAN_OPTIONS=detect_leaks=0 ctest --test-dir "out/$preset" --output-on-failure
else
    ctest --test-dir "out/$preset" --output-on-failure
fi
