# Pure · 실행 중 규칙 바꾸기

공룡 연습 장면에서 점프 규칙을 바꾸고, 현재 상태와 입력이 허용·거절된 이유를
확인하는 첫 사용 경험 실험이다. 부딪혀도 계속 진행되며, 규칙을 적용해도 위치·속도·
진행 상태는 유지된다. **처음부터**만 규칙과 상태를 함께 초기화한다.

Zig 코어 하나를 WebAssembly로 빌드해 브라우저와 터미널에서 사용한다.
JavaScript에는 게임 물리나 별도의 규칙 실행기가 없다. 이 데모의 목적은 편집 의미와
관찰 비용을 확인하는 것이다. 일반 런타임·전체 앱 편집기나 성능 우월성을 입증한 결과는 아니다.

## 실행

저장소 루트에서 다음을 실행한다. 외부 JavaScript 패키지는 필요하지 않다.

```sh
python3 prototypes/live-rules/manage.py serve --port 8765
```

브라우저에서 <http://127.0.0.1:8765>를 연다. 포함된 `web/core.wasm`을 사용한다.
`file://`로 HTML을 직접 여는 대신 위 서버를 사용한다. 서버는 로컬 주소에만 바인딩한다.

소스 수정 후에는 [Zig 0.15.2](https://ziglang.org/download/)로 다시 빌드한다.
새 버전과 구분해 재현하기 위해 이 버전으로 고정했다. 전체 재빌드에는 인터넷이 필요하지 않다.

```sh
python3 prototypes/live-rules/manage.py build
# PATH에 없는 컴파일러를 지정할 때:
python3 prototypes/live-rules/manage.py build --zig /path/to/zig
```

빌드는 네이티브 테스트를 먼저 수행한다. 그 뒤 `wasm32-freestanding`, `ReleaseSafe`,
스택 예약 16 KiB, 초기·최대 선형 메모리 64 KiB로 빌드한다. 컴파일 캐시는 `build/`에 둔다.
버전·소스 해시·바이너리 해시·검사 출력은 `build/build.json`에 저장한다.
Wasm 파일 크기와 선형 메모리 용량은 서로 다른 값이다.

## 직접 확인할 작업

1. 시작하고 **점프**를 누른다. 공중에서 다시 누르면 왜 거절됐는지 확인한다.
2. 공중에서 일시정지한다. 점프 조건을 **두 번 점프**로 바꾸고 **규칙 적용**을 누른다.
3. 위치와 진행 상태가 그대로인지 보고 다시 점프한다. 새 규칙의 판정이 표시되는지 확인한다.
4. 점프를 비활성화해 거절 이유를 확인한 뒤 다시 활성화한다.

시간을 놓치면 정지 상태에서 점프하고 **한 프레임**씩 진행해도 된다.
정지 중 입력도 판정하지만, 실제 위치 이동은 프레임을 진행할 때 일어난다.
규칙을 바꾸기 전의 마지막 판정은 해당 규칙 버전의 기록으로 남는다.
입력과 규칙 변경은 한 프레임 처리 사이에서 순서대로 적용한다.

## 같은 코어를 터미널에서 사용

Node.js 20 이상으로 실행한다. 검증 환경은 Node 24다.

```sh
node prototypes/live-rules/cli.mjs
node prototypes/live-rules/cli.mjs --demo
node prototypes/live-rules/cli.mjs --demo --json
```

터미널에서는 시간이 저절로 흐르지 않는다. 아래 명령을 순서대로 입력하면
공중에서의 거절과 규칙 수정 후 두 번째 점프를 정확히 같은 프레임에서 관찰할 수 있다.

```text
jump
step 8
jump
rules double 12 on
jump
inspect
memory
```

`help`는 명령 안내, `quit`은 종료다. 규칙 편집은 세 값을 한꺼번에 검증해 적용한다.
지원하지 않는 값이나 잘못된 명령은 이전 규칙과 상태를 보존한 채 거절한다.

## 구조와 측정 범위

| 파일 | 역할 |
|---|---|
| `core.zig` | 실제 여섯 노드 판정 그래프, 고정 간격 물리, 규칙 변경, 상태·판정·기록 보관, 네이티브 테스트 |
| `web/` | 같은 코어에 연결하는 화면·키보드·버튼 어댑터 |
| `runtime.mjs`, `cli.mjs` | 같은 Wasm을 로드하는 터미널 어댑터 |
| `adapter.test.mjs` | 실제 Wasm과 입력 어댑터의 의미·메모리 한도 검증 |
| `measure.mjs` | 자료 구조 크기, 정확한 변경 기록, 소스·바이너리 해시 저장 |

규칙은 점프 조건·힘·활성화 세 항목이다. 입력·활성 여부·접지 여부·남은 점프 수를
읽고, 조건을 선택한 뒤 AND로 합류하는 실제 고정 그래프를 평가한다.
연산이나 연결을 자유롭게 추가하는 그래프 편집기는 아직 없다.

자료 구조 크기는 Zig의 `@sizeOf`로 계산한다. 그래프 상수는 컴파일러가 코드로
접을 수도 있으므로 이 합계를 물리 메모리 실측으로 해석하지 않는다.
Wasm 선형 메모리 64 KiB에는 코어 데이터와 스택 여유가 포함되며, 둘을 더하지 않는다.
브라우저/Node 엔진, DOM·렌더러, 코드와 프로세스 메모리는 별도이며 이번 수치에 포함하지 않는다.

가장 최근 입력의 판정과 최근 사건 16개만 보관한다. 오래된 사건을 덮어쓰는
이 기록은 완전한 실행 재생 로그가 아니다. 프레임 입력 큐를 만들어 몰래 버리는 방식도 아니다.
브라우저가 숨겨지면 일시정지하고, 프레임 처리가 밀린 경우의 시간 제한은 화면에 표시한다.
이 데모는 하드 실시간 동작을 보장하지 않는다.

## 검증 및 기록 생성

```sh
python3 prototypes/live-rules/manage.py build
node prototypes/live-rules/adapter.test.mjs
node prototypes/live-rules/measure.mjs prototypes/live-rules/results/my-run
```

측정 출력 폴더의 상위 `results/`는 있어야 하며, 이미 있는 출력 폴더를 덮어쓰지 않는다.
측정 스크립트는 처리 속도·입력 지연을 측정하지 않는다. 브라우저를 통한 실제 조작 확인은
이 자동 검사와 별도로 수행한다. 자세한 결과는 [연구 보고서 003](../../docs/research/003-live-rules.md)에 있다.

브라우저 검증용 `qa-browser.js`는 Playwright CLI의 `run-code`에 전달하는 함수 표현식이다.
서버를 실행하고 CLI로 해당 페이지를 연 뒤 다음처럼 파일 내용을 인수로 전달할 수 있다.
검증에 사용한 CLI 버전은 0.1.21, 브라우저는 Chromium 151이며 런타임의 의존성은 아니다.
Playwright가 사용하는 브라우저의 설치 또는 실행 경로 설정은 별도로 필요하다.

```sh
npx @playwright/cli -s=pure-live open http://127.0.0.1:8765
python3 - <<'PY'
from pathlib import Path
import subprocess
Path('output/playwright').mkdir(parents=True, exist_ok=True)
script = Path('prototypes/live-rules/qa-browser.js').read_text()
subprocess.run(['npx', '@playwright/cli', '-s=pure-live', 'run-code', script], check=True)
PY
```

이 검사는 화면과 상태를 초기화하고 여러 시나리오를 조작한다. 개발용 페이지에서 실행한다.
