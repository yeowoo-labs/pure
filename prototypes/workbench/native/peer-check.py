#!/usr/bin/env python3
"""Loopback integration of the optional TLS peer host and an ordinary Python client."""
import argparse
import json
from pathlib import Path
import select
import socket
import ssl
import struct
import subprocess
import tempfile


def run(*args):
    subprocess.run(args, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def certificates(root):
    ca = root / 'ca.pem'
    run('openssl', 'req', '-x509', '-newkey', 'ec', '-pkeyopt', 'ec_paramgen_curve:P-256',
        '-nodes', '-days', '1', '-subj', '/CN=Pure test CA',
        '-addext', 'basicConstraints=critical,CA:TRUE',
        '-addext', 'keyUsage=critical,keyCertSign,cRLSign',
        '-keyout', root / 'ca.key', '-out', ca)
    for role, san, eku in [('server', 'localhost', 'serverAuth'),
                           ('client', 'pure-client', 'clientAuth'),
                           ('other', 'wrong-client', 'clientAuth')]:
        config = root / (role + '.ext')
        config.write_text(f'basicConstraints=critical,CA:FALSE\n'
                          f'keyUsage=critical,digitalSignature\n'
                          f'subjectAltName=DNS:{san}\nextendedKeyUsage={eku}\n')
        run('openssl', 'req', '-newkey', 'ec', '-pkeyopt', 'ec_paramgen_curve:P-256',
            '-nodes', '-subj', '/CN=' + san, '-keyout', root / (role + '.key'),
            '-out', root / (role + '.csr'))
        run('openssl', 'x509', '-req', '-in', root / (role + '.csr'), '-CA', ca,
            '-CAkey', root / 'ca.key', '-CAcreateserial', '-days', '1',
            '-extfile', config, '-out', root / (role + '.pem'))
    return ca


def frame(payload):
    return struct.pack('!H', len(payload)) + payload


def recv_exact(sock, size):
    result = bytearray()
    while len(result) < size:
        chunk = sock.recv(size - len(result))
        if not chunk:
            raise EOFError('unexpected end of TLS stream')
        result += chunk
    return bytes(result)


def receive(sock):
    size, = struct.unpack('!H', recv_exact(sock, 2))
    return recv_exact(sock, size)


def peer(root, ca, port, *, cert='client', alpn='pure/1', server_name='localhost'):
    ctx = ssl.create_default_context(cafile=str(ca))
    ctx.minimum_version = ssl.TLSVersion.TLSv1_3
    ctx.maximum_version = ssl.TLSVersion.TLSv1_3
    if cert:
        ctx.load_cert_chain(str(root / (cert + '.pem')), str(root / (cert + '.key')))
    ctx.set_alpn_protocols([alpn])
    plain = socket.create_connection(('127.0.0.1', port), timeout=3)
    try:
        secure = ctx.wrap_socket(plain, server_hostname=server_name)
        secure.settimeout(3)
        return secure
    except BaseException:
        plain.close()
        raise


def start(binary, root, ca, store):
    command = [str(binary), 'serve', '127.0.0.1', '0', str(store), 'scene',
               str(ca), str(root / 'server.pem'), str(root / 'server.key'), 'pure-client']
    proc = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                            text=True, bufsize=1)
    try:
        assert select.select([proc.stdout], [], [], 5)[0], 'server did not become ready'
        line = proc.stdout.readline()
        assert line.startswith('READY '), (line, proc.stderr.read())
        return proc, int(line.split()[1])
    except BaseException:
        proc.kill()
        proc.wait(timeout=3)
        raise


def stop(proc):
    proc.terminate()
    proc.communicate(timeout=5)
    assert proc.returncode == 0, proc.returncode


def state(sock):
    sock.sendall(frame(b'\x00'))
    reply = receive(sock)
    assert len(reply) >= 26 and reply[0] == 0
    revision, stored = struct.unpack_from('!QQ', reply, 1)
    assert len(reply) == 26 + 8 * reply[25]
    values = struct.unpack_from('!' + 'd' * reply[25], reply, 26)
    return revision, stored, values, struct.unpack_from('!I', reply, 21)[0]


def rss_kib(pid):
    status = Path(f'/proc/{pid}/status')
    if not status.exists():
        return None
    for line in status.read_text().splitlines():
        if line.startswith('VmRSS:'):
            return int(line.split()[1])
    return None


def check(binary):
    with tempfile.TemporaryDirectory(prefix='pure-peer-check-') as tmp:
        root = Path(tmp)
        ca = certificates(root)
        server, port = start(binary, root, ca, root / 'store')
        idle_rss = rss_kib(server.pid)
        connected_rss = None
        try:
            # The peer is an ordinary Python TLS client: no Pure client library.
            with peer(root, ca, port) as sock:
                hello = receive(sock)
                assert len(hello) == 53 and hello[:4] == b'PUR1'
                revision, _, _, held = state(sock)
                connected_rss = rss_kib(server.pid)
                assert held == 0
                stale = struct.pack('!BQ', 2, revision + 1)
                sock.sendall(frame(stale))
                conflict = receive(sock)
                assert conflict[0] == 2 and struct.unpack_from('!Q', conflict, 1)[0] == revision
                assert state(sock)[0] == revision

                # Routed target zero; ending the session while held must cancel it.
                down = struct.pack('!BQBBBdd', 1, revision, 4, 0, 0, 90., 130.)
                sock.sendall(frame(down))
                assert receive(sock)[0] == 0
            with peer(root, ca, port) as sock:
                receive(sock)
                revision, _, values, held = state(sock)
                assert held == 0, (revision, values, held)

            # A malformed request and overlong frame close their sessions without mutation.
            for bad in [frame(b'\xff'), b'\x01\x00']:
                with peer(root, ca, port) as sock:
                    receive(sock)
                    sock.sendall(bad)
                    try:
                        assert sock.recv(2) == b''
                    except (ssl.SSLError, ConnectionResetError):
                        pass
            with peer(root, ca, port) as sock:
                receive(sock)
                assert state(sock)[0] == revision

            # An authenticated client that sends nothing cannot retain the app forever.
            with peer(root, ca, port) as sock:
                receive(sock)
                sock.settimeout(7)
                try:
                    assert sock.recv(1) == b''
                except ssl.SSLError:
                    pass
            with peer(root, ca, port) as sock:
                receive(sock)
                assert state(sock)[0] == revision

            # Authentication is mandatory on both sides; ALPN is mandatory too.
            for kwargs in [{'cert': None}, {'cert': 'other'}, {'alpn': 'http/1.1'}]:
                try:
                    with peer(root, ca, port, **kwargs) as sock:
                        receive(sock)
                except (ssl.SSLError, EOFError, ConnectionResetError, TimeoutError):
                    pass
                else:
                    raise AssertionError('untrusted or unsupported peer accepted: ' + str(kwargs))
            try:
                peer(root, ca, port, server_name='wrong-server').close()
            except ssl.SSLError:
                pass
            else:
                raise AssertionError('wrong server name accepted')

            commands = 'point drag down green_box 90 130\npoint drag move green_box 230 210\npoint drag up green_box 230 210\nsave\nquit\n'
            result = subprocess.run([str(binary), 'call', '127.0.0.1', str(port), 'localhost',
                                     str(ca), str(root / 'client.pem'), str(root / 'client.key')],
                                    input=commands, text=True, capture_output=True, timeout=10)
            assert result.returncode == 0, (result.stdout, result.stderr)
            rows = [json.loads(line) for line in result.stdout.splitlines()]
            assert rows[-1]['status'] == 0 and rows[-1]['stored_revision'] == '1'
            assert rows[-1]['states']['green.x'] == 220 and rows[-1]['states']['green.y'] == 200
            assert rows[-1]['held'] == 0
        finally:
            stop(server)

        server, port = start(binary, root, ca, root / 'store')
        try:
            with peer(root, ca, port) as sock:
                receive(sock)
                _, stored, _, held = state(sock)
                assert stored == 1 and held == 0
            result = subprocess.run([str(binary), 'call', '127.0.0.1', str(port), 'localhost',
                                     str(ca), str(root / 'client.pem'), str(root / 'client.key')],
                                    input='quit\n', text=True, capture_output=True, timeout=10)
            assert result.returncode == 0, (result.stdout, result.stderr)
            restored = json.loads(result.stdout.splitlines()[0])
            assert restored['states']['green.x'] == 220 and restored['states']['green.y'] == 200
        finally:
            stop(server)
        return {'python_client': 'pass', 'revision_conflict': 'pass',
                'disconnect_release': 'pass', 'malformed_frames': 'pass',
                'idle_deadline': 'pass',
                'mutual_authentication': 'pass', 'alpn': 'pass',
                'server_hostname': 'pass', 'persist_restart': 'pass',
                'server_idle_rss_kib': idle_rss, 'server_connected_rss_kib': connected_rss}


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('binary', type=Path)
    args = parser.parse_args()
    print(json.dumps(check(args.binary.resolve()), sort_keys=True))
