#!/bin/sh
# Coverage fuzzing of the loader, the verifier and events on a loaded module (tests/fuzz-load.c, tests/fuzz-run.c).
#
#   sh tests/fuzz.sh corpus [AUP_ROOT [VEGA_ROOT]]   the seeds: every .aup of aup, vega and this tree that builds,
#                                                    into build/fuzz-corpus/load (modules) and .../run (a module and
#                                                    a few empty messages)
#   sh tests/fuzz.sh load|run MINUTES [WORKERS]      fuzz for MINUTES (three workers: a core is left); the corpus
#                                                    grows in place, a crash is left in build/fuzz-crashes/
#
# Not part of make check: it runs for as long as it is given. research/guard.py keeps it within 2 GiB.
set -eu
cd "$(dirname "$0")/.."
case ${1:-} in
corpus)
    aup_root=${2:-../aup} vega_root=${3:-../vega}
    aup=$aup_root/build/aup
    mkdir -p build/fuzz-corpus/load build/fuzz-corpus/run
    built=0
    for source in $(find "$aup_root/examples" "$aup_root/tests" "$vega_root/examples" benchmarks research/experiments tests -name '*.aup' -not -path '*/build/*' | sort); do
        name=$(printf '%s' "$source" | cksum | cut -d' ' -f1)
        if AUP_PATH=$vega_root/lib "$aup" build "$source" -o "build/fuzz-corpus/load/$name.pmod" 2> /dev/null; then built=$((built + 1)); fi
    done
    python3 - <<'PY'
import struct
from pathlib import Path
for module in sorted(Path('build/fuzz-corpus/load').glob('*.pmod')):
    data = module.read_bytes()
    messages = b''.join(bytes([i]) + struct.pack('<H', n) + bytes(n) for i in range(4) for n in (0, 8))
    (Path('build/fuzz-corpus/run') / module.name).write_bytes(struct.pack('<I', len(data)) + data + messages)
PY
    echo "fuzz corpus: $built modules built, $(ls build/fuzz-corpus/load | wc -l) seeds to load, $(ls build/fuzz-corpus/run | wc -l) to run" ;;
load|run)
    minutes=${2:?minutes} workers=${3:-3}
    root=$(pwd)
    mkdir -p build/fuzz-crashes "build/fuzz-corpus/$1" "build/fuzz-logs/$1"
    # (ASan keeps freed blocks aside, 256 MiB of them by default: with a corpus of thousands that alone passes the
    # workers' 600 MiB. 32 MiB keeps a use after free seen and the workers near 150 MiB.)
    export ASAN_OPTIONS="${ASAN_OPTIONS:-quarantine_size_mb=32}"
    cd "build/fuzz-logs/$1"  # each worker's log (fuzz-N.log) lands where it runs
    rm -f fuzz-*.log
    python3 "$root/research/guard.py" --most 2048 -- "$root/build/fuzz-$1" -max_total_time=$((60 * minutes)) -jobs="$workers" \
        -workers="$workers" -rss_limit_mb=600 -timeout=10 -max_len=262144 -artifact_prefix="$root/build/fuzz-crashes/" \
        "$root/build/fuzz-corpus/$1" > run.log 2>&1 || true
    echo "fuzz $1: $minutes minutes, $workers workers; $(grep -h 'cov: ' fuzz-*.log 2> /dev/null | tail -n 1 | grep -o 'cov: [0-9]* ft: [0-9]*'); crashes: $(ls "$root/build/fuzz-crashes" | wc -l)" ;;
*)
    echo "sh tests/fuzz.sh corpus [AUP_ROOT [VEGA_ROOT]] | load MINUTES [WORKERS] | run MINUTES [WORKERS]"
    exit 2 ;;
esac
