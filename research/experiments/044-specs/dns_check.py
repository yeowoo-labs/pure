#!/usr/bin/env python3
"""Research 140: DNS messages (RFC 1035) written in aup (aup's examples/dns.aup).

Builds it with aup (AUP, default the sibling checkout) and reads messages dnspython made (queries and responses: A,
AAAA, NS, CNAME, PTR, MX, SOA, TXT and SRV records, EDNS, names sharing suffixes so compression points back, labels of
63 bytes and bytes above 0x7f, names near 255 bytes) on Pure's three paths (batches, rounds, all compiled): each
message's id, flags and records (names uncompressed) as dnspython reads them, and the message the view wire writes back
read by dnspython as the same message. Then damaged messages (cut short, a bit flipped) against dns_message.py, which
reads them the same way: the same records and bytes when it takes one, a failed event when it refuses it. Then counts
lines (code lines: neither blank nor a comment; brace lines: a closing brace alone) of dns.aup and of dns_message.py.
One JSON line. Given a path, it also writes the messages there for dns_time.c."""
import json, os, random, subprocess, sys, tempfile

import dns.flags, dns.message, dns.rrset

here = os.path.dirname(os.path.abspath(__file__))
pure = os.path.normpath(os.path.join(here, '../../..'))
aup = os.environ.get('AUP', os.path.join(pure, '..', 'aup'))
sys.path.insert(0, here)
import dns_message  # noqa: E402


def lines(path, stop=None):
    code = braces = 0
    for line in open(path):
        if stop and line.startswith(stop): break
        text = line.strip()
        if not text or text.startswith('#'): continue
        code += 1
        braces += text == '}'
    return code, braces


def messages(chooser, count):
    def label():
        pick = chooser.random()
        if pick < 0.05:
            return ''.join(chooser.choices('abcdefghij', k=63))
        if pick < 0.1:
            return ''.join(f'\\{chooser.randrange(128, 256):03d}' for _ in range(chooser.randrange(1, 4)))
        return ''.join(chooser.choices('abcdefghijklmnopqrstuvwxyz0123456789-', k=chooser.randrange(1, 12)))

    zones = [f'{label()}.{chooser.choice(["com", "net", "org", "kr"])}.' for _ in range(6)]

    def name():
        pick = chooser.random()
        if pick < 0.03:  # 245 bytes
            return '.'.join(''.join(chooser.choices('xyz', k=60)) for _ in range(4)) + '.'
        if pick < 0.06:
            return '.'
        return '.'.join([label() for _ in range(chooser.randrange(0, 3))] + [chooser.choice(zones)])

    def rdata(kind):
        return {'A': lambda: '.'.join(str(chooser.randrange(256)) for _ in range(4)),
                'AAAA': lambda: ':'.join(f'{chooser.randrange(65536):x}' for _ in range(8)),
                'NS': name, 'CNAME': name, 'PTR': name,
                'MX': lambda: f'{chooser.randrange(65536)} {name()}',
                'SOA': lambda: f'{name()} {name()} ' + ' '.join(str(chooser.randrange(2 ** 32)) for _ in range(5)),
                'TXT': lambda: ' '.join('"' + ''.join(chooser.choices('abc xyz', k=chooser.randrange(0, 40))) + '"'
                                        for _ in range(chooser.randrange(1, 4))),
                'SRV': lambda: f'{chooser.randrange(65536)} {chooser.randrange(65536)} {chooser.randrange(65536)} {name()}'}[kind]()

    kinds = ['A', 'AAAA', 'NS', 'CNAME', 'PTR', 'MX', 'SOA', 'TXT', 'SRV']
    out = []
    for _ in range(count):
        q = dns.message.make_query(name(), chooser.choice(kinds), use_edns=0 if chooser.random() < 0.4 else None,
                                   payload=chooser.choice([512, 1232, 4096]), id=chooser.randrange(65536))
        if chooser.random() < 0.2:
            out.append(q)
            continue
        r = dns.message.make_response(q)
        r.flags |= chooser.choice([0, dns.flags.AA, dns.flags.TC | dns.flags.AA, dns.flags.RA])
        r.set_rcode(chooser.choice([0, 0, 0, 2, 3, 5]))
        ttls = {}  # an RRset's records share a TTL (RFC 2181), so one name and type keep theirs
        for section in (r.answer, r.authority, r.additional):
            for _ in range(chooser.choice([0, 1, 1, 2, 4])):
                kind, owner = chooser.choice(kinds), name()
                ttl = ttls.setdefault((owner, kind), chooser.randrange(2 ** 31))
                section.append(dns.rrset.from_text(owner, ttl, 'IN', kind, *[rdata(kind) for _ in range(chooser.randrange(1, 4))]))
        out.append(r)
    return out


def seen(m):
    """A message's id, flags and records (section, name, type, class, ttl, data) as dnspython reads it, each section's
    records in order (dnspython gathers a section's records of one name and type into one set, so its order is not the
    message's)."""
    records = [(0, q.name.to_wire(), q.rdtype, q.rdclass, 0, b'') for q in m.question]
    for section, rrsets in ((1, m.answer), (2, m.authority), (3, m.additional)):
        for rrset in rrsets:
            records += [(section, rrset.name.to_wire(), rrset.rdtype, rrset.rdclass, rrset.ttl, rd.to_wire()) for rd in rrset]
    if m.opt:
        records += [(3, b'\0', 41, m.opt.rdclass, m.opt.ttl, rd.to_wire()) for rd in m.opt]
    return m.id, m.flags, sorted(records)


def damaged(chooser, wires):
    out = []
    for wire in chooser.sample(wires, 150):
        cut = wire[:chooser.randrange(len(wire))]
        flipped = bytearray(wire)
        flipped[chooser.randrange(len(wire))] ^= 1 << chooser.randrange(8)
        for message in (cut, bytes(flipped)):
            try:
                id, flags, records = dns_message.read(message)
                out.append((message, (id, flags, records, dns_message.write(id, flags, records))))
            except (ValueError, IndexError):
                out.append((message, None))
    return out


def run(module, wires, env, work):
    given = []
    for n, wire in enumerate(wires):
        path = os.path.join(work, f'{n}.dns')
        with open(path, 'wb') as f:
            f.write(wire)
        given.append(f'message read @{path}\nshow\n')
    out = subprocess.run([os.path.join(pure, 'build', 'pure-run'), module], input=''.join(given), capture_output=True,
                         text=True, env=dict(os.environ, **env), timeout=1800).stdout.splitlines()
    got = []
    for line in out[1::2]:  # each message's show
        state = json.loads(line)
        s = state['states']
        records = [(r['section'], bytes.fromhex(r['name']), r['type'], r['class'], r['ttl'], bytes.fromhex(r['data']))
                   for r in s['records']]
        got.append((s['id'], s['flags'], records, bytes.fromhex(state['views']['wire'])))
    failed = [json.loads(line)['fault'] != 'none' for line in out[0::2]]
    return out, got, failed


chooser = random.Random(140)
made = messages(chooser, 400)
wires = [m.to_wire(max_size=65535, want_shuffle=False) for m in made]  # dnspython shuffles a set's records
if len(sys.argv) > 1:  # the messages for dns_time.c: each a u16 big-endian length, then its bytes (as over TCP)
    with open(sys.argv[1], 'wb') as f:
        f.write(b''.join(len(w).to_bytes(2, 'big') + w for w in wires))
broken = damaged(chooser, wires)
paths = {}
with tempfile.TemporaryDirectory() as work:
    module = os.path.join(work, 'dns.pmod')
    subprocess.run([os.path.join(aup, 'build', 'aup'), 'build', os.path.join(aup, 'examples', 'dns.aup'), '-o', module],
                   check=True)
    outputs = {}
    for name, env in (('batches', {'PURE_JIT': '0'}), ('rounds', {'PURE_JIT': '0', 'PURE_BATCH': '0'}),
                      ('all', {'PURE_JIT': 'all'})):
        raw, got, failed = run(module, wires, env, work)
        right = 0
        for wire, g, f in zip(wires, got, failed):
            read = dns.message.from_wire(wire)
            right += not f and (g[0], g[1], sorted(g[2])) == seen(read) and dns.message.from_wire(g[3]) == read and \
                dns_message.read(wire)[2] == g[2]
        paths[name] = right if len(got) == len(wires) else 0
        outputs[name] = raw
    same_paths = outputs['rounds'] == outputs['batches'] and outputs['all'] == outputs['batches']
    raw, got, failed = run(module, [m for m, _ in broken], {}, work)
    agreed = sum((want is None and f) or (want is not None and not f and g == want)
                 for (_, want), g, f in zip(broken, got, failed)) if len(got) == len(broken) else 0
    refused = sum(want is None for _, want in broken)
aup_code, aup_braces = lines(os.path.join(aup, 'examples', 'dns.aup'))
python_code, _ = lines(os.path.join(here, 'dns_message.py'), stop='if __name__')
ok = all(n == len(wires) for n in paths.values()) and same_paths and agreed == len(broken)
print(json.dumps({'messages': len(wires), 'bytes': sum(map(len, wires)), 'records': sum(len(dns_message.read(w)[2]) for w in wires),
                  'right': paths, 'paths_same': same_paths, 'damaged': len(broken), 'damaged_refused': refused,
                  'damaged_agreed': agreed, 'aup_lines': aup_code, 'aup_brace_lines': aup_braces, 'python_lines': python_code}))
sys.exit(0 if ok else 1)
