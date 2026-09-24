#!/usr/bin/env python3
"""Compare the authored and parser-free runners on real document operations."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent


def run(binary, store, program, *args):
    command = [str(binary)]
    if program is not None:
        command += ['--program', str(program)]
    command += ['--store', str(store), *map(str, args)]
    return subprocess.run(command, capture_output=True, timeout=15)


def same_call(source, fixed, source_store, fixed_store, program, *args):
    original = run(source, source_store, program, *args)
    prepared = run(fixed, fixed_store, None, *args)
    assert (original.returncode, original.stdout, original.stderr) == (
        prepared.returncode, prepared.stdout, prepared.stderr), (
        args, original, prepared)
    return prepared


def check(build, selection_build, excerpt_build):
    source = (build / 'pure-edit').resolve()
    fixed = (build / 'pure-edit-fixed').resolve()
    selected = (selection_build / 'pure-edit-fixed').resolve()
    excerpt_fixed = (excerpt_build / 'pure-edit-fixed').resolve()
    compiler = (build / 'pure-compile').resolve()
    with tempfile.TemporaryDirectory(prefix='pure-fixed-program-') as name:
        root = Path(name)
        source_store, fixed_store = root / 'source', root / 'fixed'
        editor = HERE / 'examples/editor.pure'
        small = root / 'small'
        small.write_bytes(b'alpha')
        first = same_call(source, fixed, source_store, fixed_store, editor,
                          'edit', 'note', '0', '0', '0', small)
        assert first.returncode == 0
        assert same_call(source, fixed, source_store, fixed_store, editor,
                         'show', 'note').stdout == b'alpha'
        small.write_bytes(b'XY')
        assert same_call(source, fixed, source_store, fixed_store, editor,
                         'edit', 'note', '1', '2', '2', small).returncode == 0
        assert same_call(source, fixed, source_store, fixed_store, editor,
                         'edit', 'note', '1', '0', '0', small).returncode == 3
        assert same_call(source, fixed, source_store, fixed_store, editor,
                         'edit', 'note', '2', '500', '0', small).returncode != 0
        assert same_call(source, fixed, source_store, fixed_store, editor,
                         'show', 'note').stdout == b'alXYa'

        one_mib = root / 'one-mib'
        one_mib.write_bytes(b'A' * 1_048_576)
        assert same_call(source, fixed, source_store, fixed_store, editor,
                         'edit', 'large', '0', '0', '0', one_mib).returncode == 0
        small.write_bytes(b'Z')
        assert same_call(source, fixed, source_store, fixed_store, editor,
                         'edit', 'large', '1', '1048575', '1', small).returncode == 0
        assert same_call(source, fixed, source_store, fixed_store, editor,
                         'show', 'large').stdout == b'A' * 1_048_575 + b'Z'

        selection = HERE / 'examples/selection.pure'
        input_file = root / 'selection-input'
        input_file.write_bytes(struct.pack('<II', 2, 3) + b'abcdef')
        selected_store = root / 'selected'
        authored_store = root / 'authored-selected'
        assert same_call(source, selected, authored_store, selected_store,
                         selection, 'apply', 'value', '0', input_file).returncode == 0
        assert same_call(source, selected, authored_store, selected_store,
                         selection, 'show', 'value').stdout == b'cde'

        excerpt = HERE / 'examples/excerpt.pure'
        excerpt_input = root / 'excerpt-input'
        excerpt_input.write_bytes(bytes(range(79)))
        excerpt_source_store, excerpt_fixed_store = root / 'excerpt-source', root / 'excerpt-fixed'
        assert same_call(source, excerpt_fixed, excerpt_source_store,
                         excerpt_fixed_store, excerpt,
                         'apply', 'sample', '0', excerpt_input).returncode == 0
        assert same_call(source, excerpt_fixed, excerpt_source_store,
                         excerpt_fixed_store, excerpt, 'show', 'sample').returncode != 0
        excerpt_input.write_bytes(bytes(range(80)))
        assert same_call(source, excerpt_fixed, excerpt_source_store,
                         excerpt_fixed_store, excerpt,
                         'apply', 'sample', '1', excerpt_input).returncode == 0
        assert same_call(source, excerpt_fixed, excerpt_source_store,
                         excerpt_fixed_store, excerpt, 'show', 'sample').stdout == bytes(range(32, 48))
        input_file.write_bytes(struct.pack('<II', 2, 300) + b'abcdef')
        assert same_call(source, selected, authored_store, selected_store,
                         selection, 'apply', 'value', '1', input_file).returncode != 0
        assert same_call(source, selected, authored_store, selected_store,
                         selection, 'show', 'value').stdout == b'cde'

        generated = root / 'plan.h'
        generated.write_bytes(b'prior validated plan\n')
        invalid = root / 'invalid.pure'
        invalid.write_bytes(b'on put { broken }\n')
        attempt = subprocess.run([str(compiler), str(invalid), str(generated)],
                                 capture_output=True, timeout=15)
        assert attempt.returncode != 0 and generated.read_bytes() == b'prior validated plan\n'
        same_path = root / 'source.pure'
        same_path.write_bytes(editor.read_bytes())
        attempt = subprocess.run([str(compiler), str(same_path), str(same_path)],
                                 capture_output=True, timeout=15)
        assert attempt.returncode != 0 and same_path.read_bytes() == editor.read_bytes()

        # A changed FIXED_PROGRAM in one build directory must never silently
        # reuse the previous embedded plan.
        switched_build = root / 'reconfigured-build'
        digests = []
        for definition in ('editor', 'selection', 'editor'):
            command = ['make', '-C', str(HERE), f'BUILD={switched_build}',
                       f'FIXED_PROGRAM=examples/{definition}.pure', 'fixed-editor']
            built = subprocess.run(command, capture_output=True, timeout=30)
            assert built.returncode == 0, built.stderr[-1000:]
            digests.append(hashlib.sha256((switched_build / 'pure-edit-fixed').read_bytes()).hexdigest())
        assert digests[0] != digests[1] and digests[0] == digests[2]

    symbols = subprocess.run(['nm', '-g', str(fixed)], capture_output=True,
                             text=True, check=True, timeout=15).stdout
    assert 'pure_program_parse' not in symbols and 'pure_program_load' not in symbols
    return {'profile': 'build-time-prepared-native-document-runner',
            'editor_create_edit_conflict_large_restore': 'pass',
            'dynamic_selection_and_invalid_range': 'pass',
            'nested_read_and_required_extent': 'pass',
            'invalid_compilation_preserves_previous_plan': 'pass',
            'compiler_rejects_source_as_output': 'pass',
            'same_directory_program_switch_rebuilds': 'pass',
            'runtime_parser_symbols_absent': 'pass',
            'fixed_sha256': hashlib.sha256(fixed.read_bytes()).hexdigest(),
            'source_sha256': hashlib.sha256(source.read_bytes()).hexdigest(),
            'selection_sha256': hashlib.sha256(selected.read_bytes()).hexdigest(),
            'excerpt_sha256': hashlib.sha256(excerpt_fixed.read_bytes()).hexdigest()}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, default=HERE / 'build')
    parser.add_argument('--selection-build', type=Path,
                        default=HERE / 'build/fixed-selection')
    parser.add_argument('--excerpt-build', type=Path,
                        default=HERE / 'build/fixed-excerpt')
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    result = check(args.build, args.selection_build, args.excerpt_build)
    body = json.dumps(result, indent=2) + '\n'
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(body)
    print(body, end='')


if __name__ == '__main__':
    main()
