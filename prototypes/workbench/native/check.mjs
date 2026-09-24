#!/usr/bin/env node
import assert from 'node:assert/strict';
import { readFile, writeFile, mkdir, mkdtemp, rm } from 'node:fs/promises';
import { spawnSync } from 'node:child_process';
import { createHash } from 'node:crypto';
import { tmpdir } from 'node:os';
import { resolve, join, relative } from 'node:path';
import { fileURLToPath } from 'node:url';
import { compile } from '../web/compiler.mjs';
import { createRuntime } from '../web/runtime.mjs';

const root = fileURLToPath(new URL('../', import.meta.url));
const repo = resolve(root, '../..');
const connections = resolve(root, '../connections');
const args = process.argv.slice(2);
const sanitized = args.includes('--sanitize');
const fixed = args.includes('--fixed');
const zigIndex = args.indexOf('--zig');
const zig = zigIndex >= 0 ? args[zigIndex + 1] : process.env.ZIG || 'zig';
const index = args.indexOf('--output');
const output = index >= 0 ? resolve(args[index + 1]) : null;
const temporary = await mkdtemp(join(tmpdir(), 'pure-native-parity-'));
const library = join(temporary, 'general.a');
const wasm = await readFile(resolve(root, 'web/core.wasm'));
const hash = data => createHash('sha256').update(data).digest('hex');
const compiler = process.env.CC || 'cc';
const flags = ['-std=c17', sanitized ? '-O1' : '-O2', '-g', '-Wall', '-Wextra', '-Wpedantic', '-Werror',
  '-DPURE_RUNTIME_METRICS',
  ...(sanitized ? ['-fsanitize=address,undefined', '-fno-omit-frame-pointer'] : []),
  ...(fixed ? ['-DPURE_FIXED_DEFINITION=1'] : []), '-I' + resolve(root, 'native')];
function run(command, arguments_, options = {}) {
  const p = spawnSync(command, arguments_, { encoding: 'utf8', maxBuffer: 8 * 1024 * 1024, timeout: 60000, ...options });
  assert.ifError(p.error);
  return p;
}
function successful(command, arguments_, options) {
  const p = run(command, arguments_, options);
  assert.equal(p.status, 0, p.stdout + p.stderr);
  return p.stdout;
}
const units = [resolve(root, 'native/app.c'), resolve(root, 'native/plan.c'),
  ...['runtime.c', 'evaluate.c', 'store.c', 'store-port.c'].map(name => resolve(connections, name))];
const objects = [];
const cases = [];
let unitResult;
try {
  successful(zig, ['build-lib', resolve(root, 'native.zig'), '-O', 'ReleaseSafe',
    '-fno-compiler-rt', '-femit-bin=' + library, '--cache-dir', resolve(root, 'build/cache'),
    '--global-cache-dir', resolve(root, 'build/global-cache')]);
  for (const [i, path] of units.entries()) {
    const object = join(temporary, `unit-${i}.o`);
    successful(compiler, [...flags, '-c', path, '-o', object]);
    objects.push(object);
  }
  const main = join(temporary, 'main.o');
  successful(compiler, [...flags, '-c', resolve(root, 'native/main.c'), '-o', main]);
  const sizeSource = join(temporary, 'size.c');
  await writeFile(sizeSource, '#include "scalar.h"\n#include <stdio.h>\nint main(void) { printf("%zu\\n", pure_scalar_bytes()); }\n');
  const generalSizeBinary = join(temporary, 'general-size');
  successful(compiler, [...flags, sizeSource, library, '-lm', '-o', generalSizeBinary]);
  const generalInstanceBytes = Number(successful(generalSizeBinary, []).trim());
  const definition = name => readFile(resolve(root, 'examples', name + '.pure'), 'utf8');
  const repeat = (line, n) => Array(n).fill(line);
  const definitions = [
    ['counter', await definition('counter'), ['key tap down', 'key tap down', 'key tap up', 'key tap down', 'key tap up']],
    ['toggle', await definition('toggle'), ['key toggle down', 'key toggle up', ...Array.from({length: 13}, () => ['key move down', 'key move up']).flat()]],
    ['charge', await definition('charge'), ['key jump down', ...repeat('frame', 12), 'key jump up', ...repeat('frame', 90)]],
    ['direct', await definition('direct'), ['point drag down tile 90 180', 'point drag move tile 250 220', 'point drag up tile 250 220']],
    ['routed', await definition('routed-layout'), ['point drag down green_box 90 130', 'point drag down blue_box 290 170',
      'point drag move green_box 200 210', 'point drag move blue_box 500 270', 'point drag cancel green_box 200 210',
      'point drag up blue_box 500 270', 'key left down', 'key left up']],
    ['layout', await definition('layout'), ['point drag1 down tile1 90 130', 'point drag1 move tile1 230 210',
      'point drag1 up tile1 230 210', 'key right down', 'key right up',
      'point drag3 down tile3 460 230', 'point drag3 cancel tile3 510 270']],
    ['shared-layout', await definition('shared-layout'), ['point drag_green down green_box 90 130',
      'point drag_green move green_box 230 210', 'point drag_green up green_box 230 210',
      'key right down', 'key right up', 'point drag_amber down amber_box 460 230',
      'point drag_amber cancel amber_box 510 270']],
    ['document', await definition('document'), ['value split 99', 'value split -10', 'value split 61.5', 'key reset down', 'key reset up']],
    ['atomic-error', 'state x=7\nstate y=2\ninput n=value\non n.change {x=event_value; y=1/event_value}\ntext v(1,2,y,"value: ")',
      ['value n 0', 'value n 4']],
    ['blank', '', []],
    ['literal-boundary', 'state x=-0\ninput n=value\non n.change {x=event_value}\ntext v(1,2,x,"한글 \\\" \\\\ \\n")', ['value n -0']],
  ];
  for (const [name, source, commands] of definitions) {
    const sourcePath = join(temporary, name + '.pure'), prepared = join(temporary, name + '.c'), binary = join(temporary, name);
    await writeFile(sourcePath, source);
    const profile = join(temporary, name + '.zig');
    successful(process.execPath, [resolve(root, 'native/prepare.mjs'), sourcePath, prepared,
      ...(fixed ? [profile] : [])]);
    let caseLibrary = library;
    if (fixed) {
      caseLibrary = join(temporary, name + '.a');
      successful(zig, ['build-lib', '-O', 'ReleaseSafe', '-fno-compiler-rt', '--dep', 'profile',
        '-Mroot=' + resolve(root, 'native-fixed.zig'), '-Mprofile=' + profile,
        '-femit-bin=' + caseLibrary, '--cache-dir', resolve(root, 'build/cache'),
        '--global-cache-dir', resolve(root, 'build/global-cache')]);
    }
    successful(compiler, [...flags, prepared, main, ...objects, caseLibrary, '-lm', '-o', binary]);
    const sizeBinary = join(temporary, name + '-size');
    successful(compiler, [...flags, sizeSource, caseLibrary, '-lm', '-o', sizeBinary]);
    const instanceBytes = Number(successful(sizeBinary, []).trim());
    assert(Number.isSafeInteger(instanceBytes) && instanceBytes > 0 && instanceBytes <= generalInstanceBytes);
    const native = run(binary, [join(temporary, name + '-store'), 'project'], { input: commands.concat(['save', 'quit']).join('\n') + '\n' });
    const rows = native.stdout.trim().split('\n').map(line => JSON.parse(line));
    const runtime = await createRuntime(wasm);
    const plan = compile(source);
    runtime.apply(source);
    function equivalent(row) {
      const snapshot = runtime.snapshot();
      assert.deepEqual(row.states, snapshot.states, name);
      const views = Object.fromEntries(snapshot.views.map(view => [view.id, Object.fromEntries(Object.keys(plan.views.find(v => v.id === view.id).bindings).map(key => [key, view[key]]))]));
      assert.deepEqual(row.views, views, name);
    }
    equivalent(rows[0]);
    let failures = 0;
    commands.forEach((command, i) => {
      const [verb, input, phase, target, x, y] = command.split(' ');
      let code = 0;
      if (verb === 'key') code = phase === 'down' ? runtime.down(input) : runtime.up(input);
      else if (verb === 'frame') code = runtime.frame(1);
      else if (verb === 'value') code = runtime.change(input, Number(phase));
      else if (verb === 'point') {
        const routed = plan.inputs.find(p => p.name === input).routes;
        code = runtime.point(input, phase, Number(x), Number(y), target, routed ? target : undefined);
      }
      if (code) ++failures;
      assert.equal(rows[i + 1].status === 0, code === 0, command);
      if (code) assert.equal(rows[i + 1].scalar_error, code, command);
      equivalent(rows[i + 1]);
    });
    assert.equal(rows.length, commands.length + 2);
    assert.equal(rows.at(-1).status, 0);
    assert.equal(native.status, failures ? 1 : 0, native.stderr);
    const restored = JSON.parse(successful(binary, [join(temporary, name + '-store'), 'project'], {input: 'load\nquit\n'}).trim().split('\n').at(-1));
    equivalent(restored);
    assert.equal(restored.stored_revision, '1');
    if (name === 'routed') {
      if (fixed) {
        const capacityUnit = join(temporary, 'fixed-capacity');
        successful(compiler, [...flags, prepared, resolve(root, 'native/test-fixed.c'),
          resolve(root, 'native/plan.c'), caseLibrary, '-lm', '-o', capacityUnit]);
        unitResult = JSON.parse(successful(capacityUnit, []));
      } else {
        const unit = join(temporary, 'test-native');
        const authoring = join(temporary, 'app-author.o');
        successful(compiler, [...flags, '-DPURE_NATIVE_AUTHORING=1', '-c', units[0], '-o', authoring]);
        successful(compiler, [...flags, '-DPURE_NATIVE_AUTHORING=1', prepared, resolve(root, 'native/test.c'),
          authoring, ...objects.slice(1), caseLibrary, '-lm', '-o', unit]);
        unitResult = JSON.parse(successful(unit, []));
      }
    }
    cases.push({name, commands: commands.length, failed_events: failures, states_and_views_match: true,
      instance_bytes: instanceBytes, fixed_profile_sha256: fixed ? hash(await readFile(profile)) : null,
      fresh_process_restore: true, source, source_sha256: hash(source), prepared_sha256: hash(await readFile(prepared)),
      native_binary_sha256: hash(await readFile(binary)), native_binary_bytes: (await readFile(binary)).length});
  }
  const paths = [resolve(root, 'core.zig'), resolve(root, 'engine.zig'), resolve(root, 'evaluation.zig'), resolve(root, 'native.zig'), resolve(root, 'native-fixed.zig'),
    resolve(root, 'manage.py'), ...['scalar.h','plan.h','plan.c','app.h','app.c','main.c','prepare.mjs','test.c','test-fixed.c','check.mjs'].map(name => resolve(root, 'native', name)),
    ...['compiler.mjs','runtime.mjs','project.mjs','core.wasm'].map(name => resolve(root, 'web', name)),
    ...['runtime.c','runtime.h','evaluate.c','program.h','flow.h','result.h','store.c','store.h','store-port.c','store-port.h'].map(name => resolve(connections, name))];
  const sourceHashes = Object.fromEntries(await Promise.all(paths.map(async path => [relative(repo, path), hash(await readFile(path))])));
  const result = {kind:'native-instance-and-authored-behavior-correctness-not-performance', sanitized_c_host: sanitized,
    profile: fixed ? 'fixed-definition' : 'general-authoring', general_instance_bytes: generalInstanceBytes,
    zig_mode:'ReleaseSafe; Zig object is not ASan-instrumented', compiler: successful(compiler,['--version']).split('\n')[0],
    cases, units: unitResult, source_sha256: sourceHashes, scalar_archive_sha256: hash(await readFile(library)),
    scalar_archive_size: successful('size',[library]), scalar_archive_undefined: successful('nm',['-u',library])};
  if (output) {
    await mkdir(resolve(output, '..'), {recursive: true});
    await writeFile(output, JSON.stringify(result,null,2)+'\n', {flag:'wx'});
  }
  console.log(JSON.stringify({cases:cases.length, event_comparisons:cases.reduce((n,c)=>n+c.commands,0),
    general_instance_bytes: generalInstanceBytes,
    instance_bytes: Object.fromEntries(cases.map(c => [c.name, c.instance_bytes])),
    units:unitResult, sanitized_c_host:sanitized, profile:result.profile}));
} finally {
  await rm(temporary, {recursive:true, force:true});
}
