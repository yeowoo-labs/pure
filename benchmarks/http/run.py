#!/usr/bin/env python3
"""HTTP benchmarks: Pure's document servers against an axum service with the
same contract, on the same machine, with the same load generator.

Servers are pinned to CPUs 0-2 and the load to CPU 3; stores live under
--directory (use the device you care about: fsync costs are the device's).
Prints one JSON object per (server, workload) and a summary table.
"""
import argparse
import json
import os
import select
import shutil
import signal
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
    results = []
    try:
        for name, command in servers.items():
            if args.only and name not in args.only.split(','):
                continue
            directory = base / name
            process, port = start(command(directory))
            idle = rss_kib(process.pid, 'VmRSS:')
            for key in range(64):
                put(port, f'k{key}', b'x' * 56, 0)
            big = b'B' * (1 << 20)
            put(port, 'big', big, 0)
            for label, workload in workloads:
                keys = workload['keys']
                if workload.get('big'):
                    # the 1 MiB document is k0 for this workload
                    put(port, 'k0', big, 1)
                if workload['method'] == 'put':
                    # fresh keys per run so every connection starts at If-None-Match: *
                    pass
                measured = load(port, workload['connections'], args.seconds, workload['method'], keys,
                                workload['mode'])
                measured.update(server=name, workload=label, rss_kib=rss_kib(process.pid, 'VmRSS:'),
                                ok_per_second=round(measured['2xx'] / args.seconds))
                results.append(measured)
                print(json.dumps(measured), flush=True)
            peak = rss_kib(process.pid, 'VmHWM:')
            process.send_signal(signal.SIGINT if name.startswith('axum') else signal.SIGTERM)
            process.communicate(timeout=30)
            for r in results:
                if r['server'] == name:
                    r['idle_rss_kib'], r['peak_rss_kib'] = idle, peak
    finally:
        shutil.rmtree(base, ignore_errors=True)
    print()
    print(f"{'workload':32s} {'server':14s} {'2xx/s':>9s} {'p50 us':>8s} {'p99 us':>8s} {'errors':>7s} {'non-2xx':>8s}")
    for r in results:
        print(f"{r['workload']:32s} {r['server']:14s} {r['ok_per_second']:9d} {r['p50_us']:8d} {r['p99_us']:8d} "
              f"{r['errors']:7d} {r['4xx'] + r['5xx']:8d}")
    print()
    for name in servers:
        rows = [r for r in results if r['server'] == name]
        if rows:
            print(f"{name:14s} idle RSS {rows[0]['idle_rss_kib']} KiB, peak RSS {rows[0]['peak_rss_kib']} KiB")


if __name__ == '__main__':
    sys.exit(main())
