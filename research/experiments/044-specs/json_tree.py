# A JSON text read into a tree and walked breadth first for a digest: the design of aup's examples/json_tree.aup in
# plain Python (the reference 140 counts its lines against; the tree is Python's own values, a dict kept as pairs).
def space(b):
    return b in (0x20, 0x09, 0x0a, 0x0d)


def digit(b):
    return 0x30 <= b <= 0x39


def hexdigit(b):
    return digit(b) or 0x61 <= b <= 0x66 or 0x41 <= b <= 0x46


class Pairs(list):
    pass


def read(d):
    try:
        d.decode('utf-8')
    except UnicodeDecodeError:
        return None
    n, i, ok, mode, root, open_, v = len(d), 0, True, 0, [], [], []
    while ok:
        while i < n and space(d[i]):
            i += 1
        if i == n:
            ok = mode == 1 and not open_
            break
        b = d[i]
        if mode == 1:
            if not open_:
                ok = False
            elif b == 0x2c:
                mode = 2 if open_[-1]['object'] else 0
            elif b == 0x5d and not open_[-1]['object']:
                v.append(open_[-1]['items'])
            elif b == 0x7d and open_[-1]['object']:
                v.append(Pairs(open_[-1]['members']))
            else:
                ok = False
            if v:
                open_.pop()
            i += 1
        elif mode == 3 and b == 0x7d or mode == 4 and b == 0x5d:
            v.append(Pairs(open_[-1]['members']) if b == 0x7d else open_[-1]['items'])
            open_.pop()
            i += 1
        elif mode in (2, 3):
            ok = b == 0x22
            mode = 5
        elif b in (0x5b, 0x7b):
            open_.append({'items': [], 'members': [], 'name': '', 'object': b == 0x7b})
            mode = 4 if b == 0x5b else 3
            i += 1
        elif b == 0x22 or mode == 5:
            i += 1
            s = bytearray()
            while ok and i < n and d[i] != 0x22:
                start = i
                while i < n and d[i] != 0x22 and d[i] != 0x5c and d[i] >= 0x20:
                    i += 1
                s += d[start:i]
                if i < n and d[i] < 0x20:
                    ok = False
                elif i + 1 < n and d[i] == 0x5c:
                    e = d[i + 1]
                    if e == 0x75:
                        ok = i + 5 < n and all(hexdigit(x) for x in d[i + 2:i + 6])
                        u = int(d[i + 2:i + 6], 16) if ok else 0
                        i += 6
                        if 0xd800 <= u < 0xdc00 and i + 5 < n and d[i] == 0x5c and d[i + 1] == 0x75 and all(hexdigit(x) for x in d[i + 2:i + 6]):
                            low = int(d[i + 2:i + 6], 16)
                            if 0xdc00 <= low < 0xe000:
                                u = 0x10000 + ((u - 0xd800) << 10 | low - 0xdc00)
                                i += 6
                        s += chr(u).encode('utf-8', 'surrogatepass')
                    else:
                        ok = e in b'"\\/bfnrt'
                        s += {0x62: b'\b', 0x66: b'\f', 0x6e: b'\n', 0x72: b'\r', 0x74: b'\t'}.get(e, bytes([e]))
                        i += 2
                elif i < n and d[i] == 0x5c:
                    ok = False
            ok = ok and i < n
            i += 1
            try:
                t = s.decode('utf-8')
            except UnicodeDecodeError:
                return None
            if mode == 5:
                open_[-1]['name'] = t
                while i < n and space(d[i]):
                    i += 1
                ok = ok and i < n and d[i] == 0x3a
                i += 1
                mode = 0
            else:
                v.append(t)
        elif b == 0x2d or digit(b):
            start = i
            if b == 0x2d:
                i += 1
            while i < n and (digit(d[i]) or d[i] in b'.eE+-'):
                i += 1
            text = d[start:i].decode()
            import re
            if not re.fullmatch(r'-?(0|[1-9]\d*)(\.\d+)?([eE][+-]?\d+)?', text) or float(text) in (float('inf'), -float('inf')):
                return None
            v.append(float(text))
        else:
            word = b'true' if b == 0x74 else b'false' if b == 0x66 else b'null'
            ok = d[i:i + len(word)] == word
            i += len(word)
            if ok:
                v.append({b'true': True, b'false': False, b'null': None}[word])
        if ok and v:
            if not open_:
                ok = not root
                root.append(v[-1])
            elif open_[-1]['object']:
                open_[-1]['members'].append((open_[-1]['name'], v[-1]))
            else:
                open_[-1]['items'].append(v[-1])
            v.pop()
            mode = 1
    return root if ok else None


def digest(tree):
    d = dict(nulls=0, trues=0, falses=0, numbers=0, strings=0, arrays=0, objects=0, sum=0.0, text_bytes=0, key_bytes=0)
    q, at = [tree], 0
    while at < len(q):
        x = q[at]
        if x is None: d['nulls'] += 1
        elif x is True: d['trues'] += 1
        elif x is False: d['falses'] += 1
        elif isinstance(x, float): d['numbers'] += 1; d['sum'] += x
        elif isinstance(x, str): d['strings'] += 1; d['text_bytes'] += len(x.encode('utf-8'))
        elif isinstance(x, Pairs):
            d['objects'] += 1
            for k, y in x: d['key_bytes'] += len(k.encode('utf-8')); q.append(y)
        else: d['arrays'] += 1; q.extend(x)
        at += 1
    return d


if __name__ == '__main__':
    import json, os, sys, time
    suite = sys.argv[1]
    for name in sorted(os.listdir(suite)):
        tree = read(open(os.path.join(suite, name), 'rb').read())
        assert not name.startswith('y_') or tree is not None, name
        assert not name.startswith('n_') or tree is None, name
    big = b'[' + b','.join(b'{"key%d": [1.5e3, true, null, "text \\u00e9 %d"]}' % (k, k) for k in range(2000)) + b']'
    runs, loads = [], []
    for _ in range(7):
        start = time.perf_counter()
        digest(read(big)[0])
        runs.append((time.perf_counter() - start) * 1e9 / len(big))
        start = time.perf_counter()
        digest(json.loads(big, object_pairs_hook=Pairs))
        loads.append((time.perf_counter() - start) * 1e9 / len(big))
    print('{"size":%d,"python_ns_per_byte":%.1f,"python_json_module_ns_per_byte":%.1f}' % (len(big), sorted(runs)[3], sorted(loads)[3]))
