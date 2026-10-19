# HTTP/1.1 requests (RFC 9112) read a packet at a time in plain Python: the reference 140 counts aup's lines against.
# Each packet's bytes are added to what the last ones left, and every request they complete is read: its line, its
# fields (names lowercased), its body by Content-Length or chunked, a chunked one's trailers. The grammar is h11's.
TOKEN = set(b"!#$%&'*+-.^_`|~0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ")
HEX = set(b'0123456789abcdefABCDEF')
SPACE = set(b'\0\t\n\x0b\x0c\r ')  # never in a field value's words


def line(d, i):
    """The line from i to the next CRLF, and where the next starts; None, None if it has not come yet."""
    j = d.find(b'\r\n', i)
    return (None, None) if j < 0 else (d[i:j], j + 2)


def field(text):
    name, colon, value = text.partition(b':')
    value = value.strip(b' \t')
    if not colon or not name or not set(name) <= TOKEN or any(b in SPACE and b not in b' \t' for b in value):
        raise ValueError('a field')
    return name.lower(), value


def fields(d, i):
    """The field lines from i to an empty line, and where it ends; None, None if they have not all come."""
    out = []
    while True:
        text, i = line(d, i)
        if text is None or not text:
            return (None, None) if text is None else (out, i)
        out.append(field(text))


def request(d):
    """The request at the start of d and its length; None if d holds only part of one."""
    text, i = line(d, 0)
    if text is None:
        return None
    parts = text.split(b' ')
    if len(parts) != 3 or not parts[0] or not set(parts[0]) <= TOKEN or not parts[1] or \
            any(b < 0x21 or b > 0x7e for b in parts[1]) or parts[2] not in (b'HTTP/1.0', b'HTTP/1.1'):
        raise ValueError('a request line')
    method, target, minor = parts[0], parts[1], parts[2][-1] - 0x30
    found, i = fields(d, i)
    if found is None:
        return None
    named = lambda n: [v for k, v in found if k == n]
    hosts, coded, length = named(b'host'), named(b'transfer-encoding'), named(b'content-length')
    if len(hosts) > 1 or (minor == 1 and not hosts):
        raise ValueError('a host')
    body, trailers = b'', []
    if coded:
        if length or len(coded) > 1 or coded[0].lower() != b'chunked':
            raise ValueError('a coding')
        while True:
            text, i = line(d, i)
            if text is None:
                return None
            size, semi, ext = text.partition(b';')
            size = size if semi else size.rstrip(b' \t')
            if not 1 <= len(size) <= 20 or not set(size) <= HEX or b'\n' in ext or int(size, 16) > 2 ** 31:
                raise ValueError('a chunk size')  # 2^31 at most: ours, not h11's
            n = int(size, 16)
            if n == 0:
                trailers, i = fields(d, i)
                if trailers is None:
                    return None
                break
            if len(d) < i + n + 2:
                return None
            if d[i + n:i + n + 2] != b'\r\n':
                raise ValueError('a chunk end')
            body += d[i:i + n]
            i += n + 2
    elif length:
        if len(length) > 1 or not length[0].isdigit() or int(length[0]) > 2 ** 31:
            raise ValueError('a length')
        if len(d) < i + int(length[0]):
            return None
        body, i = d[i:i + int(length[0])], i + int(length[0])
    return (method, target, minor, found, body, trailers), i


def feed(pending, packet):
    """The requests the packet completes, and the bytes left for the next."""
    d, out = pending + packet, []
    while d:
        got = request(d)
        if got is None:
            break
        out.append(got[0])
        d = d[got[1]:]
    return out, d


if __name__ == '__main__':
    import sys, time
    import h11
    with open(sys.argv[1], 'rb') as f:  # http_check.py's whole streams: each a u32 big-endian length, then its bytes
        given = f.read()
    streams, at = [], 0
    while at < len(given):
        streams.append(given[at + 4:at + 4 + int.from_bytes(given[at:at + 4], 'big')])
        at += 4 + len(streams[-1])
    size = sum(map(len, streams))

    def by_h11(stream):  # h11 as a server reads each request, answers it, and goes on
        conn = h11.Connection(h11.SERVER)
        conn.receive_data(stream)
        while (event := conn.next_event()) not in (h11.NEED_DATA, h11.PAUSED):
            if isinstance(event, h11.EndOfMessage):
                conn.send(h11.Response(status_code=200, headers=[(b'content-length', b'0')]))
                conn.send(h11.EndOfMessage())
                if conn.our_state is h11.DONE and conn.their_state is h11.DONE:
                    conn.start_next_cycle()

    times = {}
    for name, read in (('python', lambda s: feed(b'', s)), ('h11', by_h11)):
        runs = []
        for _ in range(7):
            start = time.perf_counter()
            for s in streams:
                read(s)
            runs.append((time.perf_counter() - start) * 1e9 / size)
        times[name] = sorted(runs)[3]
    print('{"streams":%d,"bytes":%d,"python_ns_per_byte":%.1f,"h11_ns_per_byte":%.1f}' % (len(streams), size, times['python'], times['h11']))
