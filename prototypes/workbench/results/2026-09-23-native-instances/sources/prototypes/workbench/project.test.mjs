import test from 'node:test';
import assert from 'node:assert/strict';
import {readFile} from 'node:fs/promises';
import {createRuntime} from './web/runtime.mjs';
import {parseProject, MAX_PROJECT_BYTES} from './web/project.mjs';
const wasm = await readFile(new URL('./web/core.wasm',import.meta.url));
const layout = await readFile(new URL('./examples/layout.pure',import.meta.url),'utf8');
const fresh = () => createRuntime(wasm,{cacheEntries:32});
const roundTrip = value => parseProject(JSON.stringify(value));

test('multiple authored placements and a separate invalid draft survive a fresh runtime restore',async()=>{
  const a=await fresh(); a.apply(layout);
  for(const [name,x,y,nx,ny] of [['drag1',90,130,180,200],['drag2',290,170,370,210],['drag3',460,230,520,280]]) {
    a.point(name,'down',x,y);a.point(name,'move',nx,ny);a.point(name,'up',x,y);
  }
  a.down('left');a.up('left');
  const before=a.snapshot(), document=roundTrip(a.exportDocument('state unfinished ='));
  assert.equal(document.version,2);assert.equal(document.source,layout);
  const b=await fresh(); b.apply('state old=99'); b.frame(5);
  const restored=b.restoreDocument(document), after=b.snapshot();
  assert.equal(restored.draft,'state unfinished =');assert.equal(restored.source,layout);
  assert.deepEqual(after.states,before.states);assert.deepEqual(after.views,before.views);
  assert.equal(after.tick,0);assert.equal(after.successfulEvents,0);assert.equal(after.lastEvent,null);assert.deepEqual(after.held,[]);
  b.down('right');b.up('right');assert.equal(b.snapshot().states.x3,before.states.x3+10);
  b.reset();assert.equal(b.snapshot().states.x1,80);assert.equal(b.snapshot().states.x2,280);assert.equal(b.snapshot().states.x3,450);
});

test('malformed files reject before changing active code, values, diagnostics or warm display records',async()=>{
  const r=await fresh();r.apply('state x=5\ntext t(0,0,x)');r.snapshot();
  const good=r.exportDocument(), before=r.snapshot(), active=r.currentProgram;
  const bad=[null,{}, {...good,version:99},{...good,draft:null},{...good,source:'state x='},{...good,states:[]},
    {...good,states:[{name:'other',value:2}]},{...good,states:[{name:'x',value:null}]},
    {...good,states:[{name:'x',value:'2'}]},{...good,states:[{name:'x',value:Infinity}]},
    {...good,states:[{name:'x',value:NaN}]},{...good,draft:'x'.repeat(32769)},
    {...good,source:'state x=0\nstate y=0',states:[{name:'x',value:1},{name:'x',value:2}]}];
  for(const doc of bad){assert.throws(()=>r.restoreDocument(doc));assert.equal(r.currentProgram,active);assert.deepEqual(r.snapshot(),before);}
  assert(r.evaluationStats().cacheHits>0);
  assert.throws(()=>parseProject('{'));assert.throws(()=>parseProject(' '.repeat(MAX_PROJECT_BYTES+1)));
});

test('restoration matches names rather than ordering and never reuses stale cached values',async()=>{
  const r=await fresh();r.apply('state x=1\nstate y=2\ntext t(0,0,x+y)');r.snapshot();r.snapshot();
  const doc=r.exportDocument();doc.states=[{name:'y',value:20},{name:'x',value:10}];
  r.restoreDocument(roundTrip(doc));assert.deepEqual(r.snapshot().states,{x:10,y:20});assert.equal(r.snapshot().views[0].value,30);
  r.apply('state y=200\nstate x=100');assert.deepEqual(r.snapshot().states,{y:20,x:10});
  r.reset();assert.deepEqual(r.snapshot().states,{y:200,x:100});
});

test('finite float values including negative zero round trip without changing authored reset values',async()=>{
  const r=await fresh();r.apply('state z=-0\nstate tiny=5e-324\nstate large=1.7976931348623157e308\nstate constructor=1');
  const doc=roundTrip(r.exportDocument());assert.equal(doc.states[0].value,'-0');
  r.restoreDocument(doc);const s=r.snapshot().states;
  assert(Object.is(s.z,-0));assert.equal(s.tiny,Number.MIN_VALUE);assert.equal(s.large,Number.MAX_VALUE);assert.equal(s.constructor,1);
});

test('held and failed interactions cannot be saved and restoration cannot steal an active input',async()=>{
  const r=await fresh();r.apply(layout);const saved=r.exportDocument();
  r.point('drag1','down',90,130);const before=r.snapshot();
  assert.throws(()=>r.exportDocument(),/조작/);assert.throws(()=>r.restoreDocument(saved),/놓은 뒤/);assert.deepEqual(r.snapshot(),before);
  r.releaseAll();r.apply('state x=0\ninput fail=Space\non fail.down { x=1/0 }');
  r.down('fail');r.up('fail');assert.throws(()=>r.exportDocument(),/오류/);
  r.restoreDocument(saved);assert.equal(r.snapshot().lastError,0);
});

test('empty and full documents restore without imposing a new source size or state count',async()=>{
  const r=await fresh();r.apply('');r.restoreDocument(roundTrip(r.exportDocument()));assert.deepEqual(r.snapshot().states,{});
  r.apply(Array.from({length:32},(_,i)=>`state s${i}=${i}`).join('\n'));const doc=r.exportDocument();
  for(const s of doc.states) s.value+=100;
  r.restoreDocument(roundTrip(doc));assert.equal(r.snapshot().states.s31,131);
  assert.equal(parseProject(JSON.stringify({format:'pure-workbench',version:1,source:'state unfinished ='})).version,1);
  assert.throws(()=>r.restoreDocument({format:'pure-workbench',version:1,source:''}));
});

test('document APIs respect the output callback boundary and export independent values',async()=>{
  const r=await fresh();r.apply('state x=1');const doc=r.exportDocument();doc.states[0].value=500;
  assert.equal(r.snapshot().states.x,1);
  r.present({begin(){assert.throws(()=>r.restoreDocument(doc),/출력 전달/);assert.throws(()=>r.exportDocument(),/출력 전달/);},state(){},view(){},value(){},end(){}});
  assert.equal(r.snapshot().states.x,1);
});
