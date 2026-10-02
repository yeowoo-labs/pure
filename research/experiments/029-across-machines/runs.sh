#!/bin/sh
# Research 029: Pure, aup and their checks on other machines (windows: WSL2
# on x86_64, 4 KiB pages, gcc 13; mac: macOS on an M3, Apple clang 21,
# where the Pi is Linux on AArch64, 16 KiB pages, gcc 14), and every
# pure-run of aup's check compared run by run. In steps:
#   runs.sh ship [HOST]   (on the Pi) Pure's and aup's tracked files as they
#                         are (git stash create), and this experiment, to
#                         HOST (default windows, into its WSL) under
#                         ~/pure-029 there
#   runs.sh setup         (there) the Pi's Node and OpenSSL (Pure seals with
#                         HPKE, OpenSSL 3.2 on; Ubuntu 24.04 has 3.0) under
#                         ~/pure-029/opt, from their releases: nothing of
#                         the system's changes
#   runs.sh record NAME   aup's check with every pure-run recorded
#                         (recorded-pure-run), into NAME.jsonl beside this
#   runs.sh check         make check of Pure and aup (each stops at its
#                         first failure)
#   runs.sh units         the core's and the store's own tests (all a Mac
#                         builds: pure-serve is Linux's)
#   runs.sh checks NAME   Pure's tests/check.py a check at a time, going on
#                         past a failure (each_check.py), into
#                         NAME-checks.jsonl
#   runs.sh clock S NAME  whether the wall clock jumps against the
#                         monotonic one over S seconds (clock.py), into
#                         NAME-clock.jsonl
#   runs.sh stores make DIR | open DIR OUT   stores made on one machine,
#                         opened and written on another (stores.py)
#   runs.sh compare A B   two records, run by run (compare.py)
# Everything there is in ~/pure-029: removing it removes all of it. On the
# Pi a step runs under research/guard.py (a run that outgrows its memory
# freezes the Pi), its stores in ~/.cache, not a tmpfs.
set -e
here=$(cd "$(dirname "$0")" && pwd)
pure=$(cd "$here/../../.." && pwd)
top=$(dirname "$pure")
aup=$top/aup
if [ -d "$top/opt/node/bin" ]; then PATH=$top/opt/node/bin:$PATH; export PATH; fi
if [ -d "$top/opt/openssl" ]; then
    CPPFLAGS="-I$top/opt/openssl/include"
    LDFLAGS="-L$top/opt/openssl/lib64 -Wl,-rpath,$top/opt/openssl/lib64"
    export CPPFLAGS LDFLAGS
fi
if [ -d /opt/homebrew/bin ]; then # a Mac: Homebrew's Python, Node and GNU make (its own make is 3.81)
    PATH=/opt/homebrew/bin:$PATH
    export PATH
    make() { gmake "$@"; }
    # A default SDK newer than the linker links nothing (macOS 27's lists an
    # arm64e.x1 that ld-1267 does not know): the newest that links.
    if ! printf 'int main(void) { return 0; }\n' | cc -x c - -o "${TMPDIR:-/tmp}/pure-029-link" 2>/dev/null; then
        for sdk in $(ls -dr /Library/Developer/CommandLineTools/SDKs/MacOSX[0-9]*.sdk); do
            if printf 'int main(void) { return 0; }\n' | SDKROOT=$sdk cc -x c - -o "${TMPDIR:-/tmp}/pure-029-link" 2>/dev/null
            then SDKROOT=$sdk; export SDKROOT; break; fi
        done
    fi
fi
NODE=24.21.0 OPENSSL=3.5.7 # the Pi's
case "$1" in
ship)
    host=${2:-windows}
    work=$(mktemp -d)
    tree=$(git -C "$pure" stash create) # the working tree's tracked files, committed or not
    git -C "$pure" archive --prefix=pure/ -o "$work/pure.tar" "${tree:-HEAD}"
    git -C "$pure" ls-files --others --exclude-standard -- research/experiments/029-across-machines |
        tar -C "$pure" --transform 's,^,pure/,' -rf "$work/pure.tar" -T - # this experiment as it is
    tree=$(git -C "$aup" stash create)
    git -C "$aup" archive --prefix=aup/ -o "$work/aup.tar" "${tree:-HEAD}"
    scp -q "$work/pure.tar" "$work/aup.tar" "$host:"
    rm -r "$work"
    if [ "$host" = windows ]; then
        ssh "$host" 'wsl.exe -d Ubuntu-24.04 --cd ~ -- sh -c "mkdir -p pure-029 && cd pure-029 && rm -rf pure aup && tar -xf /mnt/c/Users/user/pure.tar && tar -xf /mnt/c/Users/user/aup.tar && rm /mnt/c/Users/user/pure.tar /mnt/c/Users/user/aup.tar && echo shipped"'
    else
        ssh "$host" 'mkdir -p pure-029 && cd pure-029 && rm -rf pure aup && tar -xf ../pure.tar && tar -xf ../aup.tar && rm ../pure.tar ../aup.tar && echo shipped'
    fi ;;
setup)
    mkdir -p "$top/opt" "$top/src"
    if [ ! -x "$top/opt/node/bin/node" ]; then
        curl -fsSL "https://nodejs.org/dist/v$NODE/node-v$NODE-linux-x64.tar.xz" | tar -xJ -C "$top/opt"
        mv "$top/opt/node-v$NODE-linux-x64" "$top/opt/node"
    fi
    if [ ! -f "$top/opt/openssl/include/openssl/hpke.h" ]; then
        curl -fsSL "https://github.com/openssl/openssl/releases/download/openssl-$OPENSSL/openssl-$OPENSSL.tar.gz" |
            tar -xz -C "$top/src"
        (cd "$top/src/openssl-$OPENSSL" && ./Configure --prefix="$top/opt/openssl" --libdir=lib64 no-docs no-tests >/dev/null &&
            make -s -j"$(nproc)" >/dev/null && make -s install_sw >/dev/null)
    fi
    "$top/opt/node/bin/node" --version
    LD_LIBRARY_PATH="$top/opt/openssl/lib64" "$top/opt/openssl/bin/openssl" version ;;
record)
    make -s -C "$pure" build/pure-run build/pure-edit-module
    make -s -C "$pure/prototypes/connections" BUILD=build/tree STORE=store-tree.c build/tree/pure-edit
    make -s -C "$aup" build/aup
    rm -f "$here/$2.jsonl"
    PURE_RUN="$pure/build/pure-run" PURE_RUN_LOG="$here/$2.jsonl" PYTHONHASHSEED=0 \
        python3 "$aup/tests/check.py" --aup "$aup/build/aup" --pure-run "$here/recorded-pure-run" --pure "$pure" \
        > "$here/$2-check.json" ;;
check)
    make -C "$pure" check AUP="$aup"
    make -C "$aup" check ;;
checks)
    make -s -C "$pure" all build/module-probe build/clock-back.so
    make -s -C "$pure/prototypes/connections" BUILD=build/tree STORE=store-tree.c build/tree/pure-connections
    make -s -C "$aup" build/aup
    python3 "$here/each_check.py" "$aup/build/aup" > "$here/$2-checks.jsonl" ;;
units)
    make -s -C "$pure" build/test-module build/test-tree-store build/test-log
    "$pure/build/test-tree-store"
    "$pure/build/test-log"
    "$pure/build/test-module" ;;
clock)
    python3 "$here/clock.py" "$2" > "$here/$3-clock.jsonl" ;;
stores)
    make -s -C "$pure" build/pure-run
    make -s -C "$aup" build/aup
    shift
    python3 "$here/stores.py" "$@" ;;
compare)
    python3 "$here/compare.py" "$here/$2.jsonl" "$here/$3.jsonl" ;;
*) echo "runs.sh ship [HOST]|setup|record NAME|check|checks NAME|clock S NAME|stores ...|compare A B" >&2; exit 2 ;;
esac
