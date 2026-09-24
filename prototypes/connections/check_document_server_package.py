#!/usr/bin/env python3
"""Exercise an extracted server-only TLS artifact with an external client."""
import argparse
import json
from pathlib import Path
import ssl
import struct
import subprocess
import tempfile

import check_document_peer as peer


def local_show(editor, program, store, key):
    result = subprocess.run([str(editor), '--program', str(program), '--store',
                             str(store), 'show', key], capture_output=True, timeout=15)
    assert result.returncode == 0, (result.returncode, result.stderr)
    return result.stdout


def check(server_binary, editor, program):
    with tempfile.TemporaryDirectory(prefix='pure-packaged-document-server-') as name:
        root = Path(name)
        ca = peer.certificates(root)
        store = root / 'objects'
        server, port = peer.start(server_binary, program, store, root, ca)
        checks = []
        try:
            idle_rss = peer.rss_kib(server.pid)
            for bad in ({'cert': None}, {'cert': 'other'}, {'alpn': 'pure/1'},
                        {'name': 'wrong.local'}):
                try:
                    with peer.connect(root, ca, port, **bad) as sock:
                        peer.hello(sock, program)
                except (ssl.SSLError, EOFError, ConnectionResetError, BrokenPipeError):
                    pass
                else:
                    raise AssertionError(f'unauthorized session succeeded: {bad}')
            checks.append('relocated server rejects invalid client identity, ALPN and hostname')

            with peer.connect(root, ca, port) as sock:
                peer.hello(sock, program)
                assert peer.request(sock, 1, 'note') == (1, 0, b'', 0)
                assert peer.request(sock, 2, 'note', 0, peer.edit(0, 0, b'hello'),
                                    want_body=False) == (0, 1, b'', 5)
                assert peer.request(sock, 1, 'note') == (0, 1, b'hello', 5)
                assert peer.request(sock, 2, 'note', 0, peer.edit(5, 0, b'wrong')) == (2, 1, b'', 0)
            assert local_show(editor, program, store, 'note') == b'hello'
            checks.append('external client and extracted CLI share a conditional document')

            large = b'a' * (1024 * 1024)
            with peer.connect(root, ca, port) as sock:
                peer.hello(sock, program)
                assert peer.request(sock, 2, 'note', 1, peer.edit(0, 5, large),
                                    pieces=True, want_body=False) == (0, 2, b'', len(large))
                assert peer.request(sock, 1, 'note') == (0, 2, large, len(large))
                assert peer.request(sock, 2, 'note', 2,
                                    peer.edit(len(large) - 1, 1, b'Z')) == (0, 3, b'Z', 1)
            expected = large[:-1] + b'Z'
            assert local_show(editor, program, store, 'note') == expected
            large_rss = peer.rss_kib(server.pid)
            checks.append('fragmented one MiB ingress, selected reply and CLI-visible splice')

            with peer.connect(root, ca, port) as sock:
                peer.hello(sock, program)
                sock.sendall(struct.pack('!BBQI', 2, 4, 3, 100) + b'note' + b'abc')
            with peer.connect(root, ca, port) as sock:
                peer.hello(sock, program)
                assert peer.request(sock, 1, 'note') == (0, 3, expected, len(expected))
                sock.sendall(struct.pack('!BBQI', 77, 4, 0, 0) + b'note')
            assert local_show(editor, program, store, 'note') == expected
            checks.append('truncated and invalid frames do not publish a change')
        finally:
            peer.stop(server)

        server, port = peer.start(server_binary, program, store, root, ca)
        try:
            with peer.connect(root, ca, port) as sock:
                peer.hello(sock, program)
                assert peer.request(sock, 1, 'note') == (0, 3, expected, len(expected))
        finally:
            peer.stop(server)
        checks.append('relocated server restart restores the committed one MiB document')
        return {'profile': 'relocated-server-only-document-peer', 'checks': checks,
                'server_idle_rss_kib': idle_rss,
                'server_after_large_work_rss_kib': large_rss}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--server', type=Path, required=True)
    parser.add_argument('--editor', type=Path, required=True)
    parser.add_argument('--program', type=Path, required=True)
    args = parser.parse_args()
    print(json.dumps(check(args.server.resolve(), args.editor.resolve(),
                           args.program.resolve()), indent=2))


if __name__ == '__main__':
    main()
