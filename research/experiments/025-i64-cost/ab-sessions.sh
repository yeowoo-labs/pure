#!/bin/zsh
# ab-sessions.sh DIR A B SESSIONS: ab.py on fresh processes each session,
# pure-serve-A and pure-serve-B from DIR, which starts first alternating;
# each session waits for a load average below 2.
here=${0:A:h}
dir=$1 a=$2 b=$3 n=$4
for i in $(seq 1 $n); do
  until [ "$(cut -d' ' -f1 /proc/loadavg | cut -d. -f1)" -lt 2 ]; do sleep 5; done
  if (( i % 2 )); then first=$a second=$b; else first=$b second=$a; fi
  echo "== session $i first=$first $(cut -d' ' -f1-3 /proc/loadavg)"
  python3 $here/ab.py $dir/pure-serve-$first $dir/pure-serve-$second 6 30 2>&1
done
echo "== end"
