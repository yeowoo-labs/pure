#!/bin/sh
# Research 140: compile each probe with aup and print what it says. A probe is
# the first thing a spec in the suite needs (a byte at a position, a bit
# operation, a record inside a record, a call to itself, a variant, a
# character at a position). AUP is the aup checkout (default ../aup beside
# Pure); the output is the evidence table in 140.
set -u
here=$(cd "$(dirname "$0")" && pwd)
aup=${AUP:-$here/../../../../aup}/build/aup
for f in "$here"/probes/*.aup; do
    printf '%s\t' "$(basename "$f")"
    if out=$("$aup" build "$f" -o /dev/null 2>&1); then echo "compiles"; else echo "$out" | head -1 | sed "s|^$here/probes/||"; fi
done
