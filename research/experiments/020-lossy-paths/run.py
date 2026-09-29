#!/usr/bin/env python3
"""M6 of docs/design/011-pure-transport.md: a link on a path of delay, loss,
reordering and a bottleneck, against quinn, and beside TCP CUBIC.

Each case runs in `unshare -rn` (an unprivileged network namespace) whose
loopback is the path: MTU 1500, and TCP's segments no larger
(gso_max_size), so each is a packet. Each way netem gives delay (half the
RTT), jitter (packets reorder) and random loss; the way to the receivers
(ports 40000-40007) then goes through a 50 Mbit/s bottleneck (tbf) whose
queue holds a round trip of it, and the way back (answers, TCP's
acknowledgments) does not queue behind it. Pure and quinn send without UDP
GSO: a qdisc takes a GSO burst as one packet.

  bulk   a backlog of 1 KiB records for --seconds (Pure: a link and a
         listener, transport.c; quinn: one stream); the goodput from the
         receiver's records in each 100 ms, the first 2 s left out.
  paced  50 records a second for --seconds (well within what each carries); the time each took from
         queued (written) to handed over (read): p50, p99, max.
  share  Pure, quinn or a second TCP flow beside a TCP CUBIC flow, each a
         process of its own, started together, for --share-seconds; each
         one's share of the goodput from 20 s on (CUBIC's flows converge
         slowly at an RTT of 100 ms). The path's delay wavers by 1 ms: with
         a fixed delay a drop-tail queue drops one flow's packets far more
         than the other's by their phases alone (Floyd and Jacobson, 1992),
         and which flow that is says nothing of either.

    python3 research/experiments/020-lossy-paths/run.py [--rounds 3] [--out FILE]
"""
import argparse
import json
import os
import shutil
import socket
import statistics
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
HERE = Path(__file__).resolve().parent
QUINN = ROOT / 'research/experiments/019-idle-links/quic/target/release/idle-quic'
TC = shutil.which('tc') or '/usr/sbin/tc'
IP = shutil.which('ip') or '/usr/sbin/ip'
RATE_MBIT, SIZE = 50, 1000
PURE_PORT, QUINN_PORT, TCP_PORT, OTHER_TCP_PORT = 40001, 40002, 40003, 40004  # the receivers, beyond the bottleneck
sys.path.insert(0, str(ROOT / 'tests'))


def build(work):
    """The transport harness, quinn's, the two modules and a certificate."""
    import check
    import importlib.util
    spec = importlib.util.spec_from_file_location('idle_links', ROOT / 'research/experiments/019-idle-links/run.py')
    idle_links = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(idle_links)
    subprocess.run(['make', '-s', '-C', str(ROOT), 'build/libpure.a', 'build/libpure-link.a', 'build/pure-crypto.so'],
                   check=True)
    subprocess.run(['cc', '-O2', '-g', '-std=c17', '-Wall', '-Wextra', '-Werror', f'-I{ROOT}/core', f'-I{ROOT}/link',
                    str(ROOT / 'research/experiments/019-idle-links/transport.c'), str(ROOT / 'build/libpure-link.a'),
                    str(ROOT / 'build/libpure.a'), '-lm', '-pthread', '-ldl', '-o', str(ROOT / 'build/link-transport')],
                   check=True)
    subprocess.run(['cargo', 'build', '--release', '--offline', '--quiet'], cwd=QUINN.parents[2], check=True)
    for name, text in (('sender', check.LINK_SENDER), ('receiver', check.LINK_RECEIVER)):
        (work / f'{name}.aup').write_text(text)
        check.compile_module(Path(os.environ.get('AUP', ROOT.parent / 'aup/build/aup')), work / f'{name}.aup',
                             work / f'{name}.pmod')
    idle_links.make_certificate(work)


# ---- Inside the namespace


def path(rtt_ms, loss, jitter_ms):
    """The loopback as the path: back (band 1:1) and forth (1:2, to the
    receivers' ports, with the bottleneck after its netem)."""
    subprocess.run([IP, 'link', 'set', 'lo', 'up', 'mtu', '1500', 'gso_max_size', '1500', 'gso_max_segs', '1'],
                   check=True)
    queue = max(30000, RATE_MBIT * 1000000 // 8 * rtt_ms // 1000)  # a round trip of the bottleneck, in bytes
    netem = ['delay', f'{rtt_ms / 2}ms'] + ([f'{jitter_ms}ms'] if jitter_ms else []) + \
            (['loss', f'{loss}%'] if loss else []) + ['limit', '100000']
    for rule in (['qdisc', 'add', 'dev', 'lo', 'root', 'handle', '1:', 'prio', 'bands', '2', 'priomap'] + ['0'] * 16,
                 ['qdisc', 'add', 'dev', 'lo', 'parent', '1:1', 'handle', '10:', 'netem'] + netem,
                 ['qdisc', 'add', 'dev', 'lo', 'parent', '1:2', 'handle', '20:', 'netem'] + netem,
                 ['qdisc', 'add', 'dev', 'lo', 'parent', '20:1', 'handle', '21:', 'tbf', 'rate', f'{RATE_MBIT}mbit',
                  'burst', '15000', 'limit', str(queue)],
                 ['filter', 'add', 'dev', 'lo', 'protocol', 'ip', 'parent', '1:0', 'prio', '1', 'u32', 'match', 'ip',
                  'dport', '40000', '0xfff8', 'flowid', '1:2']):
        subprocess.run([TC] + rule, check=True)


def goodput(timeline, record_bytes, skip_s=2.0, until_s=None):
    """Mbit/s of records from skip_s on (to until_s)."""
    first = int(skip_s * 10)
    last = len(timeline) if until_s is None else min(len(timeline), int(until_s * 10))
    counted = timeline[first:last]
    return round(sum(counted) * record_bytes * 8 / (len(counted) / 10) / 1e6, 2) if counted else 0.0


def start_pure(work, seconds, rate=0, count=2000000):
    return subprocess.Popen([str(ROOT / 'build/link-transport'), str(work / 'sender.pmod'), str(work / 'receiver.pmod'),
                             str(count), str(SIZE), '20000', str(rate), str(seconds)], stdout=subprocess.PIPE,
                            stderr=subprocess.PIPE, text=True,
                            env={**os.environ, 'TRANSPORT_GSO': '0', 'TRANSPORT_PORT': str(PURE_PORT)})


def finish_pure(process, seconds):
    out, error = process.communicate(timeout=seconds + 120)
    assert process.returncode == 0, error[-2000:]
    return json.loads(out)


def pure(work, seconds, rate=0):
    return finish_pure(start_pure(work, seconds, rate), seconds)


def quinn_server(work):
    server = subprocess.Popen([str(QUINN), 'serve', str(work / 'cert.der'), str(work / 'key.der'), str(QUINN_PORT)],
                              stdout=subprocess.PIPE, text=True, env={**os.environ, 'QUIC_GSO': '0'})
    port = json.loads(server.stdout.readline())['port']
    return server, port


def start_quinn(work, seconds, rate=0, count=2000000):
    server, port = quinn_server(work)
    # The bytes of Pure's record: its prefix (count, item's length), the item and the city ("Seoul").
    client = subprocess.Popen([str(QUINN), 'drain', str(port), str(count), str(SIZE + 8 + 5), str(rate), str(seconds)],
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
                              env={**os.environ, 'QUIC_GSO': '0'})
    return server, client


def finish_quinn(started, seconds):
    server, client = started
    try:
        _, error = client.communicate(timeout=seconds + 300)
        assert client.returncode == 0, error[-2000:]
        line = server.stdout.readline()
    finally:
        server.kill()
        server.wait()
    return json.loads(line)


def quinn(work, seconds, rate=0):
    return finish_quinn(start_quinn(work, seconds, rate), seconds)


def start_tcp(port, seconds):
    """A TCP CUBIC flow in a process of its own (--tcp)."""
    return subprocess.Popen([sys.executable, __file__, '--tcp', f'{port}:{seconds}'], stdout=subprocess.PIPE, text=True)


def finish_tcp(process, seconds):
    out, _ = process.communicate(timeout=seconds + 60)
    assert process.returncode == 0
    return json.loads(out)['timeline_100ms']


class Tcp:
    """A TCP CUBIC flow: a sender writing as fast as it may for seconds, a
    receiver counting bytes in each 100 ms from when it starts."""

    def __init__(self, seconds, port):
        self.seconds = seconds
        self.listener = socket.socket()
        self.listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.listener.bind(('127.0.0.1', port))
        self.listener.listen(1)
        self.timeline = []
        self.began = None

    def start(self):
        self.began = time.monotonic()
        self.receiver = threading.Thread(target=self.receive)
        self.sender = threading.Thread(target=self.send)
        self.receiver.start()
        self.sender.start()

    def receive(self):
        connection, _ = self.listener.accept()
        with connection:
            while True:
                data = connection.recv(1 << 20)
                if not data:
                    break
                bucket = int((time.monotonic() - self.began) * 10)
                while len(self.timeline) <= bucket:
                    self.timeline.append(0)
                self.timeline[bucket] += len(data)

    def send(self):
        s = socket.socket()
        s.setsockopt(socket.IPPROTO_TCP, socket.TCP_CONGESTION, b'cubic')
        s.connect(self.listener.getsockname())
        chunk = b'x' * 65536
        while time.monotonic() - self.began < self.seconds:
            s.sendall(chunk)
        s.close()

    def join(self):
        self.sender.join()
        self.receiver.join()
        self.listener.close()
        return self.timeline


def inner(case):
    """One case, in the namespace: a line of JSON."""
    work = Path(case['work'])
    path(case['rtt_ms'], case['loss'], case['jitter_ms'])
    seconds, kind, who = case['seconds'], case['kind'], case['who']
    row = dict(case)
    del row['work']
    if kind in ('bulk', 'paced'):
        rate = 50 if kind == 'paced' else 0
        result = pure(work, seconds, rate) if who == 'pure' else quinn(work, seconds, rate)
        timeline = result['timeline_100ms']
        row['mbit'] = goodput(timeline, SIZE + 13, 2.0, seconds)
        for key in ('waited_ms_p50', 'waited_ms_p99', 'waited_ms_max', 'resent', 'lost', 'late', 'undone', 'packets'):
            if key in result:
                row[key] = result[key]
    else:  # share: who beside a TCP CUBIC flow, started together
        tcp = start_tcp(TCP_PORT, seconds)
        if who == 'tcp':
            theirs = [b / (SIZE + 13) for b in finish_tcp(start_tcp(OTHER_TCP_PORT, seconds), seconds)]
        elif who == 'pure':
            theirs = finish_pure(start_pure(work, seconds), seconds)['timeline_100ms']
        else:
            theirs = finish_quinn(start_quinn(work, seconds), seconds)['timeline_100ms']
        mine = finish_tcp(tcp, seconds)
        row['mbit'] = goodput(theirs, SIZE + 13, 20.0, seconds - 1)
        row['tcp_mbit'] = goodput(mine, 1, 20.0, seconds - 1)
        total = row['mbit'] + row['tcp_mbit']
        row['share'] = round(row['mbit'] / total, 3) if total else 0.0
    return row


# ---- Outside


def cases(seconds, share_seconds):
    for rtt in (20, 100):
        for loss in (0, 1, 5):
            for jitter in (0, 3):
                for who in ('pure', 'quinn'):
                    yield {'kind': 'bulk', 'who': who, 'rtt_ms': rtt, 'loss': loss, 'jitter_ms': jitter, 'seconds': seconds}
        for loss in (0, 1, 5):
            for who in ('pure', 'quinn'):
                yield {'kind': 'paced', 'who': who, 'rtt_ms': rtt, 'loss': loss, 'jitter_ms': 0, 'seconds': seconds}
        for who in ('pure', 'quinn', 'tcp'):
            yield {'kind': 'share', 'who': who, 'rtt_ms': rtt, 'loss': 0, 'jitter_ms': 1, 'seconds': share_seconds}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--rounds', type=int, default=1)
    parser.add_argument('--seconds', type=int, default=20)
    parser.add_argument('--share-seconds', type=int, default=60)
    parser.add_argument('--only', default=None, help='kinds to run, for example bulk,share')
    parser.add_argument('--out', type=Path, default=None)
    parser.add_argument('--inner', default=None, help=argparse.SUPPRESS)
    parser.add_argument('--tcp', default=None, help=argparse.SUPPRESS)
    args = parser.parse_args()
    if args.tcp:
        port, seconds = (int(v) for v in args.tcp.split(':'))
        flow = Tcp(seconds, port)
        flow.start()
        print(json.dumps({'timeline_100ms': flow.join()}), flush=True)
        return
    if args.inner:
        print(json.dumps(inner(json.loads(args.inner))), flush=True)
        return
    with tempfile.TemporaryDirectory(prefix='pure-m6-') as directory:
        work = Path(directory)
        build(work)
        only = set(args.only.split(',')) if args.only else None
        for round_ in range(args.rounds):
            for case in cases(args.seconds, args.share_seconds):
                if only and case['kind'] not in only:
                    continue
                case = {**case, 'round': round_, 'work': str(work)}
                done = subprocess.run(['unshare', '-rn', sys.executable, __file__, '--inner', json.dumps(case)],
                                      capture_output=True, text=True, timeout=case['seconds'] + 600)
                if done.returncode:
                    print(json.dumps({**case, 'error': done.stderr[-1500:]}), flush=True)
                    continue
                line = done.stdout.strip().splitlines()[-1]
                print(line, flush=True)
                if args.out:
                    with open(args.out, 'a') as out:
                        out.write(line + '\n')


if __name__ == '__main__':
    main()
