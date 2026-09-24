#!/usr/bin/env python3
"""Exercise local TCP, immutable byte extents, and the existing workbench CLI."""
import argparse
from contextlib import contextmanager
from concurrent.futures import ThreadPoolExecutor
import fcntl
import hashlib
import http.client
import json
import platform
from pathlib import Path
import select
import socket
import struct
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parent
WORKBENCH = ROOT.parent / 'workbench'
OBJECT_LIMIT = (2 * 6 + 32) * 32768 + 8192


class Server:
    def __init__(self, binary, storage, budget=4 * 1024 * 1024, timeout=5000):
        self.process = subprocess.Popen([str(binary), '--store', str(storage), '--port', '0',
            '--budget', str(budget), '--timeout-ms', str(timeout), '--send-buffer', '4096'],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        if not select.select([self.process.stdout], [], [], 15)[0]:
            self.process.kill()
            raise AssertionError('server readiness timed out')
        line = self.process.stdout.readline()
        if not line:
            raise AssertionError(self.process.communicate(timeout=5))
        self.ready = json.loads(line)
        self.port = self.ready['port']
        self.stats = None

    def close(self):
        if self.process.poll() is None:
            self.process.terminate()
        try:
            stdout, stderr = self.process.communicate(timeout=10)
        except subprocess.TimeoutExpired:
            self.process.kill()
            self.process.communicate()
            raise
        assert self.process.returncode == 0, (stdout, stderr)
        self.stats = json.loads(stderr)
        assert self.stats['active'] == self.stats['retained_payload_bytes'] == 0
        assert self.stats['peak_retained_payload_bytes'] <= self.stats['payload_budget']
        assert self.stats['peak_connections'] <= 8


@contextmanager
def serving(*args, **kwargs):
    server = Server(*args, **kwargs)
    try:
        yield server
    finally:
        server.close()


def rpc(server, method, key, body=b'', expected=None):
    headers = {'Content-Length': str(len(body))}
    if expected is not None:
        headers['If-None-Match' if expected == 0 else 'If-Match'] = '*' if expected == 0 else f'"{expected}"'
    connection = http.client.HTTPConnection('127.0.0.1', server.port, timeout=8)
    try:
        connection.request(method, f'/objects/{key}', body, headers)
        response = connection.getresponse()
        return response.status, dict(response.getheaders()), response.read()
    finally:
        connection.close()


def wire(server, request, chunks=None):
    with socket.create_connection(('127.0.0.1', server.port), timeout=8) as sock:
        if chunks:
            for chunk in chunks:
                sock.sendall(chunk)
        else:
            sock.sendall(request)
        response = http.client.HTTPResponse(sock)
        response.begin()
        return response.status, dict(response.getheaders()), response.read()


def header(method, key, size=0, condition=b''):
    return (f'{method} /objects/{key} HTTP/1.1\r\nHost: localhost\r\n'
            f'Content-Length: {size}\r\n'.encode() + condition + b'\r\n')


def cli(server, commands, source=None, expected_code=0):
    result = subprocess.run(['node', str(WORKBENCH / 'cli.mjs'), *([str(source)] if source else [])],
        input=commands.replace('$URL', f'http://127.0.0.1:{server.port}'), text=True,
        capture_output=True, timeout=30)
    assert result.returncode == expected_code, (result.stdout, result.stderr)
    return [json.loads(line) for line in result.stdout.splitlines()], result.stderr


def check(binary, directory):
    storage = directory / 'objects'
    checks, records = [], []
    with serving(binary, storage) as server:
        assert rpc(server, 'GET', 'absent')[0] == 404
        payload = b'\x00\xffbinary\r\n\r\n' + bytes(range(256)) * 41
        request = header('PUT', 'binary', len(payload), b'If-None-Match: *\r\n') + payload
        status, headers, _ = wire(server, request, [request[:7], request[7:51], request[51:57], request[57:]])
        assert status == 200 and headers['ETag'] == '"1"'
        assert rpc(server, 'GET', 'binary')[2] == payload
        checks.append('fragmented request and binary bytes round trip')
        assert rpc(server, 'PUT', 'binary', b'new', 0)[0] == 412
        assert rpc(server, 'PUT', 'binary', b'new', 1)[0] == 200
        assert rpc(server, 'PUT', 'binary', b'stale', 1)[0] == 412
        assert rpc(server, 'GET', 'binary')[2] == b'new'
        checks.append('conditional create and stale revision preserve committed bytes')

        with (storage / '.writer.lock').open('r+b') as lock:
            fcntl.lockf(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            assert rpc(server, 'PUT', 'binary', b'blocked', 2)[0] == 503
            fcntl.lockf(lock, fcntl.LOCK_UN)
        broken = storage / 'obj-broken'
        broken.write_bytes(b'incomplete')
        assert rpc(server, 'GET', 'broken')[0] == 500
        assert rpc(server, 'PUT', 'broken', b'failure', 0)[0] == 500
        assert broken.read_bytes() == b'incomplete'
        assert rpc(server, 'GET', 'binary')[2] == b'new'
        checks.append('writer contention and invalid extent fail without overwriting data')

        with serving(binary, storage) as second_writer:
            with ThreadPoolExecutor(max_workers=2) as pool:
                tasks = [pool.submit(rpc, target, 'PUT', 'race', value, 0)
                         for target, value in [(server, b'first'), (second_writer, b'second')]]
                statuses = [task.result()[0] for task in tasks]
            assert statuses.count(200) == 1 and all(status in [200, 412, 503] for status in statuses)
            status, headers, body = rpc(server, 'GET', 'race')
            assert status == 200 and headers['ETag'] == '"1"' and body in [b'first', b'second']
        records.append(second_writer.stats)
        checks.append('two native writers cannot both create the same expected revision')

        sock = socket.create_connection(('127.0.0.1', server.port), timeout=8)
        sock.sendall(header('PUT', 'incomplete', 100, b'If-None-Match: *\r\n') + b'partial')
        sock.shutdown(socket.SHUT_WR)
        assert sock.recv(1) == b''
        sock.close()
        assert rpc(server, 'GET', 'incomplete')[0] == 404
        checks.append('incomplete request never commits and releases its allocation')

        # Force genuine partial socket sends. A write on another connection must
        # not alter the immutable file whose bytes are still being transmitted.
        large = bytes(range(256)) * 4096
        assert rpc(server, 'PUT', 'large', large, 0)[0] == 200
        sock = socket.create_connection(('127.0.0.1', server.port), timeout=8)
        sock.sendall(header('GET', 'large'))
        response = http.client.HTTPResponse(sock)
        response.begin()
        assert response.status == 200 and response.getheader('ETag') == '"1"'
        assert response.read(17) == large[:17]
        assert rpc(server, 'PUT', 'large', b'changed', 1)[0] == 200
        assert response.read() == large[17:]
        sock.close()
        assert rpc(server, 'GET', 'large')[2] == b'changed'
        checks.append('borrowed read remains the same snapshot during another commit')

        assert rpc(server, 'PUT', 'large', large, 2)[0] == 200
        sock = socket.create_connection(('127.0.0.1', server.port), timeout=8)
        sock.sendall(header('GET', 'large'))
        assert sock.recv(1)
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack('ii', 1, 0))
        sock.close()
        assert rpc(server, 'GET', 'binary')[2] == b'new'
        checks.append('receiver reset aborts an outstanding borrowed transfer')

        assert wire(server, header('PUT', 'too_large', OBJECT_LIMIT + 1, b'If-None-Match: *\r\n'))[0] == 413
        malformed = [
            b'PUT /objects/bad HTTP/1.1\r\nHost: x\r\nContent-Length: 1\r\nContent-Length: 1\r\nIf-None-Match: *\r\n\r\nx',
            b'PUT /objects/bad HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n',
            b'GET /objects/bad HTTP/1.1\r\nHost: x\x00hidden\r\n\r\n',
        ]
        for request in malformed:
            assert wire(server, request)[0] == 400
        assert rpc(server, 'GET', 'bad')[0] == 404
        checks.append('unsupported framing and oversized payload are rejected before effects')

        # Existing authoring path: define, manipulate, save remotely, reconstruct
        # in a fresh runtime. No fabricated project fixture or mock storage.
        source = WORKBENCH / 'examples' / 'direct.pure'
        saved = directory / 'restored.json'
        lines, _ = cli(server, 'point drag down 80 180\npoint drag move 210 210\npoint drag up 210 210\n'
            'connect $URL authored\npush 0\nquit\n', source)
        snapshots = [line for line in lines if 'states' in line]
        assert snapshots
        expected_states = snapshots[-1]['states']
        assert expected_states['x'] == 210 and expected_states['y'] == 190 and expected_states['dragging'] == 0
        lines, _ = cli(server, f'connect $URL authored\npull\nsave-project {saved}\nquit\n')
        assert [line for line in lines if 'states' in line][-1]['states'] == expected_states
        document = json.loads(saved.read_text())
        assert document['version'] == 2 and document['source'] == source.read_text()
        lines, error = cli(server, 'connect $URL authored\npush 0\nquit\n', source, 1)
        assert '412' in error
        checks.append('existing workbench manipulation restores in a fresh runtime and reports conflict')

        assert rpc(server, 'PUT', 'invalid_project', b'{"format":"pure-workbench","version":2,"source":"state x=1","states":[]}', 0)[0] == 200
        lines, error = cli(server, 'show\nconnect $URL invalid_project\npull\nshow\nquit\n', source, 1)
        states = [line['states'] for line in lines if 'states' in line]
        assert states[0] == states[-1] and error
        lines, error = cli(server, 'point drag down 90 170\nconnect $URL authored\npull\npush\nshow\nquit\n', source, 1)
        states = [line['states'] for line in lines if 'states' in line]
        assert states[0] == states[-1] and states[-1]['dragging'] == 1
        assert len(error.strip().splitlines()) == 2
        checks.append('invalid or non-idle restore preserves local state and does not adopt remote revision')

        assert rpc(server, 'PUT', 'wide_revision', rpc_bytes(saved), 0)[0] == 200
        wide = storage / 'obj-wide_revision'
        raw = wide.read_bytes()
        wide.write_bytes(raw[:8] + struct.pack('<Q', 9007199254740993) + raw[16:])
        lines, _ = cli(server, 'connect $URL wide_revision\npull\npush\nquit\n')
        revisions = [line['revision'] for line in lines if 'remote' in line and 'revision' in line]
        assert revisions == ['9007199254740993', '9007199254740994']
        checks.append('revision above JavaScript safe integer round trips without numeric coercion')

        sock = socket.create_connection(('127.0.0.1', server.port), timeout=8)
        sock.sendall(header('PUT', 'unread_ack', 4, b'If-None-Match: *\r\n') + b'done')
        try:
            deadline = time.monotonic() + 3
            while True:
                committed = storage / 'obj-unread_ack'
                if committed.exists():
                    assert committed.read_bytes()[24:] == b'done'
                    break
                assert time.monotonic() < deadline
                time.sleep(0.01)
        finally:
            sock.close()  # Client never consumes the successful response.
        assert rpc(server, 'PUT', 'unread_ack', b'again', 0)[0] == 412
        assert rpc(server, 'GET', 'unread_ack')[2] == b'done'
        checks.append('unread acknowledgment does not undo commit or permit a stale retry')

        # A disconnect after commit can hide the ACK; retries remain conditional.
        assert rpc(server, 'PUT', 'empty', b'', 0)[0] == 200
        assert rpc(server, 'GET', 'empty')[2] == b''
        checks.append('empty body has a valid lifetime and stored revision')
    records.append(server.stats)

    with serving(binary, storage) as restarted:
        assert rpc(restarted, 'GET', 'binary')[2] == b'new'
        assert rpc(restarted, 'GET', 'authored')[2] == rpc_bytes(saved)
        checks.append('committed authoring state survives process restart')
    records.append(restarted.stats)

    # Fixed small payload budget is an admission test, not a throughput workload.
    with serving(binary, storage, budget=1024 * 1024, timeout=1200) as limited:
        sock = socket.create_connection(('127.0.0.1', limited.port), timeout=8)
        sock.sendall(header('GET', 'large'))
        response = http.client.HTTPResponse(sock)
        response.begin()
        assert response.status == 200
        assert response.read(1) == large[:1]
        status, headers, _ = rpc(limited, 'GET', 'large')
        assert status == 503 and headers['X-Pure-Error'] == 'payload_budget'
        # Leave the first receiver stalled until the absolute connection deadline.
        time.sleep(1.4)
        assert rpc(limited, 'GET', 'binary')[2] == b'new'
        response.close()
        sock.close()
        checks.append('slow consumer retains its lease; budget rejects another; deadline releases it')

        idle = [socket.create_connection(('127.0.0.1', limited.port), timeout=8) for _ in range(8)]
        try:
            for s in idle:
                s.sendall(b'G')
            for s in idle:
                assert s.recv(1) == b''
        finally:
            for s in idle:
                s.close()
        assert rpc(limited, 'GET', 'binary')[0] == 200
        checks.append('connection slots stay bounded and recover after header deadlines')
    assert limited.stats['budget_rejections'] >= 1 and limited.stats['expired'] >= 9
    records.append(limited.stats)
    return {'kind': 'native-byte-path-correctness-not-performance', 'checks': checks,
            'server_runs': records, 'storage': server.ready['storage']}


def rpc_bytes(path):
    # CLI export is pretty JSON; wire export is compact JSON with the same model.
    return json.dumps(json.loads(path.read_text()), ensure_ascii=False, separators=(',', ':')).encode()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--server', type=Path, default=ROOT / 'build/pure-connections')
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    if args.output and args.output.exists():
        parser.error('evidence file already exists')
    binary = args.server.resolve()
    with tempfile.TemporaryDirectory(prefix='pure-connections-check-') as temporary:
        result = check(binary, Path(temporary))
    result['binary_sha256'] = hashlib.sha256(binary.read_bytes()).hexdigest()
    result['environment'] = {'platform': platform.platform(), 'machine': platform.machine(),
        'python': platform.python_version(), 'node': subprocess.check_output(['node', '--version'], text=True).strip()}
    result['source_sha256'] = {str(path.relative_to(ROOT.parent.parent)): hashlib.sha256(path.read_bytes()).hexdigest()
        for path in [*(ROOT / name for name in ('flow.c', 'flow.h', 'store.c', 'store.h', 'server.c', 'check.py', 'test-flow.c', 'Makefile')),
                     *(WORKBENCH / name for name in ('remote.mjs', 'cli.mjs', 'web/core.wasm', 'web/runtime.mjs',
                         'web/compiler.mjs', 'web/project.mjs', 'examples/direct.pure'))]}
    output = json.dumps(result, ensure_ascii=False, indent=2) + '\n'
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        with args.output.open('x') as destination:
            destination.write(output)
    print(output, end='')


if __name__ == '__main__':
    main()
