import test from 'node:test';
import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
import { compile } from './web/compiler.mjs';
import { createRuntime } from './web/runtime.mjs';

const wasm = await readFile(new URL('./web/core.wasm', import.meta.url));
const source = await readFile(new URL('./examples/direct.pure', import.meta.url), 'utf8');
const fresh = async () => { const r = await createRuntime(wasm, {cacheEntries:32}); r.apply(source); return r; };

test('pointer declarations resolve forward targets and reject ambiguous or contextless inputs', () => {
  const p = compile(source);
  assert.equal(p.inputs[0].target, 'tile');
  assert(p.handlers.some(h => h.event === 17)); assert(p.handlers.some(h => h.event === 25));
  assert(p.instructions.some(i => i.op === 22)); assert(p.instructions.some(i => i.op === 23));
  for (const [code, message] of [
    ['input p = pointer(missing)', /Unknown pointer target/],
    ['input p=pointer(tile)\ninput q=pointer(tile)\ntext tile(0,0,0)', /Duplicate pointer/],
    ['input p=Space\non p.move {}', /require a pointer/],
    ['input p=Space\non p.cancel {}', /require a pointer/],
    ['state x=0\non frame { x=event_x }', /only available/],
    ['text v(event_y,0,0)', /only available/],
    ['state event_x=0', /Reserved name/],
  ]) assert.throws(() => compile(code), message);
  const full = compile(Array.from({length:8}, (_, i) => `input p${i}=pointer(v${i})\ntext v${i}(0,0,0)\non p${i}.move {}\non p${i}.cancel {}`).join('\n'));
  assert.deepEqual(full.handlers.map(h => h.event), Array.from({length:8}, (_, i) => [17+i,25+i]).flat());
});

test('authored drag keeps grab offset, uses final up coordinates, and updates linked output', async () => {
  const r = await fresh();
  r.point('drag', 'down', 90, 170, 'pen');
  assert.equal(r.snapshot().states.dragging, 1);
  assert.throws(() => r.apply(''), /놓은 뒤/);
  r.point('drag', 'move', 360, 220, 'pen');
  let s = r.snapshot();
  assert.equal(s.states.x, 350); assert.equal(s.states.y, 210);
  assert.equal(s.views.find(v => v.id === 'region').value, 1);
  r.point('drag', 'up', 410, 240, 'pen');
  s = r.snapshot();
  assert.equal(s.states.x, 400); assert.equal(s.states.y, 230);
  assert.equal(s.states.dragging, 0); assert.deepEqual(s.held, []);
  assert.equal(s.successfulEvents, 3);
  assert.equal(s.lastEvent.x, 410);
  r.down('left'); r.up('left');
  assert.equal(r.snapshot().states.x, 390);
});

test('ownership, duplicate downs, stale moves, cancellation and focus cleanup cannot leave held input', async () => {
  const r = await fresh();
  r.point('drag', 'down', 90, 170, 'a');
  r.point('drag', 'down', 500, 500, 'a');
  assert.throws(() => r.point('drag', 'down', 90, 170, 'b'), /다른 포인터/);
  r.point('drag', 'move', 400, 200, 'b');
  assert.equal(r.snapshot().successfulEvents, 1);
  r.point('drag', 'move', 400, 200, 'a');
  r.releaseAll();
  const canceled = r.snapshot();
  assert.equal(canceled.states.x, 80); assert.equal(canceled.states.y, 160);
  assert.equal(canceled.states.dragging, 0); assert.equal(canceled.lastEvent.phase, 'cancel');
  assert.deepEqual(canceled.held, []);
  r.point('drag', 'cancel', 1, 1, 'a'); r.point('drag', 'move', 500, 500, 'a'); r.releaseAll();
  assert.deepEqual(r.snapshot(), canceled);
  r.point('drag', 'down', 90, 170, 'new'); r.reset();
  r.point('drag', 'up', 600, 500, 'new');
  assert.equal(r.snapshot().states.x, 80); assert.deepEqual(r.snapshot().held, []);
});

test('invalid coordinates and modes do not mutate a held gesture; arithmetic failure rolls back one event', async () => {
  const r = await fresh(); r.point('drag', 'down', 90, 170);
  const before = r.snapshot();
  for (const invoke of [() => r.point('drag','move',NaN,1), () => r.point('drag','up',1,Infinity),
    () => r.point('drag','unknown',1,2), () => r.point('left','down',1,2),
    () => r.down('drag'), () => r.up('drag'), () => r.point('drag','move',1,2,'')]) assert.throws(invoke);
  assert.deepEqual(r.snapshot(), before);
  r.releaseAll();
  r.apply(source.replace('x = clamp(event_x - grab_x, 20, 564)', 'x = event_x\n  y = 1 / 0'));
  r.point('drag', 'down', 90, 170);
  assert.equal(r.point('drag', 'move', 500, 220), 4);
  let s = r.snapshot(); assert.equal(s.states.x, 80); assert.equal(s.states.y, 160);
  assert.equal(s.lastFailure.phase, 'move');
  r.releaseAll(); s = r.snapshot(); assert.deepEqual(s.held, []); assert.equal(s.lastError, 4);
  assert.equal(s.states.dragging, 0);
});

test('independent targets share the event pipeline and publication remains non-reentrant', async () => {
  const r = await createRuntime(wasm);
  r.apply('state x=0\nstate y=0\ninput a=pointer(one)\ninput b=pointer(two)\ntext one(0,0,x)\ntext two(0,0,y)\non a.move { x=event_x }\non b.move { y=event_y }');
  r.point('a','down',1,2,'first'); r.point('b','down',3,4,'second');
  r.point('a','move',10,20,'first'); r.point('b','move',30,40,'second');
  assert.deepEqual(r.snapshot().states,{x:10,y:40});
  r.present({begin(){assert.throws(() => r.point('a','move',0,0,'first'), /출력 전달 중/);},state(){},view(){},value(){},end(){}});
  r.releaseAll(); assert.deepEqual(r.snapshot().held, []);
});

test('failing down, up and cancel never prevent terminal ownership cleanup', async () => {
  for (const phase of ['down', 'up', 'cancel']) {
    const r = await createRuntime(wasm);
    r.apply(`state x=7\ninput p=pointer(tile)\ntext tile(0,0,x)\non p.${phase} { x=event_x\nx=1/0 }`);
    const code = r.point('p','down',10,20,'owner');
    assert.equal(code, phase === 'down' ? 4 : 0);
    assert.deepEqual(r.snapshot().held, ['p']);
    if (phase === 'up') assert.equal(r.point('p','up',30,40,'owner'), 4);
    else assert.equal(r.releaseAll(), phase === 'cancel' ? 4 : 0);
    const s = r.snapshot();
    assert.equal(s.states.x, 7); assert.equal(s.lastError, 4);
    assert.equal(s.lastFailure.phase, phase); assert.deepEqual(s.held, []);
    r.apply('state x=100'); // Cleanup also unblocks definition changes.
    assert.equal(r.snapshot().states.x, 7);
  }
});
