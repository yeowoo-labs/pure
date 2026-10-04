#!/bin/sh
# The runs of research 118 (docs/design/018's stage 2), from the repository's
# root, in steps, each under research/guard.py:
#   runs.sh build NAME COMMIT   pure-serve of COMMIT, with its pure-crypto.so,
#                               in ~/.cache/pure-033/NAME
#   runs.sh pairs FIRST LAST    a host of one app, the builds before (850f783,
#                               stage 2's criteria's parent) and after in
#                               turns, as 026 ran them: TPC-C in RAM (W=1,
#                               64 terminals, 20 s) and the bank's queries
#   runs.sh counts              the bank queries' instructions before and
#                               after (025's count.py)
#   runs.sh bfs NODES EDGES SEARCHES ROUNDS   bfs.py on the after build
set -e
out=research/experiments/033-computing
builds=$HOME/.cache/pure-033
day=$(date +%F)
case "$1" in
build)
    rm -rf "$builds/src-$2"
    git worktree add -f -q --detach "$builds/src-$2" "$3"
    make -s -C "$builds/src-$2" build/pure-serve build/pure-crypto.so
    mkdir -p "$builds/$2"
    cp "$builds/src-$2/build/pure-serve" "$builds/src-$2/build/pure-crypto.so" "$builds/$2/"
    git worktree remove --force "$builds/src-$2" ;;
pairs)
    for round in $(seq "$2" "$3"); do
        first=before second=after
        if [ $((round % 2)) = 0 ]; then first=after second=before; fi
        for version in $first $second; do
            until awk '{ exit !($1 < 1.5) }' /proc/loadavg; do sleep 10; done
            echo "$round $version $(uptime)" >> "$out/$day-load.txt"
            python3 research/guard.py -- python3 benchmarks/tpcc/run.py --serve "$builds/$version/pure-serve" --warehouses 1 \
                --check 0 --terminals 64 --sqlite-threads '' --sqlite-group '' --seconds 20 \
                > "$out/$day-tpcc-$version-$round.jsonl"
            python3 research/guard.py -- python3 benchmarks/bank/queries.py --serve "$builds/$version/pure-serve" \
                > "$out/$day-queries-$version-$round.jsonl"
        done
    done ;;
counts)
    python3 research/guard.py -- python3 research/experiments/025-i64-cost/count.py "$builds/before/pure-serve" \
        "$builds/after/pure-serve" "$builds/before/pure-serve" "$builds/after/pure-serve" > "$out/$day-counts.jsonl" ;;
bfs)
    until awk '{ exit !($1 < 1.5) }' /proc/loadavg; do sleep 10; done
    python3 research/guard.py --most 2048 -- python3 $out/bfs.py "$builds/after/pure-serve" ../aup/build/aup "$2" "$3" "$4" "$5" \
        > "$out/$day-bfs-$2-$3.jsonl" ;;
*) echo "runs.sh build NAME COMMIT | pairs FIRST LAST | counts | bfs NODES EDGES SEARCHES ROUNDS" >&2
    exit 2 ;;
esac
