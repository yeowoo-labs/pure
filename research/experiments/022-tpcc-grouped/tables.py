#!/usr/bin/env python3
"""docs/research/106's tables from the result files beside this one (the names given, as globs: ram-w1, sd-w4-*):
a row a system and terminal count; on the SD card the median of the rounds and their range."""
import json
import statistics
import sys
from pathlib import Path

OUT = Path(__file__).resolve().parent
KINDS = ('new_order', 'payment', 'order_status', 'delivery', 'stock_level')


def runs(name):
    rows = {}
    ends, checks = [], []
    for path in sorted(OUT.glob(f'2026-09-30-{name}-linux-aarch64.jsonl')):
        for line in path.read_text().splitlines():
            r = json.loads(line)
            if r['step'] == 'run':
                system = 'Pure' if r['system'] == 'pure' else 'SQLite 묶음' if 'grouped' in r['system'] else 'SQLite'
                rows.setdefault((system, r['terminals']), []).append(r)
            elif r['step'] == 'end':
                ends.append(r)
            elif r['step'] == 'same transactions':
                checks.append(r)
    return rows, ends, checks


def middle(values, digits=0, spread=True):
    values = [v for v in values if v is not None]
    if not values:
        return '-'
    m = statistics.median(values)
    text = f'{m:,.{digits}f}'
    if spread and len(values) > 1:
        text += f' ({min(values):,.{digits}f}–{max(values):,.{digits}f})'
    return text


def table(name, device):
    rows, ends, checks = runs(name)
    order = [('Pure', 16), ('Pure', 64), ('SQLite', 1), ('SQLite', 4), ('SQLite 묶음', 16), ('SQLite 묶음', 64)]
    print(f'### {name}')
    for c in checks:
        print(f"check: failed {c['failed']} differing {c['differing']} grouped {c['grouped_sqlite_differing']} "
              f"consistency {c['consistency_failures']}")
    print('| | 초당 거래 | New-Order p50 / p99 (ms) | 거래당 CPU (µs) | 소유자의 코어 몫 | 메모리 (MiB) | 거래당 장치 바이트 | 묶음 크기 |')
    print('|---|---:|---:|---:|---:|---:|---:|---:|')
    for key in order:
        rs = rows.get(key, [])
        if not rs:
            continue
        failures = sum(len(r['consistency_failures']) for r in rs) + sum(r[k]['failed'] for r in rs for k in KINDS)
        tps = middle([r['transactions_per_second'] for r in rs])
        p50 = middle([r['new_order']['p50_us'] / 1000 for r in rs], 1, False)
        p99 = middle([r['new_order']['p99_us'] / 1000 for r in rs], 1, False)
        cpu = middle([r['cpu_us_per_transaction'] for r in rs], 1, False)
        owner = middle([r.get('core_shares', {}).get('pure-owner') for r in rs], 2, False)
        memory = middle([r.get('resident_mib', r.get('peak_resident_mib')) for r in rs], 0, False)
        dev = middle([r['device_bytes_per_transaction'] for r in rs], 0, False) if device else '-'
        batch = middle([r['batched'] / r['batches'] for r in rs if r.get('batches')], 1, False)
        flag = '' if not failures else f' **{failures} failures**'
        print(f'| {key[0]}, {key[1]}{"터미널" if key[0] != "SQLite" else "스레드"} | {tps} | {p50} / {p99} | {cpu} | '
              f'{owner} | {memory} | {dev} | {batch} |{flag}')
    for e in ends:
        print(f"end: pure {e['pure_resident_mib']} MiB resident, {e['pure_state_mib']} MiB states "
              f"(x{e['pure_resident_mib'] / max(e['pure_state_mib'], 0.1):.2f}), store {e['pure_store_mib']} MiB, "
              f"sqlite {e['sqlite_files_mib']} / {e['grouped_sqlite_files_mib']} MiB, {e['pure_counts']}")
    print()


for name in sys.argv[1:] or ['ram-w1', 'ram-w4', 'sd-w1-*', 'sd-w4-*']:
    table(name, name.startswith('sd'))
