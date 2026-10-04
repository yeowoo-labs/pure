#!/usr/bin/env python3
"""What a transient state saves (docs/design/018 stage 1, research 117). A
host of apps (one connection thread, two workers, cores 0-2) holds the
scoreboard (benchmarks/http/scores.aup) twice: stored, as it is, and
transient, its board a transient state. The load (benchmarks/http/load,
core 3) posts points for SECONDS on CONNECTIONS connections, keep-alive, to
one of them; the two in turns, their order turned each round. The stores
are in RAM (/dev/shm) or on the SD card (~/.cache). Each run says what the
host wrote to the device meanwhile (/proc/PID/io write_bytes).

transient.py PURE_SERVE ram|sd SECONDS ROUNDS   a JSON line per run, then medians
"""
import json
import shutil
import statistics
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
AUP = ROOT.parent / 'aup' / 'build' / 'aup'
LOAD = ROOT / 'benchmarks' / 'http' / 'load'


def written(pid):
    for line in Path(f'/proc/{pid}/io').read_text().splitlines():
        if line.startswith('write_bytes:'):
            return int(line.split()[1])


def main():
    serve, medium, seconds, rounds = Path(sys.argv[1]).resolve(), sys.argv[2], int(sys.argv[3]), int(sys.argv[4])
    base = Path('/dev/shm') if medium == 'ram' else Path.home() / '.cache'
    base.mkdir(parents=True, exist_ok=True)
    work = Path(tempfile.mkdtemp(prefix='pure-032-', dir=base))
    try:
        source = (ROOT / 'benchmarks' / 'http' / 'scores.aup').read_text()
        (work / 'transient.aup').write_text(source.replace('state board', 'transient state board'))
        for name, path in (('stored', ROOT / 'benchmarks' / 'http' / 'scores.aup'), ('transient', work / 'transient.aup')):
            subprocess.run([str(AUP), 'build', str(path), '-o', str(work / f'{name}.pmod')], check=True)
        (work / 'apps.conf').write_text(f"stored {work / 'stored.pmod'} {work / 'stored'}\n"
                                        f"transient {work / 'transient.pmod'} {work / 'transient'}\n")
        host = subprocess.Popen(['taskset', '-c', '0-2', str(serve), '--apps', str(work / 'apps.conf'), '--port', '0',
                                 '--threads', '1', '--workers', '2'], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        port = json.loads(host.stdout.readline())['port']
        rates, bytes_ = {}, {}
        for round_ in range(1, rounds + 1):
            ways = ['stored', 'transient']
            for way in (ways if round_ % 2 else ways[::-1]):
                before = written(host.pid)
                out = subprocess.run(['taskset', '-c', '3', str(LOAD), str(port), '16', str(seconds), 'score', '64',
                                      'keep-alive', '16', f'/{way}'], capture_output=True, text=True, check=True).stdout
                result = json.loads(out)
                assert result['2xx'] == result['completed'] and not result['errors'], result
                per = (written(host.pid) - before) / result['completed']
                rates.setdefault(way, []).append(result['requests_per_second'])
                bytes_.setdefault(way, []).append(per)
                print(json.dumps({'medium': medium, 'round': round_, 'way': way, 'written_per_request': round(per, 1),
                                  **result}), flush=True)
        host.terminate()
        host.communicate(timeout=60)
        stored, transient = statistics.median(rates['stored']), statistics.median(rates['transient'])
        print(json.dumps({'medium': medium, 'stored_median': stored, 'transient_median': transient,
                          'change': round((transient / stored - 1) * 100, 1),
                          'written_per_request': {w: round(statistics.median(bytes_[w]), 1) for w in bytes_}}), flush=True)
    finally:
        shutil.rmtree(work)


if __name__ == '__main__':
    main()
