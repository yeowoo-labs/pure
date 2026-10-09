#!/bin/sh
# The batch executor's runs (docs/design/024), from the repository's root,
# on builds made by 040's runs.sh build (~/.cache/pure-040/NAME), under
# research/guard.py on a quiet Pi:
#   runs.sh suite NAME ROUNDS [WORK ...]   the suite on NAME three ways: the
#                                          compiled code (jit), the executor
#                                          with its batches (batches,
#                                          PURE_JIT=0), the executor alone
#                                          (rounds, PURE_BATCH=0 too)
#   runs.sh counts NAME [WORK ...]         user-mode instructions and cycles
#                                          of suite-run's 3 rounds, each of
#                                          the two executor ways (icount.c)
#   runs.sh pairs A B ROUNDS TIMES [WORK ...]  the batches way of A then B,
#                                          TIMES turns (interleaved: the
#                                          medians of A's and B's turns)
# (A build of another lane count: CPPFLAGS=-DPURE_BATCH_LANES=256 with
# 040's runs.sh build.)
set -e
here=research/experiments/041-batches
builds=$HOME/.cache/pure-040
day=$(date +%F)
quiet() { until awk '{ exit !($1 < 1.5) }' /proc/loadavg; do sleep 10; done; }
mode=$1 name=$2
shift 2
case "$mode" in
suite)
    rounds=$1
    shift
    for way in jit batches rounds; do
        quiet
        echo "$name $way $(uptime)" >> "$here/$day-load.txt"
        case $way in
        jit) PURE_JIT=1 python3 research/guard.py --most 3072 -- "$builds/$name/suite-run" "$builds/$name" "$rounds" "$@" ;;
        batches) PURE_JIT=0 python3 research/guard.py --most 3072 -- "$builds/$name/suite-run" "$builds/$name" "$rounds" "$@" ;;
        rounds) PURE_JIT=0 PURE_BATCH=0 python3 research/guard.py --most 3072 -- "$builds/$name/suite-run" "$builds/$name" "$rounds" "$@" ;;
        esac | sed "s/^{/{\"version\":\"$name\",\"way\":\"$way\",/" >> "$here/$day-suite.jsonl"
    done ;;
counts)
    cc -O2 -o "$builds/icount" $here/icount.c
    for work in "$@"; do
        for way in batches rounds; do
            quiet
            batch=1
            [ $way = rounds ] && batch=0
            PURE_JIT=0 PURE_BATCH=$batch "$builds/icount" "$builds/$name/suite-run" "$builds/$name" 3 "$work" 2>&1 >/dev/null \
                | sed "s/^{/{\"version\":\"$name\",\"way\":\"$way\",\"work\":\"$work\",/" >> "$here/$day-counts.jsonl"
        done
    done ;;
pairs)
    other=$1 rounds=$2 times=$3
    shift 3
    for turn in $(seq "$times"); do
        for version in "$name" "$other"; do
            quiet
            PURE_JIT=0 python3 research/guard.py --most 3072 -- "$builds/$version/suite-run" "$builds/$version" "$rounds" "$@" \
                | grep median | sed "s/^{/{\"version\":\"$version\",\"way\":\"batches\",\"turn\":$turn,/" >> "$here/$day-pairs.jsonl"
        done
    done ;;
*) echo "runs.sh suite NAME ROUNDS [WORK ...] | counts NAME [WORK ...] | pairs A B ROUNDS TIMES [WORK ...]" >&2
    exit 2 ;;
esac
# 2026-10-08-lanes.jsonl (research 132): b9's code with BATCH 1024, 512 and
# 256 (core/module-batch.c's enum), suite-run 5 rounds twice (ms: the two
# medians) and costs/time.c's sum0, ops0 and nest under icount 3 rounds:
# a batch's fixed cost is (the instructions' difference) / (the batches'),
# icount's 3 rounds counted: 3,000 and 12,000 batches for sum0 and ops0.
# 2026-10-08-batch-costs.jsonl (design 025): knapsack's time a round by
# function and line, b9's code at BATCH 1024 and 256 under a 2 kHz sampled
# profile (ms by the in-process C's samples); sum0's times at both.
# A series run over midnight writes two days' files (2026-10-08 and
# 2026-10-09 for design 025's stage 5): read both.
