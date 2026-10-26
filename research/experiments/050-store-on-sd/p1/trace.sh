#!/bin/bash
# trace.sh OUT VERSION: one TPC-C run (W=1, 64 terminals, 20 s) with strace -c on the owner thread during the run step
out=$1 version=$2
here=$(dirname $0)
python3 benchmarks/tpcc/run.py --serve "$HOME/.cache/pure-040/$version/pure-serve" \
    --warehouses 1 --check 0 --terminals 64 --sqlite-threads '' --sqlite-group '' --seconds 20 > "$out-$version.jsonl" &
runner=$!
until grep -q '"step": "load"' "$out-$version.jsonl" 2>/dev/null; do sleep 0.2; done
pid=$(pgrep -x pure-serve | head -1)
tid=""
for t in /proc/$pid/task/*; do [ "$(cat $t/comm)" = pure-owner ] && tid=$(basename $t); done
before=$(cut -d')' -f2 /proc/$pid/task/$tid/stat | awk '{print $8, $12, $13}')
strace -c -p $tid -o "$out-$version.strace" 2>/dev/null &
tracer=$!
until grep -q '"step": "run"' "$out-$version.jsonl" 2>/dev/null; do sleep 0.2; done
kill -INT $tracer; wait $tracer
after=$(cut -d')' -f2 /proc/$pid/task/$tid/stat 2>/dev/null | awk '{print $8, $12, $13}')
echo "owner $tid minflt/utime/stime before: $before after: $after" > "$out-$version.faults"
wait $runner
