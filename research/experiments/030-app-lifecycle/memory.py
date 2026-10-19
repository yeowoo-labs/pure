#!/usr/bin/env python3
"""What changing an app leaves in a running host's memory (docs/design/017's
stage 3, research 115). A host of apps holds an app of items.aup (a
thousand items, its store in RAM); its own memory (smaps_rollup's
Private_Dirty and Pss) is read after a warm-up of changes, then after
CHANGES changes more: in place to another module (its line naming in turn
items.pmod and items-next.pmod: each reading loads a module, and keeps the
one it replaced), and with its module as it was (its queue budget changed
in turn: no module loaded). The difference a change. Each reading waits
SETTLE seconds first: a module replaced is freed once no request or snapshot
reads it and each connection thread has passed by (they wake each second);
the host's own count of the modules it freed and still holds comes with it.

memory.py PURE_SERVE CHANGES [SETTLE]   a JSON line per way
"""
import json
import select
import shutil
import signal
import subprocess
import sys
import tempfile
import time
from pathlib import Path

from window import AUP, HERE, grow


def own(pid):
    fields = {}
    for line in Path(f'/proc/{pid}/smaps_rollup').read_text().splitlines()[1:]:
        name, value = line.split(':', 1)
        fields[name] = int(value.split()[0])
    return fields['Private_Dirty'], fields['Pss']


def main():
    serve, changes = Path(sys.argv[1]).resolve(), int(sys.argv[2])
    settle = float(sys.argv[3]) if len(sys.argv) > 3 else 2
    work = Path(tempfile.mkdtemp(prefix='pure-030-', dir='/dev/shm'))
    try:
        modules = {}
        for name in ('items', 'items-next'):
            modules[name] = work / f'{name}.pmod'
            subprocess.run([str(AUP), 'build', str(HERE / f'{name}.aup'), '-o', str(modules[name])], check=True)
        for way in ('module', 'budget'):
            conf = work / 'apps.conf'

            def write(n):
                module = 'items-next' if way == 'module' and n % 2 else 'items'
                budget = f' queue={1000 + n % 2}' if way == 'budget' else ''
                conf.write_text(f"x {modules[module]} {work / way}{budget}\n")

            write(0)
            host = subprocess.Popen([str(serve), '--apps', str(conf), '--port', '0'], stdout=subprocess.PIPE,
                                    stderr=subprocess.PIPE)
            port = json.loads(host.stdout.readline())['port']
            grow(port, 1000)

            def change(n):
                write(n)
                host.send_signal(signal.SIGHUP)
                ready, _, _ = select.select([host.stdout], [], [], 60)
                assert ready
                said = json.loads(host.stdout.readline())
                assert said['apps'][0]['did'] == 'changed' and said['apps'][0]['running'], said

            for n in range(1, 101):  # warm: the allocator's arenas settle
                change(n)
            time.sleep(settle)
            dirty, pss = own(host.pid)
            for n in range(101, 101 + changes):
                change(n)
            time.sleep(settle)
            dirty_after, pss_after = own(host.pid)
            host.terminate()
            _, errors = host.communicate(timeout=60)
            stats = {}
            for line in errors.decode().splitlines():
                if '"modules_freed"' in line:
                    stats = json.loads(line)
            print(json.dumps({'way': way, 'changes': changes, 'private_dirty_kib': [dirty, dirty_after],
                              'pss_kib': [pss, pss_after],
                              'per_change_bytes': round((dirty_after - dirty) * 1024 / changes),
                              'modules_freed': stats.get('modules_freed'), 'modules_retired': stats.get('modules_retired')}),
                  flush=True)
    finally:
        shutil.rmtree(work)


if __name__ == '__main__':
    main()
