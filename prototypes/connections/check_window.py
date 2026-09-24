#!/usr/bin/env python3
"""Real GTK/X11 document flow. Run under xvfb-run after `make window editor`."""
import argparse
import http.client
import json
import os
from pathlib import Path
import re
import select
import struct
import subprocess
import tempfile
import time

HERE = Path(__file__).resolve().parent
BUILD = HERE / "build"
PROGRAM = HERE / "examples/editor.pure"


def command(*args, input=None):
    result = subprocess.run(args, input=input, capture_output=True, timeout=15)
    if result.returncode:
        raise AssertionError((args, result.returncode, result.stdout, result.stderr))
    return result.stdout


def xdo(*args):
    return command("xdotool", *args).decode().strip()


def switch_document(window, key):
    xdo("key", "ctrl+o")
    xdo("key", "ctrl+a")
    xdo("type", "--clearmodifiers", key)
    xdo("key", "Return")
    for _ in range(50):
        title = xdo("getwindowname", window)
        if f"{key} · Pure document" in title:
            return
        time.sleep(0.05)
    raise AssertionError((key, title))


def store_cli(directory, *args):
    return subprocess.run(
        [str(BUILD / "pure-edit"), "--program", str(PROGRAM),
         "--store", str(directory), *args], capture_output=True, timeout=15
    )


def read_document(directory, key, revision):
    for _ in range(40):
        result = store_cli(directory, "show", key)
        if result.returncode == 0:
            metadata = json.loads(result.stderr)
            if metadata["revision"] == str(revision):
                return result.stdout
        time.sleep(0.1)
    raise AssertionError((key, revision, result.returncode, result.stdout, result.stderr))


def remote_edit(directory, key, revision, offset, inserted):
    server = subprocess.Popen(
        [str(BUILD / "pure-connections"), "--program", str(PROGRAM),
         "--store", str(directory), "--port", "0"],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE
    )
    try:
        ready, _, _ = select.select([server.stdout], [], [], 5)
        assert ready, "document server did not start"
        port = json.loads(server.stdout.readline())["port"]
        connection = http.client.HTTPConnection("127.0.0.1", port, timeout=5)
        try:
            body = struct.pack("<II", offset, 0) + inserted
            connection.request("PUT", f"/objects/{key}", body,
                               {"Content-Length": str(len(body)), "If-Match": f'"{revision}"'})
            response = connection.getresponse()
            received = response.read()
            assert response.status == 200 and response.getheader("ETag") == f'"{revision + 1}"'
            assert received == inserted
        finally:
            connection.close()
    finally:
        server.terminate()
        try:
            server.communicate(timeout=5)
        except subprocess.TimeoutExpired:
            server.kill()
            server.communicate(timeout=5)


def cli_external_edit(directory, key, revision, offset, inserted):
    payload = directory / 'external-insert'
    payload.write_bytes(inserted)
    result = store_cli(directory, 'edit', key, str(revision), str(offset), '0', str(payload))
    assert result.returncode == 0, result.stderr


def start_window(directory, key):
    environment = dict(os.environ, GSK_RENDERER="cairo", GDK_DISABLE="gl,vulkan",
                       GTK_A11Y="none")
    app = subprocess.Popen(
        [str(BUILD / "pure-edit-window"), "--program", str(PROGRAM),
         "--store", str(directory), key], env=environment,
        stdout=subprocess.DEVNULL, stderr=subprocess.PIPE
    )
    for _ in range(60):
        found = subprocess.run(["xdotool", "search", "--name", "Pure document"],
                               capture_output=True, timeout=5)
        if found.returncode == 0:
            window = found.stdout.decode().splitlines()[0]
            focused = subprocess.run(["xdotool", "windowfocus", "--sync", window],
                                     capture_output=True, timeout=5)
            if focused.returncode:
                time.sleep(0.1)
                continue
            xdo("mousemove", "--window", window, "120", "150")
            xdo("click", "1")
            return app, window
        if app.poll() is not None:
            break
        time.sleep(0.1)
    app.terminate()
    _, error = app.communicate(timeout=5)
    raise AssertionError(("window failed to open", error))


def stop_window(app):
    app.terminate()
    try:
        _, error = app.communicate(timeout=5)
    except subprocess.TimeoutExpired:
        app.kill()
        _, error = app.communicate(timeout=5)
    assert app.returncode in (0, -15), error
    assert b"Gtk-CRITICAL" not in error and b"runtime error:" not in error, error
    return error


def clipboard():
    xdo("key", "ctrl+a")
    xdo("key", "ctrl+c")
    time.sleep(0.1)
    return command("xclip", "-selection", "clipboard", "-o", "-t", "UTF8_STRING")


def rss_kib(pid):
    for line in Path(f"/proc/{pid}/status").read_text().splitlines():
        if line.startswith("VmRSS:"):
            return int(line.split()[1])
    raise AssertionError("VmRSS unavailable")


def rss_breakdown_kib(pid):
    groups = {'heap': 0, 'anonymous': 0, 'file': 0, 'stack': 0, 'other': 0}
    category = 'other'
    for line in Path(f"/proc/{pid}/smaps").read_text().splitlines():
        if re.match(r'^[0-9a-f]+-[0-9a-f]+ ', line):
            fields = line.split(maxsplit=5)
            name = fields[5] if len(fields) > 5 else ''
            category = ('heap' if name == '[heap]' else
                        'stack' if name.startswith('[stack') else
                        'anonymous' if not name or name.startswith('[anon:') else
                        'file' if name.startswith('/') else 'other')
        elif line.startswith('Rss:'):
            groups[category] += int(line.split()[1])
    return groups


def check(check_key_switch=True, switch_rounds=16, diagnose_maps=False,
          switch_mode='alternate', graceful_large_close=False, settle_seconds=0,
          large_line_width=80):
    global PROGRAM
    assert os.environ.get("DISPLAY"), "run under xvfb-run or another X11 display"
    with tempfile.TemporaryDirectory(prefix="pure-document-window-") as name:
        directory = Path(name)
        source_definition = PROGRAM.read_bytes()
        PROGRAM = directory / "editable.pure"
        PROGRAM.write_bytes(source_definition)
        app, _ = start_window(directory, "note")
        try:
            xdo("type", "--clearmodifiers", "--delay", "15", "hello Pure")
            xdo("key", "ctrl+s")
            assert read_document(directory, "note", 1) == b"hello Pure"
            xdo("type", "--clearmodifiers", "!")
            if (BUILD / 'pure-connections').exists():
                remote_edit(directory, "note", 1, 10, b" remote")
                external_host = 'http'
            else:
                cli_external_edit(directory, "note", 1, 10, b" remote")
                external_host = 'cli'
            xdo("key", "ctrl+s")
            assert read_document(directory, "note", 2) == b"hello Pure remote"
            assert clipboard() == b"hello Pure!", "conflict replaced the local draft"
            xdo("key", "ctrl+w")
            time.sleep(0.15)
            assert app.poll() is None, "unsaved draft closed without confirmation"
            xdo("key", "Escape")
            time.sleep(0.1)
            assert app.poll() is None and clipboard() == b"hello Pure!"
        finally:
            stop_window(app)

        if check_key_switch:
            # One native window must switch among stored objects without dropping
            # a draft or installing a non-text object as the active document.
            app, window = start_window(directory, "first")
            try:
                xdo("type", "--clearmodifiers", "alpha")
                xdo("key", "ctrl+s")
                assert read_document(directory, "first", 1) == b"alpha"
                xdo("key", "ctrl+o")
                xdo("key", "ctrl+a")
                xdo("type", "--clearmodifiers", "second")
                xdo("key", "Return")
                assert "second · Pure document" in xdo("getwindowname", window)
                xdo("type", "--clearmodifiers", "beta")
                xdo("key", "ctrl+s")
                assert read_document(directory, "second", 1) == b"beta"
                xdo("type", "--clearmodifiers", "!")
                xdo("key", "ctrl+o")
                xdo("key", "ctrl+a")
                xdo("type", "--clearmodifiers", "third")
                xdo("key", "Return")
                assert "second · Pure document" in xdo("getwindowname", window)
                xdo("mousemove", "--window", window, "120", "150")
                xdo("click", "1")
                assert clipboard() == b"beta!", "switch discarded the document draft"
                xdo("key", "ctrl+s")
                assert read_document(directory, "second", 2) == b"beta!"
                xdo("key", "ctrl+o")
                xdo("key", "ctrl+a")
                xdo("type", "--clearmodifiers", "third")
                xdo("key", "Return")
                assert "third · Pure document" in xdo("getwindowname", window)
                xdo("type", "--clearmodifiers", "gamma")
                xdo("key", "ctrl+s")
                assert read_document(directory, "third", 1) == b"gamma"

                xdo("key", "ctrl+o")
                xdo("key", "ctrl+a")
                xdo("type", "--clearmodifiers", "bad/key")
                xdo("key", "Return")
                assert "third · Pure document" in xdo("getwindowname", window)
                payload = directory / "binary-payload"
                payload.write_bytes(b"\x00")
                assert store_cli(directory, "edit", "binary", "0", "0", "0", str(payload)).returncode == 0
                xdo("key", "ctrl+o")
                xdo("key", "ctrl+a")
                xdo("type", "--clearmodifiers", "binary")
                xdo("key", "Return")
                assert "third · Pure document" in xdo("getwindowname", window)
                assert read_document(directory, "third", 1) == b"gamma"

                xdo("key", "ctrl+shift+d")
                xdo("key", "ctrl+End")
                xdo("type", "--clearmodifiers", " draft")
                xdo("key", "ctrl+o")
                xdo("key", "ctrl+a")
                xdo("type", "--clearmodifiers", "fourth")
                xdo("key", "Return")
                assert "third · Pure document" in xdo("getwindowname", window)
                xdo("key", "ctrl+shift+d")
                xdo("key", "ctrl+shift+d")
                assert b" draft" in clipboard(), "switch discarded the definition draft"
            finally:
                stop_window(app)

        app, _ = start_window(directory, "empty")
        try:
            xdo("key", "ctrl+s")
            assert read_document(directory, "empty", 1) == b""
        finally:
            stop_window(app)
        app, _ = start_window(directory, "empty")
        try:
            xdo("key", "ctrl+s")
            assert read_document(directory, "empty", 1) == b""
        finally:
            stop_window(app)

        insertion = directory / "remote-insert"
        insertion.write_bytes("가나다".encode())
        initial = store_cli(directory, "edit", "unicode", "0", "0", "0", str(insertion))
        assert initial.returncode == 0, initial.stderr
        app, _ = start_window(directory, "unicode")
        try:
            assert clipboard() == "가나다".encode()
            # xclip forks an owner process which keeps inherited pipe handles.
            pasted = subprocess.run(["xclip", "-selection", "clipboard", "-i",
                                     "-t", "UTF8_STRING"], input="각나다".encode(),
                                    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                                    timeout=5)
            assert pasted.returncode == 0
            xdo("key", "ctrl+v")
            time.sleep(0.15)
            xdo("key", "ctrl+s")
            assert read_document(directory, "unicode", 2) == "각나다".encode()
            xdo("key", "ctrl+s")
            assert read_document(directory, "unicode", 2) == "각나다".encode(), "no-op save changed revision"
        finally:
            stop_window(app)
        app, _ = start_window(directory, "unicode")
        try:
            assert clipboard() == "각나다".encode(), "new process did not restore the edit"
        finally:
            stop_window(app)

        # A real 1 MiB document crosses the GTK boundary, but an edit still
        # carries only the prepared two u32 fields and one changed byte.
        large = ((b"A" * (large_line_width - 1) + b"\n") *
                 (1_048_576 // large_line_width) +
                 b"A" * (1_048_576 % large_line_width))
        assert len(large) == 1_048_576
        insertion.write_bytes(large)
        initial = store_cli(directory, "edit", "large", "0", "0", "0", str(insertion))
        assert initial.returncode == 0, initial.stderr
        if check_key_switch:
            other_large = b"B" + large[1:]
            other_revision = 1
            insertion.write_bytes(other_large)
            assert store_cli(directory, "edit", "large_other", "0", "0", "0",
                             str(insertion)).returncode == 0
        switch_rss_samples = []
        switch_map_samples = []
        app, large_window = start_window(directory, "large")
        try:
            time.sleep(0.25)
            resident = rss_kib(app.pid)
            xdo("key", "ctrl+End")
            xdo("key", "BackSpace")
            xdo("type", "--clearmodifiers", "Z")
            xdo("key", "ctrl+s")
            stored = read_document(directory, "large", 2)
            assert stored == large[:-1] + b"Z"
            if check_key_switch:
                if switch_mode == 'same-body':
                    insertion.write_bytes(stored)
                    assert store_cli(directory, "edit", "large_other", "1", "0",
                                     str(len(other_large)), str(insertion)).returncode == 0
                    other_large = stored
                    other_revision = 2
                for step in range(switch_rounds):
                    key = "large" if switch_mode == 'same' else (
                        "large_other" if step % 2 == 0 else "large")
                    switch_document(large_window, key)
                    if (step + 1) % 8 == 0:
                        switch_rss_samples.append(rss_kib(app.pid))
                        if diagnose_maps:
                            switch_map_samples.append(rss_breakdown_kib(app.pid))
                assert read_document(directory, "large", 2) == stored
                assert read_document(directory, "large_other", other_revision) == other_large
                if settle_seconds:
                    time.sleep(settle_seconds)
                    settled_rss = rss_kib(app.pid)
                    settled_maps = rss_breakdown_kib(app.pid) if diagnose_maps else None
            if graceful_large_close:
                xdo("key", "ctrl+w")
                _, error = app.communicate(timeout=15)
                assert app.returncode == 0, (app.returncode, error[-4000:])
        finally:
            large_stderr = stop_window(app)

        # Author the same running document app in the native window. Failed
        # preparation and an external source conflict must preserve the draft.
        changed_definition = source_definition.replace(b"reply(saved, edit.insert);",
                                                       b"reply(saved);")
        assert changed_definition != source_definition
        app, window = start_window(directory, "authored")
        try:
            xdo("key", "ctrl+shift+d")
            time.sleep(0.25)
            pasted = subprocess.run(["xclip", "-selection", "clipboard", "-i",
                                     "-t", "UTF8_STRING"], input=changed_definition,
                                    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                                    timeout=5)
            assert pasted.returncode == 0
            xdo("key", "ctrl+a")
            xdo("key", "ctrl+v")
            xdo("key", "ctrl+shift+s")
            for _ in range(30):
                if PROGRAM.read_bytes() == changed_definition:
                    break
                time.sleep(0.1)
            assert PROGRAM.read_bytes() == changed_definition, "definition was not saved"
            assert "definition 2" in xdo("getwindowname", window)
            xdo("key", "ctrl+shift+d")
            xdo("type", "--clearmodifiers", "Z")
            xdo("key", "ctrl+s")
            assert read_document(directory, "authored", 1) == b"Z"
            assert "output 0 B" in xdo("getwindowname", window), "old reply plan stayed active"

            xdo("key", "ctrl+shift+d")
            cannot_save = changed_definition + b"\n// source write must fail\n"
            pasted = subprocess.run(["xclip", "-selection", "clipboard", "-i",
                                     "-t", "UTF8_STRING"], input=cannot_save,
                                    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                                    timeout=5)
            assert pasted.returncode == 0
            xdo("key", "ctrl+a")
            xdo("key", "ctrl+v")
            prior_mode = directory.stat().st_mode & 0o777
            os.chmod(directory, 0o500)
            try:
                xdo("key", "ctrl+shift+s")
            finally:
                os.chmod(directory, prior_mode)
            assert PROGRAM.read_bytes() == changed_definition, "failed source save changed the file"
            assert "definition 2" in xdo("getwindowname", window)
            assert clipboard() == cannot_save, "failed source save dropped the draft"

            partial_read = source_definition.replace(
                b"reply(document, document.body);",
                b"reply(document, range(document.body, 0, 1));")
            assert partial_read != source_definition
            pasted = subprocess.run(["xclip", "-selection", "clipboard", "-i",
                                     "-t", "UTF8_STRING"], input=partial_read,
                                    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                                    timeout=5)
            assert pasted.returncode == 0
            xdo("key", "ctrl+a")
            xdo("key", "ctrl+v")
            xdo("key", "ctrl+shift+s")
            assert PROGRAM.read_bytes() == changed_definition, "partial read made document editing unsafe"
            assert "definition 2" in xdo("getwindowname", window)

            invalid = b"not a Pure definition\n"
            pasted = subprocess.run(["xclip", "-selection", "clipboard", "-i",
                                     "-t", "UTF8_STRING"], input=invalid,
                                    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                                    timeout=5)
            assert pasted.returncode == 0
            xdo("key", "ctrl+a")
            xdo("key", "ctrl+v")
            xdo("key", "ctrl+shift+s")
            assert PROGRAM.read_bytes() == changed_definition
            assert "definition 2" in xdo("getwindowname", window)
            xdo("key", "ctrl+a")
            xdo("key", "ctrl+c")
            assert clipboard() == invalid, "invalid definition draft was discarded"

            local_draft = changed_definition + b"\n// local draft\n"
            external = source_definition + b"\n// external change\n"
            pasted = subprocess.run(["xclip", "-selection", "clipboard", "-i",
                                     "-t", "UTF8_STRING"], input=local_draft,
                                    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                                    timeout=5)
            assert pasted.returncode == 0
            xdo("key", "ctrl+a")
            xdo("key", "ctrl+v")
            PROGRAM.write_bytes(external)
            xdo("key", "ctrl+shift+s")
            assert PROGRAM.read_bytes() == external, "external definition was overwritten"
            assert "definition 2" in xdo("getwindowname", window)
            xdo("key", "ctrl+w")
            time.sleep(0.15)
            assert app.poll() is None, "unsaved definition draft closed without confirmation"
            xdo("key", "Escape")
            time.sleep(0.1)
            assert app.poll() is None
            xdo("mousemove", "--window", window, "680", "326")
            xdo("click", "1")  # Explicitly discard the source draft and reload disk.
            xdo("key", "ctrl+shift+s")
            assert PROGRAM.read_bytes() == external
            assert "definition 3" in xdo("getwindowname", window)
            xdo("key", "ctrl+shift+d")
            xdo("type", "--clearmodifiers", "Y")
            xdo("key", "ctrl+s")
            assert len(read_document(directory, "authored", 2)) == 2
            assert "output 1 B" in xdo("getwindowname", window), "reloaded definition did not run"
        finally:
            stop_window(app)

        app, window = start_window(directory, "author-restored")
        try:
            xdo("type", "--clearmodifiers", "R")
            xdo("key", "ctrl+s")
            assert read_document(directory, "author-restored", 1) == b"R"
            assert "output 1 B" in xdo("getwindowname", window), "saved definition did not restore"
        finally:
            stop_window(app)

        result = {"gtk_cli_write": "pass", f"{external_host}_conflict_preserves_draft": "pass",
                "unsaved_close_guard": "pass",
                "empty_document_restore": "pass",
                "utf8_boundary_and_restart": "pass", "unchanged_save_keeps_revision": "pass",
                "one_mib_document_one_byte_edit": "pass", "large_window_rss_kib": resident,
                "native_definition_apply_and_running_output": "pass",
                "definition_restores_in_new_window": "pass",
                "partial_read_definition_rejected": "pass",
                "source_write_failure_preserves_active_definition": "pass",
                "invalid_and_external_definition_preserve_draft": "pass",
                "reloaded_definition_installs_without_rewriting_source": "pass",
                "profile": "optional-native-document-window"}
        if check_key_switch:
            result["multi_document_switch_preserves_draft"] = "pass"
            result["large_switch_rss_samples_kib"] = switch_rss_samples
            result["large_switch_mode"] = switch_mode
            result["large_line_width"] = large_line_width
            if settle_seconds:
                result["large_switch_settled_rss_kib"] = settled_rss
                if diagnose_maps:
                    result["large_switch_settled_map_kib"] = settled_maps
            if diagnose_maps:
                result["large_switch_map_samples_kib"] = switch_map_samples
        allocations = re.findall(
            rb'PURE_MALLOC key=([^ ]+) arena=(\d+) used=(\d+) free=(\d+) mapped=(\d+)',
            large_stderr)
        if allocations:
            result['large_malloc_samples_bytes'] = [
                {'key': key.decode(), 'arena': int(arena), 'used': int(used),
                 'free': int(free), 'mapped': int(mapped)}
                for key, arena, used, free, mapped in allocations]
        return result


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--build", type=Path, default=BUILD)
    parser.add_argument("--program", type=Path, default=PROGRAM)
    parser.add_argument("--legacy-window", action="store_true",
                        help="check an archived window built before document switching existed")
    parser.add_argument("--switch-rounds", type=int, default=16,
                        help="number of alternating 1 MiB document opens, in groups of eight")
    parser.add_argument("--diagnose-maps", action="store_true",
                        help="record RSS by Linux mapping category after each eight switches")
    parser.add_argument("--switch-mode", choices=('alternate', 'same', 'same-body'),
                        default='alternate',
                        help="alternate two different or identical documents, or reopen one")
    parser.add_argument("--graceful-large-close", action="store_true",
                        help="exit the large-text window normally so leak detectors can run")
    parser.add_argument("--settle-seconds", type=float, default=0,
                        help="wait after the final switch, then sample RSS again")
    parser.add_argument("--large-line-width", type=int, default=80,
                        help="bytes per line in the 1 MiB fixture (default: 80)")
    args = parser.parse_args()
    if args.switch_rounds < 8 or args.switch_rounds > 128 or args.switch_rounds % 8:
        parser.error('--switch-rounds must be a multiple of eight between 8 and 128')
    if args.settle_seconds < 0 or args.settle_seconds > 30:
        parser.error('--settle-seconds must be between zero and 30')
    if args.large_line_width < 2 or args.large_line_width > 1_048_576:
        parser.error('--large-line-width must be between 2 and 1048576')
    BUILD = args.build.resolve()
    PROGRAM = args.program.resolve()
    result = check(not args.legacy_window, args.switch_rounds,
                   args.diagnose_maps, args.switch_mode, args.graceful_large_close,
                   args.settle_seconds, args.large_line_width)
    text = json.dumps(result, sort_keys=True) + "\n"
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(text)
    print(text, end="")
