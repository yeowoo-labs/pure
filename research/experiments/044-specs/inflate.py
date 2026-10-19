# inflate (RFC 1951) in plain Python: the reference 140 counts aup's lines against. Codes are read a bit at a time
# from canonical counts (as zlib's puff.c does), so each step is the specification's.
LENGTH_BASE = [3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258]
LENGTH_EXTRA = [0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0]
DISTANCE_BASE = [1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073,
                 4097, 6145, 8193, 12289, 16385, 24577]
DISTANCE_EXTRA = [0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13]
ORDER = [16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15]


class Bits:
    def __init__(self, data):
        self.data, self.at = data, 0

    def take(self, n):
        v = 0
        for k in range(n):
            v |= (self.data[self.at >> 3] >> (self.at & 7) & 1) << k
            self.at += 1
        return v


def code(lengths):
    """A canonical code: how many codes of each length, and the symbols in code order."""
    counts = [lengths.count(n) for n in range(16)]
    return counts, [s for n in range(1, 16) for s in range(len(lengths)) if lengths[s] == n]


def decode(bits, counts, symbols):
    value = first = index = 0
    for n in range(1, 16):
        value |= bits.take(1)
        if value - first < counts[n]:
            return symbols[index + value - first]
        index += counts[n]
        first = (first + counts[n]) << 1
        value <<= 1
    raise ValueError('no such code')


def inflate(data):
    bits, out, last = Bits(data), bytearray(), 0
    while not last:
        last, kind = bits.take(1), bits.take(2)
        if kind == 0:  # stored: from the next byte, its length and the length's complement
            at = (bits.at + 7) >> 3
            size = data[at] | data[at + 1] << 8
            if size != (data[at + 2] | data[at + 3] << 8) ^ 0xffff or at + 4 + size > len(data):
                raise ValueError('stored length')
            out += data[at + 4:at + 4 + size]
            bits.at = (at + 4 + size) * 8
            continue
        if kind == 1:
            lengths, distances = code([8] * 144 + [9] * 112 + [7] * 24 + [8] * 8), code([5] * 30)
        elif kind == 2:
            literals, count, lengths = bits.take(5) + 257, bits.take(5) + 1, [0] * 19
            for n in range(bits.take(4) + 4):
                lengths[ORDER[n]] = bits.take(3)
            lengths = code(lengths)
            both = []
            while len(both) < literals + count:
                symbol = decode(bits, *lengths)
                if symbol < 16:
                    both.append(symbol)
                elif symbol == 16:
                    if not both:
                        raise ValueError('a repeat with nothing before it')
                    both += [both[-1]] * (3 + bits.take(2))
                else:
                    both += [0] * (3 + bits.take(3) if symbol == 17 else 11 + bits.take(7))
            if len(both) > literals + count:
                raise ValueError('too many lengths')
            lengths, distances = code(both[:literals]), code(both[literals:])
        else:
            raise ValueError('block type 3')
        while (symbol := decode(bits, *lengths)) != 256:
            if symbol < 256:
                out.append(symbol)
                continue
            symbol -= 257
            if symbol >= 29:
                raise ValueError('length symbol')
            length = LENGTH_BASE[symbol] + bits.take(LENGTH_EXTRA[symbol])
            symbol = decode(bits, *distances)
            if symbol >= 30:
                raise ValueError('distance symbol')
            distance = DISTANCE_BASE[symbol] + bits.take(DISTANCE_EXTRA[symbol])
            if distance > len(out):
                raise ValueError('too far back')
            for _ in range(length):
                out.append(out[-distance])
    return bytes(out)


if __name__ == '__main__':
    import random, sys, time, zlib
    size = int(sys.argv[1]) if len(sys.argv) > 1 else 65536
    chooser = random.Random(140)
    words = [bytes(chooser.choices(b'abcdefghijklmnopqrstuvwxyz', k=chooser.randrange(2, 9))) for _ in range(400)]
    text = b' '.join(chooser.choice(words) for _ in range(size))[:size]
    packer = zlib.compressobj(9, zlib.DEFLATED, -15)
    packed = packer.compress(text) + packer.flush()
    assert inflate(packed) == text
    runs = []
    for _ in range(7):
        start = time.perf_counter()
        inflate(packed)
        runs.append((time.perf_counter() - start) * 1e9 / size)
    print('{"size":%d,"packed":%d,"python_ns_per_byte":%.1f}' % (size, len(packed), sorted(runs)[3]))
