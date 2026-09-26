#!/usr/bin/env python3
"""HTTP benchmarks: Pure's document servers against axum (Rust) and drogon (C++)
services with the same contract, on the same machine, with the same load
generator.

Servers are pinned to CPUs 0-2 and the load to CPU 3; stores live under
--directory (use the device you care about: fsync costs are the device's).
Prints one JSON object per (server, workload, round) and a summary table of
medians over rounds. The load generator has one core, so near its limit the
servers' CPU time tells them apart: 2xx/cpu-s is answers per second of server
CPU (all threads, user and system).

Durable writes are only comparable on a device in the same state. An SD card
slows down under sustained syncs, so the order of runs alone can change a
result several times over. Each round therefore runs the servers in a
rotated order, each on a fresh store, and every write workload first times
the device (an in-place 4 KiB write + fdatasync); the table shows that probe
next to each result.

--suite app serves the scoreboard of scores.aup (built by aup) with pure-serve,
against the same scoreboard written by hand in axum and drogon: durable score
posts, a sum and a page of 20 rows as JSON.
"""
import argparse
import json
import os
import re
import select
import shutil
import signal
import statistics
import struct
import subprocess
import sys
import tempfile
import time
import http.client
from pathlib import Path

HERE = Path(__file__).resolve().parent
PURE = HERE.parent.parent / 'prototypes' / 'connections'


def start(args):
    process = subprocess.Popen(['taskset', '-c', '0-2'] + [str(a) for a in args],
                               stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    ready, _, _ = select.select([process.stdout], [], [], 20)
    assert ready, 'server did not start'
    port = json.loads(process.stdout.readline())['port']
    return process, port


def cpu_seconds(pid):
    """User and system time of every thread of the process."""
    fields = Path(f'/proc/{pid}/stat').read_text().rsplit(')', 1)[1].split()
    return (int(fields[11]) + int(fields[12])) / os.sysconf('SC_CLK_TCK')


def rss_kib(pid, field):
    for line in Path(f'/proc/{pid}/status').read_text().splitlines():
        if line.startswith(field):
            return int(line.split()[1])
    return 0


def put(port, key, body, expected):
    connection = http.client.HTTPConnection('127.0.0.1', port, timeout=30)
    headers = {'Content-Length': str(len(body) + 8)}
    headers['If-None-Match' if expected == 0 else 'If-Match'] = '*' if expected == 0 else f'"{expected}"'
    connection.request('PUT', f'/objects/{key}', struct.pack('<II', 0, 0) + body, headers)
    response = connection.getresponse()
    response.read()
    connection.close()
    assert response.status == 200, (key, response.status)


def post_score(port, name):
    for attempt in range(50):  # a server may print its port a moment before it listens
        try:
            connection = http.client.HTTPConnection('127.0.0.1', port, timeout=30)
            connection.request('POST', '/inputs/score?points=1', name.encode(), {'Content-Length': str(len(name))})
            break
        except ConnectionRefusedError:
            time.sleep(0.05)
    response = connection.getresponse()
    response.read()
    connection.close()
    assert response.status == 200, (name, response.status)


def probe_ms(directory, count=16):
    """Median time of an in-place 4 KiB write + fdatasync on the store device."""
    path = directory / 'device-probe'
    fd = os.open(path, os.O_RDWR | os.O_CREAT | os.O_TRUNC, 0o600)
    try:
        os.pwrite(fd, bytes(count * 4096), 0)
        os.fsync(fd)
        times = []
        for i in range(count):
            start = time.perf_counter()
            os.pwrite(fd, b'p' * 4096, i * 4096)
            os.fdatasync(fd)
            times.append((time.perf_counter() - start) * 1e3)
    finally:
        os.close(fd)
        path.unlink()
    return round(statistics.median(times), 2)


def load_command(port, connections, seconds, method, keys, mode, body=16):
    return ['taskset', '-c', '3', str(HERE / 'load'), str(port), str(connections), str(seconds), method, str(keys),
            mode, str(body)]


def load(port, connections, seconds, method, keys, mode, body=16):
    output = subprocess.run(load_command(port, connections, seconds, method, keys, mode, body), capture_output=True,
                            text=True, check=True)
    return json.loads(output.stdout)


def mixed(port, seconds, writers, readers):
    """Durable posts and reads of the total at once, from two load processes:
    whether reads wait while writes sync."""
    writing = subprocess.Popen(load_command(port, writers, seconds, 'score', writers, 'keep-alive'),
                               stdout=subprocess.PIPE, text=True)
    reads = load(port, readers, seconds, 'total', readers, 'keep-alive')
    writes = json.loads(writing.communicate()[0])
    assert writing.returncode == 0
    return writes, reads


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--module', type=Path, required=True, help='editor.pmod built by aup, or scores.pmod for --suite app')
    parser.add_argument('--suite', choices=['documents', 'app'], default='documents')
    parser.add_argument('--directory', type=Path, default=Path('/var/tmp'))
    parser.add_argument('--seconds', type=float, default=5)
    parser.add_argument('--only', default='')
    parser.add_argument('--workloads', default='', help='run only workloads whose label matches this regular expression')
    parser.add_argument('--rounds', type=int, default=1, help='repeat every server, rotating their order')
    parser.add_argument('--pause', type=float, default=0, help='seconds to rest the device between runs')
    parser.add_argument('--budget', type=int, default=64 << 20, help="Pure's payload budget in bytes")
    args = parser.parse_args()
    axum = HERE / 'axum-baseline/target/release/axum-baseline'
    drogon = HERE / 'drogon-baseline/drogon-baseline'
    base = Path(tempfile.mkdtemp(dir=args.directory, prefix='pure-http-bench-'))
    servers = {
        'pure-program': lambda d: [PURE / 'build/tree/pure-connections', '--program', PURE / 'examples/editor.pure',
                                   '--store', d, '--port', '0', '--budget', str(args.budget)],
        'pure-serve': lambda d: [PURE / 'build/pure-serve', '--module', args.module, '--store', d, '--port', '0',
                                 '--budget', str(args.budget)],
        'pure-serve-3': lambda d: [PURE / 'build/pure-serve', '--module', args.module, '--store', d, '--port', '0',
                                   '--budget', str(args.budget), '--threads', '3'],
        'pure-serve-2': lambda d: [PURE / 'build/pure-serve', '--module', args.module, '--store', d, '--port', '0',
                                   '--budget', str(args.budget), '--threads', '2'],
        'axum-memory': lambda d: [axum, '--port', '0'],
        'axum-durable': lambda d: [axum, '--port', '0', '--durable', d],
        'axum-atomic': lambda d: [axum, '--port', '0', '--atomic', d],
        'drogon-memory': lambda d: [drogon, '--port', '0'],
        'drogon-durable': lambda d: [drogon, '--port', '0', '--durable', d],
        'drogon-atomic': lambda d: [drogon, '--port', '0', '--atomic', d],
    }
    workloads = [
        ('get 64B, 64 conn, close', dict(connections=64, method='get', keys=64, mode='close')),
        ('get 64B, 64 conn, keep-alive', dict(connections=64, method='get', keys=64, mode='keep-alive')),
        ('get 1MiB, 8 conn, keep-alive', dict(connections=8, method='get', keys=1, mode='keep-alive', big=True)),
        ('put 16B, 64 conn, keep-alive', dict(connections=64, method='put', keys=64, mode='keep-alive')),
        ('put 16B, 8 conn, keep-alive', dict(connections=8, method='put', keys=8, mode='keep-alive')),
    ]
    app = args.suite == 'app'
    if app:
        # Durable posts write (the method is put for the device probe); reads are a sum and a page.
        workloads = [
            ('score, 64 conn, keep-alive', dict(connections=64, method='score', keys=64, mode='keep-alive', writes=True)),
            ('score, 8 conn, keep-alive', dict(connections=8, method='score', keys=8, mode='keep-alive', writes=True)),
            ('total, 64 conn, keep-alive', dict(connections=64, method='total', keys=64, mode='keep-alive')),
            ('page 20 rows, 64 conn, keep-alive', dict(connections=64, method='page', keys=64, mode='keep-alive')),
            # Reported as two rows: the posts, and the reads made meanwhile.
            ('mixed, 8 score + 56 total conn', dict(connections=64, method='mixed', keys=64, mode='keep-alive',
                                                    writes=True)),
        ]
        for name in ('pure-program', 'axum-atomic', 'drogon-atomic'):
            del servers[name]  # atomic is durable for a log already
    if args.workloads:
        workloads = [w for w in workloads if re.search(args.workloads, w[0])]
    chosen = [name for name in servers if not args.only or name in args.only.split(',')]
    results = []
    try:
        for round_ in range(args.rounds):
            turn = round_ % len(chosen)
            for name in chosen[turn:] + chosen[:turn]:
                if args.pause:
                    time.sleep(args.pause)
                directory = base / f'{name}-{round_}'
                process, port = start(servers[name](directory))
                idle = rss_kib(process.pid, 'VmRSS:')
                for key in range(64):
                    if app: post_score(port, f'k{key}')
                    else: put(port, f'k{key}', b'x' * 56, 0)
                for label, workload in workloads:
                    if workload.get('big'):
                        # the 1 MiB document is k0 for this workload
                        put(port, 'k0', b'B' * (1 << 20), 1)
                    probe = probe_ms(base) if workload['method'] == 'put' or workload.get('writes') else None
                    used = cpu_seconds(process.pid)
                    if workload['method'] == 'mixed':
                        writes, reads = mixed(port, args.seconds, 8, 56)
                        parts = [(f'{label}: posts', writes), (f'{label}: reads', reads)]
                    else:
                        parts = [(label, load(port, workload['connections'], args.seconds, workload['method'],
                                              workload['keys'], workload['mode']))]
                    used = cpu_seconds(process.pid) - used
                    answered = sum(measured['2xx'] for _, measured in parts)  # per CPU second: all parts together
                    for part, measured in parts:
                        measured.update(server=name, workload=part, round=round_, probe_ms=probe,
                                        rss_kib=rss_kib(process.pid, 'VmRSS:'), cpu_seconds=round(used, 2),
                                        ok_per_cpu_second=round(answered / used) if used else 0,
                                        ok_per_second=round(measured['2xx'] / args.seconds), idle_rss_kib=idle)
                        results.append(measured)
                        print(json.dumps(measured), flush=True)
                peak = rss_kib(process.pid, 'VmHWM:')
                process.send_signal(signal.SIGINT if name.startswith('axum') else signal.SIGTERM)
                process.communicate(timeout=30)
                shutil.rmtree(directory, ignore_errors=True)
                for r in results:
                    if r['server'] == name and r['round'] == round_:
                        r['peak_rss_kib'] = peak
    finally:
        shutil.rmtree(base, ignore_errors=True)
    median = lambda rows, field: statistics.median(r[field] for r in rows)
    print()
    print(f"medians over {args.rounds} round(s)")
    print(f"{'workload':32s} {'server':14s} {'2xx/s':>9s} {'2xx/cpu-s':>10s} {'p50 us':>8s} {'p99 us':>8s} "
          f"{'errors':>7s} {'non-2xx':>8s} {'probe ms':>9s}")
    for label in dict.fromkeys(r['workload'] for r in results):
        for name in chosen:
            rows = [r for r in results if r['server'] == name and r['workload'] == label]
            if not rows:
                continue
            probes = [r['probe_ms'] for r in rows if r['probe_ms'] is not None]
            probe = f"{min(probes):.1f}-{max(probes):.1f}" if probes else '-'
            print(f"{label:32s} {name:14s} {median(rows, 'ok_per_second'):9.0f} "
                  f"{median(rows, 'ok_per_cpu_second'):10.0f} {median(rows, 'p50_us'):8.0f} "
                  f"{median(rows, 'p99_us'):8.0f} {sum(r['errors'] for r in rows):7d} "
                  f"{sum(r['4xx'] + r['5xx'] for r in rows):8d} {probe:>9s}")
    print()
    for name in chosen:
        rows = [r for r in results if r['server'] == name]
        if rows:
            print(f"{name:14s} idle RSS {median(rows, 'idle_rss_kib'):.0f} KiB, "
                  f"peak RSS {max(r['peak_rss_kib'] for r in rows)} KiB")


if __name__ == '__main__':
    sys.exit(main())
