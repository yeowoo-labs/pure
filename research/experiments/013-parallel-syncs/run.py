#!/usr/bin/env python3
"""Does the device sync several stores at once? (docs/research/084-parallel-syncs.md)

1. probe.c: 1, 2 and 4 threads, each committing to a file of its own
   (bytes over zeros, fdatasync), the order rotated per round.
2. The scoreboard app (benchmarks/http/scores.aup) taking durable posts:
   one pure-serve owning one store under 64 connections, against two
   pure-serve, each owning a store of its own (what sharding by key would
   make), under 32 connections each at once. Servers on CPUs 0 and 1, load
   generators on 3 and 2.
Prints one JSON document; each round also goes to stderr as it ends.
"""
import argparse
import json
import select
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
HTTP = ROOT / 'benchmarks' / 'http'


def start(cpu, module, store):
    process = subprocess.Popen(['taskset', '-c', str(cpu), str(ROOT / 'build/pure-serve'), '--module', str(module),
                                '--store', str(store), '--port', '0'], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    ready, _, _ = select.select([process.stdout], [], [], 20)
    assert ready, 'pure-serve did not start'
    return process, json.loads(process.stdout.readline())['port']


def loads(targets, seconds):
    """Load generators at once, one per (cpu, port, connections); their JSON."""
    running = [subprocess.Popen(['taskset', '-c', str(cpu), str(HTTP / 'load'), str(port), str(connections),
                                 str(seconds), 'score', str(connections), 'keep-alive', '16'],
                                stdout=subprocess.PIPE, text=True) for cpu, port, connections in targets]
    return [json.loads(process.communicate()[0]) for process in running]


def owners(count, module, directory, seconds):
    stores = [Path(tempfile.mkdtemp(dir=directory, prefix='pure-sync-owner-')) for _ in range(count)]
    servers = []
    try:
        for i, store in enumerate(stores):
            shutil.rmtree(store)
            servers.append(start(i, module, store))
        measured = loads([(3 - i, port, 64 // count) for i, (_, port) in enumerate(servers)], seconds)
        return {'owners': count, 'posts_per_second': sum(m['2xx'] for m in measured) / seconds,
                'p50_us': max(m['p50_us'] for m in measured), 'p99_us': max(m['p99_us'] for m in measured),
                'errors': sum(m['errors'] + m['4xx'] + m['5xx'] for m in measured)}
    finally:
        for process, _ in servers:
            process.terminate()
            process.wait()
        for store in stores:
            shutil.rmtree(store, ignore_errors=True)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--directory', type=Path, default=Path('/var/tmp'), help='on the device to measure')
    parser.add_argument('--aup', type=Path, default=ROOT.parent / 'aup' / 'build' / 'aup')
    parser.add_argument('--rounds', type=int, default=3)
    parser.add_argument('--seconds', type=float, default=3)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory() as work:
        work = Path(work)
        subprocess.run(['cc', '-O2', '-std=c17', '-pthread', '-o', str(work / 'probe'), str(HERE / 'probe.c')], check=True)
        subprocess.run(['cc', '-O2', '-std=c17', '-o', str(HTTP / 'load'), str(HTTP / 'load.c')], check=True)
        module = work / 'scores.pmod'
        subprocess.run([str(args.aup), 'build', str(HTTP / 'scores.aup'), '-o', str(module)], check=True)
        results = {'rounds': []}
        for round_ in range(args.rounds):
            measured = {'probe': [], 'app': []}
            threads = [1, 2, 4]
            threads = threads[round_ % 3:] + threads[:round_ % 3]
            for count in threads:
                probe = subprocess.run([str(work / 'probe'), str(args.directory), str(count), str(args.seconds), '4096'],
                                       capture_output=True, text=True, check=True)
                measured['probe'].append(json.loads(probe.stdout))
            for count in ((1, 2) if round_ % 2 == 0 else (2, 1)):
                measured['app'].append(owners(count, module, args.directory, args.seconds))
            results['rounds'].append(measured)
            print(json.dumps(measured), file=sys.stderr, flush=True)
        print(json.dumps(results, indent=1))


if __name__ == '__main__':
    sys.exit(main())
