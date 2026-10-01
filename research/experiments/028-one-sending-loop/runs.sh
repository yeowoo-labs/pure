#!/bin/sh
# The runs of docs/design/017's 2b, from the repository's root, in steps
# (a step at a time: a background job may be stopped after half an hour).
#   runs.sh build   the builds, each in /tmp/pure-028/NAME with its
#                   pure-crypto.so: before (a thread a link, 3791ab8),
#                   threads-push (before with threads-push.patch: the
#                   owner pushes a turn's sends at once), loop (one
#                   sending loop, 32f96b3), push (the loop with push,
#                   6d3c3c2)
#   runs.sh links   five rounds of 1,000 bursts by one link, four links and
#                   one wire, the builds in turns, once the last minute's
#                   load is below 1.5
set -e
out=research/experiments/028-one-sending-loop
builds=/tmp/pure-028
case "$1" in
build)
    for pair in before:3791ab8 threads-push:3791ab8 loop:32f96b3 push:6d3c3c2; do
        name=${pair%%:*} commit=${pair#*:}
        rm -rf $builds/src-$name
        git worktree add -f -q --detach $builds/src-$name $commit
        if [ $name = threads-push ]; then git -C $builds/src-$name apply "$PWD/$out/threads-push.patch"; fi
        make -s -C $builds/src-$name build/pure-serve build/pure-crypto.so
        mkdir -p $builds/$name
        cp $builds/src-$name/build/pure-serve $builds/src-$name/build/pure-crypto.so $builds/$name/
        git worktree remove --force $builds/src-$name
    done ;;
links)
    until awk '{ exit !($1 < 1.5) }' /proc/loadavg; do sleep 10; done
    echo "$(uptime)" > $out/2026-10-01-links-load.txt
    python3 $out/sends.py 5 1000 link,links4,wire before=$builds/before/pure-serve \
        threads-push=$builds/threads-push/pure-serve loop=$builds/loop/pure-serve push=$builds/push/pure-serve \
        > $out/2026-10-01-links-linux-aarch64.jsonl
    echo "$(uptime)" >> $out/2026-10-01-links-load.txt ;;
*) echo "runs.sh build|links" >&2; exit 2 ;;
esac
