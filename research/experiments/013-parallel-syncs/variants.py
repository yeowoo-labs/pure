#!/usr/bin/env python3
"""Where the tree store's extra sync work goes (docs/research/084-parallel-syncs.md).

Builds pure-serve with stores from store/store-tree.c: as it is; with no
compaction; with no compaction and a 64 MiB tail written at creation (no tail
growth during a run); and as before 085, compacted files getting only 256 KiB
of zeros and commits growing the tail as they need it. Then durable score posts (64 connections), the
builds in rotated order per round, each on a fresh store after 64 warm-up
posts. Prints one JSON line per run and a summary.
"""
import argparse
import json
import select
import shutil
import statistics
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
HTTP = ROOT / 'benchmarks' / 'http'
PATCHES = {
    'as it is': [],
    'no compaction': [('enum { COMPACT_SLACK = 1048576 };', 'enum { COMPACT_SLACK = 1 << 30 };')],
    'no compaction, no growth': [('enum { COMPACT_SLACK = 1048576 };', 'enum { COMPACT_SLACK = 1 << 30 };'),
                                 ('TAIL_STEP = 262144', 'TAIL_STEP = 67108864')],
    # Before 085: a compacted file got 256 KiB of zeros, and commits grew the tail as they needed it.
    'tail grown as commits need it': [('zero_fill(fd, head->end, cycle_end(head, replaced));',
                                       'zero_fill(fd, head->end, tail_end(head->end));')],
}


def build(work, name, patches):
    source = (ROOT / 'store' / 'store-tree.c').read_text()
    for old, new in patches:
        assert old in source, old
        source = source.replace(old, new)
    store = work / f'store-{len(name)}.c'
    store.write_text(source)
    binary = work / f'pure-serve-{len(name)}'
    subprocess.run(['cc', '-O2', '-std=c17', f'-I{ROOT / "core"}', f'-I{ROOT / "serve"}', '-DPURE_ASYNC_EFFECTS', '-pthread',
                    *[str(ROOT / s) for s in ('serve/serve.c', 'serve/deliver.c', 'serve/message-json.c', 'serve/crypto.c',
                                              'core/module.c',
                                              'core/module-list.c', 'core/module-save.c', 'core/module-text.c',
                                              'core/module-store.c')], str(store), str(ROOT / 'core/runtime.c'),
                    '-lm', '-o', str(binary)], check=True)
    return binary


def posts(binary, module, directory, seconds):
    store = Path(tempfile.mkdtemp(dir=directory, prefix='pure-store-variant-'))
    shutil.rmtree(store)
    process = subprocess.Popen(['taskset', '-c', '0-2', str(binary), '--module', str(module), '--store', str(store),
                                '--port', '0'], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    try:
        ready, _, _ = select.select([process.stdout], [], [], 20)
        assert ready, 'pure-serve did not start'
        port = json.loads(process.stdout.readline())['port']
        load = ['taskset', '-c', '3', str(HTTP / 'load'), str(port), '64']
        subprocess.run(load + ['1', 'score', '64', 'keep-alive', '16'], capture_output=True, check=True)
        measured = json.loads(subprocess.run(load + [str(seconds), 'score', '64', 'keep-alive', '16'], capture_output=True,
                                             text=True, check=True).stdout)
        return {'posts_per_second': measured['2xx'] / seconds, 'p50_us': measured['p50_us'], 'p99_us': measured['p99_us'],
                'max_us': measured['max_us'], 'errors': measured['errors'] + measured['4xx'] + measured['5xx']}
    finally:
        process.terminate()
        process.wait()
        shutil.rmtree(store, ignore_errors=True)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--directory', type=Path, default=Path('/var/tmp'), help='on the device to measure')
    parser.add_argument('--aup', type=Path, default=ROOT.parent / 'aup' / 'build' / 'aup')
    parser.add_argument('--rounds', type=int, default=6)
    parser.add_argument('--seconds', type=float, default=4)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory() as work:
        work = Path(work)
        module = work / 'scores.pmod'
        subprocess.run([str(args.aup), 'build', str(HTTP / 'scores.aup'), '-o', str(module)], check=True)
        subprocess.run(['cc', '-O2', '-std=c17', '-o', str(HTTP / 'load'), str(HTTP / 'load.c')], check=True)
        binaries = {name: build(work, name, patches) for name, patches in PATCHES.items()}
        names, runs = list(binaries), {name: [] for name in binaries}
        for round_ in range(args.rounds):
            for name in names[round_ % len(names):] + names[:round_ % len(names)]:
                measured = posts(binaries[name], module, args.directory, args.seconds)
                runs[name].append(measured)
                print(json.dumps({'round': round_, 'store': name, **measured}), flush=True)
        for name in names:
            print(json.dumps({'store': name, 'rounds': args.rounds,
                              'posts_per_second_median': statistics.median(r['posts_per_second'] for r in runs[name]),
                              'p99_us_median': statistics.median(r['p99_us'] for r in runs[name]),
                              'max_us': max(r['max_us'] for r in runs[name])}))


if __name__ == '__main__':
    sys.exit(main())
