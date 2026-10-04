#!/bin/sh
# The runs of research 115 (docs/design/017's stage 3), from the repository's
# root, in steps, each under research/guard.py:
#   runs.sh build NAME COMMIT   pure-serve of COMMIT, with its pure-crypto.so,
#                               in ~/.cache/pure-030/NAME
#   runs.sh window NAME ITEMS CHANGES RESTARTS [MODULE]   window.py on that
#                               build, into DATE-window-NAME-ITEMS[-MODULE].jsonl
#   runs.sh isolation NAME ITEMS NEXT EVERY SECONDS ROUNDS   isolation.py,
#                               into DATE-isolation-NAME-ITEMS-NEXT.jsonl
#   runs.sh pairs FIRST LAST    a host of one app, the builds before (e4c7f7e,
#                               stage 3's criteria) and after in turns, as
#                               026 ran them: TPC-C in RAM (W=1, 64
#                               terminals, 20 s) and the bank's queries
#   runs.sh counts              the bank queries' instructions before and
#                               after (025's count.py)
#   runs.sh memory CHANGES      what changing an app leaves in memory (memory.py)
set -e
out=research/experiments/030-app-lifecycle
builds=$HOME/.cache/pure-030
day=$(date +%F)
case "$1" in
build)
    rm -rf "$builds/src-$2"
    git worktree add -f -q --detach "$builds/src-$2" "$3"
    make -s -C "$builds/src-$2" build/pure-serve build/pure-crypto.so
    mkdir -p "$builds/$2"
    cp "$builds/src-$2/build/pure-serve" "$builds/src-$2/build/pure-crypto.so" "$builds/$2/"
    git worktree remove --force "$builds/src-$2" ;;
window)
    until awk '{ exit !($1 < 1.5) }' /proc/loadavg; do sleep 10; done
    python3 research/guard.py --most 2048 -- python3 $out/window.py "$builds/$2/pure-serve" "$3" "$4" "$5" ${6:+"$6"} \
        > "$out/$day-window-$2-$3${6:+-$6}.jsonl" ;;
pairs)
    for round in $(seq "$2" "$3"); do
        first=before second=in-place
        if [ $((round % 2)) = 0 ]; then first=in-place second=before; fi
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
        "$builds/in-place/pure-serve" "$builds/before/pure-serve" "$builds/in-place/pure-serve" > "$out/$day-counts.jsonl" ;;
memory)
    python3 research/guard.py -- python3 $out/memory.py "$builds/in-place/pure-serve" "$2" > "$out/$day-memory.jsonl" ;;
isolation)
    until awk '{ exit !($1 < 1.5) }' /proc/loadavg; do sleep 10; done
    python3 research/guard.py --most 2048 -- python3 $out/isolation.py "$builds/$2/pure-serve" "$3" "$4" "$5" "$6" "$7" \
        > "$out/$day-isolation-$2-$3-$4.jsonl" ;;
*) echo "runs.sh build NAME COMMIT | window NAME ITEMS CHANGES RESTARTS [MODULE] | isolation NAME ITEMS NEXT EVERY SECONDS ROUNDS | pairs FIRST LAST | counts | memory CHANGES" >&2
    exit 2 ;;
esac
