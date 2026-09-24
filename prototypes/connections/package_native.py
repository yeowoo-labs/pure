#!/usr/bin/env python3
"""Bundle the selected Linux document app without folding hosts into the core."""
import argparse
import gzip
import hashlib
import io
import json
from pathlib import Path
import platform
import re
import subprocess
import tarfile
import tempfile

HERE = Path(__file__).resolve().parent
NEEDED = re.compile(r'\(NEEDED\).*Shared library: \[([^]]+)\]')
ROOT_NAME = 'pure-document'
PAGED_STORAGE = """# Selected page storage format

This preview uses the PUREP001 16 KiB-page store. It cannot open PUREB001
flat-store objects. Use a new --store directory; this is a research format,
not a migration or a default storage backend. Small objects currently pay a
full-page write cost. See docs/research/033-paged-document-storage.md in the
source repository for measured boundaries and limitations.
"""
HYBRID_STORAGE = """# Selected inline/page storage experiment

Objects up to 16 KiB use the existing PUREB001 file format. Larger objects
use PUREP001 page roots and append-only data generations. The current flat
binary cannot read PUREP001 objects, so use a new --store directory for this
preview. This is a selected research build, not the default backend or a
bidirectional migration path. It requires a Linux host with 16 KiB OS pages.
See docs/research/034-hybrid-document-storage.md in the source repository.
"""

RUN = """# Pure document preview — Linux native build

This archive includes the CLI and editable editor.pure program. HTTP, TLS peer,
and GTK hosts are included only when listed in manifest.json. Shared system libraries are not bundled;
manifest.json records every binary's direct dependencies.

From this directory, try a local edit:

    printf 'hello Pure\\n' > /tmp/pure-insert.txt
    bin/pure-edit --program share/editor.pure --store /tmp/pure-preview-store \\
      edit note 0 0 0 /tmp/pure-insert.txt
    bin/pure-edit --program share/editor.pure --store /tmp/pure-preview-store show note

When selected, the same program and store can be opened with the GTK window:

    bin/pure-edit-window --program share/editor.pure \\
      --store /tmp/pure-preview-store note

When selected, the source-loaded terminal editor uses the same program and
store without GTK. Edit share/editor.pure, then start a new TUI session to
load the changed definition:

    bin/pure-edit-tui --program share/editor.pure \\
      --store /tmp/pure-preview-store note

When selected, the TLS peer needs your own CA and server/client certificates. The server
certificate needs a DNS SAN matching SERVER_DNS; the client certificate needs
a DNS SAN matching CLIENT_DNS. This example binds only to loopback:

    bin/pure-document-peer serve 127.0.0.1 9443 share/editor.pure \\
      /tmp/pure-preview-store CA.pem SERVER.pem SERVER.key CLIENT_DNS
    bin/pure-document-peer call 127.0.0.1 9443 SERVER_DNS share/editor.pure \\
      CA.pem CLIENT.pem CLIENT.key show note

If manifest.json lists pure-document-server instead, run:

    bin/pure-document-server serve 127.0.0.1 9443 share/editor.pure \\
      /tmp/pure-preview-store CA.pem SERVER.pem SERVER.key CLIENT_DNS

That selection omits the native `call` command; connect with an authenticated
external client using the pure-doc/1 binary protocol.

When selected, the HTTP host is a plaintext compatibility adapter; bind it only to loopback.
This preview was checked on Linux AArch64. It is not a static or macOS package.
"""
RUN_FIXED = """# Pure fixed document runner — Linux AArch64

This artifact runs one build-time prepared editor.pure definition. It has no
source parser, compiler, GTK or TLS host. Editing a .pure file does not
change this binary; rebuild it with `make fixed-editor` in the source tree.

    bin/pure-edit-fixed --store STORE edit KEY 0 0 0 INSERT_FILE
    bin/pure-edit-fixed --store STORE show KEY

When manifest.json lists pure-connections-fixed, it serves the same definition
and store through a loopback HTTP compatibility boundary:

    bin/pure-connections-fixed --store STORE --port 8767

When manifest.json lists pure-edit-tui-fixed, it opens a native terminal
editor for the same stored document. Ctrl+S saves, Ctrl+O enters another key,
Ctrl+L browses stored keys (type a prefix to jump), Ctrl+R reloads, and
Ctrl+Q quits:

    bin/pure-edit-tui-fixed --store STORE note

The external storage files use the default PUREB001 format. The ELF binary
needs the host's libc and is not a static or macOS package.
"""


def dependencies(path):
    header = subprocess.check_output(['readelf', '-h', str(path)], text=True)
    if not re.search(r'Machine:\s+AArch64\s*$', header, re.MULTILINE):
        raise ValueError(f'{path} is not an AArch64 ELF')
    output = subprocess.check_output(['readelf', '-d', str(path)], text=True)
    return sorted(NEEDED.findall(output))


def reviewed_budget(build, name, selected=()):
    command = ['python3', str(HERE / 'check_profile.py'), '--build', str(build),
               '--budget', str(HERE / name)]
    for binary in selected:
        command.extend(['--only', binary])
    result = subprocess.run(command, capture_output=True, text=True)
    if result.returncode:
        raise ValueError(f'{name} failed: {result.stdout or result.stderr}')
    checked = json.loads(result.stdout)
    return {'profile': checked['profile'], 'budget_sha256': checked['budget_sha256'],
            'checked_binaries': sorted(checked['binaries'])}


def reviewed_memory(build, fixed=False):
    result = subprocess.run(['python3', str(HERE / 'check_micro_memory.py'),
                             '--build', str(build), *(['--fixed'] if fixed else [])],
                            capture_output=True, text=True)
    if result.returncode:
        raise ValueError(f'memory-budget.json failed: {result.stdout or result.stderr}')
    checked = json.loads(result.stdout)
    return {'profile': checked['profile'], 'budget_sha256': checked['budget_sha256']}


def reviewed_tui_memory(build, source=False):
    result = subprocess.run(['python3', str(HERE / 'check_tui.py'), '--build', str(build),
                             *(['--source'] if source else [])],
                            capture_output=True, text=True)
    if result.returncode:
        raise ValueError(f'terminal editor check failed: {result.stdout or result.stderr}')
    checked = json.loads(result.stdout)
    return {'profile': checked['profile'],
            'one_mib_peak_rss_limit_kib': checked['one_mib_peak_rss_limit_kib'],
            'binary_sha256': checked['binary_sha256']}


def add_bytes(archive, name, content, mode):
    info = tarfile.TarInfo(ROOT_NAME + '/' + name)
    info.size = len(content)
    info.mode = mode
    info.mtime = 0
    info.uid = info.gid = 0
    info.uname = info.gname = ''
    archive.addfile(info, io.BytesIO(content))


def without_debug(source, compact=False):
    # The release build retains symbols for local debugging; the runnable
    # fixed package needs neither DWARF nor unused symbols. Never mutate the
    # build used by size checks or source/authoring package symbols.
    with tempfile.TemporaryDirectory(prefix='pure-package-strip-') as directory:
        destination = Path(directory) / source.name
        destination.write_bytes(source.read_bytes())
        subprocess.run(['strip', '--strip-unneeded' if compact else '--strip-debug',
                        str(destination)], check=True)
        if dependencies(destination) != dependencies(source):
            raise ValueError(f'{source}: stripping changed direct dependencies')
        return destination.read_bytes()


def verify_storage(binary, mode):
    with tempfile.TemporaryDirectory(prefix='pure-package-store-probe-') as directory:
        root = Path(directory)
        payload = root / 'insert'
        payload.write_bytes(b'X')
        result = subprocess.run([str(binary), '--program', str(HERE / 'examples/editor.pure'),
                                 '--store', str(root / 'store'), 'edit', 'probe', '0', '0', '0',
                                 str(payload)], capture_output=True, timeout=15)
        expected = b'PUREB001' if mode == 'hybrid' else b'PUREP001'
        if result.returncode or (root / 'store/obj-probe').read_bytes()[:8] != expected:
            raise ValueError(f'{binary} is not a working {mode} CLI: {result.stderr[-300:]}')
        if mode == 'hybrid':
            payload.write_bytes(b'X' * (16 * 1024 + 1))
            result = subprocess.run([str(binary), '--program', str(HERE / 'examples/editor.pure'),
                                     '--store', str(root / 'store'), 'edit', 'large', '0', '0', '0',
                                     str(payload)], capture_output=True, timeout=15)
            if result.returncode or (root / 'store/obj-large').read_bytes()[:8] != b'PUREP001':
                raise ValueError(f'{binary} does not select PUREP001 for large objects')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, default=HERE / 'build')
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--with-http', action='store_true')
    parser.add_argument('--with-tui', action='store_true')
    peer = parser.add_mutually_exclusive_group()
    peer.add_argument('--with-peer', action='store_true',
                      help='include the combined native TLS client/server')
    peer.add_argument('--with-peer-server', action='store_true',
                      help='include only the authenticated TLS server')
    parser.add_argument('--with-window', action='store_true')
    parser.add_argument('--fixed', action='store_true',
                        help='package only the parser-free build-time prepared editor')
    storage = parser.add_mutually_exclusive_group()
    storage.add_argument('--paged-storage', action='store_true',
                         help='package the historical PUREP001-only storage backend')
    storage.add_argument('--hybrid-storage', action='store_true',
                         help='package the selected PUREB001/PUREP001 storage experiment')
    args = parser.parse_args()
    if platform.system() != 'Linux' or platform.machine() != 'aarch64':
        parser.error('this reviewed package profile is Linux AArch64 only')
    if args.output.exists():
        parser.error('output already exists')
    if args.fixed and (args.with_peer or args.with_peer_server or args.with_window or
                       args.paged_storage or args.hybrid_storage):
        parser.error('--fixed selects only the flat-store CLI and optional HTTP/TUI hosts')
    if args.with_tui and args.paged_storage:
        parser.error('--with-tui supports the default or selected hybrid store')
    if (args.with_peer or args.with_peer_server) and (args.paged_storage or args.hybrid_storage):
        parser.error('the document TLS peer currently uses the default flat store')
    if args.fixed:
        built = subprocess.run(['make', '-C', str(HERE), f'BUILD={args.build.resolve()}',
                                'FIXED_PROGRAM=examples/editor.pure', 'fixed-editor',
                                *(['fixed-server'] if args.with_http else []),
                                *(['fixed-tui'] if args.with_tui else [])],
                               capture_output=True, text=True)
        if built.returncode:
            parser.error(f'could not prepare the fixed editor: {built.stderr[-500:]}')
    elif args.with_tui:
        built = subprocess.run(['make', '-C', str(HERE), f'BUILD={args.build.resolve()}',
                                *(['STORE=store-paged.c'] if args.hybrid_storage else []),
                                'editor', 'source-tui'], capture_output=True, text=True)
        if built.returncode:
            parser.error(f'could not prepare the source editor: {built.stderr[-500:]}')
    if args.with_peer_server:
        built = subprocess.run(['make', '-C', str(HERE), f'BUILD={args.build.resolve()}',
                                'editor', 'document-server'], capture_output=True, text=True)
        if built.returncode:
            parser.error(f'could not prepare the server-only peer: {built.stderr[-500:]}')
    if args.paged_storage or args.hybrid_storage:
        verify_storage(args.build / 'pure-edit', 'hybrid' if args.hybrid_storage else 'paged')
    helper = args.build.resolve() / 'measure-peak-rss'
    built = subprocess.run(['make', '-C', str(HERE), f'BUILD={args.build.resolve()}',
                            str(helper)], capture_output=True, text=True)
    if built.returncode:
        parser.error(f'could not prepare the RSS helper: {built.stderr[-500:]}')
    binaries = ['pure-edit-fixed' if args.fixed else 'pure-edit']
    if args.with_http:
        binaries.append('pure-connections-fixed' if args.fixed else 'pure-connections')
    if args.with_tui:
        binaries.append('pure-edit-tui-fixed' if args.fixed else 'pure-edit-tui')
    if args.with_peer:
        binaries.append('pure-document-peer')
    if args.with_peer_server:
        binaries.append('pure-document-server')
    if args.with_window:
        binaries.append('pure-edit-window')
    entries = {}
    manifest_files = {}
    for binary in binaries:
        source = args.build / binary
        content = without_debug(source, args.fixed)
        if content[:4] != b'\x7fELF':
            parser.error(f'{source} is not an ELF binary')
        name = 'bin/' + binary
        entries[name] = (content, 0o755)
        manifest_files[name] = {'sha256': hashlib.sha256(content).hexdigest(),
                                'bytes': len(content), 'needed': dependencies(source)}
    program = (HERE / 'examples/editor.pure').read_bytes()
    if not args.fixed:
        entries['share/editor.pure'] = (program, 0o644)
        manifest_files['share/editor.pure'] = {'sha256': hashlib.sha256(program).hexdigest(),
                                               'bytes': len(program)}
    entries['RUN.md'] = ((RUN_FIXED if args.fixed else RUN).encode(), 0o644)
    manifest_files['RUN.md'] = {'sha256': hashlib.sha256(entries['RUN.md'][0]).hexdigest(),
                                'bytes': len(entries['RUN.md'][0])}
    if args.paged_storage or args.hybrid_storage:
        entries['STORAGE.md'] = ((HYBRID_STORAGE if args.hybrid_storage else PAGED_STORAGE).encode(), 0o644)
        manifest_files['STORAGE.md'] = {'sha256': hashlib.sha256(entries['STORAGE.md'][0]).hexdigest(),
                                        'bytes': len(entries['STORAGE.md'][0])}
    budgets = [reviewed_budget(args.build,
                               'fixed-budget.json' if args.fixed else
                               'hybrid-budget.json' if args.hybrid_storage else
                               'paged-budget.json' if args.paged_storage else 'profile-budget.json',
                               ['pure-edit-fixed'] +
                               (['pure-connections-fixed'] if args.with_http else []) if args.fixed else
                               ['pure-edit'] + (['pure-connections'] if args.with_http else []))]
    if args.with_peer:
        budgets.append(reviewed_budget(args.build, 'peer-budget.json'))
    if args.with_peer_server:
        budgets.append(reviewed_budget(args.build, 'peer-server-budget.json'))
    if args.with_tui:
        budgets.append(reviewed_budget(args.build,
                                       'tui-budget.json' if args.fixed else
                                       'hybrid-tui-budget.json' if args.hybrid_storage else
                                       'source-tui-budget.json'))
    if args.with_window:
        budgets.append(reviewed_budget(args.build, 'window-budget.json'))
    manifest = {'format': 'pure-document-preview-1', 'platform': 'linux-aarch64',
                'selected_hosts': binaries, 'shared_libraries_bundled': False,
                'reviewed_budgets': budgets, 'reviewed_memory': reviewed_memory(args.build, args.fixed),
                'files': manifest_files}
    if args.fixed:
        manifest['program_mode'] = 'build-time-prepared'
        manifest['source_sha256'] = hashlib.sha256(program).hexdigest()
    if args.with_tui:
        manifest['reviewed_tui_memory'] = reviewed_tui_memory(args.build, not args.fixed)
    if args.with_window:
        manifest['features'] = ['native-document-key-switch']
    if args.paged_storage:
        manifest['storage_profile'] = 'PUREP001-16k-page-research'
    if args.hybrid_storage:
        manifest['storage_profile'] = 'PUREB001+PUREP001-hybrid-16k-research'
    maximum = None
    package_profile = None
    if args.fixed:
        budget_path = HERE / 'fixed-package-budget.json'
        package_budget = json.loads(budget_path.read_text())
        package_profile = 'cli' + ('_http' if args.with_http else '') + ('_tui' if args.with_tui else '')
        maximum = package_budget['max_archive_bytes'][package_profile]
        manifest['reviewed_package_budget'] = {
            'profile': package_budget['profile'], 'selection': package_profile,
            'max_archive_bytes': maximum,
            'budget_sha256': hashlib.sha256(budget_path.read_bytes()).hexdigest()}
    elif (args.with_peer or args.with_peer_server) and not (
            args.with_http or args.with_tui or args.with_window or
            args.paged_storage or args.hybrid_storage):
        budget_path = HERE / 'peer-package-budget.json'
        package_budget = json.loads(budget_path.read_text())
        package_profile = 'server_only' if args.with_peer_server else 'combined'
        maximum = package_budget['max_archive_bytes'][package_profile]
        manifest['reviewed_package_budget'] = {
            'profile': package_budget['profile'], 'selection': package_profile,
            'max_archive_bytes': maximum,
            'budget_sha256': hashlib.sha256(budget_path.read_bytes()).hexdigest()}
    entries['manifest.json'] = ((json.dumps(manifest, indent=2, sort_keys=True) + '\n').encode(), 0o644)
    stream = io.BytesIO()
    with gzip.GzipFile(filename='', mode='wb', fileobj=stream, mtime=0) as compressed:
        with tarfile.open(fileobj=compressed, mode='w') as archive:
            for name in sorted(entries):
                content, mode = entries[name]
                add_bytes(archive, name, content, mode)
    packed = stream.getvalue()
    if maximum is not None and len(packed) > maximum:
        parser.error(f'{package_profile} archive grew to {len(packed)} bytes '
                     f'(reviewed maximum {maximum}); no package was published')
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with args.output.open('xb') as output:
        output.write(packed)
    print(json.dumps({'archive': str(args.output.resolve()),
                      'sha256': hashlib.sha256(packed).hexdigest(),
                      'bytes': len(packed),
                      'selected_hosts': binaries}, sort_keys=True))


if __name__ == '__main__':
    main()
