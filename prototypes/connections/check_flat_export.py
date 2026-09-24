#!/usr/bin/env python3
"""Exercise paged-to-flat export with real native document edits and restore."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile
from unittest.mock import patch as mock_patch

import export_flat_store
from export_flat_store import export


def run(binary, store, *args):
    result = subprocess.run([str(binary), '--store', str(store), *map(str, args)],
                            capture_output=True, timeout=30)
    if result.returncode:
        raise AssertionError((result.args, result.returncode, result.stderr[-400:]))
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--paged-editor', type=Path, required=True)
    parser.add_argument('--flat-editor', type=Path, required=True)
    parser.add_argument('--extra-documents', type=int, default=0)
    parser.add_argument('--scratch-parent', type=Path)
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    if args.extra_documents < 0 or (args.output and args.output.exists()):
        parser.error('extra documents must be non-negative and evidence file must be new')
    paged, flat = args.paged_editor.resolve(), args.flat_editor.resolve()
    if args.scratch_parent:
        args.scratch_parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='pure-flat-export-',
                                     dir=args.scratch_parent.resolve() if args.scratch_parent else None) as name:
        root = Path(name)
        source, destination = root / 'paged', root / 'flat'
        bodies = {'empty': b'', 'small': b'X', 'boundary': b'A' * 16384,
                  'large': b'B' * 16385, 'megabyte': b'C' * 1_048_576}
        base_keys = tuple(bodies)
        for index in range(args.extra_documents):
            bodies[f'extra{index:06d}'] = bytes((index % 251,)) * 128
        revisions = {key: 1 for key in bodies}
        for key, body in bodies.items():
            input_file = root / (key + '.input')
            input_file.write_bytes(body)
            created = run(paged, source, 'edit', key, 0, 0, 0, input_file)
            assert json.loads(created.stdout)['revision'] == '1'
        patch = root / 'patch'
        patch.write_bytes(b'Z')
        for edit in range(65):
            changed = run(paged, source, 'edit', 'megabyte', edit + 1,
                          1_048_575, 1, patch)
            assert json.loads(changed.stdout)['revision'] == str(edit + 2)
        bodies['megabyte'] = bodies['megabyte'][:-1] + b'Z'
        revisions['megabyte'] = 66
        before = {key: run(paged, source, 'show', key).stdout for key in bodies}
        result = export(source, destination, paged, flat)
        assert result['objects'] == len(bodies)
        assert result['logical_bytes'] == sum(map(len, bodies.values()))
        for key, body in bodies.items():
            opened = run(flat, destination, 'show', key)
            assert opened.stdout == before[key] == body
            assert json.loads(opened.stderr)['revision'] == str(revisions[key])
        changed = run(flat, destination, 'edit', 'megabyte', 66,
                      1_048_575, 1, patch)
        assert json.loads(changed.stdout)['revision'] == '67'
        assert json.loads(run(paged, source, 'show', 'megabyte').stderr)['revision'] == '66'
        # Observe a cooperating writer while the export holds its read lock.
        make_stage = export_flat_store.tempfile.mkdtemp
        def check_locked(*stage_args, **stage_kwargs):
            attempted = subprocess.run([str(paged), '--store', str(source), 'edit',
                                        'small', '1', '0', '1', str(patch)],
                                       capture_output=True, timeout=30)
            assert attempted.returncode == 2
            assert json.loads(attempted.stderr)['error'] == 'busy'
            return make_stage(*stage_args, **stage_kwargs)
        with mock_patch.object(export_flat_store.tempfile, 'mkdtemp',
                               side_effect=check_locked):
            export(source, root / 'locked', paged, flat)
        assert json.loads(run(paged, source, 'edit', 'small', 1, 0, 1, patch).stdout)['revision'] == '2'
        real_show = export_flat_store.show
        calls = 0
        def fail_during_read(*show_args):
            nonlocal calls
            calls += 1
            if calls == 3:
                raise RuntimeError('injected export read failure')
            return real_show(*show_args)
        interrupted = root / 'interrupted'
        try:
            with mock_patch.object(export_flat_store, 'show', side_effect=fail_during_read):
                export(source, interrupted, paged, flat)
        except RuntimeError as error:
            assert str(error) == 'injected export read failure'
        else:
            raise AssertionError('injected export failure was not reported')
        assert not interrupted.exists() and not list(root.glob('.interrupted.export-*'))
        try:
            export(source, destination, paged, flat)
        except ValueError:
            pass
        else:
            raise AssertionError('existing destination was replaced')
        (source / 'obj-rogue').symlink_to(root / 'patch')
        rejected = root / 'rejected'
        try:
            export(source, rejected, paged, flat)
        except RuntimeError:
            pass
        else:
            raise AssertionError('symlink object was accepted')
        assert not rejected.exists()
        assert not list(root.glob('.rejected.export-*'))
    result.update({'passed': True, 'extra_documents': args.extra_documents,
                   'scratch_parent': str(args.scratch_parent.resolve()) if args.scratch_parent else None,
                   'source_revisions': {key: revisions[key] for key in base_keys},
                   'readback_sha256': {key: hashlib.sha256(body).hexdigest()
                                       for key, body in bodies.items() if key in base_keys},
                   'flat_edit_after_export': 'revision 67',
                   'writer_busy_during_export_and_resumed_after': True,
                   'partial_export_not_published': True,
                   'existing_target_and_symlink_rejected': True})
    body = json.dumps(result, indent=2) + '\n'
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(body)
    print(body, end='')


if __name__ == '__main__':
    main()
