#!/usr/bin/env python3
"""Compare complete 1,000-instance native app memory, with real actions and saves."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import statistics
import subprocess
import tempfile

WORKBENCH = Path(__file__).resolve().parents[1]
REPO = WORKBENCH.parents[1]
NATIVE = WORKBENCH / 'native'
CONNECTIONS = REPO / 'prototypes/connections'
PREPARED = WORKBENCH / 'build/native/application.c'
PROFILE = WORKBENCH / 'build/native/fixed-profile.zig'
LIBRARY = WORKBENCH / 'build/native/libpure-scalar-fixed.a'
BASELINE = WORKBENCH / 'results/2026-09-24-native-desktop/sources/native'


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def build(cc, directory, name, app, header, legacy, metrics=False):
    binary = directory / name
    command = [cc, '-std=c17', '-O2',
               '-ffile-prefix-map=' + str(directory) + '=/pure-app-memory',
               '-Wall', '-Wextra',
               '-Wpedantic', '-Werror', '-DPURE_FIXED_DEFINITION=1']
    if legacy:
        command.append('-DPURE_APP_MEMORY_PERSISTENT_WORK=1')
    if metrics:
        command.append('-DPURE_RUNTIME_METRICS')
    command += ['-I' + str(header), '-I' + str(NATIVE), str(PREPARED),
                str(directory / 'app-memory.c'), str(app), str(NATIVE / 'plan.c')]
    command += [str(CONNECTIONS / name) for name in
                ('runtime.c', 'evaluate.c', 'store.c', 'store-port.c')]
    command += [str(LIBRARY), '-lm', '-o', str(binary)]
    subprocess.run(command, check=True)
    sections = subprocess.check_output(['size', str(binary)], text=True).splitlines()[1].split()
    samples = []
    for _ in range(3):
        sample = json.loads(subprocess.check_output([str(binary)], text=True, timeout=30))
        assert sample['apps'] == sample['completed_actions'] == 1000
        assert sample['completed_transfers'] == 1005
        samples.append(sample)
    return {'binary_sha256': digest(binary), 'app_source_sha256': digest(app),
            'app_header_sha256': digest(header / 'app.h'),
            'text_bytes': int(sections[0]), 'data_bytes': int(sections[1]),
            'app_controller_bytes': samples[0]['app_controller_bytes'],
            'scalar_bytes': samples[0]['scalar_bytes'], 'samples': samples,
            'median_initialized_delta_kib': statistics.median(
                sample['rss_initialized_delta_kib'] for sample in samples),
            'median_saved_delta_kib': statistics.median(
                sample['rss_saved_delta_kib'] for sample in samples)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cc', default=os.environ.get('CC', 'cc'))
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    if args.output and args.output.exists():
        parser.error('evidence file already exists')
    with tempfile.TemporaryDirectory(prefix='pure-app-memory-') as root:
        directory = Path(root)
        shutil.copy2(NATIVE / 'app-memory.c', directory / 'app-memory.c')
        generated = directory / 'application.c'
        generated_profile = directory / 'fixed-profile.zig'
        subprocess.run(['node', str(NATIVE / 'prepare.mjs'),
                        str(WORKBENCH / 'examples/routed-layout.pure'),
                        str(generated), str(generated_profile)], check=True)
        assert generated.read_bytes() == PREPARED.read_bytes(), 'prepared app is stale'
        assert generated_profile.read_bytes() == PROFILE.read_bytes(), 'scalar profile is stale'
        baseline = build(args.cc, directory, 'baseline', BASELINE / 'app.c', BASELINE, True)
        elastic = build(args.cc, directory, 'elastic', NATIVE / 'app.c', NATIVE, False)
        elastic_metrics = build(args.cc, directory, 'elastic-metrics', NATIVE / 'app.c', NATIVE, False, True)
    assert baseline['scalar_bytes'] == elastic['scalar_bytes'] == elastic_metrics['scalar_bytes']
    assert elastic['app_controller_bytes'] < baseline['app_controller_bytes']
    assert elastic_metrics['app_controller_bytes'] - elastic['app_controller_bytes'] == 72
    result = {'profile': 'whole-native-app-idle-work-storage',
              'prepared_sha256': digest(PREPARED), 'scalar_archive_sha256': digest(LIBRARY),
              'harness_sha256': digest(NATIVE / 'app-memory.c'),
              'checker_sha256': digest(Path(__file__)),
              'baseline': baseline, 'elastic': elastic, 'elastic_metrics': elastic_metrics,
              'controller_saved_per_app_bytes': baseline['app_controller_bytes'] - elastic['app_controller_bytes'],
              'metrics_storage_saved_per_app_bytes': elastic_metrics['app_controller_bytes'] - elastic['app_controller_bytes'],
              'metrics_initialized_rss_median_saved_kib': elastic_metrics['median_initialized_delta_kib'] - elastic['median_initialized_delta_kib'],
              'metrics_saved_rss_median_saved_kib': elastic_metrics['median_saved_delta_kib'] - elastic['median_saved_delta_kib'],
              'initialized_rss_median_saved_kib': baseline['median_initialized_delta_kib'] - elastic['median_initialized_delta_kib'],
              'saved_rss_median_saved_kib': baseline['median_saved_delta_kib'] - elastic['median_saved_delta_kib']}
    output = json.dumps(result, indent=2) + '\n'
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.open('x').write(output)
    print(output, end='')


if __name__ == '__main__':
    main()
