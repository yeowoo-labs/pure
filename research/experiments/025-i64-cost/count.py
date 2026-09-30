#!/usr/bin/env python3
"""User-mode instructions and cycles per ask of queries over the bank
(benchmarks/bank, 100,000 accounts), on fresh pure-serves: the bank's four
queries and three tables over its 20,000 transfers (none kept, one field a
row, a row's account found by key). Each server is loaded alike, warmed,
then counted over ASKS asks per query with pcount (perf_event_open, every
thread of the server). Instructions are what a change adds or takes away;
cycles move with where code and data land, too.

    count.py [--profile QUERY] serve [serve ...]

With --profile, psample samples QUERY's instruction pointers (one per
20,011 instructions) and the shares of the functions are printed instead.
"""
import argparse, bisect, collections, http.client, json, random, select, shutil, subprocess, sys, tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
sys.path.insert(0, str(ROOT / 'benchmarks' / 'bank'))
import queries as bank  # noqa: E402

TABLES = {
    'none': 'view none = each h in history where h.delta > 5000 {\n  aid = h.aid\n}\n',
    'field': 'view field = each h in history where h.delta > 0 {\n  aid = h.aid\n}\n',
    'found': 'view found = each h in history where h.delta > 0 {\n  account = accounts[h.aid].balance\n}\n',
}
ASKS = 60


def load(port):
    rng = random.Random(9)
    transfers = [(rng.randint(1, 100000), rng.randint(1, 100), rng.randint(1, 10), rng.randint(-5000, 5000))
                 for _ in range(20000)]
    bank.post_all(port, '/inputs/branch', [json.dumps({'bid': b}).encode() for b in range(1, 11)], 1)
    bank.post_all(port, '/inputs/teller', [json.dumps({'tid': t, 'bid': (t - 1) // 10 + 1}).encode() for t in range(1, 101)], 1)
    bank.post_all(port, '/inputs/account', [json.dumps({'aid': a, 'bid': (a - 1) % 10 + 1}).encode()
                                            for a in range(1, 100001)])
    bank.post_all(port, '/inputs/transfer', [json.dumps({'aid': a, 'tid': t, 'bid': b, 'delta': d}).encode()
                                             for a, t, b, d in transfers], 1)


def functions(binary, maps, samples):
    """Samples by function: the binary's by its symbols, others by their file."""
    listed = subprocess.run(['nm', '-n', '--defined-only', binary], capture_output=True, text=True, check=True).stdout
    starts, names = [], []
    for line in listed.splitlines():
        parts = line.split()
        if len(parts) == 3 and parts[1] in 'tTwW':
            starts.append(int(parts[0], 16))
            names.append(parts[2])
    ranges = []
    for line in maps.splitlines():
        parts = line.split()
        if len(parts) >= 6 and 'x' in parts[1]:
            low, high = (int(x, 16) for x in parts[0].split('-'))
            ranges.append((low, high, int(parts[2], 16), parts[5]))
    counts = collections.Counter()
    for ip in samples:
        for low, high, offset, path in ranges:
            if low <= ip < high:
                if path == binary:
                    i = bisect.bisect_right(starts, ip - low + offset) - 1
                    counts[names[i] if i >= 0 else '?'] += 1
                else:
                    counts[Path(path).name] += 1
                break
        else:
            counts['[other]'] += 1
    return counts


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--aup', type=Path, default=ROOT.parent / 'aup' / 'build' / 'aup')
    parser.add_argument('--profile', help='a query to sample instead of counting')
    parser.add_argument('--asks', type=int, default=ASKS)
    parser.add_argument('serves', nargs='+', type=Path)
    args = parser.parse_args()
    views = {name: view for name, (view, _) in bank.QUERIES.items()} | TABLES
    if args.profile:
        views = {args.profile: views[args.profile]}
    work = Path(tempfile.mkdtemp(prefix='pure-count-', dir='/dev/shm'))
    try:
        for tool in ('pcount', 'psample'):
            subprocess.run(['cc', '-O2', '-Wall', '-o', str(work / tool), str(HERE / f'{tool}.c')], check=True)
        module = work / 'bank.pmod'
        subprocess.run([str(args.aup), 'build', str(bank.HERE / 'bank.aup'), '-o', str(module)], check=True)
        compiled = {}
        for name, view in views.items():
            (work / f'{name}.aup').write_text(view)
            subprocess.run([str(args.aup), 'query', str(bank.HERE / 'bank.aup'), str(work / f'{name}.aup'), '-o',
                            str(work / f'{name}.pmod')], check=True)
            compiled[name] = (work / f'{name}.pmod').read_bytes()
        profiles = {}
        for k, serve in enumerate(args.serves):
            serve = serve.resolve()
            process = subprocess.Popen(['taskset', '-c', '0-2', str(serve), '--module', str(module), '--store',
                                        str(work / f'store-{k}'), '--port', '0', '--timeout-ms', '600000'],
                                       stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            try:
                select.select([process.stdout], [], [], 10)
                port = json.loads(process.stdout.readline())['port']
                load(port)
                client = http.client.HTTPConnection('127.0.0.1', port, timeout=120)

                def ask(method, path, body=None):
                    client.request(method, path, body)
                    answer = client.getresponse()
                    data = answer.read()
                    assert answer.status == 200, (path, answer.status)
                    return data

                for name, bytes_ in compiled.items():
                    ask('PUT', f'/queries/{name}', bytes_)
                    for _ in range(5):
                        ask('GET', f'/queries/{name}')
                if args.profile:
                    maps = Path(f'/proc/{process.pid}/maps').read_text()
                    sampler = subprocess.Popen([str(work / 'psample'), str(process.pid), '20011'], stdin=subprocess.PIPE,
                                               stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
                    assert sampler.stderr.readline().strip() == 'ready'
                    for _ in range(args.asks):
                        ask('GET', f'/queries/{args.profile}')
                    out, _ = sampler.communicate('')
                    samples = [int(line, 16) for line in out.splitlines() if not line.startswith('lost')]
                    profiles[serve.name] = (len(samples), functions(str(serve), maps, samples))
                else:
                    result = {'serve': serve.name}
                    for name in compiled:
                        counter = subprocess.Popen([str(work / 'pcount'), str(process.pid)], stdin=subprocess.PIPE,
                                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
                        assert counter.stderr.readline().strip() == 'ready'
                        for _ in range(args.asks):
                            ask('GET', f'/queries/{name}')
                        counts = json.loads(counter.communicate('')[0])
                        result[name] = {'instructions': round(counts['instructions'] / args.asks),
                                        'cycles': round(counts['cycles'] / args.asks)}
                    print(json.dumps(result), flush=True)
                client.close()
            finally:
                process.terminate()
                process.communicate(timeout=60)
        if profiles:
            labels = list(profiles)
            shares = collections.Counter()
            for total, counts in profiles.values():
                for name, count in counts.items():
                    shares[name] += count / total
            print('%-32s' % 'function' + ''.join('%16s' % label[-15:] for label in labels))
            for name, _ in shares.most_common(24):
                print('%-32s' % name[:32] + ''.join('%15.2f%%' % (100 * profiles[label][1][name] / profiles[label][0])
                                                      for label in labels))
    finally:
        shutil.rmtree(work)


if __name__ == '__main__':
    main()
