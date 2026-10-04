#!/bin/sh
# The runs of research 120 (docs/design/019), from the repository's root,
# in steps, each under research/guard.py:
#   runs.sh fresh ROUND     1,000 clients follow the counter by push and by
#                           polling, at 1, 10 and 100 writes a second (the
#                           order turned in even rounds), 10 s each
#   runs.sh many ROUND      10,000 subscribers, a write a second
#   runs.sh build NAME COMMIT, pairs FIRST LAST, counts   as 033's: the
#                           host before (84483fe) and after (a7de450)
set -e
out=research/experiments/035-push
builds=$HOME/.cache/pure-035
day=$(date +%F)
case "$1" in
fresh)
    runs="1000:1:push 1000:1:poll:100 1000:10:push 1000:10:poll:20 1000:100:push 1000:100:poll:20"
    if [ $(($2 % 2)) = 0 ]; then runs=$(echo $runs | tr ' ' '\n' | tac | tr '\n' ' '); fi
    until awk '{ exit !($1 < 1.5) }' /proc/loadavg; do sleep 10; done
    python3 research/guard.py --most 2048 -- python3 $out/push.py build/pure-serve ../aup/build/aup 3 10 $runs \
        > "$out/$day-fresh-$2.jsonl" ;;
many)
    until awk '{ exit !($1 < 1.5) }' /proc/loadavg; do sleep 10; done
    python3 research/guard.py --most 3072 -- python3 $out/push.py build/pure-serve ../aup/build/aup 3 10 10000:1:push \
        > "$out/$day-many-$2.jsonl" ;;
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
*) echo "runs.sh fresh ROUND | many ROUND | build NAME COMMIT | pairs FIRST LAST | counts" >&2; exit 2 ;;
esac
