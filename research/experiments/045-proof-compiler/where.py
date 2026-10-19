#!/usr/bin/env python3
"""Research 045: where.c's samples by mechanism. Each sampled address is given to addr2line (-i: the functions inlined
there, innermost first). A sample whose chain passes through the executor's step() is the instruction whose case
holds that line (core/module.c, the case labels read from the source); one outside it is its function, named by the
binary's symbols or, in a shared library, by the library's (the maps where.c writes). Instructions go to mechanisms
(OPS and FUNCTIONS below).

where.py BINARY SAMPLES MAPS SOURCE     tables: machine instructions a code point"""
import bisect
import collections
import re
import subprocess
import sys

# What each instruction's body does, for the mechanisms; an instruction not named is 'compute'.
OPS = {
    'dispatch': {'(fetch, decode)'},
    'moves': {'CONST', 'LOAD', 'STORE', 'LOCAL_LOAD', 'LOCAL_STORE', 'PICK', 'SLIDE'},
    'list read': {'ITEM_GET', 'INDEX_AT', 'LIST_COUNT', 'ROW', 'FIELD', 'LEN', 'LIST_FIND', 'LIST_HAS', 'OUTER_ROW',
                  'KEPT_GET', 'INDEX_LOAD', 'INDEX_NUM', 'INDEX_FIRST', 'INDEX_LAST'},
    'list build': {'LIST_APPEND', 'LIST_VALUE', 'LIST_BIND', 'APPEND_IN', 'LIST_INSERT', 'LIST_REMOVE', 'ITEM_SET',
                   'KEPT_SET', 'KEPT_APPEND', 'KEPT_INSERT', 'KEPT_APPEND_IN', 'BYTES_OF', 'BYTES_FROM'},
    'call': {'CALL', 'RETURN'},
    'loop': {'LOOP', 'BREAK', 'EACH', 'FOLD'},
}
# Functions outside step() (not inlined into it), by mechanism; matched on the name before any '.' suffix.
FUNCTIONS = {
    'list read': {'pure_list_get', 'leaf_of', 'child', 'child_at', 'cell_in', 'array_cell', 'pure_list_count',
                  'pure_states_has', 'cell_out', 'width_of'},
    'list build': {'pure_list_insert', 'flat_insert', 'pure_list_new', 'pure_list_clear', 'arrays_free', 'arrays_fit',
                   'pure_list_changes', 'pure_list_settle', 'pure_list_free', 'clear_rows', 'pure_list_copied',
                   'pure_list_append', 'pure_list_set'},
    'call': {'enter_call', 'leave_call', 'states_next', 'restore_lists', 'give_back', 'unbind', 'kept_apart',
             'frame_room', 'unwind_calls', 'list_held'},
    'loop': {'iterate', 'charge', 'enter_loop', 'enter_each', 'leave_loop'},
    'event': {'message', 'close_burst', 'mark_event', 'pure_app_message', 'open_frame', 'finish', 'run', 'open_burst',
              'reserve', 'nest', 'timer_settime'},
    'compute': {'floor_divide', 'floor', 'fmod', 'fmodf64', 'copysign'},
    'memory': {'malloc', 'free', 'calloc', 'realloc', 'memset', 'memcpy', 'memmove', '_int_malloc', '_int_free',
               'cfree', 'malloc_consolidate', '__aarch64_ldadd8_relax', '__aarch64_cas8_acq_rel',
               '__xpg_strerror_r', '__default_morecore'},  # nm's nearest names for malloc's own (static) functions in libc
}


def cases(source):
    """Line ranges of step()'s cases: [(first line, instruction)], sorted; lines before the first case are dispatch."""
    lines = open(source).read().splitlines()
    start = next(i for i, l in enumerate(lines) if re.match(r'static .*\bint step\(Frame \*frame', l)) + 1
    end = next(i for i in range(start, len(lines)) if lines[i] == '}') + 1
    marks = [(start, '(fetch, decode)')]
    second = False
    for n in range(start, end + 1):
        l = lines[n - 1]
        found = re.findall(r'case PURE_OP_(\w+)', l)
        if found:
            marks.append((n, found[0]))
        elif re.match(r'\s+default: break;', l):
            second = True
        elif second and re.match(r'\s+double b = stack\[--sp\]\.num', l):
            marks.append((n, 'ARITH'))  # the second switch: the numbers' binary instructions
            second = False
    return start, end, marks


def resolve(binary, addresses, maps):
    """{address: [(function, file, line)] innermost first}."""
    regions = []
    for l in open(maps):
        part = l.split()
        if len(part) >= 6 and 'x' in part[1] and part[5].startswith('/'):
            lo, hi = (int(x, 16) for x in part[0].split('-'))
            regions.append((lo, hi, int(part[2], 16), part[5]))
    out = subprocess.run(['addr2line', '-a', '-f', '-i', '-e', binary], input='\n'.join(addresses) + '\n',
                         capture_output=True, text=True, check=True).stdout.splitlines()
    result, current = {}, None
    for line in out:
        if re.fullmatch(r'0x[0-9a-f]+', line):
            current = int(line, 16)
            result[current] = []
        else:
            result[current].append(line)
    chain = {}
    symbols = {}
    for a, l in result.items():
        pairs = [(l[i].split('.')[0], *(l[i + 1].split(' ')[0].rsplit(':', 1) + ['0'])[:2]) for i in range(0, len(l) - 1, 2)]
        if pairs and pairs[0][0] != '??':
            chain[a] = [(f, p.split('/')[-1], int(n) if n.isdigit() else 0) for f, p, n in pairs]
            continue
        region = next((r for r in regions if r[0] <= a < r[1]), None)
        if not region:
            chain[a] = [('??', '??', 0)]
            continue
        lo, _, offset, path = region
        if path not in symbols:
            listed = subprocess.run(['nm', '-D', '--defined-only', path], capture_output=True, text=True).stdout.split('\n')
            table = sorted((int(p[0], 16), p[2]) for p in (x.split() for x in listed) if len(p) == 3 and p[1] in 'TtWi')
            symbols[path] = ([t[0] for t in table], [t[1] for t in table])
        at, names = symbols[path]
        k = bisect.bisect_right(at, a - lo + offset) - 1
        chain[a] = [(names[k].split('@')[0] if k >= 0 else '??', path.split('/')[-1], 0)]
    return chain


def main():
    binary, samples, maps, source = sys.argv[1:5]
    with open(samples) as f:
        head = f.readline().split()
        total, points = int(head[1]), int(head[3])
        counts = {int(a, 16): int(n) for a, n in (l.split() for l in f)}
    start, end, marks = cases(source)
    firsts = [m[0] for m in marks]
    chains = resolve(binary, [hex(a) for a in counts], maps)
    n = sum(counts.values())
    scale = total / points / n
    by_op, by_function, mechanism, rest = (collections.Counter() for _ in range(4))
    for a, c in counts.items():
        chain = chains[a]
        at = next((x for x in chain if x[0] == 'step' and x[1] == 'module.c' and start <= x[2] <= end), None)
        if at:
            op = marks[bisect.bisect_right(firsts, at[2]) - 1][1]
            by_op[op] += c
            mechanism[next((k for k, v in OPS.items() if op in v), 'compute')] += c
            continue
        name = chain[-1][0]  # the function as linked: what an outside call shows
        inner = chain[0][0]
        by_function[name] += c
        kind = 'dispatch' if name == 'run_calls' else next((k for k, v in FUNCTIONS.items() if inner in v or name in v), None)
        mechanism[kind or 'unknown'] += c
        if not kind:
            rest[f'{inner} ({chain[0][1]}:{chain[0][2]}) < {name}'] += c
    print(f'{total / points:,.0f} machine instructions a code point, {n:,} samples')
    print('\nby mechanism:')
    for k, c in mechanism.most_common():
        print(f'  {c * scale:8,.0f}  {100 * c / n:5.1f}%  {k}')
    print('\nin step(), by the case its line is in (code the compiler shares between cases goes to one of them):')
    for op, c in by_op.most_common(40):
        print(f'  {c * scale:8,.0f}  {op}')
    print('\noutside step(), by function as linked:')
    for name, c in by_function.most_common(30):
        print(f'  {c * scale:8,.0f}  {name}')
    if rest:
        print('\nno mechanism:')
        for name, c in rest.most_common(20):
            print(f'  {c * scale:8,.0f}  {name}')


if __name__ == '__main__':
    main()
