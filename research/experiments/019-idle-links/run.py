#!/usr/bin/env python3
"""M1 of docs/design/011-pure-transport.md: what a sender at rest costs a
Pure node that takes records from it over a link without a connection.

For each count N of senders: a node (pure-serve, the receiver of
tests/check.py's link checks) allows N senders (--senders) and serves its
--link-port; build/links (links.c) makes each sender send one order and
wait until the node stored it; then every sender is quiet. The node's
memory (VmRSS, and Pss of its anonymous memory) is read when it has
allowed them, right after they sent, and after it let their active state
go (--link-idle-ms); then 1,000 senders at rest send one more order each
(the wake: one X25519 each on the node). A line of JSON for each N.

With --quic, the same for a quinn service (quic/, built offline): n
clients connect, send one order each and wait until it is stored, and stay
connected at rest; the service's memory is read then; then each sends one
more order over its connection.

With --restart (M2), n devices each make an order every --period-ms and
send it until it is stored; at --kill-at seconds the receiver is killed
(-9) and started again at once: the orders stored each 100 ms, the CPU of
both sides before and after, the X25519 made (none when the node
remembers its secrets; --no-memo forgets them), and that every order made
was applied once. With --quic too, the same for quinn clients, which
connect again when their connection is lost.

With --first-contact DELAYS (M4), for each one-way delay in ms, in a
network namespace of its own (unshare -rn) whose loopback delays UDP:
senders the node never heard from, and new quinn clients, each send one
order and wait until it is stored, one at a time.

With --transport SIZE:COUNT,... (M5), transport.c drains a backlog of
COUNT records through a link and a listener in one process (no app, no
store), then quinn sends as many messages of the same size over one
stream; --rounds times each. With --drain SIZES (M5 as the design says),
two pure-serve nodes: the sender's app saves --counts orders while the
receiver is away, then the receiver starts and the link drains them;
quinn does the same over one stream.

    python3 research/experiments/019-idle-links/run.py [--counts 1000,10000]
        [--store DIRECTORY] [--out FILE.jsonl] [--quic] [--restart [--no-memo]]
        [--first-contact 5,25,50] [--transport 48:200000 --rounds 5] [--drain 48,1000]
"""
import argparse
import json
import os
import random
import select
import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / 'tests'))
import check  # noqa: E402  (LINK_RECEIVER, compile_module)


def memory(pid):
    """The node's resident memory and the proportional share of its anonymous memory (kB)."""
    status = dict(line.split(':', 1) for line in Path(f'/proc/{pid}/status').read_text().splitlines() if ':' in line)
    rollup = {}
    for line in Path(f'/proc/{pid}/smaps_rollup').read_text().splitlines()[1:]:
        name, value = line.split(':', 1)
        rollup[name] = int(value.split()[0])
    return {'rss_kb': int(status['VmRSS'].split()[0]), 'hwm_kb': int(status['VmHWM'].split()[0]),
            'anonymous_kb': rollup.get('Anonymous', 0)}


def cpu_ms(pid):
    """The process's CPU time so far (user and system), in ms."""
    fields = Path(f'/proc/{pid}/stat').read_text().rsplit(')', 1)[1].split()
    return (int(fields[11]) + int(fields[12])) * 1000 / os.sysconf('SC_CLK_TCK')


def free_port(kind):
    with socket.socket(socket.AF_INET, kind) as probe:
        probe.bind(('127.0.0.1', 0))
        return probe.getsockname()[1]


def start(args):
    process = subprocess.Popen([str(a) for a in args], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    ready, _, _ = select.select([process.stdout], [], [], 600)
    line = process.stdout.readline() if ready else b''
    if not line:
        process.kill()
        raise SystemExit(f'the node did not start: {process.stderr.read(2000)}')
    return process, json.loads(line)


def view(port, name):
    import http.client
    connection = http.client.HTTPConnection('127.0.0.1', port, timeout=30)
    connection.request('GET', f'/views/{name}')
    body = connection.getresponse().read()
    connection.close()
    return json.loads(body)


def settle(pid, seconds):
    """Memory once the node is quiet: read until it stops moving."""
    time.sleep(seconds)
    last = memory(pid)
    for _ in range(20):
        time.sleep(0.5)
        now = memory(pid)
        if abs(now['rss_kb'] - last['rss_kb']) < 64:
            return now
        last = now
    return last


def one(n, work, keys, module, idle_ms, window):
    senders = work / f'senders-{n}'
    with open(f'{keys}.senders') as source, open(senders, 'w') as out:
        for _, line in zip(range(n), source):
            out.write(line)
    store = work / f'store-{n}'
    subprocess.run(['rm', '-rf', str(store)], check=True)
    port, link_port = free_port(socket.SOCK_STREAM), free_port(socket.SOCK_DGRAM)
    base = [ROOT / 'build/pure-serve', '--module', module, '--store', store, '--port', port, '--key', work / 'node.key',
            '--link-port', link_port, '--link-idle-ms', idle_ms]
    row = {'senders': n, 'idle_ms': idle_ms}
    # The node alone, then with the senders allowed.
    for name, args in (('empty', base), ('allowed', base + ['--senders', senders])):
        subprocess.run(['rm', '-rf', str(store)], check=True)
        started = time.monotonic()
        node, info = start(args)
        row[f'start_{name}_seconds'] = round(time.monotonic() - started, 3)
        row[name] = settle(node.pid, 1.0)
        if name == 'empty':
            node.terminate()
            node.communicate(timeout=60)
    node_key = info['public_key']
    links = [ROOT / 'build/links', 'send', keys, module, 'order', '127.0.0.1', link_port, node_key]
    cpu = cpu_ms(node.pid)
    sent = subprocess.run([str(a) for a in links + [0, n, 1, window]], capture_output=True, text=True)
    assert sent.returncode == 0, sent.stderr
    row['send'] = json.loads(sent.stdout)
    row['node_cpu_ms_per_send'] = round((cpu_ms(node.pid) - cpu) / n, 4)
    row['sent'] = memory(node.pid)
    row['after_idle'] = settle(node.pid, idle_ms / 1000 + 1.5)
    assert view(port, 'count') == n, view(port, 'count')
    wake = min(n, 1000)
    first = random.Random(n).randrange(0, n - wake + 1)
    cpu = cpu_ms(node.pid)
    woken = subprocess.run([str(a) for a in links + [first, wake, 2, 1]], capture_output=True, text=True)
    assert woken.returncode == 0, woken.stderr
    row['wake'] = json.loads(woken.stdout)
    row['node_cpu_ms_per_wake'] = round((cpu_ms(node.pid) - cpu) / wake, 4)
    assert view(port, 'count') == n + wake
    node.terminate()
    _, error = node.communicate(timeout=120)
    stats = [json.loads(line) for line in error.decode().splitlines() if line.startswith('{"active"')][-1]
    row['listener'] = {k: v for k, v in stats.items() if k.startswith('listener_')}
    row['store_bytes'] = sum(f.stat().st_size for f in store.rglob('*') if f.is_file())
    for name in ('allowed', 'after_idle'):
        row[f'{name}_bytes_per_sender'] = round((row[name]['anonymous_kb'] - row['empty']['anonymous_kb']) * 1024 / n, 1)
    row['active_bytes_per_sender'] = round((row['sent']['anonymous_kb'] - row['after_idle']['anonymous_kb']) * 1024 / n, 1)
    return row


def quic_one(n, work, window):
    """The same with quinn: a service holding n clients connected at rest."""
    binary = Path(__file__).parent / 'quic/target/release/idle-quic'
    server, info = start([binary, 'serve', work / 'cert.der', work / 'key.der'])
    row = {'clients': n, 'empty': settle(server.pid, 1.0)}
    cpu = cpu_ms(server.pid)
    client = subprocess.Popen([str(binary), 'connect', str(info['port']), '0', str(n), str(window)],
                              stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    ready, _, _ = select.select([client.stdout], [], [], 3600)
    line = client.stdout.readline() if ready else b''
    assert line, client.stderr.read(2000)
    row['connect'] = json.loads(line)
    row['service_cpu_ms_per_connect'] = round((cpu_ms(server.pid) - cpu) / n, 4)
    row['connected'] = settle(server.pid, 2.0)
    cpu = cpu_ms(server.pid)
    client.stdin.close()  # one more order each, over the connections at rest
    ready, _, _ = select.select([client.stdout], [], [], 3600)
    row['again'] = json.loads(client.stdout.readline())
    row['service_cpu_ms_per_again'] = round((cpu_ms(server.pid) - cpu) / n, 4)
    row['after_again'] = memory(server.pid)
    client.wait(timeout=600)
    # Connections not kept: each order on a new one, one at a time (a handshake each).
    cpu = cpu_ms(server.pid)
    fresh = subprocess.run([str(binary), 'connect', str(info['port']), str(n), '1000', '1'], input=b'',
                           capture_output=True, timeout=600)
    row['fresh'] = json.loads(fresh.stdout.decode().splitlines()[0])
    row['service_cpu_ms_per_fresh'] = round((cpu_ms(server.pid) - cpu) / 1000, 4)
    server.kill()
    server.wait()
    row['bytes_per_client'] = round((row['connected']['anonymous_kb'] - row['empty']['anonymous_kb']) * 1024 / n, 1)
    return row


def make_certificate(work):
    """A self-signed certificate (ECDSA P-256) and its key, as DER."""
    subprocess.run(['openssl', 'req', '-x509', '-newkey', 'ec', '-pkeyopt', 'ec_paramgen_curve:prime256v1', '-nodes',
                    '-keyout', str(work / 'key.pem'), '-out', str(work / 'cert.pem'), '-days', '2', '-subj', '/CN=localhost',
                    '-addext', 'subjectAltName=DNS:localhost'], check=True, capture_output=True)
    subprocess.run(['openssl', 'x509', '-in', str(work / 'cert.pem'), '-outform', 'DER', '-out', str(work / 'cert.der')],
                   check=True)
    subprocess.run(['openssl', 'pkcs8', '-topk8', '-nocrypt', '-in', str(work / 'key.pem'), '-outform', 'DER', '-out',
                    str(work / 'key.der')], check=True)


def quic(counts, work, window, out):
    root = Path(__file__).parent / 'quic'
    subprocess.run(['cargo', 'build', '--release', '--offline', '--quiet'], cwd=root, check=True)
    make_certificate(work)
    for n in counts:
        line = json.dumps(quic_one(n, work, window))
        print(line, flush=True)
        if out:
            with open(out, 'a') as file:
                file.write(line + '\n')


def recovery(timeline, killed_ms):
    """From a timeline of orders stored each 100 ms and when the receiver was
    killed: the steady rate in the 5 s before; the 100 ms steps after the
    kill with nothing stored; and how long until the orders stored since the
    kill caught up with the steady rate (99 %), what waited included."""
    start = killed_ms // 100
    before = timeline[max(0, start - 50):start]
    steady = sum(before) / len(before) if before else 0
    silent = sum(1 for b in range(start, min(len(timeline), start + 100)) if not timeline[b])
    since = 0
    for b in range(start, len(timeline)):
        since += timeline[b]
        if b > start and since >= steady * (b + 1 - start) * 0.99:
            return {'steady_per_100ms': round(steady, 1), 'silent_100ms_steps': silent, 'caught_up_ms': (b + 1 - start) * 100}
    return {'steady_per_100ms': round(steady, 1), 'silent_100ms_steps': silent, 'caught_up_ms': None}


def restart_pure(n, work, keys, module, period_ms, seconds, kill_at, memo):
    """M2 for Pure: n devices send a record every period_ms; the node is
    killed (-9) at kill_at seconds and started again at once."""
    senders = work / f'senders-{n}'
    with open(f'{keys}.senders') as source, open(senders, 'w') as out:
        for _, line in zip(range(n), source):
            out.write(line)
    store = work / f'restart-{n}'
    subprocess.run(['rm', '-rf', str(store), str(work / 'node.key.links')], check=True)
    port, link_port = free_port(socket.SOCK_STREAM), free_port(socket.SOCK_DGRAM)
    args = [ROOT / 'build/pure-serve', '--module', module, '--store', store, '--port', port, '--key', work / 'node.key',
            '--link-port', link_port, '--senders', senders]
    node, info = start(args)
    generator = subprocess.Popen([str(a) for a in [ROOT / 'build/links', 'run', keys, module, 'order', '127.0.0.1',
                                  link_port, info['public_key'], 0, n, period_ms, seconds]],
                                 stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    for line in generator.stderr:  # progress, a line a second
        t = json.loads(line).get('t', 0)
        if t >= kill_at:
            break
    seen = time.monotonic()
    cpu_before, senders_before = cpu_ms(node.pid), cpu_ms(generator.pid)
    time.sleep(1.0)
    steady_cpu = cpu_ms(node.pid) - cpu_before  # ms of CPU in the second before the kill
    senders_steady = cpu_ms(generator.pid) - senders_before
    node.kill()
    node.wait()
    killed = time.monotonic()
    killed_ms = int((t + killed - seen) * 1000)  # on the generator's clock
    if not memo:
        (work / 'node.key.links').unlink()
    node, _ = start(args)
    row = {'devices': n, 'period_ms': period_ms, 'memo': memo, 'restart_ms': round((time.monotonic() - killed) * 1000, 1),
           'steady_cpu_ms_per_s': round(steady_cpu, 1), 'senders_steady_cpu_ms_per_s': round(senders_steady, 1)}
    cpu_start, senders_start = cpu_ms(node.pid), cpu_ms(generator.pid)
    time.sleep(10)
    row['recovery_cpu_ms_first_10s'] = round(cpu_ms(node.pid) - cpu_start, 1)
    row['senders_cpu_ms_first_10s'] = round(cpu_ms(generator.pid) - senders_start, 1)
    out, error = generator.communicate(timeout=600)
    result = json.loads(out)
    row['generator'] = {k: v for k, v in result.items() if k != 'timeline_100ms'}
    row['timeline_100ms'] = result['timeline_100ms']
    row['killed_ms'] = killed_ms
    row['recovery'] = recovery(result['timeline_100ms'], killed_ms)
    count = view(port, 'count')
    row['applied'] = count
    assert count == result['made'] == result['stored'], (count, result['made'], result['stored'])
    node.terminate()
    _, error = node.communicate(timeout=120)
    stats = [json.loads(line) for line in error.decode().splitlines() if line.startswith('{"active"')][-1]
    row['listener'] = {k: v for k, v in stats.items() if k.startswith('listener_')}
    return row


def restart_quic(n, work, period_ms, seconds, kill_at):
    """M2 for quinn: the same load; the service is killed (-9) and started
    again on its port, with the same key (so it resets the old connections)."""
    binary = Path(__file__).parent / 'quic/target/release/idle-quic'
    port = free_port(socket.SOCK_DGRAM)
    args = [binary, 'serve', work / 'cert.der', work / 'key.der', port]
    server, _ = start(args)
    client = subprocess.Popen([str(binary), 'steady', str(port), '0', str(n), str(period_ms), str(seconds), '256'],
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    began = time.monotonic()
    time.sleep(kill_at)
    cpu_before, clients_before = cpu_ms(server.pid), cpu_ms(client.pid)
    time.sleep(1.0)
    steady_cpu = cpu_ms(server.pid) - cpu_before
    clients_steady = cpu_ms(client.pid) - clients_before
    server.kill()
    server.wait()
    killed = time.monotonic()
    server, _ = start(args)
    row = {'clients': n, 'period_ms': period_ms, 'restart_ms': round((time.monotonic() - killed) * 1000, 1),
           'killed_at_s': round(killed - began, 1), 'steady_cpu_ms_per_s': round(steady_cpu, 1),
           'senders_steady_cpu_ms_per_s': round(clients_steady, 1)}
    cpu_start, clients_start = cpu_ms(server.pid), cpu_ms(client.pid)
    time.sleep(10)
    row['recovery_cpu_ms_first_10s'] = round(cpu_ms(server.pid) - cpu_start, 1)
    row['senders_cpu_ms_first_10s'] = round(cpu_ms(client.pid) - clients_start, 1)
    out, _ = client.communicate(timeout=900)
    result = json.loads(out)
    row['client'] = {k: v for k, v in result.items() if k != 'timeline_100ms'}
    row['timeline_100ms'] = result['timeline_100ms']
    row['killed_ms'] = int((killed - began) * 1000)
    row['recovery'] = recovery(result['timeline_100ms'], row['killed_ms'])
    server.kill()
    server.wait()
    return row


def first_contact(work, delay_ms, count):
    """M4, run in a network namespace of its own whose loopback delays UDP
    by delay_ms each way: count senders the node never heard from each send
    one order and wait until it is stored, one at a time; then count quinn
    clients each connect, send one and wait. How many round trips each
    takes is the time over 2 delay_ms."""
    source = work / 'receiver.aup'
    source.write_text(check.LINK_RECEIVER)
    module = work / 'receiver.pmod'
    check.compile_module(Path(os.environ.get('AUP', ROOT.parent / 'aup/build/aup')), source, module)
    keys = work / 'keys'
    subprocess.run([str(ROOT / 'build/links'), 'keys', str(count), str(keys)], check=True)
    port, link_port = free_port(socket.SOCK_STREAM), free_port(socket.SOCK_DGRAM)
    node, info = start([ROOT / 'build/pure-serve', '--module', module, '--store', work / 'store', '--port', port,
                        '--key', work / 'node.key', '--link-port', link_port, '--senders', f'{keys}.senders'])
    sent = subprocess.run([str(a) for a in [ROOT / 'build/links', 'send', keys, module, 'order', '127.0.0.1', link_port,
                                            info['public_key'], 0, count, 1, 1]], capture_output=True, text=True)
    node.terminate()
    node.communicate(timeout=60)
    pure = json.loads(sent.stdout)
    make_certificate(work)
    binary = Path(__file__).parent / 'quic/target/release/idle-quic'
    server, info = start([binary, 'serve', work / 'cert.der', work / 'key.der'])
    fresh = subprocess.run([str(binary), 'connect', str(info['port']), '0', str(count), '1'], input=b'',
                           capture_output=True, timeout=600)
    server.kill()
    server.wait()
    quic = json.loads(fresh.stdout.decode().splitlines()[0])
    rtt = 2 * delay_ms
    return {'delay_ms_each_way': delay_ms, 'senders': count,
            'pure_ms_p50': pure['stored_ms_p50'], 'pure_ms_p99': pure['stored_ms_p99'],
            'pure_round_trips': round(pure['stored_ms_p50'] / rtt, 2),
            'quinn_ms_p50': quic['stored_ms_p50'], 'quinn_ms_p99': quic['stored_ms_p99'],
            'quinn_round_trips': round(quic['stored_ms_p50'] / rtt, 2)}


def in_namespace(delay_ms, count, out):
    """first_contact in `unshare -rn`, UDP alone through netem (the node's
    HTTP view stays quick)."""
    import shutil
    tc = shutil.which('tc') or '/usr/sbin/tc'
    ip = shutil.which('ip') or '/usr/sbin/ip'
    script = f"""
import json, subprocess, sys, tempfile
from pathlib import Path
sys.path.insert(0, {str(Path(__file__).parent)!r})
import run
subprocess.run([{ip!r}, 'link', 'set', 'lo', 'up'], check=True)
for rule in (['qdisc', 'add', 'dev', 'lo', 'root', 'handle', '1:', 'prio'],
             ['qdisc', 'add', 'dev', 'lo', 'parent', '1:3', 'handle', '30:', 'netem', 'delay', '{delay_ms}ms'],
             ['filter', 'add', 'dev', 'lo', 'protocol', 'ip', 'parent', '1:0', 'prio', '3', 'u32', 'match', 'ip',
              'protocol', '17', '0xff', 'flowid', '1:3']):
    subprocess.run([{tc!r}] + rule, check=True)
with tempfile.TemporaryDirectory(prefix='pure-m4-') as d:
    print(json.dumps(run.first_contact(Path(d), {delay_ms}, {count})))
"""
    done = subprocess.run(['unshare', '-rn', sys.executable, '-c', script], capture_output=True, text=True, timeout=1800)
    assert done.returncode == 0, done.stderr[-3000:]
    line = done.stdout.strip().splitlines()[-1]
    print(line, flush=True)
    if out:
        with open(out, 'a') as file:
            file.write(line + '\n')


def thread_cpu(pid, name):
    """CPU ms of the process's threads named name (all threads when None),
    from the scheduler's nanoseconds (schedstat), not clock ticks."""
    total = 0
    for task in Path(f'/proc/{pid}/task').iterdir():
        try:
            if name and (task / 'comm').read_text().strip() != name:
                continue
            total += int((task / 'schedstat').read_text().split()[0])
        except OSError:
            pass
    return total / 1e6


def post_all(port, n, size, threads=8):
    """n orders (item of size bytes) posted to the sender app, over threads connections."""
    import http.client
    import threading
    body = json.dumps({'item': 'x' * size, 'count': 1}).encode()
    left = [n]
    lock = threading.Lock()

    def work():
        connection = http.client.HTTPConnection('127.0.0.1', port, timeout=60)
        while True:
            with lock:
                if not left[0]:
                    break
                left[0] -= 1
            connection.request('POST', '/inputs/buy', body, {'Content-Type': 'application/json'})
            response = connection.getresponse()
            response.read()
            assert response.status == 200, response.status
        connection.close()
    workers = [threading.Thread(target=work) for _ in range(threads)]
    for w in workers:
        w.start()
    for w in workers:
        w.join()


def drain_pure(work, n, size):
    """M5 for Pure: n saved sends of an order whose item is size bytes wait
    in the sender's outbox (the receiver is not there); then the receiver
    starts, and the link drains them. Rate from the first applied to the
    last; CPU of both nodes, and of their link threads alone."""
    sender_module, receiver_module = work / 'drain-sender.pmod', work / 'drain-receiver.pmod'
    for module, text in ((sender_module, check.LINK_SENDER), (receiver_module, check.LINK_RECEIVER)):
        source = module.with_suffix('.aup')
        source.write_text(text)
        check.compile_module(Path(os.environ.get('AUP', ROOT.parent / 'aup/build/aup')), source, module)
    keys = {}
    for name in ('a', 'b'):
        node, info = start([ROOT / 'build/pure-serve', '--module', receiver_module, '--store', work / f'key-{name}',
                            '--port', free_port(socket.SOCK_STREAM), '--key', work / f'{name}.key'])
        keys[name] = info['public_key']
        node.terminate()
        node.communicate(timeout=60)
    (work / 'drain-senders').write_text(f'drain-sender.ordered {keys["a"]}\n')
    link_port, port_a, port_b = free_port(socket.SOCK_DGRAM), free_port(socket.SOCK_STREAM), free_port(socket.SOCK_STREAM)
    a, _ = start([ROOT / 'build/pure-serve', '--module', sender_module, '--store', work / 'drain-a', '--port', port_a,
                  '--threads', '2', '--key', work / 'a.key', '--send', f'ordered=pure://127.0.0.1:{link_port}/order',
                  '--seal', f'ordered={keys["b"]}'])
    post_all(port_a, n, size)
    assert view(port_a, 'sent') == n
    b, _ = start([ROOT / 'build/pure-serve', '--module', receiver_module, '--store', work / 'drain-b', '--port', port_b,
                  '--threads', '2', '--key', work / 'b.key', '--link-port', link_port, '--senders', work / 'drain-senders'])
    cpu = {name: (thread_cpu(pid, None), thread_cpu(pid, thread)) for name, pid, thread in
           (('sender', a.pid, 'pure-link'), ('receiver', b.pid, 'pure-listen'))}
    import http.client
    connection = http.client.HTTPConnection('127.0.0.1', port_b, timeout=30)
    first = None
    while True:  # the count every 2 ms, over one connection
        connection.request('GET', '/views/count')
        count = json.loads(connection.getresponse().read())
        now = time.monotonic()
        if count and first is None:
            first = (now, count)
        if count >= n:
            break
        time.sleep(0.002)
    connection.close()
    seconds = now - first[0]
    row = {'messages': n, 'item_bytes': size, 'first_seen': first[1], 'seconds': round(seconds, 4),
           'per_second': round((n - first[1]) / seconds) if seconds else None}
    for name, pid, thread in (('sender', a.pid, 'pure-link'), ('receiver', b.pid, 'pure-listen')):
        total, link = thread_cpu(pid, None) - cpu[name][0], thread_cpu(pid, thread) - cpu[name][1]
        row[f'{name}_cpu_us_per_message'] = round(total * 1000 / n, 2)
        row[f'{name}_link_cpu_us_per_message'] = round(link * 1000 / n, 2)
    for name, node in (('sender', a), ('receiver', b)):
        node.terminate()
        _, error = node.communicate(timeout=120)
        stats = [json.loads(line) for line in error.decode().splitlines() if line.startswith('{"active"')][-1]
        keys = ('link_packets', 'link_records', 'link_resent', 'link_lost') if name == 'sender' else \
               ('listener_received', 'listener_deliveries', 'bursts')
        row[name] = {k: stats[k] for k in keys}
    subprocess.run(['rm', '-rf', str(work / 'drain-a'), str(work / 'drain-b')], check=True)  # tmpfs: room for the next
    return row


def drain_quic(work, n, size):
    """The same over quinn: one connection, one stream, n messages; CPU of
    both sides. The service only counts them (no store)."""
    binary = Path(__file__).parent / 'quic/target/release/idle-quic'
    import resource
    server, info = start([binary, 'serve', work / 'cert.der', work / 'key.der'])
    before, children = thread_cpu(server.pid, None), resource.getrusage(resource.RUSAGE_CHILDREN)
    # The same bytes as Pure's record: its prefix (count, item's length), the item and the city ("Seoul").
    client = subprocess.run([str(binary), 'drain', str(info['port']), str(n), str(size + 8 + 5)], capture_output=True,
                            timeout=600)
    after = resource.getrusage(resource.RUSAGE_CHILDREN)
    row = json.loads(client.stdout)
    server_cpu = thread_cpu(server.pid, None) - before
    client_cpu = (after.ru_utime + after.ru_stime - children.ru_utime - children.ru_stime) * 1000
    row['client_cpu_us_per_message'] = round(client_cpu * 1000 / n, 2)
    server.kill()
    server.wait()
    row['service_cpu_us_per_message'] = round(server_cpu * 1000 / n, 2)
    return row


def transport(work, size, n):
    """The transport alone (transport.c): a backlog of n records of an order
    with a size-byte item drained through a link and a listener in one
    process, no app and no store."""
    modules = {}
    for name, text in (('sender', check.LINK_SENDER), ('receiver', check.LINK_RECEIVER)):
        source = work / f'{name}.aup'
        source.write_text(text)
        modules[name] = work / f'{name}.pmod'
        check.compile_module(Path(os.environ.get('AUP', ROOT.parent / 'aup/build/aup')), source, modules[name])
    done = subprocess.run([str(ROOT / 'build/link-transport'), str(modules['sender']), str(modules['receiver']), str(n),
                           str(size), '0'], capture_output=True, text=True, timeout=600)
    assert done.returncode == 0, done.stderr
    return json.loads(done.stdout)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--counts', default='1000,10000,100000,1000000')
    parser.add_argument('--store', type=Path, default=None, help='where the stores go (default: a temporary directory)')
    parser.add_argument('--idle-ms', type=int, default=2000)
    parser.add_argument('--window', type=int, default=256)
    parser.add_argument('--out', type=Path, default=None)
    parser.add_argument('--quic', action='store_true', help='measure the quinn service instead (quic/)')
    parser.add_argument('--restart', action='store_true', help='M2: kill -9 the receiver under load (with --quic, quinn)')
    parser.add_argument('--no-memo', action='store_true', help='M2: the node forgets the secrets it remembered')
    parser.add_argument('--period-ms', type=int, default=5000)
    parser.add_argument('--seconds', type=int, default=40)
    parser.add_argument('--kill-at', type=int, default=20)
    parser.add_argument('--first-contact', type=str, default=None,
                        help='M4: one-way delays in ms (for example 5,25,50), each in a namespace of its own')
    parser.add_argument('--drain', type=str, default=None,
                        help='M5: item sizes in bytes (for example 48,1000,16000); --counts gives the messages')
    parser.add_argument('--transport', type=str, default=None,
                        help='M5: SIZE:COUNT pairs (for example 48:200000,1000:50000): the transport alone and quinn')
    parser.add_argument('--rounds', type=int, default=1)
    args = parser.parse_args()
    counts = [int(c) for c in args.counts.split(',')]
    if args.transport:
        subprocess.run(['make', '-s', '-C', str(ROOT), 'build/libpure.a', 'build/pure-crypto.so'], check=True)
        subprocess.run(['cc', '-O2', '-g', '-std=c17', '-Wall', '-Wextra', '-Werror', f'-I{ROOT}/core', f'-I{ROOT}/serve',
                        '-DPURE_ASYNC_EFFECTS', str(Path(__file__).parent / 'transport.c'), str(ROOT / 'serve/link.c'),
                        str(ROOT / 'serve/crypto.c'), str(ROOT / 'build/libpure.a'), '-lm', '-pthread', '-ldl', '-o',
                        str(ROOT / 'build/link-transport')], check=True)
        subprocess.run(['cargo', 'build', '--release', '--offline', '--quiet'], cwd=Path(__file__).parent / 'quic', check=True)
        with tempfile.TemporaryDirectory(prefix='pure-m5-', dir=args.store) as directory:
            work = Path(directory)
            make_certificate(work)
            for pair in args.transport.split(','):
                size, n = (int(x) for x in pair.split(':'))
                for _ in range(args.rounds):
                    for name, measure in (('pure', lambda: transport(work, size, n)), ('quinn', lambda: drain_quic(work, n, size))):
                        row = {'side': name, 'item_bytes': size, **measure()}
                        line = json.dumps(row)
                        print(line, flush=True)
                        if args.out:
                            with open(args.out, 'a') as out:
                                out.write(line + '\n')
        return
    if args.drain:
        subprocess.run(['cargo', 'build', '--release', '--offline', '--quiet'], cwd=Path(__file__).parent / 'quic', check=True)
        with tempfile.TemporaryDirectory(prefix='pure-m5-', dir=args.store) as directory:
            work = Path(directory)
            make_certificate(work)
            for size in (int(s) for s in args.drain.split(',')):
                for round_, n in enumerate(counts):
                    for name, measure in (('pure', drain_pure), ('quinn', drain_quic)):
                        case = work / f'{name}-{size}-{n}-{round_}'
                        case.mkdir()
                        if name == 'quinn':
                            for f in ('cert.der', 'key.der'):
                                (case / f).write_bytes((work / f).read_bytes())
                        row = {'side': name, **measure(case, n, size)}
                        line = json.dumps(row)
                        print(line, flush=True)
                        if args.out:
                            with open(args.out, 'a') as out:
                                out.write(line + '\n')
        return
    if args.first_contact:
        subprocess.run(['cargo', 'build', '--release', '--offline', '--quiet'], cwd=Path(__file__).parent / 'quic', check=True)
        for delay in args.first_contact.split(','):
            in_namespace(int(delay), counts[0], args.out)
        return
    if args.restart:
        with tempfile.TemporaryDirectory(prefix='pure-m2-', dir=args.store) as directory:
            work = Path(directory)
            if args.quic:
                subprocess.run(['cargo', 'build', '--release', '--offline', '--quiet'], cwd=Path(__file__).parent / 'quic',
                               check=True)
                make_certificate(work)
            else:
                source = work / 'receiver.aup'
                source.write_text(check.LINK_RECEIVER)
                module = work / 'receiver.pmod'
                check.compile_module(Path(os.environ.get('AUP', ROOT.parent / 'aup/build/aup')), source, module)
                keys = work / 'keys'
                subprocess.run([str(ROOT / 'build/links'), 'keys', str(max(counts)), str(keys)], check=True)
            for n in counts:
                row = restart_quic(n, work, args.period_ms, args.seconds, args.kill_at) if args.quic else \
                      restart_pure(n, work, keys, module, args.period_ms, args.seconds, args.kill_at, not args.no_memo)
                line = json.dumps(row)
                print(line, flush=True)
                if args.out:
                    with open(args.out, 'a') as out:
                        out.write(line + '\n')
        return
    if args.quic:
        with tempfile.TemporaryDirectory(prefix='pure-m1-quic-', dir=args.store) as directory:
            quic(counts, Path(directory), args.window, args.out)
        return
    subprocess.run(['make', '-s', '-C', str(ROOT), 'build/pure-serve', 'build/pure-crypto.so', 'build/libpure.a'], check=True)
    subprocess.run(['cc', '-O2', '-g', '-std=c17', '-Wall', '-Wextra', '-Werror', f'-I{ROOT}/core',
                    str(Path(__file__).parent / 'links.c'), str(ROOT / 'build/libpure.a'), '-lcrypto', '-lm', '-pthread',
                    '-o', str(ROOT / 'build/links')], check=True)
    with tempfile.TemporaryDirectory(prefix='pure-m1-', dir=args.store) as directory:
        work = Path(directory)
        source = work / 'receiver.aup'
        source.write_text(check.LINK_RECEIVER)
        module = work / 'receiver.pmod'
        check.compile_module(Path(os.environ.get('AUP', ROOT.parent / 'aup/build/aup')), source, module)
        keys = work / 'keys'
        subprocess.run([str(ROOT / 'build/links'), 'keys', str(max(counts)), str(keys)], check=True)
        for n in counts:
            row = one(n, work, keys, module, args.idle_ms, args.window)
            line = json.dumps(row)
            print(line, flush=True)
            if args.out:
                with open(args.out, 'a') as out:
                    out.write(line + '\n')


if __name__ == '__main__':
    main()
