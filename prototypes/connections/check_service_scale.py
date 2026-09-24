#!/usr/bin/env python3
"""Exercise the real document app across many objects and concurrent clients."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile

from check_fixed_server import Host, memory_kib, request, splice

HERE = Path(__file__).resolve().parent
DOCUMENTS = 1024
CLIENTS = 8
BODY_BYTES = 8192
SERVER_BUDGET = 1_048_576
PEAK_RSS_LIMIT_KIB = 4096


def document(index):
    seed = f'document:{index:04d}:'.encode()
    return (seed * ((BODY_BYTES + len(seed) - 1) // len(seed)))[:BODY_BYTES]


def client(host, client_number):
    digests = []
    for index in range(client_number, DOCUMENTS, CLIENTS):
        key = f'doc{index:04d}'
        original = document(index)
        created = request(host, 'PUT', key, splice(0, 0, original), 0)
        assert created == (200, '"1"', original), (index, 'create', created[:2])
        opened = request(host, 'GET', key)
        assert opened == (200, '"1"', original), (index, 'open', opened[:2])
        replacement = bytes((65 + index % 26,))
        edited = request(host, 'PUT', key,
                         splice(BODY_BYTES - 1, 1, replacement), 1)
        assert edited == (200, '"2"', replacement), (index, 'edit', edited[:2])
        final = original[:-1] + replacement
        restored = request(host, 'GET', key)
        assert restored == (200, '"2"', final), (index, 'restore', restored[:2])
        digests.append((index, hashlib.sha256(final).digest()))
    return digests


def run_variant(build, root, fixed, async_source=False):
    assert not (fixed and async_source)
    store = root / ('fixed' if fixed else 'source-async' if async_source else 'source')
    binary = build / ('pure-connections-fixed' if fixed else
                      'pure-connections-async' if async_source else 'pure-connections')
    host = Host(binary, store, fixed, budget=SERVER_BUDGET)
    idle_rss = memory_kib(host.process.pid)
    idle_vm = memory_kib(host.process.pid, 'VmSize:')
    idle_threads = memory_kib(host.process.pid, 'Threads:')
    try:
        with ThreadPoolExecutor(max_workers=CLIENTS) as pool:
            batches = list(pool.map(lambda number: client(host, number),
                                    range(CLIENTS)))
        all_digests = sorted(item for batch in batches for item in batch)
        assert len(all_digests) == DOCUMENTS
        digest = hashlib.sha256(b''.join(value for _, value in all_digests)).hexdigest()
        peak_rss = memory_kib(host.process.pid, 'VmHWM:')
        peak_vm = memory_kib(host.process.pid, 'VmPeak:')
        peak_threads = memory_kib(host.process.pid, 'Threads:')
    finally:
        stats = host.close()
    editor = build / ('pure-edit-fixed' if fixed else 'pure-edit')
    for index in (0, 127, 512, DOCUMENTS - 1):
        key = f'doc{index:04d}'
        args = [str(editor), '--store', str(store)]
        if not fixed:
            args += ['--program', str(HERE / 'examples/editor.pure')]
        shown = subprocess.run([*args, 'show', key], capture_output=True, timeout=15)
        replacement = bytes((65 + index % 26,))
        assert shown.returncode == 0, (index, shown.stderr)
        assert shown.stdout == document(index)[:-1] + replacement, index
        assert json.loads(shown.stderr)['revision'] == '2', index
    objects = list(store.glob('obj-*'))
    assert len(objects) == DOCUMENTS, len(objects)
    assert stats['accepted'] == DOCUMENTS * 4, stats
    assert stats['commits'] == DOCUMENTS * 2, stats
    assert stats['gets'] == DOCUMENTS * 2, stats
    assert stats['releases'] == DOCUMENTS * 4, stats
    assert stats['budget_rejections'] == stats['expired'] == 0, stats
    assert stats['peak_connections'] <= CLIENTS, stats
    assert stats['peak_retained_payload_bytes'] <= SERVER_BUDGET, stats
    assert peak_rss <= PEAK_RSS_LIMIT_KIB, peak_rss
    return {'binary_sha256': hashlib.sha256(binary.read_bytes()).hexdigest(),
            'document_digest': digest, 'stored_objects': len(objects),
            'stored_file_bytes': sum(path.stat().st_size for path in objects),
            'fresh_cli_restore_samples': 4, 'idle_rss_kib': idle_rss,
            'peak_rss_kib': peak_rss, 'idle_vm_kib': idle_vm,
            'peak_vm_kib': peak_vm, 'idle_threads': idle_threads,
            'peak_threads': peak_threads, 'stats': stats}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, default=HERE / 'build')
    parser.add_argument('--output', type=Path)
    parser.add_argument('--async-source', action='store_true',
                        help='also run the optional one-worker HTTP host')
    args = parser.parse_args()
    if args.output and args.output.exists():
        parser.error('evidence file already exists')
    build = args.build.resolve()
    with tempfile.TemporaryDirectory(prefix='pure-document-scale-') as directory:
        root = Path(directory)
        fixed = run_variant(build, root, True)
        source = run_variant(build, root, False)
        async_source = run_variant(build, root, False, True) if args.async_source else None
    assert fixed['document_digest'] == source['document_digest']
    if async_source:
        assert source['document_digest'] == async_source['document_digest']
    result = {'profile': 'document-app-many-objects-concurrent-clients',
              'documents_per_variant': DOCUMENTS, 'clients': CLIENTS,
              'body_bytes': BODY_BYTES, 'operations_per_variant': DOCUMENTS * 4,
              'server_budget_bytes': SERVER_BUDGET,
              'peak_rss_limit_kib': PEAK_RSS_LIMIT_KIB,
              'same_final_documents': 'pass', 'fixed': fixed, 'source': source}
    if async_source:
        result['source_async'] = async_source
    body = json.dumps(result, indent=2) + '\n'
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(body)
    print(body, end='')


if __name__ == '__main__':
    main()
