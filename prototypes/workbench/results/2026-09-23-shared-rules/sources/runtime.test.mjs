import test from 'node:test';
import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
import { createRuntime } from './web/runtime.mjs';

const wasm = await readFile(new URL('./web/core.wasm', import.meta.url));
const fresh = () => createRuntime(wasm);
const countSource = `state total = 3
input add = KeyA
box bar(20, 80, total * 10, 30, "#abcdef")
text label(20, 60, total, "Total: ")
on add.down { total = total + 2 }
on add.up { total = total + 1 }
`;

test('blank startup and applied empty source contain no application objects or behavior', async () => {
  const runtime = await fresh();
  assert.equal(runtime.currentProgram, null);
  assert.deepEqual(runtime.snapshot().states, {});
  assert.deepEqual(runtime.snapshot().views, []);
  runtime.apply('');
  runtime.frame(60);
  assert.deepEqual(runtime.snapshot().states, {});
  assert.deepEqual(runtime.snapshot().views, []);
  assert.equal(runtime.snapshot().tick, 60);
  assert.throws(() => runtime.down('jump'), /jump/);
});

test('author can add unrelated states inputs views and behavior entirely through source', async () => {
  const runtime = await fresh();
  runtime.apply(countSource);
  runtime.down('add'); runtime.up('add');
  assert.equal(runtime.snapshot().states.total, 6);
  const added = `${countSource}\nstate radius = 12
input inflate = KeyI
circle bubble(240, 160, radius, "#123456")
on inflate.down { radius = radius + total }
on frame { radius = radius + 60 * dt }
`;
  runtime.apply(added);
  runtime.down('inflate'); runtime.up('inflate'); runtime.frame(2);
  assert.deepEqual(runtime.snapshot().states, { total: 6, radius: 20 });
  assert.equal(runtime.snapshot().views.find(view => view.id === 'bubble').radius, 20);
  assert.equal(runtime.snapshot().views.length, 3);
});

test('keyboard and pointer sources aggregate first down and last up; repeats are ignored', async () => {
  const runtime = await fresh(); runtime.apply(countSource);
  runtime.down('add', 'keyboard'); runtime.down('add', 'keyboard'); runtime.down('add', 'pointer');
  assert.equal(runtime.snapshot().states.total, 5);
  runtime.up('add', 'unknown'); runtime.up('add', 'keyboard');
  assert.equal(runtime.snapshot().states.total, 5);
  assert.deepEqual(runtime.snapshot().held, ['add']);
  runtime.releaseAll(); runtime.up('add', 'pointer');
  assert.equal(runtime.snapshot().states.total, 6);
  assert.deepEqual(runtime.snapshot().held, []);
  assert.equal(runtime.snapshot().successfulEvents, 2);
});

test('held input rejects apply without changing applied source, metadata, or values', async () => {
  const runtime = await fresh(); runtime.apply(countSource); runtime.down('add');
  const before = runtime.snapshot();
  assert.throws(() => runtime.apply('state changed = 9'), /놓은 뒤/);
  assert.deepEqual(runtime.snapshot(), before);
  assert.equal(runtime.exportProject().source, countSource);
  runtime.up('add'); runtime.apply('state changed = 9');
  assert.deepEqual(runtime.snapshot().states, { changed: 9 });
});

test('invalid syntax, unknown references and capacity overflow preserve the previous program', async () => {
  const runtime = await fresh(); runtime.apply(countSource); runtime.down('add'); runtime.up('add');
  const before = runtime.snapshot();
  for (const source of ['state broken =', 'state n = 0\non frame { n = missing }',
    Array.from({ length: 33 }, (_, i) => `state v${i} = ${i}`).join('\n'), '#'.repeat(32769)]) {
    assert.throws(() => runtime.apply(source));
    assert.deepEqual(runtime.snapshot(), before);
    assert.equal(runtime.exportProject().source, countSource);
  }
});

test('state preservation follows names across reorder/add/remove; reset uses new initializers', async () => {
  const runtime = await fresh();
  runtime.apply('state x = 11\nstate y = 22');
  runtime.apply('state z = 30\nstate y = 2\nstate x = 1');
  assert.deepEqual(runtime.snapshot().states, { z: 30, y: 22, x: 11 });
  runtime.apply('state y = 200\nstate renamed = 9');
  assert.deepEqual(runtime.snapshot().states, { y: 22, renamed: 9 });
  runtime.reset();
  assert.deepEqual(runtime.snapshot().states, { y: 200, renamed: 9 });
  assert.equal(runtime.snapshot().revision, 3);
});

test('callers cannot mutate active metadata and corrupt name-based state preservation', async () => {
  const runtime = await fresh(); const program = runtime.apply('state x = 11\nstate y = 22');
  assert.throws(() => program.states.reverse(), TypeError);
  assert.throws(() => { runtime.currentProgram.states[0].name = 'y'; }, TypeError);
  runtime.apply('state x = 0\nstate y = 0');
  assert.deepEqual(runtime.snapshot().states, { x: 11, y: 22 });
});

test('conditions execute once, and assignments and multiple handlers follow source order', async () => {
  const runtime = await fresh();
  runtime.apply(`state x = 0
state seen = 0
on frame {
  if x == 0 {
    x = 1
    seen = x + 1
  }
  x = x + 3
}
on frame { seen = seen + x }
`);
  runtime.frame();
  assert.deepEqual(runtime.snapshot().states, { x: 4, seen: 6 });
  runtime.frame();
  assert.deepEqual(runtime.snapshot().states, { x: 7, seen: 13 });
});

test('a failed event rolls back all handlers and keeps its source diagnostic through release', async () => {
  const runtime = await fresh();
  const source = `state x = 7
state y = 9
input breakit = Space
on breakit.down { x = 99 }
on breakit.down {
  y = 2
  x = 1 / 0
}
`;
  runtime.apply(source);
  assert.equal(runtime.down('breakit'), 4);
  assert.deepEqual(runtime.snapshot().states, { x: 7, y: 9 });
  assert.equal(runtime.snapshot().successfulEvents, 0);
  assert.equal(runtime.snapshot().errorLine, 7);
  runtime.up('breakit'); runtime.frame();
  assert.equal(runtime.snapshot().lastEvent.status, 0);
  assert.equal(runtime.snapshot().lastFailure.phase, 'down');
  assert.equal(runtime.snapshot().lastError, 4);
  assert.equal(runtime.snapshot().errorLine, 7);
  assert.throws(() => runtime.apply('bad'));
  assert.equal(runtime.snapshot().lastError, 4);
  runtime.apply(source.replace('1 / 0', '10'));
  assert.equal(runtime.snapshot().lastError, 0);
  runtime.down('breakit'); runtime.up('breakit');
  assert.deepEqual(runtime.snapshot().states, { x: 10, y: 2 });
});

test('reset clears held inputs and latched errors; invalid frame counts have no effects', async () => {
  const runtime = await fresh();
  runtime.apply('state x = 5\ninput tap = Space\non tap.down { x = 1 / 0 }');
  runtime.down('tap'); runtime.reset();
  const before = runtime.snapshot();
  assert.equal(before.lastError, 0); assert.equal(before.lastFailure, null);
  assert.deepEqual(before.held, []);
  for (const count of [-1, 601, 0.5, NaN, Infinity]) assert.throws(() => runtime.frame(count));
  assert.deepEqual(runtime.snapshot(), before);
});

test('render evaluation is pure and invalid binding is distinct from an event rollback', async () => {
  const runtime = await fresh(); runtime.apply(countSource); runtime.down('add'); runtime.up('add');
  const before = runtime.snapshot();
  for (let i = 0; i < 100; i++) assert.deepEqual(runtime.snapshot(), before);
  runtime.apply('state n = 2\ncircle bad(20, 20, -n, "#123456")\ntext fine(0, 0, n)');
  const result = runtime.snapshot();
  assert.equal(result.lastError, 100); assert.equal(result.errorLine, 2);
  assert.deepEqual(result.states, { n: 2 }); assert.equal(result.views.length, 1);
  assert.equal(result.lastFailure, null);
  assert.deepEqual(runtime.snapshot(), result);
});

test('independent adapters replay the same author program without sharing live state', async () => {
  const left = await fresh(), right = await fresh();
  left.apply(countSource); right.apply(countSource);
  for (const runtime of [left, right]) {
    runtime.down('add', 'keyboard'); runtime.frame(3); runtime.up('add', 'keyboard');
  }
  assert.deepEqual(left.snapshot(), right.snapshot());
  left.down('add');
  assert.equal(right.snapshot().states.total, 6);
  assert.equal(left.snapshot().states.total, 8);
});

test('Wasm has no imports and reports fixed memory and exact active IR/state storage', async () => {
  const module = await WebAssembly.compile(wasm);
  assert.deepEqual(WebAssembly.Module.imports(module), []);
  const runtime = await fresh(); const program = runtime.apply(countSource);
  const before = runtime.snapshot().memory;
  assert.equal(before.linearBytes, 65536);
  assert.equal(before.instructionBytes, 16); assert.equal(before.handlerBytes, 16);
  assert.equal(before.activeIRBytes, program.instructions.length * 16 + program.handlers.length * 16);
  assert.equal(before.activeStateBytes, program.states.length * 8);
  assert(before.stagingReservedBytes < before.reservedEngineBytes);
  assert(before.reservedEngineBytes < before.linearBytes);
  for (let i = 0; i < 20; i++) runtime.frame(600);
  assert.deepEqual(runtime.snapshot().memory, before);
});
