// Reproducible storage ledger + exact state transitions, not a speed benchmark.
import { mkdir, readFile, writeFile } from 'node:fs/promises';
import { createHash } from 'node:crypto';
import { resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { spawnSync } from 'node:child_process';
import { arch, platform, release } from 'node:os';
import { loadCore, snapshot, storage, parseCommand, execute } from './runtime.mjs';

const root = fileURLToPath(new URL('.', import.meta.url));
const destination = process.argv[2];
if (!destination || process.argv.length !== 3) throw new Error('Usage: node measure.mjs NEW_OUTPUT_DIRECTORY');
const core = await loadCore();
const checked = spawnSync(process.execPath, [resolve(root, 'adapter.test.mjs')], { encoding: 'utf8' });
if (checked.error || checked.status !== 0) throw new Error(checked.error?.message || checked.stdout + checked.stderr);
process.stdout.write(checked.stdout + checked.stderr);

const trace = ['jump', 'step 8', 'jump', 'rules double 12 on', 'jump', 'step 60'];
const steps = trace.map((command) => ({ command, ...execute(core, parseCommand(command)) }));
const baseline = storage(core);
const imports = WebAssembly.Module.imports(await WebAssembly.compile(await readFile(resolve(root, 'web/core.wasm'))));
const hashes = {};
for (const name of ['core.zig', 'web/core.wasm', 'runtime.mjs', 'cli.mjs', 'adapter.test.mjs', 'measure.mjs', 'manage.py', 'qa-browser.js', 'web/app.js', 'web/index.html', 'web/style.css']) {
  const data = await readFile(resolve(root, name));
  hashes[name] = { bytes: data.length, sha256: createHash('sha256').update(data).digest('hex') };
}
let build = null;
try {
  build = JSON.parse(await readFile(resolve(root, 'build/build.json'), 'utf8'));
  if (build.source_sha256 !== hashes['core.zig'].sha256 || build.wasm_sha256 !== hashes['web/core.wasm'].sha256) {
    throw new Error('Build record and current source/binary differ. Rebuild first.');
  }
} catch (error) {
  if (error.code !== 'ENOENT') throw error;
}

const result = {
  recordedUtc: new Date().toISOString(),
  host: { platform: platform(), release: release(), arch: arch(), node: process.version },
  scope: 'Declared Zig type layouts and actual Wasm memory capacity; excludes JS/DOM/renderer, code pages and RSS. No latency or throughput measurement.',
  imports, storage: baseline, trace: steps,
  finalState: snapshot(core), build, files: hashes,
  adapterTests: { command: 'node prototypes/live-rules/adapter.test.mjs', exitCode: checked.status,
    output: checked.stdout + checked.stderr },
};
await mkdir(destination, { recursive: false });
await writeFile(resolve(destination, 'storage-and-replay.json'), JSON.stringify(result, null, 2) + '\n');
console.log(`Saved ${resolve(destination, 'storage-and-replay.json')}`);
