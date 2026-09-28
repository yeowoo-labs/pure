#!/usr/bin/env python3
"""Pure's end-to-end checks: pure-serve, the HTTP host, running modules that
aup builds (the sibling checkout, --aup-root). The core's and the store's own
tests are core/test-module.c and store/test-tree-store.c.

1. Documents: pure-serve running the document editor written in aup
   (aup's examples/editor.aup) answers random requests exactly as the earlier
   document server running editor.pure, with one and with three connection
   threads, on persistent and pipelined connections too.
2. Lending: a read lends the stored extents and still carries its version
   while the document changes; lent answers count against the payload budget;
   a text view is lent while its answer waits and let go when it is read or
   its connection closes, the store keeping one mapping again.
3. Durability: concurrent edits share commits, and every acknowledged edit
   survives kill -9, with one and with three threads; so do the app's
   acknowledged posts (keyed lists, JSON bodies with groups).
4. JSON: request bodies are read in place, fields by path, and malformed or
   mistyped ones are refused with the field named.
5. Timers run on the owner and their beats are saved like events.
6. Links: sends are saved with their event and delivered in order and in
   batches to another app, which applies each once, across kill -9 of either
   side; webhooks receive them over HTTPS only from a trusted receiver; an
   API's answers become the app's events (--reply).
7. Sealed sends: sealed to the receiver's key, they pass a relay unread and
   unchanged (a changed byte is refused, a replay applies nothing), and a
   node that keeps its deliveries sealed refuses unsealed ones.
"""
import argparse
import json
import random
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
# The app that exercises documents: the editor written in aup, from aup's examples.
EDITOR = ROOT.parent / 'aup' / 'examples' / 'editor.aup'


def run(*args, stdin=None, check=True):
    return subprocess.run([str(a) for a in args], input=stdin, capture_output=True, text=True,
                          check=check, timeout=60)


def compile_module(aup, source_path, out):
    run(aup, 'build', source_path, '-o', out)
    return out.read_bytes()


class HttpHost:
    """A Pure HTTP host on a fresh port; stops with its statistics."""

    def __init__(self, args):
        import select
        self.process = subprocess.Popen([str(a) for a in args] + ['--port', '0'], stdout=subprocess.PIPE,
                                        stderr=subprocess.PIPE)
        ready, _, _ = select.select([self.process.stdout], [], [], 10)
        assert ready, 'HTTP host did not start'
        line = self.process.stdout.readline()
        assert line, self.process.communicate(timeout=5)
        self.port = json.loads(line)['port']

    def request(self, method, key, body=b'', expected=None):
        import http.client
        headers = {'Content-Length': str(len(body))}
        if expected is not None:
            headers['If-None-Match' if expected == 0 else 'If-Match'] = '*' if expected == 0 else f'"{expected}"'
        connection = http.client.HTTPConnection('127.0.0.1', self.port, timeout=30)
        try:
            connection.request(method, f'/objects/{key}', body, headers)
            response = connection.getresponse()
            return response.status, response.getheader('ETag'), response.getheader('X-Pure-Error'), response.read()
        finally:
            connection.close()

    def stop(self):
        self.process.terminate()
        _, error = self.process.communicate(timeout=30)
        assert self.process.returncode == 0, error[-1000:]
        return [json.loads(line) for line in error.decode().splitlines() if line.startswith('{')]


def server_parity(aup, pure, work, seeds, operations=150, threads=1):
    """The document server running editor.aup answers every request exactly
    as the server running editor.pure: status, revision, error and body."""
    import struct
    module = work / 'editor-server.pmod'
    compile_module(aup, EDITOR, module)
    compared = 0
    for seed in seeds:
        rng = random.Random(seed + 13000)
        old = HttpHost([pure / 'prototypes/connections/build/tree/pure-connections', '--program', pure / 'prototypes/connections/examples/editor.pure',
                        '--store', work / f'http-old-{seed}-{threads}'])
        new = HttpHost([pure / 'build/pure-serve', '--module', module, '--store', work / f'http-new-{seed}-{threads}',
                        '--threads', str(threads)])
        try:
            revision, size = {}, {}
            for _ in range(operations):
                key = rng.choice(['a', 'b', 'long-key_1'])
                current, length = revision.get(key, 0), size.get(key, 0)
                if rng.random() < 0.35:
                    request = ('GET', key, b'', None)
                else:
                    pick = rng.random()
                    base = current if pick < 0.75 else max(current - 1, 0) if pick < 0.9 else current + 3
                    if rng.random() < 0.05:
                        body = bytes(rng.randrange(256) for _ in range(rng.randint(0, 7)))  # too short
                    else:
                        insert = bytes(rng.randrange(256) for _ in range(rng.choice([0, 1, 7, 300, 5000])))
                        body = struct.pack('<II', rng.randint(0, length + 2), rng.choice([0, 1, 3, length])) + insert
                    request = ('PUT', key, body, base)
                theirs, ours = old.request(*request), new.request(*request)
                assert ours == theirs, (seed, request[:2], request[3], ours[:3], theirs[:3])
                if theirs[0] == 200 and theirs[1]:
                    revision[key] = int(theirs[1].strip('"'))
                    if request[0] == 'GET':
                        size[key] = len(theirs[3])
                    else:
                        size[key] = len(old.request('GET', key)[3])
                        assert new.request('GET', key) == old.request('GET', key)
                compared += 1
        finally:
            old.stop()
            new.stop()
    return compared


def server_pipelining(aup, pure, work):
    """One connection, many requests: a keep-alive client and a pipelined
    burst (all requests in one write, one split across writes) get the same
    answers, in order, as separate connections would."""
    import http.client
    import socket
    import struct
    module = work / 'editor-pipeline.pmod'
    compile_module(aup, EDITOR, module)
    host = HttpHost([pure / 'build/pure-serve', '--module', module, '--store', work / 'pipeline'])
    try:
        connection = http.client.HTTPConnection('127.0.0.1', host.port, timeout=30)
        answers = []
        for body, headers in [(struct.pack('<II', 0, 0) + b'hello', {'If-None-Match': '*'}),
                              (struct.pack('<II', 5, 0) + b'!', {'If-Match': '"1"'}),
                              (struct.pack('<II', 0, 0) + b'x', {'If-Match': '"1"'})]:
            connection.request('PUT', '/objects/p', body, headers)
            response = connection.getresponse()
            answers.append((response.status, response.getheader('ETag'), response.read()))
        connection.request('GET', '/objects/p')
        response = connection.getresponse()
        answers.append((response.status, response.getheader('ETag'), response.read()))
        connection.close()
        assert answers == [(200, '"1"', b'hello'), (200, '"2"', b'!'), (412, '"2"', b''), (200, '"2"', b'hello!')], answers

        def request(method, key, body=b'', condition=''):
            return (f'{method} /objects/{key} HTTP/1.1\r\nHost: x\r\nContent-Length: {len(body)}\r\n{condition}\r\n'
                    .encode() + body)
        pipeline = (request('GET', 'p') + request('PUT', 'q', struct.pack('<II', 0, 0) + b'q1', 'If-None-Match: *\r\n') +
                    request('GET', 'q') + request('GET', 'missing') +
                    request('PUT', 'q', struct.pack('<II', 2, 0) + b'!', 'If-Match: "1"\r\n') + request('GET', 'q'))
        expected = [(200, b'hello!'), (200, b'q1'), (200, b'q1'), (404, b''), (200, b'!'), (200, b'q1!')]
        for split in (len(pipeline), 7, 61):  # one write, and small writes that cut heads and bodies
            with socket.create_connection(('127.0.0.1', host.port), timeout=30) as sock:
                for at in range(0, len(pipeline), split):
                    sock.sendall(pipeline[at:at + split])
                reader = sock.makefile('rb')
                for status, body in expected:
                    code = int(reader.readline().split()[1])
                    length = 0
                    for line in iter(reader.readline, b'\r\n'):
                        name, _, value = line.decode().partition(':')
                        if name.lower() == 'content-length':
                            length = int(value)
                    assert (code, reader.read(length)) == (status, body), (split, code, status, body)
            # The next round starts from the same documents.
            host.request('PUT', 'q', struct.pack('<II', 0, 3) + b'q1', int(host.request('GET', 'q')[1].strip('"')))
            expected = [(200, b'hello!'), (412, b''), (200, b'q1'), (404, b''), (412, b''), (200, b'q1')]
            pipeline = (request('GET', 'p') + request('PUT', 'q', struct.pack('<II', 0, 0) + b'q1', 'If-None-Match: *\r\n') +
                        request('GET', 'q') + request('GET', 'missing') +
                        request('PUT', 'q', struct.pack('<II', 2, 0) + b'!', 'If-Match: "1"\r\n') + request('GET', 'q'))
    finally:
        stats = host.stop()
    assert stats[0]['active'] == 0 and stats[0]['retained_payload_bytes'] == 0, stats
    return len(answers) + 3 * len(expected)


def server_lent_reads(aup, pure, work):
    """A read lends the stored document's extents instead of copying them: a
    1 MiB answer sent slowly still carries the version read while the
    document is replaced five times meanwhile."""
    import socket
    import struct
    module = work / 'editor-lent.pmod'
    compile_module(aup, EDITOR, module)
    host = HttpHost([pure / 'build/pure-serve', '--module', module, '--store', work / 'lent'])
    big = bytes(range(256)) * 4096
    try:
        assert host.request('PUT', 'big', struct.pack('<II', 0, 0) + big, 0)[0] == 200
        with socket.create_connection(('127.0.0.1', host.port), timeout=30) as sock:
            sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4096)
            sock.sendall(b'GET /objects/big HTTP/1.1\r\nHost: x\r\nContent-Length: 0\r\n\r\n')
            data = sock.recv(100)
            for revision in range(1, 6):
                edit = struct.pack('<II', 0, len(big) if revision == 1 else 1) + b'Z'
                assert host.request('PUT', 'big', edit, revision)[0] == 200
            while b'\r\n\r\n' not in data or len(data) < data.index(b'\r\n\r\n') + 4 + len(big):
                chunk = sock.recv(65536)
                assert chunk, len(data)
                data += chunk
        assert data[data.index(b'\r\n\r\n') + 4:] == big
        assert host.request('GET', 'big') == (200, '"6"', 'none', b'Z')
    finally:
        stats = host.stop()
    assert stats[0]['active'] == 0 and stats[0]['retained_payload_bytes'] == 0, stats
    return len(big)


def server_lent_budget(aup, pure, work):
    """Lent extents count against the payload budget while an answer waits
    on its socket. Two readers pipeline 1 MiB reads without reading: the
    kernel takes what it can and each reader leaves one answer waiting. A
    third request finds no room (503 payload_budget) until a reader goes."""
    import socket
    import struct
    import time
    module = work / 'editor-budget.pmod'
    compile_module(aup, EDITOR, module)
    big = bytes(range(256)) * 4096
    budget = 2 * len(big) + len(big) // 2
    host = HttpHost([pure / 'build/pure-serve', '--module', module, '--store', work / 'lent-budget',
                     '--budget', budget])
    readers = []
    try:
        assert host.request('PUT', 'big', struct.pack('<II', 0, 0) + big, 0)[0] == 200
        for _ in range(2):
            sock = socket.create_connection(('127.0.0.1', host.port), timeout=30)
            sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4096)
            sock.sendall(b'GET /objects/big HTTP/1.1\r\nHost: x\r\nContent-Length: 0\r\n\r\n' * 32)
            readers.append(sock)
        time.sleep(0.3)
        refused = host.request('GET', 'big')
        assert refused[0] == 503 and refused[2] == 'payload_budget', refused[:3]
        readers.pop().close()
        for _ in range(100):
            answered = host.request('GET', 'big')
            if answered[0] == 200:
                break
            time.sleep(0.05)
        assert answered[0] == 200 and answered[3] == big, answered[:3]
    finally:
        for sock in readers:
            sock.close()
        stats = host.stop()
    assert stats[0]['active'] == 0 and stats[0]['retained_payload_bytes'] == 0, stats
    return budget


NOTE_SOURCE = """state note: text
input write = message {
  text: text
}
on write { note = write.text }
view page = note
view size = note.bytes
"""


def server_lent_views(aup, pure, work, threads=1):
    """A view that shows a text state as it is lends the state's stored
    extents. Answers pipelined on one connection and left waiting hold the
    store mapping they were read from while the note is replaced: each comes
    out whole in one version. When a reader goes without reading, the
    mappings its answers held are unmapped and nothing stays charged. With
    several threads the owner lends and a connection thread gives back."""
    import socket
    import time
    module, source = work / 'note.pmod', work / 'note.aup'
    source.write_text(NOTE_SOURCE)
    compile_module(aup, source, module)
    store = work / f'lent-views-{threads}'
    host = HttpHost([pure / 'build/pure-serve', '--module', module, '--store', store, '--threads', threads])

    def write(text):
        body = json.dumps({'text': text}).encode()
        import http.client
        connection = http.client.HTTPConnection('127.0.0.1', host.port, timeout=30)
        connection.request('POST', '/inputs/write', body, {'Content-Type': 'application/json'})
        assert connection.getresponse().status == 200
        connection.close()

    def mappings():
        # A compaction replaces the file: a mapping of the old one shows as "(deleted)".
        path = str((store / 'pure.tree').resolve())
        return sum(1 for line in open(f'/proc/{host.process.pid}/maps') if path in line)

    def settle(to):
        for _ in range(100):  # an answer's ticket is released on its thread
            if mappings() <= to: return mappings()
            time.sleep(0.02)
        return mappings()

    def pipeline(count):
        # Far more than the kernel's buffers take (a few MiB): an answer waits, lent.
        sock = socket.create_connection(('127.0.0.1', host.port), timeout=30)
        sock.sendall(b'GET /views/page HTTP/1.1\r\nHost: x\r\n\r\n' * count)
        time.sleep(0.3)
        return sock

    size = 1 << 20
    try:
        write('x' * size)
        assert http(host.port, 'GET', '/views/size')[3] == str(size).encode()
        alone = mappings()
        # Whole versions, however the note changes while answers wait.
        sock = pipeline(32)
        for round_ in range(5):
            write(chr(ord('A') + round_) * size)
        held = mappings()
        data = b''
        while data.count(b'HTTP/1.1 200') < 32 or len(data) < 32 * size:
            chunk = sock.recv(1 << 20)
            assert chunk, len(data)
            data += chunk
        sock.close()
        answers = [part.split(b'\r\n\r\n', 1) for part in data.split(b'HTTP/1.1 200 OK\r\n')[1:]]
        bodies = [body[:size] for _, body in answers]
        assert all(b'text/plain' in head for head, _ in answers) and bodies[0] == b'x' * size, [b[:1] for b in bodies]
        assert all(len(set(body)) == 1 and len(body) == size for body in bodies), [len(b) for b in bodies]
        after_reading = settle(alone)
        # A reader that goes without reading gives back what its answers held.
        sock = pipeline(32)
        for round_ in range(3):
            write(chr(ord('a') + round_) * size)
        held_again = mappings()
        sock.close()
        after_leaving = settle(alone)
        assert held > alone and held_again > alone and after_reading <= alone and after_leaving <= alone, \
            (alone, held, after_reading, held_again, after_leaving)
        assert http(host.port, 'GET', '/views/page')[3] == b'c' * size
    finally:
        stats = host.stop()
    assert stats[0]['active'] == 0 and stats[0]['retained_payload_bytes'] == 0, stats
    return {'alone': alone, 'while_lent': max(held, held_again), 'after': after_leaving}


def server_bursts(aup, pure, work, clients=8, edits=30, shards=1):
    """Concurrent edits share commits, and every acknowledged edit survives a
    kill -9 of the server."""
    import http.client
    import signal
    import struct
    import threading
    import time
    module = work / 'editor-bursts.pmod'
    compile_module(aup, EDITOR, module)

    def load(host, rounds, acknowledged):
        def client(number):
            key, base = f'c{number}', acknowledged[number][1]
            for _ in range(rounds):
                try:
                    status, tag, _, _ = host.request('PUT', key, struct.pack('<II', 0, 0) + b'x', base)
                except (OSError, http.client.HTTPException):  # the server died mid-answer: not acknowledged
                    return
                if status != 200:
                    return
                base = int(tag.strip('"'))
                acknowledged[number] = (acknowledged[number][0] + 1, base)
        threads = [threading.Thread(target=client, args=(n,)) for n in range(clients)]
        start = time.perf_counter()
        for thread in threads:
            thread.start()
        return threads, start

    timings = {}
    for name, args in [('program', [pure / 'prototypes/connections/build/tree/pure-connections', '--program', pure / 'prototypes/connections/examples/editor.pure']),
                       ('module', [pure / 'build/pure-serve', '--module', module, '--threads', str(shards)])]:
        host = HttpHost(args + ['--store', work / f'bursts-{name}-{shards}'])
        acknowledged = [(0, 0)] * clients
        threads, start = load(host, edits, acknowledged)
        for thread in threads:
            thread.join()
        timings[name] = time.perf_counter() - start
        assert all(count == edits for count, _ in acknowledged), acknowledged
        stats = host.stop()
        if name == 'module':
            bursts = stats[0]['bursts']
            assert stats[0]['burst_events'] == clients * edits and (clients == 1 or bursts < clients * edits), stats
    # Kill the module server while clients edit; what it acknowledged is stored.
    store = work / f'bursts-module-{shards}'
    host = HttpHost([pure / 'build/pure-serve', '--module', module, '--store', store, '--threads', str(shards)])
    acknowledged = [(edits, edits)] * clients
    threads, _ = load(host, 1000, acknowledged)
    time.sleep(0.5)
    host.process.send_signal(signal.SIGKILL)
    host.process.wait()
    for thread in threads:
        thread.join()
    host = HttpHost([pure / 'build/pure-serve', '--module', module, '--store', store, '--threads', str(shards)])
    try:
        for number, (count, base) in enumerate(acknowledged):
            status, tag, _, body = host.request('GET', f'c{number}')
            stored = int(tag.strip('"'))
            assert status == 200 and stored in (base, base + 1) and len(body) == stored, (number, count, base, tag)
    finally:
        host.stop()
    return {'writes': clients * edits, 'bursts': bursts, 'program_seconds': round(timings['program'], 3),
            'module_seconds': round(timings['module'], 3),
            'acknowledged_before_kill': sum(count for count, _ in acknowledged) - clients * edits}


SERVED_SOURCE = """record Entry {
  name: text
  points = 0
}
state board: list of Entry by name
state at: index of board
input score = message {
  points: u32
  name: bytes
}
input join = text
input drop = key Delete
on score {
  if !board.has(text(score.name)) { board.add(Entry { name: text(score.name) }) }
  board[text(score.name)].points = board[text(score.name)].points + score.points
  at = board[text(score.name)]
  reply score.name
}
on join {
  board.add(Entry { name: join })
  at = board[join]
}
on drop.down { board.remove(at) }
view total = sum(e.points for e in board)
view current = at.name
view leaders = each e in board {
  name = e.name
  points = e.points
}
"""


def http(port, method, path, body=b''):
    """One request on its own connection: status, X-Pure-Error, content type, body."""
    import http.client
    connection = http.client.HTTPConnection('127.0.0.1', port, timeout=30)
    try:
        connection.request(method, path, body, {'Content-Length': str(len(body))} if method != 'GET' else {})
        response = connection.getresponse()
        return response.status, response.getheader('X-Pure-Error'), response.getheader('Content-Type'), response.read()
    finally:
        connection.close()


JSON_SOURCE = """record Note {
  title: text
  body: text
  points = 0
}
state notes: list of Note by title
state counted = 0
input add = message {
  title: text
  points: number
  body: text
}
input count = message {
  n: u32
}
record Shipment {
  title: text
  city: text
  note: text
  zip = 0
}
state shipped: list of Shipment
input ship = message {
  title: text
  to: {
    city: text
    zip: u32
    door: {
      note: text
    }
  }
}
on add {
  notes.add(Note { title: add.title, body: add.body, points: add.points })
  reply add.title
}
on count { counted = counted + count.n }
on ship {
  shipped.append(Shipment { title: ship.title, city: ship.to.city, note: ship.to.door.note, zip: ship.to.zip })
  reply ship.to.door.note
}
view counted_view = counted
view all = each n in notes {
  title = n.title
  body = n.body
  points = n.points
}
view shipments = each s in shipped {
  title = s.title
  city = s.city
  note = s.note
  zip = s.zip
}
"""


def server_json(aup, pure, work, rounds=300):
    """Messages of text and number fields posted as JSON objects: strings
    come out as Python's json module reads them (escapes, control bytes,
    Korean, surrogate pairs), numbers as it reads them; a missing, unknown,
    doubled or mistyped field, broken JSON and a lone surrogate answer 400."""
    module, source = work / 'json.pmod', work / 'json.aup'
    source.write_text(JSON_SOURCE)
    compile_module(aup, source, module)
    host = HttpHost([pure / 'build/pure-serve', '--module', module, '--store', work / 'json-store'])
    rng = random.Random(77)
    pieces = ['a', 'Z', ' ', '"', '\\', '/', '\n', '\t', '\x01', '\x1f', 'é', '가', '😀', '\u2028', 'x' * 20]
    notes = {}

    def post(path, body, json_body=True):
        import http.client
        connection = http.client.HTTPConnection('127.0.0.1', host.port, timeout=30)
        try:
            headers = {'Content-Length': str(len(body))}
            if json_body: headers['Content-Type'] = 'application/json'
            connection.request('POST', path, body, headers)
            response = connection.getresponse()
            return response.status, response.getheader('X-Pure-Error'), response.read()
        finally:
            connection.close()

    try:
        for _ in range(rounds):
            title = ''.join(rng.choice(pieces) for _ in range(rng.randint(0, 6)))
            body = ''.join(rng.choice(pieces) for _ in range(rng.randint(0, 4)))
            points = rng.choice([0, 1, -2.5, 1e3, 3.25e-2, 12345678.5])
            members = [('title', title), ('points', points), ('body', body)]
            rng.shuffle(members)
            text = json.dumps(dict(members), ensure_ascii=rng.random() < 0.5)
            text = text.replace(': ', ':' + ' ' * rng.randint(0, 2)).replace(', ', ',\n' if rng.random() < 0.3 else ', ')
            got = post('/inputs/add', text.encode())
            if title in notes:
                assert got[:2] == (409, 'exists'), (text, got)
                continue
            assert got == (200, 'none', title.encode()), (text, got)
            notes[title] = (body, points)
        rows = json.loads(http(host.port, 'GET', '/views/all')[3])['items']
        want = [{'title': t, 'body': notes[t][0], 'points': notes[t][1]} for t in sorted(notes, key=lambda t: t.encode())]
        assert rows == want, [(a, b) for a, b in zip(rows, want) if a != b][:2]
        # The query gives fixed fields; a body without JSON fills a message's one variable field only.
        assert post('/inputs/count?n=3', b'', json_body=False)[:2] == (200, 'none')
        assert post('/inputs/count', b'{"n": 4}')[:2] == (200, 'none')
        assert http(host.port, 'GET', '/views/counted_view')[3] == b'7'
        assert post('/inputs/add?points=2', b'{"title": "q-from-query", "body": ""}')[:2] == (200, 'none')
        wrong = [
            (b'{"title": "m", "points": 1}', 'missing_field'),
            (b'{"title": "m", "points": 1, "body": "", "extra": 1}', 'json_field'),
            (b'{"title": "m", "title": "n", "points": 1, "body": ""}', 'json_field'),
            (b'{"title": 1, "points": 1, "body": ""}', 'json_type'),
            (b'{"title": "m", "points": "1", "body": ""}', 'json_type'),
            (b'{"title": "m", "points": 1e999, "body": ""}', 'json_type'),
            (b'{"title": "m", "points": 01, "body": ""}', 'json'),
            (b'{"title": "m", "points": NaN, "body": ""}', 'json'),
            (b'{"title": "m", "points": 1, "body": "",}', 'json'),
            (b'{"title": "\\ud800", "points": 1, "body": ""}', 'json'),
            (b'{"title": "\xff", "points": 1, "body": ""}', 'json'),
            (b'{"title": "m", "points": 1, "body": ""} x', 'json'),
            (b'[1]', 'json'),
            (b'{"title": "m\n", "points": 1, "body": ""}', 'json'),  # a raw newline in a string
        ]
        for body, error in wrong:
            assert post('/inputs/add', body)[:2] == (400, error), (body, post('/inputs/add', body))
        assert post('/inputs/add?points=2', b'{"title": "m", "points": 1, "body": ""}')[:2] == (400, 'json_field')
        assert post('/inputs/add', b'plain', json_body=False)[:2] == (400, 'json')  # two text fields need JSON
        assert post('/inputs/count', b'{"n": -1}')[:2] == (400, 'json_type')
        assert post('/inputs/count', b'{"n": 4294967296}')[:2] == (400, 'json_type')
        # Groups: objects inside the body, members in any order, strings
        # read where they are (escaped ones unescaped over themselves).
        shipments = []
        for _ in range(rounds // 3):
            title, city, note = (''.join(rng.choice(pieces) for _ in range(rng.randint(0, 5))) for _ in range(3))
            zip_code = rng.randint(0, 99999)
            door = [('note', note)]
            to = [('city', city), ('zip', zip_code), ('door', dict(door))]
            rng.shuffle(to)
            members = [('title', title), ('to', dict(to))]
            rng.shuffle(members)
            text = json.dumps(dict(members), ensure_ascii=rng.random() < 0.5)
            got = post('/inputs/ship', text.encode())
            assert got == (200, 'none', note.encode()), (text, got)
            shipments.append({'title': title, 'city': city, 'note': note, 'zip': zip_code})
        # A group's fixed field from the query, by its path.
        assert post('/inputs/ship?to.zip=7', b'{"title": "q", "to": {"city": "c", "door": {"note": "n"}}}')[:2] == \
            (200, 'none')
        shipments.append({'title': 'q', 'city': 'c', 'note': 'n', 'zip': 7})
        rows = json.loads(http(host.port, 'GET', f'/views/shipments?count={len(shipments)}')[3])['items']
        assert rows == shipments, [(a, b) for a, b in zip(rows, shipments) if a != b][:2]
        grouped = '{"title": "t", "to": {"city": "c", "zip": 1, "door": {"note": "n"}}}'
        wrong_groups = [
            (grouped.replace('"zip": 1', '"zip": 1, "lat": 2'), 'json_field'),         # unknown member of a group
            (grouped.replace('"note": "n"', '"note": "n", "bell": {"x": 1}'), 'json_field'),
            (grouped.replace('"door": {"note": "n"}', '"door": "n"'), 'json_type'),    # a group as a value
            (grouped.replace('"title": "t"', '"title": {"a": "t"}'), 'json_type'),    # a field as a group
            (grouped.replace('"door": {"note": "n"}', '"door": {}'), 'missing_field'),
            (grouped.replace('"zip": 1', '"zip": 1, "zip": 2'), 'json_field'),
            (grouped.replace('"zip": 1', '"zip": [1]'), 'json_type'),
            (grouped.replace('"zip": 1', '"zip": null'), 'json_type'),
            (grouped.replace('"note": "n"', '"note": "n\\ud800"'), 'json'),
            (grouped.replace('"door": {"note": "n"}}', '"door": {"note": "n"}'), 'json'),  # unclosed
        ]
        for body, error in wrong_groups:
            assert post('/inputs/ship', body.encode())[:2] == (400, error), (body, post('/inputs/ship', body.encode()))
        assert post('/inputs/ship?to.zip=7', grouped.encode())[:2] == (400, 'json_field')  # given twice
    finally:
        host.stop()
    return rounds + len(wrong) + rounds // 3 + len(wrong_groups)


LINK_SENDER = """state n = 0
state total = 0
input buy = message {
  item: text
  count: u32
}
output ordered = message {
  item: text
  count: u32
  to: {
    city: text
  }
}
on buy {
  n = n + 1
  total = total + buy.count
  send ordered { item: buy.item, count: buy.count, to: { city: "Seoul" } }
}
view sent = n
view sent_total = total
"""

LINK_RECEIVER = """state stock = 0
state orders = 0
input order = message {
  item: text
  count: u32
  to: {
    city: text
  }
}
on order {
  require order.count < 100
  stock = stock + order.count
  orders = orders + 1
}
view total = stock
view count = orders
"""


def server_links(aup, pure, work):
    """Two served apps, one's output wired to the other's input over HTTP
    (--send). While four clients buy from the sender, the receiver and then
    the sender are killed (-9) and started again on their stores. In the end
    the receiver has applied every order the sender saved exactly once (its
    total and count equal the sender's), a refused order (count 150) was
    delivered once and did not hold back the order after it, and every
    acknowledged order is among the saved ones."""
    import select
    import signal
    import socket
    import threading
    import time
    sender_module, receiver_module = work / 'link-sender.pmod', work / 'link-receiver.pmod'
    for module, text in ((sender_module, LINK_SENDER), (receiver_module, LINK_RECEIVER)):
        source = module.with_suffix('.aup')
        source.write_text(text)
        compile_module(aup, source, module)

    def free_port():
        with socket.socket() as probe:
            probe.bind(('127.0.0.1', 0))
            return probe.getsockname()[1]

    def start(args, port):
        process = subprocess.Popen([str(a) for a in args] + ['--port', str(port)], stdout=subprocess.PIPE,
                                   stderr=subprocess.PIPE)
        ready, _, _ = select.select([process.stdout], [], [], 10)
        assert ready and process.stdout.readline(), 'host did not start'
        return process

    def post(port, path, body):
        import http.client
        connection = http.client.HTTPConnection('127.0.0.1', port, timeout=10)
        try:
            data = json.dumps(body).encode()
            connection.request('POST', path, data, {'Content-Length': str(len(data)), 'Content-Type': 'application/json'})
            response = connection.getresponse()
            response.read()
            return response.status
        finally:
            connection.close()

    def number(port, view):
        return int(http(port, 'GET', f'/views/{view}')[3])

    port_a, port_b = free_port(), free_port()
    receiver = [pure / 'build/pure-serve', '--module', receiver_module, '--store', work / 'link-b', '--threads', '2']
    sender = [pure / 'build/pure-serve', '--module', sender_module, '--store', work / 'link-a',
              '--send', f'ordered=http://127.0.0.1:{port_b}/inputs/order']
    hosts = {'b': start(receiver, port_b), 'a': start(sender, port_a)}
    acknowledged = [0] * 4
    stop = threading.Event()

    def client(which):
        rng = random.Random(which + 7700)
        while not stop.is_set():
            count = rng.randint(1, 20)
            try:
                status = post(port_a, '/inputs/buy', {'item': f'c{which}', 'count': count})
            except OSError:
                time.sleep(0.02)
                continue
            if status == 200:
                acknowledged[which] += count
            else:
                time.sleep(0.02)

    clients = [threading.Thread(target=client, args=(n,)) for n in range(len(acknowledged))]
    for thread in clients:
        thread.start()
    try:
        for name, args, port in (('b', receiver, port_b), ('a', sender, port_a)):
            time.sleep(0.6)
            hosts[name].send_signal(signal.SIGKILL)
            hosts[name].wait()
            time.sleep(0.3)
            hosts[name] = start(args, port)
        time.sleep(0.6)
    finally:
        stop.set()
        for thread in clients:
            thread.join()
    try:
        assert post(port_a, '/inputs/buy', {'item': 'big', 'count': 150}) == 200
        assert post(port_a, '/inputs/buy', {'item': 'after', 'count': 7}) == 200
        deadline = time.time() + 30
        while True:
            sent, sent_total = number(port_a, 'sent'), number(port_a, 'sent_total')
            count, total = number(port_b, 'count'), number(port_b, 'total')
            if (count, total) == (sent - 1, sent_total - 150) or time.time() > deadline:
                break
            time.sleep(0.1)
        assert (count, total) == (sent - 1, sent_total - 150), (count, total, sent, sent_total)
        assert sent_total >= sum(acknowledged) + 157 and sum(acknowledged) > 0, (sent_total, acknowledged)
    finally:
        stats = {}
        for name, process in hosts.items():
            process.terminate()
            _, error = process.communicate(timeout=30)
            assert process.returncode == 0, error[-1000:]
            stats[name] = [json.loads(line) for line in error.decode().splitlines() if line.startswith('{"active"')][-1]
    return {'orders': sent, 'acknowledged_total': sum(acknowledged), 'delivered_after_restart': stats['a']['delivered'],
            'attempts_after_restart': stats['a']['delivery_attempts']}


def server_sealed(aup, pure, work):
    """Sends sealed end to end (--seal to the receiver's --key), in one session
    a wire and authenticated by the sender's own --key: through a relay that
    records what passes, every order is applied once, the relay saw no item
    and no field name, and the wire's requests share one session, numbered
    in order. One byte changed on the way, another number or another
    sender's key is refused (400 seal) and applies nothing; the recorded
    request sent again applies nothing twice; an unsealed delivery is
    refused (403) by a node that keeps its deliveries sealed; a seal to
    another key does not open. Restarted naming its sender (--sender), the
    receiver opens that sender's session again unasked, and refuses another
    node claiming the sender's name with its own key (403 sender)."""
    import select
    import threading
    import time
    from http.client import HTTPConnection
    from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
    sender_module, receiver_module = work / 'seal-sender.pmod', work / 'seal-receiver.pmod'
    for module, text in ((sender_module, LINK_SENDER), (receiver_module, LINK_RECEIVER)):
        source = module.with_suffix('.aup')
        source.write_text(text)
        compile_module(aup, source, module)

    def start(args):
        """A host on a port of its own choosing (--port 0: no port probed
        free and then taken by another socket before it binds)."""
        process = subprocess.Popen([str(a) for a in args] + ['--port', '0'], stdout=subprocess.PIPE,
                                   stderr=subprocess.PIPE)
        ready, _, _ = select.select([process.stdout], [], [], 10)
        assert ready, 'host did not start'
        line = process.stdout.readline()
        assert line, process.communicate()[1][-1000:]
        return process, json.loads(line)

    def request(port, headers, body, path='/inputs/order'):
        connection = HTTPConnection('127.0.0.1', port, timeout=10)
        try:
            connection.request('POST', path, body, headers)
            response = connection.getresponse()
            response.read()
            return response.status, response.getheader('X-Pure-Error')
        finally:
            connection.close()

    def buy(port, item, count):
        assert request(port, {'Content-Type': 'application/json'}, json.dumps({'item': item, 'count': count}).encode(),
                       '/inputs/buy')[0] == 200

    def wait_for(condition, seconds=20):
        deadline = time.time() + seconds
        while not condition() and time.time() < deadline:
            time.sleep(0.05)
        return condition()

    def count():
        return int(http(port_b, 'GET', '/views/count')[3])

    key = work / 'seal-receiver.key'
    receiver, started = start([pure / 'build/pure-serve', '--module', receiver_module, '--store', work / 'seal-b',
                               '--key', key, '--sealed-only'])
    port_b, public_key = started['port'], started['public_key']
    assert len(public_key) == 64 and (key.stat().st_mode & 0o777) == 0o600, (public_key, oct(key.stat().st_mode))
    seen = []  # (headers, body, status, X-Pure-Error) of each request relayed

    class Relay(BaseHTTPRequestHandler):
        protocol_version = 'HTTP/1.1'

        def do_POST(self):
            body = self.rfile.read(int(self.headers['Content-Length']))
            headers = {name: value for name, value in self.headers.items()
                       if name.lower().startswith('x-pure-') or name.lower() in ('content-type', 'idempotency-key')}
            connection = HTTPConnection('127.0.0.1', port_b, timeout=10)
            connection.request('POST', self.path, body, headers)
            answer = connection.getresponse()
            data = answer.read()
            seen.append((headers, body, answer.status, answer.getheader('X-Pure-Error')))
            self.send_response(answer.status)
            for name, value in answer.getheaders():
                if name.lower().startswith('x-pure-'):
                    self.send_header(name, value)
            self.send_header('Content-Length', str(len(data)))
            self.end_headers()
            self.wfile.write(data)
            connection.close()

        def log_message(self, *args):
            pass

    relay = ThreadingHTTPServer(('127.0.0.1', 0), Relay)
    port_relay = relay.server_address[1]
    threading.Thread(target=relay.serve_forever, daemon=True).start()
    wire = f'ordered=http://127.0.0.1:{port_relay}/inputs/order'
    sender, sender_started = start([pure / 'build/pure-serve', '--module', sender_module, '--store', work / 'seal-a',
                                    '--key', work / 'seal-sender.key', '--send', wire, '--seal', f'ordered={public_key}'])
    port_a, sender_key = sender_started['port'], sender_started['public_key']
    stranger = impostor = None
    try:
        orders, total = 60, 0
        for i in range(orders):
            total += i % 7 + 1
            buy(port_a, f'secret-{i}', i % 7 + 1)
        assert wait_for(lambda: count() == orders), count()
        assert int(http(port_b, 'GET', '/views/total')[3]) == total
        seals = [headers['X-Pure-Seal'].split() for headers, _, _, _ in seen]
        assert seen and all(len(parts) == 4 and parts[3] == sender_key for parts in seals), seals[:1]
        assert len({parts[1] for parts in seals}) == 1  # one session
        numbers = [int(parts[2]) for parts in seals]
        assert numbers == list(range(len(numbers))), numbers
        assert all(status == 200 for _, _, status, _ in seen)
        assert not any(b'secret' in body or b'item' in body or b'city' in body for _, body, _, _ in seen)
        headers, body, _, _ = seen[0]
        suite, enc, n, _ = headers['X-Pure-Seal'].split()
        changed = bytearray(body)
        changed[len(changed) // 2] ^= 1
        assert request(port_b, headers, bytes(changed)) == (400, 'seal')
        renumbered = dict(headers, **{'X-Pure-Seal': f'{suite} {enc} {int(n) + 1} {sender_key}'})
        assert request(port_b, renumbered, body) == (400, 'seal')
        other_sender = f'{suite} {enc} {n} {public_key}'
        assert request(port_b, dict(headers, **{'X-Pure-Seal': other_sender}), body) == (400, 'seal')
        assert request(port_b, headers, body)[0] == 200  # heard before: applied once
        plain = {name: value for name, value in headers.items() if name != 'X-Pure-Seal'}
        assert request(port_b, plain, json.dumps({'item': 'plain', 'count': 1, 'to': {'city': 'x'}}).encode()) == \
            (403, 'unsealed')
        assert count() == orders
        # A sender sealing to another node's key: the receiver cannot open it.
        other, other_started = start([pure / 'build/pure-serve', '--module', receiver_module, '--store', work / 'seal-c',
                                      '--key', work / 'seal-other.key'])
        other.terminate()
        other.wait()
        stranger, stranger_started = start([pure / 'build/pure-serve', '--module', sender_module, '--store',
                                            work / 'seal-d', '--send', wire, '--seal',
                                            f'ordered={other_started["public_key"]}'])
        before = len(seen)
        buy(stranger_started['port'], 'stranger', 5)
        assert wait_for(lambda: len(seen) > before, 5)
        assert seen[before][2:] == (400, 'seal') and len(seen[before][0]['X-Pure-Seal'].split()) == 3
        assert count() == orders
        # Restarted naming its sender: the same key opens the sender's session
        # again, and another node claiming the sender's name is refused.
        source = seen[0][0]['X-Pure-Source']
        receiver.terminate()
        receiver.wait()
        receiver, again = start([pure / 'build/pure-serve', '--module', receiver_module, '--store', work / 'seal-b',
                                 '--key', key, '--sender', f'{source}={sender_key}'])
        port_b = again['port']
        assert again['public_key'] == public_key
        before = len(seen)
        buy(port_a, 'after', 2)
        assert wait_for(lambda: count() == orders + 1), count()
        assert seen[-1][0]['X-Pure-Seal'].split()[1] == enc  # the same session, opened again unasked
        impostor, impostor_started = start([pure / 'build/pure-serve', '--module', sender_module, '--store',
                                            work / 'seal-e', '--key', work / 'seal-impostor.key', '--send', wire,
                                            '--seal', f'ordered={public_key}'])
        before = len(seen)
        buy(impostor_started['port'], 'impostor', 9)
        assert wait_for(lambda: any(h['X-Pure-Seal'].split()[3] == impostor_started['public_key']
                                    for h, _, _, _ in seen[before:]), 5)
        claimed = [entry for entry in seen[before:] if entry[0]['X-Pure-Seal'].split()[3] != sender_key]
        assert claimed and claimed[0][0]['X-Pure-Source'] == source and claimed[0][2:] == (403, 'sender'), claimed[:1]
        assert count() == orders + 1
        return {'orders': orders, 'relayed': len(seen), 'suite': suite, 'sessions': 1}
    finally:
        for process in (sender, receiver, stranger, impostor):
            if process:
                process.terminate()
                process.wait()
        relay.shutdown()


SURFACE_SOURCE = """record Task {
  title: text
  done = 0
}
state tasks: list of Task
state at: index of tasks
input typed = text
input toggle = key Space
on typed {
  tasks.append(Task { title: typed })
  at = tasks.last
}
on toggle.down { at.done = !at.done }
view count = tasks.count
view current = if tasks.count > 0 { at.title } else { "(empty)" }
view finished = at.done
"""


def server_surface(aup, pure, work):
    """A surface in the node (--surface, serve/surface.h): a library that
    pure-serve maps and runs beside its connection threads types text and
    presses a key as a person at the node's display would. The owner applies
    its events with the requests, one save a round, and it reads each save's
    views where they are: its events show in HTTP views, and an HTTP input
    shows in it through the changes descriptor. An event for an input of
    another kind, or for none, is refused; it stops with the server; and a
    module without states cannot have one."""
    import select
    import signal
    import time
    probe = work / 'surface-probe.so'
    subprocess.run(['cc', '-O2', '-std=c17', '-Wall', '-Wextra', '-Werror', '-shared', '-fPIC', f'-I{pure / "core"}',
                    f'-I{pure / "serve"}', str(pure / 'tests' / 'surface-probe.c'), '-o', str(probe)], check=True)
    source, module = work / 'surface.aup', work / 'surface.pmod'
    source.write_text(SURFACE_SOURCE)
    compile_module(aup, source, module)
    out = work / 'surface.jsonl'
    process = subprocess.Popen([str(a) for a in (pure / 'build/pure-serve', '--module', module, '--store',
                                                 work / 'surface-store', '--port', '0', '--surface', probe, '--', out,
                                                 'typed', 'toggle', 'one,two,three', 'count', 'current', 'finished')],
                               stdout=subprocess.PIPE, stderr=subprocess.PIPE)

    def lines():
        return [json.loads(line) for line in out.read_text().splitlines()] if out.exists() else []

    def wait_for(condition):
        deadline = time.time() + 20
        while not condition() and time.time() < deadline:
            time.sleep(0.05)
        return condition()

    try:
        ready, _, _ = select.select([process.stdout], [], [], 10)
        assert ready, 'host did not start'
        port = json.loads(process.stdout.readline())['port']
        assert wait_for(lambda: any(line['event'].startswith('wrong kind') for line in lines())), lines()
        assert http(port, 'GET', '/views/count')[3] == b'3' and http(port, 'GET', '/views/current')[3] == b'three'
        assert http(port, 'POST', '/inputs/typed', b'four')[0] == 200
        assert wait_for(lambda: any(line['event'] == 'changed' and line['views']['count'] == 4 for line in lines()))
    finally:
        process.send_signal(signal.SIGTERM)
        _, error = process.communicate(timeout=30)
    assert process.returncode == 0 and b'"returned":0' in error, error[-500:]
    got = lines()
    views = [line['views'] for line in got]
    assert [line['event'] for line in got[:6]] == ['start', 'typed one: 0', 'typed two: 0', 'typed three: 0', 'pressed: 0',
                                                    'wrong kind: 8, no input: 8'], got[:6]
    assert views[:6] == [{'count': 0, 'current': '(empty)', 'finished': None},
                         {'count': 1, 'current': 'one', 'finished': 0}, {'count': 2, 'current': 'two', 'finished': 0},
                         {'count': 3, 'current': 'three', 'finished': 0}, {'count': 3, 'current': 'three', 'finished': 1},
                         {'count': 3, 'current': 'three', 'finished': 1}], views[:6]
    assert views[-1] == {'count': 4, 'current': 'four', 'finished': 0}, views[-1]
    changes = [line['changes'] for line in got]
    assert changes[1:5] == sorted(set(changes[1:5])) and changes[-1] > changes[4], changes
    # A module without states has no owner to apply a surface's events.
    stateless = work / 'surface-echo.aup'
    stateless.write_text('input shout = text\non shout { }\n')
    compile_module(aup, stateless, stateless.with_suffix('.pmod'))
    refused = subprocess.run([str(a) for a in (pure / 'build/pure-serve', '--module', stateless.with_suffix('.pmod'),
                                               '--store', work / 'surface-echo', '--port', '0', '--surface', probe, '--',
                                               out, 'shout', 'shout', 'x')], capture_output=True, timeout=30)
    assert refused.returncode == 2 and b'needs an app with states' in refused.stderr, refused.stderr
    return {'events': 5, 'lines': len(got)}


SHOP = '''
record Stock {
  item = 0
  left = 0
}
record Line {
  item = 0
  qty = 1
  note: text
}
record Entry {
  item = 0
  qty = 0
  note: text
}
state stock: list of Stock by item
state log: list of Entry
state orders = 0

input restock = message {
  rows: list of Stock
}
input order = message {
  customer: u32
  lines: list of Line
}
input clean = message {
  below: number
}

on restock {
  for r in restock.rows {
    stock.add(Stock { item: r.item, left: r.left })
  }
}
on order {
  for l in order.lines {
    stock[l.item].left = stock[l.item].left - l.qty
    require stock[l.item].left >= 0
    log.append(Entry { item: l.item, qty: l.qty, note: l.note })
  }
  orders = orders + order.lines.count
}
on clean {
  for s in stock where s.left < clean.below {
    stock.remove(s)
  }
}
view items = stock.count
view left = sum(s.left for s in stock)
view logged = log.count
view total = sum(e.qty for e in log)
view lines = orders
view notes = each e in log {
  item = e.item
  note = e.note
}
'''


def server_rows(aup, pure, work):
    """A message's rows (a list of a record: a JSON array of objects, or the
    list's stored form as the body) walked with for, all or nothing: 30,000
    stock rows in one request, then 20,000 as bytes, then random orders whose
    rows take stock (an item not stocked or not enough of it fails the whole
    order, none of its rows applied), a walk that removes, and a restart that
    keeps every state and no rows. Checked against a model."""
    import http.client
    import random
    import struct
    import time
    source, module = work / 'shop.aup', work / 'shop.pmod'
    source.write_text(SHOP)
    compile_module(aup, source, module)
    store = work / 'shop-store'
    host = HttpHost([pure / 'build/pure-serve', '--module', module, '--store', store])

    def call(method, path, body=None, kind='application/json'):
        connection = http.client.HTTPConnection('127.0.0.1', host.port, timeout=60)
        try:
            connection.request(method, path, body, {'Content-Type': kind} if body is not None else {})
            answer = connection.getresponse()
            return answer.status, answer.getheader('X-Pure-Error'), answer.read()
        finally:
            connection.close()

    def number(view):
        status, _, data = call('GET', f'/views/{view}')
        assert status == 200, (view, status)
        return float(data)

    stock, log = {}, []
    try:
        began = time.perf_counter()
        rows = [{'item': i, 'left': i % 7} for i in range(30000)]
        assert call('POST', '/inputs/restock', json.dumps({'rows': rows}).encode())[:2] == (200, 'none')
        seconds = time.perf_counter() - began
        stock.update({i: i % 7 for i in range(30000)})
        packed = b''.join(struct.pack('<dd', i, 5) for i in range(30000, 50000))
        assert call('POST', '/inputs/restock', packed, 'application/octet-stream')[:2] == (200, 'none')
        stock.update({i: 5 for i in range(30000, 50000)})
        assert number('items') == 50000 and number('left') == sum(stock.values())
        # All or nothing: a row whose item is there already fails the whole message.
        assert call('POST', '/inputs/restock', json.dumps({'rows': [{'item': 60000}, {'item': 7}]}).encode())[:2] == \
            (409, 'exists')
        assert call('POST', '/inputs/restock', packed[:40], 'application/octet-stream')[:2] == (422, 'limit')
        assert call('POST', '/inputs/restock', json.dumps({'rows': [{'item': 1, 'lft': 2}]}).encode())[:2] == \
            (400, 'json_field')
        assert call('POST', '/inputs/restock', json.dumps({'rows': [{'item': 'one'}]}).encode())[:2] == (400, 'json_type')
        assert number('items') == 50000
        rng = random.Random(21)
        notes = ['', 'fast', '빨리', 'a "quoted" \\ note', 'tab\there', '😀']
        applied = refused = 0
        for _ in range(400):
            lines = [{'item': rng.randrange(50010), 'qty': rng.randrange(4), 'note': rng.choice(notes)}
                     for _ in range(rng.randrange(6))]
            if lines and rng.random() < 0.3:
                del lines[0]['qty']  # a field left out takes its default (1)
            model = dict(stock)
            ok = True
            for line in lines:
                qty = line.get('qty', 1)
                if line['item'] not in model or model[line['item']] - qty < 0:
                    ok = False
                    break
                model[line['item']] -= qty
            status, error, _ = call('POST', '/inputs/order', json.dumps({'customer': 3, 'lines': lines}).encode())
            assert (status == 200) == ok, (status, error, lines)
            if ok:
                stock = model
                log.extend((line['item'], line.get('qty', 1), line['note']) for line in lines)
                applied += 1
            else:
                refused += 1
        assert number('logged') == len(log) and number('total') == sum(q for _, q, _ in log)
        assert number('left') == sum(stock.values()) and number('lines') == len(log)
        shown = json.loads(call('GET', f'/views/notes?from={max(0, len(log) - 50)}')[2])['items']
        assert [(r['item'], r['note']) for r in shown] == [(i, n) for i, _, n in log[-50:]]
        assert call('POST', '/inputs/clean', json.dumps({'below': 2}).encode())[:2] == (200, 'none')
        stock = {item: left for item, left in stock.items() if left >= 2}
        assert number('items') == len(stock) and number('left') == sum(stock.values())
    finally:
        host.stop()
    # Restarted: the states as they were; rows are never kept, and come again.
    host = HttpHost([pure / 'build/pure-serve', '--module', module, '--store', store])
    try:
        assert number('items') == len(stock) and number('logged') == len(log) and number('lines') == len(log)
        assert call('POST', '/inputs/restock', json.dumps({'rows': [{'item': 90000, 'left': 1}]}).encode())[:2] == \
            (200, 'none')
        assert number('items') == len(stock) + 1
    finally:
        host.stop()
    assert applied > 50 and refused > 50
    return {'bulk_rows_json': 30000, 'bulk_rows_bytes': 20000, 'bulk_rows_per_second': round(30000 / seconds),
            'orders_applied': applied, 'orders_refused': refused, 'logged': len(log)}


ARGUED = '''
record Account {
  aid = 0
  bid = 0
  balance = 0
  owner: text
}
state accounts: list of Account by aid
input open = message {
  aid: u32
  bid: u32
  balance: number
  owner: text
}
on open { accounts.add(Account { aid: open.aid, bid: open.bid, balance: open.balance, owner: open.owner }) }

view balance(aid: u32) = accounts[aid].balance
view richer(low: number) = count(a in accounts where a.balance > low)
view hello(name: text, times: u32) = "{name} x{times}"
view branch(b: u32, over: number) = each a in accounts where a.bid == b && a.balance > over {
  aid = a.aid
  share = a.balance / sum(x.balance for x in accounts where x.bid == b)
  owner = a.owner
}
view owned(who: text) = count(a in accounts where a.owner == who)
'''


def server_arguments(aup, pure, work):
    """Views that take arguments, given as the request's query (values %XX
    decoded): a keyed lookup, a count above an argument, texts shown back,
    a table filtered by two arguments whose column folds with one of them,
    paged; a registered query that takes one. Every answer against a model;
    an argument missing, unknown or not of its type is 400 invalid."""
    import http.client
    import random
    import urllib.parse
    source, module = work / 'argued.aup', work / 'argued.pmod'
    source.write_text(ARGUED)
    compile_module(aup, source, module)
    host = HttpHost([pure / 'build/pure-serve', '--module', module, '--store', work / 'argued-store'])

    def call(method, path, body=None):
        connection = http.client.HTTPConnection('127.0.0.1', host.port, timeout=30)
        try:
            connection.request(method, path, body, {'Content-Type': 'application/json'} if body is not None else {})
            answer = connection.getresponse()
            return answer.status, answer.getheader('X-Pure-Error'), answer.read()
        finally:
            connection.close()

    rng = random.Random(5)
    owners = ['김철수', 'bob', 'ann marie', 'a&b=c', '100%']
    accounts = {}
    asked = 0
    try:
        for aid in range(1, 201):
            account = (aid % 7, rng.randrange(-500, 5000), rng.choice(owners))
            accounts[aid] = account
            body = json.dumps({'aid': aid, 'bid': account[0], 'balance': account[1], 'owner': account[2]}).encode()
            assert call('POST', '/inputs/open', body)[0] == 200
        for aid in rng.sample(range(1, 260), 60):
            status, error, data = call('GET', f'/views/balance?aid={aid}')
            assert (status, float(data)) == (200, accounts[aid][1]) if aid in accounts else (status, error) == (404, 'no_value')
            asked += 1
        for low in [-1000, -1, 0, 0.5, 1000, 2500.25, 6000]:
            assert float(call('GET', f'/views/richer?low={low}')[2]) == sum(1 for _, b, _ in accounts.values() if b > low)
            asked += 1
        for name in ['세계 world', 'a&b=c', '100% sure', '😀']:
            status, _, data = call('GET', f'/views/hello?name={urllib.parse.quote(name)}&times=3')
            assert (status, data.decode()) == (200, f'{name} x3')
            asked += 1
        for who in owners:
            data = call('GET', f'/views/owned?who={urllib.parse.quote(who)}')[2]
            assert float(data) == sum(1 for _, _, o in accounts.values() if o == who)
            asked += 1
        for b in range(7):
            over = rng.choice([-1000, 0, 1500])
            rows = [(aid, balance, owner) for aid, (bid, balance, owner) in sorted(accounts.items()) if bid == b and balance > over]
            total = sum(balance for bid, balance, _ in accounts.values() if bid == b)
            page = json.loads(call('GET', f'/views/branch?b={b}&over={over}&from=2&count=5')[2])
            assert page['rows'] == len(rows) and page['from'] == min(2, len(rows))
            assert [(r['aid'], r['share'], r['owner']) for r in page['items']] == \
                [(aid, balance / total, owner) for aid, balance, owner in rows[2:7]]
            asked += 1
        # Arguments missing, unknown, not of their type, or badly escaped.
        for path in ['/views/balance', '/views/balance?aid=x', '/views/balance?aid=1&zz=2', '/views/balance?aid=-1',
                     '/views/hello?times=1&name=%zz', '/views/richer?low=1&low=2']:
            assert call('GET', path)[:2] == (400, 'invalid'), path
        assert call('GET', '/views/branch?b=1&from=x')[:2] == (400, 'query')
        # A registered query takes arguments as a view does.
        (work / 'top.aup').write_text('view top(k: u32) = each a in accounts where a.aid <= k {\n  aid = a.aid\n}\n')
        run(aup, 'query', source, work / 'top.aup', '-o', work / 'top.pmod')
        assert call('PUT', '/queries/top', (work / 'top.pmod').read_bytes())[0] == 200
        assert [r['aid'] for r in json.loads(call('GET', '/queries/top?k=4')[2])['items']] == [1, 2, 3, 4]
        assert call('GET', '/queries/top')[:2] == (400, 'invalid')
    finally:
        host.stop()
    return {'accounts': len(accounts), 'asked': asked}


def server_queries(aup, pure, work):
    """Queries as verified programs over a snapshot (PUT and GET /queries/NAME):
    aup query compiles a bank's declarations with views of its own; the node
    checks the module when it is registered (it loads, its states are the
    bank's, it has the view named) and shows that view from the last save's
    snapshot. A per-branch total (a fold reading its column's row, as a
    correlated subquery does), a join of history rows with their accounts and
    tellers (keyed lookups), and a self-join (each rich account's rank) match
    a model of the same events, also asked while transfers go on; a module of
    another app is refused (409), bytes that are no module too (400), and a
    query registered again answers anew."""
    import random
    import threading
    bank = pure / 'benchmarks' / 'bank' / 'bank.aup'
    module = work / 'queries-bank.pmod'
    compile_module(aup, bank, module)
    queries = {
        'per_branch': 'view per_branch = each b in branches {\n  bid = b.bid\n'
                      '  total = sum(a.balance for a in accounts where a.bid == b.bid)\n'
                      '  accounts = count(a in accounts where a.bid == b.bid)\n}\n',
        'large': 'view large = each h in history where h.delta > 4000 {\n  aid = h.aid\n  delta = h.delta\n'
                 '  account = accounts[h.aid].balance\n  teller = tellers[h.tid].balance\n}\n',
        'ranks': 'view ranks = each a in accounts where a.balance > 15000 {\n  aid = a.aid\n'
                 '  richer = count(b in accounts where b.balance > a.balance)\n}\n',
        'rich': 'view rich = count(a in accounts where a.balance > 0)\n',
    }
    compiled = {}
    for name, text in queries.items():
        source = work / f'query-{name}.aup'
        source.write_text(text)
        out = work / f'query-{name}.pmod'
        run(aup, 'query', bank, source, '-o', out)
        compiled[name] = out.read_bytes()
    other = work / 'queries-other.pmod'
    (work / 'queries-other.aup').write_text('state accounts = 0\nview rich = accounts\n')
    compile_module(aup, work / 'queries-other.aup', other)
    host = HttpHost([pure / 'build/pure-serve', '--module', module, '--store', work / 'queries-store', '--threads', '2'])

    def call(method, path, body=None, json_body=False):
        import http.client
        connection = http.client.HTTPConnection('127.0.0.1', host.port, timeout=30)
        try:
            connection.request(method, path, body, {'Content-Type': 'application/json'} if json_body else {})
            answer = connection.getresponse()
            return answer.status, answer.getheader('X-Pure-Error'), answer.read()
        finally:
            connection.close()

    try:
        for b in range(1, 4):
            assert call('POST', '/inputs/branch', json.dumps({'bid': b}).encode(), True)[0] == 200
        for t in range(1, 31):
            assert call('POST', '/inputs/teller', json.dumps({'tid': t, 'bid': (t - 1) // 10 + 1}).encode(), True)[0] == 200
        for a in range(1, 301):
            assert call('POST', '/inputs/account', json.dumps({'aid': a, 'bid': (a - 1) // 100 + 1}).encode(), True)[0] == 200
        accounts, tellers, history = {a: 0 for a in range(1, 301)}, {t: 0 for t in range(1, 31)}, []
        rng = random.Random(8)

        def transfer():
            aid, tid, bid, delta = rng.randint(1, 300), rng.randint(1, 30), rng.randint(1, 3), rng.randint(-5000, 5000)
            assert call('POST', '/inputs/transfer', json.dumps({'aid': aid, 'tid': tid, 'bid': bid, 'delta': delta}).encode(),
                        True)[0] == 200
            accounts[aid] += delta
            tellers[tid] += delta
            history.append((aid, tid, delta))

        for _ in range(600):
            transfer()
        for name, bytes_ in compiled.items():
            assert call('PUT', f'/queries/{name}', bytes_) == (200, 'none', b''), name

        def expected(name):
            if name == 'per_branch':
                return [{'bid': b, 'total': sum(v for a, v in accounts.items() if (a - 1) // 100 + 1 == b), 'accounts': 100}
                        for b in range(1, 4)]
            if name == 'large':
                return [{'aid': a, 'delta': d, 'account': accounts[a], 'teller': tellers[t]} for a, t, d in history if d > 4000]
            if name == 'ranks':
                return [{'aid': a, 'richer': sum(1 for v in accounts.values() if v > balance)}
                        for a, balance in sorted(accounts.items()) if balance > 15000]
            return sum(1 for v in accounts.values() if v > 0)

        def answer(name):
            status, error, data = call('GET', f'/queries/{name}')
            assert status == 200, (name, status, error)
            return json.loads(data)['items'] if name != 'rich' else int(data)

        for name in compiled:
            assert answer(name) == expected(name), name
        # Asked while transfers go on, a query sees one save or the next, whole.
        seen, stop = [], threading.Event()

        def ask():
            while not stop.is_set():
                seen.append(sum(row['total'] for row in answer('per_branch')))

        asker = threading.Thread(target=ask)
        asker.start()
        for _ in range(200):
            transfer()
        stop.set()
        asker.join()
        totals = [0]
        for _, _, delta in history:
            totals.append(totals[-1] + delta)
        assert seen and all(total in totals for total in seen), seen[:5]
        assert answer('per_branch') == expected('per_branch')
        page = json.loads(call('GET', '/queries/large?from=1&count=2')[2])
        assert page['items'] == expected('large')[1:3] and page['rows'] == len(expected('large'))
        # Refused: another app's states, bytes that are no module, a module without the view named.
        assert call('PUT', '/queries/rich', other.read_bytes())[:2] == (409, 'conflict')
        assert call('PUT', '/queries/rich', b'not a module')[:2] == (400, 'invalid')
        assert call('PUT', '/queries/missing', compiled['rich'])[:2] == (404, 'not_found')
        assert call('GET', '/queries/missing')[:2] == (404, 'no_value')
        # Registered again: the new module answers.
        (work / 'query-rich2.aup').write_text('view rich = count(a in accounts where a.balance < 0)\n')
        run(aup, 'query', bank, work / 'query-rich2.aup', '-o', work / 'query-rich2.pmod')
        assert call('PUT', '/queries/rich', (work / 'query-rich2.pmod').read_bytes())[0] == 200
        assert answer('rich') == sum(1 for v in accounts.values() if v < 0)
    finally:
        host.stop()
    return {'queries': len(compiled), 'asked_while_writing': len(seen), 'history': len(history)}


def server_webhook(aup, pure, work):
    """An output wired to a third-party HTTPS endpoint (a Python server with
    a certificate of its own): five orders arrive in order as JSON with the
    sender's headers, one answered 503 at first arrives again, and none goes
    to a receiver whose certificate the sender does not trust."""
    from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
    import ssl
    import threading
    import time
    module = work / 'hook-sender.pmod'
    module.with_suffix('.aup').write_text(LINK_SENDER)
    compile_module(aup, module.with_suffix('.aup'), module)
    for name in ('trusted', 'other'):
        subprocess.run(['openssl', 'req', '-x509', '-newkey', 'ec', '-pkeyopt', 'ec_paramgen_curve:P-256', '-nodes',
                        '-keyout', work / f'{name}.key', '-out', work / f'{name}.pem', '-days', '1',
                        '-subj', '/CN=127.0.0.1', '-addext', 'subjectAltName=IP:127.0.0.1'],
                       check=True, capture_output=True)
    received, refused_once = [], set()

    class Hook(BaseHTTPRequestHandler):
        protocol_version = 'HTTP/1.1'

        def do_POST(self):
            body = self.rfile.read(int(self.headers['Content-Length']))
            received.append((dict(self.headers), json.loads(body)))
            status = 200
            if self.headers['X-Pure-Sequence'] == '2' and '2' not in refused_once:
                refused_once.add('2')
                status = 503
            self.send_response(status)
            self.send_header('Content-Length', '0')
            self.end_headers()

        def log_message(self, *args):
            pass

    server = ThreadingHTTPServer(('127.0.0.1', 0), Hook)
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    context.load_cert_chain(work / 'trusted.pem', work / 'trusted.key')
    server.socket = context.wrap_socket(server.socket, server_side=True)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    url = f'ordered=https://127.0.0.1:{server.server_address[1]}/hook'
    try:
        def buy(host, count):
            import http.client
            connection = http.client.HTTPConnection('127.0.0.1', host.port, timeout=10)
            data = json.dumps({'item': f'i{count}', 'count': count}).encode()
            connection.request('POST', '/inputs/buy', data, {'Content-Length': str(len(data)), 'Content-Type': 'application/json'})
            assert connection.getresponse().status == 200
            connection.close()

        host = HttpHost([pure / 'build/pure-serve', '--module', module, '--store', work / 'hook-a', '--send', url,
                         '--send-ca', work / 'trusted.pem'])
        try:
            for count in range(1, 6):
                buy(host, count)
            deadline = time.time() + 20
            while len({h['X-Pure-Sequence'] for h, _ in received}) < 5 and time.time() < deadline:
                time.sleep(0.05)
        finally:
            stats = host.stop()[-1]
        firsts = []
        for headers, body in received:
            seq = int(headers['X-Pure-Sequence'])
            assert headers['X-Pure-Source'] == 'hook-sender.ordered' and headers['Content-Type'] == 'application/json'
            assert headers['Idempotency-Key'] == f'hook-sender.ordered-{seq}' and 'X-Pure-Count' not in headers
            assert body == {'item': f'i{seq}', 'count': seq, 'to': {'city': 'Seoul'}}, body
            if seq not in firsts:
                firsts.append(seq)
        assert firsts == [1, 2, 3, 4, 5] and len(received) == 6 and stats['delivered'] == 5, (firsts, len(received), stats)
        # A receiver the sender cannot trust gets nothing; the send waits in the outbox.
        before = len(received)
        host = HttpHost([pure / 'build/pure-serve', '--module', module, '--store', work / 'hook-b', '--send', url,
                         '--send-ca', work / 'other.pem'])
        try:
            buy(host, 1)
            time.sleep(1)
        finally:
            host.process.terminate()
            _, error = host.process.communicate(timeout=30)
        assert len(received) == before and b'not trusted' in error, error[-500:]
    finally:
        server.shutdown()
    return {'hooks': len(received), 'delivered': stats['delivered']}


PAY_SOURCE = """state sent = 0
state paid = 0
state declined = 0
state paid_total = 0
input buy = message {
  order: text
  amount: u32
}
output charge = message {
  order: text
  amount: u32
}
input charged = message {
  order: text
  amount: u32
  status: text
  http_status: u32
}
on buy {
  sent = sent + 1
  send charge { order: buy.order, amount: buy.amount }
}
on charged {
  if charged.http_status == 200 {
    paid = paid + 1
    paid_total = paid_total + charged.amount
  } else {
    declined = declined + 1
  }
}
view sent_view = sent
view paid_view = paid
view declined_view = declined
view paid_total_view = paid_total
"""


def server_answers(aup, pure, work):
    """An output wired to a payments API whose answers come back as events
    (--reply): the API answers 200 or 402 and gives a key's first answer to
    every request with that Idempotency-Key. While clients buy, the sender is
    killed (-9) and started again; in the end every saved order was answered
    exactly once: the app's paid and declined counts and paid total match the
    API's answers, one per key."""
    from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
    import select
    import signal
    import socket
    import threading
    import time
    module = work / 'pay.pmod'
    module.with_suffix('.aup').write_text(PAY_SOURCE)
    compile_module(aup, module.with_suffix('.aup'), module)
    answers, lock = {}, threading.Lock()

    class Payments(BaseHTTPRequestHandler):
        protocol_version = 'HTTP/1.1'

        def do_POST(self):
            body = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
            with lock:
                key = self.headers['Idempotency-Key']
                if key not in answers:
                    ok = body['amount'] <= 1000
                    answers[key] = (body, 200 if ok else 402,
                                    {'status': 'succeeded' if ok else 'declined', 'id': f'ch_{len(answers)}', 'x': [1, {}]})
                _, status, answer = answers[key]
            data = json.dumps(answer).encode()
            self.send_response(status)
            self.send_header('Content-Type', 'application/json')
            self.send_header('Content-Length', str(len(data)))
            self.end_headers()
            self.wfile.write(data)

        def log_message(self, *args):
            pass

    server = ThreadingHTTPServer(('127.0.0.1', 0), Payments)
    server.handle_error = lambda request, address: None  # a sender killed mid-request
    threading.Thread(target=server.serve_forever, daemon=True).start()
    with socket.socket() as probe:
        probe.bind(('127.0.0.1', 0))
        port = probe.getsockname()[1]
    args = [pure / 'build/pure-serve', '--module', module, '--store', work / 'pay-store', '--port', port,
            '--send', f'charge=http://127.0.0.1:{server.server_address[1]}/charge', '--reply', 'charge=charged']

    def start():
        process = subprocess.Popen([str(a) for a in args], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        ready, _, _ = select.select([process.stdout], [], [], 10)
        assert ready and process.stdout.readline(), 'host did not start'
        return process

    def number(view):
        return int(http(port, 'GET', f'/views/{view}')[3])

    host = start()
    stop = threading.Event()

    def client(which):
        import http.client
        rng = random.Random(which + 8800)
        while not stop.is_set():
            data = json.dumps({'order': f'o{which}-{rng.randrange(10**6)}', 'amount': rng.randint(1, 1500)}).encode()
            try:
                connection = http.client.HTTPConnection('127.0.0.1', port, timeout=10)
                connection.request('POST', '/inputs/buy', data, {'Content-Length': str(len(data)), 'Content-Type': 'application/json'})
                connection.getresponse().read()
                connection.close()
            except OSError:
                time.sleep(0.02)

    clients = [threading.Thread(target=client, args=(n,)) for n in range(3)]
    for thread in clients:
        thread.start()
    try:
        time.sleep(0.8)
        host.send_signal(signal.SIGKILL)
        host.wait()
        host = start()
        time.sleep(0.8)
    finally:
        stop.set()
        for thread in clients:
            thread.join()
    try:
        deadline = time.time() + 30
        while number('paid_view') + number('declined_view') < number('sent_view') and time.time() < deadline:
            time.sleep(0.1)
        sent, paid, declined, total = (number(v) for v in ('sent_view', 'paid_view', 'declined_view', 'paid_total_view'))
        with lock:
            keys = list(answers.values())
        assert paid + declined == sent == len(keys), (paid, declined, sent, len(keys))
        assert paid == sum(1 for _, status, _ in keys if status == 200), (paid, len(keys))
        assert total == sum(body['amount'] for body, status, _ in keys if status == 200)
    finally:
        host.terminate()
        _, error = host.communicate(timeout=30)
        server.shutdown()
    stats = [json.loads(line) for line in error.decode().splitlines() if line.startswith('{"active"')][-1]
    return {'orders': sent, 'paid': paid, 'declined': declined, 'answers_after_restart': stats['answers']}


BEAT_SOURCE = """state beats = 0
state last = 0
state stamped = 0
input beat = every 50 ms
input stamp = message {
  note: text
}
on beat {
  beats = beats + 1
  last = now
}
on stamp { stamped = now }
view beats_view = beats
view last_view = last
view stamped_view = stamped
"""


def server_timers(aup, pure, work):
    """A served app's timers tick on the server's clock and its events see
    the time they were applied, on one thread and with an owner; the ticks
    are saved, so a restarted server goes on from them."""
    import time
    module, source = work / 'beat.pmod', work / 'beat.aup'
    source.write_text(BEAT_SOURCE)
    compile_module(aup, source, module)
    seen = {}
    for threads in (1, 3):
        store = work / f'beat-{threads}'
        beats = 0
        for run_number in range(2):
            host = HttpHost([pure / 'build/pure-serve', '--module', module, '--store', store, '--threads', threads])
            try:
                posted = time.time() * 1000
                assert http(host.port, 'POST', '/inputs/stamp', b'x')[0] == 200
                time.sleep(0.6)
                before = time.time() * 1000
                read = lambda view: float(http(host.port, 'GET', f'/views/{view}')[3])
                count, last, stamped = read('beats_view'), read('last_view'), read('stamped_view')
                after = time.time() * 1000 + 1  # the server's clock reads whole milliseconds
                assert count >= beats + 5, (threads, run_number, count, beats)
                assert before - 1000 < last <= after and posted - 50 <= stamped <= before, (last, stamped, posted, before)
                beats = count
            finally:
                stats = host.stop()
            assert stats[0]['ticks'] >= 5, stats
        seen[threads] = beats
    return seen


def server_app(aup, pure, work, seeds, operations=200, threads=1):
    """An app served by its views and inputs, no routes written: random
    POSTs (a message with query fields and a body, a text, a key) and GETs
    (a number, a text, a table's pages) answer exactly as a Python model
    says, failures with their fault; what was acknowledged is there after
    the server stops, and after it is killed while clients post. The app's
    owner thread applies the inputs; connection threads (one or several)
    show views from the snapshot of its last save."""
    import signal
    import threading
    import time
    module = work / 'served.pmod'
    source = work / 'served.aup'
    source.write_text(SERVED_SOURCE)
    compile_module(aup, source, module)
    names = ['ann', 'bob', 'zed', '가', 'a"b', 'back\\slash', 'tab\t', '', 'bob2']
    compared = 0
    for seed in seeds:
        rng, board, at = random.Random(seed + 9100), {}, 0
        store = work / f'served-{seed}-{threads}'
        serve = [pure / 'build/pure-serve', '--module', module, '--threads', threads]
        host = HttpHost(serve + ['--store', store])
        order = lambda: sorted(board, key=lambda name: name.encode())

        def place(name):
            nonlocal at
            keys = order()
            if name not in board:
                position = sum(1 for k in keys if k.encode() < name.encode())
                board[name] = 0
                if len(board) > 1 and at >= position: at += 1

        try:
            for _ in range(operations):
                roll, keys = rng.random(), order()
                if roll < 0.35:
                    name, points = rng.choice(names), rng.randint(0, 9)
                    if rng.random() < 0.05:
                        request = ('POST', '/inputs/score', b'x')  # no points
                        want = (400, 'missing_field', b'')
                    elif rng.random() < 0.05:
                        request = ('POST', f'/inputs/score?points={points}', b'\xff')
                        want = (422, 'encoding', b'')
                    else:
                        request = ('POST', f'/inputs/score?points={points}', name.encode())
                        place(name)
                        board[name] += points
                        at = order().index(name)
                        want = (200, 'none', name.encode())
                elif roll < 0.5:
                    name = rng.choice(names)
                    request = ('POST', '/inputs/join', name.encode())
                    if name in board: want = (409, 'exists', b'')
                    else:
                        place(name)
                        at = order().index(name)
                        want = (200, 'none', b'')
                elif roll < 0.58:
                    request = ('POST', '/inputs/drop', b'')
                    if not board: want = (404, 'not_found', b'')
                    else:
                        del board[keys[at]]
                        at = min(at, max(len(board) - 1, 0))
                        want = (200, 'none', b'')
                elif roll < 0.68:
                    request = ('GET', '/views/total', b'')
                    want = (200, 'none', str(sum(board.values())).encode())
                elif roll < 0.78:
                    request = ('GET', '/views/current', b'')
                    want = (200, 'none', keys[at].encode()) if board else (404, 'no_value', b'')
                elif roll < 0.96:
                    first, count = rng.randint(0, 4), rng.choice([1, 2, 5, 100])
                    request = ('GET', f'/views/leaders?from={first}&count={count}', b'')
                    shown = min(first, len(keys))
                    rows = [{'name': k, 'points': board[k]} for k in keys[shown:shown + count]]
                    want = (200, 'none', {'rows': len(keys), 'from': shown, 'items': rows})
                else:
                    request, want = rng.choice([(('GET', '/views/nope', b''), (404, 'no_view', b'')),
                                                (('POST', '/inputs/nope', b''), (404, 'no_input', b'')),
                                                (('POST', '/inputs/join?x=1', b'q'), (400, 'query', b'')),
                                                (('GET', '/views/total?from=1', b''), (400, 'query', b''))])
                status, error, kind, body = http(host.port, *request)
                if isinstance(want[2], dict):
                    assert kind == 'application/json', kind
                    body = json.loads(body)
                assert (status, error, body) == want, (seed, request, (status, error, body), want)
                compared += 1
        finally:
            stats = host.stop()
        assert stats[0]['bursts'] <= stats[0]['requests'] and stats[0]['owner'], stats
        assert stats[0]['views_from_snapshots'] > 0, stats
        # A graceful stop kept every acknowledged event.
        host = HttpHost(serve + ['--store', store])
        try:
            got = json.loads(http(host.port, 'GET', '/views/leaders')[3])
            assert got['items'] == [{'name': k, 'points': board[k]} for k in order()], (seed, got)
        finally:
            host.stop()

    # Clients post while the server is killed: each acknowledged point is
    # stored. Readers on other connections meanwhile see every point
    # acknowledged before they asked, never a total going back, and never one
    # that was not saved: the total after the kill is at least any they saw.
    store = work / f'served-kill-{threads}'
    serve = [pure / 'build/pure-serve', '--module', module, '--threads', threads]
    host = HttpHost(serve + ['--store', store])
    acknowledged = [0] * 6
    seen = [0, 0]

    def client(number):
        for _ in range(10000):
            try:
                status, _, _, _ = http(host.port, 'POST', '/inputs/score?points=1', f'c{number}'.encode())
            except OSError:
                return
            if status != 200:
                return
            acknowledged[number] += 1

    def reader(number):
        for _ in range(100000):
            floor = sum(acknowledged)
            try:
                status, _, _, body = http(host.port, 'GET', '/views/total')
            except OSError:
                return
            if status != 200:
                return
            total = int(body)
            assert floor <= total and seen[number] <= total, (floor, seen[number], total)
            seen[number] = total

    clients = [threading.Thread(target=client, args=(n,)) for n in range(len(acknowledged))]
    clients += [threading.Thread(target=reader, args=(n,)) for n in range(len(seen))]
    for thread in clients:
        thread.start()
    time.sleep(0.8)
    host.process.send_signal(signal.SIGKILL)
    host.process.wait()
    for thread in clients:
        thread.join()
    host = HttpHost(serve + ['--store', store])
    try:
        stored = {row['name']: row['points'] for row in json.loads(http(host.port, 'GET', '/views/leaders')[3])['items']}
        for number, count in enumerate(acknowledged):
            assert count > 0 and stored.get(f'c{number}', 0) in (count, count + 1), (number, count, stored)
        assert min(seen) > 0 and max(seen) <= sum(stored.values()), (seen, stored)
    finally:
        host.stop()
    return compared, sum(acknowledged)


def main():
    global EDITOR
    parser = argparse.ArgumentParser()
    parser.add_argument('--aup', type=Path, required=True, help='the aup compiler')
    parser.add_argument('--aup-root', type=Path, default=ROOT.parent / 'aup', help='its checkout, for examples/editor.aup')
    parser.add_argument('--seeds', type=int, default=4)
    args = parser.parse_args()
    aup, pure = args.aup.resolve(), ROOT
    EDITOR = args.aup_root.resolve() / 'examples' / 'editor.aup'
    report = {}
    with tempfile.TemporaryDirectory(prefix='pure-check-') as directory:
        work = Path(directory)
        report['server_parity_requests'] = sum(server_parity(aup, pure, work, range(args.seeds), threads=threads)
                                               for threads in (1, 3))
        report['server_pipelined_answers'] = server_pipelining(aup, pure, work)
        report['server_lent_read_bytes'] = server_lent_reads(aup, pure, work)
        report['server_lent_budget_bytes'] = server_lent_budget(aup, pure, work)
        report['server_lent_views'] = [server_lent_views(aup, pure, work, threads) for threads in (1, 3)]
        report['server_bursts'] = server_bursts(aup, pure, work)
        report['server_bursts_three_threads'] = server_bursts(aup, pure, work, shards=3)
        report['server_app_requests'], report['server_app_acknowledged_before_kill'] = \
            server_app(aup, pure, work, range(args.seeds))
        report['server_app_owner_requests'], report['server_app_owner_acknowledged_before_kill'] = \
            server_app(aup, pure, work, range(args.seeds), threads=3)
        report['server_json_requests'] = server_json(aup, pure, work)
        report['server_timer_beats'] = server_timers(aup, pure, work)
        report['server_links'] = server_links(aup, pure, work)
        report['server_webhook'] = server_webhook(aup, pure, work)
        report['server_sealed'] = server_sealed(aup, pure, work)
        report['server_surface'] = server_surface(aup, pure, work)
        report['server_queries'] = server_queries(aup, pure, work)
        report['server_rows'] = server_rows(aup, pure, work)
        report['server_arguments'] = server_arguments(aup, pure, work)
        report['server_answers'] = server_answers(aup, pure, work)
    print(json.dumps(report))


if __name__ == '__main__':
    sys.exit(main())
