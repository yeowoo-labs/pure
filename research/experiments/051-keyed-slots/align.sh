#!/bin/bash
# align.sh OUT ROUNDS WORK...: research 051, align1's suite-run with each loop alignment (none 16 32 64), in rotating turns.
out=$1 rounds=$2; shift 2
B=$HOME/.cache/pure-040
quiet() { until awk '{ exit !($1 < 1.5) }' /proc/loadavg; do sleep 10; done; }
aligns=(0 16 32 64)
for round in $(seq 1 "$rounds"); do
    for j in 0 1 2 3; do
        a=${aligns[$(( (j + round) % 4 ))]}
        quiet
        if [ "$a" = 0 ]; then env -u PURE_TIER_ALIGN "$B/align1/suite-run" "$B/align1" 5 "$@" 2>/dev/null | grep median | sed "s/^{/{\"version\":\"a$a\",\"pair\":$round,/" >> "$out"
        else PURE_TIER_ALIGN=$a "$B/align1/suite-run" "$B/align1" 5 "$@" 2>/dev/null | grep median | sed "s/^{/{\"version\":\"a$a\",\"pair\":$round,/" >> "$out"; fi
    done
done
