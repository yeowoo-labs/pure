#!/usr/bin/env python3
"""Pushing a view against polling it (docs/design/019, research 120).
pure-serve (connection threads THREADS, store in RAM, cores 0-2) holds
counter.aup; benchmarks/http/watch (core 3) has CLIENTS clients follow its
total view, by push (SSE) or by polling every MS, while one writer moves it
RATE times a second for SECONDS. Each run says the freshness (from the
writer's answer to a client seeing that value or a newer one), what the
host's process used of the CPU meanwhile (/proc/PID/stat), and its RSS
with the clients on less without them.

push.py PURE_SERVE AUP THREADS SECONDS RUNS...   RUNS: CLIENTS:RATE:push or CLIENTS:RATE:poll:MS, in turn ROUNDS times
"""
import json
import os
import shutil
import statistics
import subprocess
import sys
import tempfile
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
WATCH = HERE.parents[2] / 'benchmarks' / 'http' / 'watch'
TICK = os.sysconf('SC_CLK_TCK')


def cpu(pid):
    fields = Path(f'/proc/{pid}/stat').read_text().rsplit(')', 1)[1].split()
    return (int(fields[11]) + int(fields[12])) / TICK  # utime, stime


def rss(pid):
    return next(int(line.split()[1]) for line in Path(f'/proc/{pid}/status').read_text().splitlines()
                if line.startswith('VmRSS'))


def main():
    serve, aup, threads, seconds = Path(sys.argv[1]).resolve(), Path(sys.argv[2]).resolve(), sys.argv[3], sys.argv[4]
    runs = sys.argv[5:]
    work = Path(tempfile.mkdtemp(prefix='pure-035-', dir='/dev/shm'))
    try:
        subprocess.run(['cc', '-O2', '-Wall', '-Wextra', '-Werror', '-o', str(WATCH), str(WATCH) + '.c'], check=True)
        subprocess.run([str(aup), 'build', str(HERE / 'counter.aup'), '-o', str(work / 'counter.pmod')], check=True)
        for run in runs:
            clients, rate, way = run.split(':', 2)
            host = subprocess.Popen(['taskset', '-c', '0-2', str(serve), '--module', str(work / 'counter.pmod'), '--store',
                                     str(work / f'store-{run.replace(":", "-")}'), '--port', '0', '--threads', threads,
                                     '--connections', '32768', '--subscribers', '65536'],
                                    stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            port = json.loads(host.stdout.readline())['port']
            time.sleep(0.3)
            idle = rss(host.pid)
            watcher = subprocess.Popen(['taskset', '-c', '3', str(WATCH), str(port), clients, seconds, rate, way,
                                        '/views/total', '/inputs/tap'], stdout=subprocess.PIPE, text=True)
            time.sleep(2.0 + int(clients) / 5000)  # every client on, its first value had
            followed_rss, began = rss(host.pid), cpu(host.pid)
            started = time.monotonic()
            out, _ = watcher.communicate(timeout=600)
            used = cpu(host.pid) - began
            took = time.monotonic() - started
            host.terminate()
            _, err = host.communicate(timeout=60)
            stats = json.loads(err.decode().strip().splitlines()[-1])
            result = json.loads(out)
            result.update({'host_cpu_seconds': round(used, 2), 'host_cpu_share': round(used / took, 3),
                           'rss_kib_per_client': round((followed_rss - idle) / int(clients), 2),
                           'pushes': stats['pushes'], 'pushed': stats['pushed'], 'replaced': stats['push_replaced'],
                           'requests': stats['requests']})
            print(json.dumps(result), flush=True)
    finally:
        shutil.rmtree(work)


if __name__ == '__main__':
    main()
