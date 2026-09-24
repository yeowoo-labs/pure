import test from 'node:test';
import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
import { createRuntime } from './web/runtime.mjs';
import { createRuntime as before } from './results/2026-09-23-presentation/before/web/runtime.mjs';

const wasm = await readFile(new URL('./web/core.wasm', import.meta.url));
const source = `state x = 3
state radius = 5
input move = KeyM
input shrink = KeyS
box tile(x, 10, x * 2, 20, "#123456")
circle dot(50, 50, radius, "#abcdef")
text label(0, 0, 10 / x, "Value: ")
on move.down { x = x + 0.25 }
on shrink.down { radius = radius - 10 }
on frame { x = x + dt }
`;

// Reconstruct only in tests, for comparison with the archived snapshot path.
function collect(runtime, hooks = {}) {
  let states, views, declared;
  const result = runtime.present({
    begin(program) {
      declared = program; states = {}; views = [];
      hooks.begin?.();
    },
    state(i, value) { states[declared.states[i].name] = value; },
    view(i, valid) {
      if (!valid) return;
      const {id, type, color, prefix} = declared.views[i];
      views.push({id, type, color, prefix});
    },
    value(i, name, value) { views.at(-1)[name] = value; hooks.value?.(); },
    end(count) { assert.equal(views.length, count); },
  });
  return {states, views, ...result};
}

test('direct publication matches archived snapshots and work counts through authored changes', async () => {
  for (const cacheEntries of [0, 1, 32]) {
    const left = await createRuntime(wasm, {cacheEntries}), right = await before(wasm, {cacheEntries});
    const compare = () => {
      assert.deepEqual(collect(left), right.snapshot());
      assert.deepEqual(left.evaluationStats(), right.evaluationStats());
    };
    compare();
    for (const r of [left, right]) r.apply(source);
    compare();
    for (let i = 0; i < 120; i++) {
      for (const r of [left, right]) {
        r.down('move', 'keyboard'); r.down('move', 'pointer');
        r.up('move', 'keyboard'); r.up('move', 'pointer');
        if (i % 11 === 0) r.frame(2);
        if (i % 17 === 0) r.forgetEvaluation('history');
        if (i % 23 === 0) r.reset();
      }
      compare();
    }
    for (const r of [left, right]) { r.down('shrink'); r.up('shrink'); }
    compare(); // invalid circle only; other views still publish
    for (const r of [left, right]) assert.throws(() => r.apply('state broken ='));
    compare();
    for (const r of [left, right]) r.apply('state radius = 1\nstate x = 0\ntext label(1, 2, x)');
    compare();
    for (const r of [left, right]) r.apply('');
    compare();
  }
});

test('whole-scene budget failure never publishes partial bindings and recovers exactly', async () => {
  const left = await createRuntime(wasm, {cacheEntries:32}), right = await before(wasm, {cacheEntries:32});
  for (const r of [left, right]) r.apply(source);
  for (let workLimit = 0; workLimit <= 80; workLimit++) {
    for (const r of [left, right]) r.configureEvaluation({workLimit});
    let calls = 0;
    const result = collect(left, {value: () => calls++});
    assert.deepEqual(result, right.snapshot());
    assert.deepEqual(left.evaluationStats(), right.evaluationStats());
    if (result.displayError?.code === 101) assert.equal(calls, 0);
    else assert.equal(calls, 10);
  }
});

test('latched event rollback, nonfinite bindings, reset and signed zero keep their semantics', async () => {
  const left = await createRuntime(wasm), right = await before(wasm);
  const definition = 'state x = 0\ninput fail = Space\ntext value(0, 0, 1 / x)\non fail.down { x = 2\nx = 1 / 0 }';
  for (const r of [left, right]) { r.apply(definition); r.down('fail'); r.up('fail'); }
  assert.deepEqual(collect(left), right.snapshot());
  for (const r of [left, right]) r.configureEvaluation({workLimit:0});
  assert.deepEqual(collect(left), right.snapshot());
  for (const r of [left, right]) {
    r.apply('state x = -0\ntext value(x, 0, x)'); r.reset(); r.configureEvaluation({workLimit:2048});
  }
  const result = collect(left);
  assert.deepEqual(result, right.snapshot());
  assert(Object.is(result.states.x, -0));
});

test('publication rejects reentrant runtime changes and reads before they affect the scene', async () => {
  const runtime = await createRuntime(wasm);
  runtime.apply(source);
  const calls = [() => runtime.apply(''), () => runtime.reset(), () => runtime.frame(),
    () => runtime.down('move'), () => runtime.up('move'), () => runtime.releaseAll(),
    () => runtime.configureEvaluation({workLimit:0}), () => runtime.forgetEvaluation(),
    () => runtime.snapshot(), () => runtime.present({})];
  const expected = runtime.snapshot();
  assert.deepEqual(collect(runtime, {begin() {
    for (const call of calls) assert.throws(call, /출력 전달 중/);
  }}), expected);
  assert.deepEqual(runtime.snapshot(), expected);
});

test('invalid or throwing output adapters do not strand the runtime or alter authored state', async () => {
  const runtime = await createRuntime(wasm);
  runtime.apply(source);
  const expected = runtime.snapshot();
  assert.throws(() => runtime.present({}), TypeError);
  assert.throws(() => collect(runtime, {value() { throw new Error('destination failed'); }}), /destination failed/);
  assert.deepEqual(collect(runtime), expected);
  runtime.down('move'); runtime.up('move');
  assert.equal(collect(runtime).states.x, 3.25);
});
