#!/usr/bin/env python3
"""Build the authoring VM or serve its local browser adapter."""
import argparse
from datetime import datetime, timezone
from functools import partial
import hashlib
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
import json
from pathlib import Path
import shutil
import shlex
import subprocess

ROOT = Path(__file__).resolve().parent


def sync_examples():
    target = ROOT / 'web/examples'
    target.mkdir(exist_ok=True)
    for source in (ROOT / 'examples').glob('*.pure'):
        shutil.copyfile(source, target / source.name)


def build(zig):
    compiler = shutil.which(zig)
    if not compiler:
        raise SystemExit('Zig 0.15.2가 필요합니다. --zig /path/to/zig로 지정할 수 있습니다.')
    version = subprocess.check_output([compiler, 'version'], text=True).strip()
    if version != '0.15.2':
        raise SystemExit(f'이 실험은 Zig 0.15.2로 고정했습니다. 현재 {version}')
    folder = ROOT / 'build'
    folder.mkdir(exist_ok=True)
    cache = ['--cache-dir', str(folder / 'cache'), '--global-cache-dir', str(folder / 'global-cache')]
    source = ROOT / 'core.zig'
    result = subprocess.run([compiler, 'test', str(source), *cache], capture_output=True, text=True)
    print(result.stdout + result.stderr, end='', flush=True)
    result.check_returncode()
    flags = ['-target', 'wasm32-freestanding', '-O', 'ReleaseSafe', '-fstrip', '-fno-entry',
             '-rdynamic', '--export-memory', '--stack', '16384',
             '--initial-memory=65536', '--max-memory=65536']
    artifact = ROOT / 'web/core.wasm'
    subprocess.run([compiler, 'build-exe', str(source), *flags, f'-femit-bin={artifact}', *cache], check=True)
    sync_examples()
    sha = lambda path: hashlib.sha256(path.read_bytes()).hexdigest()
    (folder / 'build.json').write_text(json.dumps({
        'builtUtc': datetime.now(timezone.utc).isoformat(), 'zigVersion': version,
        'sourceSha256': sha(source), 'wasmSha256': sha(artifact),
        'evaluationSourceSha256': sha(ROOT / 'evaluation.zig'),
        'engineSourceSha256': sha(ROOT / 'engine.zig'),
        'wasmFileBytes': artifact.stat().st_size, 'flags': flags,
        'nativeTests': result.stdout + result.stderr,
    }, indent=2) + '\n')
    print(f'Built {artifact} ({artifact.stat().st_size:,} bytes)')


def native(zig, source, output, cc, desktop=False, peer=False):
    if desktop and peer:
        raise SystemExit('--desktop and --peer are separate optional hosts')
    compiler = shutil.which(zig)
    if not compiler or subprocess.check_output([compiler, 'version'], text=True).strip() != '0.15.2':
        raise SystemExit('Zig 0.15.2가 필요합니다. --zig /path/to/zig로 지정하세요.')
    folder = ROOT / 'build/native'
    folder.mkdir(parents=True, exist_ok=True)
    cache = ['--cache-dir', str(ROOT / 'build/cache'), '--global-cache-dir', str(ROOT / 'build/global-cache')]
    prepared = folder / 'application.c'
    profile = folder / 'fixed-profile.zig'
    prepare = ['node', str(ROOT / 'native/prepare.mjs'), str(source.resolve()), str(prepared)]
    if not desktop:
        prepare.append(str(profile))
    subprocess.run(prepare, check=True)
    library = folder / ('libpure-scalar.a' if desktop else 'libpure-scalar-fixed.a')
    if desktop:
        subprocess.run([compiler, 'build-lib', str(ROOT / 'native.zig'), '-O', 'ReleaseSafe',
                        '-fno-compiler-rt', f'-femit-bin={library}', *cache], check=True)
    else:
        subprocess.run([compiler, 'build-lib', '-O', 'ReleaseSafe', '-fno-compiler-rt',
                        '--dep', 'profile', '-Mroot=' + str(ROOT / 'native-fixed.zig'),
                        '-Mprofile=' + str(profile), f'-femit-bin={library}', *cache], check=True)
    result = output.resolve() if output else folder / ('pure-desktop' if desktop else 'pure-peer' if peer else 'pure-native')
    result.parent.mkdir(parents=True, exist_ok=True)
    connections = ROOT.parent / 'connections'
    inputs = [prepared, ROOT / ('native/desktop.c' if desktop else 'native/peer.c' if peer else 'native/main.c'),
              *([ROOT / 'native/peer-wire.c', ROOT.parent / 'common/tls-link.c'] if peer else []),
              ROOT / 'native/app.c', ROOT / 'native/plan.c',
              *(connections / name for name in ('runtime.c', 'evaluate.c', 'store.c', 'store-port.c')), library]
    flags = ['-std=c17', '-O2', '-g', '-Wall', '-Wextra', '-Wpedantic', '-Werror']
    if not desktop:
        flags.append('-DPURE_FIXED_DEFINITION=1')
    desktop_flags = []
    peer_flags = []
    if desktop:
        desktop_flags = shlex.split(subprocess.check_output(['pkg-config', '--cflags', '--libs', 'gtk4'], text=True))
        flags.extend(['-DPURE_NATIVE_AUTHORING=1', f'-DPURE_PREPARE_PATH="{ROOT / "native/prepare.mjs"}"',
                      f'-DPURE_NATIVE_HEADERS="{ROOT / "native"}"', f'-DPURE_CC="{cc}"'])
    if peer:
        peer_flags = shlex.split(subprocess.check_output(['pkg-config', '--cflags', '--libs', 'openssl'], text=True))
    subprocess.run([cc, *flags, '-I' + str(ROOT / 'native'), *(str(p) for p in inputs),
                    '-lm', *desktop_flags, *peer_flags, *(['-ldl'] if desktop else []),
                    '-o', str(result)], check=True)
    sha = lambda path: hashlib.sha256(path.read_bytes()).hexdigest()
    (folder / 'build.json').write_text(json.dumps({'source': str(source.resolve()), 'sourceSha256': sha(source),
        'zigVersion': '0.15.2', 'zigFlags': ['-O', 'ReleaseSafe', '-fno-compiler-rt'],
        'scalarProfile': 'general-authoring' if desktop else 'fixed-definition',
        'zigRoot': str(ROOT / ('native.zig' if desktop else 'native-fixed.zig')),
        'cFlags': flags,
        'binary': str(result), 'binarySha256': sha(result), 'librarySha256': sha(library),
        'fixedProfileSha256': sha(profile) if not desktop else None,
        'preparedSha256': sha(prepared), 'desktop': desktop, 'desktopFlags': desktop_flags,
        'peer': peer, 'peerFlags': peer_flags,
        'implementationSha256': {str(p.relative_to(ROOT.parent.parent)): sha(p) for p in [
            ROOT / 'engine.zig', ROOT / 'evaluation.zig', ROOT / 'native.zig',
            ROOT / 'native-fixed.zig', ROOT / 'native/prepare.mjs',
            ROOT / 'native/scalar.h', ROOT / 'native/plan.h', ROOT / 'native/app.h', ROOT / 'web/compiler.mjs',
            *(p for p in inputs if p.suffix == '.c' and p != prepared)]}}, indent=2) + '\n')
    print(f'Built native application: {result}')


class Handler(SimpleHTTPRequestHandler):
    extensions_map = {**SimpleHTTPRequestHandler.extensions_map, '.wasm': 'application/wasm', '.mjs': 'text/javascript', '.pure': 'text/plain'}

    def end_headers(self):
        self.send_header('Cache-Control', 'no-store')
        super().end_headers()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest='action', required=True)
    b = sub.add_parser('build')
    b.add_argument('--zig', default='zig')
    n = sub.add_parser('native', help='prepare an existing .pure definition as a native CLI, authoring desktop, or peer app')
    n.add_argument('source', type=Path)
    n.add_argument('--zig', default='zig')
    n.add_argument('--cc', default='cc')
    n.add_argument('--output', type=Path)
    n.add_argument('--desktop', action='store_true', help='build the optional Linux GTK4 authoring shell')
    n.add_argument('--peer', action='store_true', help='build the optional TLS 1.3 Pure-peer host')
    s = sub.add_parser('serve')
    s.add_argument('--port', type=int, default=8766)
    args = parser.parse_args()
    if args.action == 'build':
        build(args.zig)
        return
    if args.action == 'native':
        native(args.zig, args.source, args.output, args.cc, args.desktop, args.peer)
        return
    if not 1 <= args.port <= 65535:
        parser.error('port must be 1..65535')
    if not (ROOT / 'web/core.wasm').is_file():
        parser.error('먼저 manage.py build를 실행하세요.')
    sync_examples()
    with ThreadingHTTPServer(('127.0.0.1', args.port), partial(Handler, directory=str(ROOT / 'web'))) as server:
        print(f'Pure workbench: http://127.0.0.1:{args.port}', flush=True)
        try:
            server.serve_forever()
        except KeyboardInterrupt:
            pass


if __name__ == '__main__':
    main()
