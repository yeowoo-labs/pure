"""Real native automation and HTTP clients editing the same authored document."""
from concurrent.futures import ThreadPoolExecutor
import json
import socket
import struct
import subprocess


def check_edits(editor, binary, root, directory, serving, rpc, header, records, checks):
    storage = directory / 'edited-objects'
    program = root / 'examples/editor.pure'
    file = directory / 'insertion.bin'

    def local(*args, body=None, code=0, source=program, budget=None):
        if body is not None:
            file.write_bytes(body)
            args = (*args, str(file))
        command = [str(editor), '--program', str(source), '--store', str(storage)]
        if budget is not None:
            command += ['--budget', str(budget)]
        result = subprocess.run(command + [str(arg) for arg in args], capture_output=True, timeout=20)
        assert result.returncode == code, (command, args, result.stdout, result.stderr)
        return result

    initial = 'Pure에서 안녕 🌏\n'.encode()
    created = local('edit', 'note', 0, 0, 0, body=initial)
    assert json.loads(created.stdout)['revision'] == '1'
    start = initial.index('안녕'.encode())
    inserted = '반가워'.encode()
    expected = initial[:start] + inserted + initial[start + len('안녕'.encode()):]
    local('edit', 'note', 1, start, len('안녕'.encode()), body=inserted)
    shown = local('show', 'note')
    assert shown.stdout == expected and json.loads(shown.stderr)['revision'] == '2'
    checks.append('native authored byte edit and fresh native process restore UTF-8 content')
    stale = local('edit', 'note', 1, 0, 0, body=b'bad', code=3)
    assert json.loads(stale.stderr) == {'error': 'conflict', 'revision': '2'}
    local('edit', 'note', 2, 0xffffffff, 0, body=b'bad', code=2)
    assert local('show', 'note').stdout == expected
    checks.append('native stale revision and invalid byte position preserve the committed document')

    reordered = directory / 'edit-reordered.pure'
    reordered.write_text('layout E { tag: bytes[1]; remove: u32le; start: u32le; body: bytes; } '
        'on get { let o = store.read(request.key); reply(o, o.body); } '
        'on put { let e = view(E, request.body); '
        'let s = store.splice(request.key, request.revision, e.start, e.remove, e.body); reply(s, e.body); }')
    local('edit', 'note', 2, len(expected), 0, body=b'!', source=reordered)
    expected += b'!'
    assert local('show', 'note', source=reordered).stdout == expected
    checks.append('native convenience editor derives unaligned reordered field positions from the authored program')

    custom = directory / 'edit-custom.pure'
    custom.write_text('on get { let o = store.read(request.key); reply(o, o.body); } '
        'on put { let s = store.splice(request.key, request.revision, 0, 0, request.body); reply(s); }')
    local('edit', 'custom', 0, 0, 0, body=b'ignored', source=custom, code=2)
    applied = local('apply', 'custom', 0, body=b'custom input', source=custom)
    assert json.loads(applied.stdout) == {'revision': '1', 'reply_bytes': 0}
    assert local('show', 'custom', source=custom).stdout == b'custom input'
    checks.append('raw native apply runs another authored input contract without changing the executor')

    with serving(binary, storage, program=program) as server:
        assert rpc(server, 'GET', 'note')[2] == expected
        status, headers, body = rpc(server, 'PUT', 'note', struct.pack('<II', len(expected), 0) + b'?', 3)
        assert status == 200 and headers['ETag'] == '"4"' and body == b'?'
        expected += b'?'
        assert local('show', 'note').stdout == expected
        local('edit', 'note', 4, 0, 4, body=b'PURE')
        expected = b'PURE' + expected[4:]
        assert rpc(server, 'PUT', 'note', struct.pack('<II', 0, 0) + b'stale', 4)[0] == 412
        assert rpc(server, 'GET', 'note')[2] == expected
        checks.append('native CLI and external HTTP share revisions and the same authored storage effect in both directions')
        with socket.create_connection(('127.0.0.1', server.port), timeout=8) as sock:
            sock.sendall(header('PUT', 'note', 12, b'If-Match: "5"\r\n') + b'partial')
            sock.shutdown(socket.SHUT_WR)
            assert sock.recv(1) == b''
        assert rpc(server, 'PUT', 'note', b'short', 5)[0] == 413
        assert rpc(server, 'PUT', 'note', struct.pack('<II', 0xffffffff, 0), 5)[0] == 413
        assert rpc(server, 'GET', 'note')[2] == expected
        checks.append('partial or invalid edit input never modifies the stored document')
        with serving(binary, storage, program=program) as other:
            with ThreadPoolExecutor(max_workers=2) as pool:
                tasks = [pool.submit(rpc, target, 'PUT', 'note', struct.pack('<II', 0, 0) + value, 5)
                         for target, value in ((server, b'A'), (other, b'B'))]
                statuses = [task.result()[0] for task in tasks]
            assert statuses.count(200) == 1 and all(status in (200, 412, 503) for status in statuses)
            status, headers, body = rpc(server, 'GET', 'note')
            assert status == 200 and headers['ETag'] == '"6"'
            assert body == (b'A' if statuses[0] == 200 else b'B') + expected
        records.append(other.stats)
        checks.append('two processes cannot both splice the same expected document revision')
    records.append(server.stats)

    # This tests retained input size for a real large edit, not throughput.
    large = bytes(range(256)) * 4096
    local('edit', 'large', 0, 0, 0, body=large)
    local('edit', 'large', 1, 524287, 3, body=b'XYZ', budget=11)
    expected_large = large[:524287] + b'XYZ' + large[524290:]
    assert local('show', 'large').stdout == expected_large
    with serving(binary, storage, program=program, budget=11) as bounded:
        status, headers, body = rpc(bounded, 'PUT', 'large', struct.pack('<II', 524287, 3) + b'abc', 2)
        assert status == 200 and headers['ETag'] == '"3"' and body == b'abc'
        assert rpc(bounded, 'PUT', 'large', struct.pack('<II', 0, 0) + b'abcd', 3)[0] == 503
    assert bounded.stats['peak_retained_payload_bytes'] == 11
    records.append(bounded.stats)
    assert local('show', 'large').stdout == large[:524287] + b'abc' + large[524290:]
    checks.append('1 MiB document changes with 11 input credits; larger input is rejected before an effect')
    return {'large_document_bytes': len(large), 'small_edit_input_bytes': 11,
            'storage_rewrites_full_extent': True, 'storage_scratch_bytes': 4096,
            'kind': 'correctness-and-admission-not-total-memory-or-performance'}
