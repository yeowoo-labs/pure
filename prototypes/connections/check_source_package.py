#!/usr/bin/env python3
"""Verify a source snapshot after safe extraction and a clean native build."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import tarfile
import tempfile

PREFIX = 'pure-source-preview/'


def run(*args, timeout=120):
    result = subprocess.run([str(arg) for arg in args], capture_output=True, timeout=timeout)
    if result.returncode:
        raise AssertionError((result.args, result.returncode,
                              result.stdout[-2000:], result.stderr[-2000:]))
    return result


def check(archive, release, peer, copy_range, flat_export, mirror, hybrid_tui):
    archive = archive.resolve()
    checks = []
    with tempfile.TemporaryDirectory(prefix='pure-source-extracted-') as directory:
        root = Path(directory)
        package = root / PREFIX[:-1]
        with tarfile.open(archive, 'r:gz') as bundle:
            members = bundle.getmembers()
            names = [member.name for member in members]
            assert len(names) == len(set(names))
            assert all(member.isfile() and member.name.startswith(PREFIX) and
                       '..' not in Path(member.name).parts for member in members)
            for member in members:
                destination = root / member.name
                destination.parent.mkdir(parents=True, exist_ok=True)
                destination.write_bytes(bundle.extractfile(member).read())
        manifest = json.loads((package / 'manifest.json').read_text())
        assert manifest['format'] == 'pure-source-preview-1'
        assert set(names) == {PREFIX + name for name in manifest['files']} | {PREFIX + 'manifest.json'}
        for name, metadata in manifest['files'].items():
            content = (package / name).read_bytes()
            assert len(content) == metadata['bytes']
            assert hashlib.sha256(content).hexdigest() == metadata['sha256']
        checks.append('every extracted source file matches the manifest')

        connections = package / 'prototypes/connections'
        run('make', '-C', connections, timeout=120)
        default_outputs = {path.name for path in (connections / 'build').iterdir()}
        assert default_outputs == {'runtime.o', 'libpure-execution.a',
                                   'pure-compile', 'embedded-plan.h',
                                   'pure-edit-fixed'}, default_outputs
        checks.append('clean default build contains only the core, parser-free CLI and its build-time compiler')
        run('make', '-C', connections, 'check-native-checkpoint', timeout=300)
        checks.append('clean C17, both real terminal PTY scenarios and fixed/source CLI document RSS checks pass')
        run('make', '-C', connections, 'fixed-editor')
        editor = connections / 'build/pure-edit'
        fixed = connections / 'build/pure-edit-fixed'
        program = connections / 'examples/editor.pure'
        store = root / 'document-store'
        insert = root / 'insert.bin'
        insert.write_bytes(b'hello Pure')
        written = run(editor, '--program', program, '--store', store,
                      'edit', 'note', '0', '0', '0', insert)
        assert json.loads(written.stdout) == {'revision': '1', 'reply_bytes': 10}
        assert run(fixed, '--store', store, 'show', 'note').stdout == b'hello Pure'
        checks.append('source-loaded CLI and parser-free CLI share the real stored document')

        changed = root / 'changed.pure'
        source = program.read_text()
        assert 'reply(saved, edit.insert);' in source
        changed.write_text(source.replace('reply(saved, edit.insert);', 'reply(saved);'))
        changed_store = root / 'changed-store'
        changed_result = run(editor, '--program', changed, '--store', changed_store,
                             'edit', 'note', '0', '0', '0', insert)
        assert json.loads(changed_result.stdout) == {'revision': '1', 'reply_bytes': 0}
        checks.append('editing .pure changes the native source-loaded reply')

        if peer:
            run('make', '-C', connections, 'check-portable-peer', timeout=180)
            checks.append('clean native TLS client and both server profiles pass authenticated document work')

        if copy_range:
            run('make', '-C', connections, 'check-copy-range', timeout=240)
            checks.append('clean selected Linux copy path passes app, scale, size, RSS and traced one-MiB splice')

        if flat_export:
            run('make', '-C', connections, 'check-flat-export', timeout=180)
            checks.append('clean selected page store exports mixed revisions to the default flat store')

        if mirror:
            run('make', '-C', connections, 'check-mirror', timeout=180)
            checks.append('clean one-MiB document fans out to two same-process Pure domains with independent conflict and release')

        if hybrid_tui:
            run('make', '-C', connections, 'BUILD=build/hybrid', 'STORE=store-paged.c',
                'editor', 'source-tui', timeout=120)
            run('python3', connections / 'check_tui.py', '--build',
                connections / 'build/hybrid', '--source', timeout=120)
            selected = root / 'hybrid-tui.tar.gz'
            run('python3', connections / 'package_native.py', '--build',
                connections / 'build/hybrid', '--hybrid-storage', '--with-tui',
                '--output', selected, timeout=120)
            run('python3', connections / 'check_package.py', selected, timeout=120)
            checks.append('clean selected page store runs the source-loaded TUI and relocatable CLI+TUI package')

        if release:
            run('make', '-C', connections, 'check-release', timeout=180)
            checks.append('clean Linux release behavior, code, dependency and RSS gates pass')
            run('make', '-C', connections, 'check-source-tui', timeout=120)
            checks.append('clean source-loaded terminal editor PTY scenarios pass')
    return {'archive_sha256': hashlib.sha256(archive.read_bytes()).hexdigest(),
            'archive_bytes': archive.stat().st_size, 'source_files': len(manifest['files']),
            'checks': checks}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('archive', type=Path)
    parser.add_argument('--release', action='store_true', help='also run Linux release gates')
    parser.add_argument('--peer', action='store_true',
                        help='also build and exercise the optional TLS client and servers')
    parser.add_argument('--copy-range', action='store_true',
                        help='also check the opt-in Linux file copy path with strace')
    parser.add_argument('--flat-export', action='store_true',
                        help='also check selected paged-to-default document store export')
    parser.add_argument('--mirror', action='store_true',
                        help='also check one-process document fan-out through two Pure domains')
    parser.add_argument('--hybrid-tui', action='store_true',
                        help='also build and relocate the selected 16 KiB-page native terminal app')
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    if args.output and args.output.exists():
        parser.error('evidence file already exists')
    result = check(args.archive, args.release, args.peer, args.copy_range,
                   args.flat_export, args.mirror, args.hybrid_tui)
    text = json.dumps(result, indent=2) + '\n'
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(text)
    print(text, end='')


if __name__ == '__main__':
    main()
