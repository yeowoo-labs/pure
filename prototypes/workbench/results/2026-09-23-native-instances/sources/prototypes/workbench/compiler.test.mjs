import test from 'node:test';
import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
import { compile, LIMITS } from './web/compiler.mjs';

function diagnostic(source, pattern, line, column) {
  assert.throws(() => compile(source), error => {
    assert.match(error.message, pattern);
    assert.ok(Number.isInteger(error.line) && error.line > 0);
    assert.ok(Number.isInteger(error.column) && error.column > 0);
    if (line !== undefined) assert.equal(error.line, line);
    if (column !== undefined) assert.equal(error.column, column);
    return true;
  });
}

// A small independent evaluator checks compiler lowering. The product executes
// these instructions in Zig; the VM has its own native and integration checks.
function evaluate(program, start, length, values, dt = 0) {
  const stack = [];
  for (let pc = start; pc < start + length; pc++) {
    const instruction = program.instructions[pc];
    const { op, arg, value } = instruction;
    if (op === 0) stack.push(value);
    else if (op === 1) stack.push(values[arg]);
    else if (op === 2) stack.push(dt);
    else if (op === 7) stack.push(-stack.pop());
    else if (op === 16) stack.push(Number(!stack.pop()));
    else if (op === 19) {
      const high = stack.pop(), low = stack.pop(), x = stack.pop();
      assert.ok(low <= high); stack.push(Math.min(Math.max(x, low), high));
    } else if (op === 20) values[arg] = stack.pop();
    else if (op === 21) { if (!stack.pop()) pc = arg - 1; }
    else {
      const right = stack.pop(), left = stack.pop();
      const operations = {
        3: () => left + right, 4: () => left - right, 5: () => left * right,
        6: () => left / right, 8: () => Number(left < right),
        9: () => Number(left <= right), 10: () => Number(left > right),
        11: () => Number(left >= right), 12: () => Number(left === right),
        13: () => Number(left !== right), 14: () => Number(Boolean(left) && Boolean(right)),
        15: () => Number(Boolean(left) || Boolean(right)),
        17: () => Math.min(left, right), 18: () => Math.max(left, right),
      };
      assert.ok(operations[op], `Unknown opcode ${op}`);
      stack.push(operations[op]());
    }
    assert.ok(stack.every(Number.isFinite));
  }
  return stack;
}

function dispatch(program, event, values, dt = 0) {
  for (const handler of program.handlers.filter(handler => handler.event === event)) {
    assert.deepEqual(evaluate(program, handler.start, handler.length, values, dt), []);
  }
}

test('a blank document is an empty program with no implied behavior', () => {
  const result = compile('');
  for (const field of ['states', 'inputs', 'views', 'instructions', 'handlers']) assert.deepEqual(result[field], []);
  assert.equal(result.metrics.sourceBytes, 0);
  assert.equal(result.metrics.maxStack, 0);
  assert.deepEqual(compile('# comment only\n;').instructions, []);
});

test('forward references and separate state/input namespaces preserve declaration IDs', () => {
  const program = compile('on tap.down { second = tap + first }\ntext label(0,0,second)\nstate first = -2\nstate tap = +3\ninput tap = Space\nstate second = .5e2');
  assert.deepEqual(program.states.map(state => state.name), ['first', 'tap', 'second']);
  assert.equal(program.handlers[0].event, 1);
  const values = program.states.map(state => state.initial);
  dispatch(program, 1, values);
  assert.deepEqual(values, [-2, 3, 1]);
  const binding = program.views[0].bindings.value;
  assert.deepEqual(evaluate(program, binding.start, binding.length, values), [1]);
});

test('ordered assignments, repeated handlers, and condition evaluation once', () => {
  const program = compile(`state x = 1
input tap = Enter
on tap.down {
  if x == 1 {
    x = 0
    x = x + 5
  }
}
on tap.down { x = x * 2 }
on tap.up { x = x - 1 }`);
  const values = [1];
  dispatch(program, 1, values);
  assert.equal(values[0], 10);
  dispatch(program, 2, values);
  assert.equal(values[0], 9);
  const branch = program.instructions.find(instruction => instruction.op === 21);
  assert.equal(branch.arg, program.handlers[0].start + program.handlers[0].length);
});

test('nested branches use absolute forward targets and skip correct bodies', () => {
  const program = compile(`state x = 0
text label(0, 0, x)
on frame {
  if x > 0 {
    x = 8
    if x == 8 { x = 9 }
  }
  x = x + 1
}`);
  const values = [0];
  dispatch(program, 0, values, 1 / 60); assert.equal(values[0], 1);
  dispatch(program, 0, values, 1 / 60); assert.equal(values[0], 10);
  const handler = program.handlers[0];
  program.instructions.forEach((instruction, pc) => {
    if (instruction.op === 21) assert.ok(instruction.arg > pc && instruction.arg <= handler.start + handler.length);
  });
});

test('operator precedence, associativity, unary and builtin function lowering', () => {
  const expressions = new Map([
    ['2 + 3 * 4', 14], ['8 - 3 - 2', 3], ['12 / 3 / 2', 2],
    ['-2 * -3', 6], ['!0 + !4', 1], ['1 || 0 && 0', 1],
    ['1 < 2 && 2 <= 2 && 4 > 3 && 4 >= 4', 1],
    ['3 == 3 && 3 != 4', 1], ['clamp(max(3, 9) - min(4, 7), 0, 4)', 4],
  ]);
  for (const [source, expected] of expressions) {
    const program = compile(`state x = 0\non frame { x = ${source} }`);
    const values = [0]; dispatch(program, 0, values, 1 / 60);
    assert.equal(values[0], expected, source);
  }
  const eager = compile('state x = 0\non frame { x = 1 || x / 0 }');
  assert.ok(eager.instructions.some(instruction => instruction.op === 6));
  assert.ok(!eager.instructions.some(instruction => instruction.op === 21));
});

test('multiline parentheses, scientific literals, comments, and string escapes', () => {
  const program = compile(`state x = 1e-2 # comment
box tile(
  (x +
    2), 0, max(
      1, 2), 4,
  "#AABBCC")
text label(0, 0, x, "# count: \\\"value\\\" \\n")
on frame { x = x + dt }`);
  assert.equal(program.views[0].color, '#AABBCC');
  assert.equal(program.views[1].prefix, '# count: "value" \n');
  const values = [.01]; dispatch(program, 0, values, 1 / 60);
  assert.equal(values[0], .01 + 1 / 60);
  const binding = program.views[0].bindings.x;
  assert.deepEqual(evaluate(program, binding.start, binding.length, values), [values[0] + 2]);
});

test('all instruction fields are numeric and carry original source line', () => {
  const program = compile('state x = 0\non frame {\n x = x + dt\n}');
  for (const instruction of program.instructions) {
    assert.deepEqual(Object.keys(instruction), ['op', 'arg', 'value', 'line']);
    assert.ok(Object.values(instruction).every(Number.isFinite));
    assert.equal(instruction.line, 3);
  }
});

test('contextual declaration words remain valid names outside reserved builtins', () => {
  const program = compile('state if = 1\nstate state = 2\non frame { if = if + state }');
  const values = [1, 2]; dispatch(program, 0, values, 1 / 60);
  assert.deepEqual(values, [3, 2]);
});

test('unknown and reserved symbols have useful one-based locations', () => {
  diagnostic('state x = 0\non frame {\n  x = missing\n}', /Unknown state missing/, 3, 7);
  diagnostic('on missing.down {}', /Unknown input missing/, 1, 4);
  diagnostic('on frame { absent = 1 }', /Unknown state absent/, 1, 12);
  for (const name of ['frame', 'dt', 'min', 'max', 'clamp']) {
    diagnostic(`state ${name} = 0`, /Reserved name/, 1, 7);
    diagnostic(`input ${name} = Space`, /Reserved name/);
  }
  diagnostic('text clock(0, 0, dt)', /dt is only available in handlers/);
});

test('duplicates, unsupported keys, initializers, and invalid syntax are rejected', () => {
  const invalid = [
    ['state x=0\nstate x=1', /Duplicate state/],
    ['input tap=Space\ninput tap=Enter', /Duplicate input/],
    ['input tap=Space\ninput other=Space', /Duplicate keyboard/],
    ['box a(0,0,1,1,"#000000")\ncircle a(0,0,1,"#000000")', /Duplicate view/],
    ['input tap=Escape', /Unsupported keyboard/],
    ['state x=1+2', /Expected newline/],
    ['state x=1e999', /finite/],
    ['state x=NaN', /numeric state initializer/],
    ['on tap.click {}', /Expected down or up/],
    ['on frame.down {}', /Expected {/],
    ['state x=0\non frame { x = min(1) }', /min expects 2/],
    ['state x=0\non frame { x = clamp(1,2) }', /clamp expects 3/],
    ['state x=0\non frame { x = 1 +\n2 }', /Expected a number/],
    ['text a(0,0,1,"broken)', /Unterminated string/],
    ['text a(0,0,1,"bad\\q")', /Unsupported string escape/],
    ['box a(0,0,1,1,"blue")', /Color must use/],
    ['state x=0 @', /Unexpected character/],
    ['on frame {', /assignment, if, or closing brace/],
    ['state x = 0 state y = 1', /Expected newline/],
  ];
  for (const [source, pattern] of invalid) diagnostic(source, pattern);
});

test('document, declaration, instruction, and nesting limits reject overflow', () => {
  diagnostic('#' + '한'.repeat(11000), /32768 UTF-8 bytes/);
  diagnostic(Array.from({ length: 33 }, (_, i) => `state s${i}=0`).join('\n'), /32 states/);
  diagnostic(Array.from({ length: 9 }, (_, i) => `input i${i}=Digit${i}`).join('\n'), /8 inputs/);
  diagnostic(Array.from({ length: 25 }, (_, i) => `text t${i}(0,0,0)`).join('\n'), /24 views/);
  diagnostic('on frame {}\n'.repeat(65), /64 handlers/);
  diagnostic(`state x=0\non frame {\n${'x=1\n'.repeat(257)}}`, /512 instructions/);
  assert.equal(compile(`state x=0\non frame {\n${'x=1\n'.repeat(256)}}`).instructions.length, LIMITS.instructions);
  diagnostic(`state x=0\non frame { x=${'('.repeat(65)}1${')'.repeat(65)} }`, /nesting exceeds 64/);
  diagnostic(`on frame { ${'if 1 { '.repeat(65)}${'}\n'.repeat(66)}`, /nesting exceeds 64/);
  // Right-nested clamp's two waiting operands exceed the stack before its depth limit.
  let deep = '0';
  for (let i = 0; i < 33; i++) deep = `clamp(0,0,${deep})`;
  diagnostic(`text stack(0,0,${deep})`, /stack exceeds 64/);
});

test('every supported input maps to a bounded ordered event ID', () => {
  const keys = ['Space', 'Enter', 'ArrowLeft', 'ArrowRight', 'ArrowUp', 'ArrowDown', 'KeyZ', 'Digit9'];
  const program = compile(keys.map((key, i) => `input i${i}=${key}\non i${i}.down {}\non i${i}.up {}`).join('\n'));
  assert.deepEqual(program.handlers.map(handler => handler.event), Array.from({ length: 16 }, (_, i) => i + 1));
});

test('optional examples compile and charge behavior is entirely authored', async () => {
  for (const name of ['counter', 'charge', 'toggle']) {
    const program = compile(await readFile(new URL(`./examples/${name}.pure`, import.meta.url), 'utf8'));
    assert.ok(program.instructions.length <= LIMITS.instructions);
    assert.ok(program.views.length > 0);
    if (name === 'charge') {
      const values = program.states.map(state => state.initial);
      dispatch(program, 1, values);
      for (let frame = 0; frame < 90; frame++) dispatch(program, 0, values, 1 / 60);
      assert.equal(values[0], 1); assert.equal(values[1], 1); assert.equal(values[2], 240);
      dispatch(program, 2, values); assert.deepEqual(values, [0, 0, 240, -460]);
      dispatch(program, 0, values, 1 / 60); assert.ok(values[2] < 240);
      for (let frame = 0; frame < 120; frame++) dispatch(program, 0, values, 1 / 60);
      assert.equal(values[2], 240); assert.equal(values[3], 0);
    }
  }
});
