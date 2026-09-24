#!/usr/bin/env node
// Build tool only: reuse the existing authoring compiler. Its parser, JS runtime
// and source syntax do not become dependencies of the resulting native app.
import { readFile, writeFile } from 'node:fs/promises';
import { createHash } from 'node:crypto';
import { compile } from '../web/compiler.mjs';

const [input, output] = process.argv.slice(2);
if (!input || !output || process.argv.length !== 4) throw new Error('prepare.mjs SOURCE.pure OUTPUT.c');
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
text += declaration('PureScalarInstruction', 'instructions', p.instructions.map(i => `{${i.op}, ${i.arg}, ${number(i.value)}}`));
text += declaration('PureScalarHandler', 'handlers', p.handlers.map(h => `{${h.event}, ${h.start}, ${h.length}, ${h.parameterBase ?? 0}, ${h.parameterCount ?? 0}}`));
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
text += `\nconst PureScalarPlan pure_app_plan = {\n    ${p.states.length}, ${p.inputs.length}, ${p.instructions.length}, ${p.handlers.length}, ${p.pointerTargets.length}, ${p.views.length},\n    states, inputs, instructions, handlers, targets, views,\n    {${[...identity].join(', ')}}\n};\n`;
await writeFile(output, text);
