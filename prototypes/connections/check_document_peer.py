#!/usr/bin/env python3
"""Real TLS, document work, external client and CLI interop for the optional peer."""
import argparse
import json
from pathlib import Path
import select
import socket
import ssl
import struct
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent


def openssl(*args):
    subprocess.run(['openssl', *map(str, args)], check=True,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def certificates(root):
    ca = root / 'ca.pem'
    openssl('req', '-x509', '-newkey', 'ec', '-pkeyopt', 'ec_paramgen_curve:P-256',
            '-nodes', '-days', '1', '-subj', '/CN=Pure test CA',
            '-addext', 'basicConstraints=critical,CA:TRUE',
            '-addext', 'keyUsage=critical,keyCertSign,cRLSign',
            '-keyout', root / 'ca.key', '-out', ca)
    for role, name, eku in [('server', 'localhost', 'serverAuth'),
                            ('client', 'pure-client', 'clientAuth'),
                            ('other', 'wrong-client', 'clientAuth')]:
        ext = root / (role + '.ext')
        ext.write_text('basicConstraints=critical,CA:FALSE\n'
                       'keyUsage=critical,digitalSignature\n'
                       f'subjectAltName=DNS:{name}\nextendedKeyUsage={eku}\n')
        openssl('req', '-newkey', 'ec', '-pkeyopt', 'ec_paramgen_curve:P-256',
                '-nodes', '-subj', '/CN=' + name, '-keyout', root / (role + '.key'),
                '-out', root / (role + '.csr'))
        openssl('x509', '-req', '-in', root / (role + '.csr'), '-CA', ca,
                '-CAkey', root / 'ca.key', '-CAcreateserial', '-days', '1',
                '-extfile', ext, '-out', root / (role + '.pem'))
    return ca


def start(binary, program, store, root, ca):
    proc = subprocess.Popen([str(binary), 'serve', '127.0.0.1', '0', str(program),
                             str(store), str(ca), str(root / 'server.pem'),
                             str(root / 'server.key'), 'pure-client'],
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                            text=True, bufsize=1)
    try:
        assert select.select([proc.stdout], [], [], 5)[0], 'server did not become ready'
        line = proc.stdout.readline()
        assert line.startswith('READY '), (line, proc.poll())
        return proc, int(line.split()[1])
    except BaseException as error:
        proc.kill()
        stdout, stderr = proc.communicate(timeout=3)
        raise AssertionError(f'server start failed: {error}; stdout={stdout!r}, stderr={stderr!r}') from error


def stop(proc):
    proc.terminate()
    stdout, stderr = proc.communicate(timeout=5)
    assert proc.returncode == 0, (proc.returncode, stdout, stderr)


def connect(root, ca, port, *, cert='client', alpn='pure-doc/1', name='localhost'):
    ctx = ssl.create_default_context(cafile=str(ca))
    ctx.minimum_version = ssl.TLSVersion.TLSv1_3
    ctx.maximum_version = ssl.TLSVersion.TLSv1_3
    if cert:
        ctx.load_cert_chain(str(root / (cert + '.pem')), str(root / (cert + '.key')))
    ctx.set_alpn_protocols([alpn])
    plain = socket.create_connection(('127.0.0.1', port), timeout=5)
    try:
        secure = ctx.wrap_socket(plain, server_hostname=name)
        secure.settimeout(7)
        if secure.selected_alpn_protocol() != alpn:
            raise ssl.SSLError('ALPN mismatch')
        return secure
    except BaseException:
        plain.close()
        raise


def exact(sock, count):
    parts = []
    while count:
        part = sock.recv(count)
        if not part:
            raise EOFError('unexpected EOF')
        parts.append(part)
        count -= len(part)
    return b''.join(parts)


def hello(sock, program):
    import hashlib
    assert exact(sock, 40) == b'PDOC\x01\0\0\0' + hashlib.sha256(program.read_bytes()).digest()


def request(sock, op, key, expected=0, body=b'', *, pieces=False, want_body=True):
    key = key.encode('ascii')
    wire = struct.pack('!BBQI', op | (0x80 if want_body else 0), len(key),
                       expected, len(body)) + key + body
    if pieces:
        for at in range(0, len(wire), 4093):
            sock.sendall(wire[at:at + 4093])
    else:
        sock.sendall(wire)
    status, revision, length = struct.unpack('!BQI', exact(sock, 13))
    assert length <= 1450000, length
    return status, revision, exact(sock, length if want_body else 0), length


def edit(offset, remove, inserted):
    return struct.pack('<II', offset, remove) + inserted


def call(binary, root, ca, program, port, *command):
    return subprocess.run([str(binary), 'call', '127.0.0.1', str(port), 'localhost',
                           str(program), str(ca), str(root / 'client.pem'),
                           str(root / 'client.key'), *map(str, command)],
                          capture_output=True, timeout=15)


def rss_kib(pid, portable=False):
    if portable:
        return None
    for line in Path(f'/proc/{pid}/status').read_text().splitlines():
        if line.startswith('VmRSS:'):
            return int(line.split()[1])
    raise AssertionError('VmRSS unavailable')


def check(binary, editor, program, root, ca, *, server=None, portable=False):
    if server is None:
        server = binary
    store = root / 'objects'
    proc, port = start(server, program, store, root, ca)
    checks = []
    try:
        baseline = rss_kib(proc.pid, portable)
        for bad in ({'cert': None}, {'cert': 'other'}, {'alpn': 'pure/1'}, {'name': 'wrong.local'}):
            try:
                with connect(root, ca, port, **bad) as sock:
                    hello(sock, program)
            except (ssl.SSLError, EOFError, ConnectionResetError, BrokenPipeError):
                pass
            else:
                raise AssertionError(f'unauthorized connection succeeded: {bad}')
        checks.append('mutual certificate identity, server hostname and ALPN separation')

        changed = root / 'different.pure'
        changed.write_bytes(program.read_bytes() + b'\n')
        with connect(root, ca, port) as sock:
            try:
                hello(sock, changed)
            except AssertionError:
                pass
            else:
                raise AssertionError('different program source was accepted by C client contract')
        bad_client = call(binary, root, ca, changed, port, 'show', 'note')
        assert bad_client.returncode == 2 and b'definition mismatch' in bad_client.stderr.lower()
        checks.append('program source identity checked before C client sends work')

        with connect(root, ca, port) as sock:
            hello(sock, program)
            assert request(sock, 1, 'note') == (1, 0, b'', 0)
            assert request(sock, 2, 'note', 0, edit(0, 0, b'hello')) == (0, 1, b'hello', 5)
            assert request(sock, 1, 'note') == (0, 1, b'hello', 5)
            assert request(sock, 2, 'note', 0, edit(5, 0, b'wrong')) == (2, 1, b'', 0)
            assert request(sock, 1, 'note') == (0, 1, b'hello', 5)
        checks.append('persistent external TLS client: create, read, conflict and recovery')

        insert = root / 'insert.bin'
        insert.write_bytes(b' world')
        result = call(binary, root, ca, program, port, 'edit', 'note', '1', '5', '0', insert)
        assert result.returncode == 0, result.stderr
        assert json.loads(result.stdout)['revision'] == '2'
        local = subprocess.run([str(editor), '--program', str(program), '--store', str(store),
                                'show', 'note'], capture_output=True, timeout=5)
        assert local.returncode == 0 and local.stdout == b'hello world', local.stderr
        checks.append('native peer edit and local CLI use the same authored document and storage')

        full = b'a' * (1024 * 1024)
        with connect(root, ca, port) as sock:
            hello(sock, program)
            assert request(sock, 2, 'note', 2, edit(0, 11, full), pieces=True,
                           want_body=False) == (0, 3, b'', len(full))
            assert request(sock, 1, 'note') == (0, 3, full, len(full))
            assert request(sock, 2, 'note', 3, edit(len(full) - 1, 1, b'Z')) == (0, 4, b'Z', 1)
            assert request(sock, 1, 'note') == (0, 4, full[:-1] + b'Z', len(full))
        checks.append('fragmented 1 MiB direct ingress, demand-selected reply and one-byte splice')
        large_file = root / 'large.bin'
        large_file.write_bytes(full)
        native = call(binary, root, ca, program, port, 'edit', 'native-large', '0', '0', '0', large_file)
        assert native.returncode == 0, native.stderr
        assert json.loads(native.stdout) == {'revision': '1', 'reply_bytes': len(full)}
        assert len(native.stdout) < 100
        with connect(root, ca, port) as sock:
            hello(sock, program)
            assert request(sock, 1, 'native-large') == (0, 1, full, len(full))
        checks.append('native C peer uploads 1 MiB and receives only the requested metadata')
        large_rss = rss_kib(proc.pid, portable)

        with connect(root, ca, port) as sock:
            hello(sock, program)
            partial = struct.pack('!BBQI', 2, 4, 4, 100) + b'note' + b'\0' * 3
            sock.sendall(partial)
        with connect(root, ca, port) as sock:
            hello(sock, program)
            assert request(sock, 1, 'note') == (0, 4, full[:-1] + b'Z', len(full))
            sock.sendall(struct.pack('!BBQI', 77, 4, 0, 0) + b'note')
        with connect(root, ca, port) as sock:
            hello(sock, program)
            sock.sendall(struct.pack('!BBQI', 0x82, 4, 4, 1450001) + b'note')
        with connect(root, ca, port) as sock:
            hello(sock, program)
            assert request(sock, 1, 'note') == (0, 4, full[:-1] + b'Z', len(full))
        checks.append('incomplete, invalid and oversized frames do not publish a change')

        with connect(root, ca, port) as sock:
            hello(sock, program)
            sock.settimeout(7)
            assert sock.recv(1) == b''
        checks.append('idle authenticated session closes after deadline')
    finally:
        stop(proc)

    proc, port = start(server, program, store, root, ca)
    try:
        with connect(root, ca, port) as sock:
            hello(sock, program)
            assert request(sock, 1, 'note') == (0, 4, full[:-1] + b'Z', len(full))
        result = call(binary, root, ca, program, port, 'show', 'note')
        assert result.returncode == 0 and result.stdout == full[:-1] + b'Z', result.stderr
    finally:
        stop(proc)
    checks.append('server restart and native C peer preserve the committed large document')
    return {'profile': 'selected-authenticated-document-peer', 'checks': checks,
            'server_idle_rss_kib': baseline, 'server_after_large_work_rss_kib': large_rss,
            'portable': portable}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--peer', type=Path, default=ROOT / 'build/pure-document-peer')
    parser.add_argument('--server', type=Path,
                        help='optional alternate server; --peer remains the native client')
    parser.add_argument('--editor', type=Path, default=ROOT / 'build/pure-edit')
    parser.add_argument('--program', type=Path, default=ROOT / 'examples/editor.pure')
    parser.add_argument('--portable', action='store_true',
                        help='run the full TLS scenarios without Linux /proc RSS measurement')
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    if args.output and args.output.exists():
        parser.error('evidence file already exists')
    with tempfile.TemporaryDirectory(prefix='pure-document-peer-') as tmp:
        root = Path(tmp)
        ca = certificates(root)
        result = check(args.peer.resolve(), args.editor.resolve(), args.program.resolve(), root, ca,
                       server=args.server.resolve() if args.server else None,
                       portable=args.portable)
    output = json.dumps(result, indent=2) + '\n'
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(output)
    print(output, end='')


if __name__ == '__main__':
    main()
