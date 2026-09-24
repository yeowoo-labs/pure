#!/usr/bin/env python3
"""Count real idle poll calls before/after deadline-driven HTTP sleeping."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import select
import signal
import subprocess
import tarfile
import tempfile
import time


def observe(binary, seconds):
    with tempfile.TemporaryDirectory(prefix='pure-idle-poll-') as name:
        root = Path(name)
        trace = root / 'poll.log'
        tracer = subprocess.Popen(['strace', '-qq', '-e', 'trace=poll,ppoll', '-o',
                                   str(trace), str(binary), '--store',
                                   str(root / 'store'), '--port', '0'],
                                  stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                  start_new_session=True)
        try:
            ready, _, _ = select.select([tracer.stdout], [], [], 10)
            assert ready, 'HTTP host did not become ready under strace'
            message = json.loads(tracer.stdout.readline())
            assert message['port'] > 0
            children = Path(f'/proc/{tracer.pid}/task/{tracer.pid}/children')
            pids = children.read_text().split()
            assert len(pids) == 1, pids
            time.sleep(seconds)
            os.kill(int(pids[0]), signal.SIGTERM)
            _, error = tracer.communicate(timeout=10)
            assert tracer.returncode == 0, (tracer.returncode, error[-1000:])
            captured = trace.read_text()
            calls = [line for line in captured.splitlines()
                     if line.startswith(('poll(', 'ppoll('))]
            values = []
            for line in calls:
                if line.startswith('ppoll('):
                    if re.search(r'\],\s*\d+,\s*NULL,', line):
                        values.append(-1)
                    elif match := re.search(r'\{tv_sec=(\d+), tv_nsec=(\d+)\}', line):
                        values.append(int(match.group(1)) * 1000 +
                                      int(match.group(2)) // 1_000_000)
                elif match := re.search(r',\s*(-?\d+)\)\s+=', line):
                    values.append(int(match.group(1)))
            return {'poll_calls': len(calls), 'timeout_values_ms': sorted(set(values)),
                    'trace_excerpt': captured[:500],
                    'binary_sha256': hashlib.sha256(binary.read_bytes()).hexdigest()}
        finally:
            if tracer.poll() is None:
                os.killpg(tracer.pid, signal.SIGTERM)
                try:
                    tracer.communicate(timeout=5)
                except subprocess.TimeoutExpired:
                    os.killpg(tracer.pid, signal.SIGKILL)
                    tracer.communicate(timeout=5)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--before', type=Path, required=True)
    parser.add_argument('--after', type=Path, required=True)
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    seconds = 1.25
    with tempfile.TemporaryDirectory(prefix='pure-idle-before-') as directory:
        baseline = args.before.resolve()
        if baseline.name.endswith('.tar.gz'):
            with tarfile.open(baseline, 'r:gz') as archive:
                body = archive.extractfile('pure-document/bin/pure-connections-fixed').read()
            baseline = Path(directory) / 'pure-connections-fixed'
            baseline.write_bytes(body)
            baseline.chmod(0o755)
        before = observe(baseline, seconds)
    after = observe(args.after.resolve(), seconds)
    assert before['poll_calls'] >= 10, before
    assert after['poll_calls'] <= 2, after
    assert -1 in after['timeout_values_ms'], after
    result = {'profile': 'idle-http-host-poll-syscalls',
              'observation_seconds': seconds, 'before': before, 'after': after,
              'deadline_driven_idle_wait': 'pass'}
    body = json.dumps(result, indent=2) + '\n'
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(body)
    print(body, end='')


if __name__ == '__main__':
    main()
