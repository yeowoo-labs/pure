#!/usr/bin/env python3
"""Compare authenticated document reads through direct and Pure server paths."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import statistics
import subprocess
import sys
import tempfile
import time

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
CONNECTIONS = ROOT / 'prototypes/connections'
COMMON = ROOT / 'prototypes/common'
sys.path.insert(0, str(CONNECTIONS))
import check_document_peer as peer  # noqa: E402


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def command(*args):
    result = subprocess.run([str(arg) for arg in args], capture_output=True, text=True,
                            timeout=60)
    if result.returncode:
        raise AssertionError((result.args, result.returncode, result.stdout, result.stderr))
    return result.stdout


def compile_servers(directory, cc):
    flags = [cc, '-O2', '-g', '-std=c17', '-Wall', '-Wextra', '-Wpedantic', '-Werror',
             '-I' + str(CONNECTIONS)]
    common = [COMMON / 'tls-link.c', CONNECTIONS / 'store.c',
              CONNECTIONS / 'program.c', CONNECTIONS / 'evaluate.c']
    direct = directory / 'direct-peer'
    pure = directory / 'pure-server'
    command(*flags, HERE / 'direct-peer.c', *common, '-lssl', '-lcrypto', '-o', direct)
    command(*flags, '-DPURE_PEER_SERVER_ONLY', CONNECTIONS / 'document-peer.c',
            *common, CONNECTIONS / 'store-port.c', CONNECTIONS / 'runtime.c',
            '-lssl', '-lcrypto', '-o', pure)
    return {'direct': direct, 'pure': pure}


def sections(binary):
    row = command('size', binary).splitlines()[1].split()
    return dict(zip(('text', 'data', 'bss'), map(int, row[:3])))


def trial(binary, program, root, cert_root, ca, body, reads):
    store = root / 'store'
    server, port = peer.start(binary, program, store, cert_root, ca)
    try:
        with peer.connect(cert_root, ca, port) as sock:
            peer.hello(sock, program)
            assert peer.request(sock, 2, 'bench', 0, peer.edit(0, 0, body),
                                want_body=False) == (0, 1, b'', len(body))
            assert peer.request(sock, 1, 'bench') == (0, 1, body, len(body))
            rss_before = peer.rss_kib(server.pid)
            start = time.perf_counter_ns()
            for _ in range(reads):
                assert peer.request(sock, 1, 'bench') == (0, 1, body, len(body))
            elapsed_ns = time.perf_counter_ns() - start
            rss_after = peer.rss_kib(server.pid)
    finally:
        peer.stop(server)
    stored = store / 'obj-bench'
    return {'reads': reads, 'body_bytes': len(body), 'elapsed_ns': elapsed_ns,
            'server_rss_before_kib': rss_before, 'server_rss_after_kib': rss_after,
            'stored_file_bytes': stored.stat().st_size,
            'stored_file_sha256': sha(stored)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cc', default=os.environ.get('CC', 'cc'))
    parser.add_argument('--body-bytes', type=int, default=8192)
    parser.add_argument('--reads', type=int, default=512)
    parser.add_argument('--pairs', type=int, default=4)
    parser.add_argument('--work-root', type=Path, default=HERE / 'results')
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    if not 1 <= args.body_bytes <= 1024 * 1024 or not 1 <= args.reads <= 4096 or not 1 <= args.pairs <= 16:
        parser.error('body-bytes 1..1048576, reads 1..4096, pairs 1..16')
    if args.output and args.output.exists():
        parser.error('evidence file already exists')
    args.work_root.mkdir(parents=True, exist_ok=True)
    filesystem = command('df', '-T', args.work_root).splitlines()[1].split()
    program = CONNECTIONS / 'examples/editor.pure'
    body = bytes((ord('a') + i % 26 for i in range(args.body_bytes)))
    with tempfile.TemporaryDirectory(prefix='pure-direct-peer-', dir=args.work_root) as name:
        root = Path(name)
        binaries = compile_servers(root, args.cc)
        sizes = {variant: sections(binary) for variant, binary in binaries.items()}
        binary_hashes = {variant: sha(binary) for variant, binary in binaries.items()}
        ca = peer.certificates(root)
        samples = []
        for pair in range(args.pairs):
            order = ('direct', 'pure') if pair % 2 == 0 else ('pure', 'direct')
            entry = {'order': list(order)}
            for variant in order:
                path = root / f'{pair}-{variant}'
                path.mkdir()
                # The certificates live at the common root. The store is unique
                # to each server so conditional revision 0 always means create.
                entry[variant] = trial(binaries[variant], program, path, root, ca,
                                       body, args.reads)
            assert entry['direct']['stored_file_sha256'] == entry['pure']['stored_file_sha256']
            samples.append(entry)
    medians = {variant: {'elapsed_ms': statistics.median(
                    sample[variant]['elapsed_ns'] for sample in samples) / 1e6,
                         'server_rss_before_kib': statistics.median(
                    sample[variant]['server_rss_before_kib'] for sample in samples),
                         'server_rss_after_kib': statistics.median(
                    sample[variant]['server_rss_after_kib'] for sample in samples)}
               for variant in ('direct', 'pure')}
    sources = [HERE / 'direct-peer.c', HERE / 'run.py',
               CONNECTIONS / 'document-peer.c', CONNECTIONS / 'check_document_peer.py',
               COMMON / 'tls-link.c', CONNECTIONS / 'store.c',
               CONNECTIONS / 'store-port.c', CONNECTIONS / 'program.c',
               CONNECTIONS / 'evaluate.c', CONNECTIONS / 'runtime.c', program]
    result = {'profile': 'authenticated-document-read-direct-versus-pure',
              'definition_sha256': sha(program), 'body_bytes': args.body_bytes,
              'reads_per_trial': args.reads, 'pairs': args.pairs,
              'filesystem': {'device': filesystem[0], 'type': filesystem[1],
                             'mount': filesystem[-1]},
              'sections': sizes, 'binary_sha256': binary_hashes,
              'samples': samples, 'medians': medians,
              'sources_sha256': {str(path.relative_to(ROOT)): sha(path) for path in sources},
              'environment': {'platform': platform.platform(), 'machine': platform.machine(),
                              'compiler': command(args.cc, '--version').splitlines()[0],
                              'openssl': command('openssl', 'version').strip()}}
    output = json.dumps(result, indent=2) + '\n'
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(output)
    print(output, end='')


if __name__ == '__main__':
    main()
