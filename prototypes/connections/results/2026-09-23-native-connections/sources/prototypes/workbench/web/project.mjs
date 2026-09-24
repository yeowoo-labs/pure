import { compile } from './compiler.mjs';

// Two escaped source strings plus up to 32 generated item.field names. A long
// schema field can appear in every item without repeating in the source text.
export const MAX_PROJECT_BYTES = (2 * 6 + 32) * 32768 + 8192;
const encoder = new TextEncoder();
function sourceText(value, label) {
  if (typeof value !== 'string' || encoder.encode(value).length > 32768)
    throw new Error(`${label}: UTF-8 32 KiB 이하의 코드가 필요합니다.`);
  return value;
}

// No runtime mutation. Validate every saved value before opening core staging.
export function validateDocument(document) {
  if (!document || typeof document !== 'object' || Array.isArray(document) ||
      document.format !== 'pure-workbench' || document.version !== 2)
    throw new Error('배치 파일 형식이 맞지 않습니다. pure-workbench version 2가 필요합니다.');
  const source = sourceText(document.source, '적용된 코드');
  const draft = sourceText(Object.hasOwn(document, 'draft') ? document.draft : source, '편집 중인 코드');
  const program = compile(source);
  if (!Array.isArray(document.states) || document.states.length !== program.states.length)
    throw new Error('저장된 상태 목록이 적용된 코드와 일치하지 않습니다.');
  const values = new Map();
  for (const entry of document.states) {
    if (!entry || typeof entry !== 'object' || Array.isArray(entry) || typeof entry.name !== 'string' || values.has(entry.name))
      throw new Error('상태 이름이 잘못됐거나 중복됐습니다.');
    const value = entry.value === '-0' ? -0 : entry.value;
    if (typeof value !== 'number' || !Number.isFinite(value)) throw new Error('상태 값은 유한한 숫자여야 합니다.');
    values.set(entry.name, value);
  }
  for (const state of program.states)
    if (!values.has(state.name)) throw new Error(`저장된 상태가 없습니다: ${state.name}`);
  return { source, draft, program, values: program.states.map(state => values.get(state.name)) };
}

export function parseProject(text) {
  if (typeof text !== 'string' || encoder.encode(text).length > MAX_PROJECT_BYTES)
    throw new Error('작업 파일이 너무 큽니다.');
  let project;
  try { project = JSON.parse(text); } catch { throw new Error('올바른 JSON 파일이 아닙니다.'); }
  if (!project || typeof project !== 'object' || Array.isArray(project) || project.format !== 'pure-workbench')
    throw new Error('Pure 작업물 형식이 아닙니다.');
  if (project.version === 1) sourceText(project.source, '코드');
  else if (project.version === 2) validateDocument(project);
  else throw new Error('지원하지 않는 작업 파일 버전입니다.');
  return project;
}
