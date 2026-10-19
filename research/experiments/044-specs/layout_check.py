#!/usr/bin/env python3
"""Research 140: box layout (a subset of CSS Flexbox 1) written in aup (aup's examples/layout.aup).

Draws random trees of flex containers inside the subset layout.py states (one line, row or column, padding, gap,
justify-content, align-items, grow, shrink and a definite basis, cross sizes definite or stretched, min sizes 0), and
lays each out three ways: Taffy (0.15, the known implementation: layout_taffy, built by cargo), layout.py (the Python
reference) and the app on Pure's three paths (batches, rounds, all compiled), a tree a message of its nodes in preorder.
Taffy works in binary32: its boxes must be the reference's within TOLERANCE; the app's must be the reference's exactly,
the paths the same. A tree the reference refuses (a size from content) is drawn again, and counted; the items 9.7's
loop froze at their padding are counted too. Then counts lines (code lines: neither blank nor a comment; brace lines: a closing brace alone) of layout.aup
and of layout.py. One JSON line; given a directory, it writes the trees there for layout_time.c.

    python3 layout_check.py [TREES] [DIRECTORY]     (TREES: 2,000 by default)"""
import json, os, random, struct, subprocess, sys, tempfile

here = os.path.dirname(os.path.abspath(__file__))
pure = os.path.normpath(os.path.join(here, '../../..'))
aup = os.environ.get('AUP', os.path.join(pure, '..', 'aup'))
taffy = os.environ.get('TAFFY', os.path.join(here, 'layout_taffy', 'target', 'release', 'layout_taffy'))
sys.path.insert(0, here)
import layout  # noqa: E402

TOLERANCE = 0.01  # Taffy's binary32 against binary64, at sizes up to a thousand
JUSTIFY = ['start', 'end', 'center', 'space-between', 'space-around', 'space-evenly']
ALIGN = ['start', 'end', 'center', 'stretch']


def lines(path):
    code = braces = 0
    for line in open(path):
        text = line.strip()
        if not text or text.startswith('#'): continue
        code += 1
        braces += text == '}'
    return code, braces


def draw(chooser, depth=0, parent=None):
    """A node and its subtree: a container's own properties, and an item's in its parent (main size auto, the basis
    definite; the cross size definite, or auto when the parent stretches)."""
    node = {'dir': chooser.choice(['row', 'column']), 'pad': [chooser.choice([0, 0, 0, 1, 2, 4, 8]) for _ in range(4)],
            'gap': chooser.choice([0, 0, 2, 5, 10]), 'justify': chooser.choice(JUSTIFY), 'align': chooser.choice(ALIGN),
            'grow': 0, 'shrink': 0, 'basis': None, 'w': None, 'h': None}
    if parent is None:
        node['w'], node['h'] = chooser.randrange(100, 1000), chooser.randrange(100, 800)
    else:
        node['grow'], node['shrink'] = chooser.choice([0, 0, 1, 1, 2, 3]), chooser.choice([0, 1, 1, 1, 2, 3])
        node['basis'] = chooser.choice([0, 5, 10, 20, 40, 60, 100, 150, 250])
        cross = 'w' if parent['dir'] == 'column' else 'h'
        if parent['align'] != 'stretch' or chooser.random() < 0.4:
            node[cross] = chooser.choice([0, 10, 20, 30, 50, 80, 120, 200])
    count = chooser.choice([1, 2, 3, 4, 5]) if parent is None else chooser.choice([0, 0, 0, 1, 2, 3, 4]) if depth < 3 else 0
    node['kids'] = [draw(chooser, depth + 1, node) for _ in range(count)]
    return node


def rows(tree, parent=-1, out=None):
    """The nodes in preorder, as layout.py and the app take them."""
    out = [] if out is None else out
    me = len(out)
    out.append((parent, int(tree['dir'] == 'column'), -1 if tree['w'] is None else tree['w'], -1 if tree['h'] is None else tree['h'],
                tree['grow'], tree['shrink'], tree['basis'] or 0, *tree['pad'], tree['gap'], JUSTIFY.index(tree['justify']),
                ALIGN.index(tree['align'])))
    for kid in tree['kids']:
        rows(kid, me, out)
    return out


def trees(count, seed=140):
    chooser, out, refused = random.Random(seed), [], {}
    while len(out) < count:
        tree = draw(chooser)
        try:
            out.append((tree, rows(tree), layout.layout(rows(tree))))
        except layout.Refused as why:
            refused[str(why)] = refused.get(str(why), 0) + 1
    return out, refused


def floored(nodes, boxes):
    """The items 9.7's loop froze at their padding: shrunk from a basis above it to it."""
    count = 0
    for k, node in enumerate(nodes[1:], 1):
        column = nodes[node[0]][1]
        pad = node[9] + node[10] if column else node[7] + node[8]
        count += max(node[6], pad) > pad and boxes[k][3 if column else 2] == pad
    return count


def run(module, given, env, work):
    commands = os.path.join(work, 'layout.commands')
    with open(commands, 'w') as f:
        for _, nodes, _ in given:
            f.write(f"message tree {b''.join(struct.pack('<14d', *n) for n in nodes).hex()}\n")
    got = []
    with open(commands) as stdin:
        process = subprocess.Popen([os.path.join(pure, 'build', 'pure-run'), module], stdin=stdin, stdout=subprocess.PIPE,
                                   text=True, env=dict(os.environ, **env))
        for line in process.stdout:
            state = json.loads(line)
            got.append([[b['x'], b['y'], b['w'], b['h']] for b in state['states']['boxes']] if state['fault'] == 'none' else None)
        process.wait()
    return got


if __name__ == '__main__':
    count = int(sys.argv[1]) if len(sys.argv) > 1 else 2000
    given, refused = trees(count)
    oracle = subprocess.run([taffy], input=''.join(json.dumps(t) + '\n' for t, _, _ in given), capture_output=True, text=True,
                            check=True).stdout.splitlines()
    worst, taffy_right = 0.0, 0
    for (_, _, want), line in zip(given, oracle):
        boxes = json.loads(line)
        far = max(abs(a - b) for g, w in zip(boxes, want) for a, b in zip(g, w)) if len(boxes) == len(want) else float('inf')
        worst = max(worst, far)
        taffy_right += far <= TOLERANCE
    frozen = [floored(nodes, want) for _, nodes, want in given]
    report = {'trees': len(given), 'nodes': sum(len(n) for _, n, _ in given), 'refused': refused,
              'floored': {'items': sum(frozen), 'trees': sum(f > 0 for f in frozen)}, 'taffy_right': taffy_right,
              'taffy_worst': worst, 'right': {}, 'paths_same': True}
    if len(sys.argv) > 2:
        os.makedirs(sys.argv[2], exist_ok=True)
        with open(os.path.join(sys.argv[2], 'trees.bin'), 'wb') as f:  # each: u32 nodes, their rows, then their boxes
            for _, nodes, want in given:
                f.write(struct.pack('<I', len(nodes)) + b''.join(struct.pack('<14d', *n) for n in nodes) +
                        b''.join(struct.pack('<4d', *b) for b in want))
    source = os.path.join(aup, 'examples', 'layout.aup')
    if os.path.exists(source):
        with tempfile.TemporaryDirectory() as work:
            module = os.path.join(work, 'layout.pmod')
            subprocess.run([os.path.join(aup, 'build', 'aup'), 'build', source, '-o', module], check=True)
            outputs = {}
            for name, env in (('batches', {'PURE_JIT': '0'}), ('rounds', {'PURE_JIT': '0', 'PURE_BATCH': '0'}), ('all', {'PURE_JIT': 'all'})):
                outputs[name] = run(module, given, env, work)
                report['right'][name] = sum(g == w for g, (_, _, w) in zip(outputs[name], given))
            report['paths_same'] = outputs['batches'] == outputs['rounds'] == outputs['all']
        report['aup_lines'], report['aup_brace_lines'] = lines(source)
    report['python_lines'] = lines(os.path.join(here, 'layout.py'))[0]
    ok = taffy_right == len(given) and report['paths_same'] and all(n == len(given) for n in report['right'].values())
    print(json.dumps(report))
    sys.exit(0 if ok else 1)
