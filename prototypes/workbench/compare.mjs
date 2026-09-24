// Reproduce the fixed charge workload in the Wasm-host runtime and handwritten JS.
// This compares full host calls, not VM instructions or feature-equivalent products.
import { readFile, writeFile } from 'node:fs/promises';
import { createHash } from 'node:crypto';
import { cpus, platform, release, arch, availableParallelism } from 'node:os';
import { performance } from 'node:perf_hooks';
import { fileURLToPath } from 'node:url';
import { createRuntime } from './web/runtime.mjs';
import { createChargeModel } from './web/baseline.mjs';

const args = process.argv.slice(2);
if (args.length && !(args.length === 2 && args[0] === '--output')) {
  throw new Error('Usage: node compare.mjs [--output report.json]');
}
const startedAt = new Date().toISOString();
const seed = 20260922;
const trials = 7;
const warmupTraces = 5;
const targetTrialMs = 12;
const absoluteTolerance = 1e-11;
const relativeTolerance = 1e-12;
const files = {
  charge: new URL('./examples/charge.pure', import.meta.url),
  baseline: new URL('./web/baseline.mjs', import.meta.url),
  runtime: new URL('./web/runtime.mjs', import.meta.url),
  compiler: new URL('./web/compiler.mjs', import.meta.url),
  wasm: new URL('./web/core.wasm', import.meta.url),
  harness: new URL('./compare.mjs', import.meta.url),
};
const contents = Object.fromEntries(await Promise.all(
  Object.entries(files).map(async ([name, path]) => [name, await readFile(path)]),
));
const hash = bytes => createHash('sha256').update(bytes).digest('hex');
const sourceInfo = name => {
  const text = contents[name].toString('utf8');
  return {
    bytes: contents[name].length,
    physicalLines: text === '' ? 0 : text.split('\n').length - Number(text.endsWith('\n')),
    nonblankLines: text.split('\n').filter(line => line.trim().length).length,
    sha256: hash(contents[name]),
  };
};

function makeTrace() {
  const trace = [];
  const action = (method, source = undefined) => trace.push({ method, ...(source === undefined ? {} : { source }) });
  const frames = count => {
    // Individual calls let correctness checks inspect every frame, not just a batch end.
    for (let i = 0; i < count; i++) action('frame');
  };
  action('reset'); action('up', 'unknown');
  action('down', 'keyboard'); action('down', 'keyboard'); frames(1);
  action('down', 'pointer:1'); frames(59); action('up', 'keyboard'); frames(10);
  action('up', 'pointer:1'); frames(6);
  action('down', 'keyboard'); frames(2); action('up', 'keyboard'); frames(120);
  action('down', 'pointer:1'); frames(600); action('releaseAll'); frames(120);
  action('down', 'keyboard'); action('reset'); action('up', 'keyboard'); frames(1);
  let random = seed >>> 0;
  const next = limit => {
    random = (Math.imul(random, 1664525) + 1013904223) >>> 0;
    return random % limit;
  };
  for (let cycle = 0; cycle < 48; cycle++) {
    const source = next(2) ? 'keyboard' : 'pointer:1';
    action('down', source); frames(next(91)); action('down', source);
    if (cycle % 3 === 0) action('down', 'pointer:2');
    frames(next(11)); action('up', source);
    if (cycle % 3 === 0) { frames(next(17)); action('up', 'pointer:2'); }
    if (cycle % 7 === 0) action('reset');
    else if (cycle % 5 === 0) action('releaseAll');
    frames(next(121));
  }
  action('releaseAll'); frames(180);
  return trace;
}

const trace = makeTrace();
function traceCounts() {
  const counts = { hostActions: trace.length, frames: 0, logicalDown: 0,
    logicalUp: 0, resets: 0, releaseAllCalls: 0, downCalls: 0, upCalls: 0 };
  const held = new Set();
  for (const action of trace) {
    if (action.method === 'frame') counts.frames++;
    else if (action.method === 'reset') { counts.resets++; held.clear(); }
    else if (action.method === 'releaseAll') {
      counts.releaseAllCalls++;
      if (held.size) counts.logicalUp++;
      held.clear();
    } else if (action.method === 'down') {
      counts.downCalls++;
      if (!held.size) counts.logicalDown++;
      held.add(action.source);
    } else {
      counts.upCalls++;
      if (held.delete(action.source) && !held.size) counts.logicalUp++;
    }
  }
  counts.dispatchedEvents = counts.frames + counts.logicalDown + counts.logicalUp;
  return counts;
}
const counts = traceCounts();
const pure = await createRuntime(contents.wasm);
const program = pure.apply(contents.charge.toString('utf8'));
const baseline = createChargeModel();
function perform(model, action) {
  switch (action.method) {
    case 'frame': return model.frame(1);
    case 'down': return model.down('jump', action.source);
    case 'up': return model.up('jump', action.source);
    case 'reset': model.reset(); return 0;
    case 'releaseAll': return model.releaseAll();
    default: throw new Error(`Unknown trace action ${action.method}`);
  }
}

let checkedNumbers = 0, maxAbsoluteDifference = 0;
function compare(actual, expected, path) {
  if (typeof expected === 'number') {
    if (!Number.isFinite(actual) || !Number.isFinite(expected)) throw new Error(`${path}: non-finite value`);
    const difference = Math.abs(actual - expected);
    maxAbsoluteDifference = Math.max(maxAbsoluteDifference, difference);
    checkedNumbers++;
    if (difference > absoluteTolerance + relativeTolerance * Math.max(Math.abs(actual), Math.abs(expected))) {
      throw new Error(`${path}: ${actual} != ${expected}`);
    }
  } else if (Array.isArray(expected)) {
    if (!Array.isArray(actual) || actual.length !== expected.length) throw new Error(`${path}: array shape mismatch`);
    expected.forEach((item, index) => compare(actual[index], item, `${path}[${index}]`));
  } else if (expected && typeof expected === 'object') {
    if (!actual || typeof actual !== 'object') throw new Error(`${path}: object missing`);
    const actualKeys = Object.keys(actual).filter(key => actual[key] !== undefined).sort();
    const expectedKeys = Object.keys(expected).filter(key => expected[key] !== undefined).sort();
    if (JSON.stringify(actualKeys) !== JSON.stringify(expectedKeys)) throw new Error(`${path}: object keys mismatch`);
    for (const key of expectedKeys) compare(actual[key], expected[key], `${path}.${key}`);
  } else if (actual !== expected) throw new Error(`${path}: ${JSON.stringify(actual)} != ${JSON.stringify(expected)}`);
}
function checkSnapshots(index) {
  const actual = pure.snapshot(), expected = baseline.snapshot();
  if (actual.lastError) throw new Error(`Action ${index}: ${actual.errorMessage}`);
  compare(actual.states, expected.states, `action[${index}].states`);
  compare(actual.views, expected.views, `action[${index}].views`);
  compare(actual.tick, expected.tick, `action[${index}].tick`);
  compare(actual.held, expected.held, `action[${index}].held`);
}
checkSnapshots(-1);
for (let index = 0; index < trace.length; index++) {
  for (const model of [pure, baseline]) {
    const code = perform(model, trace[index]);
    if (code !== 0) throw new Error(`Action ${index}: execution status ${code}`);
  }
  checkSnapshots(index);
}
const correctness = { passed: true, checkedSnapshots: trace.length + 1,
  checkedNumbers, maxAbsoluteDifference, absoluteTolerance, relativeTolerance,
  checkedFields: ['states', 'all view fields', 'tick', 'held input names'],
  traceSha256: hash(JSON.stringify(trace)), counts };
const memory = pure.snapshot().memory;

// Snapshot construction, binding evaluation, compilation, Wasm loading, and DOM
// rendering are deliberately outside these timed loops. Host dispatch is inside.
function run(model, repetitions) {
  for (let repeat = 0; repeat < repetitions; repeat++) {
    for (const action of trace) {
      const code = perform(model, action);
      if (code !== 0) throw new Error(`Timing run failed with execution status ${code}`);
    }
  }
}
const models = { pureHostWasm: pure, handwrittenJavaScript: baseline };
for (const model of Object.values(models)) run(model, warmupTraces);
const repetitions = {};
const calibration = {};
for (const [name, model] of Object.entries(models)) {
  const start = performance.now(); run(model, 3);
  const elapsedMs = performance.now() - start;
  repetitions[name] = Math.max(1, Math.min(2048, Math.ceil(targetTrialMs / Math.max(elapsedMs / 3, .001))));
  calibration[name] = { elapsedMs, traces: 3, selectedRepetitions: repetitions[name] };
}
const samples = [];
for (let trial = 0; trial < trials; trial++) {
  const order = trial % 2 === 0 ? Object.keys(models) : Object.keys(models).reverse();
  for (const [position, name] of order.entries()) {
    const start = performance.now(); run(models[name], repetitions[name]);
    const elapsedMs = performance.now() - start;
    const nsPerTrace = elapsedMs * 1e6 / repetitions[name];
    samples.push({ trial: trial + 1, position: position + 1, implementation: name,
      repetitions: repetitions[name], elapsedMs, nsPerTrace,
      nsPerHostAction: nsPerTrace / counts.hostActions,
      nsPerDispatchedEvent: nsPerTrace / counts.dispatchedEvents });
  }
}
// Each trace begins with reset. Both timing implementations must still agree.
checkSnapshots('after timing');
const summary = {};
for (const name of Object.keys(models)) {
  const values = samples.filter(sample => sample.implementation === name)
    .map(sample => sample.nsPerHostAction).sort((a, b) => a - b);
  summary[name] = { medianNsPerHostAction: values[Math.floor(values.length / 2)],
    minNsPerHostAction: values[0], maxNsPerHostAction: values.at(-1) };
}
summary.pureOverJavaScriptMedianRatio = summary.pureHostWasm.medianNsPerHostAction
  / summary.handwrittenJavaScript.medianNsPerHostAction;

const report = {
  experiment: 'Pure Workbench charge / handwritten JavaScript reference',
  environment: { startedAt, completedAt: new Date().toISOString(),
    node: process.version, v8: process.versions.v8, platform: platform(), osRelease: release(),
    architecture: arch(), cpuModel: cpus()[0]?.model ?? 'unknown',
    logicalCPUs: cpus().length, availableParallelism: availableParallelism(),
    affinityControlled: false, powerControlled: false,
    command: `node ${fileURLToPath(import.meta.url)}${args.length ? ` --output ${args[1]}` : ''}` },
  artifacts: Object.fromEntries(Object.keys(files).map(name => [name,
    { bytes: contents[name].length, sha256: hash(contents[name]) }])),
  correctness,
  sources: { chargePure: sourceInfo('charge'), fixedJavaScriptModel: sourceInfo('baseline'),
    note: 'Raw file size and line counts only; excludes HTML/host/compiler and does not establish authoring effort or DX quality.' },
  compilerMetrics: program.metrics,
  memory: { pure: memory,
    note: 'Wasm linear capacity and core layout counters; not process RSS or browser memory. No equivalent JS heap measurement collected. Do not add overlapping counters.' },
  timing: { seed, warmupTracesPerImplementation: warmupTraces,
    trialsPerImplementation: trials, targetTrialMs, calibration, traceCounts: counts,
    order: 'Alternated each trial; Pure first on odd trials, JavaScript first on even trials.',
    included: 'Host method dispatch, source-latch aggregation, state logic, and resets in the fixed trace. Pure additionally maintains generic VM/event diagnostics.',
    excluded: 'Compilation, loading, snapshots, binding evaluation, DOM rendering, scene editing, and live apply.',
    normalization: 'nsPerHostAction divides total time by calls including repeats, releaseAll and reset; nsPerDispatchedEvent also amortizes that host overhead.',
    limitations: 'Single-machine batch throughput; no CPU pinning or energy/thermal control. Seven trial averages are not event latency percentiles. Fixed JS has no live program replacement, general compiler, or state migration; not feature-equivalent whole-product timing.',
    samples, summary },
};
const json = JSON.stringify(report, null, 2) + '\n';
if (args.length) await writeFile(args[1], json, { flag: 'wx' });
process.stdout.write(json);
