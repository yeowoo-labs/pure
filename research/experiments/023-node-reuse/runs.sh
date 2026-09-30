#!/bin/sh
# The runs of docs/research/107, from the repository's root. BEFORE is
# pure-serve built at c9fda91 (nodes a snapshot let go were freed), AFTER at
# the commit that reuses them; copy each out of build/ first, since the
# names they run under do not matter (the owner is found by its thread).
set -e
out=research/experiments/023-node-reuse
before=${BEFORE:?pure-serve built before}
after=${AFTER:?pure-serve built after}
jemalloc=/usr/lib/aarch64-linux-gnu/libjemalloc.so.2
run() {
    name=$1
    shift
    python3 benchmarks/tpcc/run.py "$@" --sqlite-threads '' --sqlite-group '' > $out/2026-09-30-$name-linux-aarch64.jsonl
}
alone() {
    name=$1
    serve=$2
    run $name --serve $serve --warehouses 1 --check 0 --terminals 64 --seconds 20
}

# Is it the allocator? The same server under jemalloc, and with glibc's
# thread cache raised (it stays at 1,032 B: no change), against glibc.
printf '#!/bin/sh\nLD_PRELOAD=%s exec %s "$@"\n' $jemalloc $before > $out/serve-jemalloc.sh
printf '#!/bin/sh\nGLIBC_TUNABLES=glibc.malloc.tcache_max=4096 exec %s "$@"\n' $before > $out/serve-tcache.sh
chmod +x $out/serve-jemalloc.sh $out/serve-tcache.sh
alone allocator-glibc-1 $before
alone allocator-jemalloc-1 $out/serve-jemalloc.sh
alone allocator-tcache-1 $out/serve-tcache.sh
alone allocator-glibc-2 $before
alone allocator-jemalloc-2 $out/serve-jemalloc.sh
rm $out/serve-jemalloc.sh $out/serve-tcache.sh

# Before and after, taking turns: three pairs before first, three after first.
for round in 1 2 3; do
    alone before-$round $before
    alone after-$round $after
done
for round in 4 5 6; do
    alone after-$round $after
    alone before-$round $before
done

# Where the owner is now (as in 106): 80 samples while AFTER runs 90 seconds.
python3 research/experiments/022-tpcc-grouped/owner-samples.py 80 $out/2026-09-30-owner-stacks-linux-aarch64.txt &
run owner-run --serve $after --warehouses 1 --check 0 --terminals 64 --seconds 90
wait

# 106's RAM matrix again with AFTER (SQLite as it was).
both='--terminals 16,64 --sqlite-threads 1,4 --sqlite-group 16,64'
python3 benchmarks/tpcc/run.py --serve $after --warehouses 1 --check 20000 $both --seconds 20 \
    > $out/2026-09-30-ram-w1-linux-aarch64.jsonl
python3 benchmarks/tpcc/run.py --serve $after --warehouses 4 --check 20000 $both --seconds 20 \
    > $out/2026-09-30-ram-w4-linux-aarch64.jsonl
