#!/usr/bin/env bash
# The first hour: the commands of the three READMEs (pure, aup, vega), as written, from clean clones.
#
#   bash tests/first-hour.sh [SOURCE]     SOURCE: a directory holding pure, aup and vega (default: the one this
#                                         checkout is in), or a URL prefix (https://github.com/yeowoo-labs)
#
# Every step looks at three things: the command is still in the README letter for letter (else the README changed
# and this script must follow it), it exits as it should, and its output carries what the README promises. The
# clones, HOME and the stores live in a temporary directory (the README's /tmp/tasks is put there too); the two
# ports the examples use (8767, 8080) must be free. What needs a person is left out: `vega tui` (make check's
# interactive drives it), the framebuffer surface, vega-symbols' font examples.
#
# FULL=1 also runs the three `make check`s. KEEP=1 keeps the directory. Exit: 0 all steps passed, 1 some failed,
# 77 the environment cannot run it (a tool missing, a port in use).
set -u
here=$(cd "$(dirname "$0")/.." && pwd)
source_of=${1:-$(dirname "$here")}
for tool in git make cc curl xxd timeout; do
    command -v "$tool" > /dev/null || { echo "first hour: $tool is not installed"; exit 77; }
done
for port in 8767 8080; do
    if (exec 3<> "/dev/tcp/127.0.0.1/$port") 2> /dev/null; then echo "first hour: port $port is in use"; exit 77; fi
done
W=$(mktemp -d "${TMPDIR:-/tmp}/first-hour.XXXXXX")
export HOME="$W/home"
mkdir -p "$HOME"
pids=()
cleanup() {
    for pid in "${pids[@]:-}"; do [ -n "$pid" ] && kill "$pid" 2> /dev/null; done
    [ "${KEEP:-}" = 1 ] && echo "first hour: kept $W" || rm -rf "$W"
}
trap cleanup EXIT
steps=0 failed=0

fail() { failed=$((failed + 1)); echo "FAIL $*"; }
# readme REPO TEXT...: each TEXT is in that repository's README as it stands
readme() {
    local repo=$1 text
    shift
    for text in "$@"; do
        grep -qF -- "$text" "$W/$repo/README.md" || fail "$repo/README.md no longer has: $text (the README changed: bring this script in line)"
    done
}
# run NAME REPO COMMAND: COMMAND in that clone, its output in $W/out; the status wanted in $want (default 0)
run() {
    local name=$1 repo=$2 status
    steps=$((steps + 1))
    (cd "$W/$repo" && eval "$3") > "$W/out" 2> "$W/err"
    status=$?
    if [ "$status" -ne "${want:-0}" ]; then
        fail "$name: exit $status"
        tail -n 20 "$W/err" "$W/out"
        return 1
    fi
    echo "ok   $name"
}
has() { grep -qF -- "$2" "$W/out" || { fail "$1: the output lacks: $2"; tail -n 5 "$W/out"; }; }
# serve NAME REPO PORT COMMAND: COMMAND in the background, until the port answers (10 s)
serve() {
    local name=$1 repo=$2 port=$3 n=0
    steps=$((steps + 1))
    (cd "$W/$repo" && eval "exec $4") > "$W/served" 2>&1 &
    pids+=($!)
    until (exec 3<> "/dev/tcp/127.0.0.1/$port") 2> /dev/null; do
        n=$((n + 1))
        if [ "$n" -gt 100 ] || ! kill -0 "${pids[-1]}" 2> /dev/null; then fail "$name: nothing answers on port $port"; tail -n 5 "$W/served"; return 1; fi
        sleep 0.1
    done
    echo "ok   $name"
}
stop() { kill "${pids[-1]}" 2> /dev/null; wait "${pids[-1]}" 2> /dev/null; unset 'pids[-1]'; }
http() { curl -s -m 10 -o "$W/out" -w '%{http_code}' "$@"; }
seconds() { local start=$SECONDS; "$@"; took=$((SECONDS - start)); }

case $source_of in
    http://*|https://*) for r in pure aup vega; do git clone -q "$source_of/$r.git" "$W/$r" || { echo "first hour: cannot clone $r"; exit 77; }; done ;;
    *) for r in pure aup vega; do git clone -q "$source_of/$r" "$W/$r" || { echo "first hour: cannot clone $r"; exit 77; }; done ;;
esac
tasks='printf '\''type typed %s\ndown again\ndown toggle\nshow\n'\'' "$(printf '\''milk'\'' | xxd -p)" \'

# ---- pure/README.md: Quick start
readme pure 'build/libpure.a, libpure-link.a, pure-run, pure-serve, pure-crypto.so' 'make -C ../aup' \
    '../aup/build/aup build ../aup/examples/tasks.aup -o tasks.pmod' "$tasks" '  | build/pure-run tasks.pmod --store /tmp/tasks' \
    'build/pure-serve --module tasks.pmod --store /tmp/tasks --threads 2 &' 'curl -s localhost:8767/views/count' \
    "curl -s -X POST -H 'Content-Type: application/json' \\" "  -d '{\"text\": \"ship it\"}' localhost:8767/inputs/typed"
seconds run 'pure: make' pure 'make'
pure_s=$took
for built in libpure.a libpure-link.a pure-run pure-serve pure-crypto.so; do
    [ -e "$W/pure/build/$built" ] || fail "pure: make left no build/$built"
done
seconds run 'pure: make -C ../aup' pure 'make -C ../aup'
aup_s=$took
run 'pure: aup builds tasks' pure '../aup/build/aup build ../aup/examples/tasks.aup -o tasks.pmod'
run 'pure: pure-run tasks' pure "printf 'type typed %s\ndown again\ndown toggle\nshow\n' \"\$(printf 'milk' | xxd -p)\" | build/pure-run tasks.pmod --store $W/tasks"
has 'pure: pure-run tasks' '"successfulEvents":3' && has 'pure: pure-run tasks' '"fault":"none"'
if serve 'pure: pure-serve tasks' pure 8767 "build/pure-serve --module tasks.pmod --store $W/tasks --threads 2"; then
    steps=$((steps + 1))
    code=$(http localhost:8767/views/count); before=$(cat "$W/out")
    posted=$(http -X POST -H 'Content-Type: application/json' -d '{"text": "ship it"}' localhost:8767/inputs/typed)
    again=$(http localhost:8767/views/count); after=$(cat "$W/out")
    if [ "$code $posted $again" = '200 200 200' ] && [ "$after" = "$((before + 1))" ] 2> /dev/null; then echo 'ok   pure: a view, an input, the view again'
    else fail "pure: the service answered $code, $posted, $again; count $before then $after"; fi
    stop
fi

# ---- aup/README.md: Build and run
readme aup '# build/aup' 'build/aup build examples/counter.aup -o counter.pmod' "printf 'down tap\\nup tap\\nshow\\n' | ../pure/build/pure-run counter.pmod" \
    'build/aup build examples/tasks.aup -o tasks.pmod' "$tasks" '  | ../pure/build/pure-run tasks.pmod --store /tmp/tasks' \
    'build/aup watch examples/tasks.aup -o tasks.pmod'
run 'aup: make' aup 'make'
run 'aup: counter builds' aup 'build/aup build examples/counter.aup -o counter.pmod'
run 'aup: pure-run counter' aup "printf 'down tap\nup tap\nshow\n' | ../pure/build/pure-run counter.pmod"
has 'aup: pure-run counter' '"states":{"count":1}'
run 'aup: tasks builds' aup 'build/aup build examples/tasks.aup -o tasks.pmod'
run 'aup: pure-run tasks' aup "printf 'type typed %s\ndown again\ndown toggle\nshow\n' \"\$(printf 'milk' | xxd -p)\" | ../pure/build/pure-run tasks.pmod --store $W/tasks-aup"
has 'aup: pure-run tasks' '"fault":"none"'
want=124 run 'aup: watch builds and waits' aup 'timeout 3 build/aup watch examples/tasks.aup -o tasks.pmod 2>&1'
has 'aup: watch builds and waits' 'built tasks.pmod'

# ---- vega/README.md: Build and run, The same app for a person and a program
readme vega '# build/vega, build/vega-symbols' 'AUP_PATH=lib ../aup/build/aup build examples/todo.aup -o todo.pmod' \
    "printf 'text 6d696c6b\\nkey Space\\nshow\\n' | build/vega cli todo.pmod" "printf 'text 6d696c6b\\nkey Space\\nshow\\n' | build/vega say todo.pmod" \
    'build/vega web todo.pmod --watch' 'AUP_PATH=lib ../aup/build/aup build examples/board.aup -o board.pmod' \
    '../pure/build/pure-serve --module board.pmod --store ~/.board --threads 2 &' 'curl -s localhost:8767/views/counts' \
    "  -d '{\"title\": \"ship it\", \"lane\": 2}' localhost:8767/inputs/post"
seconds run 'vega: make' vega 'make'
vega_s=$took
for built in vega vega-symbols; do [ -e "$W/vega/build/$built" ] || fail "vega: make left no build/$built"; done
run 'vega: todo builds' vega 'AUP_PATH=lib ../aup/build/aup build examples/todo.aup -o todo.pmod'
run 'vega: cli' vega "printf 'text 6d696c6b\nkey Space\nshow\n' | build/vega cli todo.pmod"
has 'vega: cli' 'To do' && has 'vega: cli' '> milk [done]'
run 'vega: say' vega "printf 'text 6d696c6b\nkey Space\nshow\n' | build/vega say todo.pmod"
has 'vega: say' 'Item 1 of 1: milk, done.'
if serve 'vega: web' vega 8080 'build/vega web todo.pmod --watch'; then
    steps=$((steps + 1))
    url=$(grep -o 'http://[^"]*' "$W/served" | head -n 1)
    code=$(http "$url")
    if [ -n "$url" ] && [ "$code" = 200 ]; then echo 'ok   vega: the page it names'; else fail "vega: web named '$url', which answered $code"; fi
    stop
fi
run 'vega: board builds' vega 'AUP_PATH=lib ../aup/build/aup build examples/board.aup -o board.pmod'
if serve 'vega: pure-serve board' vega 8767 '../pure/build/pure-serve --module board.pmod --store ~/.board --threads 2'; then
    steps=$((steps + 1))
    code=$(http localhost:8767/views/counts); before=$(cat "$W/out")
    posted=$(http -X POST -H 'Content-Type: application/json' -d '{"title": "ship it", "lane": 2}' localhost:8767/inputs/post)
    again=$(http localhost:8767/views/counts); after=$(cat "$W/out")
    case "$code $posted $again|$before|$after" in
        '200 200 200|'*'0 done|'*'1 done') echo 'ok   vega: the board over HTTP' ;;
        *) fail "vega: the board answered $code, $posted, $again; '$before' then '$after'" ;;
    esac
    stop
fi

if [ "${FULL:-}" = 1 ]; then
    readme pure 'make check' && readme aup 'make check' && readme vega 'make check'
    run 'aup: make check' aup 'make check'
    run 'pure: make check' pure 'make check'
    run 'vega: make check' vega 'make check'
fi
echo "first hour: $steps steps, $failed failed, builds ${pure_s:-?}/${aup_s:-?}/${vega_s:-?} s (pure/aup/vega)"
[ "$failed" -eq 0 ]
