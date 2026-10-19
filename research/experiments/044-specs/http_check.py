#!/usr/bin/env python3
"""Research 140: HTTP/1.1 requests read a packet at a time, written in aup (aup's examples/http.aup).

Builds it with aup (AUP, default the sibling checkout) and feeds it streams of pipelined requests (methods, targets,
HTTP/1.1 and a last HTTP/1.0, fields with mixed-case names and spaces around their values, bodies by Content-Length or
chunked with extensions and trailers, some streams ending in part of a request), each cut into packets: whole, at
random boundaries, and a byte at a time for the shorter ones, each packet an event. The requests read must be h11's (a
known implementation that reads as bytes come) and the bytes left those after the last of them, on every cut, and the
three paths (batches, rounds, all compiled) the same. Then damaged streams (cut short, a bit flipped) fed whole against http_requests.py, which reads
them the same way: the same requests and bytes left when it takes one, a failed event when it refuses it. Then counts
lines (code lines: neither blank nor a comment; brace lines: a closing brace alone) of http.aup and of
http_requests.py's algorithm. One JSON line. Given a path, it also writes the whole streams there for http_time.c; given a directory after it, Pure's
tests/specs/http there."""
import json, os, random, subprocess, sys, tempfile

import h11

here = os.path.dirname(os.path.abspath(__file__))
pure = os.path.normpath(os.path.join(here, '../../..'))
aup = os.environ.get('AUP', os.path.join(pure, '..', 'aup'))
sys.path.insert(0, here)
import http_requests  # noqa: E402

TOKEN = "!#$%&'*+-.^_`|~0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ"


def lines(path, stop=None):
    code = braces = 0
    for line in open(path):
        if stop and line.startswith(stop): break
        text = line.strip()
        if not text or text.startswith('#'): continue
        code += 1
        braces += text == '}'
    return code, braces


def streams(chooser, count):
    def word(chars, low, high):
        return ''.join(chooser.choices(chars, k=chooser.randrange(low, high))).encode()

    def ows():
        return chooser.choice([b'', b'', b' ', b'\t', b'  \t'])

    def request(minor):
        method = chooser.choice([b'GET', b'POST', b'PUT', b'DELETE', b'PATCH', b'OPTIONS', b'HEAD', word(TOKEN, 1, 8)])
        target = b'/' + bytes(chooser.randrange(0x21, 0x7f) for _ in range(chooser.randrange(0, 30)))
        fields = [(chooser.choice([b'Host', b'host', b'HOST']), b'example.com')]
        for _ in range(chooser.randrange(0, 6)):
            name = word(TOKEN, 1, 12)
            if name.lower() in (b'host', b'content-length', b'transfer-encoding', b'connection', b'expect', b'upgrade'):
                continue
            value = (b' ' if chooser.random() < 0.5 else b'\t').join(
                bytes(chooser.randrange(0x21, 0x7f) for _ in range(chooser.randrange(1, 10))) for _ in range(chooser.randrange(0, 4)))
            fields.append((name, value))
        chooser.shuffle(fields)
        body, framing = b'', []
        pick = chooser.random()
        if pick < 0.35:
            body = bytes(chooser.randrange(256) for _ in range(chooser.randrange(0, 300)))
            framing = [(b'Content-Length', str(len(body)).encode())]
        elif pick < 0.65 and minor == 1:
            framing = [(chooser.choice([b'Transfer-Encoding', b'transfer-encoding']), chooser.choice([b'chunked', b'Chunked']))]
            chunks = [bytes(chooser.randrange(256) for _ in range(chooser.randrange(1, 120))) for _ in range(chooser.randrange(0, 5))]
            for c in chunks:
                size = (f'{len(c):x}' if chooser.random() < 0.5 else f'{len(c):X}').encode()
                size = b'0' * chooser.choice([0, 0, 1, 3]) + size
                tail = b';' + word('abc=', 0, 6) if chooser.random() < 0.3 else ows()
                body += size + tail + b'\r\n' + c + b'\r\n'
            trailers = b''.join(word(TOKEN, 1, 8) + b':' + ows() + word('xyz', 0, 6) + b'\r\n' for _ in range(chooser.randrange(0, 3)))
            body += b'0' + (b';x' if chooser.random() < 0.2 else b'') + b'\r\n' + trailers + b'\r\n'
        head = method + b' ' + target + b' HTTP/1.' + str(minor).encode() + b'\r\n'
        head += b''.join(n + b':' + ows() + v + ows() + b'\r\n' for n, v in fields + framing)
        return head + b'\r\n' + body

    out = []
    for _ in range(count):
        stream = b''.join(request(1) for _ in range(chooser.randrange(1, 5)))
        if chooser.random() < 0.2:
            stream += request(0)  # an HTTP/1.0 request ends its connection: last
        elif chooser.random() < 0.3:
            part = request(1)
            stream += part[:chooser.randrange(1, len(part))]
        out.append(stream)
    return out


def cuts(chooser, stream):
    """The stream whole, at random boundaries, and (if it is short) a byte at a time."""
    at = sorted(chooser.sample(range(1, len(stream)), min(len(stream) - 1, chooser.randrange(1, 12))))
    out = [[stream], [stream[a:b] for a, b in zip([0] + at, at + [len(stream)])]]
    if len(stream) < 400:
        out.append([stream[i:i + 1] for i in range(len(stream))])
    return out


def h11_reading(packets):
    """The requests h11 reads whole from the packets, each as http_requests' tuples. (h11 hands a request's line, fields
    and body on as they come, so the bytes left are those after the last whole request, as http_requests keeps them.)"""
    conn, out, current = h11.Connection(h11.SERVER), [], None
    for packet in packets:
        conn.receive_data(packet)
        while True:
            event = conn.next_event()
            if event in (h11.NEED_DATA, h11.PAUSED):
                break
            if isinstance(event, h11.Request):
                current = [event.method, event.target, int(event.http_version[-1:]), list(event.headers), b'', []]
            elif isinstance(event, h11.Data):
                current[4] += event.data
            elif isinstance(event, h11.EndOfMessage):
                current[5] = list(event.headers)
                out.append(tuple(current))
                conn.send(h11.Response(status_code=200, headers=[(b'content-length', b'0')]))
                conn.send(h11.EndOfMessage())
                if conn.our_state is h11.DONE and conn.their_state is h11.DONE:
                    conn.start_next_cycle()
    return out


def normal(requests):
    """Requests with transfer-encoding's value lowercased, as h11 hands it on (that field alone)."""
    return [(m, t, v, [(n, x.lower() if n == b'transfer-encoding' else x) for n, x in f], b, tr) for m, t, v, f, b, tr in requests]


def run(module, packets, env, work):
    given = []
    for n, packet in enumerate(packets):
        path = os.path.join(work, f'{n}.packet')
        with open(path, 'wb') as f:
            f.write(packet)
        given.append(f'message packet @{path}\n')
    out = subprocess.run([os.path.join(pure, 'build', 'pure-run'), module], input=''.join(given), capture_output=True,
                         text=True, env=dict(os.environ, **env), timeout=600).stdout.splitlines()
    state = json.loads(out[-1])
    s = state['states']
    pairs = lambda fs: [(bytes.fromhex(f['name']), bytes.fromhex(f['value'])) for f in fs]
    requests = [(bytes.fromhex(r['method']), bytes.fromhex(r['target']), r['minor'], pairs(r['fields']), bytes.fromhex(r['body']),
                 pairs(r['trailers'])) for r in s['requests']]
    left = bytes.fromhex(s['pending'])  # a bytes state (028)
    return out, (requests, left), any(json.loads(line)['fault'] != 'none' for line in out)


chooser = random.Random(140)
made = streams(chooser, 200)
if len(sys.argv) > 1:  # the whole streams (no request left in part) for http_time.c: each a u32 big-endian length, then it
    with open(sys.argv[1], 'wb') as f:
        f.write(b''.join(len(s).to_bytes(4, 'big') + s for s in made if not http_requests.feed(b'', s)[1]))
if len(sys.argv) > 2:  # Pure's tests/specs/http: the first 80 streams so, and what http_requests (h11's reading) reads
    with open(os.path.join(sys.argv[2], 'streams.bin'), 'wb') as f:
        f.write(b''.join(len(s).to_bytes(4, 'big') + s for s in made[:80]))
    hexed = lambda fs: [[n.hex(), v.hex()] for n, v in fs]
    with open(os.path.join(sys.argv[2], 'read.json'), 'w') as f:
        json.dump([{'requests': [[m.hex(), t.hex(), v, hexed(fs), b.hex(), hexed(tr)] for m, t, v, fs, b, tr in got], 'left': left.hex()}
                   for got, left in (http_requests.feed(b'', s) for s in made[:80])], f, separators=(',', ':'))
        f.write('\n')
paths = {'batches': 0, 'rounds': 0, 'all': 0}
fed = {'whole': 0, 'random': 0, 'bytes': 0}
same_paths = True
with tempfile.TemporaryDirectory() as work:
    module = os.path.join(work, 'http.pmod')
    subprocess.run([os.path.join(aup, 'build', 'aup'), 'build', os.path.join(aup, 'examples', 'http.aup'), '-o', module],
                   check=True)
    for stream in made:
        for kind, packets in zip(('whole', 'random', 'bytes'), cuts(chooser, stream)):
            want = http_requests.feed(b'', stream)
            assert normal(want[0]) == h11_reading(packets), 'the reference differs from h11'
            envs = (('batches', {'PURE_JIT': '0'}), ('rounds', {'PURE_JIT': '0', 'PURE_BATCH': '0'}), ('all', {'PURE_JIT': 'all'})) \
                if kind == 'random' else (('batches', {'PURE_JIT': '0'}),)
            raws = []
            for name, env in envs:
                raw, got, failed = run(module, packets, env, work)
                right = not failed and got == want
                fed[kind] += right and name == 'batches'
                if kind == 'random':
                    paths[name] += right
                raws.append(raw)
            same_paths = same_paths and all(r == raws[0] for r in raws)
    broken = []
    for stream in chooser.sample(made, 100):
        flipped = bytearray(stream)
        flipped[chooser.randrange(len(stream))] ^= 1 << chooser.randrange(8)
        broken += [stream[:chooser.randrange(1, len(stream))], bytes(flipped)]
    agreed = refused = 0
    for stream in broken:
        try:
            want = http_requests.feed(b'', stream)
        except ValueError:
            want = None
        refused += want is None
        _, got, failed = run(module, [stream], {}, work)
        agreed += (want is None and failed) or (want is not None and not failed and got == want)
aup_code, aup_braces = lines(os.path.join(aup, 'examples', 'http.aup'))
python_code, _ = lines(os.path.join(here, 'http_requests.py'), stop='if __name__')
ok = all(n == len(made) for n in paths.values()) and fed['whole'] == fed['random'] == len(made) and same_paths and \
    fed['bytes'] == sum(len(s) < 400 for s in made) and agreed == len(broken)
print(json.dumps({'streams': len(made), 'bytes': sum(map(len, made)),
                  'requests': sum(len(http_requests.feed(b'', s)[0]) for s in made), 'right': fed, 'paths': paths,
                  'paths_same': same_paths, 'damaged': len(broken), 'damaged_refused': refused, 'damaged_agreed': agreed,
                  'aup_lines': aup_code, 'aup_brace_lines': aup_braces, 'python_lines': python_code}))
sys.exit(0 if ok else 1)
