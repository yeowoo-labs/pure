# DNS messages (RFC 1035) in plain Python: the reference 140 counts aup's lines against. A message's bytes read into its
# header and records, names in their wire form uncompressed (the owner's and those inside NS, CNAME, PTR, MX, SOA and
# SRV data), and written back with the owners compressed. Each pointer points before the last, so they end.
NAMED = {2: 0, 5: 0, 12: 0, 15: 2, 33: 6}  # NS, CNAME, PTR, MX, SRV: the bytes before the name


def name(d, i):
    """The name at i uncompressed, and where it ends in place."""
    out, j, end, below = b'', i, None, i
    while d[j]:
        if d[j] >= 0xc0:
            to = (d[j] & 0x3f) << 8 | d[j + 1]
            if to >= below:
                raise ValueError('a pointer forward')
            end, below, j = end or j + 2, to, to
        elif d[j] < 0x40:
            out += d[j:j + d[j] + 1]
            j += d[j] + 1
        else:
            raise ValueError('a label type')
    if len(out) >= 255:
        raise ValueError('a name too long')
    return out + b'\0', end or j + 1


def data(d, i, size, type):
    """A record's data, the names in it uncompressed."""
    if type in NAMED:
        n, end = name(d, i + NAMED[type])
        if end != i + size:
            raise ValueError('data length')
        return d[i:i + NAMED[type]] + n
    if type == 6:
        m, after = name(d, i)
        r, end = name(d, after)
        if end + 20 != i + size:
            raise ValueError('data length')
        return m + r + d[end:end + 20]
    return d[i:i + size]


def read(d):
    """The id, the flags and the records (section, name, type, class, ttl, data) of a message."""
    if len(d) < 12:
        raise ValueError('a header is 12 bytes')
    word = lambda i: d[i] << 8 | d[i + 1]
    records, at = [], 12
    for section in range(4):
        for _ in range(word(4 + 2 * section)):
            owner, at = name(d, at)
            type, klass = word(at), word(at + 2)
            at += 4
            if section == 0:
                records.append((0, owner, type, klass, 0, b''))
                continue
            ttl, size = word(at) << 16 | word(at + 2), word(at + 4)
            if at + 6 + size > len(d):
                raise ValueError('data length')
            records.append((section, owner, type, klass, ttl, data(d, at + 6, size, type)))
            at += 6 + size
    if at != len(d):
        raise ValueError('bytes after the records')
    return word(0), word(2), records


def write(id, flags, records):
    """The message's bytes, each owner from its first label whose rest was written before, as a pointer to it."""
    out = bytearray(id.to_bytes(2, 'big') + flags.to_bytes(2, 'big'))
    for section in range(4):
        out += sum(r[0] == section for r in records).to_bytes(2, 'big')
    seen = {}
    for section, owner, type, klass, ttl, rdata in records:
        k = 0
        while owner[k] and owner[k:] not in seen:
            if len(out) < 0x4000:
                seen[owner[k:]] = len(out)
            out += owner[k:k + owner[k] + 1]
            k += owner[k] + 1
        out += (0xc000 | seen[owner[k:]]).to_bytes(2, 'big') if owner[k] else b'\0'
        out += type.to_bytes(2, 'big') + klass.to_bytes(2, 'big')
        if section:
            out += ttl.to_bytes(4, 'big') + len(rdata).to_bytes(2, 'big') + rdata
    return bytes(out)


if __name__ == '__main__':
    import sys, time
    with open(sys.argv[1], 'rb') as f:  # dns_check.py's messages: each a u16 big-endian length, then its bytes
        given = f.read()
    messages, at = [], 0
    while at < len(given):
        messages.append(given[at + 2:at + 2 + int.from_bytes(given[at:at + 2], 'big')])
        at += 2 + len(messages[-1])
    size = sum(map(len, messages))
    runs = []
    for _ in range(7):
        start = time.perf_counter()
        for m in messages:
            write(*read(m))
        runs.append((time.perf_counter() - start) * 1e9 / size)
    print('{"messages":%d,"bytes":%d,"python_ns_per_byte":%.1f}' % (len(messages), size, sorted(runs)[3]))
