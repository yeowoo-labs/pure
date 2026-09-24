#!/usr/bin/env node
// Build tool only: reuse the existing authoring compiler. Its parser, JS runtime
// and source syntax do not become dependencies of the resulting native app.
import { readFile, writeFile } from 'node:fs/promises';
import { createHash } from 'node:crypto';
import { compile } from '../web/compiler.mjs';

const [input, output, profile] = process.argv.slice(2);
if (!input || !output || ![4, 5].includes(process.argv.length))
  throw new Error('prepare.mjs SOURCE.pure OUTPUT.c [FIXED_PROFILE.zig]');
const source = await readFile(input, 'utf8');
const p = compile(source);
// Encode strings as byte escapes, not JSON/C escape substitution. No source text
// or user-authored identifier can become executable C through this preparation.
const string = s => '"' + [...Buffer.from(s ?? '', 'utf8')].map(b => `\\${b.toString(8).padStart(3, '0')}`).join('') + '"';
const number = n => Object.is(n, -0) ? '-0.0' : Number.isFinite(n) ? String(n) : (() => { throw new Error('nonfinite prepared constant'); })();
const declaration = (type, name, rows) => `static const ${type} ${name}[] = {\n${rows.length ? rows.map(r => `    ${r},`).join('\n') : '    {0},'}\n};\n`;
let text = '#include "plan.h"\n\n';
text += declaration('PureScalarState', 'states', p.states.map(s => `{${string(s.name)}, ${number(s.initial)}}`));
text += declaration('PureScalarInput', 'inputs', p.inputs.map(i => `{${string(i.name)}, ${string(i.key)}, ${string(i.target)}, ${i.kind === 'value' ? 'PURE_VALUE' : i.routes ? 'PURE_ROUTED' : i.target ? 'PURE_POINT' : 'PURE_KEY'}}`));
text += '#ifndef PURE_FIXED_DEFINITION\n';
text += declaration('PureScalarInstruction', 'instructions', p.instructions.map(i => `{${i.op}, ${i.arg}, ${number(i.value)}}`));
text += declaration('PureScalarHandler', 'handlers', p.handlers.map(h => `{${h.event}, ${h.start}, ${h.length}, ${h.parameterBase ?? 0}, ${h.parameterCount ?? 0}}`));
text += '#endif\n';
text += declaration('PureScalarTarget', 'targets', p.pointerTargets.map((t, index) => {
  const route = p.inputs[t.input].routes.find(r => r.route === index);
  if (!route) throw new Error('missing prepared pointer target');
  return `{${t.input}, ${t.base}, ${t.count}, ${string(route.target)}}`;
}));
text += declaration('PureScalarView', 'views', p.views.map(v => {
  const fields = Object.values(v.bindings);
  return `{${string(v.id)}, ${string(v.type)}, ${string(v.color ?? v.prefix)}, ${fields.length}, {${fields.map(b => `{${b.start}, ${b.length}}`).join(', ')}}}`;
}));
// Definition identity protects restore compatibility, not authenticity. Persisted
// values from a different definition are rejected; storage authentication is separate.
const identity = createHash('sha256').update(JSON.stringify(p)).digest();
text += '#ifdef PURE_FIXED_DEFINITION\n#define PURE_INSTRUCTIONS 0\n#define PURE_HANDLERS 0\n#else\n#define PURE_INSTRUCTIONS instructions\n#define PURE_HANDLERS handlers\n#endif\n';
text += `\nconst PureScalarPlan pure_app_plan = {\n    ${p.states.length}, ${p.inputs.length}, ${p.instructions.length}, ${p.handlers.length}, ${p.pointerTargets.length}, ${p.views.length},\n    states, inputs, PURE_INSTRUCTIONS, PURE_HANDLERS, targets, views,\n    {${[...identity].join(', ')}}\n};\n`;
await writeFile(output, text);
if (profile) {
  // A fixed CLI/peer has no source-replacement path. These values size the
  // existing VM arrays; GTK and Wasm keep the full authoring capacities.
  const limits = [
    ['states', p.states.length], ['inputs', p.inputs.length],
    ['instructions', p.instructions.length], ['handlers', p.handlers.length],
    ['pointer_targets', p.pointerTargets.length],
  ];
  // Zig rejects an index operation on a statically empty array even when a
  // preceding runtime bound check makes that operation unreachable.
  const profileText = [
    ...limits.map(([name, count]) => `pub const ${name} = ${Math.max(1, count)};`),
    ...limits.map(([name, count]) => `pub const ${name}_count = ${count};`),
    `pub const identity = [_]u8{${[...identity].join(',')}};`,
    `pub const initials = [_]f64{${p.states.map(s => number(s.initial)).join(',')}};`,
    'pub const Instruction = struct { op: u32, arg: u32, value: f64 };',
    `pub const code = [_]Instruction{${p.instructions.map(i =>
      `.{.op=${i.op},.arg=${i.arg},.value=${number(i.value)}}`).join(',')}};`,
    'pub const Handler = struct { event: u32, start: u32, length: u32, base: u8, count: u8 };',
    `pub const handlers_data = [_]Handler{${p.handlers.map(h =>
      `.{.event=${h.event},.start=${h.start},.length=${h.length},.base=${h.parameterBase ?? 0},.count=${h.parameterCount ?? 0}}`).join(',')}};`,
    'pub const Target = struct { input: u8, base: u8, count: u8 };',
    `pub const targets_data = [_]Target{${p.pointerTargets.map(t =>
      `.{.input=${t.input},.base=${t.base},.count=${t.count}}`).join(',')}};`,
    `pub const routed_inputs = ${p.pointerTargets.reduce((mask, t) => mask | (1 << t.input), 0)};`,
  ].join('\n') + '\n';
  await writeFile(profile, profileText);
}
