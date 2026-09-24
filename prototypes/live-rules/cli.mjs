#!/usr/bin/env node
import { createInterface } from 'node:readline';
import { loadCore, storage, parseCommand, execute } from './runtime.mjs';

const help = `Pure · 규칙 실험실 (브라우저와 같은 Zig 코어)
  jump                       점프 입력
  step [1..600]              지정한 프레임만 진행 (기본 1)
  rules ground|double 6..16 on|off
                             점프 조건·힘·활성화를 함께 적용
  status / inspect / memory  현재 상태 / 마지막 판정·기록 / 공간
  reset                      규칙과 상태 모두 처음으로
  help / quit
예: jump → step 8 → jump → rules double 10 on → jump
시간은 step으로만 진행합니다. 실행 규칙 변경은 진행 상태를 보존합니다.`;

const reason = ['아직 입력 없음', '점프 허용', '점프 비활성', '공중: 땅에서만 가능', '두 번 모두 사용', '지원하지 않는 입력'];

async function main() {
  const args = process.argv.slice(2);
  if (args.includes('--help')) { console.log(help); return; }
  if (args.some((x) => !['--demo', '--json'].includes(x))) throw new Error('사용법: node cli.mjs [--demo] [--json]');
  const json = args.includes('--json');
  const core = await loadCore();
  function show(line) {
    const command = parseCommand(line);
    if (command.op === 'help') { console.log(help); return true; }
    if (command.op === 'quit') return false;
    const result = execute(core, command);
    if (json) {
      console.log(JSON.stringify({ command: line, ...result, ...(command.op === 'memory' ? { storage: storage(core) } : {}) }));
    } else if (command.op === 'memory') {
      console.log(JSON.stringify(storage(core), null, 2));
    } else if (command.op === 'inspect') {
      console.log(JSON.stringify(result, null, 2));
    } else {
      const s = result.state;
      console.log(`t=${s.tick} · 높이 ${(s.y / 100).toFixed(2)} · 속도 ${(s.vy / 100).toFixed(2)} · 점프 ${s.jumpsUsed}/2 · 규칙 r${s.revision} (${s.mode ? 'double' : 'ground'}, ${s.impulse}, ${s.enabled ? 'on' : 'off'}) · ${reason[s.lastReason]}`);
    }
    return true;
  }
  if (args.includes('--demo')) {
    for (const line of ['status', 'jump', 'step 8', 'jump', 'rules double 10 on', 'jump', 'step 60', 'memory']) show(line);
    return;
  }
  if (!json && process.stdin.isTTY) console.log(help);
  const terminal = createInterface({ input: process.stdin, output: process.stdout,
    terminal: Boolean(process.stdin.isTTY) });
  terminal.setPrompt('pure> ');
  if (process.stdin.isTTY) terminal.prompt();
  for await (const line of terminal) {
    try {
      if (!show(line)) { terminal.close(); break; }
    } catch (error) {
      console.error(error.message);
      if (!process.stdin.isTTY) process.exitCode = 1;
    }
    if (process.stdin.isTTY) terminal.prompt();
  }
}

main().catch((error) => {
  console.error(`${error.message}\n빌드: python3 prototypes/live-rules/manage.py build`);
  process.exitCode = 1;
});
