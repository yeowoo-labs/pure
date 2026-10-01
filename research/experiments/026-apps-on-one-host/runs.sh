#!/bin/sh
# The runs of docs/design/017's first stage, from the repository's root, in
# steps (each fits a session's background limit), the binaries kept in
# $WORK (default /tmp/pure-026) between them:
#   runs.sh build       pure-serve before 017 (0b5fa1d, in a scratch worktree) and after
#   runs.sh memory      what an app costs a host (memory.py)
#   runs.sh isolation   two apps as two processes or one host (isolation.py, five rounds of 15 s)
#   runs.sh pairs A B   a host of one app before and after, rounds A to B in turns as 025 ran them:
#                       TPC-C in RAM (W=1, 64 terminals) and the bank's queries
#   runs.sh counts      the bank queries' instructions before and after (025's count.py)
# Each timed step waits until the Pi has been quiet (the last minute's load below 1.5).
set -e
out=research/experiments/026-apps-on-one-host
work=${WORK:-/tmp/pure-026}
quiet() { until awk '{ exit !($1 < 1.5) }' /proc/loadavg; do sleep 10; done; }
case $1 in
build)
    mkdir -p $work
    git worktree add -q --detach $work/tree 0b5fa1d
    make -C $work/tree -s build/pure-serve
    cp $work/tree/build/pure-serve $work/pure-serve-before
    git worktree remove --force $work/tree
    make -s build/pure-serve
    cp build/pure-serve $work/pure-serve-after
    (cd benchmarks/http && cc -O2 -std=c17 -o load load.c) ;;
memory)
    quiet
    python3 $out/memory.py $work/pure-serve-after 1 10 100 > $out/2026-10-01-memory-linux-aarch64.jsonl ;;
isolation)
    quiet
    echo "isolation $(uptime)" >> $out/2026-10-01-load.txt
    python3 $out/isolation.py $work/pure-serve-after 5 15 > $out/2026-10-01-isolation-linux-aarch64.jsonl ;;
pairs)
    for round in $(seq $2 $3); do
        first=before second=after
        if [ $((round % 2)) = 0 ]; then first=after second=before; fi
        for version in $first $second; do
            quiet
            echo "$round $version $(uptime)" >> $out/2026-10-01-load.txt
            python3 benchmarks/tpcc/run.py --serve $work/pure-serve-$version --warehouses 1 --check 0 --terminals 64 \
                --sqlite-threads '' --sqlite-group '' --seconds 20 > $out/2026-10-01-tpcc-$version-$round-linux-aarch64.jsonl
            python3 benchmarks/bank/queries.py --serve $work/pure-serve-$version \
                > $out/2026-10-01-queries-$version-$round-linux-aarch64.jsonl
        done
    done ;;
counts)
    python3 research/experiments/025-i64-cost/count.py $work/pure-serve-before $work/pure-serve-after \
        $work/pure-serve-before $work/pure-serve-after > $out/2026-10-01-counts-linux-aarch64.jsonl ;;
*)
    echo "runs.sh build|memory|isolation|pairs FIRST LAST|counts" >&2
    exit 2 ;;
esac
