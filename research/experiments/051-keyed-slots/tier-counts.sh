#!/bin/bash
# tier-counts.sh OUT A B WORK...: suite-run's 3 rounds under icount, the tier on, A and B in turns, twice each.
out=$1 a=$2 b=$3; shift 3
B=$HOME/.cache/pure-040
quiet() { until awk '{ exit !($1 < 1.5) }' /proc/loadavg; do sleep 10; done; }
for work in "$@"; do
    for round in 1 2; do
        if [ $round = 2 ]; then order="$b $a"; else order="$a $b"; fi
        for v in $order; do
            quiet
            line=$("$B/icount" "$B/$v/suite-run" "$B/$v" 3 "$work" 2>&1 >/dev/null | tail -1)
            echo "$work $v $round $line" >> "$out"
        done
    done
done
