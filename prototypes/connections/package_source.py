#!/usr/bin/env python3
"""Make a deterministic, buildable source snapshot without result/build trees."""
import argparse
import gzip
import hashlib
import io
import json
from pathlib import Path
import tarfile

ROOT = Path(__file__).resolve().parents[2]
PREFIX = 'pure-source-preview'
SOURCE_ROOTS = ('prototypes/connections', 'prototypes/workbench', 'prototypes/common')
SKIP_DIRS = {'build', 'results', '__pycache__', 'node_modules', '.git', '.playwright-cli'}
SUFFIXES = {'.c', '.h', '.py', '.mjs', '.js', '.zig', '.json', '.pure',
            '.html', '.css', '.svg', '.wasm', '.md', '.txt'}


def files():
    selected = {}
    for relative_root in SOURCE_ROOTS:
        for path in (ROOT / relative_root).rglob('*'):
            relative = path.relative_to(ROOT)
            if any(part in SKIP_DIRS for part in relative.parts) or path.is_symlink() or not path.is_file():
                continue
            if path.name != 'Makefile' and path.suffix not in SUFFIXES:
                continue
            selected[relative.as_posix()] = path.read_bytes()
    return selected


def add(archive, name, content):
    info = tarfile.TarInfo(PREFIX + '/' + name)
    info.size = len(content)
    info.mode = 0o644
    info.mtime = info.uid = info.gid = 0
    info.uname = info.gname = ''
    archive.addfile(info, io.BytesIO(content))


def make_archive(output):
    selected = files()
    selected['RUN.md'] = selected.pop('prototypes/connections/SOURCE_PREVIEW.md')
    manifest = {'format': 'pure-source-preview-1', 'files': {
        name: {'bytes': len(content), 'sha256': hashlib.sha256(content).hexdigest()}
        for name, content in sorted(selected.items())}}
    selected['manifest.json'] = (json.dumps(manifest, indent=2, sort_keys=True) + '\n').encode()
    output.parent.mkdir(parents=True, exist_ok=True)
    with output.open('xb') as stream:
        with gzip.GzipFile(fileobj=stream, mode='wb', filename='', mtime=0, compresslevel=9) as compressed:
            with tarfile.open(fileobj=compressed, mode='w', format=tarfile.USTAR_FORMAT) as archive:
                for name, content in sorted(selected.items()):
                    add(archive, name, content)
    return {'archive': str(output.resolve()), 'bytes': output.stat().st_size,
            'sha256': hashlib.sha256(output.read_bytes()).hexdigest(),
            'source_files': len(manifest['files'])}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    print(json.dumps(make_archive(args.output), sort_keys=True))


if __name__ == '__main__':
    main()
