import test from 'node:test';
import assert from 'node:assert/strict';
import { createChargeModel } from './web/baseline.mjs';

test('reference begins with the authored states and matching scene', () => {
  const model = createChargeModel();
  assert.deepEqual(model.snapshot().states, { holding: 0, charge: 0, y: 240, velocity: 0 });
  assert.deepEqual(model.snapshot().held, []);
  assert.equal(model.snapshot().tick, 0);
  assert.equal(model.snapshot().views.find(view => view.id === 'floor').y, 261);
  assert.equal(model.snapshot().views.find(view => view.id === 'actor').radius, 20);
});

test('press, hold, release, and frame order produce exact expected values', () => {
  const model = createChargeModel();
  model.down(); model.frame(15);
  let expectedCharge = 0;
  for (let frame = 0; frame < 15; frame++) expectedCharge = Math.min(Math.max(expectedCharge + 1 / 60, 0), 1);
  assert.deepEqual(model.snapshot().states, { holding: 1, charge: expectedCharge, y: 240, velocity: 0 });
  model.up();
  const initialVelocity = -(160 + expectedCharge * 300);
  assert.deepEqual(model.snapshot().states, { holding: 0, charge: 0, y: 240, velocity: initialVelocity });
  model.frame();
  const nextVelocity = initialVelocity + 600 * (1 / 60);
  assert.deepEqual(model.snapshot().states, {
    holding: 0, charge: 0, y: 240 + nextVelocity * (1 / 60), velocity: nextVelocity,
  });
});

test('charge clamps at one and explicit floor restores rest', () => {
  const model = createChargeModel();
  model.down(); model.frame(120);
  assert.equal(model.snapshot().states.charge, 1);
  model.up(); assert.equal(model.snapshot().states.velocity, -460);
  model.frame(); assert.equal(model.snapshot().states.y, 232.5);
  model.frame(120);
  assert.deepEqual(model.snapshot().states, { holding: 0, charge: 0, y: 240, velocity: 0 });
});

test('repeat and concurrent physical sources produce only logical edges', () => {
  const model = createChargeModel();
  model.down('jump', 'keyboard'); model.frame(6);
  const charge = model.snapshot().states.charge;
  model.down('jump', 'keyboard'); model.down('jump', 'pointer');
  assert.equal(model.snapshot().states.charge, charge);
  model.up('jump', 'keyboard');
  assert.equal(model.snapshot().states.holding, 1);
  model.up('jump', 'unknown'); assert.equal(model.snapshot().states.holding, 1);
  model.up('jump', 'pointer'); assert.equal(model.snapshot().states.velocity, -(160 + charge * 300));
  const released = model.snapshot(); model.up('jump', 'pointer');
  assert.deepEqual(model.snapshot(), released);
});

test('airborne presses cannot charge or cause another jump', () => {
  const model = createChargeModel(); model.down(); model.up(); model.frame();
  const airborne = model.snapshot().states;
  model.down(); model.up();
  assert.deepEqual(model.snapshot().states, airborne);
});

test('releaseAll emits release and reset clears latches without a jump', () => {
  const model = createChargeModel();
  model.down('jump', 'keyboard'); model.down('jump', 'pointer'); model.frame(10);
  model.releaseAll(); assert.equal(model.snapshot().states.holding, 0);
  assert.ok(model.snapshot().states.velocity < 0); assert.deepEqual(model.snapshot().held, []);
  model.reset(); assert.deepEqual(model.snapshot().states, { holding: 0, charge: 0, y: 240, velocity: 0 });
  assert.equal(model.snapshot().tick, 0);
  model.down(); model.reset(); model.up(); assert.equal(model.snapshot().states.velocity, 0);
});

test('unknown inputs and invalid frame counts reject without mutation', () => {
  const model = createChargeModel(), initial = model.snapshot();
  for (const method of ['down', 'up']) assert.throws(() => model[method]('other'), /Unknown input/);
  for (const count of [-1, .5, 601, Infinity, NaN, '1']) assert.throws(() => model.frame(count), /integer/);
  model.frame(0); assert.deepEqual(model.snapshot(), initial);
  const snapshot = model.snapshot(); snapshot.states.y = -999;
  assert.equal(model.snapshot().states.y, 240);
});
