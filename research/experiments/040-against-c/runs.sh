#!/bin/sh
# The runs of research 124's suite (docs/design/023), from the repository's
# root, each under research/guard.py on a quiet Pi:
#   runs.sh build NAME COMMIT    suite-run on COMMIT's core, and the
#                                workloads' modules, in ~/.cache/pure-040/NAME
#   runs.sh suite NAME ROUNDS    every workload on NAME: as it runs (JIT where
#                                Linux AArch64), the executor alone
#                                (PURE_JIT=0), and BFS without reading ahead
#                                (PURE_AHEAD=0)
#   runs.sh floor ROUNDS         floor.c's three modes (bare, hoisted, naive),
#                                each against natural.h's C
#   runs.sh serve NAME COMMIT    pure-serve of COMMIT and its pure-crypto.so,
#                                in ~/.cache/pure-040/NAME
#   runs.sh pairs A B FIRST LAST TPC-C in RAM (W=1, 64 terminals, 20 s) and
#                                the bank's queries on A and B, in turns
#                                (036's), rounds FIRST to LAST
#   runs.sh counts A B           the bank queries' instructions on A and B
#                                (025's count.py)
set -e
here=research/experiments/040-against-c
builds=$HOME/.cache/pure-040
day=$(date +%F)
quiet() { until awk '{ exit !($1 < 1.5) }' /proc/loadavg; do sleep 10; done; }
case "$1" in
build)
    rm -rf "$builds/src-$2"
    git worktree add -f -q --detach "$builds/src-$2" "$3"
    make -s -C "$builds/src-$2" build/libpure.a
    mkdir -p "$builds/$2"
    cc -O2 -g -std=c17 -I"$builds/src-$2/core" $here/suite-run.c "$builds/src-$2/build/libpure.a" -lm -o "$builds/$2/suite-run"
    git worktree remove --force "$builds/src-$2"
    for work in roots knapsack sort join words matmul; do ../aup/build/aup build $here/$work.aup -o "$builds/$2/$work.pmod"; done
    ../aup/build/aup build research/experiments/033-computing/graph.aup -o "$builds/$2/graph.pmod" ;;
suite)
    quiet
    echo "$2 $(uptime)" >> "$here/$day-load.txt"
    python3 research/guard.py --most 3072 -- "$builds/$2/suite-run" "$builds/$2" "$3" \
        | sed "s/^{/{\"version\":\"$2\",\"jit\":1,\"ahead\":1,/" >> "$here/$day-suite.jsonl"
    quiet
    PURE_JIT=0 python3 research/guard.py --most 3072 -- "$builds/$2/suite-run" "$builds/$2" "$3" \
        | sed "s/^{/{\"version\":\"$2\",\"jit\":0,\"ahead\":1,/" >> "$here/$day-suite.jsonl"
    quiet
    PURE_AHEAD=0 python3 research/guard.py --most 3072 -- "$builds/$2/suite-run" "$builds/$2" "$3" bfs bfs_ahead \
        | sed "s/^{/{\"version\":\"$2\",\"jit\":1,\"ahead\":0,/" >> "$here/$day-suite.jsonl" ;;
serve)
    rm -rf "$builds/src-$2"
    git worktree add -f -q --detach "$builds/src-$2" "$3"
    make -s -C "$builds/src-$2" build/pure-serve build/pure-crypto.so
    mkdir -p "$builds/$2"
    cp "$builds/src-$2/build/pure-serve" "$builds/src-$2/build/pure-crypto.so" "$builds/$2/"
    git worktree remove --force "$builds/src-$2" ;;
pairs)
    for round in $(seq "$4" "$5"); do
        first=$2 second=$3
        if [ $((round % 2)) = 0 ]; then first=$3 second=$2; fi
        for version in $first $second; do
            quiet
            echo "$round $version $(uptime)" >> "$here/$day-load.txt"
            python3 research/guard.py -- python3 benchmarks/tpcc/run.py --serve "$builds/$version/pure-serve" --warehouses 1 \
                --check 0 --terminals 64 --sqlite-threads '' --sqlite-group '' --seconds 20 \
                > "$here/$day-tpcc-$version-$round.jsonl"
            python3 research/guard.py -- python3 benchmarks/bank/queries.py --serve "$builds/$version/pure-serve" \
                > "$here/$day-queries-$version-$round.jsonl"
        done
    done ;;
counts)
    python3 research/guard.py -- python3 research/experiments/025-i64-cost/count.py "$builds/$2/pure-serve" \
        "$builds/$3/pure-serve" "$builds/$2/pure-serve" "$builds/$3/pure-serve" > "$here/$day-counts.jsonl" ;;
floor)
    for mode in 0 1 2; do
        cc -O2 -std=c17 -DMODE=$mode -o "$builds/floor-$mode" $here/floor.c -lm
        quiet
        python3 research/guard.py --most 2048 -- "$builds/floor-$mode" "$2" >> "$here/$day-floor.jsonl"
    done ;;
*) echo "runs.sh build NAME COMMIT | suite NAME ROUNDS | floor ROUNDS | serve NAME COMMIT | pairs A B FIRST LAST | counts A B" >&2
    exit 2 ;;
esac
