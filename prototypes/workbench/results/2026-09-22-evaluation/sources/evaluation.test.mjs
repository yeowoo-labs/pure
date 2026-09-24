import test from 'node:test';
import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
import { createRuntime } from './web/runtime.mjs';

const wasm = await readFile(new URL('./web/core.wasm', import.meta.url));
const make = (options = {}) => createRuntime(wasm, options);
const source = `state x = 3
state spare = 0
input next = KeyN
input other = KeyO
input fail = KeyF
text value(0, 0, (x + 1) * (x + 2) / 2)
on next.down { x = x + 0.25 }
on other.down { spare = spare + 1 }
on fail.down { x = 900; spare = 1 / 0 }
`;

test('cache reuse, unrelated changes and discarded records preserve authored values', async () => {
  const cached = await make({ cacheEntries: 32 }), plain = await make();
  cached.apply(source); plain.apply(source);
  assert.deepEqual(cached.snapshot(), plain.snapshot());
  assert.deepEqual(cached.snapshot(), plain.snapshot());
  assert(cached.evaluationStats().cacheHits > 0);
  assert(cached.evaluationStats().instructions < plain.evaluationStats().instructions);
  for (let i = 0; i < 100; i++) {
    const name = i % 3 ? 'other' : 'next';
    for (const runtime of [cached, plain]) { runtime.down(name); runtime.up(name); }
    if (i % 7 === 0) cached.forgetEvaluation('history');
    if (i % 11 === 0) cached.forgetEvaluation('values');
    if (i % 13 === 0) cached.forgetEvaluation('all');
    assert.deepEqual(cached.snapshot(), plain.snapshot());
  }
  // Omit observations long enough to overwrite every change record.
  for (let i = 0; i < 20; i++) for (const runtime of [cached, plain]) {
    runtime.down('next'); runtime.up('next');
  }
  assert.deepEqual(cached.snapshot(), plain.snapshot());
});

test('committed changes only: failed events and failed edits preserve reusable output', async () => {
  const cached = await make({ cacheEntries: 32 }); cached.apply(source);
  const before = cached.snapshot();
  cached.down('fail'); cached.up('fail');
  const failed = cached.snapshot();
  assert.deepEqual(failed.states, before.states);
  assert.deepEqual(failed.views, before.views);
  assert.equal(failed.lastError, 4);
  assert.equal(cached.evaluationStats().instructions, 0);
  assert.throws(() => cached.apply('state x = missing'));
  assert.deepEqual(cached.snapshot(), failed);
  assert.equal(cached.evaluationStats().instructions, 0);
});

test('program edits, state reordering and reset cannot reuse old definitions', async () => {
  const cached = await make({ cacheEntries: 32 }), plain = await make();
  for (const r of [cached, plain]) { r.apply(source); r.down('next'); r.up('next'); r.snapshot(); }
  const changed = source.replace('state x = 3\nstate spare = 0', 'state spare = 9\nstate x = 300')
    .replace('(x + 1) * (x + 2) / 2', '(x + 10) * (x + 20) / 2');
  for (const r of [cached, plain]) r.apply(changed);
  assert.deepEqual(cached.snapshot(), plain.snapshot());
  for (const r of [cached, plain]) r.reset();
  assert.deepEqual(cached.snapshot(), plain.snapshot());
  assert.equal(cached.snapshot().states.x, 300);
});

test('finite float semantics include signed zero, high state bit and arithmetic errors', async () => {
  const cached = await make({ cacheEntries: 32 }), plain = await make();
  const program = `${Array.from({ length: 32 }, (_, i) => `state s${i} = 0`).join('\n')}
input flip = KeyF
text sign(0, 0, s31)
text error(0, 0, 1 / s31)
on flip.down { s31 = -s31 }
on flip.up { s31 = 2 }
`;
  for (const r of [cached, plain]) r.apply(program);
  assert.deepEqual(cached.snapshot(), plain.snapshot());
  for (const r of [cached, plain]) r.down('flip');
  assert.deepEqual(cached.snapshot(), plain.snapshot());
  assert(Object.is(cached.snapshot().views[0].value, -0));
  assert.equal(cached.snapshot().displayError.code, 100);
  for (const r of [cached, plain]) r.up('flip');
  assert.deepEqual(cached.snapshot(), plain.snapshot());
  assert.equal(cached.snapshot().displayError, null);
});

test('one budget covers the whole scene; failures publish no partial scene or state rollback', async () => {
  const r = await make({ cacheEntries: 32 }); r.apply(source);
  r.down('next'); r.up('next');
  const good = r.snapshot();
  r.configureEvaluation({ workLimit: 5 });
  const limited = r.snapshot();
  assert.equal(limited.lastError, 101);
  assert.deepEqual(limited.views, []);
  assert.deepEqual(limited.states, good.states);
  assert.equal(limited.successfulEvents, good.successfulEvents);
  assert(r.evaluationStats().work <= 5);
  r.down('fail'); r.up('fail');
  assert.equal(r.snapshot().lastError, 4);
  assert.equal(r.snapshot().displayError.code, 101);
  r.configureEvaluation({ workLimit: 2048 });
  assert.deepEqual(r.snapshot().views, good.views);
  assert.equal(r.snapshot().lastError, 4);
});

test('small caches, no history and every small work limit give exact results or explicit failure', async () => {
  const plain = await make(); plain.apply(source);
  for (const cacheEntries of [0, 1, 32]) for (const historyEntries of [0, 1, 8]) {
    const r = await make({ cacheEntries, historyEntries }); r.apply(source);
    for (let workLimit = 0; workLimit < 100; workLimit++) {
      r.configureEvaluation({ workLimit });
      const actual = r.snapshot();
      assert(r.evaluationStats().work <= workLimit);
      if (actual.lastError) {
        assert.equal(actual.lastError, 101);
        assert.deepEqual(actual.views, []);
      } else assert.deepEqual(actual, plain.snapshot());
    }
  }
});

test('invalid settings and forget names preserve policy and state', async () => {
  const r = await make({ cacheEntries: 32 }); r.apply(source);
  const before = r.snapshot(), policy = r.evaluationStats();
  for (const option of [{cacheEntries:33}, {historyEntries:9}, {workLimit:-1},
    {workLimit:0x100000000}, {workLimit:NaN}, {cacheEntries:0.5}]) {
    assert.throws(() => r.configureEvaluation(option));
    assert.deepEqual(r.evaluationStats(), policy);
  }
  for (const name of ['unknown', '__proto__', 'constructor']) assert.throws(() => r.forgetEvaluation(name));
  assert.deepEqual(r.snapshot(), before);
  assert.equal(r.snapshot().memory.linearBytes, 65536);
});
