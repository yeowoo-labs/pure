#!/usr/bin/env python3
"""Research 140: line breaking (UAX #14, Unicode 16.0) written in aup (aup's examples/linebreak.aup).

Builds the properties' table from the UCD Vega's text is frozen at (its text/unicode.py reads the derived files): ranges
of code points, each its class after LB1, initial or final quotation (Pi, Pf), East Asian (F, W, H), and pictographic and
unassigned (Extended_Pictographic and Cn). Loads it into the app (a load message's rows), then gives it every line of
LineBreakTest.txt and random lines (code points drawn from every class), a line a message, on Pure's three paths
(batches, rounds, all compiled): each line's boundaries must be the test's (the random lines': linebreak.py's, which
passes the test whole), the paths the same. pure-run prints views alone (--views): the table would be in every line.
Then counts lines (code lines: neither blank nor a comment; brace lines: a closing brace alone) of linebreak.aup and of
linebreak.py. One JSON line; given a directory, it writes the table and the lines there for linebreak_time.c.

    python3 linebreak_check.py [DIRECTORY] [LINES]     (LINES: the test's lines to take, all by default)"""
import json, os, random, struct, subprocess, sys, tempfile
from pathlib import Path

here = os.path.dirname(os.path.abspath(__file__))
pure = os.path.normpath(os.path.join(here, '../../..'))
aup = os.environ.get('AUP', os.path.join(pure, '..', 'aup'))
vega = os.environ.get('VEGA', os.path.join(pure, '..', 'vega'))
ucd_path = Path(vega) / 'build/upstream/ucd-16.0.0'
sys.path.insert(0, here)
import linebreak  # noqa: E402

CLASSES = 'BK CR LF NL SP ZW ZWJ CM WJ GL BA BB B2 HY CB CL CP EX IN NS OP QU IS NU PO PR SY AK AL AP AS EB EM H2 H3 HL ID JL JV JT RI VF VI'.split()


def lines(path, stop=None):
    code = braces = 0
    for line in open(path):
        if stop and line.startswith(stop): break
        text = line.strip()
        if not text or text.startswith('#'): continue
        code += 1
        braces += text == '}'
    return code, braces


def table():
    """The ranges' starts and their rows (class, pi, pf, east, pictographic), every code point in one."""
    argv, sys.argv = sys.argv, ['unicode.py', str(ucd_path)]
    sys.path.insert(0, os.path.join(vega, 'text'))
    import unicode as ucd
    sys.argv = argv
    line = ucd.derived('extracted/DerivedLineBreak.txt', ucd.LINE, ucd.aliases('lb'))
    category = ucd.derived('extracted/DerivedGeneralCategory.txt', ucd.CATEGORIES)
    width = ucd.derived('extracted/DerivedEastAsianWidth.txt', ucd.WIDTHS, ucd.aliases('ea'))
    pictographic = bytearray(ucd.LAST + 1)
    for first, last, fields, missing in ucd.ranges('emoji/emoji-data.txt'):
        if not missing and fields[0] == 'Extended_Pictographic':
            pictographic[first:last + 1] = b'\1' * (last - first + 1)
    starts, rows = [], []
    for c in range(ucd.LAST + 1):
        name, g = ucd.LINE[line[c]], ucd.CATEGORIES[category[c]]
        if name in ('AI', 'SG', 'XX'): name = 'AL'  # LB1
        elif name == 'SA': name = 'CM' if g in ('Mn', 'Mc') else 'AL'
        elif name == 'CJ': name = 'NS'
        row = (CLASSES.index(name), g == 'Pi', g == 'Pf', ucd.WIDTHS[width[c]] in ('F', 'W', 'H'), bool(pictographic[c]) and g == 'Cn')
        if not rows or rows[-1] != row:
            starts.append(c)
            rows.append(row)
    return starts, rows


def test_lines(limit=None):
    out = []
    for text in open(ucd_path / 'auxiliary/LineBreakTest.txt', encoding='utf-8'):
        body = text.split('#', 1)[0].split()
        if body:
            out.append(([int(x, 16) for x in body if x not in ('÷', '×')], [x == '÷' for x in body if x in ('÷', '×')]))
    return out[:limit] if limit else out


def random_lines(chooser, starts, rows, count):
    """Lines of code points drawn from each class's ranges (and from the specials the rules name), their boundaries
    as linebreak.py works them out."""
    by_class = {}
    for k, (start, row) in enumerate(zip(starts, rows)):
        end = starts[k + 1] if k + 1 < len(starts) else 0x110000
        by_class.setdefault(row[0], []).append((start, end))
    specials = [0x2010, 0x25CC, 0x0020, 0x000A, 0x000D, 0x200D, 0x00AB, 0x00BB, 0x201C, 0x201D, 0x1F1E6, 0x1F3FB, 0x1F466]
    out = []
    for _ in range(count):
        codes = []
        for _ in range(chooser.randrange(1, 9)):
            if chooser.random() < 0.15:
                codes.append(chooser.choice(specials))
            else:
                start, end = chooser.choice(by_class[chooser.choice(list(by_class))])
                codes.append(chooser.randrange(start, end))
        out.append((codes, [b != linebreak.NONE for b in linebreak.breaks(codes, starts, rows)]))
    return out


def run(module, load, given, env):
    """Each line's boundaries as the app gives them (None where its event failed), pure-run's output streamed."""
    commands = load + '.commands'
    with open(commands, 'w') as f:
        f.write(f'message load @{load}\n')
        for codes, _ in given:
            f.write(f"message line {b''.join(struct.pack('<d', c) for c in codes).hex() or '-'}\n")
    with open(commands) as stdin:
        process = subprocess.Popen([os.path.join(pure, 'build', 'pure-run'), module, '--views'], stdin=stdin,
                                   stdout=subprocess.PIPE, text=True, env=dict(os.environ, **env))
    got, digest = [], 0
    for k, line in enumerate(process.stdout):
        digest = hash((digest, line))
        if k == 0: continue  # the load
        state = json.loads(line)
        got.append([b != 0 for b in bytes.fromhex(state['views']['boundaries'])] if state['fault'] == 'none' else None)
    process.wait()
    return got, digest


if __name__ == '__main__':
    starts, rows = table()
    for codes, want in test_lines():  # the reference passes the test whole first
        assert [b != linebreak.NONE for b in linebreak.breaks(codes, starts, rows)] == want, codes
    given = test_lines(int(sys.argv[2]) if len(sys.argv) > 2 else None)
    given += random_lines(random.Random(140), starts, rows, 2000)
    with tempfile.TemporaryDirectory() as work:
        load = os.path.join(work, 'ranges.bin')
        with open(load, 'wb') as f:
            f.write(b''.join(struct.pack('<6d', start, *row) for start, row in zip(starts, rows)))
        if len(sys.argv) > 1 and sys.argv[1]:
            os.makedirs(sys.argv[1], exist_ok=True)
            with open(os.path.join(sys.argv[1], 'ranges.bin'), 'wb') as f, open(load, 'rb') as g:
                f.write(g.read())
            with open(os.path.join(sys.argv[1], 'lines.bin'), 'wb') as f:  # each: u32 count, its code points, count + 1 bytes
                for codes, want in given:
                    f.write(struct.pack('<I', len(codes)) + struct.pack(f'<{len(codes)}I', *codes) + bytes(want))
        module = os.path.join(work, 'linebreak.pmod')
        subprocess.run([os.path.join(aup, 'build', 'aup'), 'build', os.path.join(aup, 'examples', 'linebreak.aup'), '-o', module],
                       check=True)
        right, digests = {}, set()
        for name, env in (('batches', {'PURE_JIT': '0'}), ('rounds', {'PURE_JIT': '0', 'PURE_BATCH': '0'}), ('all', {'PURE_JIT': 'all'})):
            got, digest = run(module, load, given, env)
            right[name] = sum(g == want for g, (_, want) in zip(got, given)) if len(got) == len(given) else 0
            digests.add(digest)
    tested = len(given) - 2000
    aup_code, aup_braces = lines(os.path.join(aup, 'examples', 'linebreak.aup'))
    python_code, _ = lines(os.path.join(here, 'linebreak.py'))
    ok = all(n == len(given) for n in right.values()) and len(digests) == 1
    print(json.dumps({'ranges': len(starts), 'test_lines': tested, 'random_lines': 2000, 'right': right,
                      'paths_same': len(digests) == 1, 'aup_lines': aup_code, 'aup_brace_lines': aup_braces,
                      'python_lines': python_code}))
    sys.exit(0 if ok else 1)
