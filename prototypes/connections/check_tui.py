#!/usr/bin/env python3
"""Exercise the real terminal editor through a PTY and the same document CLI."""
import argparse
import fcntl
import hashlib
import json
import locale
import os
from pathlib import Path
import pty
import select
import signal
import struct
import subprocess
import tempfile
import termios
import time

HERE = Path(__file__).resolve().parent


def command(*args):
    return subprocess.run([str(arg) for arg in args], capture_output=True,
                          timeout=20, check=True)


def utf8_locale():
    original = locale.setlocale(locale.LC_CTYPE)
    candidates = [os.environ.get('LC_ALL'), os.environ.get('LC_CTYPE'),
                  os.environ.get('LANG'), 'C.UTF-8', 'en_US.UTF-8']
    try:
        for candidate in candidates:
            if not candidate:
                continue
            try:
                locale.setlocale(locale.LC_CTYPE, candidate)
            except locale.Error:
                continue
            if locale.nl_langinfo(locale.CODESET).upper().replace('-', '') == 'UTF8':
                return candidate
    finally:
        locale.setlocale(locale.LC_CTYPE, original)
    raise AssertionError('no UTF-8 locale available for terminal test')


def memory_kib(pid, field, portable=False):
    if portable:
        return None
    for line in Path(f'/proc/{pid}/status').read_text().splitlines():
        if line.startswith(field):
            return int(line.split()[1])
    raise AssertionError(field)


class Terminal:
    def __init__(self, binary, program, store, key):
        self.master, self.slave = pty.openpty()
        fcntl.ioctl(self.slave, termios.TIOCSWINSZ, struct.pack('HHHH', 24, 80, 0, 0))
        self.original = termios.tcgetattr(self.slave)
        language = utf8_locale()
        self.process = subprocess.Popen([str(binary),
                                         *(['--program', str(program)] if program else []),
                                         '--store', str(store), key],
                                        stdin=self.slave, stdout=self.slave, stderr=self.slave,
                                        env={**os.environ, 'LANG': language,
                                             'LC_ALL': language})
        self.wait_for(f'Pure | {key}'.encode())

    def wait_for(self, marker, timeout=8):
        seen = b''
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            ready, _, _ = select.select([self.master], [], [], 0.1)
            if ready:
                part = os.read(self.master, 65536)
                seen = (seen + part)[-65536:]
                if marker in seen:
                    return seen
            if self.process.poll() is not None:
                break
        raise AssertionError((marker, self.process.poll(), seen[-2000:]))

    def type(self, data):
        os.write(self.master, data)

    def resize(self, rows, columns):
        fcntl.ioctl(self.slave, termios.TIOCSWINSZ,
                    struct.pack('HHHH', rows, columns, 0, 0))
        os.kill(self.process.pid, signal.SIGWINCH)

    def close(self, discard=False):
        if self.process.poll() is None:
            self.type(b'\x11')
            if discard:
                self.wait_for(b'Unsaved draft')
                self.type(b'\x11')
        self.process.wait(timeout=8)
        assert self.process.returncode == 0, self.process.returncode
        restored = termios.tcgetattr(self.slave)
        assert restored == self.original, (restored, self.original)
        os.close(self.master)
        os.close(self.slave)

    def signal_close(self):
        os.kill(self.process.pid, signal.SIGTERM)
        self.process.wait(timeout=8)
        assert self.process.returncode == 0, self.process.returncode
        assert termios.tcgetattr(self.slave) == self.original
        os.close(self.master)
        os.close(self.slave)


def shown(editor, store, key):
    result = command(*editor, '--store', store, 'show', key)
    return result.stdout, json.loads(result.stderr)['revision']


def wait_stored(editor, store, key, expected, revision):
    deadline = time.monotonic() + 8
    while time.monotonic() < deadline:
        try:
            body, found_revision = shown(editor, store, key)
            if body == expected and found_revision == str(revision):
                return
        except subprocess.CalledProcessError:
            pass
        time.sleep(0.05)
    raise AssertionError((key, expected[:100], shown(editor, store, key)))


def check(build, rss_limit_kib, source, program_path, portable=False, object_files=True):
    binary = (build / ('pure-edit-tui' if source else 'pure-edit-tui-fixed')).resolve()
    program = (program_path or HERE / 'examples/editor.pure').resolve() if source else None
    editor = [str((build / ('pure-edit' if source else 'pure-edit-fixed')).resolve())]
    if source:
        editor += ['--program', str(program)]
    with tempfile.TemporaryDirectory(prefix='pure-tui-') as name:
        root = Path(name)
        store = root / 'store'
        if source:
            invalid = root / 'invalid.pure'
            invalid.write_text('not a pure program\n')
            for candidate, marker in ((invalid, b'Program:'),
                                      (HERE / 'examples/storage.pure', b'Program needs')):
                denied = subprocess.run([str(binary), '--program', str(candidate),
                                         '--store', str(store), 'note'],
                                        capture_output=True, timeout=10)
                assert denied.returncode == 2 and marker in denied.stderr
                assert not store.exists()
        terminal = Terminal(binary, program, store, 'note')
        try:
            terminal.type(b'hello Pure\x1b[D\x1b[D!\x13')
            wait_stored(editor, store, 'note', b'hello Pu!re', 1)
            if source:
                assert command(*editor, '--store', store, 'show', 'note').stdout == b'hello Pu!re'
            terminal.type(b'\x0fsecond\r')
            terminal.wait_for(b'Pure | second')
            terminal.type('안녕'.encode() + b'\x1b[D' + '하'.encode() + b'\x13')
            terminal.wait_for('안하녕'.encode())
            wait_stored(editor, store, 'second', '안하녕'.encode(), 1)
            terminal.resize(32, 100)
            terminal.wait_for(b'Pure | second')
            terminal.type(b'\x0fnote\r')
            terminal.wait_for(b'Pure | note')
            terminal.type(b'\x0c')
            listing = terminal.wait_for(b'Pure | documents')
            assert b'note' in listing and b'second' in listing, listing[-1000:]
            terminal.type(b'\x1b[B\r')
            terminal.wait_for(b'Pure | second')
            terminal.type(b'\x0c\x1b[A\r')
            terminal.wait_for(b'Pure | note')
            terminal.type(b'\x0c')
            terminal.wait_for(b'Pure | documents')
            if object_files:
                # Only per-object file stores can lose one document underneath the list.
                (store / 'obj-second').unlink()
                terminal.type(b'\x1b[B\r')
                terminal.wait_for(b'Document disappeared; refresh the list')
            terminal.type(b'\x1b')
            terminal.wait_for(b'Pure | note')
            terminal.type(b'!')
            terminal.type(b'\x0c')
            terminal.wait_for(b'Save or reload the draft before browsing')
            external = root / 'external'
            external.write_bytes(b' remote')
            command(*editor, '--store', store, 'edit', 'note', '1', '11', '0', external)
            terminal.type(b'\x13')
            terminal.wait_for(b'Version conflict')
            assert shown(editor, store, 'note') == (b'hello Pu!re remote', '2')
            small_peak = memory_kib(terminal.process.pid, 'VmHWM:', portable)
        finally:
            terminal.close(discard=True)

        multiline = b'first\n\tsecond\nthird'
        insert = root / 'multiline-insert'
        insert.write_bytes(multiline)
        command(*editor, '--store', store, 'edit', 'multiline', '0', '0', '0', insert)
        terminal = Terminal(binary, program, store, 'multiline')
        try:
            terminal.resize(25, 80)
            terminal.wait_for(b'third')
            terminal.type(b'!\x13')
            wait_stored(editor, store, 'multiline', multiline + b'!', 2)
        finally:
            terminal.close()
        terminal = Terminal(binary, program, store, 'multiline')
        terminal.resize(25, 80)
        terminal.wait_for(b'third!')
        terminal.close()

        rows = [f'line-{index:02}'.encode() for index in range(60)]
        insert.write_bytes(b'\n'.join(rows))
        command(*editor, '--store', store, 'edit', 'rows', '0', '0', '0', insert)
        terminal = Terminal(binary, program, store, 'rows')
        try:
            terminal.type(b'\x1b[A' * 3 + b'X\x13')
            rows[56] += b'X'
            wait_stored(editor, store, 'rows', b'\n'.join(rows), 2)
            terminal.wait_for(b'line-56X')
        finally:
            terminal.close()

        large = b'Q' * 1_048_576
        insert = root / 'large-insert'
        insert.write_bytes(large)
        command(*editor, '--store', store, 'edit', 'large', '0', '0', '0', insert)
        terminal = Terminal(binary, program, store, 'large')
        try:
            terminal.type(b'Z')
            terminal.wait_for(b'Z')
            terminal.type(b'\x13')
            wait_stored(editor, store, 'large', large + b'Z', 2)
            large_peak = memory_kib(terminal.process.pid, 'VmHWM:', portable)
        finally:
            terminal.close()
        if rss_limit_kib:
            assert large_peak <= rss_limit_kib, (large_peak, rss_limit_kib)

        unicode_body = '안녕'.encode() * 174_762
        insert.write_bytes(unicode_body)
        command(*editor, '--store', store, 'edit', 'unicode', '0', '0', '0', insert)
        terminal = Terminal(binary, program, store, 'unicode')
        try:
            terminal.type(b'K')
            terminal.wait_for(b'K')
            terminal.type(b'\x1b[HX')
            terminal.wait_for(b'X' + '안녕'.encode())
            terminal.type(b'\x13')
            wait_stored(editor, store, 'unicode', b'X' + unicode_body + b'K', 2)
        finally:
            terminal.close()

        listed = root / 'listed-store'
        insert.write_bytes(b'X')
        for index in range(1024):
            command(*editor, '--store', listed, 'edit', f'doc{index:04}', '0', '0', '0', insert)
        terminal = Terminal(binary, program, listed, 'doc0000')
        try:
            terminal.type(b'\x0c')
            terminal.wait_for(b'Pure | documents | 1024 keys')
            terminal.resize(8, 12)
            terminal.wait_for(b'\033[8;1H')
            terminal.resize(24, 80)
            terminal.wait_for(b'Pure | documents | 1024 keys')
            terminal.type(b'\x1b[F\r')
            terminal.wait_for(b'Pure | doc1023')
            assert shown(editor, listed, 'doc1023') == (b'X', '1')
            terminal.type(b'\x0c')
            terminal.wait_for(b'Pure | documents | 1024 keys')
            terminal.type(b'doc0919')
            terminal.wait_for(b'Find: doc0919_')
            terminal.type(b'\r')
            terminal.wait_for(b'Pure | doc0919')
            assert shown(editor, listed, 'doc0919') == (b'X', '1')
            terminal.type(b'\x0czz')
            terminal.wait_for(b'No matching key')
            terminal.type(b'\r')
            terminal.wait_for(b'No matching key')
            terminal.type(b'\x7f\x7fdoc1023\r')
            terminal.wait_for(b'Pure | doc1023')
            list_peak = memory_kib(terminal.process.pid, 'VmHWM:', portable)
        finally:
            terminal.close()
        if rss_limit_kib:
            assert list_peak <= rss_limit_kib, (list_peak, rss_limit_kib)

        if source:
            alternate = root / 'alternate.pure'
            definition = program.read_text()
            fields = '    start: u32le;\n    remove: u32le;'
            assert fields in definition
            alternate.write_text(definition.replace(fields,
                '    remove: u32le;\n    start: u32le;'))
            alternate_store = root / 'alternate-store'
            terminal = Terminal(binary, alternate, alternate_store, 'different_layout')
            try:
                terminal.type(b'abcdef\x13')
                wait_stored(editor, alternate_store, 'different_layout', b'abcdef', 1)
                terminal.type(b'\x1b[D\x1b[D\x1b[D!\x13')
                wait_stored(editor, alternate_store, 'different_layout', b'abc!def', 2)
            finally:
                terminal.close()

        terminal = Terminal(binary, program, store, 'note')
        terminal.signal_close()
        return {'profile': 'native-source-document-terminal-editor' if source else
                           'native-fixed-document-terminal-editor',
            'checks': (['source parse and incompatible definitions fail before store creation',
                        'source CLI and TUI read the same document',
                        'changed source field layout drives TUI input encoding'] if source else []) +
                      ['cursor insertion and save through terminal', 'open another key',
                       'bounded document browser selects keys and protects unsaved draft',
                       'UTF-8 codepoint movement, visible redraw and edit', 'resize redraw',
                       'external revision conflict preserves draft',
                       'stored multiline and tabbed document survives edit and reopen',
                       'cached viewport preserves vertical movement across many rows',
                       'one MiB document viewport, edit and fresh CLI restore',
                       'one MiB UTF-8 viewport and edit before cached anchor',
                       '1,024 real documents listed, searched, resized and opened' +
                       (' within RSS gate' if not portable else ''),
                       'terminal mode restored after key and signal exit'],
            'small_peak_rss_kib': small_peak,
            'one_mib_peak_rss_kib': large_peak,
            'one_thousand_key_list_peak_rss_kib': list_peak,
            'one_mib_peak_rss_limit_kib': rss_limit_kib,
            'portable': portable,
            'terminal_locale': utf8_locale(),
            'binary_sha256': hashlib.sha256(binary.read_bytes()).hexdigest()}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, default=HERE / 'build')
    parser.add_argument('--rss-limit-kib', type=int, default=4096,
                        help='0 skips the release RSS gate for sanitizer builds')
    parser.add_argument('--source', action='store_true', help='check the source-loaded TUI')
    parser.add_argument('--portable', action='store_true',
                        help='run all PTY behavior without Linux /proc RSS measurement')
    parser.add_argument('--program', type=Path, help='definition for --source (including relocated bundles)')
    parser.add_argument('--single-file-store', action='store_true',
                        help='store keeps all documents in one file (skips external deletion)')
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    if args.output and args.output.exists():
        parser.error('evidence file already exists')
    result = check(args.build, 0 if args.portable else args.rss_limit_kib,
                   args.source, args.program, args.portable, not args.single_file_store)
    body = json.dumps(result, indent=2) + '\n'
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(body)
    print(body, end='')


if __name__ == '__main__':
    main()
