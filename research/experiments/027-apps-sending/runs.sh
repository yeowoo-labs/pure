#!/bin/sh
# The runs of docs/design/017's second stage, from the repository's root:
# two apps of one host, sending through the local outlet or over a
# loopback link, five rounds of 2,000 bursts (2,000,000 sends), the ways in
# turns, once the last minute's load is below 1.5.
set -e
out=research/experiments/027-apps-sending
until awk '{ exit !($1 < 1.5) }' /proc/loadavg; do sleep 10; done
echo "$(uptime)" > $out/2026-10-01-load.txt
make -s build/pure-serve
python3 $out/sends.py build/pure-serve 5 2000 > $out/2026-10-01-sends-linux-aarch64.jsonl
echo "$(uptime)" >> $out/2026-10-01-load.txt
