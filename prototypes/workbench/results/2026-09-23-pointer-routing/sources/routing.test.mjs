import test from 'node:test';
import assert from 'node:assert/strict';
import {readFile} from 'node:fs/promises';
import {spawnSync} from 'node:child_process';
import {compile} from './web/compiler.mjs';
import {createRuntime} from './web/runtime.mjs';
const wasm=await readFile(new URL('./web/core.wasm',import.meta.url));
const shared=await readFile(new URL('./examples/shared-layout.pure',import.meta.url),'utf8');
const routed=await readFile(new URL('./examples/routed-layout.pure',import.meta.url),'utf8');
const fresh=()=>createRuntime(wasm,{cacheEntries:32});
const fixture=n=>`input drag=pointer {\n${Array.from({length:n},(_,i)=>`v${i}:c${i}`).join('\n')}\n}\non drag.move use move(drag.target)\nrule move(t:Cell) {t.x=event_x}\nrecord Cell {x=0}\n`+Array.from({length:n},(_,i)=>`item c${i}=Cell()\nbox v${i}(c${i}.x,0,10,10,"#123456")`).join('\n');

test('one input and one handler serve up to 24 static targets without instruction or handler expansion',async()=>{
  for(const n of [1,3,9,24]){
    const p=compile(fixture(n));assert.equal(p.inputs.length,1);assert.equal(p.handlers.length,1);
    assert.equal(p.handlers[0].length,2);assert.equal(p.pointerTargets.length,n);
    assert.equal(p.handlers[0].parameterBase,255);
    const r=await fresh();r.apply(fixture(n));
    r.point('drag','down',0,0,'p',`v${n-1}`);r.point('drag','move',37,0,'p',`v${n-1}`);r.point('drag','up',0,0,'p',`v${n-1}`);
    const s=r.snapshot();assert.equal(s.states[`c${n-1}.x`],37);
    for(let i=0;i<n-1;i++)assert.equal(s.states[`c${i}.x`],0);
    assert.equal(s.memory.pointerTargets,n);assert.equal(s.memory.pointerTargetBytes,3);
  }
});

test('routes validate forward references, exact types, unique views/items and current input target',()=>{
  const base=fixture(2);
  for(const source of [
    base.replace('v0:c0','missing:c0'),base.replace('v0:c0','v0:missing'),
    base.replace('v1:c1','v0:c1'),base.replace('v1:c1','v1:c0'),
    base.replace('c1=Cell()','c1=Other()')+'\nrecord Other {x=0}',
    base.replace('use move(drag.target)','use move(other.target)'),
    base.replace('use move(drag.target)','use move(drag.missing)'),
    base+'\ninput another=pointer(v0)',
    base+'\ninput other=pointer { v2:c0 }\nbox v2(0,0,1,1,"#000000")',
    'input empty=pointer {}',
    base.replace('on drag.move','on frame'),
    base.replace('rule move(t:Cell)','rule move(t:Other)')+'\nrecord Other {x=0}',
  ])assert.throws(()=>compile(source),source);
});

test('grouped layout matches explicit bindings through concurrent capture, partial cancel, keyboard and restore',async()=>{
  const a=await fresh(),b=await fresh();a.apply(shared);b.apply(routed);
  const compare=()=>{const x=a.snapshot(),y=b.snapshot();assert.deepEqual(y.states,x.states);assert.deepEqual(y.views,x.views);assert.equal(y.lastError,x.lastError);};
  const point=(name,phase,x,y)=>{a.point(`drag_${name}`,phase,x,y,name);b.point('drag',phase,x,y,name,`${name}_box`);compare();};
  point('green','down',90,130);point('blue','down',290,170);
  assert.equal(b.snapshot().held.length,2);
  assert.throws(()=>b.apply(routed),/놓은/);assert.throws(()=>b.exportDocument(),/조작/);
  assert.throws(()=>b.point('drag','down',0,0,'intruder','green_box'),/다른 포인터/);
  b.point('drag','move',999,999,'intruder','green_box');compare();
  point('green','move',200,210);point('blue','move',500,270);point('green','cancel',200,210);
  assert.equal(b.snapshot().held.length,1);point('blue','up',290,170);
  for(const key of ['left','up','right','down']){a.down(key);b.down(key);a.up(key);b.up(key);compare();}
  point('amber','down',460,230);point('amber','move',900,500);point('amber','up',460,230);
  const c=await fresh();c.restoreDocument(JSON.parse(JSON.stringify(b.exportDocument())));
  assert.deepEqual(c.snapshot().states,b.snapshot().states);
  for(const r of [b,c]){r.point('drag','down',570,290,'new','amber_box');r.point('drag','move',100,150,'new','amber_box');r.releaseAll();}
  assert.deepEqual(c.snapshot().states,b.snapshot().states);
  assert.throws(()=>b.point('drag','down',0,0),/도형/);
  assert.throws(()=>b.point('drag','down',0,0,'p','board'),/도형/);
  assert.throws(()=>b.down('drag'),/좌표/);
});

test('capture remains bound to its target and releaseAll cancels each target exactly once',async()=>{
  const r=await fresh();r.apply(routed+'\nstate cancelled=0\non drag.cancel {cancelled=cancelled+1}');
  r.point('drag','down',90,130,'a','green_box');r.point('drag','move',200,200,'a','green_box');
  r.point('drag','down',290,170,'b','blue_box');r.point('drag','move',400,250,'b','blue_box');
  r.point('drag','up',0,0,'a','blue_box');assert.equal(r.snapshot().held.length,2);
  r.releaseAll();r.releaseAll();const s=r.snapshot();assert.equal(s.held.length,0);
  assert.equal(s.states.cancelled,2);assert.equal(s.states['green.x'],80);assert.equal(s.states['blue.x'],280);
});

test('reorder and remove/readd rebuild routes by names without persisting physical indices',async()=>{
  const r=await fresh();r.apply(fixture(3));
  r.point('drag','down',0,0,'p','v2');r.point('drag','move',55,0,'p','v2');r.releaseAll();
  const reordered=fixture(3).replace('v0:c0\nv1:c1\nv2:c2','v2:c2\nv1:c1\nv0:c0').replace('item c0=Cell()','').replace('item c2=Cell()','item c2=Cell()\nitem c0=Cell()');
  r.apply(reordered);assert.equal(r.snapshot().states['c2.x'],55);
  r.point('drag','down',0,0,'p','v2');r.point('drag','move',66,0,'p','v2');r.releaseAll();
  assert.equal(r.snapshot().states['c2.x'],66);assert.equal(r.snapshot().states['c0.x'],0);
  const saved=r.exportDocument();const c=await fresh();c.restoreDocument(saved);assert.deepEqual(c.snapshot().states,r.snapshot().states);
  r.apply(fixture(2));r.apply(fixture(3));assert.equal(r.snapshot().states['c2.x'],0);
  r.point('drag','move',999,0,'p','v2');assert.equal(r.snapshot().states['c2.x'],0);
});

test('routed writes invalidate cached displays and failed handlers rollback without losing another target owner',async()=>{
  const source=fixture(2)+'\nstate total=0\non drag.move {total=total+1; total=1/event_y}';
  const r=await fresh();r.apply(source);r.snapshot();r.snapshot();
  r.point('drag','down',0,0,'a','v0');r.point('drag','down',0,0,'b','v1');
  assert.equal(r.point('drag','move',50,2,'a','v0'),0);assert.equal(r.snapshot().views[0].x,50);
  assert.equal(r.point('drag','move',99,0,'b','v1'),4);let s=r.snapshot();
  assert.equal(s.states['c1.x'],0);assert.equal(s.states['c0.x'],50);assert.equal(s.states.total,0.5);
  assert.equal(s.lastFailure.target,'v1');r.releaseAll();assert.equal(r.snapshot().lastFailure.status,4);
  const previous=r.currentProgram;assert.throws(()=>r.apply(source.replace('v0:c0','v0:missing')));
  assert.equal(r.currentProgram,previous);r.reset();assert.equal(r.snapshot().held.length,0);
});

test('CLI sends grouped pointer events and rejects an unknown target',()=>{
  const path=new URL('./examples/routed-layout.pure',import.meta.url).pathname;
  const run=input=>spawnSync(process.execPath,[new URL('./cli.mjs',import.meta.url).pathname,path],{input,encoding:'utf8'});
  const ok=run('point-target drag green_box down 90 130\npoint-target drag green_box move 210 220\npoint-target drag green_box up 90 130\nquit\n');
  assert.equal(ok.status,0,ok.stderr);const rows=ok.stdout.trim().split('\n').map(JSON.parse);
  assert.equal(rows.at(-1).states['green.x'],200);assert.equal(rows.at(-1).states['blue.x'],280);assert.equal(rows.at(-1).held.length,0);
  const bad=run('point-target drag missing down 0 0\nquit\n');assert.equal(bad.status,1);assert.match(bad.stderr,/도형/);
});


test('a route from a different input group cannot be delivered through the named group',async()=>{
  const source=fixture(2).replace('v1:c1','')+'\ninput second=pointer {v1:c1}\non second.move use move(second.target)';
  const r=await fresh();r.apply(source);
  assert.throws(()=>r.point('drag','down',0,0,'p','v1'),/도형/);
  assert.throws(()=>r.point('second','down',0,0,'p','v0'),/도형/);
  r.point('second','down',0,0,'p','v1');r.point('second','move',77,0,'p','v1');r.releaseAll();
  assert.equal(r.snapshot().states['c0.x'],0);assert.equal(r.snapshot().states['c1.x'],77);
});
