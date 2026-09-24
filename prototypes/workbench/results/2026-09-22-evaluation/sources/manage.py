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
        'wasmFileBytes': artifact.stat().st_size, 'flags': flags,
        'nativeTests': result.stdout + result.stderr,
    }, indent=2) + '\n')
    print(f'Built {artifact} ({artifact.stat().st_size:,} bytes)')


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
    s = sub.add_parser('serve')
    s.add_argument('--port', type=int, default=8766)
    args = parser.parse_args()
    if args.action == 'build':
        build(args.zig)
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
