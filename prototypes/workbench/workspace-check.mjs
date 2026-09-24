// Compare the archived separate-storage build with the shared-workspace build.
// This is a correctness and storage check, not another speed benchmark.
import assert from 'node:assert/strict';
import { readFile, writeFile } from 'node:fs/promises';
import { createHash } from 'node:crypto';
import { createRuntime } from './web/runtime.mjs';
import { createRuntime as createPrevious } from './results/2026-09-22-evaluation/sources/web/runtime.mjs';

const args = process.argv.slice(2);
if (args.length !== 2 || args[0] !== '--output') throw new Error('Use --output NEW.json');
const root = new URL('.', import.meta.url);
const archived = new URL('./results/2026-09-22-evaluation/sources/', root);
const sha = bytes => createHash('sha256').update(bytes).digest('hex');
const manifest = JSON.parse(await readFile(new URL('manifest.json', archived), 'utf8'));
for (const [path, expected] of Object.entries(manifest))
  assert.equal(sha(await readFile(new URL(path, archived))), expected, path);
const oldWasm = await readFile(new URL('web/core.wasm', archived));
const wasm = await readFile(new URL('web/core.wasm', root));
const record = {utc:new Date().toISOString(),kind:'workspace-reuse-correctness-and-storage',
  baselineManifestSha256:sha(await readFile(new URL('manifest.json', archived))),
  sourceSha256:{},cases:[],node:process.version,platform:process.platform,arch:process.arch};
for (const path of ['core.zig','evaluation.zig','web/runtime.mjs','web/compiler.mjs','web/core.wasm','workspace-check.mjs'])
  record.sourceSha256[path] = sha(await readFile(new URL(path,root)));

for (const name of ['counter','toggle','charge']) {
  const path = `examples/${name}.pure`, source = await readFile(new URL(path,root),'utf8');
  record.sourceSha256[path] = sha(source);
  const old = await createPrevious(oldWasm,{cacheEntries:32});
  const current = await createRuntime(wasm,{cacheEntries:32});
  const program = old.apply(source); current.apply(source);
  const input = program.inputs[0].name;
  let snapshots = 0;
  function check() {
    const before = old.snapshot(), after = current.snapshot();
    const {memory:oldMemory,...oldObservable} = before;
    const {memory:newMemory,...newObservable} = after;
    assert.deepEqual(newObservable,oldObservable);
    for (const field of ['work','instructions','cacheHits'])
      assert.equal(current.evaluationStats()[field],old.evaluationStats()[field]);
    assert.equal(oldMemory.linearBytes,newMemory.linearBytes);
    ++snapshots;
  }
  check();
  for (let i=0;i<256;i++) {
    for (const r of [old,current]) {
      if (i%23===0) {r.down(input);r.up(input);}
      if (i%31===0) r.reset();
      if (i%47===0) r.apply(source+`\n# replacement ${i}\n`);
      if (i%7===0) r.forgetEvaluation('history');
      if (i%13===0) r.forgetEvaluation('all');
      assert.equal(r.frame(),0);
    }
    check();
  }
  record.cases.push({name,snapshots,before:old.snapshot().memory,after:current.snapshot().memory,
    evaluation:current.evaluationStats()});
}
record.wasmFileBytes = {before:oldWasm.length,after:wasm.length};
await writeFile(args[1],JSON.stringify(record,null,2)+'\n',{flag:'wx'});
console.log(JSON.stringify({checkedSnapshots:record.cases.reduce((sum,c)=>sum+c.snapshots,0),
  engineBefore:record.cases[0].before.reservedEngineBytes,engineAfter:record.cases[0].after.reservedEngineBytes,
  wasmFileBytes:record.wasmFileBytes,evaluation:record.cases[0].evaluation}));
