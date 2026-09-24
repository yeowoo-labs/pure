#!/usr/bin/env python3
"""Export the selected paged document store to a new default-format store."""
import argparse
import fcntl
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

KEY = re.compile(r'[A-Za-z0-9_-]{1,64}\Z')
LIMIT = (2 * 6 + 32) * 32768 + 8192
FLAGS = os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW | os.O_CLOEXEC


def show(binary, store, key):
    result = subprocess.run([str(binary), '--store', str(store), 'show', key],
                            capture_output=True, timeout=30)
    if result.returncode:
        raise RuntimeError(f'{binary.name} could not read {key}: {result.stderr[-400:]!r}')
    details = json.loads(result.stderr)
    revision = int(details['revision'])
    body = result.stdout
    if not 1 <= revision <= (1 << 63) - 1 or len(body) != details['bytes'] or len(body) > LIMIT:
        raise RuntimeError(f'invalid stored document: {key}')
    return revision, body


def write_all(fd, content):
    content = memoryview(content)
    while content:
        written = os.write(fd, content)
        if written <= 0:
            raise OSError('short store write')
        content = content[written:]


def export(source, destination, paged_editor, flat_editor):
    source = source.resolve(strict=True)
    if os.path.lexists(destination):
        raise ValueError('destination must not exist, including a symlink')
    destination = destination.resolve(strict=False)
    if (not source.is_dir() or source == destination or source in destination.parents or
            os.path.lexists(destination)):
        raise ValueError('source must be a directory and destination must be a new separate path')
    parent = destination.parent.resolve(strict=True)
    if not parent.is_dir():
        raise ValueError('destination parent must already exist')
    lock = os.open(source / '.writer.lock', os.O_RDONLY | os.O_NOFOLLOW | os.O_CLOEXEC)
    stage = None
    try:
        # A separate CLI process reads each object; its lock-file close cannot
        # release this parent's POSIX read lock. Cooperating writers get BUSY.
        try:
            fcntl.lockf(lock, fcntl.LOCK_SH | fcntl.LOCK_NB)
        except BlockingIOError as error:
            raise RuntimeError('source has an active writer; retry export') from error
        keys = []
        with os.scandir(source) as entries:
            for entry in entries:
                if not entry.name.startswith('obj-'):
                    continue
                key = entry.name[4:]
                if not KEY.fullmatch(key) or not entry.is_file(follow_symlinks=False):
                    raise RuntimeError(f'invalid object entry: {entry.name}')
                keys.append(key)
        stage = Path(tempfile.mkdtemp(prefix=f'.{destination.name}.export-', dir=parent))
        stage_fd = os.open(stage, FLAGS)
        digest = hashlib.sha256()
        total = 0
        try:
            marker = os.open('.writer.lock', os.O_CREAT | os.O_EXCL | os.O_WRONLY |
                             os.O_NOFOLLOW | os.O_CLOEXEC, 0o600, dir_fd=stage_fd)
            try:
                os.fsync(marker)
            finally:
                os.close(marker)
            for key in sorted(keys):
                revision, body = show(paged_editor, source, key)
                name = 'obj-' + key
                fd = os.open(name, os.O_CREAT | os.O_EXCL | os.O_WRONLY |
                             os.O_NOFOLLOW | os.O_CLOEXEC, 0o600, dir_fd=stage_fd)
                try:
                    header = b'PUREB001' + revision.to_bytes(8, 'little') + len(body).to_bytes(8, 'little')
                    write_all(fd, header)
                    write_all(fd, body)
                    os.fsync(fd)
                finally:
                    os.close(fd)
                if show(flat_editor, stage, key) != (revision, body):
                    raise RuntimeError(f'flat readback differs: {key}')
                digest.update(key.encode('ascii') + b'\0')
                digest.update(header[8:])
                digest.update(body)
                total += len(body)
            os.fsync(stage_fd)
        finally:
            os.close(stage_fd)
        if os.path.lexists(destination):
            raise FileExistsError(destination)
        os.rename(stage, destination)
        stage = None
        parent_fd = os.open(parent, FLAGS)
        try:
            os.fsync(parent_fd)
        finally:
            os.close(parent_fd)
        return {'objects': len(keys), 'logical_bytes': total,
                'document_digest': digest.hexdigest(),
                'paged_reader_sha256': hashlib.sha256(paged_editor.read_bytes()).hexdigest(),
                'flat_reader_sha256': hashlib.sha256(flat_editor.read_bytes()).hexdigest()}
    finally:
        if stage is not None:
            shutil.rmtree(stage)
        os.close(lock)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, required=True)
    parser.add_argument('--destination', type=Path, required=True)
    parser.add_argument('--paged-editor', type=Path, required=True,
                        help='fixed editor linked with store-paged.c')
    parser.add_argument('--flat-editor', type=Path, required=True,
                        help='fixed editor linked with the default store.c')
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    if args.output and args.output.exists():
        parser.error('evidence file already exists')
    result = export(args.source, args.destination,
                    args.paged_editor.resolve(strict=True), args.flat_editor.resolve(strict=True))
    body = json.dumps(result, indent=2) + '\n'
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(body)
    print(body, end='')


if __name__ == '__main__':
    main()
