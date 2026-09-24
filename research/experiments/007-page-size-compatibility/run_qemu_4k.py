#!/usr/bin/env python3
"""Run the selected page store and the real document CLI under a 4 KiB kernel."""
import argparse
import gzip
import hashlib
import json
import os
from pathlib import Path
import platform
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[3]
CONNECTIONS = ROOT / 'prototypes/connections'
GUEST = Path(__file__).with_name('guest.c')


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def run(command, *, cwd=None, data=None, timeout=120):
    result = subprocess.run(command, cwd=cwd, input=data, capture_output=True,
                            timeout=timeout)
    if result.returncode:
        raise RuntimeError(f'{command[0]} failed ({result.returncode}): '
                           f'{result.stdout[-1000:]!r} {result.stderr[-1000:]!r}')
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--kernel', type=Path, required=True,
                        help='gzip-compressed or raw AArch64 4 KiB Linux kernel')
    parser.add_argument('--config', type=Path, required=True,
                        help='configuration belonging to that kernel')
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--log', type=Path, required=True)
    parser.add_argument('--cc', default='cc')
    parser.add_argument('--qemu', default='qemu-system-aarch64')
    args = parser.parse_args()
    if args.log.suffix != '.gz':
        parser.error('--log must be a .gz path')
    if platform.system() != 'Linux' or platform.machine() not in ('aarch64', 'arm64'):
        parser.error('this runner compiles an AArch64 guest on an AArch64 Linux host')
    if 'CONFIG_ARM64_4K_PAGES=y' not in args.config.read_text().splitlines():
        parser.error('the supplied kernel configuration is not a 4 KiB AArch64 build')
    if args.output.exists() or args.log.exists():
        parser.error('evidence output already exists')

    with tempfile.TemporaryDirectory(prefix='pure-qemu-4k-') as temporary:
        work = Path(temporary)
        build = work / 'build'
        run(['make', '-C', str(CONNECTIONS), f'BUILD={build}', 'fixed-editor'])
        edit = work / 'edit.o'
        run([args.cc, '-O2', '-std=c17', '-Wall', '-Wextra', '-Wpedantic', '-Werror',
             '-DPURE_EMBEDDED_PROGRAM', '-Dmain=pure_edit_main', f'-I{build}',
             '-c', str(CONNECTIONS / 'edit.c'), '-o', str(edit)])

        rootfs = work / 'rootfs'
        rootfs.mkdir()
        for name in ('tmp', 'dev'):
            (rootfs / name).mkdir()
        guest_binary = rootfs / 'init'
        run([args.cc, '-O2', '-static', '-std=c17', '-Wall', '-Wextra',
             '-Wpedantic', '-Werror', '-DPURE_STORE_TEST', f'-I{CONNECTIONS}',
             str(GUEST), str(edit), str(CONNECTIONS / 'store-paged.c'),
             str(CONNECTIONS / 'store-port.c'), str(CONNECTIONS / 'evaluate.c'),
             str(CONNECTIONS / 'runtime.c'), '-o', str(guest_binary)])
        guest_sha = sha256(guest_binary)
        for path in (rootfs, rootfs / 'tmp', rootfs / 'dev', guest_binary):
            os.utime(path, (0, 0))
        archive = work / 'initrd.cpio'
        entries = b'.\0./tmp\0./dev\0./init\0'
        archive.write_bytes(run(['cpio', '--null', '-o', '-H', 'newc', '--owner=0:0',
                                 '--reproducible'],
                                cwd=rootfs, data=entries).stdout)
        kernel_image = work / 'Image'
        with args.kernel.open('rb') as source:
            compressed_kernel = source.read(2) == b'\x1f\x8b'
        if compressed_kernel:
            with gzip.open(args.kernel, 'rb') as source, kernel_image.open('wb') as target:
                while chunk := source.read(1024 * 1024):
                    target.write(chunk)
        else:
            kernel_image.write_bytes(args.kernel.read_bytes())

        command = [args.qemu, '-machine', 'virt', '-cpu', 'cortex-a72',
                   '-smp', '1', '-m', '256M', '-nographic', '-no-reboot',
                   '-nic', 'none', '-kernel', str(kernel_image), '-initrd', str(archive),
                   '-append', 'console=ttyAMA0 loglevel=4 rdinit=/init panic=-1']
        guest_run = subprocess.run(command, capture_output=True, timeout=90)
        log_bytes = guest_run.stdout + guest_run.stderr
        args.log.parent.mkdir(parents=True, exist_ok=True)
        with args.log.open('xb') as destination:
            with gzip.GzipFile(fileobj=destination, mode='wb', filename='',
                               mtime=0, compresslevel=9) as compressed:
                compressed.write(log_bytes)
        log = log_bytes.decode(errors='replace').replace('\r\n', '\n')
        required = ('PURE_GUEST_OS_PAGE=4096', 'PURE_4K_STORE_RESULT=0',
                    'PURE_4K_APP_RESULT=0', '"mixed_edits":128',
                    '{"revision":"1","reply_bytes":1048576}',
                    '{"revision":"2","reply_bytes":1}',
                    '{"error":"conflict","revision":"2"}',
                    'reboot: Power down')
        missing = [marker for marker in required if marker not in log]
        if guest_run.returncode or missing or 'Kernel panic' in log:
            raise AssertionError(f'guest exit {guest_run.returncode}, missing {missing}; log: {args.log}')

        evidence = {
            'passed': True, 'os_page_bytes': 4096,
            'checks': ['128 mixed and fault-injected page-store edits',
                       'real prepared CLI creates and rereads every byte of a 1 MiB document',
                       'one-byte last-position revision edit and stale-revision rejection'],
            'guest_shutdown': 'power down', 'qemu_exit_code': guest_run.returncode,
            'host': {'system': platform.system(), 'machine': platform.machine(),
                     'page_bytes': os.sysconf('SC_PAGESIZE')},
            'kernel_sha256': sha256(args.kernel), 'config_sha256': sha256(args.config),
            'guest_binary_sha256': guest_sha, 'initramfs_sha256': sha256(archive),
            'log_sha256': hashlib.sha256(log_bytes).hexdigest(),
            'compressed_log_sha256': sha256(args.log),
            'qemu_version': run([args.qemu, '--version']).stdout.decode().splitlines()[0],
        }
        args.output.parent.mkdir(parents=True, exist_ok=True)
        with args.output.open('x') as destination:
            json.dump(evidence, destination, indent=2)
            destination.write('\n')
        print(json.dumps(evidence, indent=2))


if __name__ == '__main__':
    main()
