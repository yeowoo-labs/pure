#!/usr/bin/env python3
"""Exercise snapshot reads, deadlines and shutdown during a held store write."""
import argparse
import hashlib
from contextlib import suppress
import http.client
import json
import os
from pathlib import Path
import select
import socket
import struct
import subprocess
import tempfile
import time

HERE = Path(__file__).resolve().parent


def gated_server(directory, binary):
    signaled_read, signaled_write = os.pipe()
    release_read, release_write = os.pipe()
    environment = os.environ.copy()
    environment['PURE_TEST_GATE_SIGNAL_FD'] = str(signaled_write)
    environment['PURE_TEST_GATE_RELEASE_FD'] = str(release_read)
    process = subprocess.Popen([
        str(binary), '--store', str(directory),
        '--program', str(HERE / 'examples/editor.pure'), '--port', '0',
        '--timeout-ms', '250'
    ], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
        pass_fds=(signaled_write, release_read), env=environment)
    os.close(signaled_write)
    os.close(release_read)
    assert select.select([process.stdout], [], [], 5)[0], 'server did not start'
    port = json.loads(process.stdout.readline())['port']
    return process, port, signaled_read, release_write


def begin_write(port):
    connection = socket.create_connection(('127.0.0.1', port), timeout=3)
    connection.settimeout(3)
    body = struct.pack('<II', 3, 0) + b'after'
    header = (b'PUT /objects/gated HTTP/1.1\r\nHost: localhost\r\n'
              b'If-Match: "1"\r\nContent-Length: 13\r\n\r\n')
    connection.sendall(header + body)
    connection.shutdown(socket.SHUT_WR)
    return connection


def await_gate(fd):
    assert select.select([fd], [], [], 5)[0], 'store worker did not reach gate'
    assert os.read(fd, 1) == b'\1'


def malformed_response(port):
    with socket.create_connection(('127.0.0.1', port), timeout=3) as connection:
        connection.settimeout(3)
        connection.sendall(b'BAD /objects/invalid HTTP/1.1\r\nHost: localhost\r\n\r\n')
        response = connection.recv(512)
        assert response.startswith(b'HTTP/1.1 400 '), response


def seed_document(directory, editor):
    source = directory.parent / (directory.name + '-seed')
    source.write_bytes(b'old')
    result = subprocess.run([str(editor), '--store', str(directory), 'edit',
                             'gated', '0', '0', '0', str(source)],
                            capture_output=True, timeout=5)
    assert result.returncode == 0, result.stderr
    assert json.loads(result.stdout)['revision'] == '1', result.stdout


def read_document(port, expected, revision):
    client = http.client.HTTPConnection('127.0.0.1', port, timeout=2)
    try:
        client.request('GET', '/objects/gated')
        response = client.getresponse()
        observed = (response.status, response.getheader('ETag'), response.read())
        assert observed == (200, f'"{revision}"', expected), observed
        return {'status': observed[0], 'etag': observed[1],
                'body': observed[2].decode('ascii')}
    finally:
        client.close()


def finish(process, signal_fd, release_fd):
    output, error = process.communicate(timeout=5)
    os.close(signal_fd)
    os.close(release_fd)
    assert process.returncode == 0, (process.returncode, output, error)
    stats = json.loads(error.splitlines()[-1])
    assert stats['active'] == stats['retained_payload_bytes'] == 0, stats
    return stats


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--server', type=Path,
                        default=HERE / 'build/pure-connections-async-gated')
    parser.add_argument('--editor', type=Path,
                        default=HERE / 'build/pure-edit-fixed')
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    if args.output and args.output.exists():
        parser.error('evidence file already exists')
    binary = args.server.resolve()
    editor = args.editor.resolve()
    with tempfile.TemporaryDirectory(prefix='pure-async-server-') as root:
        first = Path(root) / 'deadline'
        seed_document(first, editor)
        process, port, signal_fd, release_fd = gated_server(first, binary)
        try:
            pending = begin_write(port)
            await_gate(signal_fd)
            before_publication = read_document(port, b'old', 1)
            # The writer is still held; a read completed independently.
            assert process.poll() is None
            malformed_response(port)  # Must answer while the store worker is blocked.
            assert pending.recv(1) == b''  # Deadline releases the caller's ticket.
            assert process.poll() is None
            os.write(release_fd, b'1')
            after_publication = read_document(port, b'oldafter', 2)
            pending.close()
            process.terminate()
            deadline_stats = finish(process, signal_fd, release_fd)
            assert (deadline_stats['commits'], deadline_stats['gets'], deadline_stats['expired']) == (1, 2, 1), deadline_stats
        finally:
            if process.poll() is None:
                with suppress(OSError):
                    os.write(release_fd, b'1')
                process.kill()
                process.wait(timeout=5)
            with suppress(OSError):
                os.close(signal_fd)
            with suppress(OSError):
                os.close(release_fd)

        second = Path(root) / 'shutdown'
        seed_document(second, editor)
        process, port, signal_fd, release_fd = gated_server(second, binary)
        try:
            pending = begin_write(port)
            await_gate(signal_fd)
            process.terminate()
            time.sleep(0.1)
            assert process.poll() is None  # Shutdown waits for the in-flight effect.
            os.write(release_fd, b'1')
            pending.close()
            shutdown_stats = finish(process, signal_fd, release_fd)
            assert shutdown_stats['commits'] == 1, shutdown_stats
            restored = subprocess.run([str(editor), '--store', str(second),
                                       'show', 'gated'], capture_output=True, timeout=5)
            assert restored.returncode == 0, restored.stderr
            assert restored.stdout == b'oldafter', restored.stdout
            assert json.loads(restored.stderr)['revision'] == '2', restored.stderr
        finally:
            if process.poll() is None:
                with suppress(OSError):
                    os.write(release_fd, b'1')
                process.kill()
                process.wait(timeout=5)
            with suppress(OSError):
                os.close(signal_fd)
            with suppress(OSError):
                os.close(release_fd)
    result = {'async_http': 'snapshot-read-write-overlap-deadline-recovery-draining-shutdown',
              'before_publication': before_publication,
              'after_publication': after_publication,
              'shutdown_restore': {'revision': '2', 'body': 'oldafter'},
              'deadline_stats': deadline_stats, 'shutdown_stats': shutdown_stats,
              'test_source_sha256': hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
              'gated_test_binary_sha256': hashlib.sha256(binary.read_bytes()).hexdigest()}
    output = json.dumps(result, indent=2) + '\n'
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.open('x').write(output)
    print(output, end='')


if __name__ == '__main__':
    main()
