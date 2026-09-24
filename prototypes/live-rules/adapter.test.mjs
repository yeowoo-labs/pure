import { test } from 'node:test';
import assert from 'node:assert/strict';
import { loadCore, snapshot, storage, parseCommand, execute } from './runtime.mjs';

test('the actual Wasm preserves airborne state through a rule change, then permits the second jump', async () => {
  const c = await loadCore();
  execute(c, parseCommand('jump'));
  execute(c, parseCommand('step 8'));
  execute(c, parseCommand('jump'));
  const before = snapshot(c);
  assert.equal(before.state.lastReason, 3);
  const after = execute(c, parseCommand('rules double 12 on'));
  for (const field of ['tick', 'y', 'vy', 'distance', 'obstacleX', 'jumpsUsed', 'hits', 'passed']) {
    assert.equal(after.state[field], before.state[field], field);
  }
  assert.deepEqual(after.decision, before.decision);
  assert.equal(after.state.revision, 1);
  execute(c, parseCommand('jump'));
  assert.equal(c.get(10), 1);
  assert.equal(c.get(2), 1200);
  assert.equal(c.get(7), 2);
  assert.equal(c.input(2), 0);
  assert.equal(c.get(10), 4);
});

test('bad CLI edits and malformed numeric input cannot silently coerce into a different rule', async () => {
  const c = await loadCore();
  const before = snapshot(c);
  for (const command of ['rules double 8.5 on', 'rules double 4294967306 on', 'rules ground 5 on',
    'rules other 10 on', 'rules double 10 maybe', 'rules double 10 on extra', 'step 0', 'step 601', 'jump extra']) {
    assert.throws(() => execute(c, parseCommand(command)), undefined, command);
    assert.deepEqual(snapshot(c), before);
  }
  assert.equal(c.apply_rules(2, 10, 1), 1);
  assert.deepEqual(snapshot(c), before);
});

test('keyboard, pointer and terminal requests give the same simulation result', async () => {
  const states = [];
  for (const source of [0, 1, 2]) {
    const c = await loadCore();
    c.apply_rules(1, 12, 1);
    c.input(source);
    for (let i = 0; i < 8; i++) c.step();
    c.input(source);
    const { lastSource, ...state } = snapshot(c).state;
    assert.equal(lastSource, source);
    states.push(state);
  }
  assert.deepEqual(states[0], states[1]);
  assert.deepEqual(states[1], states[2]);
});

test('paused input has an inspectable decision; advance cannot move paused state', async () => {
  const c = await loadCore();
  assert.equal(snapshot(c).decision, null);
  assert.equal(c.input(0), 1);
  assert.equal(c.get(1), 0);
  assert.equal(c.get(17), 0);
  const before = snapshot(c);
  assert.equal(c.advance(60), 0);
  assert.deepEqual(snapshot(c), before);
  c.step();
  assert.equal(c.get(1), 945);
  assert.equal(c.get(0), 1);
});

test('bounded history and fixed linear memory survive repeated play and edits', async () => {
  const c = await loadCore();
  const before = storage(c);
  c.set_running(1);
  for (let i = 0; i < 5000; i++) {
    c.apply_rules(i % 2, 6 + i % 11, 1);
    c.input(i % 3);
    c.advance(8);
  }
  assert.equal(c.history_count(), 16);
  assert.equal(c.history_get(16, 0), -1);
  assert.equal(c.get(0), 40000);
  assert.deepEqual(storage(c), before);
  assert.equal(before.wasmLinearMemoryBytes, 65536);
  assert.throws(() => c.memory.grow(1), RangeError);
  const b = before.layoutBytes;
  assert.equal(b.logicalTotal, b.engine + b.graph);
  assert.ok(b.engine >= b.rules + b.state + b.decision + b.history);
});

test('replaying the same CLI operations produces identical snapshots and journal', async () => {
  const trace = ['jump', 'step 8', 'jump', 'rules double 10 on', 'jump', 'step 60', 'rules ground 8 off', 'jump'];
  const snapshots = [];
  for (let i = 0; i < 2; i++) {
    const c = await loadCore();
    snapshots.push(trace.map((line) => execute(c, parseCommand(line))));
  }
  assert.deepEqual(snapshots[0], snapshots[1]);
});
