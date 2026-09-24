#!/usr/bin/env python3
"""Extract a binary preview elsewhere and run its real document paths."""
import argparse
import hashlib
import http.client
import json
from pathlib import Path
import select
import subprocess
import struct
import tarfile
import tempfile

HERE = Path(__file__).resolve().parent


def run(*args):
    result = subprocess.run([str(arg) for arg in args], capture_output=True, timeout=90)
    if result.returncode:
        raise AssertionError((result.args, result.returncode, result.stdout[-1000:], result.stderr[-1000:]))
    return result


def check_http(server_binary, binary, program, root):
    store = root / 'http-store'
    insert = root / 'http-insert'
    insert.write_bytes(b'hello Pure')
    program_args = [] if program is None else ['--program', program]
    run(binary, *program_args, '--store', store,
        'edit', 'note', '0', '0', '0', insert)
    server = subprocess.Popen([str(server_binary), *map(str, program_args),
                               '--store', str(store), '--port', '0'],
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    try:
        ready, _, _ = select.select([server.stdout], [], [], 5)
        assert ready, 'packaged HTTP host did not start'
        port = json.loads(server.stdout.readline())['port']
        connection = http.client.HTTPConnection('127.0.0.1', port, timeout=5)
        try:
            body = struct.pack('<II', 10, 0) + b'!'
            connection.request('PUT', '/objects/note', body,
                               {'Content-Length': str(len(body)), 'If-Match': '"1"'})
            reply = connection.getresponse()
            assert reply.status == 200 and reply.getheader('ETag') == '"2"'
            assert reply.read() == b'!'
        finally:
            connection.close()
        shown = run(binary, *program_args, '--store', store, 'show', 'note')
        assert shown.stdout == b'hello Pure!'
    finally:
        server.terminate()
        try:
            server.communicate(timeout=5)
        except subprocess.TimeoutExpired:
            server.kill()
            server.communicate(timeout=5)


def check(archive):
    archive = archive.resolve()
    digest = hashlib.sha256(archive.read_bytes()).hexdigest()
    checks = []
    with tempfile.TemporaryDirectory(prefix='pure-preview-extracted-') as directory:
        root = Path(directory)
        with tarfile.open(archive, 'r:gz') as bundle:
            members = bundle.getmembers()
            assert all(item.isfile() and item.name.startswith('pure-document/') and
                       '..' not in Path(item.name).parts for item in members)
            bundle.extractall(root, filter='data')
        package = root / 'pure-document'
        manifest = json.loads((package / 'manifest.json').read_text())
        assert manifest['format'] == 'pure-document-preview-1'
        package_budget = manifest.get('reviewed_package_budget')
        if package_budget:
            assert archive.stat().st_size <= package_budget['max_archive_bytes']
        expected = {'pure-document/' + name for name in manifest['files']}
        assert {item.name for item in members} == expected | {'pure-document/manifest.json'}
        for name, metadata in manifest['files'].items():
            content = (package / name).read_bytes()
            assert len(content) == metadata['bytes']
            assert hashlib.sha256(content).hexdigest() == metadata['sha256']
        checks.append('relocated archive has only declared files and every file hash matches')

        if manifest.get('program_mode') == 'build-time-prepared':
            assert manifest['selected_hosts'][0] == 'pure-edit-fixed'
            assert set(manifest['selected_hosts']) <= {
                'pure-edit-fixed', 'pure-connections-fixed', 'pure-edit-tui-fixed'}
            assert 'share/editor.pure' not in manifest['files']
            binary = package / 'bin/pure-edit-fixed'
            store = root / 'fixed-store'
            insert = root / 'fixed-insert'
            insert.write_bytes(b'hello Pure')
            written = run(binary, '--store', store, 'edit', 'note', '0', '0', '0', insert)
            assert json.loads(written.stdout) == {'revision': '1', 'reply_bytes': 10}
            assert (store / 'obj-note').read_bytes()[:8] == b'PUREB001'
            assert run(binary, '--store', store, 'show', 'note').stdout == b'hello Pure'
            checks.append('relocated parser-free runner creates and restores a document')
            insert.write_bytes(b'!')
            run(binary, '--store', store, 'edit', 'note', '1', '10', '0', insert)
            assert run(binary, '--store', store, 'show', 'note').stdout == b'hello Pure!'
            checks.append('relocated parser-free runner performs revision-checked edits')
            if 'pure-connections-fixed' in manifest['selected_hosts']:
                check_http(package / 'bin/pure-connections-fixed', binary, None, root)
                checks.append('relocated parser-free HTTP host and CLI share the document')
            if 'pure-edit-tui-fixed' in manifest['selected_hosts']:
                tui = json.loads(run('python3', HERE / 'check_tui.py', '--build',
                                     package / 'bin').stdout)
                assert len(tui['checks']) >= 7
                checks.append('relocated terminal editor saves UTF-8 and one MiB documents')
            return {'archive_sha256': digest, 'archive_bytes': archive.stat().st_size,
                    'selected_hosts': manifest['selected_hosts'], 'checks': checks}

        binary = package / 'bin/pure-edit'
        program = package / 'share/editor.pure'
        store = root / 'store'
        insert = root / 'insert.bin'
        insert.write_bytes(b'hello Pure')
        written = run(binary, '--program', program, '--store', store,
                      'edit', 'note', '0', '0', '0', insert)
        assert json.loads(written.stdout) == {'revision': '1', 'reply_bytes': 10}
        profile = manifest.get('storage_profile')
        expected_magic = b'PUREP001' if profile == 'PUREP001-16k-page-research' else b'PUREB001'
        assert (store / 'obj-note').read_bytes()[:8] == expected_magic
        shown = run(binary, '--program', program, '--store', store, 'show', 'note')
        assert shown.stdout == b'hello Pure'
        checks.append('extracted native CLI creates and restores the real document')
        if profile == 'PUREB001+PUREP001-hybrid-16k-research':
            big = root / 'big.bin'
            big.write_bytes(b'X' * (16 * 1024 + 1))
            run(binary, '--program', program, '--store', store,
                'edit', 'big', '0', '0', '0', big)
            assert (store / 'obj-big').read_bytes()[:8] == b'PUREP001'
            assert run(binary, '--program', program, '--store', store,
                       'show', 'big').stdout == big.read_bytes()
            checks.append('selected storage keeps small values inline and maps large values')

        changed = root / 'changed.pure'
        source = program.read_text()
        assert 'reply(saved, edit.insert);' in source
        changed.write_text(source.replace('reply(saved, edit.insert);', 'reply(saved);'))
        changed_store = root / 'changed-store'
        changed_result = run(binary, '--program', changed, '--store', changed_store,
                             'edit', 'note', '0', '0', '0', insert)
        assert json.loads(changed_result.stdout) == {'revision': '1', 'reply_bytes': 0}
        checks.append('editing the packaged .pure source changes the executed reply')

        if 'pure-connections' in manifest['selected_hosts']:
            check_http(package / 'bin/pure-connections', binary, program, root)
            checks.append('extracted HTTP host updates the same document as the native CLI')

        if 'pure-edit-tui' in manifest['selected_hosts']:
            tui = json.loads(run('python3', HERE / 'check_tui.py', '--build',
                                 package / 'bin', '--source', '--program', program).stdout)
            assert 'source CLI and TUI read the same document' in tui['checks']
            checks.append('relocated source-loaded TUI and CLI share UTF-8, large and many-document paths')

        peer = None
        assert not ({'pure-document-peer', 'pure-document-server'} <=
                    set(manifest['selected_hosts']))
        if 'pure-document-peer' in manifest['selected_hosts']:
            peer = json.loads(run('python3', HERE / 'check_document_peer.py', '--peer',
                                  package / 'bin/pure-document-peer', '--editor', binary,
                                  '--program', program).stdout)
            assert len(peer['checks']) >= 9
            checks.append('extracted C/Python TLS peers share a 1 MiB document and recover')
        if 'pure-document-server' in manifest['selected_hosts']:
            peer = json.loads(run('python3', HERE / 'check_document_server_package.py',
                                  '--server', package / 'bin/pure-document-server',
                                  '--editor', binary, '--program', program).stdout)
            assert len(peer['checks']) >= 5
            checks.append('extracted server-only TLS peer and external client share a 1 MiB document and recover')
        if 'pure-edit-window' in manifest['selected_hosts']:
            key_switch = 'native-document-key-switch' in manifest.get('features', [])
            window = json.loads(run('xvfb-run', '-a', '-s', '-screen 0 1280x800x24',
                                    'python3', HERE / 'check_window.py', '--build',
                                    package / 'bin', '--program', program,
                                    *([] if key_switch else ['--legacy-window'])).stdout)
            assert window['one_mib_document_one_byte_edit'] == 'pass'
            if key_switch:
                assert window['multi_document_switch_preserves_draft'] == 'pass'
                checks.append('extracted GTK window switches documents without losing drafts')
            else:
                checks.append('extracted archived GTK window edits the same document')
        result = {'archive_sha256': digest, 'archive_bytes': archive.stat().st_size,
                  'selected_hosts': manifest['selected_hosts'], 'checks': checks}
        if peer is not None:
            result['peer_checks'] = peer['checks']
            result['peer_idle_rss_kib'] = peer['server_idle_rss_kib']
            result['peer_after_large_work_rss_kib'] = peer['server_after_large_work_rss_kib']
        return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('archive', type=Path)
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    if args.output and args.output.exists():
        parser.error('evidence file already exists')
    result = check(args.archive)
    output = json.dumps(result, indent=2) + '\n'
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(output)
    print(output, end='')


if __name__ == '__main__':
    main()
