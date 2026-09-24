# 빈 작업물에서 시작하는 Pure

자신이 만들고 싶은 작은 동작을 작성하는 실험이다. 처음에는 코드·장면·입력이
모두 비어 있다. 상태, 입력, 도형과 동작을 추가하는 데 실행기 소스를 고칠 필요가 없다.

이 디렉터리는 브라우저·CLI 검증 호스트, 선택 가능한 네이티브 계산 모듈과 Linux GTK 작성 창이다.
Pure의 최종 목표는 앱 로직·네트워크·RPC·DB·UI를 다루는 독립 네이티브 애플리케이션 런타임이다.
UI는 그중 하나의 하위 체계이며, 현재 호스트는 전체 런타임이나 일반 네이티브 UI 스택이 아니다.
네이티브 코어 검사와 Wasm 크기도 네이티브 UI 완성이나 전체 메모리 효율의 근거가 아니다.
현재 개발 방향은 [로드맵](../../docs/research/010-roadmap.md)을 따른다.

CLI에는 기존 작업물을 실제 네이티브 서버·바이트 저장소에 저장하고 새 실행기에서 복원하는
`connect`·`push`·`pull`이 추가됐다. [서버 실행과 사용법](../connections/README.md)을 따른다.
브라우저 저장 버튼은 이 서버에 자동 연결되지 않는다.

같은 `.pure`를 **Node·브라우저 없이 실행하는 네이티브 프로그램**으로 빌드하는 경로도 있다.
빌드 시 기존 컴파일러를 재사용하며, 입력·상태 스냅샷·저장·복원을 C 실행 코어에 연결했다.
실행 전용 터미널 프로그램, 선택형 GTK 작성 창, 선택형 TLS 피어 호스트가 있다.
[빌드와 사용법](native/README.md), [작성 창의 비용과 검증](../../docs/research/025-native-authoring-shell.md),
[피어 연결과 전체 비용](../../docs/research/026-secure-peer-link.md).

## 열기

저장소 루트에서 다음 명령을 실행한다. Python 3가 필요하며, 포함된 Wasm으로 바로 열린다.

```sh
python3 prototypes/workbench/manage.py serve --port 8766
```

[제작 화면](http://127.0.0.1:8766)을 연다. 기존 Dino 실험은 8765 포트를 사용한다.
서버는 로컬 컴퓨터에서만 접속할 수 있다.

## 직접 만드는 흐름

1. 먼저 만들고 싶은 작은 동작을 정한다. 무엇을 기억하고, 어떤 입력에 어떻게 바뀔지 정하면 된다.
2. 빈 편집기에 작성한다. 화면 아래 **언어 안내**의 조각을 조합해도 된다.
3. **코드 적용**을 누르면 도형과 선언한 입력 버튼이 생긴다.
4. 버튼을 눌러보거나 장면을 클릭한 뒤 지정한 키를 누른다. `on frame`이 있다면 **실행** 또는 **한 프레임**으로 시간을 진행한다.
5. 필요한 상태·입력·도형·반응을 더 작성하고 다시 적용한다. 같은 이름의 상태 값은 이어진다.
6. **.pure 저장**으로 코드를, **배치 저장**으로 코드와 현재 배치를 보관한다. 새로고침으로 복원되지 않으므로 저장이 필요하다.

문법이 막힐 때 볼 수 있는 최소 예시다. 이 예시를 실행하는 것 자체가 제작 경험 평가를 뜻하지는 않는다.

```text
state x = 80
input right = ArrowRight
circle seed(x, 180, 20, "#7ed9ae")
on right.down {
  x = x + 20
}
```

새 반응을 추가할 때 원하는 표현을 찾을 수 있는지, 어디서 막히는지가 이번에 확인할 점이다.
관찰은 **하려던 일 / 실제로 일어난 일 / 기대했던 일** 정도면 된다.
완성된 카운터·충전·전환·직접 조작·세 도형 배치 예제는 별도로 선택해 불러오며 자동 적용하지 않는다.

## 적용·정지·오류의 의미

- 편집 중인 코드와 적용한 코드는 별개다. 저장은 **미적용 변경까지 포함한 편집기 내용**을 저장한다.
- 적용은 같은 이름의 숫자 상태를 보존한다. 새 이름은 초깃값으로 시작하고 삭제한 이름은 사라진다.
  이름 변경을 자동으로 추적하지 않는다. 초깃값만 바꿨다면 **초깃값으로**를 눌러야 현재 값도 바뀐다.
- 문법·이름·용량 오류가 있는 수정은 기존 작업을 유지한 채 거절한다. **문제 위치로**에서 해당 행으로 이동한다.
- 누르는 중에는 적용할 수 없다. 모든 입력을 놓은 뒤 적용한다.
- 정지는 시간 진행만 멈춘다. 입력 버튼은 정지 중에도 작동한다.
- 계산 실패는 해당 이벤트의 모든 상태 변경을 취소한다. 마지막 실패 안내는 놓기 동작으로 사라지지 않으며,
  수정 적용 또는 초기화로 지워진다. 잘못된 도형 크기·표시 계산은 별도의 표시 오류다.
- 편집기나 버튼에 포커스가 있으면 키 입력을 장면에 전달하지 않는다. 장면을 클릭하면 키를 사용할 수 있다.
  창에서 포커스가 벗어나면 시간을 멈추고 누른 입력을 놓는다. 키 입력의 `.up`, 직접 조작 입력의 `.cancel` 동작이 실행될 수 있다.
- 실행은 프레임당 `dt = 1/60`이다. 화면 갱신마다 최대 6프레임을 진행하며, 밀린 나머지는 건너뛰고 횟수를 표시한다.

## 표현할 수 있는 범위

상태는 숫자다. 입력은 키의 누름·놓음과 대상에 연결한 포인터 좌표, 시간은 `on frame`으로 선언한다.
출력은 사각형·원·숫자 글자이며 좌표·크기·표시 값을 상태 표현식에 연결한다.
색상은 `"#rrggbb"` 상수다. 화면 좌표는 640×360 기준이다.

```text
on right.up { x = 80 }
on frame {
  x = x + 30 * dt
  if x > 600 { x = 0 }
}
box tile(x, 220, 40, 40, "#356548")
text label(40, 60, x, "x: ")
```

연산: `+ - * /`, 비교, `&& || !`, `min`, `max`, `clamp`.
논리 연산은 양쪽을 모두 계산한다. `if`의 조건은 한 번만 계산하고 대입은 위에서 아래로 실행한다.
같은 이벤트에 여러 반응을 선언하면 작성 순서대로 실행하며 한 이벤트 전체를 함께 성공·실패 처리한다.
이벤트의 `dt`는 시간 진행에서 1/60, 입력에서는 0이다. 표시 표현식에서는 `dt`를 쓰지 않는다.

키: Space, Enter, 방향키, KeyA~KeyZ, Digit0~Digit9. 한 키는 하나의 입력에 연결한다.
주석은 `#`, 문장 구분은 줄바꿈 또는 세미콜론이다.

한도: 상태 32개, 입력 8개, 도형/글자 24개, 반응 64개, 중간 명령 512개,
계산 스택 64칸, 소스 32,768 UTF-8 바이트. 반복문·일반 함수 호출·문자열 상태·동적 배열은 없다. 매개변수 하나를 받는 공유 반응 규칙은 지원한다.
충돌·중력·점프 등의 동작은 자동으로 제공되지 않는다.
정확한 인터페이스와 명령 형식은 [CONTRACT.md](CONTRACT.md)에 있다.

## 도형을 직접 잡아 조작하기

예제 선택에서 **직접 잡아 이동 → 예제 불러오기 → 코드 적용**을 누른다.
[direct.pure](examples/direct.pure)는 잡은 위치를 유지한 이동, 영역 제한,
연결된 숫자 출력, 취소 시 복귀를 작성한 코드다. 좌우 방향키로도 위치를 바꿀 수 있다.

```text
input drag = pointer(tile)
on drag.move {
  x = clamp(event_x - grab_x, 20, 564)
  y = clamp(event_y - grab_y, 100, 276)
}
```

전체 예제에서 `.down`은 잡은 위치와 시작 위치를 기억하고, `.up`은 마지막으로 표시한 위치를
확정하며, `.cancel`은 시작 위치로 되돌린다. 이 동작은 자동 제공되는 물리가 아니다.
제한 값이나 취소 규칙을 바꾸고 다시 적용하면 동작도 바뀐다.
`event_x/event_y`는 포인터 반응 안에서만 쓸 수 있는 장면 좌표다.
서로 다른 대상은 동시에 조작할 수 있고, 같은 대상의 두 번째 접촉은 무시한다.
초점·캡처를 잃으면 `.cancel`로 끝난다. 하나의 이벤트가 실패하면 그 이벤트만 취소된다.

터미널에서도 같은 정의에 `point drag down 90 170`, `point drag move 360 220`,
`point drag up 410 240`을 보내 재현할 수 있다.
기능 검증과 범위는 [직접 조작 보고서](../../docs/research/013-authored-pointer-input.md)에 기록했다.

## 배치를 저장하고 다시 열기

**세 도형 배치 → 예제 불러오기 → 코드 적용**으로 시작한다.
세 도형을 따로 움직일 수 있고, 마지막으로 잡은 도형은 방향키로 10씩 조정한다.
시간 진행과 조작을 끝낸 뒤 **배치 저장**을 누르면 현재 상태까지 포함된 JSON을 내려받는다.
**파일 열기**로 이 파일을 선택하면 새 페이지에서도 같은 배치가 복원된다.

- 배치 파일에는 적용된 코드, 현재 숫자 상태, 아직 적용하지 않은 편집 초안이 들어 있다.
- 복원은 저장된 코드를 실행 정의로 사용한다. 미완성 초안은 편집기에 남기며 자동 적용하지 않는다.
- 조작 중인 입력과 실행 시계는 복원하지 않는다. 열린 작업은 정지 상태로 시작한다.
- **초깃값으로**는 코드의 원래 초깃값으로 돌아간다. 저장 당시 배치로 돌아가려면 파일을 다시 연다.
- **.pure 저장 / 코드 JSON 저장**은 기존처럼 초안만 저장한다. 배치 저장과 구분한다.
- 잘못된 파일이나 읽는 동안 작업이 바뀐 파일은 기존 작업을 유지한 채 거절한다.

터미널에서도 같은 파일을 쓸 수 있다.

```sh
node prototypes/workbench/cli.mjs my-layout.pure.json
```

명령 `save-project new-layout.json`으로 현재 배치를 저장하고 `restore saved-layout.json`으로
복원한다. 기존 파일을 덮어쓰지 않는다. 브라우저에서 저장한 미적용 초안도 다시 저장할 때 보존한다.

## 같은 규칙을 여러 대상에 연결하기

**공유 규칙으로 배치** 예제는 앞의 세 도형 배치와 같은 작업을 공통 규칙으로 작성한다.
`record`는 필드의 기본값, `item`은 대상별 실제 값, `rule`은 재사용할 반응이다.

```text
record Position { x=0; y=0 }
item green = Position(x=80,y=120)
rule move(t: Position) { t.x=event_x; t.y=event_y }
input drag = pointer(tile)
box tile(green.x,green.y,56,44,"#c9f08b")
on drag.move use move(green)
```

여러 입력을 같은 규칙에 연결해도 규칙의 실행 명령은 한 벌만 저장한다.
매개변수의 필드는 연결된 대상의 값을 직접 바꾼다. 같은 사건의 여러 반응 중 하나가
실패하면 앞의 변경도 함께 취소한다. 대상·필드 선언 순서를 바꾸어 적용해도
`green.x` 같은 이름으로 값을 유지하고, 배치 파일도 그 이름을 저장한다.

규칙은 레코드 매개변수 하나를 받는다. 레코드당 필드는 1~8개이며 모든 대상의 필드는
기존 상태 32개 한도에 포함된다. 실행 중 대상 생성·삭제는 아직 없다.
전체 작성물은 [shared-layout.pure](examples/shared-layout.pure), 결과와 한계는
[공유 규칙 보고서](../../docs/research/015-shared-rules.md)에 있다.

## 여러 도형을 하나의 포인터 입력으로 연결하기

**입력 묶어서 배치 → 예제 불러오기 → 코드 적용**으로 시작한다.

```text
input drag = pointer { green_box: green; blue_box: blue }
on drag.down use grab(drag.target)
on drag.move use move(drag.target)
on drag.up use drop(drag.target)
on drag.cancel use cancel(drag.target)
```

같은 record 타입의 대상을 묶고, 현재 잡은 대상의 필드를 공통 규칙에 전달한다.
도형을 늘려도 이 네 반응을 대상마다 복제하지 않는다. 서로 다른 도형은 동시에
조작할 수 있고 하나를 놓거나 취소해도 다른 도형의 조작은 이어진다.
같은 대상은 한 그룹 연결만 가질 수 있다. 전체 연결 대상은 최대 24개이며
상태 32개 등 기존 한도는 그대로다. 현재 예제의 키보드 선택 연결은 기존 방식이다.

배치 파일은 이전과 같은 방식으로 저장·복원한다. 마지막 입력에는 대상 도형도 표시한다.
CLI에서는 `point-target drag green_box down 90 130`을 사용한다.
[전체 작성물](examples/routed-layout.pure), [구현 결과와 한계](../../docs/research/016-pointer-routing.md).

## 일반 코드와 비교하기

[일반 JavaScript 비교 앱](http://127.0.0.1:8766/baseline.html)은
[충전 예제](examples/charge.pure)와 같은 상태·출력 계산을
[독립 JavaScript 모델](web/baseline.mjs)로 작성한 것이다.
Pure 컴파일러나 실행기를 가져오지 않는다.

JS 모델은 고정된 앱이다. 일반 프로그램의 실행 중 교체나 상태 이전은 제공하지 않는다.
두 파일의 길이만으로 개발 생산성을 비교하지 않는다. 자세한 실행 비용과 범위는
[연구 보고서 004](../../docs/research/004-authoring.md)에 있다.

## 터미널에서 같은 작업 실행하기

Node.js가 필요하다. 검증에 사용한 버전은 24.20.0이다.

```sh
node prototypes/workbench/cli.mjs my-work.pure
```

```text
down right
up right
frame 10
show
load my-work.pure
reset
save saved-work.pure
quit
```

`load`는 같은 이름의 상태를 보존한다. `save`는 적용한 소스를 저장하고 기존 파일은 덮어쓰지 않는다.
경로에 공백이 있으면 명령 뒤에 경로 전체를 입력한다. 별도 따옴표는 붙이지 않는다.
브라우저와 터미널은 동일한 컴파일러·Wasm을 쓰지만 **각자 별도의 실행 상태**를 갖는다.

## 계산 기록과 작업량 제한

표시 계산에 [기록을 버릴 수 있는 실행 계약](../../docs/research/009-bounded-display-integration.md)을
연결했다. 모든 도형에 하나의 작업 한도를 적용한다. 한도를 넘으면 장면 전체를
표시하지 않고 오류를 알린다. 그 전에 성공한 입력 이벤트의 상태는 유지된다.
기본값은 작업량 2,048, 캐시 꺼짐이다. 터미널에서 다음처럼 사용할 수 있다.

```text
limits 32 8 2048
show
costs
forget all
show
limits 32 8 1
limits 32 8 2048
```

`limits`의 숫자는 결과 캐시 개수·변경 기록 개수·표시 작업량이다.
`forget`은 `all`, `values`, `history`를 받는다. `costs`는 마지막 표시 계산의
작업량·명령 수·캐시 적중을 보여준다. 한도가 부족하면 101 표시 오류가 나고,
충분히 늘리면 다시 계산한다. 파이프로 실행한 CLI는 중간 실패도 종료 코드 1에 남긴다.

임베딩 API는 `createRuntime(wasmBytes, {cacheEntries:32, historyEntries:8, workLimit:2048})`,
`configureEvaluation`, `forgetEvaluation`, `evaluationStats`다. 브라우저 작성 화면에는
이 설정을 위한 새 UI를 추가하지 않았다. 저장 공간 반납이나 앱 전체의 메모리·시간
상한을 제공하는 기능은 아니다. 자세한 범위는 [계약](CONTRACT.md)을 따른다.

## 빌드·검증 재현

Zig 0.15.2를 설치하고 저장소 루트에서 실행한다. 다른 경로의 Zig는 `--zig /path/to/zig`로 지정한다.

```sh
python3 prototypes/workbench/manage.py build
node prototypes/workbench/compiler.test.mjs
node prototypes/workbench/runtime.test.mjs
node prototypes/workbench/baseline.test.mjs
node prototypes/workbench/cli.test.mjs
node prototypes/workbench/evaluation.test.mjs
node prototypes/workbench/workspace.test.mjs
node prototypes/workbench/presentation.test.mjs
node prototypes/workbench/routing.test.mjs
node prototypes/workbench/evaluation-check.mjs --output /tmp/pure-evaluation-new.json
node prototypes/workbench/workspace-check.mjs --output /tmp/pure-workspace-new.json
node prototypes/workbench/compare.mjs --output /tmp/pure-comparison-new.json
```

현재 빌드 명령은 네이티브 Zig 검사 37개도 실행한다. 전체 JS 검사는
`node --test prototypes/workbench/*.test.mjs`로 실행하며 현재 87개다.
비교 파일은 덮어쓰지 않으므로 매번 새 출력 경로를 지정한다.
빌드는 `ReleaseSafe`, 디버그 정보 제거, 16 KiB 스택, 초기·최대 선형 메모리 64 KiB다.
산출물과 빌드 기록의 해시로 재현 대상을 확인할 수 있다.

실제 브라우저 조작 시나리오는 [qa-browser.js](qa-browser.js)에 있다.
Playwright CLI 0.1.21의 `run-code`에 함수 전체를 전달하고,
마지막의 `window.__pureWorkbenchQAEvidence`를 후속 읽기로 수집했다.
대화상자·다운로드에서 CLI가 일찍 반환할 수 있으므로 반환 자체를 완료로 간주하지 않는다.
저장된 [브라우저 결과](results/2026-09-22-linux-aarch64/browser-checks.json)는 44개 확인을 포함한다.

이 결과는 Linux aarch64에서 얻었다. M3 macOS, 네이티브 GUI·Metal, 센서 입력,
프로세스 전체 메모리와 실제 사용자의 제작 시간은 아직 측정하지 않았다.

표시 계산의 첫 통합 기록은
[2026-09-22-evaluation](results/2026-09-22-evaluation/verification.json)에 있다.
기존 44개 브라우저 확인은 이전 빌드의 기록이다. 이번 변경의 실제 브라우저
확인 절차는 [qa-evaluation.js](qa-evaluation.js)다.
최신 빌드의 [브라우저 결과](results/2026-09-23-workspace/browser.json)는 12개 확인을 포함한다.
이 절차의 결과는 `window.__pureEvaluationEvidence`에서 읽는다.

현재 빌드는 코드 교체 준비 공간과 계산 기록을 공유한다. 교체 준비 중에도
기존 이벤트와 표시 계산은 실행할 수 있으며, 표시 계산은 기록 없이 재계산한다.
검사·비교 결과는 [2026-09-23-workspace](results/2026-09-23-workspace/verification.json)에 있다.
코어 고정 구조는 19,128 B로, 기록 기능을 넣기 전보다 증가한 전용 공간은
56 B다. 공유 영역을 메모리 합계에 중복해서 더하지 않는다.

현재 화면 갱신은 `present(target)`으로 계산 결과를 기존 DOM 요소에 전달한다.
매 입력마다 전체 장면 스냅샷이나 도형·상태·비용 요소를 다시 만들지 않는다.
CLI·검사용 `snapshot()`은 유지한다. [입력→출력 구현 보고서](../../docs/research/011-direct-presentation.md)에
구조 변경, 추가 공간, 속도 측정의 한계를 기록했다.
직접 출력 통합 당시의 [52개 JS/CLI/Wasm 검사](results/2026-09-23-presentation/verification.json),
[브라우저 경로 비교](results/2026-09-23-presentation/browser.json),
[작성 흐름 검사](results/2026-09-23-presentation/browser-authoring.json),
[표시 한도 검사](results/2026-09-23-presentation/browser-limit.json)를 함께 보존했다.

브라우저 비교는 Playwright CLI 0.1.21로 로컬 페이지를 연 세션에서 다음처럼 재실행한다.
이전 구현은 테스트 전용 응답 교체로 불러오며 현재 제품 파일은 바꾸지 않는다.

```sh
python3 prototypes/workbench/qa-presentation.py --cli /path/to/playwright-cli.js --session pure-presentation --output /tmp/pure-presentation-new.json
```

후속으로 실행 입력과 코드 편집의 갱신 경로를 분리했다. 실행 중인 값이 바뀌어도 소스의
줄 번호·크기·커서 정보를 다시 계산하지 않는다. 실제 소스 변경과 적용 상태는 계속 갱신한다.
[결과 보고서](../../docs/research/012-refresh-ownership.md),
[브라우저 비교](results/2026-09-23-refresh/browser.json),
[작성 흐름](results/2026-09-23-refresh/browser-authoring.json),
[표시 한도](results/2026-09-23-refresh/browser-limit.json)를 보존했다.
이 변경에서 런타임·컴파일러·Wasm은 그대로이며, 이전 단위 검사를 새 검사로 세지 않는다.

```sh
python3 prototypes/workbench/qa-refresh.py --cli /path/to/playwright-cli.js --session pure-path --output /tmp/pure-refresh-new.json
```

## 실제 웹 레이아웃과 문서 편집

[문서 작업 화면](http://127.0.0.1:8766/document.html)은 일반 UTF-8 텍스트를 열고,
수정하고, 옆에서 읽고, `.txt`로 저장하는 작업이다. 기존 작업 화면에도 링크가 있다.

- 넓은 화면에서는 경계를 잡거나 방향키·슬라이더로 두 패널의 비율을 조절한다.
- Esc·접촉 취소는 시작 비율로 돌아간다. 창 크기가 바뀌어도 조작 좌표를 다시 맞춘다.
- 좁은 화면에서는 작성·읽기가 위아래로 놓인다. 각 패널은 브라우저의 스크롤을 사용한다.
- **분할 규칙 수정**에서 `clamp(event_value,20,80)`의 제한을 더 좁혀 적용할 수 있다.
- 파일은 텍스트만 저장한다. 패널 비율과 규칙을 저장하는 별도 문서 형식은 만들지 않았다.

이 화면은 직접 작성한 HTML 호스트다. Pure의 현재 DSL만으로 일반 웹 UI 전체를
작성하게 된 것은 아니다. 레이아웃·텍스트·IME·선택을 VM에 복제하지 않고, 필요한
숫자 조작만 전달한다. 파일은 2 MiB까지, UTF-8을 사용하며 저장 시 줄바꿈은 LF다.
현재 읽기 화면은 문서 편집 시 텍스트 전체를 갱신한다. 전체 앱의 메모리 절감이나
큰 문서 성능 우위는 주장하지 않는다. [결과와 한계](../../docs/research/017-web-layout-boundary.md).

```text
state ratio = 50
input split = value
on split.change { ratio = clamp(event_value,20,80) }
```

웹 호스트 API는 `runtime.change('split',65,'range')`, CLI는 `change split 65`다.
숫자 값은 유한해야 하고 상태 배열에 자동 보관되지 않는다. 필요한 값만 규칙에서 저장한다.
