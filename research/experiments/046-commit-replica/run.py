#!/usr/bin/env python3
"""Research 046: 029's principle 2 ("the commit is the replica") put to its breaking experiment on one Pi.

An owner (pure-run --store, with shim.so preloaded) takes `add` messages of entries.aup; every sync of its store ends a
cut that the shipper sends over pure link to the follower, which does the same ops under its own directory; the
owner's sync returns only once the follower saved the cut (synchronous). Both stores on /dev/shm (what is tested is
identity and taking over, not the medium).

  run.py identity EVENTS EVERY   the owner takes EVENTS messages; every EVERY cuts it pauses and the two stores are
                                  compared byte for byte (names and contents), and once more at the end
  run.py takeover RUNS EVENTS    RUNS times: the owner (and in every other run its shipper too, as when its machine
                                  dies) killed with SIGKILL after a random number of messages up to EVENTS; the
                                  follower's store opened by pure-run as the owner: it must open, hold every message
                                  the owner said done, none it was not sent, and the entries in order
  run.py torn RUNS EVENTS [B]    as takeover, the whole node killed every time, the cuts sent as records of at most B
                                  bytes (512): a kill leaves the follower part of a cut, as a crash leaves a disk
  run.py price EVENTS            the owner's time for EVENTS messages with and without the shim, and what went on the link

One JSON line a run (or a sample)."""
import json
import os
import random
import select
import shutil
import signal
import socket
import struct
import subprocess
import sys
import threading
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
PURE = HERE.parents[2]
AUP = Path('/home/eunhhu/work/aup/build/aup')
WORK = Path(os.environ.get('RUN_WORK', '/dev/shm/pure-046'))
RUN = PURE / 'build' / 'pure-run'
ENV = dict(os.environ, PURE_CRYPTO=str(PURE / 'build' / 'pure-crypto.so'))


def build():
    WORK.mkdir(parents=True, exist_ok=True)
    cc = ['cc', '-O2', '-std=gnu17', '-Wall', '-Wextra']
    subprocess.run(cc + ['-fPIC', '-shared', str(HERE / 'shim.c'), '-ldl', '-pthread', '-o', str(WORK / 'shim.so')], check=True)
    for name in ('shipper', 'follower'):
        subprocess.run(cc + [f'-I{PURE / "link"}', str(HERE / f'{name}.c'), str(PURE / 'build' / 'libpure-link.a'),
                             '-lm', '-pthread', '-ldl', '-o', str(WORK / name)], check=True)
    subprocess.run([str(AUP), 'build', str(HERE / 'entries.aup'), '-o', str(WORK / 'entries.pmod')], check=True)
    keys = json.loads(subprocess.run([str(WORK / 'follower'), 'keys', str(WORK)], capture_output=True, text=True,
                                     check=True, env=ENV).stdout)
    return keys


def free_port():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.bind(('127.0.0.1', 0))
    port = s.getsockname()[1]
    s.close()
    return port


def message(i):  # not whole: a logged change takes its 8 bytes, so the log outgrows the list's items within the run
    return 'message add ' + struct.pack('<d', i % 1000 + 0.25).hex() + '\n'


def stores_differ(a, b):
    """Differences between two store directories: names in one only, and files whose bytes differ (the first offset)."""
    names_a = {p.relative_to(a) for p in a.rglob('*')}
    names_b = {p.relative_to(b) for p in b.rglob('*')}
    out = [f'only owner: {n}' for n in sorted(names_a - names_b)] + [f'only follower: {n}' for n in sorted(names_b - names_a)]
    for n in sorted(names_a & names_b):
        x, y = a / n, b / n
        if x.is_dir() or y.is_dir():
            if x.is_dir() != y.is_dir():
                out.append(f'kind: {n}')
            continue
        p, q = x.read_bytes(), y.read_bytes()
        if p != q:
            first = next((i for i in range(min(len(p), len(q))) if p[i] != q[i]), min(len(p), len(q)))
            out.append(f'bytes: {n} sizes {len(p)} {len(q)} first difference at {first}')
    return out


class Run:
    """A follower, a shipper and an owner, started in that order on fresh stores."""

    def __init__(self, keys, every=0, shim=True):
        self.owner_dir, self.follower_dir = WORK / 'owner-store', WORK / 'follower-store'
        for d in (self.owner_dir, self.follower_dir):
            shutil.rmtree(d, ignore_errors=True)
        port = free_port()
        self.follower = subprocess.Popen([str(WORK / 'follower'), str(self.follower_dir), str(port), str(WORK / 'follower.key'),
                                          keys['owner']], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, env=ENV)
        assert json.loads(self.follower.stdout.readline())['ready']
        sock = WORK / 'ship.sock'
        self.shipper = subprocess.Popen([str(WORK / 'shipper'), str(sock), f'pure://127.0.0.1:{port}/store',
                                         str(WORK / 'owner.key'), keys['follower']], stdout=subprocess.PIPE,
                                        stderr=subprocess.PIPE, text=True, env=ENV)
        assert json.loads(self.shipper.stdout.readline())['ready']
        env = dict(ENV)
        if shim:
            env.update(LD_PRELOAD=str(WORK / 'shim.so'), SHIM_STORE=str(self.owner_dir), SHIM_SOCKET=str(sock),
                       SHIM_STATS=str(WORK / 'shim-stats.json'))
        if every:
            for f in ('paused', 'resume'):
                p = WORK / f
                if p.exists():
                    p.unlink()
                os.mkfifo(p)
            self.paused = os.open(WORK / 'paused', os.O_RDWR)  # held open both ways: no open waits for the other end
            self.resume = os.open(WORK / 'resume', os.O_RDWR)
            env.update(SHIM_PAUSE_EVERY=str(every), SHIM_PAUSE_DIR=str(WORK))
        self.owner = subprocess.Popen([str(RUN), str(WORK / 'entries.pmod'), '--views', '--store', str(self.owner_dir)],
                                      stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
                                      env=env, bufsize=1)
        self.done = 0  # messages the owner said done (each line comes after its save, so after the follower's ack)
        self.reader = threading.Thread(target=self.read, daemon=True)
        self.reader.start()

    def read(self):
        for line in self.owner.stdout:
            if line.startswith('{'):
                self.done += 1

    def feed(self, events, stop_after=None):
        sent = 0
        try:
            for i in range(events):
                self.owner.stdin.write(message(i))
                sent += 1
                if stop_after is not None and sent >= stop_after:
                    break
            if stop_after is None:
                self.owner.stdin.close()
        except BrokenPipeError:
            pass
        return sent

    def stop_follower(self):
        self.follower.send_signal(signal.SIGTERM)
        out, err = self.follower.communicate(timeout=60)
        return json.loads(out.strip().splitlines()[-1]) if out.strip() else {'error': err[-500:]}


def opened(store):
    """The store opened by pure-run as its owner: the views (total, summed) it shows, or why it did not open."""
    r = subprocess.run([str(RUN), str(WORK / 'entries.pmod'), '--views', '--store', str(store)], input='show\n',
                       capture_output=True, text=True, env=ENV, timeout=120)
    lines = [l for l in r.stdout.splitlines() if l.startswith('{')]
    if r.returncode or not lines:
        return {'opened': False, 'exit': r.returncode, 'stderr': r.stderr[-300:]}
    shown = json.loads(lines[-1])
    return {'opened': True, 'fault': shown.get('fault'), **shown['views']}


def summed(n):  # every partial sum a multiple of 0.25 below 2^50: exact as a binary64
    return float(sum(i % 1000 for i in range(n))) + 0.25 * n


def identity(keys, events, every):
    run = Run(keys, every=every)
    feeder = threading.Thread(target=run.feed, args=(events,), daemon=True)
    feeder.start()
    samples, differing = 0, []
    while True:
        ready, _, _ = select.select([run.paused], [], [], 0.2)
        if ready:
            cut = int(os.read(run.paused, 64).split()[0])
            diff = stores_differ(run.owner_dir, run.follower_dir)
            samples += 1
            if diff:
                differing.append({'cut': cut, 'differences': diff[:5]})
            os.write(run.resume, b'g')
        elif run.owner.poll() is not None:
            break
    feeder.join()
    run.reader.join(timeout=10)
    owner_exit = run.owner.wait()
    shipper_out = run.shipper.communicate(timeout=60)[0].strip().splitlines()
    end = stores_differ(run.owner_dir, run.follower_dir)
    follower = run.stop_follower()
    stats = json.loads((WORK / 'shim-stats.json').read_text())
    files = {p.name: p.stat().st_size for p in sorted(run.owner_dir.iterdir())}
    print(json.dumps({'run': 'identity', 'events': events, 'done': run.done, 'owner_exit': owner_exit, 'every': every,
                      'samples': samples, 'samples_differing': len(differing), 'first_differing': differing[:3],
                      'end_differences': end[:5], 'shim': stats, 'shipper': json.loads(shipper_out[-1]) if shipper_out else None,
                      'follower': follower, 'owner_files': files,
                      'follower_opened': opened(run.follower_dir)}), flush=True)


def takeover(keys, runs, events, torn=False):
    for k in range(runs):
        whole_node = torn or k % 2 == 1  # the shipper dies with the owner
        stop_after = random.randint(1, events)
        run = Run(keys)
        sent = run.feed(events, stop_after=stop_after)
        deadline = time.time() + 120
        while run.done < max(1, sent // 2) and time.time() < deadline and run.owner.poll() is None:
            time.sleep(0.001)
        # the owner is working through what was sent: kill it at a moment it does not choose
        time.sleep(random.random() * 0.05)
        run.owner.send_signal(signal.SIGKILL)
        if whole_node:
            run.shipper.send_signal(signal.SIGKILL)
        run.owner.wait()
        done = run.done
        if not whole_node:
            run.shipper.communicate(timeout=60)
        else:
            run.shipper.wait()
        time.sleep(1.0)  # what was on the way reaches the follower
        follower = run.stop_follower()
        took = opened(run.follower_dir)
        good = (took.get('opened') and took.get('fault') == 'none' and done <= took.get('total', -1) <= sent and
                took.get('summed') == summed(int(took.get('total', 0))))
        print(json.dumps({'run': 'takeover', 'k': k, 'whole_node': whole_node, 'record': os.environ.get('SHIPPER_RECORD'), 'sent': sent, 'owner_said_done': done,
                          'follower_took': took, 'follower': follower, 'passed': bool(good),
                          'owner_opened': opened(run.owner_dir)}), flush=True)


def price(keys, events):
    out = {}
    for shim in (False, True):
        run = Run(keys, shim=shim)
        start = time.perf_counter()
        run.feed(events)
        run.owner.wait()
        run.reader.join(timeout=10)
        seconds = time.perf_counter() - start
        run.owner.stdin = None
        shipper = run.shipper
        if not shim:
            shipper.terminate()
        lines = shipper.communicate(timeout=60)[0].strip().splitlines()
        run.stop_follower()
        key = 'with_shim' if shim else 'without'
        out[key] = {'seconds': round(seconds, 3), 'us_per_event': round(1e6 * seconds / events, 1), 'done': run.done}
        if shim:
            out['shim'] = json.loads((WORK / 'shim-stats.json').read_text())
            out['shipper'] = json.loads(lines[-1]) if lines else None
    print(json.dumps({'run': 'price', 'events': events, **out}), flush=True)


def main():
    keys = build()
    what = sys.argv[1]
    if what == 'identity':
        identity(keys, int(sys.argv[2]), int(sys.argv[3]))
    elif what == 'takeover':
        takeover(keys, int(sys.argv[2]), int(sys.argv[3]))
    elif what == 'torn':  # the whole node killed, its cuts sent as small records: the follower left with part of one
        os.environ['SHIPPER_RECORD'] = sys.argv[4] if len(sys.argv) > 4 else '512'
        ENV['SHIPPER_RECORD'] = os.environ['SHIPPER_RECORD']
        takeover(keys, int(sys.argv[2]), int(sys.argv[3]), torn=True)
    elif what == 'price':
        price(keys, int(sys.argv[2]))


if __name__ == '__main__':
    main()
