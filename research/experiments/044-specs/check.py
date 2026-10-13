#!/usr/bin/env python3
"""Research 140 / design 027 stage 1: SHA-256 written in aup (aup's examples/sha256.aup).

Builds it with aup (AUP, default the sibling checkout), runs FIPS 180-2's
examples and messages of every length 0-129 and some longer ones on Pure's
three paths (batches, rounds, all compiled) and the default, each digest
against hashlib; then counts lines (code lines: neither blank nor a comment;
brace lines: a closing brace alone) of sha256.aup and of sha256.py's
algorithm (above its main). One JSON line."""
import hashlib, json, os, random, subprocess, sys, tempfile

here = os.path.dirname(os.path.abspath(__file__))
pure = os.path.normpath(os.path.join(here, '../../..'))
aup = os.environ.get('AUP', os.path.join(pure, '..', 'aup'))

def lines(path, stop=None):
    code = braces = 0
    for line in open(path):
        if stop and line.startswith(stop): break
        text = line.strip()
        if not text or text.startswith('#'): continue
        code += 1
        braces += text == '}'
    return code, braces

random.seed(140)
messages = [b'', b'abc', b'abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq',
            b'abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmnoijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu']
messages += [bytes(random.randrange(256) for _ in range(n)) for n in list(range(130)) + [255, 256, 511, 512, 1000, 4096, 10000, 32000]]
with tempfile.TemporaryDirectory() as tmp:
    module = os.path.join(tmp, 'sha256.pmod')
    subprocess.run([os.path.join(aup, 'build', 'aup'), 'build', os.path.join(aup, 'examples', 'sha256.aup'), '-o', module], check=True)
    given = ''.join(f"message hash {m.hex() or '-'}\n" for m in messages).encode()
    paths = {}
    for name, env in (('batches', {'PURE_JIT': '0'}), ('rounds', {'PURE_JIT': '0', 'PURE_BATCH': '0'}),
                      ('all', {'PURE_JIT': 'all'}), ('default', {})):
        out = subprocess.run([os.path.join(pure, 'build', 'pure-run'), module], input=given, capture_output=True,
                             env=dict(os.environ, **env), check=True).stdout.decode().splitlines()
        paths[name] = sum(json.loads(o)['states']['digest'] == hashlib.sha256(m).hexdigest() and json.loads(o)['fault'] == 'none'
                          for m, o in zip(messages, out)) if len(out) == len(messages) else 0
aup_code, aup_braces = lines(os.path.join(aup, 'examples', 'sha256.aup'))
python_code, _ = lines(os.path.join(here, 'sha256.py'), stop='if __name__')
ok = all(n == len(messages) for n in paths.values())
print(json.dumps({'messages': len(messages), 'right': paths, 'aup_lines': aup_code, 'aup_brace_lines': aup_braces,
                  'python_lines': python_code}))
sys.exit(0 if ok else 1)
