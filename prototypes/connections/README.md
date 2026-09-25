# Pure: 작성한 연결을 준비해 실행하는 네이티브 경로

기존 Workbench에서 만든 코드·배치를 실제 TCP 요청으로 저장하고 새 실행기에서 복원한다.
서버는 C17 네이티브 실행 파일이다. SQL·SQLite 의존성을 제거했고, 호출자의 바이트를
그대로 파일에 기록한다. 저장할 구간과 응답할 구간은 `.pure` 파일로 작성하며 서버 시작 때
실행 경로를 준비한다. 기존 Node/Wasm CLI는 그대로 사용한다.

이 구현은 저장·전송의 수명을 확인하는 최소 경로다. 객체별 파일이나 전체 교체 방식을
Pure의 최종 데이터 모델로 정한 것은 아니다. 필드 배치·상수 구간·별칭은 준비 단계에서
처리하고, 입력 값에 의존하는 구간은 요청 때 계산한다.
[구현 결과와 한계](../../docs/research/021-field-connections.md).

## 실행 코어의 현재 상태

요청 접수·본문 예산·작업 준비·완료·해제를 `runtime.c`가 관리한다. HTTP는 그 코어의 한
입력/출력 어댑터다. 다른 실행 영역에 결과를 전달할 때 코어가 원본을 유지하며, 수신 영역의
효과와 응답 소비가 끝나면 회수한다. `test-runtime.c`는 수명·예산 단위 계약을,
선택형 `make check-mirror`는 실제 1 MiB 문서가 한 프로세스의 두 저장 영역으로
전달되는 것을 검사한다. [실제 연결 비용과 한계](../../docs/research/063-real-in-process-routing.md).

`make all`은 파서 없는 고정 정의 CLI와 실행 전용 `build/libpure-execution.a`를 만든다.
빌드 시 `.pure` 정의를 준비하는 컴파일러도 생성하지만 CLI에는 링크되지 않는다.
실행 중 소스를 읽는 CLI는 `make editor`, HTTP 호스트는 `make server`로 선택한다.
아카이브에는 `runtime.c`만
들어가며 문서 평가기·파서·HTTP·POSIX 저장 구현은 없다. 문서 평가는 선택한
`store-port.c`에 연결된다. 서버는 소스 파일을 읽으므로 파서를 별도로 포함한다.
기본 호스트 효과는 동기식이고 작성 문법은 아직 get/put 저장 프로필이다.
전체 앱 런타임·자체 보안 RPC·스레드 간 안전성·강제 메모리 격리를 구현한 것은 아니다.
[구현·비용·검증 보고서](../../docs/research/022-execution-core.md),
[첫 공개 체크포인트](../../CHECKPOINT.md).

인증 문서 피어가 필요한 경우 `make document-peer`는 서버·클라이언트 명령을
한 실행물에 넣는다. 서버만 배치할 때는 `make document-server`를 선택할 수 있다.
`make check-document-server`는 서버 전용 코드·의존성 상한과 기존 C/Python TLS
문서 시나리오를 검사한다. 이 선택 빌드는 기본 코어·CLI에 TLS를 추가하지 않는다.
루프백의 [직접 서버 비교와 전송 지연 결과](../../docs/research/055-authenticated-peer-transport-cost.md)는
전체 프로세스 비용과 아직 남은 다중 연결 한계를 함께 기록한다.
이동 가능한 서버 전용 묶음은 `package_native.py --with-peer-server --output FILE`로
만들고 `check_package.py FILE`로 검증한다. CLI+TLS 아카이브 전체 상한과
[이동 후 인증·문서 작업 결과](../../docs/research/056-server-only-document-package.md)를
별도로 공개한다.

## 필요한 구성만 빌드하기

`make core`, `make fixed-editor`, `make editor`, `make server`로 구성을 각각 선택한다. `make core`에는
소스 파서·저장소·네트워크·UI가 들어가지 않는다. 영역마다 스레드·소켓·힙을 생성하지 않는다.
release 빌드의 `make check-core`는 코어 코드 섹션 4,864 B, data/BSS 0 B와 현재 구조체 비용의
상한·아카이브 의존성을 검사한다. 현재 Linux AArch64의 기본 구성은
`text` 4,632 B / 영역 120 B / 작업 216 B다. 누적 계수가 필요한 HTTP 호스트와
테스트는 `PURE_RUNTIME_METRICS`를 선택한다. 이 프로필의 영역은 192 B다.
코어에서 문서 해석을 분리하기 전의 `text` 5,980 B / 영역 128 B와 구분한다.
[현재 경계와 전체 앱 비용](../../docs/research/068-opaque-app-core.md)에 기록했다.
[해제 콜백 재진입 방어 비용](../../docs/research/064-release-callback-reentrancy.md)을
기본 코어 상한 안에서 검증했다.
[실제 앱 1,000개 비교](../../docs/research/051-optional-runtime-metrics.md)에서 두 비용을
분리했다. 전체 앱의 RAM 수치는 아니다. GNU 호환 도구를 사용하는 현재 프로필의 회귀 검사다.
`make check-profile`은 기본 CLI·HTTP 실행물의 링크된 섹션과 직접 동적 의존성을 검사한다.
두 검사를 실제 동작·CLI 전체 프로세스 RSS 검사와 함께 실행할 때는 release 빌드에서
`make check-release`를 사용한다. RSS 검사는 1 B/1 MiB 실제 문서 편집·읽기를
[현재 장치의 4 MiB 회귀 상한](memory-budget.json)과 비교한다.
이 검사에는 파서 없는 고정 정의 CLI·HTTP 실행물의 크기·RSS 상한도 포함된다.
`make check-service-scale`은 같은 문서 앱의 1,024개 객체를 8개 클라이언트가
생성·읽기·수정·복원하여 두 HTTP 호스트의 결과, 서버 RSS, 버퍼 회수를 확인한다.
[검사 범위와 결과](../../docs/research/040-document-service-capacity.md).
`make check-async`는 선택형 비동기 완료 코어와 실제 저장 효과를 검사한다.
유휴 동기 앱은 `pure_runtime_bind()`로 작업 슬롯을 분리할 수 있다. 진행 중인
작업·대여가 남으면 분리를 거절하며, 슬롯 재결합에도 티켓 세대는 유지된다.
[실제 네이티브 앱 1,000개의 메모리 비용](../../docs/research/049-idle-app-work-slots.md).
기본 코어·CLI·HTTP에는 작업자 스레드나 비동기 완료 코드가 링크되지 않는다.
선택형 코어 자체는 작업자를 만들지 않으며, 효과를 가져온 호스트가 I/O를 실행하고
같은 소유 스레드에서 완료해야 한다. 기본 서비스 호스트는 이 경로를 사용하지
않는다. [계약·비용·한계](../../docs/research/045-owner-thread-async-effects.md).
`make async-server`는 이 계약을 기존 문서 HTTP 앱에 선택적으로 연결한다.
`make check-async-server`는 크기 상한과 쓰기가 멈춘 동안 기존 문서 읽기·기한·
종료를 검사하고, `make check-async-service-scale`은 기존 1,024개 문서 작업을
같은 소스 정의 앱으로 다시 실행한다. 이 선택 호스트는 읽기 작업자 하나와
쓰기 작업자 하나를 사용하고, 모든 쓰기는 한 작업자에서 직렬화한다.
[현재 동작과 비용](../../docs/research/048-immutable-read-write-overlap.md).
GTK·TLS를 선택하지 않아도 기본 실행물에 섞이면 검사가 실패한다. 예산은
[`profile-budget.json`](profile-budget.json)에 기록했으며, 호스트·컴파일러가 바뀌면
측정값을 다시 검토해야 한다. 이 검사만으로 전체 앱 RAM이 작다고 증명하지는 않는다.
[현재 비용과 제한](../../docs/research/023-document-edits.md).

Linux AArch64의 기본 시험 묶음에는 CLI와 `.pure` 정의만 넣는다. `make check-micro-memory`로
빌드한 뒤 첫 명령을 실행한다. 전체 교차 검증 묶음은 `make all server document-peer window`
와 `make check-micro-memory` 뒤에 HTTP·피어·창을 모두 명시적으로 선택한다.
패키징은 CLI 메모리 상한도 다시 확인한다. 소스 작성 묶음은 디버그 정보를,
고정 정의 묶음은 실행에 필요 없는 심볼까지 제거하며 빌드 실행물은 그대로 둔다.
둘 다 정적 실행물이 아니고 매니페스트에 시스템 공유
라이브러리를 기록한다. `check_package.py`는 묶음을 다른 위치에 풀어 선택된
경로의 실제 동작을 다시 확인한다.
고정 정의 묶음은 선택한 CLI·HTTP·TUI 조합의 압축 아카이브 전체 크기 상한도
[`fixed-package-budget.json`](fixed-package-budget.json)으로 검사한다. 초과하면 패키지를
만들지 않는다. 이 값은 libc·터미널·저장 파일을 포함한 실행 메모리 값은 아니다.
[현재 터미널 편집기](../../docs/research/044-terminal-viewport-cost.md)는 여러 행·탭 문서의
재열기를 처리하며, 1 MiB 화면의 반복 입력은 문서 전체를 매번 스캔하지 않는다.
[문서 이름 접두어 탐색](../../docs/research/050-bounded-document-prefix-find.md)은 이미 정렬된
목록을 사용하며, 목록을 연 상태에서 이름을 입력하고 Enter로 연다. Backspace는
접두어를 줄이고 화살표는 검색어를 지운 뒤 목록을 이동한다.

```sh
python3 prototypes/connections/package_native.py \
  --output dist/pure-document-micro-linux-aarch64.tar.gz
python3 prototypes/connections/check_package.py \
  dist/pure-document-micro-linux-aarch64.tar.gz

python3 prototypes/connections/package_native.py \
  --with-http --with-peer --with-window \
  --output dist/pure-document-authoring-linux-aarch64.tar.gz
python3 prototypes/connections/check_package.py \
  dist/pure-document-authoring-linux-aarch64.tar.gz
```

필요한 호스트만 선택할 수도 있다. 예를 들어 `--with-window`만 주면 HTTP·TLS 없이
CLI와 GTK 창만 포함하며, 창의 외부 변경·충돌은 CLI를 통해 검증한다.

[기본·선택 묶음, 메모리 비용과 한계](../../docs/research/032-micro-native-authoring-boundary.md).

동일한 문서 인터페이스에 인라인/16 KiB 페이지 혼합 저장소를 **선택형 연구 경로**로 붙여볼 수 있다.
작은 기존 `PUREB001` 값은 그대로 읽지만, 큰 값을 `PUREP001`로 바꾼 뒤에는 기본 저장기로
직접 열 수 없다. 따라서 실험에 별도 `--store` 디렉터리를 쓴다. 아래 내보내기
명령은 새 기본 형식 디렉터리를 만들어 본문·리비전을 보존하며, 기존 디렉터리를
제자리에서 바꾸지 않는다. 기본 저장소를 교체하는 명령이 아니다.

```sh
make -C prototypes/connections check-hybrid
printf 'hello Pure\n' > /tmp/pure-hybrid-insert.txt
prototypes/connections/build/hybrid/pure-edit \
  --program prototypes/connections/examples/editor.pure \
  --store /tmp/pure-hybrid-documents edit note 0 0 0 /tmp/pure-hybrid-insert.txt
prototypes/connections/build/hybrid/pure-edit \
  --program prototypes/connections/examples/editor.pure \
  --store /tmp/pure-hybrid-documents show note
python3 prototypes/connections/package_native.py \
  --build prototypes/connections/build/hybrid --hybrid-storage \
  --output dist/pure-document-hybrid-micro-linux-aarch64.tar.gz
make -C prototypes/connections check-flat-export
python3 prototypes/connections/export_flat_store.py \
  --source /tmp/pure-hybrid-documents --destination /tmp/pure-flat-documents \
  --paged-editor prototypes/connections/build/hybrid/pure-edit-fixed \
  --flat-editor prototypes/connections/build/pure-edit-fixed
```

[실제 큰·작은 문서 쓰기량과 전체 제한](../../docs/research/034-hybrid-document-storage.md),
[기본 형식으로 내보내는 검증](../../docs/research/062-paged-store-rollback.md).

선택형 영속 트리 저장소(`store-tree.c`)는 모든 문서를 추가 전용 파일 하나에 불변 익스텐트
트리로 둔다. 편집은 바뀐 leaf와 경로만 기록하고, 읽기는 잠그지 않는다. 기존 런타임·부분 수정·
비동기·실제 PTY 검사와 자체 고장·오라클 검사를 함께 실행한다. 형식이 달라 별도 `--store`
디렉터리를 쓴다. [설계·측정·한계](../../docs/research/073-single-file-persistent-tree.md).
같은 저장소에서 런타임이 앱의 작업 버전을 소유하는 draft API는 [`state.h`](state.h)이며,
설계와 측정은 [074](../../docs/research/074-runtime-owned-drafts.md)에 있다.

```sh
make -C prototypes/connections check-tree
prototypes/connections/build/tree/pure-edit-tui-fixed --store /tmp/pure-tree-documents note
```

같은 `.pure` 문서 편집기를 선택형 페이지 저장소와 함께 터미널에서 사용하려면
새 저장 디렉터리를 지정한다. 논리 페이지는 16 KiB이며 OS 페이지가 이를
나눌 때만 초기화한다. Linux 16 KiB 호스트에서 전체 선택형 검사가 통과했고,
4 KiB QEMU 게스트에서 저장소 단언과 1 MiB 고정 CLI 작업을 통과했다.
Linux x86_64 CI와 macOS 실행 결과는 아직 없다. CLI와 TUI를
함께 담은 [선택형 묶음](results/2026-09-24-native-save/pure-document-hybrid-cli-tui-linux-aarch64.tar.gz)은
다음 빌드로 만들며, 기본 `make all`과 기본 저장 형식은 유지된다.

```sh
make -C prototypes/connections BUILD=build/hybrid STORE=store-paged.c editor source-tui
python3 prototypes/connections/package_native.py \
  --build prototypes/connections/build/hybrid --hybrid-storage --with-tui \
  --output dist/pure-document-hybrid-cli-tui-linux-aarch64.tar.gz
python3 prototypes/connections/check_package.py \
  dist/pure-document-hybrid-cli-tui-linux-aarch64.tar.gz
```

[실제 Ctrl+S 완료 시간·코드·RSS 비용](../../docs/research/065-native-save-latency.md)을
함께 검토해야 한다. 큰 문서의 `PUREP001` 형식은 기본 저장기가 직접 열 수 없다.

기본 `PUREB001` 형식을 유지하며 큰 수정의 사용자 공간 복사 호출을 줄이는
Linux 전용 선택형 실험은 `make -C prototypes/connections check-copy-range`로
검사한다. `strace`·`findmnt`와 GNU 크기 도구가 필요하며, 기본 `make editor`에는
이 경로가 들어가지 않는다. [같은 1 MiB 문서의 시스템 호출·전체 RSS·코드 비용과
지원 범위](../../docs/research/059-immutable-file-copy-path.md)를 분리해 공개한다.

## 네이티브 CLI로 문서 일부 바꾸기

저장소 루트에서 실행한다. 입력 파일에 삽입할 실제 내용만 둔다.

```sh
make -C prototypes/connections editor
printf 'hello Pure\n' > /tmp/pure-insert.txt
prototypes/connections/build/pure-edit --program prototypes/connections/examples/editor.pure --store prototypes/connections/build/edited-objects edit note 0 0 0 /tmp/pure-insert.txt
printf 'world' > /tmp/pure-insert.txt
prototypes/connections/build/pure-edit --program prototypes/connections/examples/editor.pure --store prototypes/connections/build/edited-objects edit note 1 6 4 /tmp/pure-insert.txt
prototypes/connections/build/pure-edit --program prototypes/connections/examples/editor.pure --store prototypes/connections/build/edited-objects show note
```

새 저장소에서 마지막 출력은 `hello world`와 줄바꿈이다. `edit`의 숫자는 예상 버전·시작 바이트·
삭제 바이트 수다. 성공 시 JSON 버전을 출력하며, `show`는 stdout에 원문을, stderr에 버전을
출력한다. 버전 충돌의 종료 코드는 3, 다른 오류는 2다. 변경이 없더라도 성공한 효과는 버전을
증가시킨다. UTF-8 문자소 위치는 호출자가 바이트 범위로 변환해야 한다.

편의 명령은 준비된 두 숫자 필드와 뒤따르는 삽입 바이트를 채운다. 다른 입력 구성은
`apply KEY REVISION INPUT_FILE`로 실행한다. `--budget BYTES`를 명령 앞에 지정하면 입력/결과의
본문 크레딧을 제한한다. 저장소가 파일을 다시 쓰는 4 KiB 스택 버퍼·커널 비용은 별도다.
서버에 같은 `editor.pure`·저장 경로를 지정하면 외부 HTTP 입력도 같은 효과를 실행한다.
이 CLI는 네이티브 자동화 경로다. 문서 창은 아래의 선택 빌드다.

## 인증된 문서 피어

`make document-peer`는 같은 `editor.pure`·저장소·실행 코어를 사용하면서 TLS 1.3
상호 인증과 `pure-doc/1` 자체 이진 형식을 선택 빌드한다. 기본 `make all`에는
OpenSSL이 들어가지 않는다. 준비된 CA와 서버·클라이언트 인증서가 필요하며,
서버 인증서의 DNS SAN은 `localhost`, 클라이언트 인증서의 DNS SAN은
아래 예시의 `pure-client`와 일치해야 한다.

```sh
make -C prototypes/connections document-peer
prototypes/connections/build/pure-document-peer serve 127.0.0.1 9443 \
  prototypes/connections/examples/editor.pure /tmp/pure-documents \
  /path/to/ca.pem /path/to/server.pem /path/to/server.key pure-client
prototypes/connections/build/pure-document-peer call 127.0.0.1 9443 localhost \
  prototypes/connections/examples/editor.pure /path/to/ca.pem \
  /path/to/client.pem /path/to/client.key show note
```

`call`은 `show KEY`, `edit KEY REVISION OFFSET REMOVE INSERT_FILE`,
`apply KEY REVISION INPUT_FILE`을 받는다. 읽기는 원문을 stdout에 보낸다.
편집은 결과 본문을 소비하지 않으므로 논리적 결과 길이만 받고 본문 전송은 생략한다.
서버는 지속 세션을 처리하지만 예제 C 명령은 호출마다 연결한다. 인증서를 자동 생성해
실제 C·Python 클라이언트, 1 MiB 문서, 실패·재시작을 검사하려면
`make -C prototypes/connections check-document-peer`를 실행한다. 이 대상은 선택형
실행물의 코드·직접 의존성 [예산](peer-budget.json)도 검사한다.
[형식·비용·한계](../../docs/research/030-authenticated-document-peer.md).

## 네이티브 문서 창

Linux에서 GTK 4.10 이상 개발 헤더·라이브러리와 `pkg-config`가 있으면 같은
[`editor.pure`](examples/editor.pure)를 실제 텍스트 창으로 실행할 수 있다. GTK는 이
선택 빌드에만 들어가며 `make all`이나 코어·CLI·서버의 의존성이 아니다.

```sh
make -C prototypes/connections window
prototypes/connections/build/pure-edit-window \
  --program prototypes/connections/examples/editor.pure \
  --store /tmp/pure-documents note
```

문서를 편집한 뒤 **Save** 또는 Ctrl+S를 누른다. **Definition** 버튼 또는
Ctrl+Shift+D로 같은 `.pure` 소스를 편집하고 **Apply & save** 또는 Ctrl+Shift+S로
현재 창에 적용한다. 잘못된 정의, 다른 실행기의 소스 변경, 저장 실패는 초안과 기존
실행 계획을 보존한다. 다른 실행기는 재시작한 뒤 저장된 정의를 읽는다.
창 아래의 문서 키 입력과 **Open** 또는 Ctrl+O로 다른 문서를 연다. 없는 키는 새
빈 문서로 열고, 미저장 문서·정의 초안이 있거나 키·본문이 유효하지 않으면
현재 문서와 버전을 유지한다. 목록·검색 기능은 아직 없다.
변경한 바이트 구간만 작성된 splice 입력에
넣고, 저장소는 이전 버전이 일치할 때만 공개한다. CLI·HTTP 서버도 같은 프로그램과 저장
경로를 쓰므로 다른 실행기가 먼저 저장하면 창은 초안을 유지하며 충돌을 보여준다. Reload는
미저장 변경을 보호하고 Discard & reload만 명시적으로 버린다. 닫기·Ctrl+W도 미저장
초안을 확인한다. 창은 UTF-8 텍스트만 열며 이진 데이터는 CLI로 다룬다.

실제 창·CLI·HTTP 교차 검증, 정의 적용·실패 복구, 1 MiB 문서 수정과 전체 메모리 비용은
[032 보고서](../../docs/research/032-micro-native-authoring-boundary.md)에 있다. 화면 없이
같은 시나리오를 재현하려면 Xvfb·xdotool·xclip을 설치하고 다음을 실행한다.

```sh
make -C prototypes/connections window editor server
xvfb-run -a -s '-screen 0 1280x800x24' \
  python3 prototypes/connections/check_window.py
```

## 작은 네이티브 터미널 편집기

빌드 시 준비한 같은 문서 앱을 GTK 없이 터미널에서 편집하려면 `fixed-tui`를
선택한다. Ctrl+S 저장, Ctrl+O 다른 키 열기, Ctrl+R 다시 읽기, Ctrl+Q 종료다.
미저장 초안을 버리려면 다시 읽기·종료 명령을 두 번 입력해야 한다. 이 실행물은
`.pure` 정의를 실행 중 다시 작성하지 않는다.

```sh
make -C prototypes/connections fixed-tui
prototypes/connections/build/pure-edit-tui-fixed --store /tmp/pure-documents note
make -C prototypes/connections check-tui
python3 prototypes/connections/package_native.py --fixed --with-tui \
  --output dist/pure-document-cli-tui-linux-aarch64.tar.gz
```

실제 PTY 조작·1 MiB 편집·프로세스 메모리·패키지의 범위는
[042 보고서](../../docs/research/042-micro-native-terminal-editor.md)에 있다.

실행 시 `.pure` 정의를 읽는 TUI가 필요하면 `source-tui`를 선택한다. 기존 고정 TUI와
달리 파서가 선택한 실행물에 포함된다. 정의를 고친 뒤 새 세션을 열어야 적용되며,
편집 중인 문서 상태를 묵시적으로 변환하지 않는다. 기본 코어와 고정 TUI에는 영향이 없다.

```sh
make -C prototypes/connections source-tui
prototypes/connections/build/pure-edit-tui --program \
  prototypes/connections/examples/editor.pure --store /tmp/pure-documents note
make -C prototypes/connections check-source-tui
python3 prototypes/connections/package_native.py --with-tui \
  --output dist/pure-document-source-cli-tui-linux-aarch64.tar.gz
```

구체적인 지원 범위와 전체 비용은 [소스 TUI 검증](../../docs/research/053-source-loaded-native-terminal-editor.md)에 기록한다.

## 실행

POSIX 환경의 C17 컴파일러와 make가 필요하다. 검증 환경은 Linux AArch64다.
저장소 루트에서 실행한다.

```sh
make -C prototypes/connections server
prototypes/connections/build/pure-connections --program prototypes/connections/examples/storage.pure --store prototypes/connections/build/objects --port 8767
```

`--store`는 디렉터리다. 기존 SQLite 파일 등 일반 파일을 지정하면 거절하며 덮어쓰거나
변환하지 않는다. 자동 마이그레이션은 제공하지 않는다.
`--program`은 필수다. 파일 수정은 다음 서버 시작에 반영되며 실행 중 교체는 지원하지 않는다.

다른 터미널에서 기존 작업물을 연다. Node가 필요하다.

```sh
node prototypes/workbench/cli.mjs prototypes/workbench/examples/direct.pure
```

CLI에서 실행할 예:

```text
point drag down 90 170
point drag move 360 220
point drag up 360 220
connect http://127.0.0.1:8767 my_work
push 0
quit
```

빈 CLI를 다시 실행하고 `connect http://127.0.0.1:8767 my_work`, `pull`, `show`를 입력하면
저장한 배치가 복원된다. 이후 변경은 `push`로 저장한다. `push 0`은 새 이름을 만들 때만 사용한다.
서버를 재시작해도 같은 저장 디렉터리를 지정하면 내용을 읽을 수 있다. 다른 실행기가 먼저
갱신했다면 412 충돌로 거절하고 로컬 배치를 유지한다. `pull`은 로컬 배치를 원격 내용으로
교체하므로, 보존할 작업은 먼저 `save-project PATH`로 저장한다.

## 필드로 문서 선택·저장하기

[selection.pure](examples/selection.pure)는 선택 시작·길이와 문서를 입력받아 선택한 내용만
저장하고 응답한다. 필드의 바이트 위치를 연산마다 직접 계산하지 않는다.

```text
layout Selection {
    start: u32le;
    count: u32le;
    document: bytes;
}

on put {
    let input = view(Selection, request.body);
    let selected = range(input.document, input.start, input.count);
    let saved = store.write(request.key, request.revision, selected);
    reply(saved, selected);
}
```

예제 파일에는 저장한 내용을 조회하는 get도 있다. 별도 저장 디렉터리로 실행한다.

```sh
prototypes/connections/build/pure-connections --program prototypes/connections/examples/selection.pure --store prototypes/connections/build/selections --port 8768
```

다른 터미널에서 UTF-8 텍스트 파일의 선택 구간을 저장한다. `--start`·`--end`는 0부터 시작하는
유니코드 코드 포인트 위치이며 끝은 포함하지 않는다. 바이트 위치 변환과 입력 인코딩은
예제 Python 클라이언트가 맡는다. 사용자에게 보이는 글자 덩어리(grapheme) 단위는 아니다.

```sh
python3 prototypes/connections/examples/select-text.py /path/to/document.txt --key excerpt --start 0 --end 5 --url http://127.0.0.1:8768
```

성공하면 저장 버전과 선택 내용이 출력된다. 이후 갱신에는 `--revision 1`처럼 현재 버전을
지정한다. 서버의 숫자 필드는 바이트 단위다. 예제 클라이언트는 UTF-8 문자 경계에 맞추지만
서버의 `bytes` 필드 자체가 텍스트 인코딩을 검증하는 것은 아니다. 클라이언트는 Python이며
필드 해석·범위 계산·저장·응답을 하는 서버는 네이티브다.

`layout`은 선택한 입출력 경계의 배치 선언이다. 패딩 없이 선언 순서로 필드를 배치하며,
`u32le`, `bytes[N]`, 마지막 가변 `bytes`를 지원한다. 고정 필드만 있는 선언은 접두부를 읽는다.
다른 입력·저장·RAM 표현을 이 형식으로 바꾸도록 강제하지 않는다. 송신자와 수신자의 선언이
맞아야 하며 자동 형식 감지·버전 변환은 없다. 예제는 두 u32 뒤에 원본 UTF-8 바이트를 보낸다.

필드 이름과 타입은 준비 단계에서 접근 위치로 바뀐다. 요청별 이름 검색이나 레코드 객체
할당은 없다. 이 예제에는 요청 시 계산할 범위가 하나 남고 저장·응답이 그 결과를 함께 참조한다.
입력 헤더가 잘렸거나 범위가 틀리면 저장 전에 실패한다.
[필드 연결 계약](../../docs/design/005-field-connections.md).

## 상수 구간 작성과 준비 결과 확인

[storage.pure](examples/storage.pure)는 전체 작업물을 저장·복원하는 기존 동작이다.
[excerpt.pure](examples/excerpt.pure)는 저장된 본문의 일부만 응답한다. 다음 get 정의에서
원본은 최소 80 B여야 하며 실제 응답은 오프셋 32부터 16 B다.

```text
on get {
    let object = store.read(request.key);
    let window = range(object.body, 16, 64);
    let excerpt = range(window, 16, 16);
    reply(object, excerpt);
}
```

준비 결과는 저장소·소켓을 만들지 않고 확인할 수 있다. 서버의 `--program`에 같은 파일을
지정하면 해당 경로로 실행한다. 부분 응답 예제는 전체 JSON을 요구하는 Workbench `pull`용이 아니다.

```sh
prototypes/connections/build/pure-connections --check-program prototypes/connections/examples/excerpt.pure
```

`on put`에서는 저장할 부분을 고르고 같은 입력을 응답에도 연결할 수 있다.
해제·복사 코드를 따로 쓰지 않아도 원본 수신 버퍼를 전송 완료까지 유지한다.

```text
on put {
    let window = range(request.body, 2, 10);
    let value = range(window, 3, 4);
    let saved = store.write(request.key, request.revision, value);
    reply(saved, value);
}
```

두 핸들러가 모두 필요하고 각각 저장 효과는 한 번이다. `range`는 정확한 범위를 검사한다.
사용하지 않은 구간의 실패도 보존하며, 쓰기의 모든 구간 정의는 저장 호출보다 앞에 둔다.
작성 오류는 저장소 초기화 전에 거절한다. 소스는 16 KiB, 핸들러당 정의는 32개까지다.
배치 선언은 4개, 선언당 필드는 16개, 입력 값에 의존하는 구간 계산은 8단계까지다.
이 동적 범위는 PUT 입력에 한정하며 GET의 숫자 필드 값으로 결정하는 범위는 시작 때 거절한다.
GET의 이름 기반 고정 필드·가변 꼬리 구간은 지원한다. 숫자/화면 DSL과 파서는 아직 별개다.
조건 분기·반복·여러 외부 효과·일반 산술은 이번 프로파일에 없다.
[작성 계약](../../docs/design/004-prepared-connections.md).

실제 문서 효과에는 선택형 최종 크기 정책을 쓸 수 있다.
[`bounded-editor.pure`](examples/bounded-editor.pure)의
`store.splice(request.key, request.revision, edit.start, edit.remove, edit.insert, 8)`은
수정 후 문서가 8 B를 넘으면 `limit`을 돌려준다. 현재 크기·제거 범위·삽입 길이는
쓰기 잠금 안에서 같은 버전으로 계산한다. 상한은 작성자가 준비한 상수이며 요청 본문이
정책을 바꿀 수 없다. 기본 `editor.pure`는 기존 객체 상한을 사용한다.
`make check-policy`는 소스·고정 CLI/HTTP/TUI의 허용·거절·리비전·초안 보존을,
`make check-policy-peer`는 선택형 인증 피어까지 같은 정의로 검사한다.
이 기능은 문서 어댑터와 저장소에만 있고 4,632 B 기본 코어와 작업 슬롯은 늘리지 않는다.
일반 숫자 규칙 언어가 완성됐다는 뜻은 아니다.

`--check-program`의 `steps`가 비면 구간 연산은 준비 단계에서 끝난다. `write_step`·`reply_step`이
0이면 정적인 범위 설명을 쓰고, 양수이면 `steps`의 해당 1-based 결과를 사용한다.
`to_end`는 offset부터 원본 끝까지를 뜻한다. `runtime_view_steps`는 준비된 동적 단계 수이며
실행 속도나 CPU 명령 수 측정값이 아니다. 오류가 나면 뒤 단계와 저장 효과는 실행하지 않는다.

## 연결과 소유권

| 경로 | 데이터와 완료의 의미 |
|---|---|
| 수신 → 저장 | 본문 크기만큼 한도를 예약하고 수신한다. 작성한 구간을 쓰기 호출에 전달한다. 응답도 입력을 사용하면 전송 종료까지 원본을 유지하며, 아니면 쓰기 호출 종료 후 반납한다. |
| 저장 → 공개 | 쓰기 잠금 안에서 예상 revision을 확인한다. 새 파일에 메타데이터와 본문을 기록·fsync하고 rename으로 공개한다. |
| 공개 → 응답 | 디렉터리 fsync까지 성공해야 저장 성공으로 응답한다. 공개 후 fsync 실패는 `durability_unknown`이며 롤백으로 표현하지 않는다. |
| 조회 → 전송 | 중간 구간들의 길이 조건과 최종 범위를 같은 파일 버전에서 검사한다. 응답에 필요한 부분만 읽기 전용으로 매핑한다. 별도의 응답 본문 배열을 만들지 않는다. |
| 부분 전송 → 회수 | 헤더·본문의 전송 위치만 전진시킨다. 마지막 바이트를 OS에 넘겼거나 전송이 중단되면 매핑을 해제한다. 상대 앱의 수신 확인은 별개다. |
| 복원 → 상태 공개 | 클라이언트가 UTF-8·기존 작업물 형식·상태를 검증한 뒤 복원한다. 실패하면 로컬 상태와 기억한 원격 revision을 유지한다. |

저장된 버전은 제자리에서 수정하지 않는다. 새 파일로 교체하므로 기존 매핑은 전송이 끝날
때까지 이전 버전을 참조한다. 이는 불변 파일 운용 규칙에 의한 보장이다. 외부 프로그램이
파일을 직접 수정·잘라내는 경우까지 `mmap`이 스냅샷을 보장하는 것은 아니다.

저장 C API의 `pure_store_select`는 준비된 구간과 최소 원본 길이를 받아 필요한 부분만 매핑한다.
기본 `storage.pure`와 Workbench 복원은 전체 작업물을 사용한다. HTTP Range나 저장 파일의
제자리 부분 갱신은 구현한 것이 아니다. 본문 없는 응답도 존재·파일 형식 검사는 수행한다.

파일은 식별자·버전·길이의 24 B 헤더 뒤에 원본 본문을 둔다. 테이블·행·쿼리 계획은 없다.
이 헤더는 공개·복원을 위한 프로토타입 메타데이터이며 새 범용 문서·미디어 형식이 아니다.
Workbench JSON의 의미 검증은 클라이언트가 맡는다. 이 JSON이나 파일 배치를 Pure의 RAM
표현으로 강제하지 않는다. 현재 작성 가능한 연결은 등록된 읽기·쓰기·응답에 한정되며
[모듈 계약 초안](../../docs/design/003-module-contract.md) 전체의 구현은 아니다.

`GET /objects/NAME`, `PUT /objects/NAME`의 제한된 HTTP/1.1 요청 경로다.
이름은 영문·숫자·`_`·`-` 1~64자다. 생성은 `If-None-Match: *`, 갱신은 `If-Match: "revision"`을
요구한다. 성공한 조회·저장은 `ETag: "revision"`을 반환한다. revision은 64비트 양수이며
클라이언트에서도 문자열로 유지한다. 404 없음, 412 충돌, 413 크기/버전 한도,
503 예산 부족/쓰기 잠금, 500 저장 오류를 구분한다. 500의 `X-Pure-Error: durability_unknown`은
새 내용이 이미 공개됐지만 영속화 확인이 실패했다는 뜻이다. 재조회는 현재 내용을 확인할
수 있을 뿐 전원 장애 내구성을 증명하지 않는다. 응답 유실도 저장 취소를 뜻하지 않는다.
요청마다 연결을 닫고 Content-Length만 지원한다. 범용 HTTP/RPC 서버는 아니다.

HTTP를 Pure 간 기본 통신 모델로 확정한 것은 아니다.
[자체 통신 형식 설계](../../docs/design/006-native-protocol.md)는 작성된 연결에서 바이너리
형식을 준비하고 보안 전송을 별도로 선택하는 방향이다. 같은 get/put 효과를 실행하는
[선택형 인증 문서 피어](../../docs/research/030-authenticated-document-peer.md)가 별도로 있다.
이 평문·요청당 연결 종료 HTTP 경로로 QUIC과 성능을 비교하지 않는다.

## 한도와 남은 비용

- 이 절의 HTTP 서버는 `127.0.0.1`에만 바인드하며 인증·TLS가 없다. 인증된 선택형
  문서 피어는 별도 실행물이고 인터넷 공개 배포는 검증하지 않았다.
- 동시 연결은 8개, 헤더는 연결당 4 KiB, 객체는 Workbench 파일 상한인 1,449,984 B까지다.
- 현재 준비 구조체는 검증 환경에서 프로그램당 648 B다. 동적 구간 8개를 위한 공간도
  고정 예약한다. 이름·필드 표는 준비 후 유지하지 않지만 전체 컴파일·실행 메모리의 상한을 뜻하지 않는다.
- `--budget` 기본값은 4 MiB다. 수신 본문 할당과 전송 중 참조하는 본문 길이를 합해 접수한다.
  이는 전체 RSS·매핑 페이지·페이지 캐시·디스크 사용량의 상한이 아니다. 매핑 정렬·페이지
  단위 비용, OS 소켓 버퍼, 클라이언트 JSON·문자열·응답 버퍼도 별도다.
  입력의 일부만 응답해도 원본 수신 할당 전체를 붙잡으므로 해당 예산을 전부 유지한다.
- 헤더와 함께 받은 본문 접두부는 한 번 복사하며 `ingress_prefix_copy_bytes`에 기록한다.
  `borrowed_output_bytes`는 참조한 구간의 누적 길이다. 절약한 RAM의 측정값이 아니다.
  커널의 파일 쓰기·소켓 전송, 페이지 폴트, 클라이언트의 직렬화 비용은 남는다.
- `--timeout-ms` 기본값은 접속부터 10초다. 느린 송수신자는 기한 뒤 종료한다. 파일 작업은
  동기 실행하므로 디스크 호출 중에는 기한을 강제로 집행하거나 취소하지 못한다.
- 매핑을 유지하면 이전 파일의 저장 공간도 남을 수 있다. 작은 범위 참조도 해당 파일 버전의
  디스크 수명을 연장한다. 본문 예산만으로 장기 운용의 디스크 사용량을 보장하지 않는다.
- 쓰기는 디렉터리별 잠금 하나로 직렬화한다. C API 제어 호출은 단일 스레드를 전제로 하며,
  이 잠금 규칙을 따르는 여러 프로세스의 충돌을 처리한다. 다중 객체 트랜잭션은 없다.
- 공개 전 프로세스가 종료되면 `.pending-*` 파일이 남을 수 있다. 읽기는 이를 무시하며
  자동 정리·디스크 복구 정책은 아직 없다. 본문 손상 검출용 체크섬도 없다.
- macOS 실행, 실제 전원 장애·디스크 고장은 검증하지 않았다. 파일을 이 어댑터 밖에서
  제자리 수정하지 않는 로컬 저장소를 전제로 한다.

## 검증

Python 3와 Node가 필요하다.

```sh
make -C prototypes/connections check
make -C prototypes/connections check-portable
make -C prototypes/connections check-portable-tui
make -C prototypes/connections BUILD=build/native-checkpoint check-native-checkpoint
make -C prototypes/connections check-portable-peer
make -C prototypes/connections check BUILD=build/sanitized CFLAGS='-std=c17 -O1 -g -Wall -Wextra -Wpedantic -Werror -fsanitize=address,undefined -fno-omit-frame-pointer'
```

`check-portable`은 C 단위 동작을 검사하며 Linux AArch64의 GNU `size`/`readelf`와
RSS 상한 검사를 요구하지 않는다. `check-portable-tui`는 고정·소스 정의 편집기를
실제 PTY에서 검사하며 `/proc` RSS를 읽지 않는다. `check-native-checkpoint`는
둘을 실행하고 고정·소스 CLI의 실제 1 MiB 생성·읽기·1 B 수정 RSS를 호스트
단위에 맞춰 기록한다. [기록된 Linux 결과](../../docs/research/060-host-native-checkpoint.md)는
코어 코드 증가 없이 기존 RSS 상한 검사를 통과했다. 현재 통과 결과는
Linux AArch64에만 해당한다. 기존 `check-tui`·`check-source-tui`의 Linux
4,096 KiB RSS 상한 검사는 그대로 유지한다. Apple Silicon 실행과
디렉터리 동기화·전원 장애 내구성은 별도 검증이 필요하다.
`check-portable-peer`는 OpenSSL이 준비된 경우에만 선택해서 실행한다. C 피어
클라이언트·두 서버를 실제 TLS 소켓으로 검증하지만 `/proc` RSS는 수집하지 않는다.
[휴대형 피어 검사와 Linux 비용](../../docs/research/058-portable-authenticated-peer-check.md)을
별도로 기록했으며 macOS에서의 통과 결과는 아직 없다.

임시 디렉터리와 실제 루프백 TCP 소켓을 사용한다. 단위 검사에는 범위 접근·페이지 경계·
이전 참조 유지·공개 전후 실패 주입·공개 전 자식 프로세스 종료가 포함된다. 주입 훅은
단위 검사 실행 파일에만 들어간다. 실제 두 서버의 쓰기 경쟁과 Workbench 저장·복원 등
작성에 따른 부분 읽기·쓰기·응답, 한글·이모지 문서 선택, 필드 재배치, 잘린 입력·동적 범위·
수명 보존을 포함한 28개 통합 시나리오를 일반 빌드와 ASan·UBSan 빌드에서 확인했다.
준비된 구간과 순차 범위 연산, 숫자 필드의 정렬·바이트 순서·경계 동작도 대조했다.

[검증 자료](results/2026-09-23-field-connections/README.md). 시간·처리량 비교는 수행하지 않았다.
상수 구간 작성 자료는 [020](results/2026-09-23-prepared-connections/README.md)에 있다.
작성 기능 도입 전 자료는 [019](results/2026-09-23-byte-storage/README.md)에 있다.
이전 SQLite 구현의 결과는 [018 자료](results/2026-09-23-native-connections/README.md)에
별도로 보존했다.

## 이번 변경의 판단 기준

이름 기반 필드·데이터 의존 범위를 실제 저장·응답으로 연결하는
[착수 기준](../../docs/design/005-field-connections.md)을 확인했다. 다음에는 앱의 읽기·변경·저장
경로에서 필요한 연산과 버전 관계를 다룬다. 범용 그래프 실행기나 자동 재시도·피드백 루프를
검증 없이 추가하지 않는다.
