#!/bin/sh
# The runs of research 122 (docs/design/020's stage 2), from the repository's
# root, in steps, each under research/guard.py:
#   runs.sh build NAME COMMIT   pure-serve of COMMIT, with its pure-crypto.so,
#                               in ~/.cache/pure-037/NAME
#   runs.sh tail ROUND          views.py on the after build: writes alone, beside
#                               2 askers of the BFS view, and beside 2 askers
#                               of another host (apart); 20,000 nodes, 100,000
#                               edges, connection threads 3, 10 s, 3 rounds
#   runs.sh control ROUND       the same with --view-workers 0 (the views on the
#                               connection threads, as before)
#   runs.sh scaling ROUND       views.py, connection threads 1: one asker, two
#   runs.sh plain               plain.py: GET /views/total (64 connections,
#                               10 s), before (9484700) and after, 6 pairs
set -e
here=research/experiments/037-view-workers
builds=$HOME/.cache/pure-037
day=$(date +%F)
views() {
    until awk '{ exit !($1 < 1.5) }' /proc/loadavg; do sleep 10; done
    python3 research/guard.py --most 2048 -- python3 $here/views.py "$builds/after/pure-serve" ../aup/build/aup \
        20000 100000 "$1" 2 10 3 "$2"
}
case "$1" in
build)
    rm -rf "$builds/src-$2"
    git worktree add -f -q --detach "$builds/src-$2" "$3"
    make -s -C "$builds/src-$2" build/pure-serve build/pure-crypto.so
    mkdir -p "$builds/$2"
    cp "$builds/src-$2/build/pure-serve" "$builds/src-$2/build/pure-crypto.so" "$builds/$2/"
    git worktree remove --force "$builds/src-$2" ;;
tail) views 3 writes,both,apart > "$here/$day-tail-$2.jsonl" ;;
control) VIEWS_SERVE_ARGS='--view-workers 0' views 3 writes,both,apart > "$here/$day-control-$2.jsonl" ;;
scaling) views 1 one,many > "$here/$day-scaling-$2.jsonl" ;;
plain)
    until awk '{ exit !($1 < 1.5) }' /proc/loadavg; do sleep 10; done
    python3 research/guard.py -- python3 $here/plain.py "$builds/before/pure-serve" "$builds/after/pure-serve" \
        ../aup/build/aup 10 6 > "$here/$day-plain.jsonl" ;;
*) echo "runs.sh build NAME COMMIT | tail ROUND | control ROUND | scaling ROUND | plain" >&2
    exit 2 ;;
esac
