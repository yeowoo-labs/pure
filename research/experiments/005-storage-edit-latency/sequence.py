#!/usr/bin/env python3
"""Measure repeated durable edits and page-segment rebuilds on one real document."""
import argparse
import hashlib
import json
from pathlib import Path
import platform
import statistics
import subprocess
import tempfile

from measure import INITIAL, PROGRAM, invoke


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--flat', type=Path, required=True)
    parser.add_argument('--paged', type=Path, required=True)
    parser.add_argument('--scratch-parent', type=Path, required=True)
    parser.add_argument('--edits', type=int, default=70)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    if args.edits < 66 or args.output.exists():
        parser.error('at least 66 edits are needed to observe the current rebuild boundary; output must be new')
    binaries = {'flat': args.flat.resolve(), 'paged': args.paged.resolve()}
    args.scratch_parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='pure-storage-sequence-',
                                     dir=args.scratch_parent.resolve()) as name:
        root = Path(name)
        filesystem = subprocess.run(['findmnt', '-T', str(root), '-no', 'FSTYPE,SOURCE'],
                                    capture_output=True, text=True, check=True).stdout.strip()
        (root / 'initial').write_bytes(INITIAL)
        patches = {'Y': root / 'Y', 'Z': root / 'Z'}
        for label, file in patches.items():
            file.write_bytes(label.encode())
        bases = {variant: [binary, '--program', PROGRAM, '--store', root / variant]
                 for variant, binary in binaries.items()}
        for base in bases.values():
            result, _ = invoke([*base, 'edit', 'document', 0, 0, 0, root / 'initial'])
            assert json.loads(result.stdout)['revision'] == '1'
        samples = {variant: [] for variant in binaries}
        for edit in range(1, args.edits + 1):
            patch = 'Z' if edit % 2 else 'Y'
            order = ('flat', 'paged') if edit % 2 else ('paged', 'flat')
            for variant in order:
                base = bases[variant]
                result, elapsed = invoke([*base, 'edit', 'document', edit,
                                          len(INITIAL) - 1, 1, patches[patch]])
                assert json.loads(result.stdout)['revision'] == str(edit + 1)
                files = [file for file in (root / variant).iterdir() if file.is_file()]
                samples[variant].append({'edit': edit, 'elapsed_ms': round(elapsed, 3),
                                         'files': len(files),
                                         'allocated_bytes': sum(file.stat().st_blocks * 512
                                                                for file in files)})
        expected = INITIAL[:-1] + patches[patch].read_bytes()
        for base in bases.values():
            shown, _ = invoke([*base, 'show', 'document'])
            assert shown.stdout == expected
            assert json.loads(shown.stderr)['revision'] == str(args.edits + 1)
    result = {'profile': 'repeated-durable-one-mib-document-last-byte-edit',
              'platform': platform.platform(), 'filesystem': filesystem,
              'edits_per_variant': args.edits,
              'final_body_sha256': hashlib.sha256(expected).hexdigest(),
              'scope': ('Same actual source-loaded CLI and editor.pure, two persistent '
                        'stores, alternating operation order. Timings include each '
                        'edit process, parser, store init and fsync; creation and '
                        'final show are not timed. Device state is uncontrolled.'),
              'variants': {variant: {
                  'binary_sha256': hashlib.sha256(binary.read_bytes()).hexdigest(),
                  'median_edit_ms': round(statistics.median(row['elapsed_ms']
                                                             for row in samples[variant]), 3),
                  'samples': samples[variant]}
                  for variant, binary in binaries.items()}}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps({variant: {'median_edit_ms': row['median_edit_ms'],
                                'max_edit_ms': max(sample['elapsed_ms']
                                                   for sample in row['samples']),
                                'max_allocated_bytes': max(sample['allocated_bytes']
                                                           for sample in row['samples'])}
                      for variant, row in result['variants'].items()}, indent=2))


if __name__ == '__main__':
    main()
