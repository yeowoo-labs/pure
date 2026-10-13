# SHA-256 (FIPS 180-4) in plain Python: the reference 140 counts aup's lines against.
K = [
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
]
H = [0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19]


def rotr(x, n):
    return (x >> n | x << (32 - n)) & 0xffffffff


def sha256(data):
    h = list(H)
    padded = data + b'\x80' + b'\x00' * ((55 - len(data)) % 64) + (len(data) * 8).to_bytes(8, 'big')
    for at in range(0, len(padded), 64):
        w = [int.from_bytes(padded[at + 4 * t:at + 4 * t + 4], 'big') for t in range(16)]
        for t in range(16, 64):
            x, y = w[t - 15], w[t - 2]
            w.append((w[t - 16] + (rotr(x, 7) ^ rotr(x, 18) ^ x >> 3) + w[t - 7] + (rotr(y, 17) ^ rotr(y, 19) ^ y >> 10)) & 0xffffffff)
        a, b, c, d, e, f, g, hh = h
        for t in range(64):
            t1 = (hh + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + (e & f ^ ~e & g) + K[t] + w[t]) & 0xffffffff
            t2 = ((rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + (a & b ^ a & c ^ b & c)) & 0xffffffff
            a, b, c, d, e, f, g, hh = (t1 + t2) & 0xffffffff, a, b, c, (d + t1) & 0xffffffff, e, f, g
        h = [(x + y) & 0xffffffff for x, y in zip(h, [a, b, c, d, e, f, g, hh])]
    return ''.join(f'{x:08x}' for x in h)


if __name__ == '__main__':
    import hashlib, random, sys, time
    size = int(sys.argv[1]) if len(sys.argv) > 1 else 65536
    data = bytes(random.Random(140).randrange(256) for _ in range(size))
    assert sha256(data) == hashlib.sha256(data).hexdigest()
    runs = []
    for _ in range(7):
        start = time.perf_counter()
        sha256(data)
        runs.append((time.perf_counter() - start) * 1e9 / size)
    print('{"size":%d,"python_ns_per_byte":%.1f}' % (size, sorted(runs)[3]))
