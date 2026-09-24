// A bounded adoption check on existing authored files, not a platform benchmark.
import assert from 'node:assert/strict';
import { readFile, writeFile } from 'node:fs/promises';
import { createHash } from 'node:crypto';
import { platform, release, arch, cpus } from 'node:os';
import { performance } from 'node:perf_hooks';
import { createRuntime } from './web/runtime.mjs';

const args = process.argv.slice(2);
if (args.length !== 2 || args[0] !== '--output') throw new Error('Use --output NEW.json');
const root = new URL('.', import.meta.url);
const wasm = await readFile(new URL('web/core.wasm', root));
const hash = bytes => createHash('sha256').update(bytes).digest('hex');
const evidence = { purpose: 'Choose whether to enable binding reuse by default',
  utc: new Date().toISOString(), host: { platform: platform(), release: release(),
    arch: arch(), cpu: cpus()[0]?.model, node: process.version, affinityControlled: false },
  sourceSha256: {}, cases: [],
  timingScope: 'Event dispatch plus snapshot construction. Excludes loading, compilation, DOM, equality checks and statistics reads. Five batch samples are not latency percentiles.',
};
for (const path of ['web/core.wasm', 'core.zig', 'evaluation.zig', 'web/runtime.mjs',
  'web/compiler.mjs', 'evaluation-check.mjs', 'evaluation.test.mjs'])
  evidence.sourceSha256[path] = hash(await readFile(new URL(path, root)));

for (const name of ['counter', 'toggle', 'charge']) {
  const path = `examples/${name}.pure`;
  const source = await readFile(new URL(path, root), 'utf8');
  evidence.sourceSha256[path] = hash(source);
  const plain = await createRuntime(wasm), cached = await createRuntime(wasm, {cacheEntries:32});
  const program = plain.apply(source); cached.apply(source);
  const input = program.inputs[0]?.name;
  const trace = Array.from({length:100}, () => 'show');
  for (let i = 0; i < 600; i++) {
    if (input && i % 60 === 0) trace.push('down');
    if (input && i % 60 === 15) trace.push('up');
    trace.push('frame');
  }
  function action(runtime, kind) {
    if (kind === 'frame') assert.equal(runtime.frame(), 0);
    else if (kind !== 'show') assert.equal(runtime[kind](input, 'replay'), 0);
    return runtime.snapshot();
  }
  const totals = {plain:{work:0,instructions:0,cacheHits:0},cached:{work:0,instructions:0,cacheHits:0}};
  for (const kind of trace) {
    const left = action(plain, kind), right = action(cached, kind);
    assert.equal(left.lastError, 0); assert.deepEqual(right, left);
    for (const [key, runtime] of Object.entries({plain,cached})) {
      const stats = runtime.evaluationStats();
      for (const field of Object.keys(totals[key])) totals[key][field] += stats[field];
    }
  }
  function replay(runtime) {
    runtime.reset();
    for (const kind of trace) {
      if (kind === 'frame') runtime.frame();
      else if (kind !== 'show') runtime[kind](input, 'replay');
      runtime.snapshot();
    }
  }
  for (let warm = 0; warm < 3; warm++) { replay(plain); replay(cached); }
  const samples = [];
  for (let trial = 0; trial < 5; trial++) {
    for (const key of trial % 2 ? ['cached','plain'] : ['plain','cached']) {
      const runtime = key === 'plain' ? plain : cached;
      const start = performance.now(); replay(runtime);
      samples.push({trial,key,elapsedMs:performance.now()-start});
    }
    assert.deepEqual(cached.snapshot(), plain.snapshot());
  }
  const median = key => samples.filter(s=>s.key===key).map(s=>s.elapsedMs).sort((a,b)=>a-b)[2];
  evidence.cases.push({name,checkedSnapshots:trace.length, traceSha256:hash(JSON.stringify(trace)),
    totals, memory:cached.snapshot().memory, evaluation:cached.evaluationStats(), samples,
    medianMs:{plain:median('plain'),cached:median('cached')}, cachedOverPlain:median('cached')/median('plain')});
}
await writeFile(args[1], JSON.stringify(evidence,null,2)+'\n', {flag:'wx'});
for (const c of evidence.cases) console.log(JSON.stringify({name:c.name,checkedSnapshots:c.checkedSnapshots,
  totals:c.totals,medianMs:c.medianMs,cachedOverPlain:c.cachedOverPlain}));
