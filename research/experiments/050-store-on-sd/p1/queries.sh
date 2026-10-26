#!/bin/sh
# queries.sh A B FIRST LAST: the bank's queries alone on A and B in turns (as 040's runs.sh pairs runs them), rounds FIRST to LAST
set -e
here=research/experiments/040-against-c
builds=$HOME/.cache/pure-040
day=$(date +%F)
quiet() { until awk '{ exit !($1 < 1.5) }' /proc/loadavg; do sleep 10; done; }
for round in $(seq "$3" "$4"); do
    first=$1 second=$2
    if [ $((round % 2)) = 0 ]; then first=$2 second=$1; fi
    for version in $first $second; do
        quiet
        echo "$round $version queries $(uptime)" >> "$here/$day-load.txt"
        python3 research/guard.py -- python3 benchmarks/bank/queries.py --serve "$builds/$version/pure-serve" \
            > "$here/$day-queries-$version-$round.jsonl"
    done
done
