#!/usr/bin/env python3
"""Modules replaced and freed while they are read (research 115's next step: the modules an app ran are let go). A
host of apps (--threads 3, --view-workers 1) holds an app of items-body.aup; its line names in turn items-body.pmod,
items-third.pmod (the same app) and items-same.pmod (a view renamed, total and tally; count's fold the same instructions at the same place, counting
n > 1 and adding 2000), so that a module freed and the next loaded take the same address, and a fold remembered for
one (a reader's pass, kept by its module and place) gives the other a value neither has; and picked, a view with a body
whose list is of a record of one field in one module and of two in the other at the same place (a reader laid out for
one and reading the other gives neither's value: 11 and 100,222); total and tally at the same place, tally 500,000 more
(a request resolved with one module and shown with the other at the same address gives the other's). Meanwhile, until CHANGES changes are done:
- readers on connections of their own GET /x/views/count (in both), /x/views/summed and picked (views with a body: the
  view workers), and total and tally (one in each module): each answer 200 with the value the items give, 404 for a view
  the module lacks, or 503 while it changes;
- subscribers stream count and total (Accept: text/event-stream), again each time a stream ends: every count pushed
  one of the two, every total below 500,000 (a push group asked whether its app runs the module it was made in by
  address would take items-same, come where its module was freed, for that module and push tally);
- a writer sends POST /x/inputs/add with its body in two parts, a change between them: 200, or 503 refused as stopped.
Then the host is stopped, and its stats say how many modules it freed and how many it loaded at an address one freed
had (the case comparing by address would get wrong). Run with an ASan build: a module read after it
was freed stops the host there.

stress.py PURE_SERVE CHANGES   one JSON line"""
import json
import select
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path

from window import AUP, HERE, grow


def answer(sock):
    head = b''
    while b'\r\n\r\n' not in head:
        part = sock.recv(4096)
        if not part:
            raise ConnectionError('closed')
        head += part
    top, rest = head.split(b'\r\n\r\n', 1)
    status = int(top.split()[1])
    length = next(int(line.split(b':')[1]) for line in top.split(b'\r\n') if line.lower().startswith(b'content-length'))
    while len(rest) < length:
        rest += sock.recv(4096)
    return status, rest[:length]


def main():
    serve, changes = Path(sys.argv[1]).resolve(), int(sys.argv[2])
    work = Path(tempfile.mkdtemp(prefix='pure-030-', dir='/dev/shm'))
    try:
        modules = {}
        for name in ('items-body', 'items-third', 'items-same'):
            modules[name] = work / f'{name}.pmod'
            subprocess.run([str(AUP), 'build', str(HERE / f'{name}.aup'), '-o', str(modules[name])], check=True)
        conf = work / 'apps.conf'
        cycle = ('items-body', 'items-third', 'items-same')  # items-same can come where items-body was freed
        write = lambda n: conf.write_text(f"x {modules[cycle[n % 3]]} {work / 'store'}\n")
        write(0)
        host = subprocess.Popen([str(serve), '--apps', str(conf), '--port', '0', '--threads', '3', '--view-workers', '1'],
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        port = json.loads(host.stdout.readline())['port']
        items = grow(port, 1000)
        counts = (items, items - 1 + 2000)  # count in items-body (every n is 1 or more), in items-same (n > 1, + 2000)
        done = threading.Event()
        seen = {'answers': 0, 'not_found': 0, 'changing': 0, 'pushed': 0, 'writes': 0, 'writes_refused': 0, 'wrong': []}
        lock = threading.Lock()

        def tally(key, wrong=None):
            with lock:
                seen[key] += 1
                if wrong is not None and len(seen['wrong']) < 10:
                    seen['wrong'].append(wrong)

        def reader(view):
            sock = socket.create_connection(('127.0.0.1', port))
            while not done.is_set():
                sock.sendall(f'GET /x/views/{view} HTTP/1.1\r\nHost: x\r\n\r\n'.encode())
                status, body = answer(sock)
                if status == 503:
                    tally('changing')
                elif status == 404:
                    tally('not_found', None if view in ('total', 'tally') else (view, status, body[:80]))
                elif status == 200:
                    value = json.loads(body)
                    good = (value in counts if view == 'count' else value >= items if view == 'summed' else
                            value in (11, 100222) if view == 'picked' else value < 500000 if view == 'total' else
                            value >= 500000)
                    tally('answers', None if good else (view, value))
                else:
                    tally('answers', (view, status, body[:80]))

        def subscriber(view, good):  # streams again when its stream ends (its view gone from the module that runs)
            while not done.is_set():
                sock = socket.create_connection(('127.0.0.1', port))
                sock.sendall(f'GET /x/views/{view} HTTP/1.1\r\nHost: x\r\nAccept: text/event-stream\r\n\r\n'.encode())
                sock.settimeout(0.5)
                buffer = b''
                while not done.is_set():
                    try:
                        part = sock.recv(4096)
                    except socket.timeout:
                        continue
                    if not part:
                        break
                    buffer += part
                    while b'\n\n' in buffer:
                        event, buffer = buffer.split(b'\n\n', 1)
                        for line in event.split(b'\n'):
                            if line.startswith(b'data:'):
                                value = json.loads(line[5:])
                                tally('pushed', None if good(value) else ('pushed', view, value))
                sock.close()

        def writer():
            while not done.is_set():
                sock = socket.create_connection(('127.0.0.1', port))
                body = b'{"step": 1}'
                sock.sendall(b'POST /x/inputs/add HTTP/1.1\r\nHost: x\r\nContent-Type: application/json\r\n'
                             b'Content-Length: %d\r\n\r\n' % len(body) + body[:4])
                time.sleep(0.003)  # a change may come between the parts
                sock.sendall(body[4:])
                status, said = answer(sock)
                if status == 200:
                    tally('writes')
                elif status == 503:
                    tally('writes_refused')
                else:
                    tally('writes', ('write', status, said[:80]))
                sock.close()

        threads = [threading.Thread(target=reader, args=(v,)) for v in ('count', 'picked', 'summed', 'total', 'tally', 'picked')]
        threads += [threading.Thread(target=subscriber, args=('count', lambda v: v in counts)),
                    threading.Thread(target=subscriber, args=('total', lambda v: isinstance(v, (int, float)) and v < 500000)),
                    threading.Thread(target=writer)]
        for t in threads:
            t.start()
        for n in range(1, changes + 1):
            write(n)
            host.send_signal(signal.SIGHUP)
            ready, _, _ = select.select([host.stdout], [], [], 60)
            line = host.stdout.readline() if ready else b''
            if not line:
                done.set()
                raise SystemExit(f'the host stopped at change {n}: {host.stderr.read().decode()[-3000:]}')
            said = json.loads(line)
            assert said['apps'][0]['did'] == 'changed' and said['apps'][0]['running'], said
            time.sleep(0.002)
        time.sleep(1)
        done.set()
        for t in threads:
            t.join(timeout=10)
        host.terminate()
        _, errors = host.communicate(timeout=60)
        stats = next((json.loads(l) for l in errors.decode().splitlines() if '"modules_freed"' in l), {})
        print(json.dumps({'changes': changes, **{k: v for k, v in seen.items()}, 'host_exit': host.returncode,
                          'modules_freed': stats.get('modules_freed'), 'modules_retired': stats.get('modules_retired'),
                          'modules_at_freed': stats.get('modules_at_freed'),
                          'asan': [l for l in errors.decode().splitlines() if 'AddressSanitizer' in l][:3]}))
        sys.exit(0 if not seen['wrong'] and host.returncode in (0, -15) else 1)
    finally:
        shutil.rmtree(work)


if __name__ == '__main__':
    main()
