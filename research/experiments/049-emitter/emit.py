#!/usr/bin/env python3
"""Research 049 (030's step 3, its bar): a handler's verified code emitted as C, the exact rung. One pass over the code
as the load leaves it (dump.c), the operand stack's height at each instruction the verifier's: a value at height h is
the C variable s<h>, a local slot L<n>; the instructions the stage covers are written out as the executor's steps
compute them (the same checks, faults and work, the runtime's lists and bytes); any other is the executor's own step
(pure_run_step), the stack written out before it and read back after; an instruction neither covers nor can hand on
(a walk, a fold, a call) refuses the handler. The C is built with core/module.c (PURE_EMITTED: run() gives this block
to emitted_run, and the event around it is the executor's).

  emit.py DUMP.json HANDLER OUT.c      prints what it covered (JSON) and writes OUT.c"""
import json
import re
import sys
from pathlib import Path

PURE = Path(__file__).resolve().parents[3]


def opcode_names():
    """The opcodes' names in their order: the module's (module.h), then the runtime's own (module-run.h)."""
    module = (PURE / 'core' / 'module.h').read_text()
    body = module[module.index('typedef enum {'):module.index('} PureOpcode;')]
    names = [n for n in re.findall(r'PURE_OP_([A-Z0-9_]+)', body) if n != 'COUNT']
    run = (PURE / 'core' / 'module-run.h').read_text()
    body = run[run.index('PURE_OP_CUR_LOAD = PURE_OP_COUNT'):run.index('PURE_OP_STEPS')]
    return names + [n for n in re.findall(r'PURE_OP_([A-Z0-9_]+)', body) if n != 'COUNT']


NAMES = opcode_names()
NUMBER_OPS = {'ADD': 'a + b', 'SUB': 'a - b', 'MUL': 'a * b', 'LT': 'a < b', 'LE': 'a <= b', 'GT': 'a > b', 'GE': 'a >= b',
              'EQ': 'a == b', 'NE': 'a != b', 'AND': 'a != 0 && b != 0', 'OR': 'a != 0 || b != 0', 'MIN': 'minimum(a, b)',
              'MAX': 'maximum(a, b)'}
REFUSED = {'EACH', 'FOLD', 'CALL', 'RETURN', 'LIST_BIND', 'ROW', 'OUTER_ROW'}  # they need the executor's frame around them
OPAQUE = bool(__import__('os').environ.get('EMIT_OPAQUE'))  # EMIT_OPAQUE=1: constants hidden from cc (no folding)


def literal(bits):
    value = int(bits)
    return '(-INT64_C(9223372036854775807) - 1)' if value == -2**63 else f'INT64_C({value})'


class Emitter:
    def __init__(self, dump, handler):
        self.dump = dump
        self.block = dump['handlers'][handler]
        self.start, self.end = self.block['start'], self.block['start'] + self.block['length']
        self.code = dump['code']
        self.lines, self.labels, self.locals = [], set(), set()
        self.covered, self.handed = {}, {}
        self.depth = max(self.height(pc) for pc in range(self.start, self.end)) + 2
        for pc in range(self.start, self.end):
            name = self.name(pc)
            if name in REFUSED:
                raise SystemExit(f'refused: {name} at {pc}')
            if name in ('JZ', 'JMP'):
                self.labels.add(self.arg(pc))

    def name(self, pc): return NAMES[self.code[pc][0]]
    def arg(self, pc): return self.code[pc][1]
    def plan(self, pc): return self.code[pc][2]
    def height(self, pc): return self.code[pc][4] if pc < len(self.code) and pc < self.end else 0

    def out(self, text, indent):
        self.lines.append('    ' * indent + text)

    def fault(self, pc, kind):
        return f'{{ frame.kind = PURE_FAULT_{kind}; frame.fault_pc = {pc}; how = STEP_FAULT; goto done; }}'

    def stopped(self, pc, why='status'):
        return f'{{ frame.status = {why}; frame.fault_pc = {pc}; how = STEP_RETURN; goto done; }}'

    def refused(self, pc):
        return f'{{ frame.status = status; frame.fault_pc = {pc}; how = STEP_REFUSED; goto done; }}'

    def jump(self, target, loops):
        """Where a branch to target goes: the end of the innermost loop's block is its round's end."""
        if loops:
            pc, end = loops[-1]
            if target == end:
                return f'goto round_{pc};'
            assert loops[-1][0] < target < end, (target, loops[-1])
        else:
            assert self.start <= target <= self.end, target
            if target == self.end:
                return 'goto ran;'
        return f'goto at_{target};'

    def emit(self, lo, hi, loops, indent):
        pc = lo
        while pc < hi:
            if pc in self.labels:
                self.out(f'at_{pc}:;', indent - 1)
            name, arg, h = self.name(pc), self.arg(pc), self.height(pc)
            s = lambda k: f's{h - k}'  # s(1) the top, s(2) the one below
            self.out(f'/* {pc}: {name} {arg} */', indent)
            native = True
            if name == 'CONST':
                self.out(f'{s(0)}.i64 = {literal(self.code[pc][3])};', indent)
                if OPAQUE:  # the constant hidden from cc: what an emitter that folds nothing knows at the next instruction
                    self.out(f'__asm__("" : "+r"({s(0)}.i64));', indent)
            elif name == 'LOAD':
                self.out(f'{s(0)}.i64 = values[{arg}].i64;', indent)
            elif name in ('STORE', 'CUR_STORE'):
                self.out(f'values[{arg}].i64 = {s(1)}.i64;', indent)
            elif name == 'LOCAL_LOAD':
                self.locals.add(arg & 0xFF)
                self.out(f'{s(0)}.i64 = L{arg & 0xFF}.i64; {s(0)}.bytes.owner = (uint16_t)({arg} >> 16);', indent)
            elif name == 'LOCAL_STORE':
                self.locals.add(arg & 0xFF)
                self.out(f'L{arg & 0xFF}.i64 = {s(1)}.i64;', indent)
            elif name == 'PICK':
                self.out(f'{s(0)} = {s(1 + arg)};', indent)
            elif name == 'SLIDE':
                self.out(f'{s(1 + arg)} = {s(1)};', indent)
            elif name == 'NOT':
                self.out(f'{s(1)}.num = {s(1)}.num == 0;', indent)
            elif name == 'NEG':
                self.out(f'{s(1)}.num = -{s(1)}.num;', indent)
            elif name in ('LEN', 'TEXT_BYTES'):
                self.out(f'{s(1)}.num = (double){s(1)}.bytes.length;', indent)
            elif name in NUMBER_OPS or name in ('DIV', 'MOD', 'FLOORDIV'):
                self.out('{', indent)
                self.out(f'double b = {s(1)}.num, a = {s(2)}.num, value;', indent + 1)
                if name in NUMBER_OPS:
                    self.out(f'value = {NUMBER_OPS[name]};', indent + 1)
                else:
                    self.out(f'if (b == 0) {self.fault(pc, "DIVIDE")}', indent + 1)
                    self.out('value = ' + {'DIV': 'a / b', 'MOD': 'modulo(a, b)', 'FLOORDIV': 'floor_quotient(a, b)'}[name] + ';',
                             indent + 1)
                self.out(f'if (!isfinite(value)) {self.fault(pc, "NON_FINITE")}', indent + 1)
                self.out(f'{s(2)}.num = value;', indent + 1)
                self.out('}', indent)
            elif name in ('I64_ADD', 'I64_SUB', 'I64_MUL'):
                builtin = {'I64_ADD': 'add', 'I64_SUB': 'sub', 'I64_MUL': 'mul'}[name]
                self.out(f'if (__builtin_{builtin}_overflow({s(2)}.i64, {s(1)}.i64, &{s(2)}.i64)) {self.fault(pc, "OVERFLOW")}', indent)
            elif name in ('I64_DIV', 'I64_MOD'):
                div = name == 'I64_DIV'
                self.out('{', indent)
                self.out(f'int64_t b = {s(1)}.i64, a = {s(2)}.i64;', indent + 1)
                self.out(f'if (b == 0) {self.fault(pc, "DIVIDE")}', indent + 1)
                if div:
                    self.out(f'if (b == -1 && a == INT64_MIN) {self.fault(pc, "OVERFLOW")}', indent + 1)
                self.out(f'if (b == -1) {s(2)}.i64 = {"-a" if div else "0"};', indent + 1)
                self.out('else {', indent + 1)
                self.out('int64_t q = a / b, r = a % b;', indent + 2)
                self.out('if (r != 0 && (r < 0) != (b < 0)) { --q; r += b; }', indent + 2)
                self.out(f'{s(2)}.i64 = {"q" if div else "r"};', indent + 2)
                self.out('}', indent + 1)
                self.out('}', indent)
            elif name == 'I64_BITS':
                self.out('{', indent)
                self.out(f'int64_t b = {s(1)}.i64, a = {s(2)}.i64, r;', indent + 1)
                if arg == 0:
                    self.out('r = a & b;', indent + 1)
                elif arg == 1:
                    self.out('r = a | b;', indent + 1)
                elif arg == 2:
                    self.out('r = a ^ b;', indent + 1)
                else:
                    self.out(f'if (b < 0 || b > 63) {self.fault(pc, "LIMIT")}', indent + 1)
                    if arg == 4:
                        self.out('r = floor_shift(a, (unsigned)b);', indent + 1)
                    else:
                        self.out(f'if (floor_shift(r = (int64_t)((uint64_t)a << b), (unsigned)b) != a) {self.fault(pc, "OVERFLOW")}',
                                 indent + 1)
                self.out(f'{s(2)}.i64 = r;', indent + 1)
                self.out('}', indent)
            elif name == 'I64_FROM':  # pure_cell_as's NUMBER to INTEGER: a whole number in range (-2^63 is, 2^63 is not)
                self.out('{', indent)
                self.out(f'double v = {s(1)}.num;', indent + 1)
                self.out(f'if (!(v >= -9223372036854775808.0 && v < 9223372036854775808.0) || v != floor(v)) {self.fault(pc, "LIMIT")}',
                         indent + 1)
                self.out(f'{s(1)}.i64 = (int64_t)v;', indent + 1)
                self.out('}', indent)
            elif name == 'NUM_FROM':  # INTEGER to NUMBER: within 2^53
                self.out(f'if ({s(1)}.i64 > INT64_C(9007199254740992) || {s(1)}.i64 < -INT64_C(9007199254740992)) {self.fault(pc, "LIMIT")}',
                         indent)
                self.out(f'{s(1)}.num = (double){s(1)}.i64;', indent)
            elif name == 'INDEX_AT':
                self.out(f'if ({s(1)}.num != floor({s(1)}.num)) {self.fault(pc, "LIMIT")}', indent)
                self.out(f'if ({s(1)}.num < 0 || {s(1)}.num >= (double)pure_list_count(app->lists[{arg}])) {self.fault(pc, "NOT_FOUND")}',
                         indent)
                self.out(f'{s(1)}.bytes.owner = (uint16_t){arg};', indent)
            elif name == 'ITEM_GET' and not self.plan(pc) & 3:  # a number's, a bool's or an i64's cell (no text, no list)
                lst, field = arg & 0xFFFF, arg >> 16
                self.out('{', indent)
                self.out(f'size_t at = (size_t){s(1)}.num;', indent + 1)
                self.out(f'if (at >= pure_list_count(app->lists[{lst}])) {self.fault(pc, "NOT_FOUND")}', indent + 1)
                self.out(f'{s(1)}.i64 = pure_list_get(app->lists[{lst}], at, {field}).i64;', indent + 1)
                self.out('}', indent)
            elif name == 'ITEM_SET' and not self.plan(pc) & 1:  # not a text's cell: a number's, a bool's or an i64's
                lst, field = arg & 0xFFFF, arg >> 16
                self.out('{', indent)
                self.out(f'PureList *list = app->lists[{lst}];', indent + 1)
                self.out(f'if (m->record_field_type[m->record[m->state_ref[{lst}]].first + {field}] == PURE_RECORD_LIST) abort(); /* emit.py: a list field is not covered */',
                         indent + 1)
                self.out(f'size_t at = (size_t){s(2)}.num;', indent + 1)
                self.out('Held before = list_held(app, list);', indent + 1)
                self.out(f'PureCell cell = {{.i64 = {s(1)}.i64}};', indent + 1)
                setter = 'pure_list_set_row' if self.plan(pc) & 4 else 'pure_list_set'
                self.out(f'if ((status = {setter}(list, at, {field}, cell, undo_for(app, {lst}))) != PURE_OK) {self.refused(pc)}',
                         indent + 1)
                self.out(f'if (!grows(app, before, list_held(app, list))) {self.fault(pc, "MEMORY")}', indent + 1)
                self.out('}', indent)
            elif name == 'BYTES_LOAD':
                held = f'app->held[{arg & 0xFF}]'
                self.out(f'{s(0)}.bytes = {held}.by_ref ? {held}.ref : (PureBytes){{FROM_HELD, (uint16_t){arg & 0xFF}, {{0, 0}}, 0, {held}.size}};',
                         indent)
            elif name == 'BYTE_AT':
                self.out('{', indent)
                self.out(f'Slot at = {s(1)};', indent + 1)
                self.out(f'PureBytes v = {s(2)}.bytes;', indent + 1)
                test = ('at.i64 < 0 || (uint64_t)at.i64 >= v.length' if arg else
                        '!(at.num >= 0 && at.num < (double)v.length) || at.num != floor(at.num)')
                self.out(f'if ({test}) {self.fault(pc, "LIMIT")}', indent + 1)
                self.out('unsigned char byte = 0;', indent + 1)
                self.out(f'v.offset += {"(uint64_t)at.i64" if arg else "(uint64_t)at.num"};', indent + 1)
                self.out('v.length = 1;', indent + 1)
                self.out(f'if ((status = visit_value(app, arena, v, take_byte, &byte)) != PURE_OK) {self.stopped(pc)}', indent + 1)
                self.out(f'{s(2)}.i64 = byte;', indent + 1)
                self.out('}', indent)
            elif name == 'JZ':
                self.out(f'if ({s(1)}.num == 0) {self.jump(arg, loops)}', indent)
            elif name == 'JMP':
                self.out(self.jump(arg, loops), indent)
            elif name == 'BREAK':
                assert loops, pc
                self.out(f'arena->used = mark_{loops[-1][0]}; goto broke_{loops[-1][0]};', indent)
            elif name == 'LOOP':
                length, slot, integer = arg >> 16, arg & 0xFF, bool(arg & 256)
                self.locals.add(slot)
                end = pc + 1 + length
                self.out('{', indent)
                self.out(f'Slot high = {s(1)}, low = {s(2)};', indent + 1)
                self.out('uint64_t count;', indent + 1)
                if integer:
                    self.out('count = high.i64 > low.i64 ? (uint64_t)high.i64 - (uint64_t)low.i64 : 0;', indent + 1)
                else:
                    self.out('if (!(fabs(low.num) <= 0x1p53 && fabs(high.num) <= 0x1p53 && low.num == floor(low.num) && '
                             f'high.num == floor(high.num))) {self.fault(pc, "LIMIT")}', indent + 1)
                    self.out('count = high.num > low.num ? (uint64_t)(high.num - low.num) : 0;', indent + 1)
                self.out('if (count) {', indent + 1)
                self.out(f'if (!charge(app, {length + 1})) {self.fault(pc, "WORK")}', indent + 2)
                self.out(f'size_t mark_{pc} = arena->used;', indent + 2)
                self.out(f'for (uint64_t k_{pc} = 0;;) {{', indent + 2)
                self.out(f'L{slot}.{"i64" if integer else "num"} = ' +
                         ('(int64_t)((uint64_t)low.i64 + k_%d);' % pc if integer else 'low.num + (double)k_%d;' % pc), indent + 3)
                self.emit(pc + 1, end, loops + [(pc, end)], indent + 3)
                self.out(f'round_{pc}:', indent + 2)
                self.out(f'arena->used = mark_{pc};', indent + 3)
                self.out(f'if (++k_{pc} >= count) break;', indent + 3)
                self.out(f'if (!charge(app, {length + 1})) {self.fault(pc, "WORK")}', indent + 3)
                self.out('}', indent + 2)
                self.out(f'broke_{pc}:;', indent + 1)
                self.out('}', indent + 1)
                self.out('}', indent)
                self.covered[name] = self.covered.get(name, 0) + 1
                pc = end
                continue
            else:
                native = False
                after = self.height(pc + 1) if pc + 1 < self.end else 0
                self.out(''.join(f'stack[{k}] = s{k}; ' for k in range(h)) + f'frame.sp = {h};', indent)
                self.out(f'if ((how = pure_run_step(&frame, {pc})) != STEP_NEXT) goto done;', indent)
                self.out(''.join(f's{k} = stack[{k}]; ' for k in range(after)) or ';', indent)
                self.handed[name] = self.handed.get(name, 0) + 1
            if native:
                self.covered[name] = self.covered.get(name, 0) + 1
            pc += 1

    def source(self, module):
        self.emit(self.start, self.end, [], 1)
        body = self.lines
        head = [
            f'/* Research 049: {module}\'s handler at {self.start} ({self.block["length"]} instructions) emitted as C by emit.py',
            ' * (the exact rung). Built with core/module.c: run() gives this block to emitted_run. */',
            '#define PURE_EMITTED 1',
            '#include "module.c"',
            '',
            'static bool emitted_run(const PureModule *m, PureValue *values, PureBlock block, Event *ev, Slot *result, PureResult *out) {',
            f'    if (block.start != {self.start} || block.length != {self.block["length"]}) return false;',
            '    Slot stack[PURE_MODULE_STACK];',
            '    Loop loops[PURE_MODULE_NESTING];',
            '    Frame frame;',
            '    open_frame(&frame, m, values, block, ev, stack, loops);',
            '    PureApp *app = frame.app;',
            '    PureArena *arena = frame.arena;',
            '    PureResult status;',
            '    int how = STEP_NEXT;',
            '    ' + ' '.join(f'Slot s{k};' for k in range(self.depth)),
            '    ' + ' '.join(f'PureValue L{n} = {{0}};' for n in sorted(self.locals)),
            '    (void)status; (void)arena; (void)values;',
        ]
        tail = ['ran:', '    frame.sp = 0;', 'done:', '    *out = finish(&frame, how, result);', '    return true;', '}', '']
        return '\n'.join(head + body + tail)


def main():
    dump = json.loads(Path(sys.argv[1]).read_text())
    e = Emitter(dump, int(sys.argv[2]))
    Path(sys.argv[3]).write_text(e.source(Path(sys.argv[1]).stem))
    print(json.dumps({'handler': int(sys.argv[2]), 'instructions': e.block['length'], 'covered': e.covered, 'handed': e.handed}))


if __name__ == '__main__':
    main()
