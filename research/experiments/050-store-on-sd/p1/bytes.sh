#!/bin/bash
# bytes.sh OUT VERSION: one TPC-C run with the owner's pwrite64/fdatasync/renameat traced for 3 s mid-run
out=$1 version=$2
here=$(dirname $0)
python3 benchmarks/tpcc/run.py --serve "$HOME/.cache/pure-040/$version/pure-serve" \
    --warehouses 1 --check 0 --terminals 64 --sqlite-threads '' --sqlite-group '' --seconds 20 > "$out-$version.jsonl" &
runner=$!
until grep -q '"step": "load"' "$out-$version.jsonl" 2>/dev/null; do sleep 0.2; done
sleep 5
pid=$(pgrep -x pure-serve | head -1)
tid=""
for t in /proc/$pid/task/*; do [ "$(cat $t/comm)" = pure-owner ] && tid=$(basename $t); done
timeout -s INT 3 strace -p $tid -e trace=pwrite64,fdatasync,renameat,openat -e abbrev=all -s 0 -o "$out-$version.calls" 2>/dev/null
wait $runner
