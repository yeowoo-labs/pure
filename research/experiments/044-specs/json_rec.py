# A JSON text read into a tree by recursive descent and walked depth first for a digest: the design of aup's
# examples/json_tree.aup in 027's stage 2c, in plain Python (the reference 140 counts its lines against; the tree is
# Python's own values, an object kept as pairs). A text that is no JSON raises (ValueError, or IndexError past its
# end), as the event fails.
import re


class Pairs(list):
    pass


def space(b):
    return b in (0x20, 0x09, 0x0a, 0x0d)


def digit(b):
    return 0x30 <= b <= 0x39


def hexdigit(b):
    return digit(b) or 0x61 <= b <= 0x66 or 0x41 <= b <= 0x46


def require(ok):
    if not ok:
        raise ValueError


def code(d, i):
    return int(d[i:i + 4], 16)


def escape(d, i):
    return d[i] == 0x5c and d[i + 1] == 0x75 and all(hexdigit(d[k]) for k in range(i + 2, i + 6))


def skip(d, i):
    while i < len(d) and space(d[i]):
        i += 1
    return i


def string(d, i):
    require(d[i] == 0x22)
    s, j = bytearray(), i + 1
    while d[j] != 0x22:
        start = j
        while d[j] != 0x22 and d[j] != 0x5c and d[j] >= 0x20:
            j += 1
        s += d[start:j]
        require(d[j] >= 0x20)
        if d[j] == 0x5c:
            e = d[j + 1]
            if e == 0x75:
                require(escape(d, j))
                u = code(d, j + 2)
                j += 6
                if 0xd800 <= u < 0xdc00 and j + 5 < len(d) and escape(d, j) and 0xdc00 <= code(d, j + 2) < 0xe000:
                    u = 0x10000 + ((u - 0xd800) << 10 | code(d, j + 2) - 0xdc00)
                    j += 6
                s += chr(u).encode()  # a lone surrogate raises
            else:
                require(e in b'"\\/bfnrt')
                s += {0x62: b'\b', 0x66: b'\f', 0x6e: b'\n', 0x72: b'\r', 0x74: b'\t'}.get(e, bytes([e]))
                j += 2
    return s.decode(), j + 1


def number(t):
    require(re.fullmatch(r'-?(0|[1-9]\d*)(\.\d+)?([eE][+-]?\d+)?', t))
    x = float(t)
    require(abs(x) != float('inf'))
    return x


def value(d, i):
    j = skip(d, i)
    b = d[j]
    if b in (0x5b, 0x7b):
        items = [] if b == 0x5b else Pairs()
        j = skip(d, j + 1)
        if d[j] != b + 2:
            while True:
                if b == 0x5b:
                    v, k = value(d, j)
                    items.append(v)
                else:
                    name, k = string(d, j)
                    k = skip(d, k)
                    require(d[k] == 0x3a)
                    v, k = value(d, k + 1)
                    items.append((name, v))
                j = skip(d, k)
                if d[j] != 0x2c:
                    break
                j = skip(d, j + 1)
            require(d[j] == b + 2)
        return items, j + 1
    if b == 0x22:
        return string(d, j)
    if b == 0x2d or digit(b):
        start, j = j, j + 1
        while j < len(d) and (digit(d[j]) or d[j] in b'.eE+-'):
            j += 1
        return number(d[start:j].decode()), j
    for word, v in ((b'true', True), (b'false', False), (b'null', None)):
        if d[j:j + len(word)] == word:
            return v, j + len(word)
    raise ValueError


def walk(x, d):
    if x is None: d['nulls'] += 1
    elif x is True: d['trues'] += 1
    elif x is False: d['falses'] += 1
    elif isinstance(x, float): d['numbers'] += 1; d['sum'] += x
    elif isinstance(x, str): d['strings'] += 1; d['text_bytes'] += len(x.encode())
    elif isinstance(x, Pairs):
        d['objects'] += 1
        for k, v in x:
            d['key_bytes'] += len(k.encode())
            walk(v, d)
    else:
        d['arrays'] += 1
        for y in x:
            walk(y, d)


def check(data):
    data.decode()  # UTF-8, as text(check.data) is
    root, end = value(data, 0)
    require(skip(data, end) == len(data))
    d = dict(nulls=0, trues=0, falses=0, numbers=0, strings=0, arrays=0, objects=0, sum=0.0, text_bytes=0, key_bytes=0)
    walk(root, d)
    return d


if __name__ == '__main__':
    import json, os, sys, time
    sys.setrecursionlimit(100000)
    suite = sys.argv[1]
    for name in sorted(os.listdir(suite)):
        try:
            got = check(open(os.path.join(suite, name), 'rb').read())
        except (ValueError, IndexError, RecursionError):
            got = None
        assert not name.startswith('y_') or got is not None, name
        assert not name.startswith('n_') or got is None, name
    big = b'[' + b','.join(b'{"key%d": [1.5e3, true, null, "text \\u00e9 %d"]}' % (k, k) for k in range(2000)) + b']'
    runs, loads = [], []
    for _ in range(7):
        start = time.perf_counter()
        check(big)
        runs.append((time.perf_counter() - start) * 1e9 / len(big))
        start = time.perf_counter()
        d = dict(nulls=0, trues=0, falses=0, numbers=0, strings=0, arrays=0, objects=0, sum=0.0, text_bytes=0, key_bytes=0)
        walk(json.loads(big, object_pairs_hook=Pairs, parse_int=float), d)
        loads.append((time.perf_counter() - start) * 1e9 / len(big))
    print('{"size":%d,"python_ns_per_byte":%.1f,"python_json_module_ns_per_byte":%.1f}' % (len(big), sorted(runs)[3], sorted(loads)[3]))
