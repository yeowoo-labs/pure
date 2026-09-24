#!/usr/bin/env node
import { readFile, writeFile } from 'node:fs/promises';
import { createInterface } from 'node:readline';
import { createRuntime } from './web/runtime.mjs';
import { parseProject } from './web/project.mjs';

const help = `Pure authoring workbench · 같은 정의와 Zig 실행기
  load PATH       .pure 파일을 적용 (같은 이름의 상태 보존)
  down NAME       정의한 입력 누르기
  up NAME         정의한 입력 놓기
  point NAME PHASE X Y  좌표 입력 (down / move / up / cancel)
  point-target NAME VIEW PHASE X Y  그룹의 대상에 좌표 입력
  frame [1..600]  시간 진행, 1프레임 = 1/60초
  show            상태와 도형 출력
  limits C H W    표시 캐시(0..32)·변경 기록(0..8)·작업량(0..4294967295) 설정
  forget [PART]   표시 기록 삭제: all, values, history (상태는 유지)
  costs           마지막 표시 계산의 작업량·명령 수·캐시 적중 출력
  reset           현재 정의의 초기 상태로
  save PATH       적용한 정의를 저장
  save-project PATH  적용 코드와 현재 배치를 JSON으로 저장
  restore PATH    저장한 배치를 복원 (초안이 있으면 파일에 보존)
  help / quit
브라우저와 별도 인스턴스입니다. 임의 JavaScript는 실행하지 않습니다.`;

async function main() {
  const args = process.argv.slice(2);
  if (args.includes('--help')) { console.log(help); return; }
  if (args.length > 1) throw new Error('사용법: node cli.mjs [project.pure]');
  const runtime = await createRuntime(await readFile(new URL('./web/core.wasm', import.meta.url)));
  let savedDraft = null;
  async function loadFile(path, restoreOnly = false) {
    const content = await readFile(path, 'utf8');
    if (restoreOnly || path.toLowerCase().endsWith('.json')) {
      const project = parseProject(content);
      if (project.version === 2) {
        savedDraft = runtime.restoreDocument(project).draft;
        return;
      }
      if (restoreOnly) throw new Error('현재 배치가 포함된 version 2 파일이 필요합니다.');
      runtime.apply(project.source);
    } else runtime.apply(content);
    savedDraft = null;
  }
  if (args[0]) await loadFile(args[0]); else runtime.apply('');
  const terminal = createInterface({ input: process.stdin, output: process.stdout, terminal: Boolean(process.stdin.isTTY) });
  if (process.stdin.isTTY) console.log(help);
  terminal.setPrompt('pure> ');
  if (process.stdin.isTTY) terminal.prompt();
  for await (const line of terminal) {
    try {
      const match = /^(\S+)(?:\s+(.+))?$/.exec(line.trim());
      const name = match?.[1] || 'show';
      const arg = match?.[2];
      if (name === 'quit') { terminal.close(); break; }
      if (name === 'help') console.log(help);
      else if (name === 'costs' && !arg) console.log(JSON.stringify({ evaluation: runtime.evaluationStats() }));
      else {
        if (name === 'load' && arg) await loadFile(arg);
        else if (name === 'restore' && arg) await loadFile(arg, true);
        else if (name === 'save-project' && arg) await writeFile(arg, JSON.stringify(runtime.exportDocument(savedDraft ?? undefined), null, 2) + '\n', {flag:'wx'});
        else if (name === 'save' && arg) await writeFile(arg, runtime.exportProject().source, { flag: 'wx' });
        else if (name === 'down' && arg) runtime.down(arg, 'terminal');
        else if (name === 'up' && arg) runtime.up(arg, 'terminal');
        else if (name === 'point' && arg) {
          const parts = arg.split(/\s+/);
          if (parts.length !== 4) throw new Error('point NAME PHASE X Y 형식을 사용하세요.');
          runtime.point(parts[0], parts[1], Number(parts[2]), Number(parts[3]), 'terminal');
        } else if (name === 'point-target' && arg) {
          const parts = arg.split(/\s+/);
          if (parts.length !== 5) throw new Error('point-target NAME VIEW PHASE X Y 형식을 사용하세요.');
          runtime.point(parts[0], parts[2], Number(parts[3]), Number(parts[4]), 'terminal', parts[1]);
        } else if (name === 'frame') {
          if (arg !== undefined && !/^\d+$/.test(arg)) throw new Error('frame 1..600 정수를 입력하세요.');
          const count = arg === undefined ? 1 : Number(arg);
          if (count < 1 || count > 600) throw new Error('frame 1..600 정수를 입력하세요.');
          runtime.frame(count);
        } else if (name === 'reset' && !arg) runtime.reset();
        else if (name === 'limits' && arg) {
          const parts = arg.split(/\s+/);
          if (parts.length !== 3 || parts.some(part => !/^\d+$/.test(part)))
            throw new Error('limits C H W 정수 세 개를 입력하세요.');
          const [cacheEntries, historyEntries, workLimit] = parts.map(Number);
          runtime.configureEvaluation({ cacheEntries, historyEntries, workLimit });
        } else if (name === 'forget') runtime.forgetEvaluation(arg || 'all');
        else if (name !== 'show' || arg) throw new Error('명령 형식을 확인하세요. help로 안내를 볼 수 있습니다.');
        const result = runtime.snapshot();
        console.log(JSON.stringify(result));
        if (result.lastError && !process.stdin.isTTY) process.exitCode = 1;
      }
    } catch (error) {
      console.error(`${error.line ? `${error.line}:${error.column || 1} ` : ''}${error.message}`);
      if (!process.stdin.isTTY) process.exitCode = 1;
    }
    if (process.stdin.isTTY) terminal.prompt();
  }
}

main().catch((error) => { console.error(error.message); process.exitCode = 1; });
