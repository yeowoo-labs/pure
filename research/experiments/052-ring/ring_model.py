"""A crash model of design 030's S2 (the app store's ring), before it is built.

The file as bytes: two slots, then commits, each a 40-byte link and its nodes
(leaves, each key's root, the catalog). A commit goes at the front (the
newest commit's end) while the bytes there are free, else (a run change) a
wrap marker at the front names its link's place in the lowest free extent
that holds twice it, else past the end after growing. A byte is needed while
a node of the newest version or a commit on the chain from the older counted
slot's head (H_rec) on is in it. 64 KiB segments come back whole when nothing
in them lives and every commit in them is older than H_rec; at an open the
free extents are worked out byte by byte (every byte not needed). A slot is
written when 16 commits or 256 KiB have passed since the newer slot's commit
(into the older one, or one that counts for nothing), and it names the commit
before (durable), never the one in its own sync: so no slot ever names a
commit that a crash took, nor one whose link or marker a crash took (a slot
naming its own commit could land with the commit's bytes but not its marker:
the store opens there, but the chain from the older slot stops short of it).

Each commit's writes (marker, link and nodes, slot, grown zeros) are cut by a
crash in every way: each absent, whole, or torn at its front or back. The
store must then open at the commit before or this one, every key's bytes as
the oracle has them; with the newer slot damaged at rest it must still reach
the newest commit (along the chain from the older slot). A writer resumed
from a crashed file goes on and may crash again.

A writer's start clears what a crash may have left where the next commit
goes (the front's 40 bytes) and any slot that counts for nothing, synced
before its first commit; a wrap marker binds its link's own digest. Else a
commit that the first start passed over (its link and bytes on the file, one
part short) comes back after a second crash, when the next attempt at its
place lands only bytes equal to its own (its link's first bytes, a part of
its bytes): a commit the store had taken back.

Negative controls (--break), each must fail: 'newer' takes H_rec from the
newer slot, 'chain' leaves the chain out of an open's free extents, 'live'
gives back a segment that still holds a live node, 'debris' starts without
clearing.

python3 ring_model.py [--commits N] [--seed S] [--break newer|chain|live|debris]
"""
import argparse, hashlib, random, struct, sys

DATA, SLOT, LINK, SEGMENT, GROW, CHECKPOINT, CHECKPOINT_BYTES, LEAF = 8192, 128, 40, 65536, 262144, 16, 262144, 1024


def digest(*parts):
    h = hashlib.blake2b(digest_size=8)
    for p in parts: h.update(p)
    return h.digest()


def check_of(data): return struct.unpack('<Q', digest(data))[0]


class File:
    def __init__(self, data=b''): self.b = bytearray(data)
    def write(self, at, data):
        if at + len(data) > len(self.b): self.b.extend(bytes(at + len(data) - len(self.b)))
        self.b[at:at + len(data)] = data
    def read(self, at, n): return bytes(self.b[at:at + n]) if 0 <= at and at + n <= len(self.b) else None


# ---- Records. A head: (seq, start, end, catalog, check).
def link(seq, end, catalog, check):
    body = b'PRL1' + struct.pack('<IQQQ', 0, end, catalog, check)
    return body + digest(struct.pack('<Q', seq), body)

def marker(seq, target, bound):
    """At the front, naming the next commit's link elsewhere: its place, and its link's own digest (bound)."""
    body = b'PRW1' + struct.pack('<IQ', 0, target) + bound + bytes(8)
    return body + digest(struct.pack('<Q', seq), body)

def encode_slot(h):
    body = b'PURERNG1' + struct.pack('<QQQQQ', *h)
    body += bytes(SLOT - len(body) - 8)
    return body + digest(body)

def decode_slot(f, which):
    b = f.read(which * 4096, SLOT)
    if not b or b[:8] != b'PURERNG1' or digest(b[:SLOT - 8]) != b[SLOT - 8:]: return None
    return struct.unpack_from('<QQQQQ', b, 8)

def decode_link(b, seq):
    if not b or b[:4] != b'PRL1' or digest(struct.pack('<Q', seq), b[:32]) != b[32:40]: return None
    return struct.unpack_from('<QQQ', b, 8)

def decode_marker(b, seq):
    if not b or b[:4] != b'PRW1' or digest(struct.pack('<Q', seq), b[:32]) != b[32:40]: return None
    return struct.unpack_from('<Q', b, 8)[0], b[16:24]


# ---- Nodes: a leaf b'L' + its bytes; a root b'R' + (ref, size) each; the catalog b'C' + (key, root ref, size) each.
def leaf_node(data): return b'L' + struct.pack('<I', len(data)) + data
def root_node(leaves): return b'R' + struct.pack('<I', len(leaves)) + b''.join(struct.pack('<QQ', r, n) for r, n in leaves)
def catalog_node(keys): return b'C' + struct.pack('<I', len(keys)) + b''.join(struct.pack('<QQQ', k, r, n) for k, r, n in keys)


def read_version(f, catalog):
    """{key: bytes} and every node's extent from a catalog ref; None if anything is off."""
    nodes, keys = [], {}
    c = f.read(catalog, 5)
    if not c or c[0] != ord('C'): return None
    n = struct.unpack_from('<I', c, 1)[0]
    c = f.read(catalog, 5 + 24 * n)
    if not c: return None
    nodes.append((catalog, len(c)))
    for i in range(n):
        k, r, size = struct.unpack_from('<QQQ', c, 5 + 24 * i)
        rb = f.read(r, 5)
        if not rb or rb[0] != ord('R'): return None
        rr = f.read(r, 5 + 16 * struct.unpack_from('<I', rb, 1)[0])
        if not rr: return None
        nodes.append((r, len(rr)))
        value = b''
        for j in range((len(rr) - 5) // 16):
            lr, ln = struct.unpack_from('<QQ', rr, 5 + 16 * j)
            lb = f.read(lr, 5 + ln)
            if not lb or lb[0] != ord('L') or struct.unpack_from('<I', lb, 1)[0] != ln: return None
            nodes.append((lr, len(lb)))
            value += lb[5:]
        if len(value) != size: return None
        keys[k] = value
    return keys, nodes


# ---- Opening
def whole(f, h):
    b = f.read(h[1], h[2] - h[1])
    return b is not None and check_of(b) == h[4]

def follow(f, h):
    """The commit after h (its link at h's end, or a marker there naming it), if whole: (head, marker at or None)."""
    seq, end = h[0], h[2]
    r, at, mark = f.read(end, LINK), end, None
    if decode_link(r, seq + 1) is None:
        named = decode_marker(r, seq + 1)
        if named is None or named[0] < DATA: return None
        mark, at, r = end, named[0], f.read(named[0], LINK)
        if decode_link(r, seq + 1) is None or r[32:40] != named[1]: return None  # that link, not one left there before
    nend, ncat, ncheck = decode_link(r, seq + 1)
    n = (seq + 1, at + LINK, nend, ncat, ncheck)
    return (n, mark) if at + LINK <= nend <= len(f.b) and whole(f, n) else None

def open_store(f):
    """(newest head, the slots' heads that count, each or None)."""
    heads = [decode_slot(f, i) for i in (0, 1)]
    heads = [h if h and h[2] <= len(f.b) else None for h in heads]
    for i in sorted((0, 1), key=lambda i: -(heads[i][0] if heads[i] else 0)):
        if heads[i] and not whole(f, heads[i]): heads[i] = None  # PSA-2026-013: counts for nothing
    counted = [h for h in heads if h]
    if not counted: return None
    h = max(counted)
    while (n := follow(f, h)): h = n[0]
    return h, heads


def merged(extents):
    out = []
    for lo, hi in sorted(e for e in extents if e[0] < e[1]):
        if out and out[-1][1] >= lo: out[-1][1] = max(out[-1][1], hi)
        else: out.append([lo, hi])
    return out

def minus(extents, lo, hi):
    out = []
    for a, b in extents:
        if b <= lo or a >= hi: out.append([a, b])
        else:
            if a < lo: out.append([a, lo])
            if b > hi: out.append([hi, b])
    return out


# ---- The writer
class Writer:
    def __init__(self, f, head, heads, broken=None):
        """As at an open: what is needed is the newest version's nodes and the chain from the older counted slot."""
        self.f, self.broken, self.head, self.heads = f, broken, head, list(heads)
        self.size = len(f.b)
        if broken != 'debris':  # what a crash left where the next commit goes, and slots that lead nowhere: gone, synced
            f.write(head[2], bytes(LINK))
            for i in (0, 1):
                if not self.heads[i]: f.write(i * 4096, bytes(SLOT))
        keys, nodes = read_version(f, head[3])
        self.keys, self.roots = keys, self.roots_of(head[3])
        self.live = {}    # segment -> live bytes (the newest version's nodes)
        self.newest = {}  # segment -> the newest seq of a commit (or marker) written into it
        for at, n in nodes: self.count(at, n, +1)
        chain = []
        counted = [h for h in self.heads if h]
        h = min(counted)
        chain.append((h[1] - LINK, h[2], h[0]))
        while h[0] < head[0]:
            n = follow(f, h)
            assert n, f'the chain from the older slot ({min(counted)[0]}) breaks after {h[0]}, short of {head[0]}'
            n, mark = n
            if mark is not None: chain.append((mark, mark + LINK, n[0]))
            chain.append((n[1] - LINK, n[2], n[0]))
            h = n
        free = [[DATA, self.size]]
        for at, n in nodes: free = minus(free, at, at + n)
        for lo, hi, seq in chain:
            self.wrote(lo, hi, seq)
            if broken != 'chain': free = minus(free, lo, hi)
        self.free = merged(free)
        self.since = 0  # commit bytes since the newer slot's

    def roots_of(self, catalog):
        c = self.f.read(catalog, 5 + 24 * struct.unpack_from('<I', self.f.read(catalog, 5), 1)[0])
        roots = {}
        for i in range((len(c) - 5) // 24):
            k, r, size = struct.unpack_from('<QQQ', c, 5 + 24 * i)
            rr = self.f.read(r, 5 + 16 * struct.unpack_from('<I', self.f.read(r, 5), 1)[0])
            roots[k] = (r, [struct.unpack_from('<QQ', rr, 5 + 16 * j) for j in range((len(rr) - 5) // 16)])
        return roots

    def segments(self, lo, hi): return range(lo // SEGMENT, (hi - 1) // SEGMENT + 1)

    def count(self, at, n, sign):
        for s in self.segments(at, at + n):
            self.live[s] = self.live.get(s, 0) + sign * (min(at + n, (s + 1) * SEGMENT) - max(at, s * SEGMENT))
            assert self.live[s] >= 0, f'segment {s} counted below nothing'

    def wrote(self, lo, hi, seq):
        for s in self.segments(lo, hi): self.newest[s] = max(self.newest.get(s, 0), seq)

    def h_rec(self):
        counted = [h[0] for h in self.heads if h]
        return max(counted) if self.broken == 'newer' else min(counted)

    def give_back(self):
        rec = self.h_rec()
        for s in sorted(set(self.live) | set(self.newest)):
            live = self.live.get(s, 0)
            if (live == 0 or (self.broken == 'live' and live < 256)) and self.newest.get(s, 0) < rec:
                self.live.pop(s, None)
                self.newest.pop(s, None)
                self.free.append([max(DATA, s * SEGMENT), min(self.size, (s + 1) * SEGMENT)])
        self.free = merged(self.free)

    def fits(self, lo, hi): return any(a <= lo and hi <= b for a, b in self.free)

    def nodes_at(self, base, changes, keys):
        out, roots = bytearray(), {}
        for k in sorted(keys):
            if k not in changes:
                roots[k] = self.roots[k]
                continue
            old = self.roots[k][1] if k in self.roots else []
            value, leaves = keys[k], []
            for j in range(0, max(len(value), 1), LEAF):
                part, i = value[j:j + LEAF], j // LEAF
                if i < len(old) and old[i][1] == len(part) and self.f.read(old[i][0] + 5, len(part)) == part:
                    leaves.append(old[i])
                else:
                    leaves.append((base + len(out), len(part)))
                    out += leaf_node(part)
            roots[k] = (base + len(out), leaves)
            out += root_node(leaves)
        cat = base + len(out)
        out += catalog_node([(k, roots[k][0], len(keys[k])) for k in sorted(keys)])
        return bytes(out), roots, cat

    def commit(self, changes):
        """The writes of one sync, in order, and what settle needs."""
        seq, front = self.head[0] + 1, self.head[2]
        keys = {k: v for k, v in self.keys.items()}
        for k, v in changes.items():
            if v is None: keys.pop(k, None)
            else: keys[k] = v
        self.free = merged(self.free + [[front, front + LINK]])  # the front's 40 bytes: this commit's link or marker
        self.give_back()
        need = LINK + len(self.nodes_at(0, changes, keys)[0]) + LINK  # its link, its nodes, the next front's 40 bytes
        writes, at = [], front
        if not self.fits(front, front + need):
            self.free = minus(self.free, front, front + LINK)  # the marker's
            target = next((a for a, b in self.free if b - a >= 2 * need), None)
            if target is None:
                old, self.size = self.size, ((self.size + 2 * need) // GROW + 1) * GROW
                writes.append(('zeros', old, bytes(self.size - old)))
                self.free = merged(self.free + [[old, self.size]])
                target = next(a for a, b in self.free if b == self.size)
            writes.append(('marker', front, None))
            self.wrote(front, front + LINK, seq)
            at = target
        body, roots, cat = self.nodes_at(at + LINK, changes, keys)
        head = (seq, at + LINK, at + LINK + len(body), cat, check_of(body))
        assert self.fits(at, head[2] + LINK), 'a commit goes only where bytes are free'
        made = link(seq, head[2], cat, head[4])
        writes = [('marker', front, marker(seq, at, made[32:40])) if x[0] == 'marker' else x for x in writes]
        writes.append(('commit', at, made + body))
        self.free = minus(self.free, at, head[2] + LINK)
        self.wrote(at, head[2], seq)
        self.since += head[2] - at
        counted = [h[0] if h else 0 for h in self.heads]
        older = 0 if counted[0] <= counted[1] else 1
        slot = self.head[0] - max(counted) >= CHECKPOINT or self.since >= CHECKPOINT_BYTES
        if slot: writes.append(('slot', older * 4096, encode_slot(self.head)))  # the commit before: durable
        return writes, (head, keys, roots, cat, older if slot else None)

    def settle(self, settled):
        """After the sync: the old version's dead nodes leave their segments' live counts, the front moves on."""
        head, keys, roots, cat, slot = settled
        self.previous = self.head
        old = self.extents(self.roots, self.head[3])
        self.roots, self.keys, self.head = roots, keys, head
        new = self.extents(roots, cat)
        for at, n in old - new: self.count(at, n, -1)
        for at, n in new - old: self.count(at, n, +1)
        if slot is not None:
            self.heads[slot] = self.previous
            self.since = head[2] - head[1] + LINK

    def extents(self, roots, cat):
        out = {(cat, 5 + 24 * len(roots))}
        for r, leaves in roots.values():
            out.add((r, 5 + 16 * len(leaves)))
            out.update((lr, 5 + ln) for lr, ln in leaves)
        return out


def cuts(data):
    """Each way a write lands: whole, absent, or torn at its front or back."""
    yield ('whole',)
    yield ('absent',)
    n = len(data)
    for p in sorted({1, n // 2, n - 1} - {0, n}):
        yield ('front', p)
        yield ('back', p)

def land(f, write, how):
    kind, at, data = write
    if how[0] == 'whole': f.write(at, data)
    elif how[0] == 'front': f.write(at, data[:how[1]])
    elif how[0] == 'back': f.write(at + how[1], data[how[1]:])


def opens_as(f, expected, why):
    """None if the store opens at one of expected's versions with its bytes, else what went wrong."""
    o = open_store(f)
    if o is None: return f'{why}: no head opens'
    if o[0][0] not in expected: return f'{why}: opened {o[0][0]}, not {sorted(expected)}'
    v = read_version(f, o[0][3])
    if v is None or v[0] != expected[o[0][0]]: return f'{why}: version {o[0][0]} reads wrong'
    return None


def first_store():
    f = File(bytes(DATA + GROW))
    value = b'0' * 100
    leaf = leaf_node(value)
    root = root_node([(DATA + LINK, len(value))])
    cat_at = DATA + LINK + len(leaf) + len(root)
    body = leaf + root + catalog_node([(1, DATA + LINK + len(leaf), len(value))])
    head = (1, DATA + LINK, DATA + LINK + len(body), cat_at, check_of(body))
    f.write(DATA, link(1, head[2], cat_at, head[4]) + body)
    f.write(0, encode_slot(head))
    f.write(4096, encode_slot(head))
    return f, {1: value}


def stream(rng, keys):
    """An editor-like stream: a text typed into, a big value now and then, small keys made and dropped."""
    r = rng.random()
    if r < 0.6:
        v = bytearray(keys.get(1, b''))
        p = rng.randrange(len(v) + 1)
        v[p:p] = bytes([65 + rng.randrange(26)]) * rng.randrange(1, 40)
        return {1: bytes(v[:6000] if len(v) > 12000 else v)}
    if r < 0.65: return {rng.randrange(2, 6): bytes([rng.randrange(256)]) * rng.randrange(2000, 30000)}
    k = rng.randrange(10, 20)
    return {k: None if k in keys and rng.random() < 0.2 else bytes([rng.randrange(256)]) * rng.randrange(8, 300)}


def run(commits, seed, broken, stats, f=None, keys=None, depth=0):
    rng = random.Random(seed)
    if f is None: f, keys = first_store()
    o = open_store(f)
    w = Writer(f, o[0], o[1], broken)
    for i in range(commits):
        before, writes_settled = bytes(f.b), w.commit(stream(rng, w.keys))
        writes, settled = writes_settled
        seq = settled[0][0]
        expected = {seq - 1: dict(w.keys), seq: settled[1]}
        stats['wraps'] += any(x[0] == 'marker' for x in writes)
        stats['growths'] += any(x[0] == 'zeros' for x in writes)
        stats['slots'] += settled[4] is not None
        if settled[4] is not None or len(writes) > 1 or rng.random() < 0.02:
            combos = [[]]
            for x in writes: combos = [c + [h] for c in combos for h in cuts(x[2])]
            if len(combos) > 81: combos = rng.sample(combos, 81)
            for combo in combos:
                g = File(before)
                for x, how in zip(writes, combo): land(g, x, how)
                stats['crashes'] += 1
                wrong = opens_as(g, expected, f'commit {seq} cut {[h[0] for h in combo]}')
                if wrong: return wrong
                if depth == 0 and rng.random() < 0.03:  # a writer resumed there, crashing again
                    stats['resumed'] += 1
                    o = open_store(g)
                    wrong = run(rng.randrange(30, 300), rng.randrange(1 << 30), broken, stats, g, None, depth + 1)
                    if wrong: return f'resumed after commit {seq}: {wrong}'
        for x in writes: f.write(x[1], x[2])
        w.settle(settled)
        stats['commits'] += 1
        wrong = opens_as(f, {seq: w.keys}, f'commit {seq} whole')
        if wrong: return wrong
        newer = max((0, 1), key=lambda i: w.heads[i][0] if w.heads[i] else 0)
        if w.heads[newer] and rng.random() < 0.05:  # the newer slot damaged at rest: the older slot's chain leads on
            g = File(f.b)
            g.write(newer * 4096 + 20, bytes([g.b[newer * 4096 + 20] ^ 0x5a]))
            stats['damaged_slots'] += 1
            wrong = opens_as(g, {seq: w.keys}, f'commit {seq} with the newer slot damaged')
            if wrong: return wrong
    stats['size'] = max(stats.get('size', 0), len(f.b)) if depth == 0 else stats.get('size', 0)
    return None


if __name__ == '__main__':
    p = argparse.ArgumentParser()
    p.add_argument('--commits', type=int, default=3000)
    p.add_argument('--seed', type=int, default=1)
    p.add_argument('--break', dest='broken', choices=['newer', 'chain', 'live', 'debris'])
    a = p.parse_args()
    stats = dict(commits=0, crashes=0, wraps=0, growths=0, slots=0, resumed=0, damaged_slots=0)
    try:
        wrong = run(a.commits, a.seed, a.broken, stats)
    except AssertionError as e:
        wrong = f'assertion: {e}'

    print(('FAILED ' + wrong) if wrong else 'passed', f'seed {a.seed}', a.broken or '', stats)
    sys.exit(1 if wrong else 0)
