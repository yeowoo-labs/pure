#!/usr/bin/env python3
"""docs/research/107's numbers from the result files beside this one: the
allocators, before and after in turns (six pairs: runs.sh made the first
three, before first; three more followed, after first), and 106's RAM
matrix again (with 022-tpcc-grouped/tables.py)."""
import importlib.util
import json
from pathlib import Path

HERE = Path(__file__).resolve().parent


def pure_run(name):
    run = end = None
    for line in (HERE / f'2026-09-30-{name}-linux-aarch64.jsonl').read_text().splitlines():
        r = json.loads(line)
        run = r if r['step'] == 'run' else run
        end = r if r['step'] == 'end' else end
    return run, end


def row(label, name):
    run, end = pure_run(name)
    ratio = end['pure_resident_mib'] / end['pure_state_mib']
    print(f"| {label} | {run['transactions_per_second']:,} | {run['cpu_us_per_transaction']} | "
          f"{run['core_shares']['pure-owner']:.2f} | {end['pure_resident_mib']:.0f} / {end['pure_state_mib']:.0f} MiB "
          f"({ratio:.2f}) |")
    return run['transactions_per_second']


print('| | 초당 거래 | 거래당 CPU (µs) | 소유자의 코어 몫 | 상주 / 상태 (배수) |')
print('|---|---:|---:|---:|---:|')
for name, label in (('allocator-glibc-1', 'glibc'), ('allocator-jemalloc-1', 'jemalloc'),
                    ('allocator-tcache-1', 'glibc, tcache_max=4096'), ('allocator-glibc-2', 'glibc'),
                    ('allocator-jemalloc-2', 'jemalloc')):
    row(label, name)
print()
print('| | 초당 거래 | 거래당 CPU (µs) | 소유자의 코어 몫 | 상주 / 상태 (배수) |')
print('|---|---:|---:|---:|---:|')
for round in range(1, 7):  # 1-3 before first, 4-6 after first
    before = row(f'전, {round}', f'before-{round}')
    after = row(f'후, {round}', f'after-{round}')
    print(f'| ({(after / before - 1) * 100:+.1f}%) | | | | |')
print()
spec = importlib.util.spec_from_file_location('tables', HERE.parent / '022-tpcc-grouped' / 'tables.py')
tables = importlib.util.module_from_spec(spec)
spec.loader.exec_module(tables)
tables.OUT = HERE
for name in ('ram-w1', 'ram-w4'):
    tables.table(name, False)
