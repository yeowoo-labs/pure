import test from 'node:test';
import assert from 'node:assert/strict';
import {readFile} from 'node:fs/promises';
import {compile} from './web/compiler.mjs';
import {createRuntime} from './web/runtime.mjs';
import {parseProject} from './web/project.mjs';
const wasm=await readFile(new URL('./web/core.wasm',import.meta.url));
const shared=await readFile(new URL('./examples/shared-layout.pure',import.meta.url),'utf8');
const explicit=await readFile(new URL('./examples/layout.pure',import.meta.url),'utf8');
const fresh=()=>createRuntime(wasm,{cacheEntries:32});
const named=['green','blue','amber'];

function simple(n){return 'record Cell { value=1 }\ninput tap=Space\nrule inc(t: Cell) { t.value=t.value+1 }\n'+Array.from({length:n},(_,i)=>`item cell${i}=Cell()\non tap.down use inc(cell${i})`).join('\n');}

test('binding more objects shares one VM body instead of expanding its instructions',()=>{
  for(const n of [1,2,4,8]){
    const p=compile(simple(n));
    assert.equal(p.instructions.length,4);assert.equal(p.handlers.length,n);
    assert.deepEqual(p.handlers.map(h=>h.start),Array(n).fill(0));
    assert.deepEqual(p.handlers.map(h=>[h.parameterBase,h.parameterCount]),Array.from({length:n},(_,i)=>[i,1]));
  }
  assert.equal(compile('record R { value=0 }\nrule unused(t:R) { t.value=t.value+1 }').instructions.length,0);
});

test('records and rules resolve forward names and reject invalid types fields and bindings',()=>{
  const p=compile('on tap.down use inc(a)\nitem a=R(v=4)\ninput tap=Space\nrule inc(t:R) { t.v=t.v+1 }\nrecord R { v=0 }');
  assert.equal(p.states[0].name,'a.v');assert.deepEqual([p.handlers[0].parameterBase,p.handlers[0].parameterCount],[0,1]);
  for(const code of [
    'record R {}','record R { x=0; x=1 }','record R { event_x=0 }',
    'item a=Missing()','record R {x=0}\nitem a=R(y=1)',
    'record R {x=0}\nitem a=R(x=1,x=2)','record R {x=0}\nstate a=0\nitem a=R()',
    'record R {x=0}\nrule bad(t:R) {t.missing=0}',
    'record R {x=0}\nrecord S {y=0}\nitem t=S()\nrule bad(t:R) {t.y=1}',
    'state value=0\nrecord R {x=0}\nrecord S {y=0}\nitem t=S()\nrule bad(t:R) {value=t.y}',
    'rule bad(t:Missing) {}','record R {x=0}\nitem a=R()\ninput k=Space\non k.down use missing(a)',
    'record R {x=0}\nrule r(t:R) {}\ninput k=Space\non k.down use r(missing)',
    'record R {x=0}\nrecord S {x=0}\nitem a=S()\nrule r(t:R) {}\ninput k=Space\non k.down use r(a)',
    'record R {x=0}\nitem a=R()\nrule r(t:R) {t.x=event_x}\ninput k=Space\non k.down use r(a)',
    'record R {x=0}\nitem a=R()\nrule r(t:R) {t.x=event_y}\non frame use r(a)',
    'record R {x=0}\nitem a=R()\ntext label(0,0,t.x)',
    `record R { ${Array.from({length:9},(_,i)=>`f${i}=0`).join(';')} }`,
  ]) assert.throws(()=>compile(code),code);
  assert.throws(()=>compile(simple(33)),/32 states/);
});

test('shared layout matches the existing authored layout across independent and simultaneous gestures',async()=>{
  const a=await fresh(),b=await fresh();a.apply(explicit);b.apply(shared);
  const compare=()=>{
    const old=a.snapshot(),next=b.snapshot();assert.equal(next.lastError,old.lastError);
    assert.equal(next.states.selected,old.states.selected);
    for(let i=1;i<=3;i++)for(const field of ['x','y','ox','oy','sx','sy','held'])assert.equal(next.states[`${named[i-1]}.${field}`],old.states[`${field}${i}`]);
    for(let i=0;i<old.views.length;i++)for(const field of ['x','y','width','height','value'])assert.equal(next.views[i][field],old.views[i][field]);
  };
  const point=(i,phase,x,y)=>{a.point(`drag${i}`,phase,x,y,`p${i}`);b.point(`drag_${named[i-1]}`,phase,x,y,`p${i}`);compare();};
  compare();point(1,'down',90,130);point(2,'down',290,170);point(1,'move',350,200);point(2,'move',500,300);point(2,'up',290,170);point(1,'cancel',350,200);
  for(const key of ['left','right','up','down']){a.down(key);b.down(key);a.up(key);b.up(key);compare();}
  point(3,'down',460,230);point(3,'move',900,500);point(3,'up',460,230);
  const saved=JSON.parse(JSON.stringify(b.exportDocument()));const c=await fresh();c.restoreDocument(saved);
  assert.deepEqual(c.snapshot().states,b.snapshot().states);assert.deepEqual(c.snapshot().views,b.snapshot().views);
  c.down('left');c.up('left');assert.equal(c.snapshot().states['amber.x'],b.snapshot().states['amber.x']-10);
});

test('field names retain identity across schema and instance reorder, removal and restoration',async()=>{
  const r=await fresh();
  const source='record R { x=1;y=2 }\nitem a=R(x=10)\nitem b=R(x=20)\ninput k=Space\nrule step(t:R) {t.x=t.x+1}\non k.down use step(b)';
  r.apply(source);r.down('k');r.up('k');
  r.apply(source.replace('x=1;y=2','y=2;x=1').replace('item a=R(x=10)\nitem b=R(x=20)','item b=R(x=20)\nitem a=R(x=10)'));
  r.down('k');r.up('k');assert.equal(r.snapshot().states['b.x'],22);assert.equal(r.snapshot().states['a.x'],10);
  r.apply(source.replace('item a=R(x=10)\n',''));r.down('k');r.up('k');assert.equal(r.snapshot().states['b.x'],23);assert(!Object.hasOwn(r.snapshot().states,'a.x'));
  const saved=r.exportDocument();r.apply(source);assert.equal(r.snapshot().states['a.x'],10);
  r.restoreDocument(saved);assert.equal(r.snapshot().states['b.x'],23);assert(!Object.hasOwn(r.snapshot().states,'a.x'));
});

test('bindings share one event transaction including global effects and multiple uses of one item',async()=>{
  const r=await fresh();r.apply('state total=0\nrecord R {x=8;divisor=2}\nitem a=R()\nitem b=R(divisor=0)\ninput k=Space\nrule divide(t:R) {total=total+1\nt.x=t.x/t.divisor}\non k.down use divide(a)\non k.down use divide(b)');
  r.down('k');assert.equal(r.snapshot().lastError,4);assert.equal(r.snapshot().states.total,0);assert.equal(r.snapshot().states['a.x'],8);
  r.up('k');r.apply(simple(1)+'\non tap.down use inc(cell0)');r.down('tap');r.up('tap');assert.equal(r.snapshot().states['cell0.value'],3);
});

test('bound writes invalidate affected display caches and failed edits preserve active bindings',async()=>{
  const r=await fresh();const source=simple(2)+'\ntext first(0,0,cell0.value)\ntext second(0,0,cell1.value)';r.apply(source);r.snapshot();r.snapshot();
  r.down('tap');r.up('tap');assert.deepEqual(r.snapshot().views.map(v=>v.value),[2,2]);
  const before=r.snapshot();assert.throws(()=>r.apply(source.replace('inc(cell1)','inc(missing)')));assert.deepEqual(r.snapshot(),before);
  r.down('tap');r.up('tap');assert.deepEqual(r.snapshot().views.map(v=>v.value),[3,3]);
});

test('generated qualified names cannot make a valid exported document unreadable by its own loader',async()=>{
  const field='f'.repeat(18000);
  const source=`record R { ${field}=1 }\n`+Array.from({length:32},(_,i)=>`item i${i}=R()`).join('\n');
  const r=await fresh();r.apply(source);
  const saved=JSON.stringify(r.exportDocument('\u0001'.repeat(32768)));
  r.restoreDocument(parseProject(saved));
  assert.equal(r.snapshot().states[`i31.${field}`],1);
});
