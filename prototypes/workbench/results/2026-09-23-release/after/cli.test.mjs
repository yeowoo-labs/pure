import test from 'node:test';
import assert from 'node:assert/strict';
import { spawnSync } from 'node:child_process';
import { mkdtemp, readFile, rm, writeFile } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { createRuntime } from './web/runtime.mjs';

const root = fileURLToPath(new URL('.', import.meta.url));
function run(input, file) {
  const result = spawnSync(process.execPath, [join(root, 'cli.mjs'), ...(file ? [file] : [])], {
    input, encoding: 'utf8', cwd: root,
  });
  if (result.error) throw result.error;
  return { ...result, snapshots: result.stdout.split('\n').filter(line => line.startsWith('{')).map(line => JSON.parse(line)) };
}

test('terminal starts empty and produces the same charge trace as the shared runtime', async () => {
  assert.deepEqual(run('show\nquit\n').snapshots[0].states, {});
  const file = join(root, 'examples/charge.pure');
  const result = run('down jump\nframe 30\nup jump\nframe 1\nshow\nquit\n', file);
  assert.equal(result.status, 0, result.stderr);
  const runtime = await createRuntime(await readFile(join(root, 'web/core.wasm')));
  runtime.apply(await readFile(file, 'utf8'));
  runtime.down('jump', 'terminal'); runtime.frame(30); runtime.up('jump', 'terminal'); runtime.frame();
  // JSON has no undefined-valued optional fields; compare the same wire form.
  assert.deepEqual(result.snapshots.at(-1), JSON.parse(JSON.stringify(runtime.snapshot())));
  assert(result.snapshots.at(-1).states.y < 240);
});

test('invalid terminal commands do not alter state and fail piped execution', () => {
  const result = run('show\nframe 0\nframe -1\nframe 1.5\nframe 1e2\nframe 601\nshow extra\ndown unknown\nshow\nquit\n', join(root, 'examples/counter.pure'));
  assert.equal(result.status, 1);
  assert.deepEqual(result.snapshots[0], result.snapshots[1]);
  assert.equal(result.stderr.trim().split('\n').length, 7);
});

test('terminal replays authored pointer movement and cancellation through the shared runtime', async () => {
  const file = join(root, 'examples/direct.pure');
  const result = run('point drag down 90 170\npoint drag move 360 220\npoint drag up 410 240\npoint drag down 410 240\npoint drag move 500 260\npoint drag cancel 500 260\nshow\nquit\n', file);
  assert.equal(result.status, 0, result.stderr);
  const runtime = await createRuntime(await readFile(join(root, 'web/core.wasm')));
  runtime.apply(await readFile(file, 'utf8'));
  for (const [phase, x, y] of [['down',90,170],['move',360,220],['up',410,240],['down',410,240],['move',500,260],['cancel',500,260]])
    runtime.point('drag', phase, x, y, 'terminal');
  assert.deepEqual(result.snapshots.at(-1), JSON.parse(JSON.stringify(runtime.snapshot())));
  assert.equal(result.snapshots.at(-1).states.x, 350);
  assert.equal(result.snapshots.at(-1).states.y, 210);
  assert.deepEqual(result.snapshots.at(-1).held, []);
});

test('file load preserves by name, rejected load preserves active work, and save never overwrites', async t => {
  const directory = await mkdtemp(join(tmpdir(), 'pure-author-cli-'));
  t.after(() => rm(directory, { recursive: true, force: true }));
  const first = join(directory, 'first.pure'), second = join(directory, 'second with space.pure');
  const invalid = join(directory, 'invalid.pure'), saved = join(directory, 'saved.pure');
  await writeFile(first, 'state n = 7');
  await writeFile(second, 'state n = 100\nstate added = 9');
  await writeFile(invalid, 'state n = broken');
  const result = run(`load ${second}\nload ${invalid}\nshow\nsave ${saved}\nreset\nsave ${saved}\nquit\n`, first);
  assert.equal(result.status, 1);
  assert.deepEqual(result.snapshots[0].states, { n: 7, added: 9 });
  assert.deepEqual(result.snapshots[1], result.snapshots[0]);
  assert.deepEqual(result.snapshots.at(-1).states, { n: 100, added: 9 });
  assert.equal(await readFile(saved, 'utf8'), await readFile(second, 'utf8'));
  assert.match(result.stderr, /EEXIST/);
});

test('terminal exposes arithmetic rollback and retains failure through matching release', async t => {
  const directory = await mkdtemp(join(tmpdir(), 'pure-author-error-'));
  t.after(() => rm(directory, { recursive: true, force: true }));
  const file = join(directory, 'error.pure');
  await writeFile(file, 'state n = 7\ninput tap = Space\non tap.down { n = 1 / 0 }');
  const result = run('down tap\nup tap\nshow\nreset\nquit\n', file);
  assert.equal(result.status, 1);
  for (const snapshot of result.snapshots.slice(0, 3)) {
    assert.equal(snapshot.states.n, 7); assert.equal(snapshot.lastError, 4);
    assert.equal(snapshot.errorLine, 3);
  }
  assert.equal(result.snapshots[3].lastError, 0);
});

test('terminal configures and discards evaluation records and recovers from a display limit', () => {
  const result = run('limits 32 8 2048\nshow\ncosts\nforget all\nlimits 32 8 0\nlimits 32 8 2048\nshow\nquit\n', join(root, 'examples/counter.pure'));
  assert.equal(result.status, 1); // The explicit zero-budget display failed.
  assert.equal(result.stderr, '');
  const [configured, warm, costs, forgotten, limited, recovered, final] = result.snapshots;
  assert.deepEqual(warm.views, configured.views);
  assert(costs.evaluation.cacheHits > 0);
  assert.deepEqual(forgotten.states, configured.states);
  assert.deepEqual(limited.views, []);
  assert.equal(limited.lastError, 101);
  assert.deepEqual(recovered, final);
  assert.deepEqual(recovered.views, configured.views);
  assert.deepEqual(recovered.states, configured.states);
});
