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
   API's answers become the app's events (--reply). Linked without a
   connection (pure://, UDP, docs/design/011), the same holds across kill -9
   of either side or both with no handshake, also with the link's packets
   delayed, lost and reordered (netem, in a namespace of its own).
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
import time
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

    def __init__(self, args, env=None):
        import select
        self.process = subprocess.Popen([str(a) for a in args] + ['--port', '0'], stdout=subprocess.PIPE,
                                        stderr=subprocess.PIPE, env=env)
        ready, _, _ = select.select([self.process.stdout], [], [], 10)
        assert ready, 'HTTP host did not start'
        line = self.process.stdout.readline()
        assert line, self.process.communicate(timeout=5)
        self.info = json.loads(line)  # its port, name, public key
        self.port = self.info['port']

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


def post(port, path, body):
    """A JSON body posted on its own connection: status, X-Pure-Error."""
    import http.client
    connection = http.client.HTTPConnection('127.0.0.1', port, timeout=30)
    try:
        data = json.dumps(body).encode()
        connection.request('POST', path, data, {'Content-Length': str(len(data)), 'Content-Type': 'application/json'})
        response = connection.getresponse()
        response.read()
        return response.status, response.getheader('X-Pure-Error')
    finally:
        connection.close()


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


def server_links_udp(aup, pure, work, netem=None):
    """The same two apps linked without a connection (--send
    OUTPUT=pure://host:port/INPUT to the receiver's --link-port, sealed with
    the two nodes' keys, docs/design/011-pure-transport.md). While four
    clients buy from the sender, the receiver, then the sender, then both
    are killed (-9) and started again on their stores: neither side makes a
    handshake, and in the end the receiver has applied every order the
    sender saved exactly once, the refused order (count 150) did not hold
    back the one after it, and every acknowledged order is among the saved
    ones. With netem (in a network namespace of its own), the loopback
    delays, loses and reorders packets meanwhile. The receiver takes its
    senders from a file (--senders, the sender among a thousand others) and
    lets a sender's keys and streams go after 400 ms of quiet
    (--link-idle-ms): the order after a longer pause arrives all the same.
    Started again, it makes no X25519: the secret is remembered beside its
    key (KEY.links)."""
    import select
    import signal
    import socket
    import threading
    import time
    sender_module, receiver_module = work / 'udp-sender.pmod', work / 'udp-receiver.pmod'
    for module, text in ((sender_module, LINK_SENDER), (receiver_module, LINK_RECEIVER)):
        source = module.with_suffix('.aup')
        source.write_text(text)
        compile_module(aup, source, module)

    def free_port(kind=socket.SOCK_STREAM):
        with socket.socket(socket.AF_INET, kind) as probe:
            probe.bind(('127.0.0.1', 0))
            return probe.getsockname()[1]

    def start(args, port):
        process = subprocess.Popen([str(a) for a in args] + ['--port', str(port)], stdout=subprocess.PIPE,
                                   stderr=subprocess.PIPE)
        ready, _, _ = select.select([process.stdout], [], [], 10)
        line = process.stdout.readline() if ready else b''
        assert line, f'host did not start: {process.stderr.read(2000) if process.poll() is not None else ""}'
        return process, json.loads(line)

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

    tag = 'udp-lossy' if netem else 'udp'
    keys = {}
    for name in ('sender', 'receiver'):  # each node's key, made by the node itself
        process, info = start([pure / 'build/pure-serve', '--module', receiver_module, '--store', work / f'{tag}-key',
                               '--key', work / f'{tag}-{name}.key'], 0)
        keys[name] = info['public_key']
        process.terminate()
        process.communicate(timeout=30)
    port_a, port_b, link_port = free_port(), free_port(), free_port(socket.SOCK_DGRAM)
    others = random.Random(4242)
    senders = [f'other{n} {others.getrandbits(256):064x}' for n in range(1000)]
    senders.insert(500, f'udp-sender.ordered {keys["sender"]}')
    (work / f'{tag}-senders').write_text('\n'.join(senders) + '\n')
    receiver = [pure / 'build/pure-serve', '--module', receiver_module, '--store', work / f'{tag}-b', '--threads', '2',
                '--key', work / f'{tag}-receiver.key', '--link-port', link_port,
                '--senders', work / f'{tag}-senders', '--link-idle-ms', '400']
    sender = [pure / 'build/pure-serve', '--module', sender_module, '--store', work / f'{tag}-a',
              '--key', work / f'{tag}-sender.key', '--send', f'ordered=pure://127.0.0.1:{link_port}/order',
              '--seal', f'ordered={keys["receiver"]}']
    hosts = {'b': start(receiver, port_b)[0], 'a': start(sender, port_a)[0]}
    acknowledged = [0] * 4
    stop = threading.Event()

    def client(which):
        rng = random.Random(which + 9900)
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
        for names in (('b',), ('a',), ('a', 'b')):
            time.sleep(0.6)
            for name in names:
                hosts[name].send_signal(signal.SIGKILL)
                hosts[name].wait()
            time.sleep(0.3)
            for name in names:
                hosts[name] = start(receiver if name == 'b' else sender, port_b if name == 'b' else port_a)[0]
        time.sleep(0.6)
    finally:
        stop.set()
        for thread in clients:
            thread.join()
    try:
        assert post(port_a, '/inputs/buy', {'item': 'big', 'count': 150}) == 200
        assert post(port_a, '/inputs/buy', {'item': 'after', 'count': 7}) == 200
        deadline = time.time() + 60
        while True:
            sent, sent_total = number(port_a, 'sent'), number(port_a, 'sent_total')
            count, total = number(port_b, 'count'), number(port_b, 'total')
            if (count, total) == (sent - 1, sent_total - 150) or time.time() > deadline:
                break
            time.sleep(0.1)
        assert (count, total) == (sent - 1, sent_total - 150), (count, total, sent, sent_total)
        assert sent_total >= sum(acknowledged) + 157 and sum(acknowledged) > 0, (sent_total, acknowledged)
        time.sleep(1.2)  # the receiver lets the quiet sender go
        assert post(port_a, '/inputs/buy', {'item': 'late', 'count': 3}) == 200
        deadline = time.time() + 30
        while (number(port_b, 'count'), number(port_b, 'total')) != (sent, sent_total - 147) and time.time() < deadline:
            time.sleep(0.05)
        assert (number(port_b, 'count'), number(port_b, 'total')) == (sent, sent_total - 147)
    finally:
        stats = {}
        for name, process in hosts.items():
            process.terminate()
            _, error = process.communicate(timeout=30)
            assert process.returncode == 0, error[-1000:]
            stats[name] = [json.loads(line) for line in error.decode().splitlines() if line.startswith('{"active"')][-1]
    # The receiver, started again, remembered the sender's secret: no X25519 for it.
    assert stats['b']['listener_swept'] >= 1 and stats['b']['listener_woken'] >= 2, stats['b']
    assert stats['b']['listener_remembered'] == stats['b']['listener_woken'], stats['b']
    return {'orders': sent, 'acknowledged_total': sum(acknowledged),
            'packets_after_restart': stats['a']['link_packets'], 'resent_after_restart': stats['a']['link_resent'],
            'lost_after_restart': stats['a']['link_lost'], 'received_after_restart': stats['b']['listener_received'],
            'woken_after_restart': stats['b']['listener_woken'], 'swept_after_restart': stats['b']['listener_swept'],
            'remembered_after_restart': stats['b']['listener_remembered']}


def server_links_udp_lossy(aup, pure, work):
    """server_links_udp in a network namespace of its own (unshare -rn, no
    privileges), its loopback delaying (5 ms, 2 ms jitter), losing (3 %) and
    reordering (10 %) the link's packets (UDP; the clients' HTTP goes
    straight). None where there is no unshare or tc."""
    import shutil
    unshare = shutil.which('unshare')
    tc = shutil.which('tc') or next((path for path in ('/usr/sbin/tc', '/sbin/tc') if Path(path).exists()), None)
    ip = shutil.which('ip') or next((path for path in ('/usr/sbin/ip', '/sbin/ip', '/bin/ip') if Path(path).exists()), None)
    if not unshare or not tc or not ip or subprocess.run([unshare, '-rn', 'true'], capture_output=True).returncode:
        return None
    script = f"""
import json, subprocess, sys
from pathlib import Path
sys.path.insert(0, {str(HERE)!r})
import check
subprocess.run([{ip!r}, 'link', 'set', 'lo', 'up'], check=True)
# UDP alone through netem (the clients' HTTP stays quick): a prio qdisc, its third band netem, a filter
for rule in (['qdisc', 'add', 'dev', 'lo', 'root', 'handle', '1:', 'prio'],
             ['qdisc', 'add', 'dev', 'lo', 'parent', '1:3', 'handle', '30:', 'netem', 'delay', '5ms', '2ms',
              'loss', '3%', 'reorder', '10%'],
             ['filter', 'add', 'dev', 'lo', 'protocol', 'ip', 'parent', '1:0', 'prio', '3', 'u32', 'match', 'ip',
              'protocol', '17', '0xff', 'flowid', '1:3']):
    subprocess.run([{tc!r}] + rule, check=True)
print(json.dumps(check.server_links_udp(Path({str(aup)!r}), Path({str(pure)!r}), Path({str(work)!r}), netem=True)))
"""
    done = subprocess.run([unshare, '-rn', sys.executable, '-c', script], capture_output=True, text=True, timeout=600)
    assert done.returncode == 0, done.stderr[-3000:]
    return json.loads(done.stdout.strip().splitlines()[-1])


RANGES_SOURCE = """record Item { id = 0 }
state rows: list of Item by id
state other: list of Item by id
state at: index of other
state limit = 2
state done = 0
state below = 0
input seed = key KeyS
input go = key KeyG
input count = key KeyC
on seed.down {
  rows.add(Item { id: 0 })
  rows.add(Item { id: 1 })
  rows.add(Item { id: 2 })
  rows.add(Item { id: 3 })
  rows.add(Item { id: 4 })
  other.add(Item { id: 100 })
}
on go.down {
  for r in rows where r.id <= at.position {
    other.add(Item { id: -r.id - 1 - done })
    done = done + 1
  }
}
on count.down {
  for r in rows where r.id <= limit {
    below = below + 1
  }
}
view total = done
view counted = below
"""


def module_ranges(aup, pure, work):
    """A walk whose where reads an index into a list its body inserts into:
    each insert before the index's item moves the index on, so the walk's
    bound moves with it and all five rows pass, twice (review 004, F1). The
    module runs ranged and not from the same keys (build/module-probe), and
    the two must end the same, state for state; a walk whose bound its body
    cannot move stays ranged."""
    source, module = work / 'ranges.aup', work / 'ranges.pmod'
    source.write_text(RANGES_SOURCE)
    compile_module(aup, source, module)
    done = subprocess.run([str(pure / 'build/module-probe'), str(module), 'seed', 'go', 'count', 'go'],
                          capture_output=True, text=True, timeout=60)
    assert done.returncode == 0, done.stderr
    row = json.loads(done.stdout)
    states = row['states']
    assert states['done'] == 10 and states['at'] == 10 and states['below'] == 3, row
    assert row['ranged'] >= 1, row
    return row


ASSUMPTIONS_SOURCE = """record Cell {
  at = 0
  v = 0
}
state total = 0
state big = 0
state count = 0
input go = message {
  n: u32
  half: u32
}
on go {
  let cells: list of Cell
  let keyed: list of Cell by at
  for i in 0 .. go.n {
    cells.append(Cell { at: i, v: i * 3 })
    keyed.add(Cell { at: i, v: i + 1 })
  }
  for i in 0 .. go.n * 2 {
    if i >= cells.count { break }
    if i == go.half {
      cells.append(Cell { at: 0.5, v: -0.0 })
      keyed.add(Cell { at: go.n + 0.5, v: 0.25 })
    }
    let c = cells.first + i
    total = total + c.v + c.at * 2
    if keyed.has(c.at) {
      total = total + keyed[c.at].v
    }
    big = big + c.at * 1099511627776
    count = count + cells.count
  }
}
"""

# A local's products and sums, whole, of a size the code does not know: where
# a check gives up, the result's register may be its operand's, and the
# executor takes the operands again as they were.
LOCALS_SOURCE = """record Cell {
  at = 0
}
state small = 0
state big = 0
input go = message {
  n: u32
}
on go {
  let cells: list of Cell
  for i in 0 .. go.n {
    cells.append(Cell { at: i })
  }
  for i in 0 .. go.n {
    let c = cells.first + i
    let v = c.at
    small = small + v * 1048576
    let w = c.at * 1099511627776
    let u = w + w
    big = big + w * 3 + u + w * 1024
  }
}
"""


# The tier where its first checks missed (module_tier's seeds 17 and 23): a
# value in a loop made of a phi where an if's ways meet in it (MERGES_SOURCE:
# taken as made outside the loop, it was made once for every round), and the
# outermost loop's round past the work budget (BUDGET_SOURCE: given up at the
# loop's instruction, with nothing on the stack, rather than its iteration).
MERGES_SOURCE = """record Cell {
  at = 0
  v = 0
}
state out = 0
state acc = 0
input go = message {
  n: u32
  m: u32
  q: u32
}
on go {
  let t: list of Cell by at
  let f: list of Cell
  for i in 0 .. go.n {
    t.add(Cell { at: i, v: i })
    f.append(Cell { at: i * 2, v: i })
  }
  let s = 0
  let u = 1
  for i in 0 .. go.m {
    for j in 0 .. go.n {
      if t.has(go.q) {
        acc = acc + u
      }
      for c in f {
        if go.m != u {
          s = s + (floor(go.n / 7) - max(u, go.m))
        }
        t[3].v = 1000
      }
      acc = acc + (go.n + go.m)
    }
    t[3].v = ((255 - s) + (1000 - 1000))
  }
  out = s
  acc = acc + u
}
"""

BUDGET_SOURCE = """record Cell {
  at = 0
  v = 0
}
state out = 0
state acc = 0
input go = message {
  n: u32
  m: u32
  q: u32
}
on go {
  let t: list of Cell by at
  let f: list of Cell
  for i in 0 .. go.n {
    t.add(Cell { at: i, v: i })
    f.append(Cell { at: i * 2, v: i })
  }
  let s = 0
  let u = 1
  for i in 0 .. go.m {
    if !(go.q != s) { break }
    t[3].v = (t[floor(go.n / 2)].v + (go.m + go.n))
  }
  out = s
  acc = acc + u
}
"""

# Numbers' arithmetic in a run, each read by the next alone: only the last
# checks it is finite, giving up at the run's first snapshot (023); the
# product passes the largest number at the 56th round.
CHAIN_SOURCE = """state out = 0
state steps = 0
input go = message {
  n: u32
}
on go {
  let s = 1
  let c = 0
  for i in 0 .. go.n {
    s = (s * 1000000 + i) / 3 * 2 + 1
    c = c + 1
  }
  out = s
  steps = c
}
"""

# Loops a batch runs (Pure docs/design/024) where it must give its rounds
# back or change a list's form (BATCH_SOURCE): cells past a byte and past
# four, a fraction and -0 set in the middle of a batch (the columns widened),
# 0 divided by at round q (a fault halfway through a batch: the executor's,
# at its round), each round reading the last one's set (not apart: run one
# by one), keys going down with an if and an else, two sets of one field,
# sums into three cells in turn (the column widened as they grow), a walk
# of a slice setting another field at each item.
BATCH_SOURCE = """record Cell {
  at = 0
  v = 0
  w = 0
}
state out = 0
state low = 0
input go = message {
  n: u32
  q: u32
}
on go {
  let t: list of Cell by at
  for i in 0 .. go.n {
    t.add(Cell { at: i })
  }
  for k in 0 .. go.n {
    t[k].v = k * k * 1000
    t[k].w = k * (0 - 1)
  }
  for k in 0 .. go.n {
    if k == go.q { t[k].v = 0.25 } else { t[k].w = t[k].w + 1 }
  }
  for k in 1 .. go.n {
    t[k].v = t[k - 1].v + 1
  }
  for k in 0 .. go.n {
    let x = t[go.n - 1 - k].v
    if x > 5000 { t[go.n - 1 - k].w = x - 5000 } else { t[go.n - 1 - k].w = 7 }
    t[go.n - 1 - k].v = x + 1
  }
  for k in 0 .. go.n {
    t[k - floor(k / 3) * 3].v = t[k - floor(k / 3) * 3].v + k * 1000000
  }
  for c in t[3 .. go.n - 1] {
    t[c.at].w = c.v + c.at
  }
  for k in 0 .. go.n {
    t[k].w = 1000 / (k - go.q)
  }
  out = t[go.n - 1].v + t[0].w
  low = t[floor(go.n / 2)].w
}
"""


# Sums into cells in whole numbers (SUMS_SOURCE, docs/design/025's stage 5):
# sixteen rows met again and again, a one-byte column widened to four in the
# middle of a batch, a four-byte one past 2^32 (to eight), and a masked
# fraction where the whole numbers stop.
SUMS_SOURCE = """record Total {
  region = 0
  amount = 0
}
state totals = 0
state tinies = 0
state bigs = 0
state mixed = 0
input go = message {
  n: u32
  q: u32
}
on go {
  let t: list of Total by region
  let u: list of Total by region
  let w: list of Total by region
  let m: list of Total by region
  for r in 0 .. 16 {
    t.add(Total { region: r })
    u.add(Total { region: r })
    w.add(Total { region: r })
    m.add(Total { region: r })
  }
  for o in 0 .. go.n {
    let region = o - floor(o / 16) * 16
    t[region].amount = t[region].amount + (o - floor(o / 1000) * 1000)
    u[region].amount = u[region].amount + 1
    w[region].amount = w[region].amount + 1500000000
    if o - floor(o / go.q) * go.q == 0 { m[region].amount = m[region].amount + 0.5 }
  }
  let a = 0
  let b = 0
  let c = 0
  let d = 0
  for x in t {
    a = a + x.amount * (x.region + 1)
  }
  for x in u {
    b = b + x.amount * (x.region + 1)
  }
  for x in w {
    c = c + x.amount
  }
  for x in m {
    d = d + x.amount * (x.region + 1)
  }
  totals = a
  tinies = b
  bigs = c
  mixed = d
}
"""

# Columns read in place (VIEW_SOURCE, docs/design/025's stage 4): a local
# holding a row's field while the batch sets that field (its last value
# written back after the sets), a set whose value is the same column three
# rows over in the order that overwrites what it reads, and a set that widens
# the column to eight bytes while a local holds a view of it.
VIEW_SOURCE = """record Cell {
  at = 0
  v = 0
}
state last = 0
state back = 0
state wide = 0
state sum = 0
input go = message {
  n: u32
  q: u32
}
on go {
  let t: list of Cell by at
  for i in 0 .. go.n {
    t.add(Cell { at: i, v: i * 3 })
  }
  let a = 0
  for i in 0 .. go.n - 3 {
    a = t[i].v
    t[i].v = t[i + 3].v + 1
  }
  last = a
  for k in 0 .. go.n - 3 {
    let i = go.n - 1 - k
    t[i].v = t[i - 3].v
  }
  back = t[go.n - 1].v + t[go.n - 2].v * 1000
  let w = 0
  for i in 0 .. go.n - 3 {
    w = t[i].v
    t[i].v = t[i + 3].v + go.q + 0.5
  }
  wide = w
  let s = 0
  for c in t {
    s = s + c.v
  }
  sum = s
}
"""

# Reductions that start with no lanes (REDUCE_SOURCE, docs/design/025): the
# same e in every lane (a count, a uniform max and min, a fraction, a sum
# past 2^53 one a lane), a count in a nested loop of the same rounds in
# every lane, and the same e first, then a masked one or a lane's own (the
# lanes made from the one number), in both orders.
REDUCE_SOURCE = """state count = 0
state mixed = 0
state other = 0
state low = 0
state high = 0
state frac = 0
state far = 0
state inner = 0
input go = message {
  n: u32
  q: u32
}
on go {
  let c = 0
  let m = 0
  let o = 0
  let lo = 1000000
  let hi = 0 - 1000000
  let f = 0
  let x = 0
  let t = 0
  for i in 0 .. go.n {
    c = c + 1
    m = m + go.q
    if i - floor(i / go.q) * go.q == 0 { m = m + i }
    o = o + i
    o = o + 3
    lo = min(lo, go.q)
    hi = max(hi, go.q * 2)
    hi = max(hi, i - 5)
    f = f + 0.5
    x = x + 1099511627777
  }
  for i in 0 .. go.n {
    for k in 0 .. go.q {
      t = t + 2
    }
    t = t + i
  }
  count = c
  mixed = m
  other = o
  low = lo
  high = hi
  frac = f
  far = x
  inner = t
}
"""

# Nested loops a batch runs together, and reductions (NESTED_SOURCE): Newton
# steps breaking out lane by lane, a sum of fractions one a round, a nested
# loop of 0 to 6 rounds a lane whose division by 0 is a fault in some
# lane's round (q < 4), max and min, a sum of fractions several a lane (the
# batch gives its rounds back), -0 summed, a grid's sums.
NESTED_SOURCE = """state total = 0
state steps = 0
state most = 0
state least = 0
state frac = 0
state zero = 0
state grid = 0
input go = message {
  n: u32
  q: u32
}
on go {
  let sum = 0
  let count = 0
  for i in 1 .. go.n + 1 {
    let r = i
    for s in 0 .. 64 {
      let next = (r + i / r) / 2
      if next >= r { break }
      r = next
      count = count + 1
    }
    sum = sum + r
  }
  let high = 0
  let low = 0
  for i in 0 .. go.n {
    let w = 0
    for k in 0 .. i - floor(i / 7) * 7 {
      w = w + 1000 / (k + go.q - 3)
    }
    high = max(high, w)
    low = min(low, 0 - w)
  }
  let f = 0
  for i in 0 .. go.n {
    for k in 0 .. 3 {
      f = f + 0.1
    }
  }
  let z = 0 * (0 - 1)
  for i in 0 .. go.n {
    z = z + 0 * (0 - 1)
  }
  let g = 0
  for j in 0 .. go.n {
    let c = 0
    for k in 0 .. 20 {
      c = c + (j * 7 + k) * (k + 1)
    }
    g = g + c
  }
  total = sum
  steps = count
  most = high
  least = low
  frac = f
  zero = z
  grid = g
}
"""


# Recurrences a batch runs ahead of its rounds (RECUR_SOURCE): two updates
# of x a round, read between them, items added at keys and appended; a
# division by 0 at round q (a fault in the run ahead: the executor's at
# that round); a product past the numbers (not finite at some round); a
# recurrence of the loop's value and a field.
RECUR_SOURCE = """record Cell {
  at = 0
  v = 0
}
state first = 0
state last = 0
state far = 0
state sum = 0
input go = message {
  n: u32
  q: u32
}
on go {
  let a: list of Cell by at
  let b: list of Cell
  let x = 1
  for i in 0 .. go.n {
    x = x * 48271 - floor(x * 48271 / 2147483647) * 2147483647
    a.add(Cell { at: i, v: x - floor(x / 100) * 100 })
    x = x * 48271 - floor(x * 48271 / 2147483647) * 2147483647
    b.append(Cell { at: x - floor(x / 7) * 7, v: x / (i - go.q) })
  }
  let y = 3
  for i in 0 .. go.n {
    y = y * 1000 + i
  }
  let z = 0.5
  for i in 0 .. go.n {
    z = z + i * go.q
  }
  first = a[0].v
  last = x
  far = y
  sum = z
}
"""


# Walks in a batch's rounds (FRONT_SOURCE): a count's loop to a list's
# count (if i >= L.count { break }, past it as the loop goes on), each
# round walking its slice of a keyed list (slices of every length, empty
# too) with sums of fractions (their order tells), the most, an item's own
# local and the round's value read in it (its items spread apart into
# lanes); a walk that breaks out (its items together, round by round); a
# division by 0 in some round's item; under work budgets that run out
# within a walk.
FRONT_SOURCE = """record Cell {
  at = 0
  v = 0
}
state total = 0
state most = 0
state last = 0
state broke = 0
input go = message {
  n: u32
  q: u32
}
on go {
  let g: list of Cell by at
  let f: list of Cell
  for i in 0 .. go.n {
    g.add(Cell { at: i * 3, v: i * 0.25 + 1 })
    f.append(Cell { at: i, v: floor(i / 7) })
  }
  let s = 0
  let m = 0
  let k = 0
  let z = 0
  for i in 0 .. go.n * 2 + 5 {
    if i >= f.count { break }
    let w = f[f.first + i].v
    for c in g[i * 3 - w .. i * 3 + w * 2] {
      let o = c.v * 0.1 + w
      s = s + o
      m = max(m, c.v - i)
      z = z + 1 / (c.at - go.q)
    }
    for c in g[i .. i + 9] {
      if c.v > 3 + w { break }
      k = k + c.v
    }
  }
  total = s
  most = m
  last = k
  broke = z
}
"""

# Filters in a batch's block (GRAPH_SOURCE): a graph built in the handler
# (an edge added if not there yet, keys from a recurrence), searched breadth
# first twice (a count's loop to the queue's count, each node's edges
# walked and spread, if !seen.has(e.to) { seen.add; queue.append }: asked
# as the batch opens and again in the rounds' order at its end), the depths
# summed by a walk; marks whose filter asks one key and adds another (one
# there already: EXISTS at its round); under work budgets that end within a
# batch and memory budgets that end in the filters' adds.
GRAPH_SOURCE = """record Edge {
  link = 0
  to = 0
}
record Seen {
  node = 0
  depth = 0
}
record Step {
  node = 0
}
state reached = 0
state hops = -1
state far = 0
state marked = 0
input go = message {
  n: u32
  m: u32
  from: u32
  q: u32
}
on go {
  let edges: list of Edge by link
  let seen: list of Seen by node
  let queue: list of Step
  let again: list of Seen by node
  let next: list of Step
  let marks: list of Seen by node
  let order: list of Step
  let x = 7
  for k in 0 .. go.m {
    x = x * 48271 - floor(x * 48271 / 2147483647) * 2147483647
    let a = x - floor(x / go.n) * go.n
    let b = floor(x / 1000) - floor(x / 1000 / go.n) * go.n
    if !edges.has(a * 1048576 + b) { edges.add(Edge { link: a * 1048576 + b, to: b }) }
  }
  queue.append(Step { node: go.from })
  seen.add(Seen { node: go.from })
  for i in 0 .. go.n {
    if i >= queue.count { break }
    let q = queue.first + i
    for e in edges[q.node * 1048576 .. q.node * 1048576 + 1048576] {
      if !seen.has(e.to) {
        seen.add(Seen { node: e.to, depth: seen[q.node].depth + 1 })
        queue.append(Step { node: e.to })
      }
    }
  }
  next.append(Step { node: go.q })
  again.add(Seen { node: go.q, depth: 1 })
  for i in 0 .. go.n * 2 {
    if i >= next.count { break }
    let q = next.first + i
    for e in edges[q.node * 1048576 + 1 .. q.node * 1048576 + 1048576] {
      if !again.has(e.to) {
        again.add(Seen { node: e.to, depth: again[q.node].depth * 2 + 0.5 })
        next.append(Step { node: e.to })
      }
    }
  }
  let total = 0
  for s in seen {
    total = total + s.depth * (s.node + 1)
  }
  for s in again {
    total = total + s.depth
  }
  reached = seen.count * 1000 + again.count
  hops = if seen.has(go.q) { seen[go.q].depth } else { -1 }
  far = total
  for v in 0 .. go.n {
    order.append(Step { node: v * 7 - floor(v * 7 / go.n) * go.n })
  }
  for i in 0 .. go.n {
    if i >= order.count { break }
    let p = order.first + i
    if !marks.has(p.node) { marks.add(Seen { node: p.node * 3 - floor(p.node * 3 / (go.n + go.q)) * (go.n + go.q), depth: i }) }
  }
  marked = marks.count
}
"""

# Items a batch appends at once (APPEND_SOURCE): keys there already, met
# at round q of a DENSE list and at round p of a HASHED one (the
# executor's fault at that round); keys past what a DENSE list holds from
# round p on (it changes its form there, round by round); a list appended
# to twice a round (round by round); columns widened by the items; all
# under memory budgets that run out in the middle of a batch.
APPEND_SOURCE = """record Cell {
  at = 0
  v = 0
}
state items = 0
state last = 0
input go = message {
  n: u32
  q: u32
  p: u32
}
on go {
  let d: list of Cell by at
  let e: list of Cell by at
  let h: list of Cell by at
  let u: list of Cell
  let w: list of Cell
  for i in 0 .. go.n {
    w.append(Cell { at: i, v: i * 300 })
    d.add(Cell { at: i - floor(i / go.q) * go.q, v: i * 0.5 })
  }
  for i in 0 .. go.n {
    e.add(Cell { at: i + floor(i / go.p) * 1000000, v: i })
    h.add(Cell { at: i * 7919 - floor(i * 7919 / go.p) * go.p + 5000000, v: 0 - i })
  }
  for i in 0 .. go.n {
    u.append(Cell { at: i, v: i })
    u.append(Cell { at: 0 - i, v: i * 70000 })
  }
  items = d.count + e.count * 10000 + h.count * 100000000 + u.count + w.count
  let s = 0
  for c in u {
    s = s + c.v
  }
  for c in w {
    s = s + c.v * 2
  }
  last = s
}
"""

def module_specs(aup, pure, work):
    """Specs written in aup (docs/research/140), checked by their own cases
    on Pure's three paths, each path's output the same byte for byte: SHA-256
    (aup's examples/sha256.aup) against hashlib on FIPS 180-2's examples and
    messages of every length up to two blocks; whether bytes are JSON (its
    examples/json.aup) on JSONTestSuite's parsing cases (tests/specs/json):
    every y_ case taken, every n_ case refused (a refusal: valid false, or
    the event failing as encoding on bytes that are not UTF-8)."""
    import hashlib
    import os
    import random
    import struct
    runs = {}
    program = work / 'sha256.pmod'
    compile_module(aup, EDITOR.parent / 'sha256.aup', program)
    chooser = random.Random(140)
    messages = [b'abc', b'abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq',
                b'abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmnoijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu']
    messages += [bytes(chooser.randrange(256) for _ in range(n)) for n in list(range(130)) + [1000, 4096]]
    given = ''.join(f"message hash {m.hex() or '-'}\n" for m in messages)
    outputs = {name: subprocess.run([str(pure / 'build/pure-run'), str(program)], input=given, capture_output=True, text=True,
                                    timeout=120, env=dict(os.environ, **env)).stdout
               for name, env in (('batches', {'PURE_JIT': '0'}), ('rounds', {'PURE_JIT': '0', 'PURE_BATCH': '0'}), ('all', {'PURE_JIT': 'all'}))}
    for name in ('rounds', 'all'):
        assert outputs[name] == outputs['batches'], ('sha256', name)
    digests = [json.loads(line)['states']['digest'] for line in outputs['batches'].splitlines()]
    assert digests == [hashlib.sha256(m).hexdigest() for m in messages], 'sha256'
    runs['sha256'] = len(messages)
    program = work / 'json.pmod'
    compile_module(aup, EDITOR.parent / 'json.aup', program)
    suite = pure / 'tests/specs/json'
    cases = {name: (suite / name).read_bytes() for name in sorted(os.listdir(suite)) if name.endswith('.json')}
    cases['n_structure_100000_opening_arrays.json'] = b'[' * 100000
    cases['n_structure_open_array_object.json'] = b'[{"":' * 50000 + b'\n'
    assert hashlib.sha256(cases['n_structure_100000_opening_arrays.json']).hexdigest().startswith('13f86ea1e7edd116')
    assert hashlib.sha256(cases['n_structure_open_array_object.json']).hexdigest().startswith('48b232fcd18ce2f7')
    for name, data in cases.items():
        (work / name).write_bytes(data)
    given = ''.join(f'message check @{work / name}\n' for name in cases)
    outputs = {name: subprocess.run([str(pure / 'build/pure-run'), str(program)], input=given, capture_output=True, text=True,
                                    timeout=300, env=dict(os.environ, **env)).stdout
               for name, env in (('batches', {'PURE_JIT': '0'}), ('rounds', {'PURE_JIT': '0', 'PURE_BATCH': '0'}), ('all', {'PURE_JIT': 'all'}))}
    for name in ('rounds', 'all'):
        assert outputs[name] == outputs['batches'], ('json', name)
    taken = {'y': 0, 'n': 0, 'i': 0}
    for name, line in zip(cases, outputs['batches'].splitlines()):
        state = json.loads(line)
        accepted = state['fault'] == 'none' and state['states']['valid'] is True
        assert accepted or state['fault'] in ('none', 'encoding'), (name, state['fault'])
        assert accepted == (name[0] == 'y') or name[0] == 'i', (name, accepted)
        taken[name[0]] += accepted
    runs['json'] = {'cases': len(cases), 'taken': taken}
    # A JSON text read into a tree of values by recursive descent (its examples/json_tree.aup, 027's 2b and 2c: a
    # variant, lists in records, match, fns that call themselves), walked depth first for a digest, against Python's
    # reading walked so: JSONTestSuite's cases and random documents.
    class Pairs(list):
        pass

    def digest(data):
        doc = json.loads(data.decode('utf-8'), object_pairs_hook=Pairs)
        d = dict(nulls=0, trues=0, falses=0, numbers=0, strings=0, arrays=0, objects=0, sum=0.0, text_bytes=0, key_bytes=0)

        def walk(x):
            if x is None: d['nulls'] += 1
            elif x is True: d['trues'] += 1
            elif x is False: d['falses'] += 1
            elif isinstance(x, (int, float)): d['numbers'] += 1; d['sum'] += float(x)
            elif isinstance(x, str): d['strings'] += 1; d['text_bytes'] += len(x.encode('utf-8'))
            elif isinstance(x, Pairs):
                d['objects'] += 1
                for k, v in x: d['key_bytes'] += len(k.encode('utf-8')); walk(v)
            else:
                d['arrays'] += 1
                for y in x: walk(y)
        walk(doc)
        return d

    def value(depth):
        pick = chooser.random()
        if depth > 4 or pick < 0.35:
            return chooser.choice([None, True, False, chooser.randrange(-10 ** 6, 10 ** 6), chooser.uniform(-1e3, 1e3),
                                   chooser.choice(['', 'a', 'é ü', 'line\nbreak', 'quote"back\\', '\U0001f600', '\u0001'])])
        if pick < 0.7:
            return [value(depth + 1) for _ in range(chooser.randrange(0, 6))]
        return {f'k{chooser.randrange(50)}' if chooser.random() < 0.8 else '': value(depth + 1) for _ in range(chooser.randrange(0, 6))}

    for k in range(100):
        cases[f'r_{k}.json'] = json.dumps(value(0), ensure_ascii=chooser.random() < 0.5, indent=chooser.choice([None, 1, 2])).encode()
        (work / f'r_{k}.json').write_bytes(cases[f'r_{k}.json'])
    program = work / 'json_tree.pmod'
    compile_module(aup, EDITOR.parent / 'json_tree.aup', program)
    given = ''.join(f'message check @{work / name}\n' for name in cases)
    outputs = {name: subprocess.run([str(pure / 'build/pure-run'), str(program)], input=given, capture_output=True, text=True,
                                    timeout=600, env=dict(os.environ, **env)).stdout
               for name, env in (('batches', {'PURE_JIT': '0'}), ('rounds', {'PURE_JIT': '0', 'PURE_BATCH': '0'}), ('all', {'PURE_JIT': 'all'}))}
    for name in ('rounds', 'all'):
        assert outputs[name] == outputs['batches'], ('json_tree', name)
    keys = ('nulls', 'trues', 'falses', 'numbers', 'strings', 'arrays', 'objects', 'sum', 'text_bytes', 'key_bytes')
    trees = {'y': 0, 'n': 0, 'i': 0, 'r': 0}
    for name, line in zip(cases, outputs['batches'].splitlines()):
        state = json.loads(line)
        accepted = state['fault'] == 'none' and state['states']['valid'] is True
        assert accepted == (name[0] in 'yr') or name[0] == 'i', (name, state['fault'])
        if accepted:
            assert {k: state['states'][k] for k in keys} == digest(cases[name]), name
        trees[name[0]] += accepted
    runs['json_tree'] = {'cases': len(cases), 'taken': trees}
    return runs


def module_assumptions(aup, pure, work):
    """The compiled code's integers (Pure docs/design/022) where they stop
    holding: a loop whose columns hold small whole numbers until, halfway,
    an item with a fraction (and -0) widens them and a fraction's key takes
    a keyed list out of its DENSE form; a product past 2^53; a local's
    products and sum past 2^53 and 2^64 (LOCALS_SOURCE). Each event's
    states, compiled with every loop (PURE_JIT=all) and run by the executor
    alone (PURE_JIT=0), are the same byte for byte."""
    import os
    import struct
    source, module = work / 'assumptions.aup', work / 'assumptions.pmod'
    source.write_text(ASSUMPTIONS_SOURCE)
    compile_module(aup, source, module)
    runs = [(10, 3), (10, 99), (70, 65), (70, 5), (200, 150), (9000, 4000), (9000, 9999), (3, 0)]
    locals_source, locals_module = work / 'locals.aup', work / 'locals.pmod'
    locals_source.write_text(LOCALS_SOURCE)
    compile_module(aup, locals_source, locals_module)
    for program, commands in (
            (module, ''.join(f'message go {struct.pack("<II", n, half).hex()}\n' for n, half in runs)),
            (locals_module, ''.join(f'message go {struct.pack("<I", n).hex()}\n' for n in (3, 70, 5000, 9000)))):
        outputs = {}
        for jit in ('all', '0'):
            done = subprocess.run([str(pure / 'build/pure-run'), str(program)], input=commands, capture_output=True, text=True,
                                  timeout=120, env=dict(os.environ, PURE_JIT=jit))
            assert done.returncode == 0, done.stderr
            outputs[jit] = done.stdout
        assert outputs['all'] == outputs['0'], next((a, b) for a, b in zip(outputs['all'].splitlines(), outputs['0'].splitlines()) if a != b)
        assert all(json.loads(line)['fault'] == 'none' for line in outputs['0'].splitlines())
    messages = ''.join(f'message go {struct.pack("<III", n, m, q).hex()}\nwork\n' for n, m, q in ((70, 40, 3), (130, 25, 2)))
    for name, text in (('merges', MERGES_SOURCE), ('budget', BUDGET_SOURCE), ('chain', CHAIN_SOURCE)):  # the tier's, as the executor
        source, program = work / f'{name}.aup', work / f'{name}.pmod'
        source.write_text(text)
        compile_module(aup, source, program)
        given = ''.join(f'message go {struct.pack("<I", n).hex()}\nwork\n' for n in (3, 50, 60, 100)) if name == 'chain' else messages
        outputs = {jit: subprocess.run([str(pure / 'build/pure-run'), str(program)], input=given + 'budget 1201\n' + given,
                                       capture_output=True, text=True, timeout=120, env=dict(os.environ, PURE_JIT=jit)).stdout
                   for jit in ('all', '0')}
        assert outputs['all'] == outputs['0'], (name, next((a, b) for a, b in zip(outputs['all'].splitlines(), outputs['0'].splitlines()) if a != b))
    source, program = work / 'batch.aup', work / 'batch.pmod'  # batches against the executor's rounds one by one
    source.write_text(BATCH_SOURCE)
    compile_module(aup, source, program)
    given = ''.join(f'message go {struct.pack("<II", n, q).hex()}\nwork\n' for n, q in
                    ((600, 9999), (600, 300), (3000, 2900), (40, 9999), (5, 1), (3000, 70000), (257, 256)))
    given += 'budget 4099\n' + given + 'budget 50000\n' + given
    outputs = {name: subprocess.run([str(pure / 'build/pure-run'), str(program)], input=given, capture_output=True, text=True,
                                    timeout=120, env=dict(os.environ, **env)).stdout
               for name, env in (('batches', {'PURE_JIT': '0'}), ('rounds', {'PURE_JIT': '0', 'PURE_BATCH': '0'}), ('all', {'PURE_JIT': 'all'}))}
    for name in ('rounds', 'all'):
        assert outputs[name] == outputs['batches'], ('batch', name, next((a, b) for a, b in zip(outputs[name].splitlines(), outputs['batches'].splitlines()) if a != b))
    faults = [json.loads(line)['fault'] for line in outputs['batches'].splitlines() if line.startswith('{"states"')]
    assert 'divide' in faults and 'none' in faults and 'work' in faults, faults
    source, program = work / 'recur.aup', work / 'recur.pmod'  # recurrences ahead, against the rounds one by one
    source.write_text(RECUR_SOURCE)
    compile_module(aup, source, program)
    given = ''.join(f'message go {struct.pack("<II", n, q).hex()}\nwork\n' for n, q in ((3000, 70000), (3000, 1500), (90, 70000), (20, 3)))
    given += 'budget 30011\n' + given + 'budget 0\nmemory 20000\n' + given + 'memory 0\n'
    outputs = {name: subprocess.run([str(pure / 'build/pure-run'), str(program)], input=given, capture_output=True, text=True,
                                    timeout=120, env=dict(os.environ, **env)).stdout
               for name, env in (('batches', {'PURE_JIT': '0'}), ('rounds', {'PURE_JIT': '0', 'PURE_BATCH': '0'}), ('all', {'PURE_JIT': 'all'}))}
    for name in ('rounds', 'all'):
        assert outputs[name] == outputs['batches'], ('recur', name, next((a, b) for a, b in zip(outputs[name].splitlines(), outputs['batches'].splitlines()) if a != b))
    faults = [json.loads(line)['fault'] for line in outputs['batches'].splitlines() if line.startswith('{"states"')]
    assert 'divide' in faults and 'none' in faults and 'non_finite' in faults and 'memory' in faults, faults
    source, program = work / 'front.aup', work / 'front.pmod'  # walks in a batch's rounds, against the rounds one by one
    source.write_text(FRONT_SOURCE)
    compile_module(aup, source, program)
    given = ''.join(f'message go {struct.pack("<II", n, q).hex()}\nwork\n' for n, q in
                    ((3000, 300), (3000, 2000), (700, 30001), (40, 30001), (5, 30001), (1500, 6)))
    given += ''.join(f'budget {b}\n' + given for b in (30011, 400000, 1500001)) + 'budget 0\n'
    outputs = {name: subprocess.run([str(pure / 'build/pure-run'), str(program)], input=given, capture_output=True, text=True,
                                    timeout=120, env=dict(os.environ, **env)).stdout
               for name, env in (('batches', {'PURE_JIT': '0'}), ('rounds', {'PURE_JIT': '0', 'PURE_BATCH': '0'}), ('all', {'PURE_JIT': 'all'}))}
    for name in ('rounds', 'all'):
        assert outputs[name] == outputs['batches'], ('front', name, next((a, b) for a, b in zip(outputs[name].splitlines(), outputs['batches'].splitlines()) if a != b))
    faults = [json.loads(line)['fault'] for line in outputs['batches'].splitlines() if line.startswith('{"states"')]
    assert 'divide' in faults and 'none' in faults and 'work' in faults, faults
    source, program = work / 'graph.aup', work / 'graph.pmod'  # filters, against the rounds one by one
    source.write_text(GRAPH_SOURCE)
    compile_module(aup, source, program)
    given = ''.join(f'message go {struct.pack("<IIII", n, m, a, q).hex()}\nwork\n' for n, m, a, q in
                    ((3000, 20000, 1, 7), (3000, 20000, 1, 0), (300, 1200, 0, 5), (2000, 3000, 7, 11), (40, 200, 3, 1)))
    given += ''.join(f'budget {b}\n' + given for b in (200011, 900001)) + 'budget 0\n'
    given += ''.join(f'memory {b}\n' + given for b in (150000, 600000)) + 'memory 0\n'
    outputs = {name: subprocess.run([str(pure / 'build/pure-run'), str(program)], input=given, capture_output=True, text=True,
                                    timeout=300, env=dict(os.environ, **env)).stdout
               for name, env in (('batches', {'PURE_JIT': '0'}), ('rounds', {'PURE_JIT': '0', 'PURE_BATCH': '0'}), ('all', {'PURE_JIT': 'all'}))}
    for name in ('rounds', 'all'):
        assert outputs[name] == outputs['batches'], ('graph', name, next((a, b) for a, b in zip(outputs[name].splitlines(), outputs['batches'].splitlines()) if a != b))
    faults = [json.loads(line)['fault'] for line in outputs['batches'].splitlines() if line.startswith('{"states"')]
    assert 'exists' in faults and 'none' in faults and 'work' in faults and 'memory' in faults, faults
    source, program = work / 'append.aup', work / 'append.pmod'  # items appended at once, against the rounds one by one
    source.write_text(APPEND_SOURCE)
    compile_module(aup, source, program)
    given = ''.join(f'message go {struct.pack("<III", n, q, p).hex()}\nwork\n' for n, q, p in
                    ((3000, 70000, 70000), (3000, 1500, 70000), (3000, 70000, 2000), (3000, 70000, 40), (90, 70000, 70000), (20, 3, 5)))
    given += ''.join(f'memory {b}\n' + given for b in (30000, 90000, 200000)) + 'memory 0\nbudget 30011\n' + given + 'budget 0\n'
    outputs = {name: subprocess.run([str(pure / 'build/pure-run'), str(program)], input=given, capture_output=True, text=True,
                                    timeout=120, env=dict(os.environ, **env)).stdout
               for name, env in (('batches', {'PURE_JIT': '0'}), ('rounds', {'PURE_JIT': '0', 'PURE_BATCH': '0'}), ('all', {'PURE_JIT': 'all'}))}
    for name in ('rounds', 'all'):
        assert outputs[name] == outputs['batches'], ('append', name, next((a, b) for a, b in zip(outputs[name].splitlines(), outputs['batches'].splitlines()) if a != b))
    faults = [json.loads(line)['fault'] for line in outputs['batches'].splitlines() if line.startswith('{"states"')]
    assert 'exists' in faults and 'none' in faults and 'memory' in faults and 'work' in faults, faults
    source, program = work / 'nested.aup', work / 'nested.pmod'  # nested loops together and reductions, against the rounds one by one
    source.write_text(NESTED_SOURCE)
    compile_module(aup, source, program)
    given = ''.join(f'message go {struct.pack("<II", n, q).hex()}\nwork\n' for n, q in
                    ((2000, 9), (2000, 2), (300, 100), (17, 9), (5, 9), (1500, 3)))
    given += 'budget 30011\n' + given + 'budget 400000\n' + given
    outputs = {name: subprocess.run([str(pure / 'build/pure-run'), str(program)], input=given, capture_output=True, text=True,
                                    timeout=120, env=dict(os.environ, **env)).stdout
               for name, env in (('batches', {'PURE_JIT': '0'}), ('rounds', {'PURE_JIT': '0', 'PURE_BATCH': '0'}), ('all', {'PURE_JIT': 'all'}))}
    for name in ('rounds', 'all'):
        assert outputs[name] == outputs['batches'], ('nested', name, next((a, b) for a, b in zip(outputs[name].splitlines(), outputs['batches'].splitlines()) if a != b))
    faults = [json.loads(line)['fault'] for line in outputs['batches'].splitlines() if line.startswith('{"states"')]
    assert 'divide' in faults and 'none' in faults and 'work' in faults, faults
    source, program = work / 'reduce.aup', work / 'reduce.pmod'  # reductions with no lanes yet, against the rounds one by one
    source.write_text(REDUCE_SOURCE)
    compile_module(aup, source, program)
    given = ''.join(f'message go {struct.pack("<II", n, q).hex()}\nwork\n' for n, q in
                    ((3000, 7), (3000, 1), (10000, 3), (40, 3), (2, 5), (1500, 2000)))
    given += 'budget 30011\n' + given + 'budget 0\n'
    outputs = {name: subprocess.run([str(pure / 'build/pure-run'), str(program)], input=given, capture_output=True, text=True,
                                    timeout=120, env=dict(os.environ, **env)).stdout
               for name, env in (('batches', {'PURE_JIT': '0'}), ('rounds', {'PURE_JIT': '0', 'PURE_BATCH': '0'}), ('all', {'PURE_JIT': 'all'}))}
    for name in ('rounds', 'all'):
        assert outputs[name] == outputs['batches'], ('reduce', name, next((a, b) for a, b in zip(outputs[name].splitlines(), outputs['batches'].splitlines()) if a != b))
    faults = [json.loads(line)['fault'] for line in outputs['batches'].splitlines() if line.startswith('{"states"')]
    assert 'none' in faults and 'work' in faults, faults
    source, program = work / 'sums.aup', work / 'sums.pmod'  # sums into cells in whole numbers, against the rounds one by one
    source.write_text(SUMS_SOURCE)
    compile_module(aup, source, program)
    given = ''.join(f'message go {struct.pack("<II", n, q).hex()}\nwork\n' for n, q in
                    ((3000, 7), (3000, 1), (5000, 70000), (100, 3), (40, 2), (2, 3)))
    given += 'budget 30011\n' + given + 'budget 0\n'
    outputs = {name: subprocess.run([str(pure / 'build/pure-run'), str(program)], input=given, capture_output=True, text=True,
                                    timeout=120, env=dict(os.environ, **env)).stdout
               for name, env in (('batches', {'PURE_JIT': '0'}), ('rounds', {'PURE_JIT': '0', 'PURE_BATCH': '0'}), ('all', {'PURE_JIT': 'all'}))}
    for name in ('rounds', 'all'):
        assert outputs[name] == outputs['batches'], ('sums', name, next((a, b) for a, b in zip(outputs[name].splitlines(), outputs['batches'].splitlines()) if a != b))
    faults = [json.loads(line)['fault'] for line in outputs['batches'].splitlines() if line.startswith('{"states"')]
    assert 'none' in faults and 'work' in faults, faults
    source, program = work / 'view.aup', work / 'view.pmod'  # columns read in place, against the rounds one by one
    source.write_text(VIEW_SOURCE)
    compile_module(aup, source, program)
    given = ''.join(f'message go {struct.pack("<II", n, q).hex()}\nwork\n' for n, q in
                    ((3000, 7), (3000, 70000), (2049, 1), (40, 3), (5, 2), (1500, 100000)))
    given += 'budget 30011\n' + given + 'budget 0\n'
    outputs = {name: subprocess.run([str(pure / 'build/pure-run'), str(program)], input=given, capture_output=True, text=True,
                                    timeout=120, env=dict(os.environ, **env)).stdout
               for name, env in (('batches', {'PURE_JIT': '0'}), ('rounds', {'PURE_JIT': '0', 'PURE_BATCH': '0'}), ('all', {'PURE_JIT': 'all'}))}
    for name in ('rounds', 'all'):
        assert outputs[name] == outputs['batches'], ('view', name, next((a, b) for a, b in zip(outputs[name].splitlines(), outputs['batches'].splitlines()) if a != b))
    faults = [json.loads(line)['fault'] for line in outputs['batches'].splitlines() if line.startswith('{"states"')]
    assert 'none' in faults and 'work' in faults, faults
    return len(runs)


def tier_program(rng):
    """A random handler for module_tier: loops over counts and a list,
    numbers in locals and states, a keyed list read and set by key, one
    whose keys came out of order read by key alone (HASHED), one of small
    keys read by key alone (full, or not where keys are left out), ifs and
    breaks, loops whose rounds are apart (a batch's, 024), some with a loop
    run in each and reductions (ra, rb, rc), some appending (e, d), some
    with a recurrence (x1) ahead, walks of rounds apart, with what fails
    (a key not there, not whole, 0 divided by, a cell past its column's
    width) as likely as not."""
    names = iter('jklpqrxyz')
    def number(depth, loops):
        choices = ['const', 'const', 'loop', 'local', 'field']
        if depth < 3:
            choices += ['add', 'sub', 'mul', 'div', 'floor', 'minmax', 'cell', 'add', 'sub', 'hashed', 'keyed']
        kind = rng.choice(choices)
        if kind == 'const':
            return rng.choice(['0', '1', '2', '3', '7', '255', '256', '1000', '0.5', '2.25', '(0 - 3)', '(0 - 0)', '65536'])
        if kind == 'loop' and loops:
            return rng.choice(loops)
        if kind == 'field':
            return rng.choice(['go.n', 'go.m', 'go.q'])
        if kind in ('loop', 'local'):
            return rng.choice(['s', 'u'])
        if kind == 'cell':
            return f't[{key(depth + 1, loops)}].v' if rng.random() < 0.7 else 'c.v' if 'c' in loops else 's'
        if kind == 'hashed':
            return f'h[{hashed_key(depth + 1, loops)}].v'
        if kind == 'keyed':  # g: read by key alone (by a pointer where the key steps with a loop's round)
            return f'g[{key(depth + 1, loops)}].v'
        if kind == 'floor':
            return f'floor({number(depth + 1, loops)} / {rng.choice(["2", "3", "7", "go.q", "u"])})'
        if kind == 'minmax':
            return f'{rng.choice(["min", "max"])}({number(depth + 1, loops)}, {number(depth + 1, loops)})'
        op = {'add': '+', 'sub': '-', 'mul': '*', 'div': '/'}[kind]
        return f'({number(depth + 1, loops)} {op} {number(depth + 1, loops)})'
    def key(depth, loops):
        return rng.choice([rng.choice(loops) if loops else '0', number(depth + 1, loops), 'go.q', '3',
                           f'floor({number(depth + 1, loops)} / 2)'])
    def hashed_key(depth, loops):  # h's keys: i * 7919 mod 1009, past 5,000,000
        k = key(depth, loops)
        return f'(({k}) * 7919 - floor(({k}) * 7919 / 1009) * 1009 + 5000000)' if rng.random() < 0.8 else '5000001'
    def cond(depth, loops):
        kind = rng.choice(['cmp', 'cmp', 'has', 'not', 'and', 'hashed'] if depth < 2 else ['cmp', 'has'])
        if kind == 'cmp':
            return f'{number(depth + 1, loops)} {rng.choice(["<", "<=", ">", ">=", "==", "!="])} {number(depth + 1, loops)}'
        if kind == 'has':
            return f't.has({key(depth + 1, loops)})'
        if kind == 'hashed':
            return f'h.has({hashed_key(depth + 1, loops)})'
        if kind == 'not':
            return f'!({cond(depth + 1, loops)})'
        return f'({cond(depth + 1, loops)}) && ({cond(depth + 1, loops)})'
    def statements(depth, loops, indent, inside):
        lines = []
        for _ in range(rng.randint(1, 3)):
            kind = rng.choice(['s', 's', 'u', 'acc', 'if', 'loop', 'walk', 'set', 'set', 'break', 'sweep', 'lanes', 'lanes', 'nest', 'nest',
                               'recur', 'recur', 'walks', 'walks', 'front', 'front'] if depth < 3 else ['s', 'u', 'set', 'break'])
            pad = '  ' * indent
            if kind == 's':
                lines.append(f'{pad}s = s + {number(1, loops)}')
            elif kind == 'u':
                lines.append(f'{pad}u = {number(1, loops)}')
            elif kind == 'acc':
                lines.append(f'{pad}acc = acc + {number(2, loops)}')
            elif kind == 'set':
                lines.append(f'{pad}t[{key(1, loops)}].v = {number(1, loops)}')
            elif kind == 'break' and inside:
                lines.append(f'{pad}if {cond(1, loops)} {{ break }}')
            elif kind == 'if':
                lines.append(f'{pad}if {cond(1, loops)} {{')
                lines += statements(depth + 1, loops, indent + 1, inside)
                if rng.random() < 0.5:
                    lines.append(f'{pad}}} else {{')
                    lines += statements(depth + 1, loops, indent + 1, inside)
                lines.append(f'{pad}}}')
            elif kind == 'loop':
                name = next(names, None)
                if not name:
                    continue
                lines.append(f'{pad}for {name} in {rng.choice(["0", "1", "go.q"])} .. {rng.choice(["3", "go.n", "go.m", name if False else "5"])} {{')
                lines += statements(depth + 1, loops + [name], indent + 1, True)
                lines.append(f'{pad}}}')
            elif kind == 'sweep':  # keys stepping with a round: read by pointers (023), in range at both ends or not
                name = next(names, None)
                if not name:
                    continue
                low, high = rng.choice([('0', 'go.n'), ('1', 'go.n'), ('0', 'floor(go.n / 2)'), ('go.q', 'go.n'), ('0', 'go.n + 1')])
                affine = rng.choice([name, f'{name} * 2', f'{name} + 3', f'go.n - 1 - {name}', f'{name} * 3 - {name}'])
                lines.append(f'{pad}for {name} in {low} .. {high} {{')
                lines.append(f'{pad}  s = s + {rng.choice(["g", "t", "g"])}[{affine}].v')
                if rng.random() < 0.5:
                    lines += statements(depth + 1, loops + [name], indent + 1, True)
                lines.append(f'{pad}}}')
            elif kind == 'lanes':  # rounds apart (locals each sets first, cells at keys stepping with it): a batch's (024)
                name = next(names, None)
                if not name:
                    continue
                low, high = rng.choice([('0', 'go.n'), ('1', 'go.n'), ('go.q', 'go.n'), ('0', 'go.n + 1'), ('0', 'floor(go.n / 2)')])
                inner = loops + [name, f'w{name}']
                def at():
                    return rng.choice([name, f'{name} + 1', f'go.n - 1 - {name}', f'{name} * 2', f'{name} - 1', key(2, inner)])
                lines.append(f'{pad}for {name} in {low} .. {high} {{')
                lines.append(f'{pad}  let w{name} = {number(1, loops + [name])}')
                for _ in range(rng.randint(1, 3)):
                    pick = rng.random()
                    if pick < 0.15:  # appended round by round (a key there already: EXISTS; past the memory: MEMORY)
                        lines.append(f'{pad}  e.append(Cell {{ at: {at()}, v: {number(1, inner)} }})')
                    elif pick < 0.3:
                        lines.append(f'{pad}  d.add(Cell {{ at: {rng.choice([name, f"{name} * 3", key(2, inner)])}, v: {number(1, inner)} }})')
                    elif pick < 0.5:
                        lines.append(f'{pad}  t[{at()}].v = {number(1, inner)}')
                    elif pick < 0.55:  # a filter at the batch's own level: d asked, then added to
                        k = rng.choice([name, f'{name} * 3', f'w{name}', key(2, inner)])
                        lines.append(f'{pad}  if !d.has({k}) {{ d.add(Cell {{ at: {k}, v: {number(1, inner)} }}) }}')
                    elif pick < 0.6:  # a sum into a cell (its rows read and set by the rounds in turn)
                        cell = at()
                        lines.append(f'{pad}  t[{cell}].v = t[{cell}].v + {number(1, inner)}')
                    else:
                        lines.append(f'{pad}  if {cond(1, inner)} {{ t[{at()}].v = {number(1, inner)} }}' +
                                     (f' else {{ t[{at()}].v = {number(1, inner)} }}' if rng.random() < 0.4 else ''))
                lines.append(f'{pad}}}')
            elif kind == 'nest':  # rounds apart, a loop in each run together, reductions (024's stage 2)
                outer, inner = next(names, None), next(names, None)
                if not inner:
                    continue
                w = f'w{outer}'
                lines.append(f'{pad}for {outer} in {rng.choice(["0", "1", "go.q"])} .. {rng.choice(["go.n", "go.m", "go.n + 3"])} {{')
                lines.append(f'{pad}  let {w} = {number(2, loops + [outer])}')
                high = rng.choice([outer, f'{outer} + 2', 'go.q', '3', f'floor({outer} / 2)', '64'])
                lines.append(f'{pad}  for {inner} in {rng.choice(["0", "1", outer])} .. {high} {{')
                both = loops + [outer, inner, w]
                for _ in range(rng.randint(1, 3)):
                    pick = rng.random()
                    if pick < 0.25:
                        lines.append(f'{pad}    if {cond(1, both)} {{ break }}')
                    elif pick < 0.55:
                        lines.append(f'{pad}    {w} = {w} + {number(2, both)}')
                    elif pick < 0.8:
                        lines.append(f'{pad}    rc = rc + {number(2, both)}')
                    else:
                        lines.append(f'{pad}    if {cond(1, both)} {{ {w} = {number(2, both)} }}')
                lines.append(f'{pad}  }}')
                lines.append(f'{pad}  ra = ra + {w}')
                lines.append(f'{pad}  rb = {rng.choice(["max", "min"])}(rb, {number(2, loops + [outer, w])})')
                lines.append(f'{pad}}}')
            elif kind == 'recur':  # a recurrence ahead of rounds apart: x1 = e of x1 (0 divided by, or past the numbers, in some round)
                name = next(names, None)
                if not name:
                    continue
                step = lambda: rng.choice(['x1 * 31 - floor(x1 * 31 / 1009) * 1009', f'x1 + {name}', 'x1 * 3 + 1', f'x1 / ({name} - 5)',
                                           'max(x1, 7) * 2', f'x1 - floor(x1 / (go.q + 1)) * (go.q + 1) + {name}', 'floor(x1 / 2) + 0.5'])
                lines.append(f'{pad}for {name} in {rng.choice(["0", "1", "go.q"])} .. {rng.choice(["go.n", "go.m", "go.n + 7"])} {{')
                lines.append(f'{pad}  x1 = {step()}')
                lines.append(f'{pad}  let w{name} = x1 - floor(x1 / 7) * 7')
                for _ in range(rng.randint(1, 2)):
                    pick = rng.random()
                    if pick < 0.35:
                        lines.append(f'{pad}  e.append(Cell {{ at: w{name}, v: x1 }})')
                    elif pick < 0.6:
                        lines.append(f'{pad}  t[{rng.choice([name, f"w{name}", f"floor(x1 / 100)"])}].v = x1')
                    elif pick < 0.7:
                        lines.append(f'{pad}  x1 = {step()}')
                    elif pick < 0.8:
                        lines.append(f'{pad}  t[w{name}].v = t[w{name}].v + x1')
                    else:
                        lines.append(f'{pad}  ra = ra + w{name}')
                lines.append(f'{pad}}}')
            elif kind == 'walks' and 'c' not in loops:  # a walk's rounds apart: its items read at their places (024)
                walked = rng.choice(['f', 'g', 'h', 'f', f'g[{rng.choice(["0", "go.q", "3"])} .. {rng.choice(["go.n", "go.m", "50"])}]'])
                inner = loops + ['c']
                lines.append(f'{pad}for c in {walked} {{')
                lines.append(f'{pad}  let wc = {number(1, inner)}')
                for _ in range(rng.randint(1, 3)):
                    pick = rng.random()
                    if pick < 0.3:
                        lines.append(f'{pad}  t[{rng.choice(["c.v", "c.at", "wc", key(2, inner)])}].v = {rng.choice(["wc", "c.v", number(1, inner)])}')
                    elif pick < 0.5:
                        lines.append(f'{pad}  ra = ra + {rng.choice(["c.v", "wc", "c.at"])}')
                    elif pick < 0.65:
                        lines.append(f'{pad}  e.append(Cell {{ at: c.at, v: wc }})')
                    elif pick < 0.8:
                        lines.append(f'{pad}  if {cond(1, inner)} {{ t[{rng.choice(["c.v", "wc"])}].v = c.at }}')
                    else:
                        lines.append(f'{pad}  rb = {rng.choice(["max", "min"])}(rb, {rng.choice(["c.v", "wc"])})')
                lines.append(f'{pad}}}')
            elif kind == 'front' and 'c' not in loops:  # a count's loop to a list's count, a walk in each round (spread apart, or together)
                name = next(names, None)
                if not name:
                    continue
                w = f'w{name}'
                lines.append(f'{pad}for {name} in {rng.choice(["0", "1", "go.q"])} .. {rng.choice(["go.n", "go.m + 40", "go.n * 2"])} {{')
                lines.append(f'{pad}  if {name} >= {rng.choice(["f", "g", "h", "e"])}.count {{ break }}')
                lines.append(f'{pad}  let {w} = {number(2, loops + [name])}')
                walked = rng.choice(['f', 'g', f'g[{name} .. {name} + {rng.choice(["3", w, "go.q"])}]', f'g[{w} .. {name} * 2 + 5]',
                                     f'g[{name} .. {name}]', f'h[5000000 + {name} .. 5000100]', f'f[{name} .. 9]'])
                inner = loops + [name, w, 'c']
                lines.append(f'{pad}  for c in {walked} {{')
                own = filtered = False
                for _ in range(rng.randint(1, 3)):
                    pick = rng.random()
                    if pick < 0.3:
                        lines.append(f'{pad}    ra = ra + {rng.choice(["c.v", "c.at", w, number(2, inner)])}')
                    elif pick < 0.45:
                        lines.append(f'{pad}    rb = {rng.choice(["max", "min"])}(rb, {rng.choice(["c.v", w, number(2, inner)])})')
                    elif pick < 0.65 and not own:  # the item's own local
                        own = True
                        lines.append(f'{pad}    let o{name} = {number(2, inner)}')
                        lines.append(f'{pad}    rc = rc + o{name}')
                    elif pick < 0.75:
                        lines.append(f'{pad}    if {cond(1, inner)} {{ ra = ra + {rng.choice(["c.v", w])} }}')
                    elif pick < 0.92 and not filtered:  # a filter: d asked, then added to (its key, or one that may be there)
                        filtered = True
                        k = rng.choice(['c.v', 'c.at', f'floor(c.v / 2)', f'c.v + {w}'])
                        added = k if rng.random() < 0.8 else f'{k} + 1'
                        lines.append(f'{pad}    if !d.has({k}) {{')
                        lines.append(f'{pad}      d.add(Cell {{ at: {added}, v: {rng.choice([w, "c.at", number(2, inner)])} }})')
                        if rng.random() < 0.5:
                            lines.append(f'{pad}      e.append(Cell {{ at: {k}, v: d[{rng.choice(["c.v", "0", w])}].v }})')
                        lines.append(f'{pad}    }}')
                    else:
                        lines.append(f'{pad}    if {cond(1, inner)} {{ break }}')
                lines.append(f'{pad}  }}')
                lines.append(f'{pad}  rc = rc + {w}')
                lines.append(f'{pad}}}')
            elif kind == 'walk' and 'c' not in loops:
                lines.append(f'{pad}for c in f {{')
                lines += statements(depth + 1, loops + ['c'], indent + 1, True)
                lines.append(f'{pad}}}')
        return lines
    body = statements(0, ['i'], 2, True)
    return '\n'.join([
        'record Cell {', '  at = 0', '  v = 0', '}', 'state out = 0', 'state acc = 0',
        'input go = message {', '  n: u32', '  m: u32', '  q: u32', '}',
        'on go {', '  let t: list of Cell by at', '  let f: list of Cell', '  let h: list of Cell by at', '  let g: list of Cell by at',
        '  let e: list of Cell', '  let d: list of Cell by at',
        '  for i in 0 .. go.n {', f'    t.add(Cell {{ at: i, v: {rng.choice(["i", "0", "(i * 3)", "1"])} }})',
        '    f.append(Cell { at: i * 2, v: i })', '    h.add(Cell { at: i * 7919 - floor(i * 7919 / 1009) * 1009 + 5000000, v: i * 5 })',
        f'    g.add(Cell {{ at: {rng.choice(["i", "i", "i + floor(i / 40)"])}, v: i * 3 }})', '  }',
        '  let s = 0', '  let u = 1', '  let ra = 0', '  let rb = 0', '  let rc = 0', '  let x1 = 1',
        '  for i in 0 .. go.m {', *body, '  }',
        '  out = s + ra + rb + rc + e.count + d.count + x1', '  acc = acc + u', '}', ''])


def module_tier(aup, pure, work, programs=150):
    """The tier (Pure docs/design/023) against the executor: random handlers
    (tier_program) whose loops the tier runs where it can, each run on a few
    messages by the executor alone (PURE_JIT=0, its loops in batches where
    they can, docs/design/024), without batches (PURE_BATCH=0), with the
    compiled code (PURE_JIT=1, all) and without the tier (PURE_TIER=0): the same states,
    faults, errors and work, byte for byte, with the default work budget
    and a few small ones (where each runs out). Their failures (keys not there, not
    whole, 0 divided by, cells past their columns' widths, a list not yet
    DENSE) are where the tier gives up to the executor."""
    import os
    import random
    import struct
    rng = random.Random(23)
    runs = [(0, 0, 0), (5, 3, 1), (70, 40, 3), (130, 25, 2), (300, 7, 299), (70, 70, 64)]
    messages = ''.join(f'message go {struct.pack("<III", n, m, q).hex()}\nwork\n' for n, m, q in runs)
    commands = (messages + ''.join(f'budget {b}\n' + messages for b in (61, 389, 1201, 4099)) + 'budget 0\n' + messages +
                'memory 6000\n' + messages + 'memory 0\n')
    checked = failed = 0
    for i in range(programs):
        source, module = work / f'tier-{i}.aup', work / f'tier-{i}.pmod'
        source.write_text(tier_program(rng))
        try:
            compile_module(aup, source, module)
        except subprocess.CalledProcessError:
            continue  # not a program aup takes (its types): another
        outputs = {}
        for name, env in (('0', {'PURE_JIT': '0'}), ('1', {'PURE_JIT': '1'}), ('all', {'PURE_JIT': 'all'}),
                          ('tierless', {'PURE_JIT': 'all', 'PURE_TIER': '0'}), ('batchless', {'PURE_JIT': '0', 'PURE_BATCH': '0'})):
            done = subprocess.run([str(pure / 'build/pure-run'), str(module)], input=commands, capture_output=True, text=True,
                                  timeout=120, env=dict(os.environ, **env))
            assert done.returncode in (0, 1), (i, name, done.stderr)
            outputs[name] = done.stdout
        for name in ('1', 'all', 'tierless', 'batchless'):
            assert outputs[name] == outputs['0'], (i, name, source.read_text(),
                                                   next((a, b) for a, b in zip(outputs[name].splitlines(), outputs['0'].splitlines()) if a != b))
        checked += 1
        failed += sum(json.loads(line).get('fault', 'none') != 'none' for line in outputs['0'].splitlines() if line.startswith('{'))
    assert checked >= programs // 2, checked
    return {'programs': checked, 'failed_events': failed}


def server_links_refused(aup, pure, work):
    """A link to an input the receiver's app does not have: the receiver
    says so (REFUSED), and the sender says it once on stderr, as a wire says
    a receiver does not take its sends, and tries a packet a wait instead of
    sending its records again every second."""
    import select
    import socket
    import time
    sender_module, receiver_module = work / 'refused-sender.pmod', work / 'refused-receiver.pmod'
    for module, text in ((sender_module, LINK_SENDER), (receiver_module, LINK_RECEIVER)):
        source = module.with_suffix('.aup')
        source.write_text(text)
        compile_module(aup, source, module)

    def free_port(kind=socket.SOCK_STREAM):
        with socket.socket(socket.AF_INET, kind) as probe:
            probe.bind(('127.0.0.1', 0))
            return probe.getsockname()[1]

    def start(args, port):
        process = subprocess.Popen([str(a) for a in args] + ['--port', str(port)], stdout=subprocess.PIPE,
                                   stderr=subprocess.PIPE)
        ready, _, _ = select.select([process.stdout], [], [], 10)
        line = process.stdout.readline() if ready else b''
        assert line, f'host did not start: {process.stderr.read(2000) if process.poll() is not None else ""}'
        return process, json.loads(line)

    keys = {}
    for name in ('sender', 'receiver'):
        process, info = start([pure / 'build/pure-serve', '--module', receiver_module, '--store', work / 'refused-key',
                               '--key', work / f'refused-{name}.key'], 0)
        keys[name] = info['public_key']
        process.terminate()
        process.communicate(timeout=30)
    port_a, port_b, link_port = free_port(), free_port(), free_port(socket.SOCK_DGRAM)
    receiver = start([pure / 'build/pure-serve', '--module', receiver_module, '--store', work / 'refused-b',
                      '--key', work / 'refused-receiver.key', '--link-port', link_port,
                      '--sender', f'refused-sender.ordered={keys["sender"]}'], port_b)[0]
    sender = start([pure / 'build/pure-serve', '--module', sender_module, '--store', work / 'refused-a',
                    '--key', work / 'refused-sender.key', '--send', f'ordered=pure://127.0.0.1:{link_port}/nosuch',
                    '--seal', f'ordered={keys["receiver"]}'], port_a)[0]
    said = []
    try:
        import http.client
        connection = http.client.HTTPConnection('127.0.0.1', port_a, timeout=10)
        body = json.dumps({'item': 'x', 'count': 1}).encode()
        connection.request('POST', '/inputs/buy', body, {'Content-Length': str(len(body)), 'Content-Type': 'application/json'})
        assert connection.getresponse().status == 200
        connection.close()
        deadline = time.time() + 10
        while not said and time.time() < deadline:
            ready, _, _ = select.select([sender.stderr], [], [], 0.1)
            if ready:
                line = sender.stderr.readline().decode()
                if line.startswith('{"link"'):
                    said.append(json.loads(line))
        assert said and said[0]['error'].startswith('the receiver refuses the stream: it has no input of that name'), said
        time.sleep(2.5)  # tried at 1 s and at 3 s: not every second, and said once
    finally:
        stats = {}
        for name, process in (('a', sender), ('b', receiver)):
            process.terminate()
            _, error = process.communicate(timeout=30)
            assert process.returncode == 0, error[-1000:]
            lines = error.decode().splitlines()
            stats[name] = [json.loads(line) for line in lines if line.startswith('{"active"')][-1]
            if name == 'a':
                said += [json.loads(line) for line in lines if line.startswith('{"link"')]
    assert len(said) == 1, said
    assert stats['a']['link_packets'] <= 8 and stats['a']['link_records'] <= 4, stats['a']
    assert stats['a']['link_refused'] >= 2 and stats['b']['listener_refused'] >= 2, (stats['a'], stats['b'])
    return {'said': said[0]['error'], 'packets': stats['a']['link_packets']}



MEMORY_RECEIVER = """record Order {
  item: text
  count = 0
}
state kept: list of Order
state got = 0
input order = message {
  item: text
  count: u32
  to: {
    city: text
  }
}
input clean = message {
  below: u32
}
on order {
  kept.append(Order { item: order.item, count: order.count })
  got = got + 1
}
on clean {
  for o in kept where o.count < clean.below {
    kept.remove(o)
  }
}
view count = got
view held = kept.count
"""


def server_memory(aup, pure, work):
    """What an app holds, bounded (--memory, docs/design/013-memory-budget.md).
    A sender whose link's receiver is down fills its outbox: its next buy is
    refused, 503 memory, and changes nothing, while its views still read;
    once the receiver is up and stores what waited, it takes buys again. The
    receiver, started again with a budget below what it keeps, refuses the
    link's deliveries (and a client's order, 503 memory) but not the clean
    that frees them; meanwhile its link hands the records over a few times a
    second, not at every turn, and once freed it takes them all, once."""
    import select
    import socket
    import time
    sender_module, receiver_module = work / 'memory-sender.pmod', work / 'memory-receiver.pmod'
    for module, text in ((sender_module, LINK_SENDER), (receiver_module, MEMORY_RECEIVER)):
        source = module.with_suffix('.aup')
        source.write_text(text)
        compile_module(aup, source, module)

    def free_port(kind=socket.SOCK_STREAM):
        with socket.socket(socket.AF_INET, kind) as probe:
            probe.bind(('127.0.0.1', 0))
            return probe.getsockname()[1]

    def start(args, port):
        process = subprocess.Popen([str(a) for a in args] + ['--port', str(port)], stdout=subprocess.PIPE,
                                   stderr=subprocess.PIPE)
        ready, _, _ = select.select([process.stdout], [], [], 10)
        line = process.stdout.readline() if ready else b''
        assert line, f'host did not start: {process.stderr.read(2000) if process.poll() is not None else ""}'
        return process, json.loads(line)

    def stop(process):
        process.terminate()
        _, error = process.communicate(timeout=30)
        assert process.returncode == 0, error[-1000:]
        return [json.loads(line) for line in error.decode().splitlines() if line.startswith('{"active"')][-1]

    def number(port, view):
        return int(http(port, 'GET', f'/views/{view}')[3])

    def until(test, seconds):  # test once more at most after the deadline: it may post
        deadline = time.time() + seconds
        while not test():
            if time.time() > deadline:
                return False
            time.sleep(0.02)
        return True

    keys = {}
    for name in ('sender', 'receiver'):
        process, info = start([pure / 'build/pure-serve', '--module', receiver_module, '--store', work / 'memory-key',
                               '--key', work / f'memory-{name}.key'], 0)
        keys[name] = info['public_key']
        stop(process)
    port_a, port_b, link_port = free_port(), free_port(), free_port(socket.SOCK_DGRAM)
    receiving = [pure / 'build/pure-serve', '--module', receiver_module, '--store', work / 'memory-b',
                 '--key', work / 'memory-receiver.key', '--link-port', link_port,
                 '--sender', f'memory-sender.ordered={keys["sender"]}']
    sender = start([pure / 'build/pure-serve', '--module', sender_module, '--store', work / 'memory-a',
                    '--key', work / 'memory-sender.key', '--send', f'ordered=pure://127.0.0.1:{link_port}/order',
                    '--seal', f'ordered={keys["receiver"]}', '--memory', 1000], port_a)[0]
    receiver, stats, item = None, {}, 'x' * 40
    try:
        accepted, refused = 0, None
        while accepted < 200 and not refused:
            status = post(port_a, '/inputs/buy', {'item': item, 'count': 1})
            accepted += status[0] == 200
            refused = status if status[0] != 200 else None
        assert refused == (503, 'memory') and 3 <= accepted < 200, (accepted, refused)
        assert number(port_a, 'sent') == accepted
        receiver = start(receiving, port_b)[0]
        assert until(lambda: number(port_b, 'count') == accepted, 20), number(port_b, 'count')
        assert until(lambda: post(port_a, '/inputs/buy', {'item': item, 'count': 1})[0] == 200, 10)
        assert until(lambda: number(port_b, 'count') == accepted + 1, 10)
        stop(receiver)
        receiver = start(receiving + ['--memory', 300], port_b)[0]  # below what it keeps
        order = {'item': item, 'count': 1, 'to': {'city': 'Seoul'}}
        assert post(port_b, '/inputs/order', order) == (503, 'memory')
        for _ in range(3):
            assert post(port_a, '/inputs/buy', {'item': item, 'count': 1})[0] == 200
        time.sleep(2.0)
        assert number(port_b, 'count') == accepted + 1 and number(port_a, 'sent') == accepted + 4
        assert post(port_b, '/inputs/clean', {'below': 100})[0] == 200
        assert until(lambda: number(port_b, 'count') == accepted + 4, 5), number(port_b, 'count')
        assert number(port_b, 'held') == 3
    finally:
        for name, process in (('b', receiver), ('a', sender)):
            if process:
                stats[name] = stop(process)
    assert stats['a']['memory_refused'] >= 1 and stats['a']['state_bytes'] <= 1000, stats['a']
    assert 2 <= stats['b']['memory_refused'] <= 40 and stats['b']['state_bytes'] <= 300, stats['b']
    return {'sends_held': accepted, 'receiver_refused': stats['b']['memory_refused'],
            'link_resent': stats['a']['link_resent']}


WINDOW_SENDER = """state n = 0
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
output copied = message {
  item: text
  count: u32
  to: {
    city: text
  }
}
on buy {
  n = n + 1
  send ordered { item: buy.item, count: buy.count, to: { city: "Seoul" } }
  send copied { item: buy.item, count: buy.count, to: { city: "Busan" } }
}
view sent = n
"""


def server_windows(aup, pure, work):
    """Each --send holds a window of its sends (--window bytes past what its
    receiver has), not the whole outbox (docs/design/013-memory-budget.md,
    step 2). A sender's two outputs, one linked (pure://) and one over HTTP,
    both receivers down: 300 buys wait, each outlet holding some 2,000
    bytes of them. When the receivers come up, the outlets take the rest
    from the outbox as their receivers store what they hold, and each
    receiver applies all 300, once each."""
    import select
    import socket
    import time
    sender_module, receiver_module = work / 'window-sender.pmod', work / 'window-receiver.pmod'
    for module, text in ((sender_module, WINDOW_SENDER), (receiver_module, LINK_RECEIVER)):
        source = module.with_suffix('.aup')
        source.write_text(text)
        compile_module(aup, source, module)

    def free_port(kind=socket.SOCK_STREAM):
        with socket.socket(socket.AF_INET, kind) as probe:
            probe.bind(('127.0.0.1', 0))
            return probe.getsockname()[1]

    def start(args, port):
        process = subprocess.Popen([str(a) for a in args] + ['--port', str(port)], stdout=subprocess.PIPE,
                                   stderr=subprocess.PIPE)
        ready, _, _ = select.select([process.stdout], [], [], 10)
        line = process.stdout.readline() if ready else b''
        assert line, f'host did not start: {process.stderr.read(2000) if process.poll() is not None else ""}'
        return process, json.loads(line)

    def stop(process):
        process.terminate()
        _, error = process.communicate(timeout=30)
        assert process.returncode == 0, error[-1000:]
        return [json.loads(line) for line in error.decode().splitlines() if line.startswith('{"active"')][-1]

    def number(port, view):
        return int(http(port, 'GET', f'/views/{view}')[3])

    keys = {}
    for name in ('sender', 'receiver'):
        process, info = start([pure / 'build/pure-serve', '--module', receiver_module, '--store', work / 'window-key',
                               '--key', work / f'window-{name}.key'], 0)
        keys[name] = info['public_key']
        stop(process)
    port_a, port_b, port_c, link_port = free_port(), free_port(), free_port(), free_port(socket.SOCK_DGRAM)
    sender = start([pure / 'build/pure-serve', '--module', sender_module, '--store', work / 'window-a',
                    '--key', work / 'window-sender.key', '--window', 2000,
                    '--send', f'ordered=pure://127.0.0.1:{link_port}/order', '--seal', f'ordered={keys["receiver"]}',
                    '--send', f'copied=http://127.0.0.1:{port_c}/inputs/order'], port_a)[0]
    receivers, stats, total = [], {}, 0
    try:
        for i in range(300):
            count = i % 50 + 1
            assert post(port_a, '/inputs/buy', {'item': f'i{i:03}', 'count': count})[0] == 200
            total += count
        time.sleep(0.5)
        receivers.append(start([pure / 'build/pure-serve', '--module', receiver_module, '--store', work / 'window-b',
                                '--key', work / 'window-receiver.key', '--link-port', link_port,
                                '--sender', f'window-sender.ordered={keys["sender"]}'], port_b)[0])
        receivers.append(start([pure / 'build/pure-serve', '--module', receiver_module, '--store', work / 'window-c'],
                               port_c)[0])
        deadline = time.time() + 60
        while (number(port_b, 'count') < 300 or number(port_c, 'count') < 300) and time.time() < deadline:
            time.sleep(0.05)
        for port in (port_b, port_c):
            assert number(port, 'count') == 300 and number(port, 'total') == total, (port, number(port, 'count'))
    finally:
        for name, process in zip(('b', 'c'), receivers):
            stats[name] = stop(process)
        stats['a'] = stop(sender)
    assert stats['a']['windows_full'] >= 1, stats['a']
    return {'windows_full': stats['a']['windows_full'], 'wire_deliveries': stats['a']['delivered'],
            'link_records': stats['a']['link_records']}


UPGRADED_SENDER = """state n = 0
input buy = message {
  item: text
  count: u32
}
output ordered = message {
  note: text
  item: text
  count: u32
}
on buy {
  n = n + 1
  send ordered { note: "second", item: buy.item, count: buy.count }
}
view sent = n
"""

UPGRADED_RECEIVER = """state orders = 0
state noted = 0
state stock = 0
input order = message {
  item: text
  count: u32
  note: text
}
on order {
  orders = orders + 1
  stock = stock + order.count
  if order.note == "second" { noted = noted + 1 }
}
view count = orders
view noted_count = noted
view total = stock
"""


def server_upgrade(aup, pure, work):
    """A rolling upgrade of an output's fields between two linked nodes. The
    sender, its receiver down, holds five sends of ordered {item, count,
    to.city}; started again with ordered {note, item, count} it recasts them
    by name (note empty, to.city gone) where it was refused before (review
    004's F3), and a module without the output is refused naming it. The old
    receiver lacks note, so it refuses them (it would drop the field) and
    they wait; the upgraded receiver, its fields in another order, takes all
    eight by name, once each."""
    import select
    import socket
    import time
    modules = {}
    for name, text in (('sender-1', LINK_SENDER), ('sender-2', UPGRADED_SENDER), ('receiver-1', LINK_RECEIVER),
                       ('receiver-2', UPGRADED_RECEIVER), ('sender-0', 'state n = 0\ninput tap = key KeyT\non tap.down { n = 1 }\n')):
        source = work / f'upgrade-{name}.aup'
        source.write_text(text)
        modules[name] = compile_module(aup, source, source.with_suffix('.pmod')) and source.with_suffix('.pmod')

    def free_port(kind=socket.SOCK_STREAM):
        with socket.socket(socket.AF_INET, kind) as probe:
            probe.bind(('127.0.0.1', 0))
            return probe.getsockname()[1]

    def start(args, port):
        process = subprocess.Popen([str(a) for a in args] + ['--port', str(port)], stdout=subprocess.PIPE,
                                   stderr=subprocess.PIPE)
        ready, _, _ = select.select([process.stdout], [], [], 10)
        line = process.stdout.readline() if ready else b''
        assert line, f'host did not start: {process.stderr.read(2000) if process.poll() is not None else ""}'
        return process, json.loads(line)

    def stop(process):
        process.terminate()
        _, error = process.communicate(timeout=30)
        assert process.returncode == 0, error[-1000:]
        return [json.loads(line) for line in error.decode().splitlines() if line.startswith('{"active"')][-1]

    def number(port, view):
        return int(http(port, 'GET', f'/views/{view}')[3])

    keys = {}
    for name in ('sender', 'receiver'):
        process, info = start([pure / 'build/pure-serve', '--module', modules['receiver-1'], '--store',
                               work / 'upgrade-key', '--key', work / f'upgrade-{name}.key'], 0)
        keys[name] = info['public_key']
        stop(process)
    port_a, port_b, link_port = free_port(), free_port(), free_port(socket.SOCK_DGRAM)

    def sender_args(module):
        return [pure / 'build/pure-serve', '--module', modules[module], '--store', work / 'upgrade-a', '--key',
                work / 'upgrade-sender.key', '--name', 'up', '--send', f'ordered=pure://127.0.0.1:{link_port}/order',
                '--seal', f'ordered={keys["receiver"]}']

    def receiver_args(module):
        return [pure / 'build/pure-serve', '--module', modules[module], '--store', work / 'upgrade-b', '--key',
                work / 'upgrade-receiver.key', '--link-port', link_port, '--sender', f'up.ordered={keys["sender"]}']

    running, stats, said = {}, {}, ''
    try:
        running['a'] = start(sender_args('sender-1'), port_a)[0]
        for i in range(5):
            assert post(port_a, '/inputs/buy', {'item': f'first-{i}', 'count': 1})[0] == 200
        stop(running.pop('a'))
        refused = subprocess.run([str(a) for a in sender_args('sender-0')] + ['--port', str(port_a)],
                                 capture_output=True, timeout=30)
        assert refused.returncode != 0 and b"sends wait on the output 'ordered'" in refused.stderr, refused.stderr
        running['a'] = start(sender_args('sender-2'), port_a)[0]
        for i in range(3):
            assert post(port_a, '/inputs/buy', {'item': f'second-{i}', 'count': 10})[0] == 200
        running['b'] = start(receiver_args('receiver-1'), port_b)[0]
        time.sleep(1.5)
        assert number(port_b, 'count') == 0  # the old input takes another shape: the sends wait
        stop(running.pop('b'))
        running['b'] = start(receiver_args('receiver-2'), port_b)[0]
        deadline = time.time() + 20
        while number(port_b, 'count') < 8 and time.time() < deadline:
            time.sleep(0.05)
        got = (number(port_b, 'count'), number(port_b, 'noted_count'), number(port_b, 'total'))
        assert got == (8, 3, 35), got
    finally:
        for name, process in running.items():
            stats[name] = stop(process)
    assert stats['a']['link_refused'] >= 1, stats['a']
    return {'recast_waiting': 5, 'delivered': 8, 'refused_while_old': stats['a']['link_refused']}


FIELDS_SENDER = """input buy = message {
  item: text
  count: u32
}
output swapped = message {
  count: u32
  item: text
}
output plain = message {
  item: text
  count: u32
}
output noted = message {
  item: text
  count: u32
  note: text
}
output over_http = message {
  item: text
  count: u32
}
output noted_http = message {
  item: text
  count: u32
  note: text
}
on buy {
  send swapped { count: buy.count, item: buy.item }
  send plain { item: buy.item, count: buy.count }
  send noted { item: buy.item, count: buy.count, note: "kept" }
  send over_http { item: buy.item, count: buy.count }
  send noted_http { item: buy.item, count: buy.count, note: "kept" }
}
"""


def fields_receiver(narrow_notes):
    """Inputs for FIELDS_SENDER's outputs; narrow ones lack note unless narrow_notes."""
    note = '  note: text\n' if narrow_notes else ''
    kept = lambda name: f'  if {name}.note == "kept" {{ {name}_kept = {name}_kept + 1 }}\n' if narrow_notes else ''
    return (f"""state swapped_n = 0
state swapped_total = 0
state wider_n = 0
state wider_blank = 0
state narrow_n = 0
state narrow_kept = 0
state wider_http_n = 0
state wider_http_blank = 0
state narrow_http_n = 0
state narrow_http_kept = 0
input swapped_in = message {{
  item: text
  count: u32
}}
input wider = message {{
  note: text
  item: text
  count: u32
}}
input narrow = message {{
  item: text
  count: u32
{note}}}
input wider_http = message {{
  item: text
  count: u32
  note: text
}}
input narrow_http = message {{
  item: text
  count: u32
{note}}}
on swapped_in {{
  swapped_n = swapped_n + 1
  swapped_total = swapped_total + swapped_in.count
}}
on wider {{
  wider_n = wider_n + 1
  if wider.note == "" {{ wider_blank = wider_blank + 1 }}
}}
on narrow {{
  narrow_n = narrow_n + 1
{kept('narrow')}}}
on wider_http {{
  wider_http_n = wider_http_n + 1
  if wider_http.note == "" {{ wider_http_blank = wider_http_blank + 1 }}
}}
on narrow_http {{
  narrow_http_n = narrow_http_n + 1
{kept('narrow_http')}}}
view counts = "{{swapped_n}} {{swapped_total}} {{wider_n}} {{wider_blank}} {{narrow_n}} {{narrow_kept}} {{wider_http_n}} {{wider_http_blank}} {{narrow_http_n}} {{narrow_http_kept}}"
""")


def server_fields(aup, pure, work):
    """Deliveries take their fields by name (docs/design/014-fields-by-name.md).
    A sender's five outputs, three linked and two over HTTP, to one receiver:
    fields in another order are taken; an input with a field more takes
    messages without it (empty); an input lacking a field the output sends
    refuses it, over the link (REFUSED, fields lacked) and over HTTP
    (json_field), so nothing is dropped, and once the receiver starts again
    with the field, every waiting message comes, the field kept."""
    import select
    import socket
    import time
    modules = {}
    for name, text in (('sender', FIELDS_SENDER), ('receiver-1', fields_receiver(False)),
                       ('receiver-2', fields_receiver(True))):
        source = work / f'fields-{name}.aup'
        source.write_text(text)
        compile_module(aup, source, source.with_suffix('.pmod'))
        modules[name] = source.with_suffix('.pmod')

    def free_port(kind=socket.SOCK_STREAM):
        with socket.socket(socket.AF_INET, kind) as probe:
            probe.bind(('127.0.0.1', 0))
            return probe.getsockname()[1]

    def start(args, port):
        process = subprocess.Popen([str(a) for a in args] + ['--port', str(port)], stdout=subprocess.PIPE,
                                   stderr=subprocess.PIPE)
        ready, _, _ = select.select([process.stdout], [], [], 10)
        line = process.stdout.readline() if ready else b''
        assert line, f'host did not start: {process.stderr.read(2000) if process.poll() is not None else ""}'
        return process, json.loads(line)

    def stop(process):
        process.terminate()
        _, error = process.communicate(timeout=30)
        assert process.returncode == 0, error[-1000:]
        return error.decode()

    def counts(port):
        return [int(n) for n in http(port, 'GET', '/views/counts')[3].split()]

    keys = {}
    for name in ('sender', 'receiver'):
        process, info = start([pure / 'build/pure-serve', '--module', modules['receiver-1'], '--store',
                               work / 'fields-key', '--key', work / f'fields-{name}.key'], 0)
        keys[name] = info['public_key']
        stop(process)
    port_a, port_b, link_port = free_port(), free_port(), free_port(socket.SOCK_DGRAM)
    links = {'swapped': 'swapped_in', 'plain': 'wider', 'noted': 'narrow'}
    sending = [pure / 'build/pure-serve', '--module', modules['sender'], '--store', work / 'fields-a', '--key',
               work / 'fields-sender.key', '--name', 'up',
               '--send', f'over_http=http://127.0.0.1:{port_b}/inputs/wider_http',
               '--send', f'noted_http=http://127.0.0.1:{port_b}/inputs/narrow_http',
               '--seal', f'over_http={keys["receiver"]}', '--seal', f'noted_http={keys["receiver"]}']
    for output, input_name in links.items():
        sending += ['--send', f'{output}=pure://127.0.0.1:{link_port}/{input_name}', '--seal', f'{output}={keys["receiver"]}']

    def receiving(module):
        args = [pure / 'build/pure-serve', '--module', modules[module], '--store', work / 'fields-b', '--key',
                work / 'fields-receiver.key', '--link-port', link_port]
        for output in (*links, 'over_http', 'noted_http'):
            args += ['--sender', f'up.{output}={keys["sender"]}']
        return args

    running, said = {}, ''
    try:
        running['b'] = start(receiving('receiver-1'), port_b)[0]
        running['a'] = start(sending, port_a)[0]
        for i in range(3):
            assert post(port_a, '/inputs/buy', {'item': f'i{i}', 'count': i + 1})[0] == 200
        deadline = time.time() + 20
        while counts(port_b)[:4] + counts(port_b)[6:8] != [3, 6, 3, 3, 3, 3] and time.time() < deadline:
            time.sleep(0.05)
        got = counts(port_b)
        assert got == [3, 6, 3, 3, 0, 0, 3, 3, 0, 0], got  # the narrow inputs took none: nothing is dropped
        time.sleep(1.5)  # the refused link tried with its fields named: the sender has said why
        stop(running.pop('b'))
        running['b'] = start(receiving('receiver-2'), port_b)[0]
        deadline = time.time() + 30
        while counts(port_b)[4:6] + counts(port_b)[8:] != [3, 3, 3, 3] and time.time() < deadline:
            time.sleep(0.1)
        got = counts(port_b)
        assert got == [3, 6, 3, 3, 3, 3, 3, 3, 3, 3], got
    finally:
        for name, process in running.items():
            text = stop(process)
            said += text if name == 'a' else ''
    assert 'its input lacks fields this output sends' in said, said[-2000:]
    return {'links': 3, 'wires': 2, 'waited_then_taken': 6}

def server_links_independent(aup, pure, work):
    """A receiver written from docs/design/012-link-protocol.md alone, in
    Python with the cryptography package and no code of link/: a Pure node
    links to it (pure://). It checks the node's address with a CHALLENGE,
    opens the node's packets, takes its records in order, answers with ACK
    and STORED, and accepts its epoch 1, so the node goes on in the epoch
    the two made. In the end the node knows every order it saved stored,
    and some of them came sealed in epoch 1. None where the cryptography
    package is not there."""
    try:
        from cryptography.hazmat.primitives import hashes, serialization
        from cryptography.hazmat.primitives.asymmetric.x25519 import X25519PrivateKey, X25519PublicKey
        from cryptography.hazmat.primitives.ciphers.aead import AESGCM
        from cryptography.hazmat.primitives.kdf.hkdf import HKDF
    except ImportError:
        return None
    import os
    import select
    import socket
    import struct
    import threading
    import time

    def hkdf(secret, info, size):
        return HKDF(algorithm=hashes.SHA256(), length=size, salt=b'pure link 0', info=info).derive(secret)

    def raw(key):
        return key.public_key().public_bytes(serialization.Encoding.Raw, serialization.PublicFormat.Raw)

    def key_id(public):
        h = 2166136261
        for byte in public:
            h = (h ^ byte) * 16777619 & 0xffffffff
        return h

    def packet_keys(secret, sender, receiver, sender_run, receiver_run):
        out = hkdf(secret, b'pure link 0 keys' + sender + receiver + struct.pack('<QQ', sender_run, receiver_run), 28)
        return AESGCM(out[:16]), out[16:]

    def nonce(base, pn):
        return base[:4] + bytes(a ^ b for a, b in zip(base[4:], struct.pack('>Q', pn)))

    def names(body, at):
        a = body[at]
        b = body[at + 1 + a]
        return body[at + 1:at + 1 + a], body[at + 2 + a:at + 2 + a + b], at + 2 + a + b

    def varint(body, at):
        value = shift = 0
        while True:
            byte = body[at]
            value |= (byte & 0x7f) << shift
            at += 1
            if not byte & 0x80:
                return value, at
            shift += 7

    module = work / 'independent-sender.pmod'
    source = module.with_suffix('.aup')
    source.write_text(LINK_SENDER)
    compile_module(aup, source, module)

    def start(args, port):
        process = subprocess.Popen([str(a) for a in args] + ['--port', str(port)], stdout=subprocess.PIPE,
                                   stderr=subprocess.PIPE)
        ready, _, _ = select.select([process.stdout], [], [], 10)
        line = process.stdout.readline() if ready else b''
        assert line, f'host did not start: {process.stderr.read(2000) if process.poll() is not None else ""}'
        return process, json.loads(line)

    mine = X25519PrivateKey.generate()
    my_public = raw(mine)
    link = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    link.bind(('127.0.0.1', 0))
    with socket.socket() as probe:
        probe.bind(('127.0.0.1', 0))
        port = probe.getsockname()[1]
    node, info = start([pure / 'build/pure-serve', '--module', module, '--store', work / 'independent-a',
                        '--key', work / 'independent.key', '--send',
                        f'ordered=pure://127.0.0.1:{link.getsockname()[1]}/order', '--seal', f'ordered={my_public.hex()}'],
                       port)
    theirs = bytes.fromhex(info['public_key'])
    static = mine.exchange(X25519PublicKey.from_public_bytes(theirs))
    orders = 200

    def buy():
        import http.client
        for n in range(orders):
            connection = http.client.HTTPConnection('127.0.0.1', port, timeout=10)
            body = json.dumps({'item': f'i{n}', 'count': 1}).encode()
            connection.request('POST', '/inputs/buy', body, {'Content-Length': str(len(body)),
                                                             'Content-Type': 'application/json'})
            assert connection.getresponse().status == 200
            connection.close()

    buyer = threading.Thread(target=buy)
    buyer.start()
    runs = {}  # the node's runs: their epochs, answers and the numbers had
    stream, heard, early = None, 0, {}
    opened = {0: 0, 1: 0}
    deadline, done_at = time.time() + 30, None
    try:
        while time.time() < deadline and (done_at is None or time.time() < done_at):
            ready, _, _ = select.select([link], [], [], 0.05)
            if not ready:
                continue
            packet, address = link.recvfrom(2048)
            if not 37 <= len(packet) <= 1452 or packet[0] >> 4 != 0x7:
                continue
            key, run, pn = struct.unpack_from('<IQQ', packet, 1)
            assert key == key_id(theirs), 'a packet under another key id'
            r = runs.setdefault(run, {'epochs': {0: static}, 'now': 0, 'next': None, 'answer_run': 0, 'pn': 0,
                                      'had': set(), 'address': None, 'challenge': int.from_bytes(os.urandom(8), 'little'),
                                      'checked': False, 'bytes_in': 0, 'bytes_out': 0})
            if not r['answer_run']:
                r['answer_run'] = int.from_bytes(os.urandom(8), 'little') or 1
            epoch = packet[0] & 15
            tried = [n for n in {r['now'], r['next'][0] if r['next'] else r['now']} if n & 15 == epoch]
            if epoch == 0 and r['now']:
                tried.append(0)
            body = None
            for n in tried:
                secret = r['epochs'][n] if n in r['epochs'] else r['next'][1]
                aead, base = packet_keys(secret, theirs, my_public, run, 0)
                try:
                    body = aead.decrypt(nonce(base, pn), packet[21:], packet[:21])
                except Exception:
                    continue
                if r['next'] and n == r['next'][0]:  # the epoch accepted is used: it is the run's now
                    r['epochs'][n], r['now'], r['next'] = r['next'][1], n, None
                opened[min(n, 1)] += 1
                break
            assert body is not None, f'a packet of the node did not open (epoch {epoch})'
            if pn in r['had']:
                continue
            newest = not r['had'] or pn > max(r['had'])
            r['had'].add(pn)
            if newest and address != r['address']:
                r['address'], r['checked'], r['bytes_in'], r['bytes_out'] = address, False, 0, 0
            r['bytes_in'] += len(packet)
            at, offer = 0, None
            while at < len(body):
                kind = body[at]
                at += 1
                if kind == 0x01:  # STREAM
                    source_name, input_name, at = names(body, at)
                    at += 4
                    stream = (source_name, input_name)
                    assert input_name == b'order' and source_name == b'independent-sender.ordered', stream
                elif kind == 0x02:  # RECORD: small sends come whole
                    number, size, offset, length = struct.unpack_from('<QIIH', body, at)
                    at += 18
                    assert offset == 0 and length == size, 'a part of a small send'
                    early[number] = body[at:at + length]
                    at += length
                elif kind == 0x06:  # RECORDS
                    first, count = struct.unpack_from('<QB', body, at)
                    at += 9
                    for k in range(count):
                        size, at = varint(body, at)
                        early[first + k] = body[at:at + size]
                        at += size
                elif kind == 0x08:  # RESPONSE
                    if struct.unpack_from('<Q', body, at)[0] == r['challenge']:
                        r['checked'] = True
                    at += 8
                elif kind == 0x09:  # OFFER
                    offer = struct.unpack_from('<I', body, at)[0], body[at + 4:at + 36]
                    at += 36
                elif kind != 0x05:
                    break
            while heard + 1 in early:  # applied and saved, in order
                heard += 1
                del early[heard]
            if offer and not (r['next'] and r['next'][0] == offer[0] and r['next'][3] == offer[1]):
                n, their_half = offer
                if (n == 1 and epoch == 0) or (n == r['now'] + 1 and epoch == r['now'] & 15):
                    half = X25519PrivateKey.generate()
                    fresh = half.exchange(X25519PublicKey.from_public_bytes(their_half))
                    chained = r['epochs'][0] if n == 1 else r['epochs'][r['now']]
                    secret = hkdf(fresh, b'pure link 0 epoch' + struct.pack('<I', n) + chained, 32)
                    r['next'] = (n, secret, raw(half), their_half)
            answer = b''
            if not r['checked']:
                answer += b'\x07' + struct.pack('<Q', r['challenge'])
            if r['next']:
                answer += b'\x0a' + struct.pack('<I', r['next'][0]) + r['next'][2] + r['next'][3][:4]
            had = sorted(r['had'], reverse=True)
            ranges = []
            for number in had:
                if ranges and ranges[-1][0] == number + 1:
                    ranges[-1][0] = number
                elif len(ranges) < 32:
                    ranges.append([number, number])
                else:
                    break
            answer += b'\x03' + struct.pack('<QIB', had[0], 0, len(ranges))
            answer += b''.join(struct.pack('<QQ', first, last) for first, last in ranges)
            if stream:
                answer += (b'\x04' + bytes([len(stream[0])]) + stream[0] + bytes([len(stream[1])]) + stream[1] +
                           struct.pack('<QBI', heard, 0, 1024))
            head = bytes([0x70 | r['now'] & 15]) + struct.pack('<IQQ', key_id(my_public), r['answer_run'], r['pn'])
            aead, base = packet_keys(r['epochs'][r['now']], my_public, theirs, r['answer_run'], run)
            sealed = head + aead.encrypt(nonce(base, r['pn']), answer, head)
            r['pn'] += 1
            if r['checked'] or r['bytes_out'] + len(sealed) <= 3 * r['bytes_in']:
                link.sendto(sealed, r['address'])
                r['bytes_out'] += 0 if r['checked'] else len(sealed)
            if heard == orders and done_at is None:
                done_at = time.time() + 0.5  # the last STORED heard, and answered a while
    finally:
        buyer.join()
        node.terminate()
        _, error = node.communicate(timeout=30)
        link.close()
    assert node.returncode == 0, error[-1000:]
    stats = [json.loads(line) for line in error.decode().splitlines() if line.startswith('{"active"')][-1]
    assert heard == orders and stats['link_stored'] == orders, (heard, stats)
    assert opened[1] > 0 and all(r['checked'] for r in runs.values()), (opened, runs.keys())
    return {'orders': orders, 'opened_in_epoch_0': opened[0], 'opened_in_epoch_1': opened[1],
            'node_packets': stats['link_packets']}


def server_links_moved(aup, pure, work):
    """A receiver's store moved to another address (H3 of
    docs/design/011-pure-transport.md), in namespaces of its own (unshare
    -rmn: its network, and an /etc/hosts of its own): the sender links to
    pure://receiver.test, which names 127.0.0.2. While clients buy, the
    receiver is killed (-9), its store, key and memo (KEY.links) copied
    elsewhere, 127.0.0.2 made unreachable, the name pointed at 127.0.0.3,
    and the copy started there. The sender, its probes unanswered, looks
    the name up again and goes on: every order it saved is applied once,
    and the moved receiver makes no X25519 (its memo came with it). None
    where there is no unshare or ip."""
    import shutil
    unshare = shutil.which('unshare')
    ip = shutil.which('ip') or next((path for path in ('/usr/sbin/ip', '/sbin/ip', '/bin/ip') if Path(path).exists()), None)
    if not unshare or not ip or subprocess.run([unshare, '-rmn', 'true'], capture_output=True).returncode:
        return None
    hosts = work / 'moved-hosts'
    hosts.write_text('127.0.0.1 localhost\n127.0.0.2 receiver.test\n')
    script = f"""
import json, subprocess, sys
from pathlib import Path
sys.path.insert(0, {str(HERE)!r})
import check
subprocess.run([{ip!r}, 'link', 'set', 'lo', 'up'], check=True)
subprocess.run(['mount', '--bind', {str(hosts)!r}, '/etc/hosts'], check=True)
print(json.dumps(check.server_links_moved_inside(Path({str(aup)!r}), Path({str(pure)!r}), Path({str(work)!r}),
                                                 Path({str(hosts)!r}), {ip!r})))
"""
    done = subprocess.run([unshare, '-rmn', sys.executable, '-c', script], capture_output=True, text=True, timeout=600)
    assert done.returncode == 0, done.stderr[-3000:]
    return json.loads(done.stdout.strip().splitlines()[-1])


def server_links_moved_inside(aup, pure, work, hosts, ip):
    import select
    import shutil
    import signal
    import socket
    import threading
    import time
    sender_module, receiver_module = work / 'moved-sender.pmod', work / 'moved-receiver.pmod'
    for module, text in ((sender_module, LINK_SENDER), (receiver_module, LINK_RECEIVER)):
        source = module.with_suffix('.aup')
        source.write_text(text)
        compile_module(aup, source, module)

    def free_port(kind=socket.SOCK_STREAM):
        with socket.socket(socket.AF_INET, kind) as probe:
            probe.bind(('127.0.0.1', 0))
            return probe.getsockname()[1]

    def start(args, port):
        process = subprocess.Popen([str(a) for a in args] + ['--port', str(port)], stdout=subprocess.PIPE,
                                   stderr=subprocess.PIPE)
        ready, _, _ = select.select([process.stdout], [], [], 10)
        line = process.stdout.readline() if ready else b''
        assert line, f'host did not start: {process.stderr.read(2000) if process.poll() is not None else ""}'
        return process, json.loads(line)

    def post(port, body):
        import http.client
        connection = http.client.HTTPConnection('127.0.0.1', port, timeout=10)
        try:
            data = json.dumps(body).encode()
            connection.request('POST', '/inputs/buy', data, {'Content-Length': str(len(data)),
                                                             'Content-Type': 'application/json'})
            response = connection.getresponse()
            response.read()
            return response.status
        finally:
            connection.close()

    def number(port, view):
        return int(http(port, 'GET', f'/views/{view}')[3])

    keys = {}
    for name in ('sender', 'receiver'):
        process, info = start([pure / 'build/pure-serve', '--module', receiver_module, '--store', work / 'moved-key',
                               '--key', work / f'moved-{name}.key'], 0)
        keys[name] = info['public_key']
        process.terminate()
        process.communicate(timeout=30)
    port_a, port_b, link_port = free_port(), free_port(), free_port(socket.SOCK_DGRAM)
    (work / 'moved-senders').write_text(f'moved-sender.ordered {keys["sender"]}\n')

    def receiver(where):
        return [pure / 'build/pure-serve', '--module', receiver_module, '--store', work / f'moved-{where}',
                '--key', work / f'moved-{where}.key', '--link-port', link_port, '--senders', work / 'moved-senders']

    shutil.copy(work / 'moved-receiver.key', work / 'moved-first.key')
    b, _ = start(receiver('first'), port_b)
    a, _ = start([pure / 'build/pure-serve', '--module', sender_module, '--store', work / 'moved-a',
                  '--key', work / 'moved-sender.key', '--send', f'ordered=pure://receiver.test:{link_port}/order',
                  '--seal', f'ordered={keys["receiver"]}'], port_a)
    acknowledged = [0]
    stop = threading.Event()

    def client():
        rng = random.Random(7331)
        while not stop.is_set():
            count = rng.randint(1, 20)
            try:
                if post(port_a, {'item': 'm', 'count': count}) == 200:
                    acknowledged[0] += count
            except OSError:
                time.sleep(0.02)

    thread = threading.Thread(target=client)
    thread.start()
    try:
        time.sleep(0.6)
        b.send_signal(signal.SIGKILL)
        b.wait()
        # The store, key and memo carried to another machine; the old one gone, the name moved.
        shutil.copytree(work / 'moved-first', work / 'moved-second')
        for suffix in ('.key', '.key.links'):
            shutil.copy(work / f'moved-first{suffix}', work / f'moved-second{suffix}')
        subprocess.run([ip, 'route', 'add', 'unreachable', '127.0.0.2/32', 'table', 'local'], check=True)
        hosts.write_text('127.0.0.1 localhost\n127.0.0.3 receiver.test\n')
        b, _ = start(receiver('second'), port_b)
        time.sleep(1.5)
    finally:
        stop.set()
        thread.join()
    try:
        deadline = time.time() + 60
        while time.time() < deadline:
            sent, sent_total = number(port_a, 'sent'), number(port_a, 'sent_total')
            if (number(port_b, 'count'), number(port_b, 'total')) == (sent, sent_total):
                break
            time.sleep(0.1)
        count, total = number(port_b, 'count'), number(port_b, 'total')
        assert (count, total) == (sent, sent_total), (count, total, sent, sent_total)
        assert sent_total >= acknowledged[0] > 0, (sent_total, acknowledged)
    finally:
        stats = {}
        for name, process in (('a', a), ('b', b)):
            process.terminate()
            _, error = process.communicate(timeout=30)
            assert process.returncode == 0, error[-1000:]
            stats[name] = [json.loads(line) for line in error.decode().splitlines() if line.startswith('{"active"')][-1]
    assert stats['b']['listener_woken'] >= 1 and stats['b']['listener_secrets'] == 0, stats['b']
    return {'orders': sent, 'acknowledged_total': acknowledged[0], 'packets': stats['a']['link_packets'],
            'resent': stats['a']['link_resent'], 'received_after_move': stats['b']['listener_received'],
            'secrets_after_move': stats['b']['listener_secrets']}


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
  done = false
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
    assert json.dumps(views[:6]) == json.dumps([
        {'count': 0, 'current': '(empty)', 'finished': None},
        {'count': 1, 'current': 'one', 'finished': False}, {'count': 2, 'current': 'two', 'finished': False},
        {'count': 3, 'current': 'three', 'finished': False}, {'count': 3, 'current': 'three', 'finished': True},
        {'count': 3, 'current': 'three', 'finished': True}]), views[:6]  # a bool is true or false, not 1 or 0
    assert json.dumps(views[-1]) == json.dumps({'count': 4, 'current': 'four', 'finished': False}), views[-1]
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
  let taken = 0
  for l in order.lines {
    let s = stock[l.item]
    s.left = s.left - l.qty
    require s.left >= 0
    log.append(Entry { item: l.item, qty: l.qty, note: l.note })
    taken = taken + 1
  }
  orders = orders + taken
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


TREES = '''
record Node {
  name: text
  tag: bytes = "-"
  n = 0
  kids: list of Node
}
state nodes = 0
state total = 0
state deepest = 0
state name_bytes = 0
input tree = message {
  label: text
  roots: list of Node
}

fn walk(x, depth) {
  nodes = nodes + 1
  total = total + x.n
  name_bytes = name_bytes + x.name.bytes + x.tag.bytes
  if depth > deepest { deepest = depth }
  for k in x.kids { walk(k, depth + 1) }
}

on tree {
  require tree.label != "refuse"
  for r in tree.roots { walk(r, 1) }
}

view count = nodes
view sum = total
view depth = deepest
view named = name_bytes
'''


def server_trees(aup, pure, work):
    """A message's rows whose records hold lists (027's 4a): a JSON forest
    read straight into the event's lists, nested to any depth up to 64, and
    walked by a function that calls itself, a field of bytes among them
    (027's 4b). Random forests (names with
    escapes and characters past ASCII, fields left out taking their
    defaults) against a model walking them in the same order; a require
    refusing one leaves everything as it was; malformed trees (a list field
    not an array, a field the record lacks, past 64 deep) are refused before
    the app sees them; a restart keeps the states and no rows."""
    import http.client
    import random
    source, module = work / 'trees.aup', work / 'trees.pmod'
    source.write_text(TREES)
    compile_module(aup, source, module)
    store = work / 'trees-store'

    def call(port, method, path, body=None):
        connection = http.client.HTTPConnection('127.0.0.1', port, timeout=60)
        try:
            connection.request(method, path, body, {'Content-Type': 'application/json'} if body is not None else {})
            answer = connection.getresponse()
            return answer.status, answer.getheader('X-Pure-Error'), answer.read()
        finally:
            connection.close()

    def views(port):
        return tuple(float(call(port, 'GET', f'/views/{v}')[2]) for v in ('count', 'sum', 'depth', 'named'))

    rng = random.Random(44)
    names = ['', 'a', 'node', '빨리', 'a "quoted" \\ name', 'tab\there', '😀', 'line\nbreak', 'é']

    def forest(depth):
        roots = []
        for _ in range(rng.randrange(4 if depth < 6 else 1)):
            node = {'name': rng.choice(names)}
            if rng.random() < 0.5:
                node['tag'] = rng.choice(names)  # bytes, from a JSON string
            if rng.random() < 0.8:
                node['n'] = rng.randrange(-1000, 1000) / 8
            kids = forest(depth + 1)
            if kids or rng.random() < 0.5:
                node['kids'] = kids
            roots.append(node)
        return roots

    model = [0, 0.0, 0, 0]

    def walk(node, depth, into):
        into[0] += 1
        into[1] += node.get('n', 0)
        into[2] = max(into[2], depth)
        into[3] += len(node['name'].encode()) + len(node.get('tag', '-').encode())
        for k in node.get('kids', []):
            walk(k, depth + 1, into)

    host = HttpHost([pure / 'build/pure-serve', '--module', module, '--store', store])
    sent = refused = 0
    try:
        for i in range(300):
            roots = forest(1)
            label = 'refuse' if rng.random() < 0.1 else 'tree'
            after = list(model)
            for r in roots:
                walk(r, 1, after)
            status, error, _ = call(host.port, 'POST', '/inputs/tree', json.dumps({'label': label, 'roots': roots}).encode())
            if label == 'refuse':
                assert (status, error) == (422, 'limit'), (status, error)  # require fails as limit
                refused += 1
            else:
                assert (status, error) == (200, 'none'), (status, error)
                model = after
                sent += 1
            if i % 25 == 0:
                assert views(host.port) == tuple(model), (views(host.port), model)
        deep = {'name': 'z'}
        for _ in range(63):
            deep = {'name': 'z', 'kids': [deep]}
        for body, want in [({'label': 'x', 'roots': [{'name': 'a', 'kids': 3}]}, (400, 'json_type')),
                           ({'label': 'x', 'roots': [{'name': 'a', 'kid': []}]}, (400, 'json_field')),
                           ({'label': 'x', 'roots': [{'name': 'a', 'kids': [deep]}]}, (400, 'limit'))]:
            assert call(host.port, 'POST', '/inputs/tree', json.dumps(body).encode())[:2] == want, body
        assert call(host.port, 'POST', '/inputs/tree', json.dumps({'label': 'x', 'roots': [deep]}).encode())[:2] == (200, 'none')
        walk(deep, 1, model)
        assert views(host.port) == tuple(model) and model[2] == 64
    finally:
        host.stop()
    host = HttpHost([pure / 'build/pure-serve', '--module', module, '--store', store])
    try:
        assert views(host.port) == tuple(model)
    finally:
        host.stop()
    assert sent > 200 and refused > 10
    return {'forests': sent, 'refused': refused, 'nodes': model[0], 'deepest': model[2]}

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



RECEIPT_SOURCE = """record Receipt {
  order: text
  note: text
  status = 0
}
state receipts: list of Receipt
state sent = 0
state answered = 0
input buy = message {
  order: text
}
output charge = message {
  order: text
}
input charged = message {
  order: text
  note: text
  http_status: u32
}
input clean = message {
  below: u32
}
on buy {
  sent = sent + 1
  send charge { order: buy.order }
}
on charged {
  answered = answered + 1
  receipts.append(Receipt { order: charged.order, note: charged.note, status: charged.http_status })
}
on clean {
  for r in receipts where r.status < clean.below {
    receipts.remove(r)
  }
}
view sent_view = sent
view answered_view = answered
view receipts_view = receipts.count
"""


def server_answers_memory(aup, pure, work):
    """Answers (--reply) under a memory budget: an answer keeps a receipt
    larger than its send, so once receipts fill the budget the next answer
    waits (it is not lost, and not applied twice) and buys are refused, 503
    memory. A clean that frees the receipts lets the answers in, each once."""
    from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
    import threading
    import time
    module = work / 'receipt.pmod'
    module.with_suffix('.aup').write_text(RECEIPT_SOURCE)
    compile_module(aup, module.with_suffix('.aup'), module)
    seen, lock = {}, threading.Lock()

    class Api(BaseHTTPRequestHandler):
        protocol_version = 'HTTP/1.1'

        def do_POST(self):
            body = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
            with lock:
                seen[body['order']] = seen.get(body['order'], 0) + 1
            data = json.dumps({'note': 'n' * 200}).encode()
            self.send_response(200)
            self.send_header('Content-Type', 'application/json')
            self.send_header('Content-Length', str(len(data)))
            self.end_headers()
            self.wfile.write(data)

        def log_message(self, *args):
            pass

    server = ThreadingHTTPServer(('127.0.0.1', 0), Api)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    host = HttpHost([pure / 'build/pure-serve', '--module', module, '--store', work / 'receipt-store', '--memory', 3000,
                     '--send', f'charge=http://127.0.0.1:{server.server_address[1]}/charge', '--reply', 'charge=charged'])

    def number(view):
        return int(http(host.port, 'GET', f'/views/{view}')[3])

    try:
        bought, refusals = 0, 0
        while refusals < 3 and bought < 200:
            status = post(host.port, '/inputs/buy', {'order': f'o{bought}'})
            if status[0] == 200:
                bought += 1
            else:
                assert status == (503, 'memory'), status
                refusals += 1
            time.sleep(0.02)
        time.sleep(0.5)
        assert 3 <= bought < 200 and number('sent_view') == bought, bought
        waiting = bought - number('answered_view')
        assert waiting >= 1, (bought, number('answered_view'))
        assert post(host.port, '/inputs/clean', {'below': 1000})[0] == 200
        deadline = time.time() + 10
        while number('answered_view') < bought and time.time() < deadline:
            time.sleep(0.02)
        assert number('answered_view') == bought and number('receipts_view') == waiting, (number('answered_view'), waiting)
    finally:
        stats = host.stop()
        server.shutdown()
    with lock:
        assert len(seen) == bought and all(count == 1 for count in seen.values()), seen
    assert stats[-1]['memory_refused'] >= refusals + 1, stats[-1]
    return {'bought': bought, 'answers_waited': waiting, 'refused': stats[-1]['memory_refused']}

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


PLAIN_EDIT_SOURCE = """state n = 0
input edit = message { value: number }
on edit { n = edit.value }
view result = n
"""


def server_plain_edit(aup, pure, work):
    """An input named edit that is a plain message (the module keeps no
    document collection) is served as any other input: the app starts, and a
    POST to it changes its state (review 004, an earlier P2)."""
    from http.client import HTTPConnection
    module, source = work / 'plain-edit.pmod', work / 'plain-edit.aup'
    source.write_text(PLAIN_EDIT_SOURCE)
    compile_module(aup, source, module)
    host = HttpHost([pure / 'build/pure-serve', '--module', module, '--store', work / 'plain-edit'])
    try:
        connection = HTTPConnection('127.0.0.1', host.port, timeout=10)
        body = json.dumps({'value': 5}).encode()
        connection.request('POST', '/inputs/edit', body, {'Content-Length': str(len(body)), 'Content-Type': 'application/json'})
        status = connection.getresponse().status
        connection.close()
        assert status == 200, status
        result = http(host.port, 'GET', '/views/result')
        assert result[0] == 200 and float(result[3]) == 5, result
    finally:
        host.stop()
    return 5


HEARTBEAT_SOURCE = """input beat = every 20 ms
output pulse = message { value: number }
on beat { send pulse { value: 1 } }
view label = "heartbeat"
"""


def server_timers(aup, pure, work):
    """A served app's timers tick on the server's clock and its events see
    the time they were applied, on one thread and with an owner; the ticks
    are saved, so a restarted server goes on from them. An app with no state
    but an output ticks too: its sends are its state (review 004, F4). A
    window in which the wall clock jumped (a VM's time sync sets it back
    seconds at a time: research 029) says nothing of the times seen, and is
    taken again."""
    import time
    module, source = work / 'beat.pmod', work / 'beat.aup'
    source.write_text(BEAT_SOURCE)
    compile_module(aup, source, module)
    seen, jumped = {}, 0
    for threads in (1, 3):
        store = work / f'beat-{threads}'
        beats = 0
        for run_number in range(2):
            host = HttpHost([pure / 'build/pure-serve', '--module', module, '--store', store, '--threads', threads])
            try:
                for _ in range(3):
                    posted, steady = time.time() * 1000, time.monotonic() * 1000
                    assert http(host.port, 'POST', '/inputs/stamp', b'x')[0] == 200
                    time.sleep(0.6)
                    before = time.time() * 1000
                    read = lambda view: float(http(host.port, 'GET', f'/views/{view}')[3])
                    count, last, stamped = read('beats_view'), read('last_view'), read('stamped_view')
                    after = time.time() * 1000 + 1  # the server's clock reads whole milliseconds
                    if abs((after - 1 - posted) - (time.monotonic() * 1000 - steady)) < 50:
                        break
                    jumped += 1
                assert count >= beats + 5, (threads, run_number, count, beats)
                assert before - 1000 < last <= after and posted - 50 <= stamped <= before, (last, stamped, posted, before)
                beats = count
            finally:
                stats = host.stop()
            assert stats[0]['ticks'] >= 5, stats
        seen[threads] = beats
    module, source = work / 'heartbeat.pmod', work / 'heartbeat.aup'
    source.write_text(HEARTBEAT_SOURCE)
    compile_module(aup, source, module)
    host = HttpHost([pure / 'build/pure-serve', '--module', module, '--store', work / 'heartbeat'])
    try:
        time.sleep(0.45)
    finally:
        stats = host.stop()
    assert stats[0]['ticks'] >= 10, stats
    seen['stateless'] = stats[0]['ticks']
    seen['windows_the_clock_jumped'] = jumped
    return seen


def cpu_seconds(pid):
    """The CPU time a process has used (user and system)."""
    import os
    with open(f'/proc/{pid}/stat') as stat:
        fields = stat.read().rsplit(')', 1)[1].split()
    return (int(fields[11]) + int(fields[12])) / os.sysconf('SC_CLK_TCK')


def server_clocks(aup, pure, work):
    """The wall clock moved an hour while a node runs (build/clock-back.so
    moves what the node reads of it, from its start): its timers, the pauses
    of a wire whose receiver is down, the owner's waits and a store's group
    commit all count on the monotonic clock. Were a wait's deadline read
    from the wall clock, set back it would end at once, again and again (a
    core spun, the wire tried thousands of times); set forward it would last
    an hour (the timers stopped, the wire tried once, and a lone commit
    after a group waited that long for the others to join it)."""
    import os
    import socket
    import struct
    import threading
    import time
    module, source = work / 'clocks.pmod', work / 'clocks.aup'
    source.write_text(HEARTBEAT_SOURCE)
    compile_module(aup, source, module)
    with socket.socket() as probe:
        probe.bind(('127.0.0.1', 0))
        nobody = probe.getsockname()[1]
    seen = {}
    for shift in (-3600, 3600):
        env = {**os.environ, 'LD_PRELOAD': str(pure / 'build/clock-back.so'), 'CLOCK_BACK_MS': '0',
               'CLOCK_SHIFT_S': str(shift)}
        host = HttpHost([pure / 'build/pure-serve', '--module', module, '--store', work / f'clocks{shift}',
                         '--send', f'pulse=http://127.0.0.1:{nobody}/inputs/pulse'], env=env)
        try:
            time.sleep(0.2)
            began, used = time.time(), cpu_seconds(host.process.pid)
            time.sleep(1.0)
            share = (cpu_seconds(host.process.pid) - used) / (time.time() - began)
        finally:
            stats = host.stop()[-1]
        assert stats['ticks'] >= 30 and share < 0.25 and 3 <= stats['delivery_attempts'] <= 12, \
            (shift, stats['ticks'], share, stats['delivery_attempts'])
        seen[f'shift_{shift}'] = {'ticks': stats['ticks'], 'cpu_share': round(share, 3),
                                  'wire_attempts': stats['delivery_attempts']}
    editor = work / 'clocks-editor.pmod'
    compile_module(aup, EDITOR, editor)
    env = {**os.environ, 'LD_PRELOAD': str(pure / 'build/clock-back.so'), 'CLOCK_BACK_MS': '0', 'CLOCK_SHIFT_S': '3600'}
    host = HttpHost([pure / 'build/pure-serve', '--module', editor, '--store', work / 'clocks-docs', '--threads', 3],
                    env=env)
    new = struct.pack('<II', 0, 0)  # an edit that makes the document: at 0, none removed, then its bytes
    statuses, alone = [], 0.0
    try:
        for round_number in range(20):  # three at once, grouped; then one alone
            together = threading.Barrier(3)

            def put(k):
                together.wait()
                statuses.append(host.request('PUT', f'd{round_number}-{k}', new + b'x' * 100000, 0)[0])

            threads = [threading.Thread(target=put, args=(k,)) for k in range(3)]
            for thread in threads:
                thread.start()
            for thread in threads:
                thread.join()
            began = time.time()
            statuses.append(host.request('PUT', f'alone-{round_number}', new + b'y', 0)[0])
            alone = max(alone, time.time() - began)
    finally:
        stats = host.stop()[-1]
    assert statuses == [200] * 80 and alone < 1 and stats['grouped_commits'] > stats['commit_groups'], \
        (statuses, alone, stats)
    seen['alone_after_groups_s'] = round(alone, 4)
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


MEANING_KEPT = """record Kept {
  id = 0
  n = 0
  u = 0
  t: text
  x = 0
  label: text
  ok = false
  big: i64
}
state kept: list of Kept by id
"""
MEANING_ROWS = """view rows = each k in kept {
  id = k.id
  n = k.n
  u = k.u
  t = k.t
  x = k.x
  label = k.label
  ok = k.ok
  big = k.big
}
"""
# A: what comes in (a JSON body, its rows, or a query and a plain body) is kept, and sent on; a put is answered
# with its text.
MEANING_ORIGIN = MEANING_KEPT + """record Part {
  id = 0
  n = 0
  u = 0
  t: text
  ok = false
  big: i64
}
input put = message {
  id: u32
  n: number
  u: u32
  t: text
  ok: bool
  big: i64
  at: {
    x: number
    label: text
  }
}
input put_plain = message {
  id: u32
  n: number
  u: u32
  t: text
  ok: bool
  big: i64
}
input put_rows = message {
  parts: list of Part
}
input touch = message {
  id: u32
  t: text
}
output passed = message like put
on put {
  kept.add(Kept { id: put.id, n: put.n, u: put.u, t: put.t, x: put.at.x, label: put.at.label, ok: put.ok, big: put.big })
  send passed { ..put }
  reply put.t
}
on put_rows {
  for p in put_rows.parts {
    kept.add(Kept { id: p.id, n: p.n, u: p.u, t: p.t, x: 0, label: "rows", ok: p.ok, big: p.big })
    send passed { id: p.id, n: p.n, u: p.u, t: p.t, ok: p.ok, big: p.big, at: { x: 0, label: "rows" } }
  }
}
on put_plain {
  kept.add(Kept { id: put_plain.id, n: put_plain.n, u: put_plain.u, t: put_plain.t, x: 0, label: "", ok: put_plain.ok,
                  big: put_plain.big })
  send passed { id: put_plain.id, n: put_plain.n, u: put_plain.u, t: put_plain.t, ok: put_plain.ok, big: put_plain.big,
                at: { x: 0, label: "" } }
}
on touch {
  kept[touch.id].t = touch.t
}
view any_ok = count(k in kept where k.ok) > 0
view with_ok(want: bool) = count(k in kept where k.ok == want)
view biggest = max(k.big for k in kept)
view smallest = min(k.big for k in kept)
view with_big(want: i64) = count(k in kept where k.big == want)
""" + MEANING_ROWS
# B: A's sends over the link, taken by name (fields in another order, u a number, id an i64), kept and sent on over
# HTTP.
MEANING_MIRROR = MEANING_KEPT + """input passed = message {
  big: i64
  ok: bool
  at: {
    label: text
    x: number
  }
  t: text
  u: number
  n: number
  id: i64
}
output onward = message like passed
on passed {
  kept.add(Kept { id: number(passed.id), n: passed.n, u: passed.u, t: passed.t, x: passed.at.x, label: passed.at.label,
                  ok: passed.ok, big: passed.big })
  send onward { ..passed }
}
""" + MEANING_ROWS
# C: B's sends over HTTP.
MEANING_END = MEANING_KEPT + """input onward = message {
  id: u32
  n: number
  u: number
  t: text
  at: {
    x: number
    label: text
  }
  ok: bool
  big: i64
}
on onward {
  kept.add(Kept { id: onward.id, n: onward.n, u: onward.u, t: onward.t, x: onward.at.x, label: onward.at.label,
                  ok: onward.ok, big: onward.big })
}
""" + MEANING_ROWS
# The value set: every number a JSON reader must round correctly, u32s to its ends, texts JSON escapes and UTF-8.
MEANING_NUMBERS = ['0', '-0', '1', '-1', '0.1', '0.3333333333333333', '9007199254740991', '9007199254740992',
                   '9007199254740993', '-9007199254740993', '1e21', '1e20', '1e-7', '123456789012345678901234567890',
                   '1.7976931348623157e308', '1.7976931348623158e308', '2.2250738585072014e-308',
                   '2.2250738585072011e-308', '5e-324', '2.4703282292062328e-324', '1E+2', '-2.5e-10']
MEANING_U32 = [0, 1, 2147483648, 4294967295]
MEANING_I64 = ['0', '-0', '1', '-1', '9007199254740992', '9007199254740993', '-9007199254740993', '9223372036854775807',
               '-9223372036854775808', '9223372036854775806', '-9223372036854775807', '4294967296', '1234567890123456789']
MEANING_TEXTS = ['', 'a', '한글 텍스트', 'emoji 🎉 and 𝄞', 'quote " backslash \\ slash /', 'line\nfeed\ttab\rreturn',
                 'control \x01\x1f\x7f', 'nul \x00 inside', 'separator \u2028 and \u2029', '\ufeffbom',
                 '\uffff not a character', 'x' * 4096]


def server_meaning(aup, pure, work):
    """One value, every path (docs/research/108): values at the edges of what
    JSON, a query and a plain body can say come in to A; A keeps them and sends
    them over a link to B, which takes them by name (fields in another order,
    u widened to a number, id to an i64) and sends them over HTTP to C. Each
    value must come out the same at every exit: A's view, a query on A, B's
    and C's views, A after a restart and after a checkpoint, B after its
    module was replaced. A number keeps its bits (zero has no sign: -0 shows
    as 0 and counts as 0), a text its bytes, a bool is true or false (never 1
    or 0), an i64 every digit (past 2^53, to its ends). What must be refused
    is refused on every way in, and leaves nothing behind (no row, no send)."""
    import socket
    import struct
    import time
    from http.client import HTTPConnection
    from urllib.parse import quote
    modules = {}
    for name, text in (('origin', MEANING_ORIGIN), ('mirror', MEANING_MIRROR), ('end', MEANING_END),
                       ('mirror-2', MEANING_MIRROR.replace('  id = 0\n  n = 0\n  u = 0\n  t: text\n  x = 0\n  label: text\n  ok = false\n  big: i64\n',
                                                           '  big: i64\n  ok = false\n  label: text\n  x = 0\n  t: text\n  u = 0\n  n = 0\n  id = 0\n'))):
        source = work / f'meaning-{name}.aup'
        source.write_text(text)
        compile_module(aup, source, source.with_suffix('.pmod'))
        modules[name] = source.with_suffix('.pmod')
    serve = pure / 'build/pure-serve'
    keys = {}
    for name in ('a', 'b', 'c'):
        host = HttpHost([serve, '--module', modules['end'], '--store', work / 'meaning-keys', '--key', work / f'meaning-{name}.key'])
        keys[name] = host.info['public_key']
        host.stop()
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as probe:
        probe.bind(('127.0.0.1', 0))
        link_port = probe.getsockname()[1]

    def start(name, module):
        args = [serve, '--module', modules[module], '--store', work / f'meaning-{name}', '--key', work / f'meaning-{name}.key',
                '--name', name]
        if name == 'a':
            args += ['--send', f'passed=pure://127.0.0.1:{link_port}/passed', '--seal', f'passed={keys["b"]}']
        if name == 'b':
            args += ['--link-port', link_port, '--sender', f'a.passed={keys["a"]}',
                     '--send', f'onward=http://127.0.0.1:{hosts["c"].port}/inputs/onward', '--seal', f'onward={keys["c"]}']
        if name == 'c':
            args += ['--sender', f'b.onward={keys["b"]}']
        hosts[name] = HttpHost(args)

    def send(port, path, body, kind, answer=None):
        connection = HTTPConnection('127.0.0.1', port, timeout=30)
        try:
            connection.request('POST', path, body, {'Content-Length': str(len(body)), 'Content-Type': kind})
            response = connection.getresponse()
            got = response.read()
            assert answer is None or response.status != 200 or got == answer, (path, got[:80], answer[:80])
            return response.status
        finally:
            connection.close()

    def refused_as(port, path, body, kind):  # the status and X-Pure-Error of a request that must fail
        connection = HTTPConnection('127.0.0.1', port, timeout=30)
        try:
            connection.request('POST', path, body, {'Content-Length': str(len(body)), 'Content-Type': kind})
            response = connection.getresponse()
            response.read()
            return response.status, response.getheader('X-Pure-Error')
        finally:
            connection.close()

    def rows(name, path='/views/rows'):
        status, _, _, body = http(hosts[name].port, 'GET', path)
        assert status == 200, (name, path, status, body[:200])
        return {row['id']: row for row in json.loads(body)['items']}

    def same(shown, want):  # a number's bits, zero having no sign
        return float(shown) == want == 0 or struct.pack('<d', float(shown)) == struct.pack('<d', want)

    def differing(found, expected):
        wrong = [(key, found.get(key)) for key, (n, u, t, x, label, ok, big) in expected.items()
                 if key not in found or not (same(found[key]['n'], n) and float(found[key]['u']) == u and
                                             found[key]['t'] == t and same(found[key]['x'], x) and found[key]['label'] == label
                                             and found[key]['ok'] is ok and type(found[key]['big']) is int
                                             and found[key]['big'] == big)]
        return wrong + [(key, 'not sent') for key in set(found) - set(expected)]

    hosts, expected, refused = {}, {}, 0
    try:
        start('c', 'end')
        start('b', 'mirror')
        start('a', 'origin')
        a = hosts['a'].port
        count = len(MEANING_NUMBERS)
        # In as JSON, each string once escaped to ASCII and once as UTF-8, a group's fields inside.
        for i, n in enumerate(MEANING_NUMBERS):
            u, t = MEANING_U32[i % len(MEANING_U32)], MEANING_TEXTS[i % len(MEANING_TEXTS)]
            x, label = MEANING_NUMBERS[(i + 5) % count], MEANING_TEXTS[(i + 3) % len(MEANING_TEXTS)]
            ascii_only, ok, big = i % 2 == 0, i % 3 != 0, MEANING_I64[i % len(MEANING_I64)]
            body = '{"id": %d, "n": %s, "u": %d, "t": %s, "ok": %s, "big": %s, "at": {"x": %s, "label": %s}}' % (
                i + 1, n, u, json.dumps(t, ensure_ascii=ascii_only), json.dumps(ok), big, x,
                json.dumps(label, ensure_ascii=ascii_only))
            assert send(a, '/inputs/put', body.encode(), 'application/json', t.encode()) == 200, body[:200]
            expected[i + 1] = (float(n), u, t, float(x), label, ok, int(big))
        # In as rows of one message: a JSON array, placed as its list's stored form.
        parts = [(501 + i, n, MEANING_U32[(i + 2) % len(MEANING_U32)], MEANING_TEXTS[(i + 4) % len(MEANING_TEXTS)], i % 2 == 0,
                  MEANING_I64[(i + 4) % len(MEANING_I64)]) for i, n in enumerate(MEANING_NUMBERS)]
        body = '{"parts": [%s]}' % ', '.join('{"id": %d, "n": %s, "u": %d, "t": %s, "ok": %s, "big": %s}' % (
            key, n, u, json.dumps(t), json.dumps(ok), big) for key, n, u, t, ok, big in parts)
        assert send(a, '/inputs/put_rows', body.encode(), 'application/json') == 200, body[:200]
        expected.update({key: (float(n), u, t, 0.0, 'rows', ok, int(big)) for key, n, u, t, ok, big in parts})
        # In as a query (%XX where a client would write it so) and a plain body.
        for i, n in enumerate(MEANING_NUMBERS):
            u, t, ok = MEANING_U32[(i + 1) % len(MEANING_U32)], MEANING_TEXTS[(i + 2) % len(MEANING_TEXTS)], i % 2 == 1
            big = MEANING_I64[(i + 8) % len(MEANING_I64)]
            assert send(a, f'/inputs/put_plain?id={101 + i}&n={quote(n, safe="")}&u={u}&ok={json.dumps(ok)}&big={big}',
                        t.encode(), 'text/plain; charset=utf-8') == 200, n
            expected[101 + i] = (float(n), u, t, 0.0, '', ok, int(big))
        # What must be refused: numbers past the finite, u32s out of range or not whole, texts that are no UTF-8,
        # bools that are not true or false, i64s that are not an integer's digits in range.
        good = '{"id": %d, "n": %s, "u": %s, "t": %s, "ok": true, "big": 1, "at": {"x": 1, "label": ""}}'
        bad_json = [good % (201, '1e999', '1', '"t"'), good % (202, '-1e999', '1', '"t"'), good % (203, 'NaN', '1', '"t"'),
                    good % (204, '00', '1', '"t"'), good % (205, '+1', '1', '"t"'), good % (206, '.5', '1', '"t"'),
                    good % (207, '1', '4294967296', '"t"'), good % (208, '1', '-1', '"t"'), good % (209, '1', '1.5', '"t"'),
                    good % (210, '1', '1e3', '"t"'), good % (211, '1', '-0', '"t"'), good % (212, '1', '1', '"\\ud800"'),
                    good % (213, '1', '1', '"a\\ud83d"'), good % (214, '1', '1', '"\\udc00\\ud800"')]
        for body in bad_json:
            assert send(a, '/inputs/put', body.encode(), 'application/json') == 400, body
        not_bools = [('1', 'json_type'), ('0', 'json_type'), ('"true"', 'json_type'), ('null', 'json_type'), ('[]', 'json_type'),
                     ('True', 'json')]  # no JSON at all
        for i, (bad, why) in enumerate(not_bools):
            body = '{"id": %d, "n": 1, "u": 1, "t": "t", "ok": %s, "big": 1, "at": {"x": 1, "label": ""}}' % (231 + i, bad)
            assert refused_as(a, '/inputs/put', body.encode(), 'application/json') == (400, why), body
        not_integers = [('1.0', 'json_type'), ('1e3', 'json_type'), ('1.5', 'json_type'), ('9223372036854775808', 'json_type'),
                        ('-9223372036854775809', 'json_type'), ('99999999999999999999', 'json_type'), ('"1"', 'json_type'),
                        ('true', 'json_type'), ('null', 'json_type'), ('+1', 'json'), ('01', 'json'), ('-', 'json'),
                        ('0x10', 'json')]
        for i, (bad, why) in enumerate(not_integers):
            body = '{"id": %d, "n": 1, "u": 1, "t": "t", "ok": true, "big": %s, "at": {"x": 1, "label": ""}}' % (241 + i, bad)
            assert refused_as(a, '/inputs/put', body.encode(), 'application/json') == (400, why), body
        no_utf8 = [b'\xff', b'\xc3', b'\xed\xa0\x80', b'\xc0\xaf', b'\xf4\x90\x80\x80']  # a surrogate, too long, past U+10FFFF
        for i, bad in enumerate(no_utf8):
            body = b'{"id": %d, "n": 1, "u": 1, "t": "' % (215 + i) + bad + b'", "ok": true, "big": 1, "at": {"x": 1, "label": ""}}'
            assert send(a, '/inputs/put', body, 'application/json') == 400, body
        bad_query = [('1e999', '1'), ('inf', '1'), ('nan', '1'), ('0x10', '1'), ('', '1'), ('1', '4294967296'), ('1', '-1'),
                     ('1', '1.5'), ('1', '1e3'), ('1', '%2D0'), ('1', '%201'), ('1', '%zz')]
        for i, (n, u) in enumerate(bad_query):
            assert send(a, f'/inputs/put_plain?id={301 + i}&n={n}&u={u}&ok=true&big=1', b't', 'text/plain') == 400, (n, u)
        for i, bad in enumerate(['1', '0', 'True', 'TRUE', '%74rue%20', '']):
            assert refused_as(a, f'/inputs/put_plain?id={331 + i}&n=1&u=1&ok={bad}&big=1', b't', 'text/plain') == (400, 'query'), bad
        bad_i64_query = ['1.0', '1e3', '+1', '%2B1', '01', '', '-', '0x10', '9223372036854775808', '-9223372036854775809',
                         '%201', '1%20', 'true', '%31%2E0']
        for i, bad in enumerate(bad_i64_query):
            assert refused_as(a, f'/inputs/put_plain?id={341 + i}&n=1&u=1&ok=true&big={bad}', b't', 'text/plain') == \
                (400, 'query'), bad
        for i, bad in enumerate(no_utf8 + [b'ok \xc3']):
            assert send(a, f'/inputs/put_plain?id={401 + i}&n=1&u=1&ok=false&big=1', bad, 'text/plain') == 422, bad
        # Rows go whole or not at all: one number past the finite refuses them all; a u that a row holds (a
        # number) but a u32 cannot fails the event where it is sent, and none is kept.
        assert send(a, '/inputs/put_rows', b'{"parts": [{"id": 601, "n": 1, "u": 1, "t": "a"}, '
                                           b'{"id": 602, "n": 1e999, "u": 1, "t": "b"}]}', 'application/json') == 400
        assert send(a, '/inputs/put_rows', b'{"parts": [{"id": 603, "n": 1, "u": 1, "t": "a"}, '
                                           b'{"id": 604, "n": 1, "u": 1.5, "t": "b"}]}', 'application/json') != 200
        assert refused_as(a, '/inputs/put_rows', b'{"parts": [{"id": 605, "ok": true}, {"id": 606, "ok": 1}]}',
                          'application/json') == (400, 'json_type')
        for i, bad in enumerate(['1.5', '9223372036854775808', '"1"', '1e3', '-9223372036854775809']):
            body = b'{"parts": [{"id": %d, "big": 1}, {"id": %d, "big": %s}]}' % (607 + 2 * i, 608 + 2 * i, bad.encode())
            assert refused_as(a, '/inputs/put_rows', body, 'application/json') == (400, 'json_type'), bad
        refused = len(bad_json) + len(not_bools) + len(not_integers) + len(no_utf8) + len(bad_query) + 6 + \
            len(bad_i64_query) + len(no_utf8) + 1 + 2 + 1 + 5
        # A row to change often on A alone, until a checkpoint writes its list anew; it ends as it began.
        assert send(a, '/inputs/put', json.dumps({'id': 900, 'n': 9, 'u': 9, 't': 'filler', 'ok': True, 'big': 9,
                                                  'at': {'x': 9, 'label': 'filler'}}).encode(), 'application/json') == 200
        expected[900] = (9.0, 9, 'filler', 9.0, 'filler', True, 9)
        exits = {'a': rows('a')}
        deadline = time.time() + 30
        while time.time() < deadline and (len(rows('b')) < len(expected) or len(rows('c')) < len(expected)):
            time.sleep(0.05)
        time.sleep(0.3)  # anything more (a refused event sent after all) would be here by now
        exits['b'], exits['c'] = rows('b'), rows('c')
        query = work / 'meaning-query.aup'
        query.write_text(MEANING_ROWS.replace('view rows =', 'view rows_asked ='))
        run(aup, 'query', work / 'meaning-origin.aup', query, '-o', work / 'meaning-query.pmod')
        status, _, _, body = http(a, 'PUT', '/queries/rows_asked', (work / 'meaning-query.pmod').read_bytes())
        assert status == 200, body[:200]
        exits['a queried'] = rows('a', '/queries/rows_asked')
        hosts.pop('a').stop()
        start('a', 'origin')
        exits['a restarted'] = rows('a')
        for step in range(400):  # 1.6 MiB of changes to a list of a few KiB: its image is written anew
            assert send(hosts['a'].port, '/inputs/touch', json.dumps({'id': 900, 't': f'{step:04}' * 1024}).encode(),
                        'application/json') == 200
        assert send(hosts['a'].port, '/inputs/touch', json.dumps({'id': 900, 't': 'filler'}).encode(),
                    'application/json') == 200
        checkpoints = hosts.pop('a').stop()[-1]['checkpoints']
        assert checkpoints >= 1, checkpoints
        start('a', 'origin')
        exits['a after a checkpoint'] = rows('a')
        hosts.pop('b').stop()
        start('b', 'mirror-2')
        exits['b replaced'] = rows('b')
        for where, found in exits.items():
            wrong = differing(found, expected)
            assert not wrong, (where, wrong[:3])
        # A bool view is true or false as text; a bool argument is true or false in a query, nothing else.
        a = hosts['a'].port
        assert http(a, 'GET', '/views/any_ok')[::3] == (200, b'true')
        oks = sum(1 for value in expected.values() if value[5])
        assert http(a, 'GET', '/views/with_ok?want=true')[::3] == (200, str(oks).encode())
        assert http(a, 'GET', '/views/with_ok?want=false')[::3] == (200, str(len(expected) - oks).encode())
        for bad in ('1', 'True', ''):
            assert http(a, 'GET', f'/views/with_ok?want={bad}')[:2] == (400, 'invalid'), bad
        # An i64 view is every digit; an i64 argument is an integer's digits in range, nothing else.
        assert http(a, 'GET', '/views/biggest')[::3] == (200, b'9223372036854775807')
        assert http(a, 'GET', '/views/smallest')[::3] == (200, b'-9223372036854775808')
        for want in ('9007199254740993', '-9007199254740993', '-0', '9223372036854775807', '-9223372036854775808'):
            count = sum(1 for value in expected.values() if value[6] == int(want))
            assert count and http(a, 'GET', f'/views/with_big?want={want}')[::3] == (200, str(count).encode()), want
        for bad in ('9007199254740993.0', '1e3', '9223372036854775808', '-9223372036854775809', '+1', '01', '', 'true'):
            assert http(a, 'GET', f'/views/with_big?want={bad}')[:2] == (400, 'invalid'), bad
    finally:
        for host in hosts.values():
            host.stop()
    return {'values': len(expected), 'refused': refused, 'exits': len(exits), 'checkpoints': checkpoints}


WORK_SOURCE = """record Item {
  n = 0
}
state items: list of Item
input add = message { n: number }
input touch = message { step: number }
on add {
  items.append(Item { n: add.n })
}
on touch {
  for i in items {
    i.n = i.n + touch.step
  }
}
view total = sum(i.n for i in items)
view big = each i in items where i.n > 0 {
  n = i.n
}
"""


def server_work(aup, pure, work):
    """A work budget (docs/design/017) over HTTP: as the list grows, the
    walking event, the folding view and the filtered table each come to run
    past the budget, from then on answer 422 work, and a refused event
    changes nothing; the cheap event that adds an item still runs."""
    source = work / 'work.aup'
    source.write_text(WORK_SOURCE)
    compile_module(aup, source, work / 'work.pmod')
    host = HttpHost([pure / 'build/pure-serve', '--module', work / 'work.pmod', '--store', work / 'work-store', '--work', 400])
    first = {}
    try:
        def total():
            status, _, _, body = http(host.port, 'GET', '/views/total')
            return status, body

        for n in range(1, 200):
            status, why, _, _ = http(host.port, 'POST', '/inputs/add?n=1')
            assert status == 200, (n, status, why)  # adding stays cheap
            before = total()
            status, why, _, _ = http(host.port, 'POST', '/inputs/touch?step=1')
            assert status in (200, 422) and (status == 200 or why == 'work'), (n, status, why)
            if status == 422:
                first.setdefault('touch', n)
                assert total() == before, n  # refused: nothing changed
            else:
                assert 'touch' not in first, n  # once past the budget, always past it
            status, why, _, body = http(host.port, 'GET', '/views/total')
            assert status == 200 or (status, why) == (422, 'work'), (n, status, why)
            if status == 422:
                first.setdefault('total', n)
            else:
                assert 'total' not in first, n
            status, why, _, body = http(host.port, 'GET', '/views/big')
            assert status == 200 or (status, why) == (422, 'work'), (n, status, why)
            if status == 422:
                first.setdefault('big', n)
            else:
                assert 'big' not in first and json.loads(body)['rows'] == n, (n, body[:80])
            if len(first) == 3:
                break
        assert len(first) == 3 and first['touch'] < first['big'], first  # a walk's block is longer than a filter's
    finally:
        host.stop()
    return first


APPS_SOURCE = """record Item {
  n = 0
}
state n = 0
state items: list of Item
input add = message { step: number }
input fill = message { k: number }
input walk = message { step: number }
on add {
  n = n + add.step
}
on fill {
  items.append(Item { n: fill.k })
}
on walk {
  for i in items {
    i.n = i.n + walk.step
  }
}
view total = n
view count = count(i in items where i.n > 0)
"""


def server_apps(aup, pure, work):
    """One host, many apps (docs/design/017): each app's routes start with its
    name and reach its own states (two apps of one module share nothing); an
    app whose module does not load, or whose store is another app's, stops
    alone (503 stopped) while the others run; past an app's queue budget its
    requests are refused (503 busy) and another app answers meanwhile; past
    its work budget an event is refused (422 work) where the same event runs
    in an app without one; an app whose store another process holds cannot
    save (503 store_busy, nothing applied) while the others save; and after a
    restart each app has its states back."""
    import fcntl
    import socket
    source = work / 'apps.aup'
    source.write_text(APPS_SOURCE)
    compile_module(aup, source, work / 'apps.pmod')
    (work / 'broken.pmod').write_bytes(b'PUREMOD6 not a module')
    manifest = work / 'apps.conf'
    manifest.write_text(f"""# name module store budgets
a      {work / 'apps.pmod'}   {work / 'store-a'}
b      {work / 'apps.pmod'}   {work / 'store-b'}
busy   {work / 'apps.pmod'}   {work / 'store-busy'}   queue=2
tight  {work / 'apps.pmod'}   {work / 'store-tight'}  work=55
broken {work / 'broken.pmod'} {work / 'store-broken'}
twin   {work / 'apps.pmod'}   {work / 'store-a'}
""")
    serve = pure / 'build/pure-serve'

    def get(port, path):
        status, why, _, body = http(port, 'GET', path)
        return status, why, body

    def post(port, path):
        status, why, _, _ = http(port, 'POST', path)
        return status, why

    host = HttpHost([serve, '--apps', manifest, '--workers', 2])
    try:
        running = {app['name']: app for app in host.info['apps']}
        assert [name for name, app in running.items() if app['running']] == ['a', 'b', 'busy', 'tight'], running
        assert running['broken']['why'].startswith('Module:') and running['twin']['why'] == "The store is a's", running
        port = host.port
        assert post(port, '/a/inputs/add?step=2') == (200, 'none')
        assert get(port, '/a/views/total')[::2] == (200, b'2') and get(port, '/b/views/total')[::2] == (200, b'0')
        assert get(port, '/broken/views/total')[:2] == (503, 'stopped') and post(port, '/twin/inputs/add?step=1')[0] == 503
        assert get(port, '/nobody/views/total')[:2] == (404, 'no_app') and get(port, '/views/total')[:2] == (404, 'no_app')
        # Work: ten items to walk; the tight app refuses the walk and changes nothing, a's runs it.
        for app in ('a', 'tight'):
            for k in range(10):
                assert post(port, f'/{app}/inputs/fill?k={k}') == (200, 'none')
        assert post(port, '/tight/inputs/walk?step=1') == (422, 'work') and get(port, '/tight/views/count')[::2] == (200, b'9')
        assert post(port, '/a/inputs/walk?step=1') == (200, 'none') and get(port, '/a/views/count')[::2] == (200, b'10')
        # Queue: requests at once, from as many connections; b answers meanwhile.
        flood = []
        for _ in range(120):
            connection = socket.create_connection(('127.0.0.1', port))
            flood.append(connection)
        for connection in flood:
            connection.sendall(b'POST /busy/inputs/add?step=1 HTTP/1.1\r\nHost: x\r\nContent-Length: 0\r\n\r\n')
        assert post(port, '/b/inputs/add?step=5') == (200, 'none')
        answers = []
        for connection in flood:
            head = b''
            while b'\r\n\r\n' not in head:
                head += connection.recv(4096)
            answers.append(head.split(b' ', 2)[1])
            connection.close()
        ok, busy = answers.count(b'200'), answers.count(b'503')
        assert ok + busy == len(flood) and busy > 0 and ok > 0, (ok, busy)
        assert get(port, '/busy/views/total')[::2] == (200, str(ok).encode())  # the refused changed nothing
        # The store of b held by another process: b cannot save, a can.
        with open(work / 'store-b' / '.writer.lock', 'r+b') as held:
            fcntl.lockf(held, fcntl.LOCK_EX | fcntl.LOCK_NB)
            assert post(port, '/b/inputs/add?step=1') == (503, 'store_busy')
            assert post(port, '/a/inputs/add?step=1') == (200, 'none')
            assert get(port, '/b/views/total')[::2] == (200, b'5')
            fcntl.lockf(held, fcntl.LOCK_UN)
        assert post(port, '/b/inputs/add?step=1') == (200, 'none')
    finally:
        stats = host.stop()[-1]
    by_name = {app['name']: app for app in stats['apps']}
    assert by_name['busy']['busy'] == busy and by_name['a']['events'] > 0 and stats['workers'] == 2, stats['apps']
    # Restarted, on three connection threads: each app's states are back, and
    # a connection whose first request is for an app at home on another
    # thread moves there (and is answered there).
    host = HttpHost([serve, '--apps', manifest, '--threads', 3])
    try:
        for _ in range(8):
            assert get(host.port, '/a/views/total')[::2] == (200, b'3') and get(host.port, '/b/views/total')[::2] == (200, b'6')
            assert get(host.port, '/a/views/count')[::2] == (200, b'10') and get(host.port, '/busy/views/total')[::2] == \
                (200, str(ok).encode())
            assert post(host.port, '/tight/inputs/add?step=1') == (200, 'none')
        assert get(host.port, '/tight/views/total')[::2] == (200, b'8')
    finally:
        moved = host.stop()[-1]['moved_home']
    assert moved > 0, moved
    return {'apps': len(running), 'stopped': 2, 'flooded': len(flood), 'busy': busy, 'moved_home': moved}


LINKS_SOURCE = """state n = 0
input put = message { k: u32 }
output out = message { k: u32 }
output here = message { k: u32 }
output named = message { k: u32 }
output small = message { k: u32 }
on put {
  n = n + 1
  send out { k: put.k }
  send here { k: put.k }
  send named { k: put.k }
  send small { k: put.k }
}
view count = n
"""
LINKS_TAKER = """record Got {
  k = 0
}
state got: list of Got
state last = 0
state ordered = true
input take = message { k: u32 }
on take {
  if take.k != last + 1 {
    ordered = false
  }
  got.append(Got { k: take.k })
  last = take.k
}
view count = count(g in got where g.k > 0)
view sum = sum(g.k for g in got)
view in_order = ordered
"""
# The same, its input laid out otherwise (a field before k): it takes the sends by name.
LINKS_NAMED = LINKS_TAKER.replace('input take = message { k: u32 }', 'input take = message {\n  note: text\n  k: u32\n}')
# The same, and it can let go of what it got (and what it heard stays).
LINKS_SMALL = LINKS_TAKER + """input clear = message { all: u32 }
on clear {
  for g in got {
    got.remove(g)
  }
}
view held = count(g in got where g.k > 0)
"""


def server_apps_links(aup, pure, work):
    """Apps of a host send (docs/design/017, stage 2). An app's send= wires
    each output: to an app of another host over a link (its sends' source
    APP.OUTPUT, sealed with the host's key, taken on that host's one
    --link-port as APP/INPUT), and to apps of its own host with no socket
    (send=OUTPUT=APP/INPUT): one taking the same shape, one taking the
    fields by name, one whose memory budget fills. Every send arrives once
    and in order, across a kill -9 of the sending host between requests;
    the full app's sends wait in the sender's outbox until it lets go of
    what it holds, and none is lost."""
    import signal
    import socket
    import time
    for name, text in (('links-src', LINKS_SOURCE), ('links-taker', LINKS_TAKER), ('links-named', LINKS_NAMED),
                       ('links-small', LINKS_SMALL)):
        (work / f'{name}.aup').write_text(text)
        compile_module(aup, work / f'{name}.aup', work / f'{name}.pmod')
    serve = pure / 'build/pure-serve'
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as probe:
        probe.bind(('127.0.0.1', 0))
        link_port = probe.getsockname()[1]
    keys = {}
    for side in ('sender', 'receiver'):
        (work / f'links-{side}.conf').write_text(f"x {work / 'links-taker.pmod'} {work / f'links-keys-{side}'}\n")
        host = HttpHost([serve, '--apps', work / f'links-{side}.conf', '--key', work / f'links-{side}.key'])
        keys[side] = host.info['public_key']
        host.stop()
    (work / 'links-far.conf').write_text(f"far {work / 'links-taker.pmod'} {work / 'links-far'}\n")
    (work / 'links-near.conf').write_text(
        f"src {work / 'links-src.pmod'} {work / 'links-src'} send=out=pure://127.0.0.1:{link_port}/far/take "
        f"seal=out={keys['receiver']} send=here=here/take send=named=byname/take send=small=small/take\n"
        f"here {work / 'links-taker.pmod'} {work / 'links-here'}\n"
        f"byname {work / 'links-named.pmod'} {work / 'links-byname'}\n"
        f"small {work / 'links-small.pmod'} {work / 'links-small'} memory=1200\n")
    far = HttpHost([serve, '--apps', work / 'links-far.conf', '--key', work / 'links-receiver.key',
                    '--link-port', link_port, '--sender', f"src.out={keys['sender']}"])
    near_args = [serve, '--apps', work / 'links-near.conf', '--key', work / 'links-sender.key', '--threads', 2]
    near = HttpHost(near_args)
    sent, killed_at = 400, 150
    try:
        for k in range(1, sent + 1):
            if k == killed_at:  # between requests: every send before it is stored, some not yet handed on
                near.process.send_signal(signal.SIGKILL)
                near.process.wait()
                near = HttpHost(near_args)
            status, why, _, _ = http(near.port, 'POST', f'/src/inputs/put?k={k}')
            assert status == 200, (k, status, why)

        def views(port, app):
            return [http(port, 'GET', f'/{app}/views/{view}')[3] for view in ('count', 'sum', 'in_order')]

        whole = [str(sent).encode(), str(sent * (sent + 1) // 2).encode(), b'true']
        deadline = time.time() + 30
        while time.time() < deadline and (views(far.port, 'far') != whole or views(near.port, 'here') != whole or
                                          views(near.port, 'byname') != whole):
            time.sleep(0.05)
        assert views(far.port, 'far') == whole, views(far.port, 'far')
        assert views(near.port, 'here') == whole, views(near.port, 'here')
        assert views(near.port, 'byname') == whole, views(near.port, 'byname')
        # The small app is full: it holds some, the rest wait in src's outbox; it lets go, and they come.
        held = int(http(near.port, 'GET', '/small/views/held')[3])
        assert 0 < held < sent, held
        taken = held
        for _ in range(sent):
            assert http(near.port, 'POST', '/small/inputs/clear?all=1')[0] == 200
            time.sleep(0.3)  # a held-back batch goes again after 100 ms
            now = int(http(near.port, 'GET', '/small/views/held')[3])
            if not now:
                break
            taken += now
        assert taken == sent and http(near.port, 'GET', '/small/views/in_order')[3] == b'true', taken
    finally:
        near_stats = near.stop()[-1]
        far.stop()
    assert near_stats['handed'] > 0 and near_stats['link_packets'] > 0, near_stats
    return {'sent': sent, 'killed_at': killed_at, 'handed': near_stats['handed'], 'small_held': held}


LIFE_SOURCE = """state n = 0
state hits = 0
input add = message { step: number }
on add {
  n = n + add.step
  hits = hits + 1
}
view total = n
view count = hits
"""
# Its next version: another input before add (add's number moves), and a state more.
LIFE_NEXT = """state twice = 0
state n = 0
state hits = 0
input reset = message { to: number }
input add = message { step: number }
on reset {
  n = reset.to
}
on add {
  n = n + add.step
  hits = hits + 1
  twice = twice + 2 * add.step
}
view total = n
view count = hits
view doubled = twice
"""
LIFE_SENDER = """state n = 0
input put = message { k: u32 }
output here = message { k: u32 }
output far = message { k: u32 }
on put {
  n = n + 1
  send here { k: put.k }
  send far { k: put.k }
}
view count = n
"""
# A taker's next version: an input before take, which takes a field more (the sends by name).
LIFE_TAKER_NEXT = LINKS_TAKER.replace('input take = message { k: u32 }', """state pokes = 0
input poke = message { x: u32 }
input take = message {
  note: text
  k: u32
}
on poke {
  pokes = pokes + poke.x
}""")


def server_apps_lifecycle(aup, pure, work):
    """Apps start, stop and change on a running host (docs/design/017,
    stage 3): on SIGHUP it reads its --apps file again and says what it did
    on a line. An app added answers; an app removed goes (404 no_app) and
    another process opens its store; apps change module in place while
    requests, another app's local sends and another node's link records
    come to them and their views are read, the new modules' inputs at other
    numbers and taking the sends by name, and the senders change too: a
    write answered 200 is applied once and one answered 503 stopped not at
    all, every send arrives once and in order, a view answers 200 or 503
    stopped, and the app left alone answers every request; an app whose
    store moves starts again on it. An app that could not open its store starts at
    the next reading once it can; a file with a wrong line changes nothing;
    after a kill -9 the host starts as the file says, the states as saved."""
    import os
    import signal
    import select
    import socket
    import threading
    import time
    for name, text in (('life', LIFE_SOURCE), ('life-next', LIFE_NEXT), ('life-sender', LIFE_SENDER),
                       ('life-taker', LINKS_TAKER), ('life-taker-next', LIFE_TAKER_NEXT)):
        (work / f'{name}.aup').write_text(text)
        compile_module(aup, work / f'{name}.aup', work / f'{name}.pmod')
    serve = pure / 'build/pure-serve'
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as probe:
        probe.bind(('127.0.0.1', 0))
        link_port = probe.getsockname()[1]
    keys = {}
    for side in ('host', 'far'):
        (work / f'life-{side}-key.conf').write_text(f"x {work / 'life.pmod'} {work / f'life-keys-{side}'}\n")
        host = HttpHost([serve, '--apps', work / f'life-{side}-key.conf', '--key', work / f'life-{side}.key'])
        keys[side] = host.info['public_key']
        host.stop()
    module = {name: work / f'{name}.pmod' for name in ('life', 'life-next', 'life-sender', 'life-taker', 'life-taker-next')}
    store = {name: work / f'life-store-{name}' for name in ('a', 'b', 'c', 'src', 'dst', 'dstl', 'e', 'srcf')}
    (work / 'life-ro').mkdir()
    store['e'] = work / 'life-ro' / 'e'
    lines = {'a': f"a {module['life']} {store['a']}", 'b': f"b {module['life']} {store['b']}",
             'src': f"src {module['life-sender']} {store['src']} send=here=dst/take",
             'dst': f"dst {module['life-taker']} {store['dst']}", 'dstl': f"dstl {module['life-taker']} {store['dstl']}"}
    conf = work / 'life.conf'

    def write(names):
        conf.write_text(''.join(lines[name] + '\n' for name in names))

    write(['a', 'b', 'src', 'dst', 'dstl'])
    args = [serve, '--apps', conf, '--key', work / 'life-host.key', '--link-port', link_port, '--threads', 2,
            '--sender', f"srcf.far={keys['far']}"]
    host = HttpHost(args)
    (work / 'life-far.conf').write_text(f"srcf {module['life-sender']} {store['srcf']} "
                                       f"send=far=pure://127.0.0.1:{link_port}/dstl/take seal=far={keys['host']}\n")
    far = HttpHost([serve, '--apps', work / 'life-far.conf', '--key', work / 'life-far.key'])

    def reading(names=None):
        """The host reads its file again: what it said."""
        if names is not None:
            write(names)
        host.process.send_signal(signal.SIGHUP)
        ready, _, _ = select.select([host.process.stdout], [], [], 30)
        assert ready, 'no reading'
        said = json.loads(host.process.stdout.readline())
        return said, {app['name']: app for app in said.get('apps', [])}

    def number(port, path):
        status, why, _, body = http(port, 'GET', path)
        assert status == 200, (path, status, why)
        return int(body) if body not in (b'true', b'false') else body == b'true'

    sent = 600
    try:
        # Added: it answers. Removed: its routes go, and its store is let go.
        assert http(host.port, 'POST', '/a/inputs/add?step=5')[0] == 200
        assert http(host.port, 'POST', '/b/inputs/add?step=7')[0] == 200
        lines['c'] = f"c {module['life']} {store['c']}"
        _, did = reading(['a', 'b', 'src', 'dst', 'dstl', 'c'])
        assert did['c'] == {'name': 'c', 'running': True, 'did': 'started'} and did['a']['did'] == 'kept', did
        assert http(host.port, 'POST', '/c/inputs/add?step=2')[0] == 200 and number(host.port, '/c/views/total') == 2
        _, did = reading(['a', 'src', 'dst', 'dstl', 'c'])
        assert did['b'] == {'name': 'b', 'running': False, 'did': 'removed'}, did
        assert http(host.port, 'GET', '/b/views/total')[:2] == (404, 'no_app')
        other = HttpHost([serve, '--module', module['life'], '--store', store['b']])
        try:
            assert number(other.port, '/views/total') == 7
        finally:
            other.stop()
        # Changed under load: a, dst (another app's local sends) and dstl (another node's link records) take
        # modules whose inputs are at other numbers, the takers' by name; then the senders start again, src
        # (its local outlet) and, on the far node, srcf (its link); c, left alone, answers every request. Each
        # change comes while the sends go (as far as each putter got: more than none, less than all).
        answered, refused, done = [0], [0], threading.Event()
        progress, trouble = {'src': 0, 'srcf': 0}, []

        def adder():
            while not done.is_set():
                status, why, _, _ = http(host.port, 'POST', '/a/inputs/add?step=1')
                if status == 200:
                    answered[0] += 1
                elif (status, why) == (503, 'stopped'):
                    refused[0] += 1
                else:
                    trouble.append(('a', status, why))

        def putter(port, app):
            for k in range(1, sent + 1):
                status, why, _, _ = http(port, 'POST', f'/{app}/inputs/put?k={k}')
                while (status, why) == (503, 'stopped'):  # its app starting again: not applied, sent again
                    time.sleep(0.005)
                    status, why, _, _ = http(port, 'POST', f'/{app}/inputs/put?k={k}')
                if status != 200:
                    trouble.append((app, k, status, why))
                    return
                progress[app] = k
                time.sleep(0.002)

        def watcher():
            while not done.is_set():
                status, why, _, _ = http(host.port, 'GET', '/c/views/total')
                if status != 200:
                    trouble.append(('c', status, why))

        def viewer():  # the changing apps' views, lists among them, read as they change
            while not done.is_set():
                for path in ('/dst/views/count', '/dstl/views/sum', '/a/views/total', '/dst/views/in_order'):
                    status, why, _, _ = http(host.port, 'GET', path)
                    if status != 200 and (status, why) != (503, 'stopped'):
                        trouble.append((path, status, why))

        def once(app, k):
            deadline = time.time() + 30
            while progress[app] < k and time.time() < deadline and not trouble:
                time.sleep(0.005)
            return dict(progress), answered[0]

        workers = [threading.Thread(target=adder), threading.Thread(target=putter, args=(host.port, 'src')),
                   threading.Thread(target=putter, args=(far.port, 'srcf')), threading.Thread(target=watcher),
                   threading.Thread(target=viewer)]
        for thread in workers:
            thread.start()
        during = []
        during.append(once('src', 100))
        lines['a'] = f"a {module['life-next']} {store['a']}"
        lines['dst'] = f"dst {module['life-taker-next']} {store['dst']}"
        lines['dstl'] = f"dstl {module['life-taker-next']} {store['dstl']}"
        _, did = reading(['a', 'src', 'dst', 'dstl', 'c'])
        assert [did[name]['did'] for name in ('a', 'dst', 'dstl', 'c', 'src')] == \
            ['changed', 'changed', 'changed', 'kept', 'kept'], did
        during.append(once('src', 200))
        lines['dst'] = f"dst {module['life-taker']} {store['dst']}"  # and back: take's number moves again
        _, did = reading(['a', 'src', 'dst', 'dstl', 'c'])
        assert did['dst']['did'] == 'changed' and did['a']['did'] == 'kept', did
        during.append(once('src', 300))
        lines['src'] += ' queue=100000'  # the sender starts again (its module as it was), its outbox going on
        _, did = reading(['a', 'src', 'dst', 'dstl', 'c'])
        assert did['src']['did'] == 'changed' and did['dst']['did'] == 'kept', did
        during.append(once('srcf', 400))
        (work / 'life-far.conf').write_text((work / 'life-far.conf').read_text().replace('\n', ' queue=100000\n'))
        far.process.send_signal(signal.SIGHUP)  # the far node's sender starts again, its link with it
        ready, _, _ = select.select([far.process.stdout], [], [], 30)
        assert ready and json.loads(far.process.stdout.readline())['apps'][0]['did'] == 'changed'
        for thread in workers[1:3]:
            thread.join()
        done.set()
        for thread in (workers[0], workers[3], workers[4]):
            thread.join()
        assert not trouble, trouble[:5]
        for put, adds in during:  # each change came while the sends went
            assert 0 < put['src'] < sent and 0 < put['srcf'] < sent and adds > 0, during
        whole = [sent, sent * (sent + 1) // 2, True]
        deadline = time.time() + 30
        while time.time() < deadline and ([number(host.port, f'/dst/views/{v}') for v in ('count', 'sum', 'in_order')] != whole or
                                          [number(host.port, f'/dstl/views/{v}') for v in ('count', 'sum', 'in_order')] != whole):
            time.sleep(0.05)
        for app in ('dst', 'dstl'):
            got = [number(host.port, f'/{app}/views/{v}') for v in ('count', 'sum', 'in_order')]
            assert got == whole, (app, got)
        total = 5 + answered[0]
        assert number(host.port, '/a/views/total') == total and number(host.port, '/a/views/count') == 1 + answered[0], \
            (number(host.port, '/a/views/total'), answered[0], refused[0])
        # An app whose store moves starts again, on the store it moves to.
        lines['c'] = f"c {module['life']} {work / 'life-store-c2'}"
        _, did = reading(['a', 'src', 'dst', 'dstl', 'c'])
        assert did['c']['did'] == 'restarted' and number(host.port, '/c/views/total') == 0, did
        # An app whose store cannot be made yet stops alone; once it can, the next reading starts it.
        lines['e'] = f"e {module['life']} {store['e']}"
        os.chmod(work / 'life-ro', 0o500)
        try:
            _, did = reading(['a', 'src', 'dst', 'dstl', 'c', 'e'])
        finally:
            os.chmod(work / 'life-ro', 0o700)
        assert not did['e']['running'] and 'store' in did['e']['why'] and did['c']['did'] == 'kept', did
        _, did = reading()
        assert did['e'] == {'name': 'e', 'running': True, 'did': 'started'}, did
        assert http(host.port, 'POST', '/e/inputs/add?step=3')[0] == 200
        # A file with a wrong line changes nothing.
        good = conf.read_text()
        conf.write_text(good + 'two words\n')  # NAME MODULE STORE at least
        said, _ = reading()
        assert 'error' in said and number(host.port, '/a/views/total') == total, said
        conf.write_text(good)
        # After a kill -9 the host starts as the file says, its states as saved.
        host.process.send_signal(signal.SIGKILL)
        host.process.wait()
        host = HttpHost(args)
        assert all(app['running'] for app in host.info['apps']), host.info
        assert sorted(app['name'] for app in host.info['apps']) == ['a', 'c', 'dst', 'dstl', 'e', 'src'], host.info
        assert number(host.port, '/a/views/total') == total and number(host.port, '/e/views/total') == 3
        assert [number(host.port, f'/dst/views/{v}') for v in ('count', 'sum', 'in_order')] == whole
    finally:
        host.stop()
        far.stop()
    return {'readings': 9, 'far_readings': 1, 'sent': sent, 'answered_while_changing': answered[0],
            'refused_while_changing': refused[0], 'puts_at_changes': [put['src'] for put, _ in during]}


TOKENS_SOURCE = """state n = 0
state got = 0
input add = message { step: number }
input other = message { step: number }
input take = message { k: u32 }
on add {
  n = n + add.step
}
on other {
  n = n + 1000
}
on take {
  got = got + take.k
}
view total = n
view taken = got
"""
TOKENS_SENDER = """input put = message { k: u32 }
output out = message { k: u32 }
on put {
  send out { k: put.k }
}
"""


TRANSIENT_SOURCE = """record Hit {
  n = 0
}
state count = 0
transient state online = 0
transient state recent: list of Hit
state hits: list of Hit
transient state status: text = "idle"
input tap = text
input peek = text
input say = text
on say {
  status = say
}
on tap {
  count = count + 1
  online = online + 1
  recent.append(Hit { n: online })
  hits.append(Hit { n: count })
}
on peek {
  online = online + 10
  recent.append(Hit { n: online })
}
view total = count
view live = online
view seen = recent.count
view kept = hits.count
view said = status
"""


def server_transient(aup, pure, work):
    """Transient states (docs/design/018) are served as any other, and
    never stored: requests that change only them write nothing to the store,
    a store written with them between the same stored events is byte for
    byte the one written without, and a host killed and started again begins
    them anew while the stored states are back."""
    import signal
    module, source = work / 'transient.pmod', work / 'transient.aup'
    source.write_text(TRANSIENT_SOURCE)
    compile_module(aup, source, module)

    def files(store):
        return {str(p.relative_to(store)): p.read_bytes() for p in sorted(store.rglob('*')) if p.is_file()}

    def views(port):
        return {v: int(http(port, 'GET', f'/views/{v}')[3]) for v in ('total', 'live', 'seen', 'kept')}

    plain, mixed = work / 'transient-plain', work / 'transient-mixed'
    host = HttpHost([pure / 'build/pure-serve', '--module', module, '--store', plain])
    try:
        for _ in range(5):
            assert http(host.port, 'POST', '/inputs/tap', b'x')[0] == 200
    finally:
        host.stop()
    host = HttpHost([pure / 'build/pure-serve', '--module', module, '--store', mixed])
    quiet = 0
    try:
        for k in range(5):
            before = files(mixed)
            for _ in range(20):
                assert http(host.port, 'POST', '/inputs/peek', b'x')[0] == 200
            assert http(host.port, 'POST', '/inputs/say', f'busy {k}'.encode())[0] == 200
            assert http(host.port, 'GET', '/views/said')[3] == f'busy {k}'.encode()  # read from a snapshot
            assert files(mixed) == before, k  # only transient states changed: nothing written
            quiet += 20
            assert http(host.port, 'POST', '/inputs/tap', b'x')[0] == 200
        assert views(host.port) == {'total': 5, 'live': 1005, 'seen': 105, 'kept': 5}, views(host.port)
    finally:
        host.stop()
    stored_plain, stored_mixed = files(plain), files(mixed)
    assert stored_plain.keys() == stored_mixed.keys() and stored_plain == stored_mixed, \
        [name for name in stored_plain if stored_plain[name] != stored_mixed.get(name)]
    host = HttpHost([pure / 'build/pure-serve', '--module', module, '--store', mixed])
    assert views(host.port) == {'total': 5, 'live': 0, 'seen': 0, 'kept': 5}, views(host.port)
    assert http(host.port, 'GET', '/views/said')[3] == b'idle'
    for _ in range(3):
        assert http(host.port, 'POST', '/inputs/peek', b'x')[0] == 200
    assert http(host.port, 'POST', '/inputs/tap', b'x')[0] == 200
    host.process.send_signal(signal.SIGKILL)
    host.process.communicate(timeout=30)
    host = HttpHost([pure / 'build/pure-serve', '--module', module, '--store', mixed])
    try:
        assert views(host.port) == {'total': 6, 'live': 0, 'seen': 0, 'kept': 6}, views(host.port)
    finally:
        host.stop()
    return {'quiet_requests': quiet, 'store_files_compared': len(stored_plain)}


RUNAWAY_SOURCE = """record R {
  n = 0
}
state count = 0
state xs: list of R
input spin = text
input fill = text
input pile = text
input tap = text
on spin {
  for i in 0 .. 1e15 {
    count = count + 1
  }
}
on fill {
  for i in 0 .. 1e15 {
    xs.append(R { n: i })
  }
}
on pile {
  let ys: list of R
  for i in 0 .. 1e15 {
    ys.append(R { n: i })
  }
}
on tap {
  count = count + 1
}
view total = count
view items = xs.count
"""


def server_runaway(aup, pure, work):
    """Loops that would not end (docs/design/018) on a host given no budget:
    one that only counts runs out of work, one that fills a stored list
    runs out of room (2^28 bytes for the list and its records to save and
    undo), one that fills an event's list reaches a list's most items; each
    ends within seconds, the host's peak memory grows by at most 300 MiB,
    nothing changes, and the host answers after."""
    import time
    module, source = work / 'runaway.pmod', work / 'runaway.aup'
    source.write_text(RUNAWAY_SOURCE)
    compile_module(aup, source, module)
    ended = {}
    for name, why in (('spin', ('work',)), ('fill', ('memory',)), ('pile', ('memory', 'limit'))):
        host = HttpHost([pure / 'build/pure-serve', '--module', module, '--store', work / f'runaway-{name}'])
        try:
            def peak():
                return next(int(line.split()[1]) for line in Path(f'/proc/{host.process.pid}/status').read_text().splitlines()
                            if line.startswith('VmHWM'))
            before, started = peak(), time.monotonic()
            status, error, _, _ = http(host.port, 'POST', f'/inputs/{name}', b'x')
            took, grew = time.monotonic() - started, (peak() - before) // 1024
            assert status in (422, 503) and error in why and took < 20 and grew <= 300, (name, status, error, took, grew)
            assert http(host.port, 'POST', '/inputs/tap', b'x')[0] == 200
            assert http(host.port, 'GET', '/views/total')[3] == b'1' and http(host.port, 'GET', '/views/items')[3] == b'0'
            ended[name] = {'error': error, 'seconds': round(took, 1), 'peak_grew_mib': grew}
        finally:
            host.stop()
    return ended


def server_view_bodies(aup, pure, work):
    """Views that compute (docs/design/018's stage 3): aup's routes example
    served, its distance view (a breadth first search in the view's body,
    in its own lists) asked by 64 connections at once with random
    arguments, each answer as a model's; its reach view without arguments.
    A body past the work budget answers 422 work, past the memory budget 503
    memory, and changes nothing."""
    import random
    import threading
    module = work / 'routes-view.pmod'
    compile_module(aup, EDITOR.parent / 'routes.aup', module)
    rng = random.Random(18)
    n, edges = 300, set()
    while len(edges) < 1200:
        a, b = rng.randrange(n), rng.randrange(n)
        if a != b:
            edges.add((a, b))
    out = {v: [] for v in range(n)}
    for a, b in edges:
        out[a].append(b)

    def far(a, b):
        depth, frontier = {a: 0}, [a]
        for v in frontier:
            for t in out[v]:
                if t not in depth:
                    depth[t] = depth[v] + 1
                    frontier.append(t)
        return depth.get(b, -1), depth

    store = work / 'routes-view'
    host = HttpHost([pure / 'build/pure-serve', '--module', module, '--store', store, '--threads', 3])
    asked, wrong = [0], []
    try:
        for a, b in sorted(edges):
            assert http(host.port, 'POST', f'/inputs/link?from={a}&to={b}&w=1')[0] == 200
        first = min(v for edge in edges for v in edge)
        _, depth = far(first, first)
        assert int(http(host.port, 'GET', '/views/reach')[3]) == len(depth) * 1000 + max(depth.values())

        def ask(seed):
            r = random.Random(seed)
            for _ in range(20):
                a, b = r.randrange(n), r.randrange(n)
                status, _, _, body = http(host.port, 'GET', f'/views/distance?start={a}&goal={b}')
                if status != 200 or int(body) != far(a, b)[0]:
                    wrong.append((a, b, status, body))
                asked[0] += 1

        threads = [threading.Thread(target=ask, args=(k,)) for k in range(64)]
        for t in threads:
            t.start()
        for t in threads:
            t.join()
        assert not wrong and asked[0] == 64 * 20, wrong[:5]
    finally:
        host.stop()
    for budget, status, error in (('--work', 422, 'work'), ('--memory', 503, 'memory')):
        host = HttpHost([pure / 'build/pure-serve', '--module', module, '--store', store, budget, 2000])
        try:
            got = http(host.port, 'GET', '/views/distance?start=0&goal=1')
            assert got[0] == status and got[1] == error, (budget, got)
            assert http(host.port, 'GET', '/views/graph')[3] == f'{n} nodes, {len(edges)} edges'.encode()
        finally:
            host.stop()
    return {'answers': asked[0], 'connections': 64}


SLOW_SOURCE = """state n = 0
input bump = text
on bump {
  n = n + 1
}
view slow {
  let acc = 0
  for i in 0 .. 3000000 {
    acc = acc + 1
  }
  return acc + n
}
"""


def server_view_workers(aup, pure, work):
    """Views with a body on the view workers (docs/design/020's stage 2):
    while 8 connections ask a view that loops 3,000,000 times, its app's
    module changes (SIGHUP) to one with another view in its place and the
    view tenfold, then the app is removed: every answer is the old module's,
    the new one's, 503 stopped or 404 (gone), none mixed. A stream of it
    gets the last value of writes made while it is evaluated (ids only
    growing); streams opened and closed at once leave nothing behind. Past
    the app's queue=2 asks are 503 busy, and the places come back. With
    --view-workers 0 the connection threads show it, as before."""
    import select
    import signal
    import threading
    module, tenfold = work / 'slow.pmod', work / 'slow-tenfold.pmod'
    (work / 'slow.aup').write_text(SLOW_SOURCE)
    compile_module(aup, work / 'slow.aup', module)
    (work / 'slow-tenfold.aup').write_text(SLOW_SOURCE.replace('view slow {', 'view other = 7\nview slow {')
                                           .replace('return acc + n', 'return acc + n * 10'))
    compile_module(aup, work / 'slow-tenfold.aup', tenfold)
    conf = work / 'slow.conf'

    def write(app_module, name='s'):
        conf.write_text(f"{name} {app_module} {work / ('slow-' + name)}\n")

    def reading():
        host.process.send_signal(signal.SIGHUP)
        ready, _, _ = select.select([host.process.stdout], [], [], 30)
        assert ready, 'no reading'
        return json.loads(host.process.stdout.readline())

    write(module)
    host = HttpHost([pure / 'build/pure-serve', '--apps', conf, '--threads', 1, '--view-workers', 2])
    answers, wrong, stop = {}, [], threading.Event()
    try:
        assert http(host.port, 'POST', '/s/inputs/bump', b'x')[0] == 200
        assert http(host.port, 'GET', '/s/views/slow')[3] == b'3000001'
        for _ in range(10):  # a stream that goes before its value is back
            stream = EventStream(host.port, '/s/views/slow')
            assert stream.head()[0] == 200
            stream.close()
        stream = EventStream(host.port, '/s/views/slow')
        first, _, value = stream.next()
        assert value == '3000001'
        for _ in range(3):  # written while it is evaluated: the newest snapshot's value after it
            assert http(host.port, 'POST', '/s/inputs/bump', b'x')[0] == 200
        ids = [first]
        while value != '3000004':
            ident, _, value = stream.next(timeout=10)
            ids.append(ident)
        assert ids == sorted(set(ids)), ids

        def ask():
            import http.client
            c = http.client.HTTPConnection('127.0.0.1', host.port, timeout=30)
            while not stop.is_set():
                try:
                    c.request('GET', '/s/views/slow')
                    response = c.getresponse()
                    got = (response.status, response.getheader('X-Pure-Error') if response.status != 200 else 'none',
                           response.read())
                except (ConnectionError, http.client.HTTPException):
                    c.close()
                    c = http.client.HTTPConnection('127.0.0.1', host.port, timeout=30)
                    continue
                answers[got] = answers.get(got, 0) + 1
                if got not in ((200, 'none', b'3000004'), (200, 'none', b'3000040'), (503, 'stopped', b'')) and got[0] != 404:
                    wrong.append(got)
            c.close()

        threads = [threading.Thread(target=ask) for _ in range(8)]
        for t in threads:
            t.start()
        time.sleep(0.5)
        write(tenfold)
        assert reading()['apps'][0]['did'] == 'changed'
        assert stream.next(timeout=10)[2] == '3000040'  # the stream, on the new module's view
        time.sleep(0.5)
        write(tenfold, 'q')  # s removed
        reading()
        assert stream.ends(timeout=10), 'a stream of an app gone still runs'
        time.sleep(0.3)
        stop.set()
        for t in threads:
            t.join()
        stream.close()
    finally:
        stop.set()
        stats = host.stop()
    old, new = answers.get((200, 'none', b'3000004'), 0), answers.get((200, 'none', b'3000040'), 0)
    assert not wrong and old and new, (wrong[:5], answers)
    assert stats[0]['view_workers'] == 2 and stats[0]['views_by_workers'] >= old + new and stats[0]['subscribers'] == 0, stats[0]
    by_workers = stats[0]['views_by_workers']
    write(module)
    host = HttpHost([pure / 'build/pure-serve', '--apps', conf, '--view-workers', 0])
    try:
        assert http(host.port, 'POST', '/s/inputs/bump', b'x')[0] == 200
        assert http(host.port, 'GET', '/s/views/slow')[3] == b'3000005'  # its store kept the bumps
    finally:
        stats = host.stop()
    assert stats[0]['view_workers'] == 0 and stats[0]['views_by_workers'] == 0, stats[0]
    conf.write_text(f"s {module} {work / 'slow-s'} queue=2\n")
    host = HttpHost([pure / 'build/pure-serve', '--apps', conf, '--threads', 1, '--view-workers', 1])
    busy, other = [0], []
    try:
        def crowd():
            for _ in range(6):
                got = http(host.port, 'GET', '/s/views/slow')
                if got[0] == 503 and got[1] == 'busy':
                    busy[0] += 1
                elif got[0] != 200 or got[3] != b'3000005':
                    other.append((got[0], got[1], got[3][:20]))

        threads = [threading.Thread(target=crowd) for _ in range(8)]
        for t in threads:
            t.start()
        for t in threads:
            t.join()
        assert busy[0] and not other, (busy, other[:5])
        assert http(host.port, 'GET', '/s/views/slow')[3] == b'3000005'  # the queue's places came back
    finally:
        host.stop()
    return {'old': old, 'new': new, 'stopped': answers.get((503, 'stopped', b''), 0),
            'gone': sum(v for k, v in answers.items() if k[0] == 404), 'by_workers': by_workers, 'pushed_ids': len(ids),
            'busy': busy[0]}


PUSH_SOURCE = """record Row {
  name: text
  n = 0
}
state count = 0
state note: text = "a"
state rows: list of Row
input tap = text
input same = text
input fill = message {
  k: u32
}
on tap {
  count = count + 1
  note = "line {count}\\nsecond"
}
on same {
  count = count
}
on fill {
  for i in 0 .. fill.k {
    rows.append(Row { name: "row {i} {count} ......................................................................................................................................................................................................................................................................................................................................................................", n: i })
  }
}
view total = count
view shown = note
view twice(k: u32) = count * k
view page = each r in rows {
  name = r.name
  n = r.n + count
}
view sum(k: u32) {
  let acc = 0
  for i in 0 .. k {
    acc = acc + count
  }
  return acc
}
"""


class EventStream:
    """A view's event stream (docs/design/019) read off a socket: its head,
    then its events as (id, event, data)."""

    def __init__(self, port, path, token=None, rcvbuf=None):
        import socket
        self.sock = socket.socket()
        if rcvbuf:
            self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, rcvbuf)
        self.sock.connect(('127.0.0.1', port))
        head = f'GET {path} HTTP/1.1\r\nHost: x\r\nAccept: text/event-stream\r\n'
        if token:
            head += f'Authorization: Bearer {token}\r\n'
        self.sock.sendall((head + '\r\n').encode())
        self.buffer = b''
        self.status = None

    def fill(self, timeout):
        self.sock.settimeout(timeout)
        data = self.sock.recv(1 << 20)
        if not data:
            raise EOFError
        self.buffer += data

    def head(self, timeout=5):
        while b'\r\n\r\n' not in self.buffer:
            self.fill(timeout)
        head, self.buffer = self.buffer.split(b'\r\n\r\n', 1)
        lines = head.decode().split('\r\n')
        self.status = int(lines[0].split()[1])
        return self.status, dict(line.split(': ', 1) for line in lines[1:])

    def next(self, timeout=5):
        if self.status is None:
            self.head(timeout)
        while True:
            while b'\n\n' not in self.buffer:
                self.fill(timeout)
            block, self.buffer = self.buffer.split(b'\n\n', 1)
            fields = {'data': []}
            for line in block.decode().split('\n'):
                name, _, value = line.partition(': ')
                if name == 'data':
                    fields['data'].append(value)
                else:
                    fields[name] = value
            if 'id' in fields:
                return int(fields['id']), fields.get('event'), '\n'.join(fields['data'])

    def ends(self, timeout=5):
        """Whether the host ends it (the socket closes) within timeout, events meanwhile let go."""
        import socket
        deadline = time.monotonic() + timeout
        try:
            while time.monotonic() < deadline:
                self.fill(max(0.01, deadline - time.monotonic()))
        except EOFError:
            return True
        except (socket.timeout, TimeoutError):
            return False
        return False

    def close(self):
        self.sock.close()


def server_push(aup, pure, work):
    """A view's value pushed to its subscribers (docs/design/019): GET with
    Accept: text/event-stream. The value comes first, then each value that
    differs (none for a write that leaves it), the newest at least after a
    burst, ids rising; arguments, table pages, lines of text and a view
    that computes; tokens; a host of apps whose module changes (a view of
    the same name goes on, one that went ends the stream) and whose tokens
    change (a token no longer granted ends it); a subscriber that does not
    read holds the host to two events (the waiting one replaced); and the
    host lets go of every subscriber that leaves."""
    import hashlib
    import select
    import signal
    module, source = work / 'push.pmod', work / 'push.aup'
    source.write_text(PUSH_SOURCE)
    compile_module(aup, source, module)
    host = HttpHost([pure / 'build/pure-serve', '--module', module, '--store', work / 'push', '--threads', 2])
    report = {}
    try:
        def post(path, body=b'x'):
            assert http(host.port, 'POST', path, body)[0] == 200, path

        total, shown, twice, sums = (EventStream(host.port, f'/views/{p}') for p in ('total', 'shown', 'twice?k=3', 'sum?k=4'))
        assert total.head()[0] == 200 and total.next()[1:] == (None, '0')
        assert shown.next()[2] == 'a' and twice.next()[2] == '0' and sums.next()[2] == '0'
        seen = []
        for k in range(1, 6):  # one write each 50 ms: each value comes, in order
            post('/inputs/tap')
            time.sleep(0.05)
            seen.append(total.next())
            assert seen[-1][2] == str(k), seen
            assert shown.next()[2] == f'line {k}\nsecond' and twice.next()[2] == str(3 * k) and sums.next()[2] == str(4 * k)
        assert [i for i, _, _ in seen] == sorted({i for i, _, _ in seen})
        post('/inputs/same')  # the value stays: nothing comes
        for _ in range(200):  # a burst: the newest comes at last, never an older after a newer
            post('/inputs/tap')
        last, values = seen[-1][0], []
        while not values or values[-1] != '205':
            i, _, value = total.next()
            assert i > last, (i, last)
            last = i
            values.append(value)
        assert [int(v) for v in values] == sorted(int(v) for v in values)
        report['burst_events'] = len(values)
        try:
            extra = total.next(timeout=0.3)
            raise AssertionError(f'an event after the last value: {extra}')
        except (TimeoutError, OSError):
            pass
        # A table page, and one that does not read (its socket's buffer small): the host keeps two events of it.
        assert http(host.port, 'POST', '/inputs/fill?k=100', b'')[0] == 200
        page = EventStream(host.port, '/views/page?from=0&count=100')
        slow = EventStream(host.port, '/views/page?from=0&count=100', rcvbuf=4096)
        first = json.loads(page.next()[2])
        assert first['rows'] == 100 and len(first['items']) == 100, first
        rss = lambda: next(int(line.split()[1]) for line in Path(f'/proc/{host.process.pid}/status').read_text().splitlines()
                           if line.startswith('VmRSS'))
        before = rss()
        for _ in range(600):
            post('/inputs/tap')
            time.sleep(0.002)
        grew = (rss() - before) // 1024
        final = None
        while final is None or final['items'][0]['n'] != 805:
            final = json.loads(slow.next(timeout=10)[2])
        report['slow_rss_growth_mib'] = grew
        assert grew < 32, grew
        for stream in (total, shown, twice, sums, page, slow):
            stream.close()
        time.sleep(0.2)
    finally:
        stats = host.stop()
    assert stats[0]['subscribers'] == 0 and stats[0]['subscribed'] == 6, stats[0]
    assert stats[0]['push_replaced'] >= 1, stats[0]
    report['replaced'] = stats[0]['push_replaced']
    # A host of apps: tokens, and modules and tokens that change while streams run.
    token, writer = 'r' * 40, 'w' * 40
    grant = lambda kind, t: f'{kind} {hashlib.sha256(t.encode()).hexdigest()}\n'
    (work / 'push.tokens').write_text(grant('read', token) + grant('write', writer))
    other = work / 'push-tenfold.aup'
    other.write_text(PUSH_SOURCE.replace('view total = count', 'view total = count * 10'))
    compile_module(aup, other, work / 'push-tenfold.pmod')
    gone = work / 'push-gone.aup'
    gone.write_text(PUSH_SOURCE.replace('view total = count\n', ''))
    compile_module(aup, gone, work / 'push-gone.pmod')
    conf = work / 'push.conf'

    def write(app_module, tokens=True):
        conf.write_text(f"p {app_module} {work / 'push-p'}" + (f" tokens={work / 'push.tokens'}" if tokens else '') + '\n')

    def reading():
        host.process.send_signal(signal.SIGHUP)
        ready, _, _ = select.select([host.process.stdout], [], [], 30)
        assert ready, 'no reading'
        return json.loads(host.process.stdout.readline())

    write(module)
    host = HttpHost([pure / 'build/pure-serve', '--apps', conf])
    try:
        refused = EventStream(host.port, '/p/views/total')
        assert refused.head()[0] == 401
        refused.close()
        stream = EventStream(host.port, '/p/views/total', token=token)
        assert stream.next()[2] == '0'
        assert call_with(host.port, 'POST', '/p/inputs/tap', writer) == 200
        assert stream.next()[2] == '1'
        write(work / 'push-tenfold.pmod')
        said = reading()
        assert said['apps'][0]['did'] == 'changed', said
        assert stream.next()[2] == '10'  # the same stream, the new module's view
        assert call_with(host.port, 'POST', '/p/inputs/tap', writer) == 200
        assert stream.next()[2] == '20'
        (work / 'push.tokens').write_text(grant('read', 'another') + grant('write', writer))
        reading()
        assert call_with(host.port, 'POST', '/p/inputs/tap', 'another') == 403  # read alone
        assert stream.ends(), 'a stream whose token went still runs'
        (work / 'push.tokens').write_text(grant('read', token) + grant('write', writer))
        reading()
        stream = EventStream(host.port, '/p/views/total', token=token)
        assert stream.next()[2] == '20'
        write(work / 'push-gone.pmod')
        reading()
        assert stream.ends(), 'a stream of a view that went still runs'
        write(module)
        reading()
        stream = EventStream(host.port, '/p/views/total', token=token)
        assert stream.next()[2] == '2'
        conf.write_text(f"q {module} {work / 'push-q'}\n")  # p goes: its streams end
        said = reading()
        assert {app['name']: app['did'] for app in said['apps']}['p'] == 'removed', said
        assert stream.ends(), 'a stream of an app that went still runs'
    finally:
        stats = host.stop()
    assert stats[0]['subscribers'] == 0, stats[0]
    return report


def call_with(port, method, path, token, body=b'x'):
    """A request bearing a token: its status."""
    import http.client
    connection = http.client.HTTPConnection('127.0.0.1', port, timeout=10)
    try:
        connection.request(method, path, body, {'Authorization': f'Bearer {token}', 'Content-Length': str(len(body))})
        answer = connection.getresponse()
        answer.read()
        return answer.status
    finally:
        connection.close()


def server_tokens(aup, pure, work):
    """An app's routes take tokens (docs/design/017, 3b). With a tokens file
    an app answers no request without a token (401, WWW-Authenticate:
    Bearer), nor with one it does not list; read reads its views, queries
    and documents, write does all, input=add posts to add alone (403
    past a grant), and a refused write changes nothing. Tokens of any
    length check (SHA-256 against hashlib's), on one connection as well as
    apart. An app with none is open. A new tokens file on SIGHUP takes
    effect while the app runs on (no 503); one that is not a tokens file
    stops the app (closed, not open). A delivery another node sealed with a
    key this host names comes with no token; an unsealed one needs one.
    The one app of a host takes --tokens."""
    import hashlib
    import http.client
    import select
    import signal
    import struct
    import threading
    import time
    for name, text in (('tokens', TOKENS_SOURCE), ('tokens-sender', TOKENS_SENDER)):
        (work / f'{name}.aup').write_text(text)
        compile_module(aup, work / f'{name}.aup', work / f'{name}.pmod')
    editor = work / 'tokens-editor.pmod'
    compile_module(aup, EDITOR, editor)
    serve = pure / 'build/pure-serve'
    names = {'read': 'r' * 40, 'write': 'w' * 40, 'input=add': 'i' * 40, 'read2': 'q' * 40}
    lengths = {n: bytes(65 + (k * 7) % 57 for k in range(n)).decode() for n in (1, 55, 56, 63, 64, 65, 119, 200)}

    def digest(token):
        return hashlib.sha256(token.encode()).hexdigest()

    def tokens_file(grants):
        return ''.join(f'{grant} {digest(token)}  # {grant}\n' for grant, token in grants)

    base = [('read', names['read']), ('write', names['write']), ('input=add', names['input=add'])] + \
           [('read', token) for token in lengths.values()]
    (work / 'tokens.list').write_text('# who may call t\n' + tokens_file(base))
    conf = work / 'tokens.conf'
    conf.write_text(f"t {work / 'tokens.pmod'} {work / 'tokens-t'} tokens={work / 'tokens.list'}\n"
                    f"open {work / 'tokens.pmod'} {work / 'tokens-open'}\n"
                    f"docs {editor} {work / 'tokens-docs'} tokens={work / 'tokens.list'}\n")
    host = HttpHost([serve, '--apps', conf])

    def call(method, path, token=None, body=b'', headers=None, port=None):
        connection = http.client.HTTPConnection('127.0.0.1', port or host.port, timeout=10)
        try:
            sent = dict(headers or {})
            if token is not None:
                sent['Authorization'] = f'Bearer {token}'
            if method != 'GET':
                sent['Content-Length'] = str(len(body))
            connection.request(method, path, body if method != 'GET' else None, sent)
            answer = connection.getresponse()
            return answer.status, answer.getheader('X-Pure-Error'), answer.getheader('WWW-Authenticate'), answer.read()
        finally:
            connection.close()

    try:
        none = call('GET', '/t/views/total')
        assert none[:3] == (401, 'unauthorized', 'Bearer'), none
        for method, path in (('POST', '/t/inputs/add?step=1'), ('GET', '/docs/objects/k'), ('PUT', '/t/queries/q'),
                             ('GET', '/t/queries/q')):
            assert call(method, path)[:2] == (401, 'unauthorized'), (method, path)
        assert call('GET', '/t/views/total', 'x' * 40)[:2] == (401, 'unauthorized')
        r, w, i = names['read'], names['write'], names['input=add']
        assert call('GET', '/t/views/total', r)[0] == 200
        assert call('GET', '/t/queries/q', r)[:2] == (404, 'no_value')  # read, and there is none
        assert call('POST', '/t/inputs/add?step=1', r)[:2] == (403, 'forbidden')
        assert call('PUT', '/docs/objects/k', r, b'x', {'If-None-Match': '*'})[:2] == (403, 'forbidden')
        assert call('POST', '/t/inputs/add?step=1', w)[0] == 200
        edit = struct.pack('<II', 0, 0) + b'hello'  # the edit: start, remove, insert
        assert call('PUT', '/docs/objects/k', w, edit, {'If-None-Match': '*'})[0] == 200
        assert call('GET', '/docs/objects/k', r)[::3] == (200, b'hello')
        assert call('POST', '/t/inputs/add?step=2', i)[0] == 200
        assert call('POST', '/t/inputs/other?step=1', i)[:2] == (403, 'forbidden')
        assert call('GET', '/t/views/total', i)[:2] == (403, 'forbidden')
        assert call('GET', '/t/views/total', w)[3] == b'3'  # the refused writes changed nothing
        for n, token in lengths.items():
            assert call('GET', '/t/views/total', token)[0] == 200, n
        assert call('GET', '/open/views/total')[0] == 200 and call('POST', '/open/inputs/add?step=1')[0] == 200
        # One connection: the same token checked again, then another, then none.
        connection = http.client.HTTPConnection('127.0.0.1', host.port, timeout=10)
        seen = []
        for token in (r, r, r, 'x' * 40, r, None):
            connection.request('GET', '/t/views/total', headers={'Authorization': f'Bearer {token}'} if token else {})
            answer = connection.getresponse()
            answer.read()
            seen.append(answer.status)
        connection.close()
        assert seen == [200, 200, 200, 401, 200, 401], seen
        # A new tokens file, the app running on: no 503 meanwhile.
        trouble, done = [], threading.Event()

        def caller():
            while not done.is_set():
                status = call('GET', '/t/views/total', w)[0]
                if status != 200:
                    trouble.append(status)

        thread = threading.Thread(target=caller)
        thread.start()
        time.sleep(0.2)
        (work / 'tokens.list').write_text(tokens_file([('read', names['read2']), ('write', w)]))

        def reading():
            host.process.send_signal(signal.SIGHUP)
            ready, _, _ = select.select([host.process.stdout], [], [], 30)
            assert ready, 'no reading'
            return {app['name']: app for app in json.loads(host.process.stdout.readline())['apps']}

        did = reading()
        time.sleep(0.2)
        done.set()
        thread.join()
        assert not trouble and did['t']['did'] == 'changed' and did['docs']['did'] == 'changed' and \
            did['open']['did'] == 'kept', (trouble[:5], did)
        assert call('GET', '/t/views/total', r)[:2] == (401, 'unauthorized')
        assert call('GET', '/t/views/total', names['read2'])[0] == 200
        # A tokens file that is not one: the app stops (it is never open), and starts once it is one.
        (work / 'tokens.list').write_text('read not-a-digest\n')
        did = reading()
        assert not did['t']['running'] and 'Tokens' in did['t']['why'], did
        assert call('GET', '/t/views/total')[:2] == (503, 'stopped')
        (work / 'tokens.list').write_text(tokens_file([('write', w)]))
        did = reading()
        assert did['t']['running'] and call('GET', '/t/views/total', w)[3] == b'3', did
        # An unsealed delivery (this host names no sender) needs a token.
        delivery = {'X-Pure-Source': 'far.out', 'X-Pure-Sequence': '1', 'Content-Type': 'application/json'}
        assert call('POST', '/t/inputs/take', None, b'{"k": 5}', delivery)[:2] == (401, 'unauthorized')
        assert call('POST', '/t/inputs/take', w, b'{"k": 5}', delivery)[0] == 200
        stats = host.stop()[-1]
        host = None
        assert (stats['unauthorized'], stats['forbidden']) == (10, 4), (stats['unauthorized'], stats['forbidden'])
        # A sealed delivery from a sender the host names comes with no token.
        keys = {}
        for side in ('sender', 'receiver'):
            (work / f'tokens-{side}-key.conf').write_text(f"x {work / 'tokens.pmod'} {work / f'tokens-keys-{side}'}\n")
            probe = HttpHost([serve, '--apps', work / f'tokens-{side}-key.conf', '--key', work / f'tokens-{side}.key'])
            keys[side] = probe.info['public_key']
            probe.stop()
        receiver = HttpHost([serve, '--apps', conf, '--key', work / 'tokens-receiver.key', '--sender', f"src.out={keys['sender']}"])
        (work / 'tokens-sender.conf').write_text(
            f"src {work / 'tokens-sender.pmod'} {work / 'tokens-src'} "
            f"send=out=http://127.0.0.1:{receiver.port}/t/inputs/take seal=out={keys['receiver']}\n")
        sender = HttpHost([serve, '--apps', work / 'tokens-sender.conf', '--key', work / 'tokens-sender.key'])
        try:
            for k in (1, 2, 3):
                assert call('POST', f'/src/inputs/put?k={k}', port=sender.port)[0] == 200
            deadline = time.time() + 20
            while time.time() < deadline and call('GET', '/t/views/taken', w, port=receiver.port)[3] != b'11':
                time.sleep(0.05)
            assert call('GET', '/t/views/taken', w, port=receiver.port)[3] == b'11'  # 5, then 1 + 2 + 3
        finally:
            sender.stop()
            receiver.stop()
        # The one app of a host: --tokens.
        one = HttpHost([serve, '--module', work / 'tokens.pmod', '--store', work / 'tokens-one', '--tokens', work / 'tokens.list'])
        try:
            assert call('GET', '/views/total', port=one.port)[:2] == (401, 'unauthorized')
            assert call('POST', '/inputs/add?step=4', w, port=one.port)[0] == 200
            assert call('GET', '/views/total', w, port=one.port)[3] == b'4'
        finally:
            one.stop()
    finally:
        if host:
            host.stop()
    return {'grants': len(base), 'token_lengths': sorted(lengths), 'one_connection': seen,
            'unauthorized': stats['unauthorized'], 'forbidden': stats['forbidden']}


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
        report['module_ranges'] = module_ranges(aup, pure, work)
        report['module_assumptions'] = module_assumptions(aup, pure, work)
        report['module_specs'] = module_specs(aup, pure, work)
        report['module_tier'] = module_tier(aup, pure, work)
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
        report['server_clocks'] = server_clocks(aup, pure, work)
        report['server_plain_edit'] = server_plain_edit(aup, pure, work)
        report['server_links'] = server_links(aup, pure, work)
        report['server_links_udp'] = server_links_udp(aup, pure, work)
        report['server_links_udp_lossy'] = server_links_udp_lossy(aup, pure, work)
        report['server_links_refused'] = server_links_refused(aup, pure, work)
        report['server_memory'] = server_memory(aup, pure, work)
        report['server_windows'] = server_windows(aup, pure, work)
        report['server_upgrade'] = server_upgrade(aup, pure, work)
        report['server_fields'] = server_fields(aup, pure, work)
        report['server_links_independent'] = server_links_independent(aup, pure, work)
        report['server_links_moved'] = server_links_moved(aup, pure, work)
        report['server_webhook'] = server_webhook(aup, pure, work)
        report['server_sealed'] = server_sealed(aup, pure, work)
        report['server_surface'] = server_surface(aup, pure, work)
        report['server_queries'] = server_queries(aup, pure, work)
        report['server_rows'] = server_rows(aup, pure, work)
        report['server_trees'] = server_trees(aup, pure, work)
        report['server_arguments'] = server_arguments(aup, pure, work)
        report['server_answers'] = server_answers(aup, pure, work)
        report['server_answers_memory'] = server_answers_memory(aup, pure, work)
        report['server_meaning'] = server_meaning(aup, pure, work)
        report['server_work'] = server_work(aup, pure, work)
        report['server_apps'] = server_apps(aup, pure, work)
        report['server_apps_links'] = server_apps_links(aup, pure, work)
        report['server_apps_lifecycle'] = server_apps_lifecycle(aup, pure, work)
        report['server_tokens'] = server_tokens(aup, pure, work)
        report['server_transient'] = server_transient(aup, pure, work)
        report['server_runaway'] = server_runaway(aup, pure, work)
        report['server_view_bodies'] = server_view_bodies(aup, pure, work)
        report['server_push'] = server_push(aup, pure, work)
        report['server_view_workers'] = server_view_workers(aup, pure, work)
    print(json.dumps(report))


if __name__ == '__main__':
    sys.exit(main())
