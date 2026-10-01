#!/usr/bin/env python3
"""The threads of a host whose app sends to ten links and ten wires,
against the same host with no output wired (docs/design/017, 2b): many
sends one message to each of its twenty outputs; the links go to the
host's own --link-port, the wires to its own HTTP port, each to an app of
its own (d1..d20), all sealed (a host that names its senders takes
nothing unsealed). The threads are counted once every receiver has its
message. One connection thread, two workers.

threads.py NAME=PURE_SERVE [NAME=PURE_SERVE ...]   a JSON line per host
"""
import http.client
import json
import shutil
import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path

from sends import AUP, HERE, free_port, started, stopped, threads


def host(serve, work, modules, key, public, wired):
    link_port, http_port = free_port(socket.SOCK_DGRAM), free_port(socket.SOCK_STREAM)
    sends = [f'send=l{n}=pure://127.0.0.1:{link_port}/d{n}/take seal=l{n}={public}' for n in range(1, 11)] + \
        [f'send=w{n}=http://127.0.0.1:{http_port}/d{n + 10}/inputs/take seal=w{n}={public}' for n in range(1, 11)] \
        if wired else []
    (work / 'apps.conf').write_text(f"many {modules['many']} {work / 'many'} {' '.join(sends)}\n" +
                                    ''.join(f"d{n} {modules['dst']} {work / f'd{n}'}\n" for n in range(1, 21)))
    args = [serve, '--apps', work / 'apps.conf', '--threads', 1, '--workers', 2, '--port', http_port, '--key', key]
    if wired:
        args += ['--link-port', link_port, *sum((['--sender', f'many.{o}{n}={public}'] for o in 'lw' for n in range(1, 11)), [])]
    process, info = started(args)
    client = http.client.HTTPConnection('127.0.0.1', info['port'], timeout=60)

    def ask(method, path):
        client.request(method, path, b'' if method == 'POST' else None)
        answer = client.getresponse()
        body = answer.read()
        assert answer.status == 200, (path, answer.status, answer.getheader('X-Pure-Error'))
        return body

    try:
        ask('POST', '/many/inputs/ping?k=7')
        deadline = time.time() + 20
        while wired and any(int(ask('GET', f'/d{n}/views/count')) < 1 for n in range(1, 21)):
            assert time.time() < deadline, 'not every receiver has its message'
            time.sleep(0.01)
        return threads(process.pid)
    finally:
        client.close()
        stopped(process)


def main():
    serves = {name: Path(path).resolve() for name, path in (a.split('=', 1) for a in sys.argv[1:])}
    work = Path(tempfile.mkdtemp(prefix='pure-threads-', dir='/dev/shm'))
    try:
        modules = {}
        for name in ('many', 'dst'):
            modules[name] = work / f'{name}.pmod'
            subprocess.run([str(AUP), 'build', str(HERE / f'{name}.aup'), '-o', str(modules[name])], check=True)
        key = work / 'host.key'
        (work / 'one.conf').write_text(f"x {modules['dst']} {work / 'keys'}\n")
        process, info = started([list(serves.values())[-1], '--apps', work / 'one.conf', '--key', key, '--port', 0])
        public = info['public_key']
        stopped(process)
        for name, serve in serves.items():
            for wired in (False, True):
                runs = work / f'{name}-{wired}'
                runs.mkdir()
                print(json.dumps({'version': name, 'outputs': 'ten links, ten wires' if wired else 'none',
                                  'threads': host(serve, runs, modules, key, public, wired)}), flush=True)
                shutil.rmtree(runs)
    finally:
        shutil.rmtree(work)


if __name__ == '__main__':
    main()
