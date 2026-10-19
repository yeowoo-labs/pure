#!/usr/bin/env python3
"""Research 140: inflate (RFC 1951) written in aup (aup's examples/inflate.aup).

Builds it with aup (AUP, default the sibling checkout) and inflates raw deflate streams zlib made (stored, fixed and
dynamic blocks: levels 0, 1, 6 and 9 with each of zlib's five strategies, on random bytes, words, short runs and far
repeats, 0 to 70,000 bytes) on Pure's three paths (batches, rounds, all compiled), each output against the original;
then damaged streams (cut short, a bit flipped) against inflate.py, which reads them the same way: the same bytes when
it takes one, a failed event when it refuses it. Then counts lines (code lines: neither blank nor a comment; brace
lines: a closing brace alone) of inflate.aup and of inflate.py's algorithm (above its main). One JSON line."""
import json, os, random, subprocess, sys, tempfile, zlib

here = os.path.dirname(os.path.abspath(__file__))
pure = os.path.normpath(os.path.join(here, '../../..'))
aup = os.environ.get('AUP', os.path.join(pure, '..', 'aup'))
sys.path.insert(0, here)
import inflate  # noqa: E402


def lines(path, stop=None):
    code = braces = 0
    for line in open(path):
        if stop and line.startswith(stop): break
        text = line.strip()
        if not text or text.startswith('#'): continue
        code += 1
        braces += text == '}'
    return code, braces


def corpus(chooser):
    words = [bytes(chooser.choices(b'abcdefghijklmnopqrstuvwxyz', k=chooser.randrange(2, 9))) for _ in range(400)]
    streams = []
    for size in (0, 1, 2, 3, 100, 1000, 10000, 70000):
        far = bytes(chooser.randrange(256) for _ in range(min(size, 33000)))
        kinds = {'random': bytes(chooser.randrange(256) for _ in range(size)),
                 'words': b' '.join(chooser.choice(words) for _ in range(size))[:size],
                 'runs': (bytes(chooser.choices(b'ab', k=7)) * size)[:size],
                 'far': (far + far[:32768] * 3)[:size]}  # repeats 32,768 bytes back at most
        for data in kinds.values():
            for level in (0, 1, 6, 9):
                for strategy in (zlib.Z_DEFAULT_STRATEGY, zlib.Z_FILTERED, zlib.Z_HUFFMAN_ONLY, zlib.Z_RLE, zlib.Z_FIXED):
                    packer = zlib.compressobj(level, zlib.DEFLATED, -15, 9, strategy)
                    streams.append((data, packer.compress(data) + packer.flush()))
    return streams


def damaged(chooser, streams):
    """Streams cut short or with a bit flipped, each with what inflate.py makes of it (None: refused)."""
    out = []
    small = [packed for data, packed in streams if 0 < len(packed) < 3000]
    for packed in chooser.sample(small, 120):
        cut = packed[:chooser.randrange(len(packed))]
        flipped = bytearray(packed)
        flipped[chooser.randrange(len(packed))] ^= 1 << chooser.randrange(8)
        for stream in (cut, bytes(flipped)):
            try:
                out.append((stream, inflate.inflate(stream)))
            except (ValueError, IndexError):
                out.append((stream, None))
    return out


def run(module, packed_streams, env, work):
    given = []
    for n, packed in enumerate(packed_streams):
        path = os.path.join(work, f'{n}.deflate')
        with open(path, 'wb') as f:
            f.write(packed)
        given.append(f'message inflate @{path}\n')
    out = subprocess.run([os.path.join(pure, 'build', 'pure-run'), module], input=''.join(given), capture_output=True,
                         text=True, env=dict(os.environ, **env), timeout=1800).stdout.splitlines()
    got = []
    for line in out:
        state = json.loads(line)
        got.append(bytes.fromhex(state['states']['inflated']) if state['fault'] == 'none' else None)  # a bytes state (028)
    return out, got


chooser = random.Random(140)
streams = corpus(chooser)
broken = damaged(chooser, streams)
paths = {}
with tempfile.TemporaryDirectory() as work:
    module = os.path.join(work, 'inflate.pmod')
    subprocess.run([os.path.join(aup, 'build', 'aup'), 'build', os.path.join(aup, 'examples', 'inflate.aup'), '-o', module],
                   check=True)
    outputs = {}
    for name, env in (('batches', {'PURE_JIT': '0'}), ('rounds', {'PURE_JIT': '0', 'PURE_BATCH': '0'}),
                      ('all', {'PURE_JIT': 'all'})):
        raw, got = run(module, [packed for _, packed in streams], env, work)
        paths[name] = sum(g == data for (data, _), g in zip(streams, got)) if len(got) == len(streams) else 0
        outputs[name] = raw
    same_paths = outputs['rounds'] == outputs['batches'] and outputs['all'] == outputs['batches']
    # A damaged stream after a good one: a refused one fails its event and leaves the good one's bytes.
    raw, got = run(module, [stream for stream, _ in broken], {}, work)
    agreed = sum(g == want or (want is None and g is None) for (_, want), g in zip(broken, got)) if len(got) == len(broken) else 0
    refused = sum(want is None for _, want in broken)
aup_code, aup_braces = lines(os.path.join(aup, 'examples', 'inflate.aup'))
python_code, _ = lines(os.path.join(here, 'inflate.py'), stop='if __name__')
ok = all(n == len(streams) for n in paths.values()) and same_paths and agreed == len(broken)
print(json.dumps({'streams': len(streams), 'bytes': sum(len(d) for d, _ in streams), 'right': paths, 'paths_same': same_paths,
                  'damaged': len(broken), 'damaged_refused': refused, 'damaged_agreed': agreed, 'aup_lines': aup_code,
                  'aup_brace_lines': aup_braces, 'python_lines': python_code}))
sys.exit(0 if ok else 1)
