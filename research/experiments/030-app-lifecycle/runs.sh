#!/bin/sh
# The runs of research 115 (docs/design/017's stage 3), from the repository's
# root, in steps, each under research/guard.py:
#   runs.sh build NAME COMMIT   pure-serve of COMMIT, with its pure-crypto.so,
#                               in ~/.cache/pure-030/NAME
#   runs.sh window NAME ITEMS CHANGES RESTARTS [MODULE]   window.py on that
#                               build, into DATE-window-NAME-ITEMS[-MODULE].jsonl
set -e
out=research/experiments/030-app-lifecycle
builds=$HOME/.cache/pure-030
day=$(date +%F)
case "$1" in
build)
    rm -rf "$builds/src-$2"
    git worktree add -f -q --detach "$builds/src-$2" "$3"
    make -s -C "$builds/src-$2" build/pure-serve build/pure-crypto.so
    mkdir -p "$builds/$2"
    cp "$builds/src-$2/build/pure-serve" "$builds/src-$2/build/pure-crypto.so" "$builds/$2/"
    git worktree remove --force "$builds/src-$2" ;;
window)
    until awk '{ exit !($1 < 1.5) }' /proc/loadavg; do sleep 10; done
    python3 research/guard.py --most 2048 -- python3 $out/window.py "$builds/$2/pure-serve" "$3" "$4" "$5" ${6:+"$6"} \
        > "$out/$day-window-$2-$3${6:+-$6}.jsonl" ;;
*) echo "runs.sh build NAME COMMIT | window NAME ITEMS CHANGES RESTARTS [MODULE]" >&2; exit 2 ;;
esac
