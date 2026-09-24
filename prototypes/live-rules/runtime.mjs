// Node adapter only. All rule evaluation, physics and persistent state live in Zig.
import { readFile } from 'node:fs/promises';

export const fields = [
  'tick', 'y', 'vy', 'distance', 'obstacleX', 'passed', 'hits', 'jumpsUsed',
  'running', 'revision', 'lastReason', 'lastSource', 'totalInputs',
  'acceptedJumps', 'mode', 'impulse', 'enabled', 'grounded', 'obstacleHeight',
];
const signed = new Set(['y', 'vy', 'obstacleX']);

export async function loadCore() {
  const binary = await readFile(new URL('./web/core.wasm', import.meta.url));
  const module = await WebAssembly.compile(binary);
  if (WebAssembly.Module.imports(module).length !== 0) throw new Error('Core must not import a host runtime.');
  const instance = await WebAssembly.instantiate(module, {});
  instance.exports.reset();
  return instance.exports;
}

export function snapshot(core) {
  const state = Object.fromEntries(fields.map((name, id) => {
    const value = core.get(id);
    return [name, signed.has(name) ? value : value >>> 0];
  }));
  return {
    state,
    decision: core.node_value(0) === -1 ? null : {
      tick: core.decision_meta(0) >>> 0,
      revision: core.decision_meta(1) >>> 0,
      y: core.decision_meta(2),
      jumps: core.decision_meta(3) >>> 0,
      nodes: Array.from({ length: 6 }, (_, i) => core.node_value(i)),
    },
    history: Array.from({ length: core.history_count() }, (_, i) => ({
      tick: core.history_get(i, 0) >>> 0,
      kind: core.history_get(i, 1),
      detail: core.history_get(i, 2) >>> 0,
      revision: core.history_get(i, 3) >>> 0,
    })),
  };
}

export function storage(core) {
  const keys = ['rules', 'state', 'decision', 'history', 'engine', 'graph', 'logicalTotal'];
  return {
    layoutBytes: Object.fromEntries(keys.map((key, i) => [key, core.bytes(i)])),
    wasmLinearMemoryBytes: core.memory.buffer.byteLength,
    note: 'Type-layout sizes; constant graph may be compiled into code. Not browser heap or RSS.',
  };
}

export function parseCommand(line) {
  const tokens = line.trim().split(/\s+/);
  const name = tokens[0];
  if (!name) return { op: 'status' };
  if (['status', 'inspect', 'memory', 'reset', 'jump', 'help', 'quit'].includes(name)) {
    if (tokens.length !== 1) throw new Error(`${name}: 인수가 필요하지 않습니다.`);
    return { op: name };
  }
  if (name === 'step') {
    if (tokens.length > 2) throw new Error('사용법: step [1..600]');
    const count = tokens.length === 1 ? 1 : integer(tokens[1], 1, 600);
    return { op: 'step', count };
  }
  if (name === 'rules') {
    if (tokens.length !== 4 || !['ground', 'double'].includes(tokens[1]) || !['on', 'off'].includes(tokens[3])) {
      throw new Error('사용법: rules ground|double 6..16 on|off');
    }
    return { op: 'rules', mode: tokens[1] === 'double' ? 1 : 0,
      impulse: integer(tokens[2], 6, 16), enabled: tokens[3] === 'on' ? 1 : 0 };
  }
  throw new Error('알 수 없는 명령입니다. help를 입력하세요.');
}

function integer(text, min, max) {
  if (!/^\d+$/.test(text)) throw new Error(`${min}..${max} 범위의 정수를 입력하세요.`);
  const value = Number(text);
  if (!Number.isSafeInteger(value) || value < min || value > max) {
    throw new Error(`${min}..${max} 범위의 정수를 입력하세요.`);
  }
  return value;
}

export function execute(core, command) {
  switch (command.op) {
    case 'jump': core.input(2); break;
    case 'step':
      for (let n = 0; n < command.count; n += 1) core.step();
      break;
    case 'rules':
      if (core.apply_rules(command.mode, command.impulse, command.enabled) !== 0) {
        throw new Error('규칙 전체를 거절했습니다. 이전 규칙과 상태를 유지합니다.');
      }
      break;
    case 'reset': core.reset(); break;
    case 'status': case 'inspect': case 'memory': break;
    default: throw new Error('실행할 수 없는 명령입니다.');
  }
  return snapshot(core);
}
