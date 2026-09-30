#!/bin/sh
# The runs of docs/design/016's second stage, from the repository's root:
# pure-serve before i64 (9a03e72, built in a scratch worktree) and after (the
# tree as it is), in turns: TPC-C in RAM (W=1, 64 terminals), then the bank's
# queries, six rounds, the order flipped every other round. The modules are
# the same bytes on both sides (aup before and after i64 builds tpcc.aup,
# bank.aup and the four queries alike), so only the runtime differs. The
# results go to a directory named for the after commit: runs.sh fc65d66.
set -e
out=research/experiments/025-i64-cost/$1
mkdir -p $out
work=$(mktemp -d)
git worktree add -q --detach $work/tree 9a03e72
make -C $work/tree -s build/pure-serve
cp $work/tree/build/pure-serve $work/pure-serve-before
git worktree remove --force $work/tree
make -s build/pure-serve
cp build/pure-serve $work/pure-serve-after
for round in 1 2 3 4 5 6; do
    first=before second=after
    if [ $((round % 2)) = 0 ]; then first=after second=before; fi
    for version in $first $second; do
        echo "$round $version $(uptime)" >> $out/2026-09-30-load.txt
        python3 benchmarks/tpcc/run.py --serve $work/pure-serve-$version --warehouses 1 --check 0 --terminals 64 \
            --sqlite-threads '' --sqlite-group '' --seconds 20 > $out/2026-09-30-tpcc-$version-$round-linux-aarch64.jsonl
        python3 benchmarks/bank/queries.py --serve $work/pure-serve-$version \
            > $out/2026-09-30-queries-$version-$round-linux-aarch64.jsonl
    done
done
