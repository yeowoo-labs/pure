#!/usr/bin/env python3
"""Run a real document through multiple in-process Pure domains and stores."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile


def run(*args, code=0):
    result = subprocess.run([str(arg) for arg in args], capture_output=True, timeout=30)
    if result.returncode != code:
        raise AssertionError((result.args, result.returncode, code, result.stderr[-500:]))
    return result


def edit(binary, store, key, revision, offset, remove, file):
    result = run(binary, '--store', store, 'edit', key, revision, offset, remove, file)
    return json.loads(result.stdout)['revision']


def show(binary, store, key, revision, body):
    result = run(binary, '--store', store, 'show', key)
    assert result.stdout == body
    assert json.loads(result.stderr)['revision'] == str(revision)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--mirror', type=Path, required=True)
    parser.add_argument('--editor', type=Path, required=True)
    parser.add_argument('--rss-helper', type=Path)
    parser.add_argument('--scratch-parent', type=Path)
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    if args.output and args.output.exists():
        parser.error('evidence file already exists')
    mirror, editor = args.mirror.resolve(), args.editor.resolve()
    if args.scratch_parent:
        args.scratch_parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='pure-mirror-',
                                     dir=args.scratch_parent.resolve() if args.scratch_parent else None) as name:
        root = Path(name)
        source, left, right = root / 'source', root / 'left', root / 'right'
        initial = b'A' * 1_048_576
        changed = initial[:-1] + b'Z'
        input_file, patch, empty = root / 'initial', root / 'patch', root / 'empty'
        input_file.write_bytes(initial)
        patch.write_bytes(b'Z')
        empty.write_bytes(b'')
        assert edit(editor, source, 'document', 0, 0, 0, input_file) == '1'
        first = json.loads(run(mirror, source, 'document', left, 'document', 0,
                               right, 'document', 0).stdout)
        assert first['source_revision'] == '1' and first['bytes'] == len(initial)
        assert [target['status'] for target in first['targets']] == ['ok', 'ok']
        show(editor, left, 'document', 1, initial)
        show(editor, right, 'document', 1, initial)

        assert edit(editor, source, 'document', 1, len(initial) - 1, 1, patch) == '2'
        show(editor, right, 'document', 1, initial)
        partial = json.loads(run(mirror, source, 'document', left, 'document', 1,
                                 right, 'document', 0, code=3).stdout)
        assert [target['status'] for target in partial['targets']] == ['ok', 'conflict']
        show(editor, left, 'document', 2, changed)
        show(editor, right, 'document', 1, initial)
        recovered = json.loads(run(mirror, source, 'document', right, 'document', 1).stdout)
        assert recovered['targets'][0]['revision'] == '2'
        show(editor, right, 'document', 2, changed)

        assert edit(editor, source, 'empty', 0, 0, 0, empty) == '1'
        assert json.loads(run(mirror, source, 'empty', left, 'empty', 0).stdout)['bytes'] == 0
        show(editor, left, 'empty', 1, b'')
        zero_budget = root / 'zero-budget'
        assert json.loads(run(mirror, '--target-budget', 0, source, 'empty',
                              zero_budget, 'empty', 0).stdout)['targets'][0]['status'] == 'ok'
        show(editor, zero_budget, 'empty', 1, b'')
        low_budget = root / 'low-budget'
        refused = json.loads(run(mirror, '--target-budget', 1024, source,
                                 'document', low_budget, 'document', 0, code=2).stdout)
        assert refused == {'admission_status': 'budget', 'target_index': 0}
        assert not list(low_budget.glob('obj-*'))

        rejected_store = root / 'rejected'
        rejected = json.loads(run(mirror, source, 'document', rejected_store,
                                  'document', 0, right, 'bad/key', 0, code=2).stdout)
        assert rejected == {'admission_status': 'invalid', 'target_index': 1}
        assert not list(rejected_store.glob('obj-*'))
        show(editor, right, 'document', 2, changed)
        missing_store = root / 'missing-target'
        missing = json.loads(run(mirror, source, 'absent', missing_store,
                                 'document', 0, code=2).stdout)
        assert missing == {'source_status': 'not_found'} and not missing_store.exists()

        measured = None
        fanout = [root / f'fanout-{index}' for index in range(8)]
        fanout_args = [item for store in fanout for item in (store, 'document', 0)]
        if args.rss_helper:
            third, fourth = root / 'third', root / 'fourth'
            measured = json.loads(run(args.rss_helper.resolve(), mirror, source,
                                      'document', third, 'document', 0,
                                      fourth, 'document', 0).stdout)
            assert measured['exit_code'] == 0
            show(editor, third, 'document', 1, changed)
            show(editor, fourth, 'document', 1, changed)
            fanout_rss = json.loads(run(args.rss_helper.resolve(), mirror, source,
                                       'document', *fanout_args).stdout)
            assert fanout_rss['exit_code'] == 0
        else:
            fanout_result = json.loads(run(mirror, source, 'document', *fanout_args).stdout)
            assert len(fanout_result['targets']) == 8
            assert all(target['status'] == 'ok' for target in fanout_result['targets'])
            fanout_rss = None
        for store in fanout:
            show(editor, store, 'document', 1, changed)
    result = {'passed': True, 'source_body_sha256': hashlib.sha256(changed).hexdigest(),
              'mirror_sha256': hashlib.sha256(mirror.read_bytes()).hexdigest(),
              'editor_sha256': hashlib.sha256(editor.read_bytes()).hexdigest(),
              'source_one_mib_to_two_domains': first,
              'independent_partial_conflict': partial,
              'conflict_recovered_revision': recovered['targets'][0]['revision'],
              'empty_body_forwarded': True,
              'target_budget_checked_before_document_write': True,
              'all_admissions_before_document_writes': True,
              'source_failure_before_target_creation': True,
              'one_mib_two_target_peak_rss': measured,
              'one_mib_eight_targets_verified': True,
              'one_mib_eight_target_peak_rss': fanout_rss}
    body = json.dumps(result, indent=2) + '\n'
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(body)
    print(body, end='')


if __name__ == '__main__':
    main()
