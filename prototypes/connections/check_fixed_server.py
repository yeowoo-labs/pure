#!/usr/bin/env python3
"""Run one document app through parser-free HTTP, source HTTP, and native CLI."""
import argparse
import hashlib
import http.client
import json
from pathlib import Path
import select
import socket
import struct
import subprocess
import tempfile
import time

HERE = Path(__file__).resolve().parent
PEAK_RSS_LIMIT_KIB = 4096


class Host:
    def __init__(self, binary, store, fixed, budget=None, timeout_ms=10000):
        args = [str(binary), '--store', str(store), '--port', '0',
                '--send-buffer', '4096', '--timeout-ms', str(timeout_ms)]
        if budget is not None:
            args += ['--budget', str(budget)]
        if not fixed:
            args += ['--program', str(HERE / 'examples/editor.pure')]
        self.process = subprocess.Popen(args, stdout=subprocess.PIPE,
                                        stderr=subprocess.PIPE)
        ready, _, _ = select.select([self.process.stdout], [], [], 10)
        if not ready:
            self.process.kill()
            raise AssertionError('HTTP host did not become ready')
        line = self.process.stdout.readline()
        if not line:
            raise AssertionError(self.process.communicate(timeout=5))
        self.port = json.loads(line)['port']

    def close(self):
        self.process.terminate()
        _, error = self.process.communicate(timeout=10)
        assert self.process.returncode == 0, error[-1000:]
        stats = json.loads(error)
        assert stats['active'] == stats['retained_payload_bytes'] == 0
        return stats


def request(host, method, key, body=b'', expected=None):
    headers = {'Content-Length': str(len(body))}
    if expected is not None:
        headers['If-None-Match' if expected == 0 else 'If-Match'] = (
            '*' if expected == 0 else f'"{expected}"')
    connection = http.client.HTTPConnection('127.0.0.1', host.port, timeout=15)
    try:
        connection.request(method, f'/objects/{key}', body, headers)
        response = connection.getresponse()
        return response.status, response.getheader('ETag'), response.read()
    finally:
        connection.close()


def splice(offset, remove, insert):
    return struct.pack('<II', offset, remove) + insert


def memory_kib(pid, field='VmRSS:'):
    for line in Path(f'/proc/{pid}/status').read_text().splitlines():
        if line.startswith(field):
            return int(line.split()[1])
    raise AssertionError(f'{field} unavailable')


def check(build):
    fixed_binary = (build / 'pure-connections-fixed').resolve()
    source_binary = (build / 'pure-connections').resolve()
    editor = (build / 'pure-edit-fixed').resolve()
    with tempfile.TemporaryDirectory(prefix='pure-fixed-http-') as directory:
        root = Path(directory)
        store = root / 'store'
        fixed = Host(fixed_binary, store, True)
        source = Host(source_binary, store, False)
        fixed_idle_rss = memory_kib(fixed.process.pid)
        source_idle_rss = memory_kib(source.process.pid)
        try:
            assert request(fixed, 'PUT', 'note', splice(0, 0, b'hello'), 0) == (
                200, '"1"', b'hello')
            assert request(source, 'GET', 'note') == (200, '"1"', b'hello')
            assert request(source, 'PUT', 'note', splice(5, 0, b' Pure'), 1) == (
                200, '"2"', b' Pure')
            assert request(fixed, 'GET', 'note') == (200, '"2"', b'hello Pure')
            stale_fixed = request(fixed, 'PUT', 'note', splice(0, 0, b'bad'), 1)
            stale_source = request(source, 'PUT', 'note', splice(0, 0, b'bad'), 1)
            assert stale_fixed == stale_source and stale_fixed[0] == 412
            bad_fixed = request(fixed, 'PUT', 'note', splice(1000, 0, b'bad'), 2)
            bad_source = request(source, 'PUT', 'note', splice(1000, 0, b'bad'), 2)
            assert bad_fixed == bad_source and bad_fixed[0] != 200
            assert request(fixed, 'GET', 'note') == (200, '"2"', b'hello Pure')

            large = b'A' * 1_048_576
            assert request(fixed, 'PUT', 'large', splice(0, 0, large), 0) == (
                200, '"1"', large)
            assert request(source, 'GET', 'large') == (200, '"1"', large)
            shown = subprocess.run([str(editor), '--store', str(store), 'show', 'large'],
                                   capture_output=True, timeout=15)
            assert shown.returncode == 0 and shown.stdout == large
            payload = root / 'one-byte'
            payload.write_bytes(b'Z')
            edited = subprocess.run([str(editor), '--store', str(store), 'edit',
                                     'large', '1', '1048575', '1', str(payload)],
                                    capture_output=True, timeout=15)
            assert edited.returncode == 0, edited.stderr
            assert request(fixed, 'GET', 'large') == (200, '"2"', large[:-1] + b'Z')
            assert request(source, 'GET', 'large') == (200, '"2"', large[:-1] + b'Z')
            fixed_after_large_rss = memory_kib(fixed.process.pid)
            source_after_large_rss = memory_kib(source.process.pid)
            fixed_peak_rss = memory_kib(fixed.process.pid, 'VmHWM:')
            source_peak_rss = memory_kib(source.process.pid, 'VmHWM:')
            assert fixed_peak_rss <= PEAK_RSS_LIMIT_KIB, (
                fixed_peak_rss, PEAK_RSS_LIMIT_KIB)
        finally:
            fixed_stats = fixed.close()
            source_stats = source.close()

        limited = Host(fixed_binary, store, True, budget=1_048_576, timeout_ms=60000)
        try:
            with socket.create_connection(('127.0.0.1', limited.port), timeout=15) as sock:
                sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4096)
                sock.sendall(b'GET /objects/large HTTP/1.1\r\nHost: localhost\r\n'
                             b'Content-Length: 0\r\n\r\n')
                slow = http.client.HTTPResponse(sock)
                slow.begin()
                assert slow.status == 200 and slow.read(1) == b'A'
                assert request(limited, 'GET', 'note')[0] == 503
                assert slow.read() == b'A' * 1_048_574 + b'Z'
                slow.close()
            for _ in range(40):
                if request(limited, 'GET', 'note') == (200, '"2"', b'hello Pure'):
                    break
                time.sleep(0.05)
            else:
                raise AssertionError('borrowed output was not released after draining')
        finally:
            limited_stats = limited.close()
        assert limited_stats['peak_retained_payload_bytes'] == 1_048_576
    symbols = subprocess.run(['nm', '-g', str(fixed_binary)], capture_output=True,
                             text=True, check=True, timeout=15).stdout
    assert 'pure_program_parse' not in symbols and 'pure_program_load' not in symbols
    return {'profile': 'build-time-prepared-http-document-host',
            'fixed_source_cli_same_document': 'pass',
            'conditional_conflict_and_invalid_splice': 'pass',
            'one_mib_roundtrip_and_native_edit': 'pass',
            'slow_consumer_budget_and_release': 'pass',
            'runtime_parser_symbols_absent': 'pass',
            'fixed_stats': fixed_stats, 'source_stats': source_stats,
            'limited_stats': limited_stats,
            'fixed_idle_rss_kib': fixed_idle_rss,
            'source_idle_rss_kib': source_idle_rss,
            'fixed_after_large_rss_kib': fixed_after_large_rss,
            'source_after_large_rss_kib': source_after_large_rss,
            'fixed_peak_rss_kib': fixed_peak_rss,
            'source_peak_rss_kib': source_peak_rss,
            'fixed_peak_rss_limit_kib': PEAK_RSS_LIMIT_KIB,
            'fixed_sha256': hashlib.sha256(fixed_binary.read_bytes()).hexdigest(),
            'source_sha256': hashlib.sha256(source_binary.read_bytes()).hexdigest()}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, default=HERE / 'build')
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    result = check(args.build)
    body = json.dumps(result, indent=2) + '\n'
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(body)
    print(body, end='')


if __name__ == '__main__':
    main()
