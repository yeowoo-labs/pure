import { compile } from './compiler.mjs';
import { validateDocument } from './project.mjs';

const messages = [
  '', '실행기의 용량 한도를 넘었습니다.', '실행 정의가 유효하지 않습니다.',
  '계산 스택의 범위를 벗어났습니다.', '0으로 나눌 수 없습니다.',
  '계산 결과가 유한한 수의 범위를 벗어났습니다.', '조건 분기 범위가 유효하지 않습니다.',
  '존재하지 않는 상태를 참조했습니다.', '시간 간격이 유효하지 않습니다.',
  '정의되지 않은 입력 사건입니다.',
];
const sourceLimit = 32768;

function problem(message, line = null, column = null) {
  return Object.assign(new Error(message), { line, column });
}

function freezeProgram(value) {
  if (value && typeof value === 'object') {
    for (const child of Object.values(value)) freezeProgram(child);
    Object.freeze(value);
  }
  return value;
}

export async function createRuntime(wasmBytes, evaluationOptions = {}) {
  if (wasmBytes === undefined) {
    const response = await fetch(new URL('./core.wasm', import.meta.url), { cache: 'no-store' });
    if (!response.ok) throw new Error(`실행기를 불러오지 못했습니다 (HTTP ${response.status}).`);
    wasmBytes = await response.arrayBuffer();
  }
  const module = await WebAssembly.compile(wasmBytes);
  if (WebAssembly.Module.imports(module).length) throw new Error('이 실행기는 호스트 함수 없이 동작해야 합니다.');
  const { exports: core } = await WebAssembly.instantiate(module, {});
  if (typeof core.eval_bounded !== 'function' || typeof core.abort_program !== 'function' || typeof core.dispatch_point !== 'function' || typeof core.put_restored_value !== 'function' || typeof core.commit_restored_program !== 'function')
    throw new Error('실행기 버전이 맞지 않습니다. 새로고침하거나 core.wasm을 다시 빌드하세요.');
  core.reset_all();
  let program = null;
  let appliedSource = '';
  let lastEvent = null;
  // Keep a failed event visible through its release/focus-loss cleanup.
  // The active program can clear this diagnostic by apply or explicit reset.
  let lastFailure = null;
  const held = new Map();
  const points = new Map();
  let presenting = false;
  let bindings = [];
  // One bounded publication buffer, never exposed to callers. No state mirror.
  let displayValues = null, displayValid = null;
  function available() {
    if (presenting) throw new Error('출력 전달 중에는 실행기를 다시 호출할 수 없습니다.');
  }
  let evaluationPolicy = { cacheEntries: 0, historyEntries: 8, workLimit: 2048 };

  function checked(code, line = null) {
    if (code !== 0) throw problem(messages[code] || `실행기 오류 ${code}`, line);
  }
  function inputIndex(name) {
    const index = program?.inputs.findIndex((input) => input.name === name) ?? -1;
    if (index < 0) throw new Error(`정의되지 않은 입력: ${name}`);
    return index;
  }
  function dispatch(event, name, phase, source, point = null) {
    const status = point ? core.dispatch_point(event, point.x, point.y) : core.dispatch(event, event === 0 ? 1 / 60 : 0);
    const pc = core.info(2) >>> 0;
    lastEvent = { name, phase, source, status,
      line: program?.instructions[pc]?.line ?? null, ops: core.info(3) >>> 0 };
    if (point) { lastEvent.x = point.x; lastEvent.y = point.y; }
    if (status) lastFailure = { ...lastEvent };
    return status;
  }

  function evaluateDisplay() {
    displayValues ??= new Float64Array(96);
    displayValid ??= new Uint8Array(24);
    core.begin_evaluation(evaluationPolicy.workLimit);
    let error = null, slot = 0;
    for (let i = 0; i < bindings.length; i++) {
      const view = program.views[i];
      displayValid[i] = 1;
      for (const [name, binding] of bindings[i]) {
        const value = core.eval_bounded(binding.start, binding.length);
        if (core.evaluation_info(0) === 10) {
          displayValid.fill(0);
          return { code: 101, line: view.line,
            message: '표시 계산의 작업량 한도를 넘었습니다. 한도를 늘리거나 표시식을 줄이세요.' };
        }
        if (!Number.isFinite(value) || ((name === 'width' || name === 'height' || name === 'radius') && value < 0)) {
          error ??= { line: view.line, message: `${view.id}.${name}: 표시할 수 없는 값입니다. 0 나눗셈이나 음수 크기를 확인하세요.` };
          displayValid[i] = 0;
        }
        displayValues[slot++] = value;
      }
    }
    return error;
  }

  function status(viewError) {
    const executionError = lastFailure?.status || 0;
    return {
      tick: core.info(0) >>> 0, revision: core.info(4) >>> 0,
      displayError: viewError ? { code: viewError.code || 100, ...viewError } : null,
      lastError: executionError || (viewError ? viewError.code || 100 : 0),
      errorMessage: executionError ? messages[executionError] || `실행 오류 ${executionError}` : viewError?.message || '',
      errorLine: executionError ? lastFailure.line : viewError?.line ?? null,
      lastOps: core.info(3) >>> 0, successfulEvents: core.info(9) >>> 0,
      held: [...held.keys()], lastEvent: lastEvent ? { ...lastEvent } : null,
      lastFailure: lastFailure ? { ...lastFailure } : null,
      memory: {
        linearBytes: core.memory.buffer.byteLength,
        reservedEngineBytes: core.layout(0), programReservedBytes: core.layout(1),
        instructionBytes: core.layout(2), handlerBytes: core.layout(3),
        activeIRBytes: core.layout(4), activeStateBytes: core.layout(5),
        stagingReservedBytes: core.layout(6), evalStackBytes: core.layout(7),
      },
    };
  }

  function load(next, source, restored = null) {
    const oldIds = new Map(restored ? [] : program?.states.map((state, index) => [state.name, index]) ?? []);
    checked(core.begin_program(next.states.length, next.inputs.length));
    try {
      for (let index = 0; index < next.states.length; index++) {
        const state = next.states[index];
        checked(core.put_state(index, state.initial, oldIds.get(state.name) ?? -1), state.line);
        if (restored) checked(core.put_restored_value(index, restored[index]), state.line);
      }
      for (const op of next.instructions) checked(core.put_instruction(op.op, op.arg, op.value), op.line);
      for (const handler of next.handlers) checked(core.put_handler(handler.event, handler.start, handler.length), handler.line);
      checked(restored ? core.commit_restored_program() : core.commit_program());
    } catch (error) {
      core.abort_program();
      throw error;
    }
    program = next;
    bindings = next.views.map(view => Object.entries(view.bindings));
    appliedSource = source;
    lastEvent = null;
    lastFailure = null;
  }

  const api = {
    get currentProgram() { return program; },
    configureEvaluation({ cacheEntries = evaluationPolicy.cacheEntries,
      historyEntries = evaluationPolicy.historyEntries, workLimit = evaluationPolicy.workLimit } = {}) {
      available();
      for (const [name, value, maximum] of [['cacheEntries', cacheEntries, 32],
        ['historyEntries', historyEntries, 8], ['workLimit', workLimit, 0xffffffff]]) {
        if (!Number.isInteger(value) || value < 0 || value > maximum)
          throw new Error(`${name}: 0~${maximum} 사이의 정수여야 합니다.`);
      }
      checked(core.configure_evaluation(cacheEntries, historyEntries));
      evaluationPolicy = { cacheEntries, historyEntries, workLimit };
      return { ...evaluationPolicy };
    },
    forgetEvaluation(part = 'all') {
      available();
      const flags = part === 'all' ? 3 : part === 'values' ? 1 : part === 'history' ? 2 : undefined;
      if (flags === undefined) throw new Error('삭제할 기록은 values, history, all 중 하나여야 합니다.');
      core.forget_evaluation(flags);
    },
    evaluationStats() {
      return { ...evaluationPolicy, work: core.evaluation_info(1),
        instructions: core.evaluation_info(2), cacheHits: core.evaluation_info(3),
        reservedBytes: core.evaluation_info(6), dedicatedBytes: core.evaluation_info(7),
        sharedBytes: core.evaluation_info(8), reuseAvailable: Boolean(core.evaluation_info(9)),
        workspaceTagBytes: core.evaluation_info(10) };
    },
    apply(source) {
      available();
      if (typeof source !== 'string' || new TextEncoder().encode(source).length > sourceLimit) {
        throw problem('정의는 UTF-8 기준 32,768바이트 이하의 텍스트여야 합니다.');
      }
      if (held.size) throw problem('누르고 있는 입력을 놓은 뒤 적용하세요. 현재 작업은 그대로 유지합니다.');
      const next = freezeProgram(compile(source));
      load(next, source);
      return next;
    },
    restoreDocument(document) {
      available();
      if (held.size) throw problem('누르고 있는 입력을 놓은 뒤 복원하세요.');
      const next = validateDocument(document);
      load(freezeProgram(next.program), next.source, next.values);
      return { source: next.source, draft: next.draft };
    },
    exportDocument(draft = appliedSource) {
      available();
      if (!program) throw problem('먼저 코드를 적용하세요.');
      if (held.size) throw problem('조작을 끝낸 뒤 배치를 저장하세요.');
      if (lastFailure) throw problem('실행 오류를 해결한 뒤 배치를 저장하세요.');
      if (typeof draft !== 'string' || new TextEncoder().encode(draft).length > sourceLimit)
        throw problem('편집 중인 코드는 UTF-8 32 KiB 이하여야 합니다.');
      return { format: 'pure-workbench', version: 2, source: appliedSource, draft,
        states: program.states.map((state, i) => {
          const value = core.get_state(i);
          return { name: state.name, value: Object.is(value, -0) ? '-0' : value };
        }) };
    },
    reset() {
      available();
      held.clear();
      points.clear();
      core.reset_state();
      lastEvent = null;
      lastFailure = null;
    },
    frame(count = 1) {
      available();
      if (!Number.isInteger(count) || count < 0 || count > 600) throw new Error('프레임 수는 0~600 사이의 정수여야 합니다.');
      if (!program) return 0;
      for (let i = 0; i < count; i++) {
        const code = dispatch(0, 'frame', 'frame', 'clock');
        if (code !== 0) return code;
      }
      return 0;
    },
    down(name, source = 'external') {
      available();
      const index = inputIndex(name);
      if (program.inputs[index].target) throw new Error('좌표 입력은 point(name, phase, x, y)를 사용하세요.');
      const sources = held.get(name) || new Set();
      if (sources.has(source)) return 0;
      const wasDown = sources.size > 0;
      sources.add(source);
      held.set(name, sources);
      return wasDown ? 0 : dispatch(1 + index * 2, name, 'down', source);
    },
    up(name, source = 'external') {
      available();
      const index = inputIndex(name);
      if (program.inputs[index].target) throw new Error('좌표 입력은 point(name, phase, x, y)를 사용하세요.');
      const sources = held.get(name);
      if (!sources?.delete(source)) return 0;
      if (sources.size) return 0;
      held.delete(name);
      return dispatch(2 + index * 2, name, 'up', source);
    },
    point(name, phase, x, y, source = 'external') {
      available();
      const index = inputIndex(name);
      if (!program.inputs[index].target) throw new Error('포인터에 연결된 입력이 아닙니다.');
      if (!['down', 'move', 'up', 'cancel'].includes(phase)) throw new Error('포인터 단계는 down, move, up, cancel 중 하나여야 합니다.');
      if (!Number.isFinite(x) || !Number.isFinite(y)) throw new Error('포인터 좌표는 유한한 숫자여야 합니다.');
      if (typeof source !== 'string' || source.length === 0) throw new Error('포인터 출처 이름이 필요합니다.');
      let point = points.get(name);
      if (phase === 'down') {
        if (point?.source === source) return 0;
        if (point) throw new Error('이 입력은 다른 포인터가 사용 중입니다.');
        point = {source, x, y};
        points.set(name, point);
        held.set(name, new Set([source]));
      } else {
        if (!point || point.source !== source) return 0;
        point.x = x; point.y = y;
        if (phase === 'up' || phase === 'cancel') { points.delete(name); held.delete(name); }
      }
      const event = phase === 'move' ? 17 + index : phase === 'cancel' ? 25 + index : 1 + index * 2 + (phase === 'up' ? 1 : 0);
      return dispatch(event, name, phase, source, point);
    },
    releaseAll() {
      available();
      let firstError = 0;
      for (const [name, point] of [...points]) {
        const status = api.point(name, 'cancel', point.x, point.y, point.source);
        if (!firstError) firstError = status;
      }
      for (const [name, sources] of [...held]) {
        for (const source of [...sources]) {
          const status = api.up(name, source);
          if (!firstError) firstError = status;
        }
      }
      return firstError;
    },
    exportProject() {
      return { format: 'pure-workbench', version: 1, source: appliedSource };
    },
    // Synchronous trusted output adapter. Callbacks run only after the whole
    // display has been evaluated. Runtime mutation/reentrant reads are refused.
    // Exceptions from an adapter propagate; the next call republishes all values.
    present(target) {
      available();
      presenting = true;
      try {
        const { begin, state, view, value, end } = target;
        for (const callback of [begin, state, view, value, end])
          if (typeof callback !== 'function') throw new TypeError('출력 어댑터 함수가 필요합니다.');
        const error = evaluateDisplay();
        begin.call(target, program);
        for (let i = 0; i < (program?.states.length ?? 0); i++) state.call(target, i, core.get_state(i));
        let slot = 0, visible = 0;
        for (let i = 0; i < bindings.length; i++) {
          const valid = Boolean(displayValid[i]);
          view.call(target, i, valid);
          for (const [name] of bindings[i]) {
            if (valid) value.call(target, i, name, displayValues[slot]);
            slot++;
          }
          if (valid) visible++;
        }
        end.call(target, visible);
        return status(error);
      } finally { presenting = false; }
    },
    snapshot() {
      available();
      const error = evaluateDisplay();
      const states = Object.fromEntries((program?.states ?? []).map((state, index) => [state.name, core.get_state(index)]));
      const views = [];
      let slot = 0;
      for (let i = 0; i < bindings.length; i++) {
        const view = program.views[i];
        const rendered = displayValid[i] ? { id: view.id, type: view.type, color: view.color, prefix: view.prefix } : null;
        for (const [name] of bindings[i]) {
          if (rendered) rendered[name] = displayValues[slot];
          slot++;
        }
        if (rendered) views.push(rendered);
      }
      return { states, views, ...status(error) };
    },
  };
  api.configureEvaluation(evaluationOptions);
  return Object.freeze(api);
}
