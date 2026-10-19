#!/bin/sh
# Research 140: compile each probe with aup and print what it says. A probe is
# the first thing a spec in the suite needs (a byte at a position, a bit
# operation, a record inside a record, a call to itself, a variant, a
# character at a position). AUP is the aup checkout (default ../aup beside
# Pure); the output is the evidence table in 140. The probes are as written on
# 2026-10-13 (probes/), or a directory given (probes-027/: the same needs in
# the forms 027 gave them, and the gaps found after it).
set -u
here=$(cd "$(dirname "$0")" && pwd)
aup=${AUP:-$here/../../../../aup}/build/aup
dir=${1:-$here/probes}
dir=$(cd "$dir" && pwd)
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT
for f in "$dir"/*.aup; do
    printf '%s\t' "$(basename "$f")"
    if said=$("$aup" build "$f" -o "$out/probe.pmod" 2>&1); then echo "compiles"; else echo "$said" | head -1 | sed "s|^$dir/||"; fi
done
