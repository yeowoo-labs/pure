#!/usr/bin/env python3
"""Compare the same durable document job through Pure and direct store calls."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import statistics
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
CONNECTIONS = ROOT / 'prototypes/connections'
EDITOR_SHA256 = '8ca6c191b98bf53a592ce24f2b5cd040e41f05c4ccdda7a072d5d7f79870c06a'


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def run(command, timeout=180):
    result = subprocess.run([str(item) for item in command], capture_output=True,
                            text=True, timeout=timeout)
    if result.returncode:
        raise AssertionError((result.args, result.returncode,
                              result.stdout[-2000:], result.stderr[-2000:]))
    return result


def compile_all(directory, cc, body_bytes):
    flags = [cc, '-O2', '-std=c17', '-Wall', '-Wextra', '-Wpedantic', '-Werror',
             f'-DPURE_BENCH_BODY_BYTES={body_bytes}']
    compiler = directory / 'pure-compile'
    run([*flags, CONNECTIONS / 'compile-program.c', CONNECTIONS / 'program.c',
         '-o', compiler])
    header = directory / 'embedded-plan.h'
    run([compiler, CONNECTIONS / 'examples/editor.pure', header])
    common = [HERE / 'document-path.c', CONNECTIONS / 'store.c']
    direct = directory / 'direct'
    pure = directory / 'pure'
    run([*flags, '-DDIRECT_PATH', '-I' + str(CONNECTIONS), *common, '-o', direct])
    run([*flags, '-I' + str(CONNECTIONS), '-I' + str(directory), *common,
         CONNECTIONS / 'store-port.c', CONNECTIONS / 'runtime.c',
         CONNECTIONS / 'evaluate.c', '-o', pure])
    launcher = directory / 'peak-launcher'
    run([*flags, HERE / 'peak-launcher.c', '-o', launcher])
    return {'direct': direct, 'pure': pure}, launcher, header


def sections(binary):
    fields = run(['size', binary]).stdout.splitlines()[1].split()
    return {'text': int(fields[0]), 'data': int(fields[1]), 'bss': int(fields[2])}


def trial(binary, launcher, store, documents):
    result = run([launcher, binary, store, documents])
    measured = json.loads(result.stdout)
    resources = dict(line.split('=', 1) for line in result.stderr.splitlines())
    measured['peak_rss_kib'] = int(resources['PURE_PEAK_RSS_KIB'])
    measured['user_ns'] = int(resources['PURE_USER_NS'])
    measured['system_ns'] = int(resources['PURE_SYSTEM_NS'])
    objects = sorted(store.glob('obj-*'))
    assert len(objects) == documents, len(objects)
    measured['stored_file_bytes'] = sum(path.stat().st_size for path in objects)
    final_files = hashlib.sha256()
    for path in objects:
        content = path.read_bytes()
        final_files.update(path.name.encode('ascii'))
        final_files.update(len(content).to_bytes(8, 'little'))
        final_files.update(content)
    measured['stored_files_sha256'] = final_files.hexdigest()
    assert measured['documents'] == documents and measured['operations'] == 4 * documents
    return measured


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cc', default=os.environ.get('CC', 'cc'))
    parser.add_argument('--documents', type=int, default=256)
    parser.add_argument('--body-bytes', type=int, default=8192)
    parser.add_argument('--pairs', type=int, default=4)
    parser.add_argument('--work-root', type=Path, default=HERE / 'results',
                        help='filesystem for fresh stores; default is the repository filesystem')
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    if (not 1 <= args.documents <= 4096 or not 1 <= args.pairs <= 16 or
            not 1 <= args.body_bytes <= 1024 * 1024):
        parser.error('documents must be 1..4096, pairs 1..16 and body-bytes 1..1048576')
    if args.output and args.output.exists():
        parser.error('evidence file already exists')
    if platform.system() != 'Linux':
        parser.error('this release RSS apparatus uses Linux wait4 units')
    definition = CONNECTIONS / 'examples/editor.pure'
    assert sha(definition) == EDITOR_SHA256, 'direct baseline needs review after editor.pure changes'
    args.work_root.mkdir(parents=True, exist_ok=True)
    filesystem = run(['df', '-T', args.work_root]).stdout.splitlines()[1].split()
    with tempfile.TemporaryDirectory(prefix='pure-direct-document-', dir=args.work_root) as name:
        root = Path(name)
        binaries, launcher, prepared = compile_all(root, args.cc, args.body_bytes)
        sizes = {name: sections(binary) for name, binary in binaries.items()}
        binaries_sha = {name: sha(binary) for name, binary in binaries.items()}
        # One small untimed run per path loads shared code and catches a broken
        # comparison before the measured alternating, fresh-store trials.
        warmups = {name: trial(binary, launcher, root / ('warm-' + name), 4)
                   for name, binary in binaries.items()}
        assert warmups['direct']['digest'] == warmups['pure']['digest']
        samples = []
        for pair in range(args.pairs):
            order = ('direct', 'pure') if pair % 2 == 0 else ('pure', 'direct')
            entry = {'order': list(order)}
            for variant in order:
                entry[variant] = trial(binaries[variant], launcher,
                                       root / f'{pair}-{variant}', args.documents)
            assert entry['direct']['digest'] == entry['pure']['digest']
            assert entry['direct']['stored_file_bytes'] == entry['pure']['stored_file_bytes']
            assert entry['direct']['stored_files_sha256'] == entry['pure']['stored_files_sha256']
            samples.append(entry)
        prepared_sha = sha(prepared)
    medians = {variant: {'elapsed_ms': statistics.median(
                    sample[variant]['elapsed_ns'] for sample in samples) / 1e6,
                    'peak_rss_kib': statistics.median(
                    sample[variant]['peak_rss_kib'] for sample in samples),
                    'user_ms': statistics.median(
                    sample[variant]['user_ns'] for sample in samples) / 1e6,
                    'system_ms': statistics.median(
                    sample[variant]['system_ns'] for sample in samples) / 1e6,
                    'stages_ms': [statistics.median(
                    sample[variant]['stage_ns'][i] for sample in samples) / 1e6
                    for i in range(4)]}
               for variant in ('direct', 'pure')}
    result = {'profile': 'same-durable-document-app-direct-versus-pure',
              'definition_sha256': sha(definition), 'prepared_sha256': prepared_sha,
              'documents_per_variant': args.documents,
              'operations_per_variant': args.documents * 4,
              'body_bytes': args.body_bytes, 'pairs': args.pairs,
              'filesystem': {'device': filesystem[0], 'type': filesystem[1],
                             'mount': filesystem[-1]},
              'sections': sizes, 'binary_sha256': binaries_sha,
              'samples': samples, 'medians': medians,
              'median_elapsed_ratio_pure_over_direct':
                  medians['pure']['elapsed_ms'] / medians['direct']['elapsed_ms'],
              'sources_sha256': {str(path.relative_to(ROOT)): sha(path)
                  for path in [HERE / 'document-path.c', HERE / 'peak-launcher.c',
                               HERE / 'run.py', CONNECTIONS / 'store.c',
                               CONNECTIONS / 'runtime.c', CONNECTIONS / 'evaluate.c',
                               CONNECTIONS / 'store-port.c', definition]},
              'environment': {'platform': platform.platform(), 'machine': platform.machine(),
                              'compiler': run([args.cc, '--version']).stdout.splitlines()[0],
                              'size': run(['size', '--version']).stdout.splitlines()[0]}}
    body = json.dumps(result, indent=2) + '\n'
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(body)
    print(body, end='')


if __name__ == '__main__':
    main()
