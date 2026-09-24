import test from 'node:test';
import assert from 'node:assert/strict';
import {readFile} from 'node:fs/promises';
import {spawnSync} from 'node:child_process';
import {compile} from './web/compiler.mjs';
import {createRuntime} from './web/runtime.mjs';
const wasm=await readFile(new URL('./web/core.wasm',import.meta.url));
const source=await readFile(new URL('./examples/document.pure',import.meta.url),'utf8');

test('numeric controls use explicit change context and cannot read pointer or frame payloads',()=>{
  const p=compile(source);assert.equal(p.states.length,1);assert.equal(p.views.length,0);assert.equal(p.handlers[0].event,33);
  for(const s of [
    'state x=0\ninput n=value\non n.down {x=1}',
    'state x=0\ninput k=Space\non k.change {x=1}',
    'state x=0\ninput n=value\non n.change {x=event_x}',
    'state x=0\non frame {x=event_value}',
    'input n=value\ntext v(0,0,event_value)',
    'record R {x=0}\nitem a=R()\ninput k=Space\nrule r(t:R){t.x=event_value}\non k.down use r(a)',
    'state event_value=0',
  ])assert.throws(()=>compile(s),s);
});

test('numeric input handles scalar rules, rollback, cache invalidation and signed zero',async()=>{
  const r=await createRuntime(wasm,{cacheEntries:32});
  const code='record R {x=10}\nitem a=R()\ninput n=value\nrule r(t:R){t.x=event_value}\non n.change use r(a)\ntext v(0,0,a.x)';
  r.apply(code);r.snapshot();r.snapshot();r.change('n',25);assert.equal(r.snapshot().views[0].value,25);
  r.change('n',-0);assert(Object.is(r.snapshot().states['a.x'],-0));assert.deepEqual(r.snapshot().held,[]);
  const before=r.snapshot();assert.throws(()=>r.change('n',Infinity));assert.throws(()=>r.down('n'));assert.throws(()=>r.point('n','down',1,2));
  assert.equal(r.snapshot().successfulEvents,before.successfulEvents);
  r.apply(code+'\non n.change {a.x=1/0}');assert.equal(r.change('n',80),4);assert(Object.is(r.snapshot().states['a.x'],-0));
  assert.equal(r.snapshot().lastFailure.value,80);assert.throws(()=>r.exportDocument(),/오류/);
  r.apply(code);const saved=r.exportDocument();const next=await createRuntime(wasm);next.restoreDocument(saved);next.change('n',32);assert.equal(next.snapshot().states['a.x'],32);
});

test('the document policy receives range, keyboard and pointer requests without layouts or text in VM state',async()=>{
  const r=await createRuntime(wasm);r.apply(source);
  for(const [value,expected] of [[60,60],[1000,80],[-1000,20],[45.5,45.5]]){assert.equal(r.change('split',value,'control'),0);assert.deepEqual(r.snapshot().states,{ratio:expected});}
  r.down('reset');r.up('reset');assert.deepEqual(r.snapshot().states,{ratio:50});
  assert.equal(r.snapshot().views.length,0);assert.equal(r.snapshot().memory.activeStateBytes,8);
  const published=[];const target={begin(){},view(){assert.fail('no declared views');},value(){assert.fail('no view values');},state(i,v){published.push(v);},end(n){assert.equal(n,0);}};
  r.present(target);assert.deepEqual(published,[50]);
  r.apply(source.replace('20, 80','30, 70'));r.change('split',10);assert.equal(r.snapshot().states.ratio,30);
  const current=r.currentProgram;assert.throws(()=>r.apply(source+'\non split.down {ratio=0}'));assert.equal(r.currentProgram,current);
});

test('CLI supplies the same numeric control value and rejects nonfinite input',()=>{
  const run=input=>spawnSync(process.execPath,[new URL('./cli.mjs',import.meta.url).pathname,new URL('./examples/document.pure',import.meta.url).pathname],{input,encoding:'utf8'});
  const ok=run('change split 95\nquit\n');assert.equal(ok.status,0,ok.stderr);assert.equal(JSON.parse(ok.stdout.trim()).states.ratio,80);
  const bad=run('change split NaN\nquit\n');assert.equal(bad.status,1);assert.match(bad.stderr,/유한/);
});
