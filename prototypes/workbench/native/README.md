# 기존 .pure 작성물을 네이티브로 실행

기존 Workbench 문법을 그대로 준비해 C ABI의 독립 계산 인스턴스에서 실행한다.
입력과 저장·복원은 `prototypes/connections`의 실행 코어를 통과한다. 실행 전용 CLI와
선택형 Linux GTK4 작성 창과 인증된 피어 연결 호스트가 있다.
빌드에는 Node·Zig 0.15.2·C17 컴파일러가 필요하다. 실행 전용 CLI의 실행에는 필요하지 않다.
선택형 작성 창은 소스를 적용할 때 Node·C 컴파일러가 필요하다.
현재 검증 환경은 Linux AArch64다. GTK 창은 실행 전용 프로그램에 들어가지 않는다.
고정 정의 CLI·피어는 검증된 `.pure`의 불변 프로그램을 한 번 링크하고 인스턴스마다
가변 상태만 둔다. 새 정의를 사용하려면 다시 빌드한다. 작성 창과 브라우저는 정의 교체가
가능한 전체 작성 용량을 유지한다. [반복 메모리와 실행물 전체 비용](../../../docs/research/028-shared-fixed-program.md)을
구분해서 확인했다.
고정 실행 앱은 유휴 중 작업 슬롯을 예약하지 않는다. 입력·저장·복원 호출 동안만
코어 슬롯을 스택에서 대여하고, 1,000개 실제 앱의 반복 RSS 절감과 추가 스택·코드
비용을 [측정했다](../../../docs/research/049-idle-app-work-slots.md).

저장소 루트에서:

```sh
python3 prototypes/workbench/manage.py native prototypes/workbench/examples/routed-layout.pure --zig /path/to/zig
prototypes/workbench/build/native/pure-native /tmp/pure-native-work scene
```

프로그램에 입력할 명령:

```text
point drag down green_box 90 130
point drag move green_box 230 210
point drag up green_box 230 210
save
quit
```

같은 실행 파일을 같은 디렉터리·이름으로 다시 시작하고 `load`를 입력하면 저장한 상태를
복원한다. 위 예제에서 `green.x=220`, `green.y=200`, `green.held=0`이다. 시작만으로 기존 상태를
자동 채택하거나 덮어쓰지 않는다. 저장 성공 때 채택한 버전을 기준으로 다음 저장을 조건부 실행한다.
다른 실행기가 먼저 저장했으면 충돌 상태를 출력하고 로컬 값을 유지한다.

다른 작성물도 `manage.py native MY.pure --output MY_APP`으로 빌드한다. 지원 명령은
`show`, `key INPUT down|up`, `value INPUT NUMBER`, `point INPUT down|move|up|cancel TARGET X Y`,
`frame`, `reset`, `save`, `load`, `quit`이다. 입력 한 줄은 510 B까지다.
출력 JSON의 `status=0`은 성공이며 `scalar_error`는 작성된 계산의 실패 코드다.
명령 중 오류가 있었으면 종료 코드는 1, 시작 실패는 2다. 포인터 TARGET은 단일 입력에도 명시한다.
명령 인터페이스는 한 출처를 사용하고 C 호스트 API는 대상별 소유자·키별 출처를 구분한다.

## 선택형 네이티브 작성 창

Linux에서 GTK4 개발 헤더·라이브러리와 `pkg-config`를 설치한 뒤 빌드한다. 작성 창의
실시간 소스 적용에는 Node와 C 컴파일러도 실행 시 필요하다. CLI 실행에는 둘 다 필요 없다.

```sh
python3 prototypes/workbench/manage.py native prototypes/workbench/examples/routed-layout.pure \
  --desktop --zig /path/to/zig
prototypes/workbench/build/native/pure-desktop /tmp/pure-native-work scene \
  prototypes/workbench/examples/routed-layout.pure
```

왼쪽에서 `.pure`를 편집하고 **Apply source**를 누른다. 성공하면 같은 이름의 숫자 상태를
유지하고 소스 파일을 저장한다. 실패하면 편집 중인 텍스트는 남지만 이전 실행 정의와 저장된
소스는 유지된다. 창을 다시 열면 소스를 준비·적용한다. 오른쪽 화면을 눌러 키를 전달하고
도형을 잡아 이동한다. **Save state**와 **Load state**는 소스 파일과 별개의 조건부 스냅샷이다.
소스를 바꾸면 이전 정의의 스냅샷은 자동 복원되지 않는다. 새 정의를 적용한 뒤 상태를 다시
저장하면 새 정의의 스냅샷이 된다. 수치 입력은 오른쪽 아래 입력창에서 숫자를 입력하고 Enter를 누른다.

GPU가 없는 Xvfb 검증에서는 GTK의 Cairo 렌더러와 GPU 탐색 비활성화를 사용했다.

```sh
GSK_RENDERER=cairo GDK_DISABLE=gl,vulkan \
  prototypes/workbench/build/native/pure-desktop /tmp/pure-native-work scene \
  prototypes/workbench/examples/routed-layout.pure
```

이 설정은 모든 호스트의 기본값이 아니다. 이 환경의 유휴 GTK 작성 창은 약 36 MiB RSS,
기본 소프트웨어 GPU 경로는 약 174 MiB였으며 CLI는 약 1.5 MiB였다. **편집 도구 전체를
마이크로 런타임 크기라고 부르지 않는다.** [실제 창 검증·전체 비용](../../../docs/research/025-native-authoring-shell.md).

`scalar.h`는 호출자가 제공한 메모리의 명시적인 인스턴스 API다. `plan.h`는 빌드 시 준비한
읽기 전용 데이터, `app.h`는 같은 코어에서 입력·스냅샷·저장을 연결하는 호스트다.
고정 CLI·피어에서는 `plan.h`의 명령·반응 포인터가 비어 있으며, 해당 데이터는 Zig의
공유 프로그램에서 읽는다. 고정 실행물의 정의 staging 및 선택형 평가 캐시는 사용할 수 없다.
`prepare.mjs`는 기존 컴파일러를 재사용하며 새 런타임 파서를 추가하지 않는다.
준비된 명령을 해석하는 계산 모듈이며, 모든 앱 연산을 기계어로 직접 생성하는 컴파일러는 아니다.
작성 창의 새 정의 설치도 실행 코어의 작업 접수·완료를 거친다. `PURE_NATIVE_AUTHORING`은
작성 창에만 링크되어 CLI의 코드 크기와 동적 의존성을 늘리지 않는다.

## 선택형 인증 피어 연결

OpenSSL 개발 패키지와 `pkg-config`가 있는 Linux에서 빌드한다. 실행 전용 CLI에는
OpenSSL을 연결하지 않는다. 같은 `.pure` 정의로 만든 피어 서버와 클라이언트가 필요하다.

```sh
python3 prototypes/workbench/manage.py native prototypes/workbench/examples/routed-layout.pure \
  --peer --zig /path/to/zig
prototypes/workbench/build/native/pure-peer serve 127.0.0.1 8443 /tmp/pure-peer-state scene \
  CA.pem server.pem server.key pure-client
prototypes/workbench/build/native/pure-peer call 127.0.0.1 8443 localhost \
  CA.pem client.pem client.key
```

서버 인증서는 `localhost` DNS SAN과 `serverAuth`, 클라이언트 인증서는 `pure-client`
DNS SAN과 `clientAuth`가 필요하다. 양쪽 인증서는 지정한 CA의 서명을 받아야 한다.
실제 검증용 일회성 인증서를 만들고 Python 표준 TLS 클라이언트와 상호운용성·실패 경로를
검사하는 재현 명령은 다음과 같다. 인증서는 임시 디렉터리에서만 사용하고 삭제한다.

```sh
python3 prototypes/workbench/native/peer-check.py prototypes/workbench/build/native/pure-peer
```

클라이언트 명령은 `state`, `save`, `load`, `frame`, `reset`, `key NAME down|up`,
`value NAME NUMBER`, `point NAME down|move|up|cancel TARGET X Y`, `quit`이다.
서버는 시작 시 기존 저장 상태를 복원하고, 각 요청의 기대 버전을 확인한다. 연결이 끊긴 뒤
입력 소유권을 해제한다. 응답을 잃은 부작용 작업은 자동 재시도하지 않는다.
정의 identity가 다르면 Pure 클라이언트가 연결 직후 거절한다. 연결은 현재 한 번에 한
클라이언트만 처리한다. 서버와 클라이언트 모두 인증 실패 시 평문으로 낮추지 않는다.
프로토콜 길이·크기·전체 호스트 RSS와 한계는 [026 보고서](../../../docs/research/026-secure-peer-link.md)에 있다.

현재 저장 스냅샷은 이 실행물의 준비 정의에 묶여 있다. 정의를 바꾼 뒤 다른 정의의 스냅샷을
묵시적으로 적용하지 않는다. 작성 창에는 실시간 정의 교체가 있지만 CLI의 실시간 소스 편집과
기존 JSON 프로젝트 변환은 미구현이다. 현재 보기는 고정된 640×360 논리 화면을 확대·축소하며
범용 네이티브 레이아웃 엔진이나 macOS 창 백엔드는 아니다.

일반·C 호스트 sanitizer 검증 (`prototypes/workbench` 기준):

```sh
node native/check.mjs
node native/check.mjs --sanitize
node native/check.mjs --fixed --sanitize --zig /path/to/zig
node --test *.test.mjs
```

첫 세 명령은 현재 소스에서 라이브러리를 직접 빌드하고 기존 Wasm을 사용하며, 실제 자식
프로세스와 파일 저장이 필요하다. Zig 0.15.2가 `PATH`에 없다면 각 명령에
`--zig /path/to/zig`를 준다. `--fixed`는 작성물별 공유 불변 프로그램을 빌드해 같은 동작을
대조한다. Zig 오브젝트는 ReleaseSafe이며 C 호스트만 ASan/UBSan으로 계측한다.
[전체 용량 기준](../../../docs/research/024-native-app-instances.md),
[공유 고정 정의 결과](../../../docs/research/028-shared-fixed-program.md).
