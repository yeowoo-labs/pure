#!/usr/bin/env python3
"""What an app costs a host (docs/design/017): pure-serve with N apps of the
scoreboard (benchmarks/http/scores.aup), each written once and read once
(its store opened, its snapshot published, a reader of it used), in RAM;
then the host's own memory (smaps_rollup's Private_Dirty, RSS, PSS) and
threads. Beside it, one pure-serve of one app (--module) the same way: the
process an app costs when each app is a process.

memory.py SERVE [N ...]
"""
import http.client
import json
import select
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
AUP = ROOT.parent / 'aup' / 'build' / 'aup'


def used(pid):
    fields = {}
    for line in Path(f'/proc/{pid}/smaps_rollup').read_text().splitlines()[1:]:
        name, value = line.split(':')
        fields[name] = int(value.split()[0])
    threads = int(Path(f'/proc/{pid}/status').read_text().split('Threads:')[1].split()[0])
    return {'private_dirty_kib': fields['Private_Dirty'], 'rss_kib': fields['Rss'], 'pss_kib': fields['Pss'],
            'threads': threads}


def touched(port, prefixes):
    connection = http.client.HTTPConnection('127.0.0.1', port, timeout=30)
    for prefix in prefixes:
        for method, path, body in (('POST', f'{prefix}/inputs/score?points=1', b'k'), ('GET', f'{prefix}/views/total', None)):
            connection.request(method, path, body, {'Content-Length': str(len(body))} if body else {})
            answer = connection.getresponse()
            answer.read()
            assert answer.status == 200, (path, answer.status)
    connection.close()


def measured(args, prefixes):
    process = subprocess.Popen([*map(str, args), '--port', '0'], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    select.select([process.stdout], [], [], 30)
    info = json.loads(process.stdout.readline())
    touched(info['port'], prefixes)
    result = used(process.pid)
    process.terminate()
    process.communicate(timeout=60)
    return result


def main():
    serve = Path(sys.argv[1]).resolve()
    counts = [int(n) for n in sys.argv[2:]] or [1, 10, 100]
    work = Path(tempfile.mkdtemp(prefix='pure-apps-memory-', dir='/dev/shm'))
    try:
        module = work / 'scores.pmod'
        subprocess.run([str(AUP), 'build', str(ROOT / 'benchmarks' / 'http' / 'scores.aup'), '-o', str(module)], check=True)
        one = measured([serve, '--module', module, '--store', work / 'one', '--threads', 1], [''])
        print(json.dumps({'way': 'process', 'apps': 1, **one}), flush=True)
        for n in counts:
            manifest = work / f'apps-{n}.conf'
            manifest.write_text(''.join(f'app{i} {module} {work / f"store-{n}-{i}"}\n' for i in range(n)))
            host = measured([serve, '--apps', manifest, '--threads', 1, '--workers', 2], [f'/app{i}' for i in range(n)])
            print(json.dumps({'way': 'host', 'apps': n, **host}), flush=True)
    finally:
        shutil.rmtree(work)


if __name__ == '__main__':
    main()
