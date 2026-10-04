#!/usr/bin/env python3
"""How long an app cannot answer while it changes module on a running host
(docs/design/017's stage 3), against restarting the host (research 115).

An app of items.aup holds ITEMS items (a list grown by doubling) in a host
of apps, its store in RAM, the host on cores 0-2. A client on core 3 posts
writes to it one after another on one connection; the time an app cannot
answer is the longest gap between two answered writes (200) around the
change. Changed: SIGHUP after its line names the other of two module files
(items.pmod and NEXT, items-next.pmod unless named: the same states, a
view more; items-wide.pmod: its records a field more, so the list is laid
out anew), so each reading changes it to another module. Restarted: the
same change made the way there was before, the line naming the other
module, then SIGTERM and the host started again (the client reconnecting
at once).

window.py PURE_SERVE ITEMS CHANGES RESTARTS [NEXT]   a JSON line per change
                                              and per restart, then one with
                                              medians
"""
import http.client
import json
import os
import select
import shutil
import signal
import socket
import statistics
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
AUP = HERE.parents[3] / 'aup' / 'build' / 'aup'


class Host:
    def __init__(self, serve, conf, port):
        self.process = subprocess.Popen(['taskset', '-c', '0-2', str(serve), '--apps', str(conf), '--port', str(port)],
                                        stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        ready, _, _ = select.select([self.process.stdout], [], [], 600)
        assert ready, self.process.stderr.read()
        self.info = json.loads(self.process.stdout.readline())

    def reading(self):
        self.process.send_signal(signal.SIGHUP)
        ready, _, _ = select.select([self.process.stdout], [], [], 600)
        assert ready
        return json.loads(self.process.stdout.readline())

    def stop(self):
        self.process.terminate()
        _, error = self.process.communicate(timeout=600)
        assert self.process.returncode == 0, error[-500:]


class Writer(threading.Thread):
    """Writes one after another; the times of those answered 200."""

    def __init__(self, port):
        super().__init__(daemon=True)
        self.port, self.answered, self.refused, self.stopping = port, [], 0, False

    def run(self):
        os.sched_setaffinity(0, {3})
        connection = None
        while not self.stopping:
            try:
                if connection is None:
                    connection = http.client.HTTPConnection('127.0.0.1', self.port, timeout=600)
                connection.request('POST', '/x/inputs/add?step=1', b'', {'Content-Length': '0'})
                answer = connection.getresponse()
                answer.read()
                if answer.status == 200:
                    self.answered.append(time.monotonic())
                else:
                    self.refused += 1
            except (OSError, http.client.HTTPException):  # the host is down: again at once
                connection = None
                time.sleep(0.0002)

    def gap(self, since):
        """The longest gap between answered writes from since on (and the one before it)."""
        times = [t for t in self.answered if t >= since]
        before = [t for t in self.answered if t < since]
        if before:
            times = before[-1:] + times
        return max(b - a for a, b in zip(times, times[1:]))


def free_port():
    with socket.socket() as probe:
        probe.bind(('127.0.0.1', 0))
        return probe.getsockname()[1]


def grow(port, items, app='x'):
    connection = http.client.HTTPConnection('127.0.0.1', port, timeout=600)

    def post(path):
        connection.request('POST', path, b'', {'Content-Length': '0'})
        answer = connection.getresponse()
        answer.read()
        assert answer.status == 200, (path, answer.status)

    post(f'/{app}/inputs/seed?n=1')
    count = 1
    while count < items:
        post(f'/{app}/inputs/grow?step=1')
        count *= 2
    connection.close()
    return count


def main():
    serve, items, changes, restarts = Path(sys.argv[1]).resolve(), int(sys.argv[2]), int(sys.argv[3]), int(sys.argv[4])
    after = sys.argv[5] if len(sys.argv) > 5 else 'items-next'
    work = Path(tempfile.mkdtemp(prefix='pure-030-', dir='/dev/shm'))
    try:
        modules = {}
        for name in ('items', after):
            modules[name] = work / f'{name}.pmod'
            subprocess.run([str(AUP), 'build', str(HERE / f'{name}.aup'), '-o', str(modules[name])], check=True)
        conf, store, port = work / 'apps.conf', work / 'store', free_port()

        def write(module):
            conf.write_text(f"x {modules[module]} {store}\n")

        write('items')
        host = Host(serve, conf, port)
        count = grow(port, items)
        writer = Writer(port)
        writer.start()
        time.sleep(0.5)
        gaps = {'changed': [], 'restarted': []}
        for n in range(changes):
            write(after if n % 2 == 0 else 'items')
            since = time.monotonic()
            began = time.perf_counter()
            said = host.reading()
            took = time.perf_counter() - began
            assert said['apps'][0]['did'] in ('restarted', 'changed') and said['apps'][0]['running'], said
            time.sleep(0.3)
            gaps['changed'].append(writer.gap(since))
            print(json.dumps({'items': count, 'way': 'changed', 'to': after if n % 2 == 0 else 'items',
                              'did': said['apps'][0]['did'], 'gap_ms': round(gaps['changed'][-1] * 1000, 2),
                              'reading_ms': round(took * 1000, 2)}), flush=True)
        for n in range(restarts):
            write(after if (changes + n) % 2 == 0 else 'items')
            since = time.monotonic()
            host.stop()
            host = Host(serve, conf, port)
            time.sleep(0.3)
            gaps['restarted'].append(writer.gap(since))
            print(json.dumps({'items': count, 'way': 'restarted', 'gap_ms': round(gaps['restarted'][-1] * 1000, 2)}),
                  flush=True)
        writer.stopping = True
        writer.join()
        host.stop()
        print(json.dumps({'items': count, 'changed_median_ms': round(statistics.median(gaps['changed']) * 1000, 2),
                          'restarted_median_ms': round(statistics.median(gaps['restarted']) * 1000, 2),
                          'refused': writer.refused, 'answered': len(writer.answered)}), flush=True)
    finally:
        shutil.rmtree(work)


if __name__ == '__main__':
    main()
