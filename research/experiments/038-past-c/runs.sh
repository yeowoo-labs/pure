#!/bin/sh
# The runs of research 123 (docs/design/021), from the repository's
# root, in steps, each under research/guard.py:
#   runs.sh build NAME COMMIT   pure-serve of COMMIT, with its pure-crypto.so,
#                               and bfs-run on its core, in ~/.cache/pure-038/NAME
#   runs.sh core FIRST LAST     bfs-run (100,000 nodes, 1,000,000 edges, 7
#                               searches) before (a553601, before 021)
#                               and after, in turns
#   runs.sh pairs FIRST LAST    as 033's: TPC-C in RAM (W=1, 64 terminals,
#                               20 s) and the bank's queries, in turns
#   runs.sh counts              the bank queries' instructions (025's count.py)
#   runs.sh jit FIRST LAST      TPC-C as pairs, on after with the JIT and
#                               without (PURE_JIT=0), in turns
#   runs.sh bfs NAME            033's bfs.py on that build (100,000 nodes,
#                               1,000,000 edges, 5 searches, 3 rounds)
#   runs.sh floor               bfsfloor's forms in turns (9 searches)
#   runs.sh alone               bfsfloor's forms each alone, warm (5
#                               searches of 21 runs, 3 rounds)
#   runs.sh widths              narrow and narrow_ahead alone with buckets
#                               of 4, 16 and 32 items (2 rounds)
#   runs.sh roundtrip           roundtrip.py on after (3 times)
set -e
here=research/experiments/038-past-c
builds=$HOME/.cache/pure-038
day=$(date +%F)
case "$1" in
build)
    rm -rf "$builds/src-$2"
    git worktree add -f -q --detach "$builds/src-$2" "$3"
    make -s -C "$builds/src-$2" build/pure-serve build/pure-crypto.so build/libpure.a
    mkdir -p "$builds/$2"
    cp "$builds/src-$2/build/pure-serve" "$builds/src-$2/build/pure-crypto.so" "$builds/$2/"
    cc -O2 -g -std=c17 -I"$builds/src-$2/core" $here/bfs-run.c "$builds/src-$2/build/libpure.a" -lm -o "$builds/$2/bfs-run"
    git worktree remove --force "$builds/src-$2"
    ../aup/build/aup build research/experiments/033-computing/graph.aup -o "$builds/graph.pmod" ;;
core)
    for round in $(seq "$2" "$3"); do
        first=before second=after
        if [ $((round % 2)) = 0 ]; then first=after second=before; fi
        for version in $first $second; do
            until awk '{ exit !($1 < 1.5) }' /proc/loadavg; do sleep 10; done
            printf '{"version":"%s","round":%s,"run":' $version $round >> "$here/$day-core.jsonl"
            python3 research/guard.py --most 2048 -- "$builds/$version/bfs-run" "$builds/graph.pmod" 100000 1000000 7 \
                | tail -1 | sed 's/$/}/' >> "$here/$day-core.jsonl"
        done
    done ;;
pairs)
    for round in $(seq "$2" "$3"); do
        first=before second=after
        if [ $((round % 2)) = 0 ]; then first=after second=before; fi
        for version in $first $second; do
            until awk '{ exit !($1 < 1.5) }' /proc/loadavg; do sleep 10; done
            echo "$round $version $(uptime)" >> "$here/$day-load.txt"
            python3 research/guard.py -- python3 benchmarks/tpcc/run.py --serve "$builds/$version/pure-serve" --warehouses 1 \
                --check 0 --terminals 64 --sqlite-threads '' --sqlite-group '' --seconds 20 \
                > "$here/$day-tpcc-$version-$round.jsonl"
            python3 research/guard.py -- python3 benchmarks/bank/queries.py --serve "$builds/$version/pure-serve" \
                > "$here/$day-queries-$version-$round.jsonl"
        done
    done ;;
jit)
    for round in $(seq "$2" "$3"); do
        first=1 second=0
        if [ $((round % 2)) = 0 ]; then first=0 second=1; fi
        for jit in $first $second; do
            until awk '{ exit !($1 < 1.5) }' /proc/loadavg; do sleep 10; done
            PURE_JIT=$jit python3 research/guard.py -- python3 benchmarks/tpcc/run.py --serve "$builds/after/pure-serve" \
                --warehouses 1 --check 0 --terminals 64 --sqlite-threads '' --sqlite-group '' --seconds 20 \
                > "$here/$day-tpcc-jit$jit-$round.jsonl"
        done
    done ;;
counts)
    python3 research/guard.py -- python3 research/experiments/025-i64-cost/count.py "$builds/before/pure-serve" \
        "$builds/after/pure-serve" "$builds/before/pure-serve" "$builds/after/pure-serve" > "$here/$day-counts.jsonl" ;;
bfs)
    until awk '{ exit !($1 < 1.5) }' /proc/loadavg; do sleep 10; done
    python3 research/guard.py --most 2048 -- python3 research/experiments/033-computing/bfs.py "$builds/$2/pure-serve" \
        ../aup/build/aup 100000 1000000 5 3 > "$here/$day-bfs-$2.jsonl" ;;
floor|alone|widths)
    cc -O2 -std=gnu17 -o "$builds/bfsfloor" $here/bfsfloor.c -lm
    floor() { python3 research/guard.py --most 1024 -- "$builds/bfsfloor" 100000 1000000 "$@"; }
    case "$1" in
    floor) floor 9 16 8 > "$here/$day-bfsfloor.jsonl" ;;
    alone)
        for round in 1 2 3; do
            for form in c narrow_ahead narrow c_ahead packed; do
                floor 5 16 8 8 $form 21 | sed "s/^{/{\"round\":$round,/" >> "$here/$day-bfsfloor-alone.jsonl"
            done
        done ;;
    widths)
        for round in 1 2; do
            for per in 4 16 32; do
                for form in narrow_ahead narrow; do
                    floor 5 16 8 $per $form 21 | sed "s/^{/{\"round\":$round,/" >> "$here/$day-bfsfloor-widths.jsonl"
                done
            done
        done ;;
    esac ;;
roundtrip)
    for i in 1 2 3; do
        taskset -c 3 python3 $here/roundtrip.py "$builds/after/pure-serve" "$builds/graph.pmod" >> "$here/$day-roundtrip.jsonl"
    done ;;
*) echo "runs.sh build NAME COMMIT | core FIRST LAST | pairs FIRST LAST | jit FIRST LAST | counts | bfs NAME | floor | alone | widths | roundtrip" >&2
    exit 2 ;;
esac
