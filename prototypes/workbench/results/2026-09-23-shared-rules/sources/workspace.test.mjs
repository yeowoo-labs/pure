import test from 'node:test';
import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
import { compile } from './web/compiler.mjs';
import { createRuntime } from './web/runtime.mjs';

const wasm = await readFile(new URL('./web/core.wasm', import.meta.url));
const source = factor => `state x = 10
input next = KeyN
text label(0, 0, x * ${factor})
on next.down { x = x + 0.25 }
`;

function begin(core, program, preserve) {
  assert.equal(core.begin_program(program.states.length, program.inputs.length), 0);
  program.states.forEach((state, i) => assert.equal(core.put_state(i, state.initial, preserve ? i : -1), 0));
}

function finish(core, program, during = () => {}) {
  for (const op of program.instructions) {
    assert.equal(core.put_instruction(op.op, op.arg, op.value), 0);
    during();
  }
  for (const handler of program.handlers)
    assert.equal(core.put_handler(handler.event, handler.start, handler.length), 0);
  assert.equal(core.commit_program(), 0);
}

function observe(core, binding) {
  core.begin_evaluation(100);
  const value = core.eval_bounded(binding.start, binding.length);
  assert.equal(core.evaluation_info(0), 0);
  return value;
}

test('Wasm executes old events and output during staged replacement without corrupting either phase', async () => {
  const {instance:{exports:core}} = await WebAssembly.instantiate(wasm);
  core.reset_all();
  const old = compile(source(2)), next = compile(source(3));
  begin(core, old, false); finish(core, old);
  assert.equal(core.configure_evaluation(32, 8), 0);
  const binding = old.views[0].bindings.value;
  assert.equal(observe(core, binding), 20);
  assert.equal(observe(core, binding), 20);
  assert.equal(core.evaluation_info(3), 1);
  begin(core, next, true);
  let value = 10;
  finish(core, next, () => {
    assert.equal(core.evaluation_info(9), 0);
    assert.equal(core.dispatch(1, 0), 0); value += 0.25;
    assert.equal(core.configure_evaluation(32, 8), 0);
    core.forget_evaluation(3);
    assert.equal(observe(core, binding), value * 2);
    assert.equal(core.evaluation_info(3), 0);
  });
  assert.equal(core.evaluation_info(9), 1);
  assert.equal(core.get_state(0), value);
  const replaced = next.views[0].bindings.value;
  assert.equal(observe(core, replaced), value * 3);
  assert.equal(observe(core, replaced), value * 3);
  assert.equal(core.evaluation_info(3), 1);

  const revision = core.info(4);
  begin(core, old, true);
  assert.equal(core.put_instruction(0, 0, Infinity), 5);
  assert.equal(core.commit_program(), 5);
  assert.equal(observe(core, replaced), value * 3);
  core.abort_program();
  assert.equal(core.info(4), revision);
  assert.equal(core.get_state(0), value);
  assert.equal(core.evaluation_info(9), 1);
  assert.equal(observe(core, replaced), value * 3);
  assert.equal(core.memory.buffer.byteLength, 65536);
});

test('workspace accounting reports shared payload and safety tag without double-counting', async () => {
  const runtime = await createRuntime(wasm, {cacheEntries:32});
  runtime.apply(source(2)); runtime.snapshot();
  const stats = runtime.evaluationStats(), memory = runtime.snapshot().memory;
  assert.equal(stats.dedicatedBytes, 48);
  assert.equal(stats.sharedBytes, 1152);
  assert.equal(stats.workspaceTagBytes, 8);
  assert.equal(stats.reservedBytes, stats.dedicatedBytes + stats.sharedBytes);
  const restoreStorage = memory.stagingReservedBytes - memory.programReservedBytes;
  assert.equal(restoreStorage, 264); // 32 values + mask/alignment, only in pending storage.
  assert.equal(memory.reservedEngineBytes, 19072 + stats.dedicatedBytes + stats.workspaceTagBytes + restoreStorage + 2 * 64 * (memory.handlerBytes - 12));
  assert(stats.sharedBytes < memory.stagingReservedBytes);
  assert.equal(stats.reuseAvailable, true);
});
