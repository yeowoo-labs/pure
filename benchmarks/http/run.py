#!/usr/bin/env python3
"""HTTP benchmarks: Pure's document servers against an axum service with the
same contract, on the same machine, with the same load generator.

Servers are pinned to CPUs 0-2 and the load to CPU 3; stores live under
--directory (use the device you care about: fsync costs are the device's).
Prints one JSON object per (server, workload, round) and a summary table of
medians over rounds.

Durable writes are only comparable on a device in the same state. An SD card
slows down under sustained syncs, so the order of runs alone can change a
result several times over. Each round therefore runs the servers in a
rotated order, each on a fresh store, and every write workload first times
the device (an in-place 4 KiB write + fdatasync); the table shows that probe
next to each result.
"""
import argparse
import json
import os
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


def load(port, connections, seconds, method, keys, mode, body=16):
    output = subprocess.run(['taskset', '-c', '3', str(HERE / 'load'), str(port), str(connections), str(seconds),
                             method, str(keys), mode, str(body)], capture_output=True, text=True, check=True)
    return json.loads(output.stdout)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--module', type=Path, required=True, help='editor.pmod built by aup')
    parser.add_argument('--directory', type=Path, default=Path('/var/tmp'))
    parser.add_argument('--seconds', type=float, default=5)
    parser.add_argument('--only', default='')
    parser.add_argument('--workloads', default='', help='run only workloads whose label starts with one of these')
    parser.add_argument('--rounds', type=int, default=1, help='repeat every server, rotating their order')
    parser.add_argument('--pause', type=float, default=0, help='seconds to rest the device between runs')
    parser.add_argument('--budget', type=int, default=64 << 20, help="Pure's payload budget in bytes")
    args = parser.parse_args()
    axum = HERE / 'axum-baseline/target/release/axum-baseline'
    base = Path(tempfile.mkdtemp(dir=args.directory, prefix='pure-http-bench-'))
    servers = {
        'pure-program': lambda d: [PURE / 'build/tree/pure-connections', '--program', PURE / 'examples/editor.pure',
                                   '--store', d, '--port', '0', '--budget', str(args.budget)],
        'pure-serve': lambda d: [PURE / 'build/pure-serve', '--module', args.module, '--store', d, '--port', '0',
                                 '--budget', str(args.budget)],
        'pure-serve-3': lambda d: [PURE / 'build/pure-serve', '--module', args.module, '--store', d, '--port', '0',
                                   '--budget', str(args.budget), '--threads', '3'],
        'axum-memory': lambda d: [axum, '--port', '0'],
        'axum-durable': lambda d: [axum, '--port', '0', '--durable', d],
        'axum-atomic': lambda d: [axum, '--port', '0', '--atomic', d],
    }
    workloads = [
        ('get 64B, 64 conn, close', dict(connections=64, method='get', keys=64, mode='close')),
        ('get 64B, 64 conn, keep-alive', dict(connections=64, method='get', keys=64, mode='keep-alive')),
        ('get 1MiB, 8 conn, keep-alive', dict(connections=8, method='get', keys=1, mode='keep-alive', big=True)),
        ('put 16B, 64 conn, keep-alive', dict(connections=64, method='put', keys=64, mode='keep-alive')),
        ('put 16B, 8 conn, keep-alive', dict(connections=8, method='put', keys=8, mode='keep-alive')),
    ]
    if args.workloads:
        workloads = [w for w in workloads if w[0].startswith(tuple(args.workloads.split(',')))]
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
                    put(port, f'k{key}', b'x' * 56, 0)
                for label, workload in workloads:
                    if workload.get('big'):
                        # the 1 MiB document is k0 for this workload
                        put(port, 'k0', b'B' * (1 << 20), 1)
                    probe = probe_ms(base) if workload['method'] == 'put' else None
                    measured = load(port, workload['connections'], args.seconds, workload['method'],
                                    workload['keys'], workload['mode'])
                    measured.update(server=name, workload=label, round=round_, probe_ms=probe,
                                    rss_kib=rss_kib(process.pid, 'VmRSS:'),
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
    print(f"{'workload':32s} {'server':14s} {'2xx/s':>9s} {'p50 us':>8s} {'p99 us':>8s} {'errors':>7s} "
          f"{'non-2xx':>8s} {'probe ms':>9s}")
    for label, _ in workloads:
        for name in chosen:
            rows = [r for r in results if r['server'] == name and r['workload'] == label]
            if not rows:
                continue
            probes = [r['probe_ms'] for r in rows if r['probe_ms'] is not None]
            probe = f"{min(probes):.1f}-{max(probes):.1f}" if probes else '-'
            print(f"{label:32s} {name:14s} {median(rows, 'ok_per_second'):9.0f} {median(rows, 'p50_us'):8.0f} "
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
