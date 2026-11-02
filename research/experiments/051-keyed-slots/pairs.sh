#!/bin/bash
# pairs.sh OUT ROUNDS A B WORK... : suite-run of A and B in turns, the order swapped each round.
out=$1 rounds=$2 a=$3 b=$4; shift 4
B=$HOME/.cache/pure-040
cd "$(dirname "$0")/../../.."
quiet() { until awk '{ exit !($1 < 1.5) }' /proc/loadavg; do sleep 10; done; }
for round in $(seq 1 "$rounds"); do
    if [ $((round % 2)) = 0 ]; then order="$b $a"; else order="$a $b"; fi
    for v in $order; do
        quiet
        echo "$round $v $(cat /proc/loadavg)" >> "$out.load"
        python3 research/guard.py --most 3072 -- "$B/$v/suite-run" "$B/$v" 5 "$@" | grep median \
            | sed "s/^{/{\"version\":\"$v\",\"pair\":$round,/" >> "$out.jsonl"
    done
done
