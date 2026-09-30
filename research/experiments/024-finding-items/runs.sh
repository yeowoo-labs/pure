#!/bin/sh
# The runs of docs/research/109, from the repository's root: the list
# micro-benchmark before and after prefetch.patch, then TPC-C's pure-serve
# before and after in turns (the tree is patched to build it, then put back).
set -e
out=research/experiments/024-finding-items
work=$(mktemp -d)
rest="core/module.c core/module-save.c core/module-text.c core/module-store.c core/flow.c store/store-tree.c store/store-log.c"
cp core/module-list.c $work/module-list-before.c
cp core/module-list.c $work/module-list-after.c
patch -s $work/module-list-after.c $out/prefetch.patch
for version in before after; do
    cc -O2 -g -std=c17 -D_GNU_SOURCE -Icore -Istore $out/bench-seek.c $work/module-list-$version.c $rest -pthread -lm \
        -o $work/bench-$version
done
python3 $out/micro.py $work > $out/2026-09-30-micro-linux-aarch64.txt

make -s build/pure-serve
cp build/pure-serve $work/pure-serve-before
cp $work/module-list-after.c core/module-list.c
make -s build/pure-serve
cp build/pure-serve $work/pure-serve-after
cp $work/module-list-before.c core/module-list.c
make -s build/pure-serve
for round in 1 2 3 4 5 6; do
    first=before second=after
    if [ $((round % 2)) = 0 ]; then first=after second=before; fi
    for version in $first $second; do
        python3 benchmarks/tpcc/run.py --serve $work/pure-serve-$version --warehouses 1 --check 0 --terminals 64 \
            --sqlite-threads '' --sqlite-group '' --seconds 20 > $out/2026-09-30-tpcc-$version-$round-linux-aarch64.jsonl
    done
done
