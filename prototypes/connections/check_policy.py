#!/usr/bin/env python3
"""Exercise an authored final-size guard through native source and fixed apps."""
import argparse
import http.client
import json
from pathlib import Path
import select
import struct
import subprocess
import tempfile
from check_tui import Terminal
from check_document_peer import (certificates, connect, edit as peer_edit,
                                 hello, request as peer_request, start, stop)


def run(binary, program, store, *args):
    command = [str(binary)]
    if program is not None:
        command += ['--program', str(program)]
    command += ['--store', str(store), *map(str, args)]
    return subprocess.run(command, capture_output=True, timeout=15)


def edit(binary, program, store, payload, key, revision, offset, remove, body):
    payload.write_bytes(body)
    return run(binary, program, store, 'edit', key, revision, offset, remove, payload)


def result(process, status, revision):
    assert process.returncode == status, (process.returncode, process.stderr)
    channel = process.stdout if status == 0 else process.stderr
    value = json.loads(channel)
    assert value['revision'] == str(revision), value
    if status:
        assert value['error'] == 'limit' if status == 2 else value['error'] == 'conflict'


def body(binary, program, store, expected):
    shown = run(binary, program, store, 'show', 'note')
    assert shown.returncode == 0 and shown.stdout == expected
    return int(json.loads(shown.stderr)['revision'])


class Server:
    def __init__(self, binary, program, store):
        command = [str(binary), '--store', str(store), '--port', '0']
        if program is not None:
            command += ['--program', str(program)]
        self.process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        ready, _, _ = select.select([self.process.stdout], [], [], 10)
        if not ready:
            self.process.kill()
            raise AssertionError('document server did not start')
        line = self.process.stdout.readline()
        if not line:
            raise AssertionError(self.process.communicate(timeout=5))
        self.port = json.loads(line)['port']

    def request(self, method, data=b'', expected=None):
        headers = {'Content-Length': str(len(data))}
        if expected is not None:
            headers['If-None-Match' if expected == 0 else 'If-Match'] = (
                '*' if expected == 0 else f'"{expected}"')
        connection = http.client.HTTPConnection('127.0.0.1', self.port, timeout=15)
        try:
            connection.request(method, '/objects/note', data, headers)
            response = connection.getresponse()
            return response.status, response.getheader('ETag'), response.read()
        finally:
            connection.close()

    def close(self):
        self.process.terminate()
        _, error = self.process.communicate(timeout=10)
        assert self.process.returncode == 0, error[-1000:]
        stats = json.loads(error)
        assert stats['active'] == stats['retained_payload_bytes'] == 0


def splice(offset, remove, insert):
    return struct.pack('<II', offset, remove) + insert


def scenario(binary, program, directory):
    directory.mkdir()
    store = directory / 'store'
    payload = directory / 'insert'
    result(edit(binary, program, store, payload, 'note', 0, 0, 0, b'abcdef'), 0, 1)
    result(edit(binary, program, store, payload, 'note', 1, 6, 0, b'gh'), 0, 2)
    result(edit(binary, program, store, payload, 'note', 2, 8, 0, b'i'), 2, 2)
    assert body(binary, program, store, b'abcdefgh') == 2
    result(edit(binary, program, store, payload, 'note', 1, 7, 1, b'Z'), 3, 2)
    result(edit(binary, program, store, payload, 'note', 2, 7, 1, b'Z'), 0, 3)
    assert body(binary, program, store, b'abcdefgZ') == 3
    return store, payload


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, required=True)
    parser.add_argument('--fixed', type=Path, required=True)
    parser.add_argument('--source-server', type=Path, required=True)
    parser.add_argument('--fixed-server', type=Path, required=True)
    parser.add_argument('--source-tui', type=Path, required=True)
    parser.add_argument('--fixed-tui', type=Path, required=True)
    parser.add_argument('--definition', type=Path, required=True)
    parser.add_argument('--peer', type=Path)
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    definition = args.definition.read_text()
    assert definition.count('edit.insert, 8)') == 1
    with tempfile.TemporaryDirectory(prefix='pure-authored-policy-') as temp:
        root = Path(temp)
        authored = root / 'policy.pure'
        authored.write_text(definition)
        source_store, payload = scenario(args.source, authored, root / 'source')
        fixed_store, _ = scenario(args.fixed, None, root / 'fixed')
        source_http = Server(args.source_server, authored, source_store)
        try:
            assert source_http.request('GET') == (200, '"3"', b'abcdefgZ')
            assert source_http.request('PUT', splice(8, 0, b'i'), 3)[0] == 413
            assert source_http.request('GET') == (200, '"3"', b'abcdefgZ')
        finally:
            source_http.close()
        authored.write_text(definition.replace('edit.insert, 8)', 'edit.insert, 4)'))
        result(edit(args.source, authored, source_store, payload, 'note', 3, 7, 1, b'Y'), 2, 3)
        assert body(args.source, authored, source_store, b'abcdefgZ') == 3
        source_http = Server(args.source_server, authored, source_store)
        try:
            assert source_http.request('PUT', splice(4, 4, b''), 3) == (200, '"4"', b'')
        finally:
            source_http.close()
        assert body(args.source, authored, source_store, b'abcd') == 4
        result(edit(args.source, authored, source_store, payload, 'note', 4, 4, 0, b'X'), 2, 4)
        assert body(args.source, authored, source_store, b'abcd') == 4
        source_tui = Terminal(args.source_tui, authored, source_store, 'note')
        try:
            source_tui.type(b'X\x13')
            source_tui.wait_for(b'Save failed: document limit')
            assert body(args.source, authored, source_store, b'abcd') == 4
        finally:
            source_tui.close(discard=True)
        if args.peer:
            tls = root / 'tls'
            tls.mkdir()
            ca = certificates(tls)
            process, port = start(args.peer, authored, source_store, tls, ca)
            try:
                with connect(tls, ca, port) as socket:
                    hello(socket, authored)
                    assert peer_request(socket, 2, 'note', 4, peer_edit(4, 0, b'X')) == (5, 4, b'', 0)
                    assert peer_request(socket, 1, 'note') == (0, 4, b'abcd', 4)
                    assert peer_request(socket, 2, 'note', 4, peer_edit(3, 1, b'')) == (0, 5, b'', 0)
                    assert peer_request(socket, 1, 'note') == (0, 5, b'abc', 3)
            finally:
                stop(process)
            assert body(args.source, authored, source_store, b'abc') == 5
        # The compiled definition retains its original policy after source edits.
        fixed_http = Server(args.fixed_server, None, fixed_store)
        try:
            assert fixed_http.request('PUT', splice(7, 1, b'Y'), 3) == (200, '"4"', b'Y')
        finally:
            fixed_http.close()
        assert body(args.fixed, None, fixed_store, b'abcdefgY') == 4
        fixed_tui = Terminal(args.fixed_tui, None, fixed_store, 'note')
        try:
            fixed_tui.type(b'X\x13')
            fixed_tui.wait_for(b'Save failed: document limit')
            assert body(args.fixed, None, fixed_store, b'abcdefgY') == 4
        finally:
            fixed_tui.close(discard=True)
    evidence = {'checks': [
        'authored final size is checked against the locked document version',
        'a rejected growth leaves revision and bytes unchanged',
        'stale revision reports conflict before the policy decision',
        'source CLI and HTTP share a policy and committed document',
        'source policy changes behavior after the HTTP host restarts',
        'fixed CLI and HTTP retain their compiled policy after source edits',
        'source and fixed terminal editors preserve unsaved drafts on policy denial']}
    if args.peer:
        evidence['checks'].append('authenticated peer shares the policy and current stored version')
    if args.output:
        args.output.write_text(json.dumps(evidence, indent=2) + '\n')
    print(json.dumps(evidence))


if __name__ == '__main__':
    main()
