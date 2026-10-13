# Whether bytes are one JSON text (RFC 8259): the design of aup's examples/json.aup in plain Python, the reference
# 140 counts its lines against (read once from the start, a stack of the arrays and objects open).
def space(b):
    return b in (0x20, 0x09, 0x0a, 0x0d)


def digit(b):
    return 0x30 <= b <= 0x39


def hexdigit(b):
    return digit(b) or 0x61 <= b <= 0x66 or 0x41 <= b <= 0x46


def valid(d):
    try:
        t = d.decode('utf-8')
    except UnicodeDecodeError:
        return False
    n, i, mode, ok, open_ = len(d), 0, 0, True, []  # mode 0: a value, 1: after one, 2: a key, 3: a key or }, 4: a value or ]
    while ok:
        while i < n and space(d[i]):
            i += 1
        if i == n:
            return mode == 1 and not open_
        b = d[i]
        if mode == 1:
            if not open_:
                ok = False
            elif b == 0x2c:
                mode = 0 if open_[-1] == 0x5b else 2
            elif b == open_[-1] + 2:
                open_.pop()
            else:
                ok = False
            i += 1
        elif mode == 3 and b == 0x7d or mode == 4 and b == 0x5d:
            open_.pop()
            mode = 1
            i += 1
        elif mode in (2, 3):
            ok = b == 0x22
            mode = 5
        elif b in (0x5b, 0x7b):
            open_.append(b)
            mode = 4 if b == 0x5b else 3
            i += 1
        elif b == 0x22 or mode == 5:
            i += 1
            while i < n and d[i] != 0x22 and ok:
                if d[i] < 0x20:
                    ok = False
                elif d[i] == 0x5c and i + 1 < n and d[i + 1] == 0x75:
                    ok = i + 5 < n and all(hexdigit(x) for x in d[i + 2:i + 6])
                    i += 6
                elif d[i] == 0x5c:
                    ok = i + 1 < n and d[i + 1] in b'"\\/bfnrt'
                    i += 2
                else:
                    i += 1
            ok = ok and i < n
            i += 1
            if mode == 5:
                while i < n and space(d[i]):
                    i += 1
                ok = ok and i < n and d[i] == 0x3a
                i += 1
                mode = 0
            else:
                mode = 1
        elif b == 0x2d or digit(b):
            if b == 0x2d:
                i += 1
            ok = i < n and digit(d[i])
            if ok and d[i] == 0x30:
                i += 1
            else:
                while i < n and digit(d[i]):
                    i += 1
            if ok and i < n and d[i] == 0x2e:
                i += 1
                ok = i < n and digit(d[i])
                while i < n and digit(d[i]):
                    i += 1
            if ok and i < n and d[i] in (0x65, 0x45):
                i += 1
                if i < n and d[i] in (0x2b, 0x2d):
                    i += 1
                ok = i < n and digit(d[i])
                while i < n and digit(d[i]):
                    i += 1
            mode = 1
        else:
            word = b'true' if b == 0x74 else b'false' if b == 0x66 else b'null'
            ok = d[i:i + len(word)] == word
            i += len(word)
            mode = 1
    return False


if __name__ == '__main__':
    import os, sys, time
    suite = sys.argv[1]
    files = sorted(os.listdir(suite))
    data = [open(os.path.join(suite, f), 'rb').read() for f in files]
    for f, d in zip(files, data):
        assert not f.startswith('y_') or valid(d), f
        assert not f.startswith('n_') or not valid(d), f
    big = b'[' + b','.join(b'{"key%d": [1.5e3, true, null, "text \\u00e9 %d"]}' % (k, k) for k in range(2000)) + b']'
    runs = []
    for _ in range(7):
        start = time.perf_counter()
        valid(big)
        runs.append((time.perf_counter() - start) * 1e9 / len(big))
    print('{"size":%d,"python_ns_per_byte":%.1f}' % (len(big), sorted(runs)[3]))
