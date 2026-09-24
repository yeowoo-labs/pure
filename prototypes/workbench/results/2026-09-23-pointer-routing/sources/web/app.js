import { createRuntime } from './runtime.mjs';
import { createOutput } from './output.mjs';
import { bindSceneInput } from './scene-input.mjs';
import { parseProject, MAX_PROJECT_BYTES } from './project.mjs';

const $ = id => document.getElementById(id);
const source = $('source');
const MAX_SOURCE_BYTES = 32768;
const encoder = new TextEncoder();
const output = createOutput(document);
const snippets = {
  state: 'state count = 0\nstate x = 80\n',
  input: 'input tap = Space\non tap.down {\n  count = count + 1\n}\n',
  view: 'box tile(x, 180, 40, 40, "#7ed9ae")\ntext label(40, 60, count, "Count: ")\n',
  frame: 'on frame {\n  x = x + 30 * dt\n  if x > 600 { x = 0 }\n}\n',
};
let runtime = null;
let appliedSource = '';
let running = false;
let previousTime = null;
let accumulator = 0;
let droppedTicks = 0;
let diagnosticLocation = null;
let draftGeneration = 0;
const keyboardSources = new Map();
const pointerSources = new Map();
const inputButtons = new Map();
const sceneInput = bindSceneInput($('scene'), () => runtime, safeAction);

const bytes = text => encoder.encode(text).byteLength;
const dirty = () => source.value !== appliedSource;
// DOM is the published value. Avoid rewriting unchanged diagnostics/controls.
function text(id, value) {
  const node = $(id);
  if (node.textContent !== value) node.textContent = value;
}
function pressed(node, value) {
  const next = String(value);
  if (node.getAttribute('aria-pressed') !== next) node.setAttribute('aria-pressed', next);
}
function draftStatus() {
  const changed = dirty();
  text('draft-status', changed ? '미적용 변경' : runtime?.currentProgram ? '적용됨' : '빈 작업물');
  $('draft-status').classList.toggle('dirty', changed);
}

function setDiagnostic(message, kind = '', location = null) {
  $('diagnostic-text').textContent = message;
  $('diagnostic').className = `diagnostic ${kind}`;
  diagnosticLocation = location;
  $('error-location').hidden = !location;
  source.setAttribute('aria-invalid', kind === 'error' && location ? 'true' : 'false');
}

function cursorPosition() {
  const before = source.value.slice(0, source.selectionStart);
  const lines = before.split('\n');
  text('cursor-position', `${lines.length}행 · ${lines.at(-1).length + 1}열`);
}

function draftChanged() {
  draftGeneration++;
  // Read scroll position before writes; this path runs only for source edits.
  const scrollTop = source.scrollTop;
  const count = source.value.split('\n').length;
  const size = bytes(source.value);
  text('line-numbers', Array.from({ length: count }, (_, i) => i + 1).join('\n'));
  $('line-numbers').scrollTop = scrollTop;
  text('source-size', `${size.toLocaleString('en-US')} / 32,768 bytes`);
  $('source-size').style.color = size > MAX_SOURCE_BYTES ? 'var(--danger)' : '';
  draftStatus();
  cursorPosition();
}

function pause() {
  running = false;
  previousTime = null;
  accumulator = 0;
  text('play', '▶ 실행');
  pressed($('play'), false);
}

function safeAction(action) {
  if (!runtime) return;
  try { action(); }
  catch (error) { pause(); setDiagnostic(error.message, 'error', error.line ? { line: error.line, column: error.column || 1 } : null); }
  refresh();
}

function releaseEverything() {
  keyboardSources.clear();
  pointerSources.clear();
  sceneInput.releaseCaptures();
  if (runtime) safeAction(() => runtime.releaseAll());
}

function loseFocus() {
  pause();
  releaseEverything();
  refresh();
}

function refresh() {
  if (!runtime) return;
  const snapshot = runtime.present(output);
  if (snapshot.lastError) pause();
  const held = new Set((snapshot.held || []).map(input => typeof input === 'string' ? input : input.name));
  for (const [name, button] of inputButtons) {
    button.classList.toggle('held', held.has(name));
    pressed(button, held.has(name));
  }
  text('scene-status', runtime.currentProgram ? (running ? '실행 중' : '정지') : '적용 전');
  text('clock-status', `${snapshot.tick || 0} frame · ${running ? '60 Hz' : '정지'}`);
  text('revision', `revision ${snapshot.revision || 0}`);
  for (const id of ['play', 'step', 'reset', 'download-layout']) {
    if ($(id).disabled !== !runtime.currentProgram) $(id).disabled = !runtime.currentProgram;
  }
  const event = snapshot.lastEvent;
  text('last-event', event ? `${event.name}${event.phase && event.phase !== 'frame' ? `.${event.phase}` : ''}${event.target ? ` → ${event.target}` : ''} · ${event.status ? '실패' : '완료'}${event.line ? ` · ${event.line}행` : ''}` : '아직 없음');
  if ($('runtime-error').hidden !== !snapshot.lastError) $('runtime-error').hidden = !snapshot.lastError;
  const displayError = snapshot.lastError === 100 || snapshot.lastError === 101;
  let message = snapshot.lastError ? `${displayError ? '표시 오류' : '마지막 실패'} · ${snapshot.errorLine ? `${snapshot.errorLine}행 · ` : ''}${snapshot.errorMessage || `실행 오류 ${snapshot.lastError}`}${snapshot.lastError === 101 ? ' — 이번 장면을 표시하지 않았습니다. 상태 변경은 유지됩니다.' : displayError ? ' — 해당 도형을 표시할 수 없습니다.' : ' — 실패한 이벤트의 상태 변경은 취소됐습니다. 코드를 적용하거나 초깃값으로 되돌리면 이 기록을 지웁니다.'}` : '';
  if (!displayError && snapshot.displayError?.code === 101)
    message += ` · ${snapshot.displayError.message}`;
  text('runtime-error', message);
  output.costs(snapshot, runtime.currentProgram?.metrics, droppedTicks);
}

function endPointer(event) {
  const entry = pointerSources.get(event.pointerId);
  if (!entry) return;
  event.preventDefault();
  pointerSources.delete(event.pointerId);
  safeAction(() => runtime.up(entry.name, entry.source));
}

function rebuildInputControls() {
  inputButtons.clear();
  const children = [];
  for (const input of runtime.currentProgram?.inputs || []) {
    if (input.target || input.routes) {
      const note = document.createElement('p');
      note.className = 'empty-note';
      note.textContent = `${input.name} · 장면의 ${input.target || input.routes.map(route => route.target).join(', ')}을 잡아 조작하세요.`;
      children.push(note);
      continue;
    }
    const button = document.createElement('button');
    button.type = 'button';
    button.className = 'input-button';
    button.dataset.input = input.name;
    button.setAttribute('aria-pressed', 'false');
    const name = document.createElement('span'); name.textContent = input.name;
    const key = document.createElement('kbd'); key.textContent = input.key;
    button.append(name, key);
    button.addEventListener('pointerdown', event => {
      if (event.button !== 0) return;
      event.preventDefault();
      const identity = `pointer:${event.pointerId}`;
      pointerSources.set(event.pointerId, { name: input.name, source: identity });
      button.setPointerCapture(event.pointerId);
      safeAction(() => runtime.down(input.name, identity));
    });
    button.addEventListener('pointerup', endPointer);
    button.addEventListener('pointercancel', endPointer);
    button.addEventListener('lostpointercapture', endPointer);
    // Keyboard activation and assistive technology clicks are complete taps.
    // Pointer clicks already delivered down/up via the handlers above.
    button.addEventListener('click', event => {
      if (event.detail !== 0) return;
      safeAction(() => { runtime.down(input.name, 'activation'); runtime.up(input.name, 'activation'); });
    });
    inputButtons.set(input.name, button);
    children.push(button);
  }
  if (!children.length) {
    const message = document.createElement('p');
    message.className = 'empty-note';
    message.textContent = '입력을 선언하면 여기에 버튼이 생깁니다.';
    children.push(message);
  }
  $('input-controls').replaceChildren(...children);
}

function isEditing(target) {
  return target instanceof Element && Boolean(target.closest('textarea,input,select,button,[contenteditable="true"],[role="textbox"]'));
}

$('scene').addEventListener('pointerdown', () => $('scene').focus());

document.addEventListener('keydown', event => {
  if (!runtime || event.repeat || event.isComposing || event.ctrlKey || event.metaKey || event.altKey || isEditing(event.target)) return;
  const input = runtime.currentProgram?.inputs.find(item => item.key === event.code);
  if (!input) return;
  event.preventDefault();
  if (keyboardSources.has(event.code)) return;
  keyboardSources.set(event.code, input.name);
  safeAction(() => runtime.down(input.name, `keyboard:${event.code}`));
});

document.addEventListener('keyup', event => {
  const name = keyboardSources.get(event.code);
  if (!name) return;
  event.preventDefault();
  keyboardSources.delete(event.code);
  safeAction(() => runtime.up(name, `keyboard:${event.code}`));
});

source.addEventListener('input', () => {
  draftChanged();
  if (diagnosticLocation) setDiagnostic('코드를 수정했습니다. 적용해서 다시 확인하세요.');
});
source.addEventListener('scroll', () => { $('line-numbers').scrollTop = source.scrollTop; });
for (const name of ['click', 'keyup', 'select']) source.addEventListener(name, cursorPosition);
source.addEventListener('keydown', event => {
  if (event.key === 'Tab' && !event.shiftKey && !event.ctrlKey && !event.metaKey && !event.altKey) {
    event.preventDefault();
    source.setRangeText('  ', source.selectionStart, source.selectionEnd, 'end');
    draftChanged();
  }
});

$('apply').addEventListener('click', () => {
  if (!runtime) return;
  try {
    runtime.apply(source.value);
    appliedSource = source.value;
    rebuildInputControls();
    setDiagnostic('코드를 적용했습니다. 같은 이름의 상태는 유지됩니다.', 'success');
  } catch (error) {
    const location = error.line ? { line: error.line, column: error.column || 1 } : null;
    setDiagnostic(`${location ? `${location.line}행 ${location.column}열 · ` : ''}${error.message} · 기존 장면은 그대로 유지됩니다.`, 'error', location);
  }
  draftStatus();
  refresh();
});

$('error-location').addEventListener('click', () => {
  if (!diagnosticLocation) return;
  const lines = source.value.split('\n');
  const lineIndex = Math.min(lines.length - 1, diagnosticLocation.line - 1);
  const start = lines.slice(0, lineIndex).reduce((sum, line) => sum + line.length + 1, 0);
  const at = start + Math.min(lines[lineIndex].length, diagnosticLocation.column - 1);
  source.focus();
  source.setSelectionRange(at, Math.min(at + 1, source.value.length));
  const lineHeight = parseFloat(getComputedStyle(source).lineHeight);
  source.scrollTop = Math.max(0, lineIndex * lineHeight - source.clientHeight / 3);
  $('line-numbers').scrollTop = source.scrollTop;
  cursorPosition();
});

$('play').addEventListener('click', () => {
  if (running) pause();
  else { running = true; previousTime = null; accumulator = 0; $('play').textContent = 'Ⅱ 정지'; $('play').setAttribute('aria-pressed', 'true'); }
  refresh();
});
$('step').addEventListener('click', () => { pause(); safeAction(() => runtime.frame()); });
$('reset').addEventListener('click', () => {
  pause(); releaseEverything(); droppedTicks = 0;
  safeAction(() => runtime.reset());
  setDiagnostic('적용된 코드의 초깃값으로 되돌렸습니다. 편집 중인 코드는 유지됩니다.');
});

function replaceDraft(text, message) {
  source.value = text;
  source.scrollTop = 0;
  source.setSelectionRange(0, 0);
  setDiagnostic(message);
  draftChanged();
}

function mayReplaceDraft() {
  return !source.value || window.confirm('현재 편집 중인 코드를 바꿀까요? 저장하지 않은 내용은 사라집니다. 실행 장면은 적용 전까지 유지됩니다.');
}

$('new').addEventListener('click', () => {
  if (mayReplaceDraft()) replaceDraft('', '편집기만 비웠습니다. 빈 코드를 적용하면 장면도 비워집니다.');
});

for (const button of document.querySelectorAll('.insert-snippet')) {
  button.addEventListener('click', () => {
    const at = source.selectionStart;
    const prefix = at > 0 && source.value[at - 1] !== '\n' ? '\n' : '';
    source.setRangeText(prefix + snippets[button.dataset.snippet], source.selectionStart, source.selectionEnd, 'end');
    source.focus();
    draftChanged();
    setDiagnostic('조각을 삽입했습니다. 이름과 동작을 원하는 대로 바꾸고 적용하세요.');
  });
}

$('load-example').addEventListener('click', async () => {
  if (!mayReplaceDraft()) return;
  const selected = $('example').value;
  const before = source.value;
  const button = $('load-example');
  button.disabled = true;
  try {
    const response = await fetch(`./examples/${selected}.pure`);
    if (!response.ok) throw new Error(`예제를 불러오지 못했습니다 (${response.status}).`);
    const text = await response.text();
    if (bytes(text) > MAX_SOURCE_BYTES) throw new Error('예제가 코드 크기 제한을 넘었습니다.');
    if (source.value !== before) throw new Error('불러오는 동안 코드가 바뀌었습니다. 변경 내용을 지키기 위해 불러오기를 취소했습니다.');
    replaceDraft(text, '예제를 편집기에 불러왔습니다. 아직 적용하지 않았습니다.');
  } catch (error) { setDiagnostic(error.message, 'error'); }
  finally { button.disabled = false; }
});

function download(filename, content, mime, message = '편집 중인 코드를 파일로 저장했습니다. 적용되지 않은 변경도 포함합니다.') {
  const url = URL.createObjectURL(new Blob([content], { type: mime }));
  const link = document.createElement('a');
  link.href = url; link.download = filename;
  document.body.append(link); link.click(); link.remove();
  setTimeout(() => URL.revokeObjectURL(url), 1000);
  setDiagnostic(message);
}
$('download').addEventListener('click', () => download('my-work.pure', source.value, 'text/plain;charset=utf-8'));
$('download-project').addEventListener('click', () => download('my-work.pure.json', JSON.stringify({ format: 'pure-workbench', version: 1, source: source.value }, null, 2) + '\n', 'application/json'));
$('download-layout').addEventListener('click', () => {
  try {
    if (running) throw new Error('시간 진행을 정지한 뒤 배치를 저장하세요.');
    const project = runtime.exportDocument(source.value);
    download('my-layout.pure.json', JSON.stringify(project, null, 2) + '\n', 'application/json',
      '적용된 코드·현재 배치·미적용 초안을 함께 저장했습니다.');
  } catch (error) { setDiagnostic(error.message, 'error'); }
});
$('upload').addEventListener('click', () => $('file-input').click());
$('file-input').addEventListener('change', async event => {
  const file = event.target.files[0];
  event.target.value = '';
  if (!file) return;
  try {
    if (file.size > MAX_PROJECT_BYTES) throw new Error('파일이 너무 큽니다. 코드 제한은 32 KiB입니다.');
    const before = draftGeneration;
    const active = runtime?.currentProgram;
    const events = runtime?.snapshot().successfulEvents;
    let content = await file.text();
    if (file.name.toLowerCase().endsWith('.json')) {
      const project = parseProject(content);
      if (project.version === 2) {
        if (!runtime) throw new Error('실행기가 준비되지 않았습니다.');
        if (draftGeneration !== before || runtime.currentProgram !== active || runtime.snapshot().successfulEvents !== events)
          throw new Error('파일을 읽는 동안 작업이 바뀌었습니다. 정지한 뒤 다시 불러오세요.');
        if (running) throw new Error('시간 진행을 정지한 뒤 배치를 복원하세요.');
        if ((source.value || appliedSource) && !window.confirm('현재 코드와 배치를 파일의 내용으로 복원할까요? 저장하지 않은 작업은 사라집니다.')) return;
        const restored = runtime.restoreDocument(project);
        pause(); droppedTicks = 0;
        appliedSource = restored.source;
        rebuildInputControls();
        replaceDraft(restored.draft, '배치를 복원했습니다. 미적용 초안도 보존했고, 시간은 정지 상태입니다.');
        refresh();
        return;
      }
      content = project.source;
    }
    if (bytes(content) > MAX_SOURCE_BYTES) throw new Error('코드가 32 KiB 제한을 넘었습니다.');
    if (draftGeneration !== before) throw new Error('파일을 읽는 동안 코드가 바뀌었습니다. 불러오기를 다시 시도하세요.');
    if (mayReplaceDraft()) replaceDraft(content, `${file.name}을 편집기에 불러왔습니다. 아직 적용하지 않았습니다.`);
  } catch (error) { setDiagnostic(error.message, 'error'); }
});

window.addEventListener('blur', loseFocus);
document.addEventListener('visibilitychange', () => { if (document.hidden) loseFocus(); });
window.addEventListener('beforeunload', event => {
  // Applying is not persistence: even applied code can be lost when leaving.
  if (source.value || appliedSource) { event.preventDefault(); event.returnValue = ''; }
});

function animate(time) {
  if (running && runtime) {
    if (previousTime !== null) {
      accumulator += Math.max(0, time - previousTime);
      const wanted = Math.floor(accumulator / (1000 / 60));
      if (wanted > 0) {
        const steps = Math.min(6, wanted);
        accumulator -= wanted * (1000 / 60);
        droppedTicks += wanted - steps;
        safeAction(() => runtime.frame(steps));
      }
    }
    previousTime = running ? time : null;
  }
  requestAnimationFrame(animate);
}

Object.defineProperty(window, 'pureWorkbench', {
  configurable: false,
  writable: false,
  value: Object.freeze({ snapshot: () => structuredClone({ ...(runtime?.snapshot() || {}), ready: Boolean(runtime), source: source.value, appliedSource, dirty: dirty(), running, droppedTicks, diagnostic: $('diagnostic-text').textContent }) }),
});

draftChanged();
requestAnimationFrame(animate);
try {
  runtime = await createRuntime();
  $('ready-status').textContent = '실행기 준비됨';
  $('ready-dot').classList.add('ready');
  $('apply').disabled = false;
  refresh();
} catch (error) {
  $('ready-status').textContent = '실행기 준비 실패';
  $('ready-dot').classList.add('failed');
  setDiagnostic(`실행기를 열지 못했습니다. ${error.message}`, 'error');
}
