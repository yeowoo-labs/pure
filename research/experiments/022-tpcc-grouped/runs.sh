#!/bin/sh
# The runs of docs/research/106, as they were made, from the repository's root
# (the SD card's directory is any on ext4 there). Each result file records
# SQLite's pragmas and page size in its lines.
set -e
out=research/experiments/022-tpcc-grouped
sd=${SD:-$HOME/tpcc-sd}
run() {
    name=$1
    shift
    python3 benchmarks/tpcc/run.py "$@" > $out/2026-09-30-$name-linux-aarch64.jsonl
}
tuned='PRAGMA mmap_size=1073741824; PRAGMA cache_size=-65536; PRAGMA temp_store=MEMORY;'

# SQLite's settings, W=1, SQLite alone (harness of 3c04860).
run tune-ram-defaults --warehouses 1 --check 0 --terminals '' --sqlite-threads 1,4 --sqlite-group 16,64 \
    --seconds 20 --sqlite-pragmas ''
run tune-ram-tuned --warehouses 1 --check 0 --terminals '' --sqlite-threads 1,4 --sqlite-group 16,64 \
    --seconds 20 --sqlite-pragmas "$tuned"
run tune-sd-page4096 --directory $sd --warehouses 1 --check 0 --terminals '' --sqlite-threads 1 --sqlite-group 16,64 \
    --seconds 30 --sqlite-pragmas "$tuned"
run tune-sd-page1024 --directory $sd --warehouses 1 --check 0 --terminals '' --sqlite-threads 1 --sqlite-group 16,64 \
    --seconds 30 --sqlite-page-size 1024 --sqlite-pragmas "$tuned"
# Beyond the criteria, in SQLite's favour: a checkpoint each 40 MB of WAL.
run tune-sd-page1024-checkpoint40000 --directory $sd --warehouses 1 --check 0 --terminals '' --sqlite-threads 1 \
    --sqlite-group 16,64 --seconds 30 --sqlite-page-size 1024 --sqlite-pragmas "$tuned PRAGMA wal_autocheckpoint=40000;"
run tune-ram-checkpoint10000 --warehouses 1 --check 0 --terminals '' --sqlite-threads 1,4 --sqlite-group 16,64 \
    --seconds 20 --sqlite-pragmas "$tuned PRAGMA wal_autocheckpoint=10000;"

# The matrix (harness of 20b313c, SQLite as its tuned() makes it): RAM, then
# the card in three rounds, W=1 and W=4 taking turns.
both='--terminals 16,64 --sqlite-threads 1,4 --sqlite-group 16,64'
run ram-w1 --warehouses 1 --check 20000 $both --seconds 20
run ram-w4 --warehouses 4 --check 20000 $both --seconds 20
for round in 1 2 3; do
    for w in 1 4; do
        run sd-w$w-$round --directory $sd --warehouses $w --check 0 $both --seconds 30 --sqlite-page-size 1024
    done
done

# Where the owner thread is (RAM, W=1, 64 terminals): 80 samples of its stack
# with gdb while Pure alone runs for 90 seconds.
python3 $out/owner-samples.py 80 $out/2026-09-30-owner-stacks-linux-aarch64.txt &
run owner-run --warehouses 1 --check 0 --terminals 64 --sqlite-threads '' --sqlite-group '' --seconds 90
wait
