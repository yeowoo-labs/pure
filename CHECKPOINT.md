# 첫 공개 체크포인트 — 진행 중

목표는 작은 OS의 운영 모델을 가진 네이티브 마이크로 런타임을 실제 앱으로 보여주는 것이다.
작은 코어, 새 문법, 단일 버퍼 실험만으로 완료하지 않는다. 사용자가 직접 작성·변경할 수 있는
앱과 재현 가능한 실행·비용·실패 검증을 함께 공개할 수 있어야 한다.

## 현재 앱 모델과 근거 (2026-09-26)

아래 표의 문서 앱·scalar 앱 기록 뒤로, 앱 모델은 하나가 됐다. 앱은 [aup](/srv/git/aup.git)로 쓰고 Pure가 검증해
실행하는 모듈([008 모듈 형식 v5](docs/design/008-module-format.md))이며, 인터페이스는 [Vega](/srv/git/vega.git)가 같은
명세에서 만든다. 이 절은 그 뒤에 확인한 것만 적는다.

| 영역 | 확인한 것 | 근거 |
|---|---|---|
| 앱 표현 | 상태(수, text와 커서·마크, 레코드의 리스트와 키 있는 리스트, 인덱스), 입력(키, text, 필드 있는 메시지, 타이머), 원자적 이벤트 핸들러, 순수 보기(조건식, 표와 걸러진 표, count·sum·max·min·first), `fn`·`use` 라이브러리, text 비교, 시간(`now`, `every`, `floor`, `%`). 로드할 때 타입·스택·합류·커서와 인덱스의 소속을 모두 검증하고 이벤트의 실행량에 상한이 있다 | 008, aup `docs/design/002-dsl-direction.md`, aup `make check`(파이썬 참조 모델 대조, 진단 132건) |
| 영속 | 선언한 상태가 곧 영속 상태다. 트리 저장소에 커밋당 동기화 한 번(헤드를 바이트 바로 앞에 두어 한 연속 구간), 이벤트 묶음마다 저장 한 번. `kill -9` 뒤 확인 응답한 이벤트가 모두 남는다. 레코드·키가 바뀐 모듈로 바꿔도 데이터가 이름으로 옮겨진다 | [076](docs/research/076-one-sync-commit.md), [081](docs/research/081-tail-records.md), 008 상태 영속·교체 |
| 인터페이스 | 한 명세가 TUI(셀 단위 갱신), CLI, 문장(NUI), 웹 GUI가 된다. 저장하면 실행 중인 앱이 상태를 이어 새 모듈로 바뀐다(`aup watch`, `vega … --watch`) | Vega `docs/design/001-first-component.md`, Vega `make check`(가상 터미널, 헤드리스 Chromium 포함) |
| HTTP 서비스 | 보기가 GET, 입력이 POST다(JSON 본문과 그 안의 그룹 객체를 제자리에서 읽는다, 라우터 코드 없음). 앱의 `send`는 이벤트와 함께 저장되고, `--send 출력=URL`로 다른 앱이나 HTTP 끝점에 순서대로 묶어 전해지며, 받는 앱은 번호마다 한 번 적용한다(서버를 죽였다 살려도 정확히 한 번). `--seal 출력=공개키`면 받는 노드의 키로 봉인해(HPKE, 전달선마다 세션 하나, 보내는 노드의 키로 인증) 사이의 중계가 읽지도 바꾸지도 못하고, 받는 노드는 `--sender 이름=키`로 보낸 이를 가린다(봉인 전달 462k/s, 봉인 없이 468k/s). 답은 저장 뒤에 나간다. 상태를 가진 앱은 소유자 스레드 하나가 입력을 적용하고, 연결 스레드는 마지막 저장의 스냅숏으로 보기를 답한다(쓰기가 동기화하는 동안에도 읽기는 기다리지 않는다) | 008 HTTP 호스트, [077](docs/research/077-apps-as-http-services.md), [080](docs/research/080-snapshot-reads.md), [082](docs/research/082-json-in-place.md), [083](docs/research/083-delivery-over-http.md), [086](docs/research/086-sealed-sends.md), [088](docs/research/088-sealed-sessions.md) |
| 성능(Pi 5) | 2026-09-27, 같은 기기에서 axum·drogon과 함께 쟀다. 문서 서버(한 스레드): 64 B GET 139k/s(CPU 1초당 140k; axum 127k·63k, drogon 147k·63k), 1 MiB GET 2,362/s(axum 1,304, drogon 754), 충돌 안전 PUT은 axum-atomic의 5.1–5.8배. 앱 서비스: 내구성 있는 쓰기 64연결 21.3k/s(axum·drogon 내구 약 200/s), 합계 GET 126k/s·두 스레드 153k(axum 136k, drogon 151k), 20행 페이지 두 스레드 148k(axum 93k, drogon 138k), 쓰기와 섞인 읽기 119k/s(axum·drogon 내구 1.1–1.4k/s). 유휴 RSS 2.3 MiB(axum 2.8, drogon 10.9), 최대 3.2–5.8 MiB(axum 6.6–9.7, drogon 11.9–78.8) | [075](docs/research/075-http-host-against-axum.md), [077](docs/research/077-apps-as-http-services.md), [080](docs/research/080-snapshot-reads.md), [081](docs/research/081-tail-records.md), [087](docs/research/087-small-reads-and-idle-memory.md) |
| 표면과 메신저 | 사람을 위한 표면이 노드 안에서 돈다(`pure-serve --surface`): 마지막 저장의 스냅숏을 제자리에서 읽고, 친 키와 글자는 HTTP 요청과 같은 줄로 소유자에게 간다. Vega의 디스플레이·픽셀이 그 표면이다. 두 노드의 메신저(Vega `examples/messenger.aup`)는 서로에게 봉인·인증해 보내고, 한 줄이 상대 노드에 보이기까지 RAM 저장소에서 p50 0.79 ms·p99 1.1 ms, SD 카드에서 p50 3.5 ms(꼬리는 SD의 동기화) | [설계 010](docs/design/010-surfaces.md), [089](docs/research/089-messenger.md) |

키로 나눈 소유자와 저장을 넘길 스레드는 하지 않기로 했다. 이 장치는 동기화를 한 줄로 세워, 나누면 쓰기가 23% 줄고 넘겨도
기기의 일은 줄지 않는다([084](docs/research/084-parallel-syncs.md)). 대신 압축한 파일이 한 주기의 0을 미리 가져 커밋이 새
블록을 받지 않는다(저장소 커밋 +21%, 저널 커밋 −59%, [085](docs/research/085-cycle-sized-files.md)).

남은 것: macOS 검증, `bool`·`i64`, 압축을 없애는 제자리 재사용 저장소(085 다음), 응답을 기다리는 출력의 묶음 전달(083), 앱
라우트의 인증·TLS(문서 피어의 TLS는 있다), aup·Vega의 배포 묶음.

## 공개할 작업물과 완료 증거

기존 작업물 편집·저장 흐름을 발전시킨 **네이티브 작업물 편집기**를 하나의 검증 앱으로 삼는다.
앱 정의와 현재 상태를 구분하고, 선택한 항목·내용을 변경·저장·복원하며, 다른 실행기와
변경을 주고받는다. CLI의 자동화와 네이티브 UI가 같은 앱 로직을 사용해야 한다.
현재 브라우저 UI나 Python 요청 클라이언트를 네이티브 앱 완성으로 대체하지 않는다.

검증 기기는 로컬 Raspberry Pi 5(Linux AArch64)다(2026-09-25 변경). 아래의 macOS 관련
기록과 [Apple Silicon CI 작업](.github/workflows/native-macos-arm64.yml)은 선택적인 이식성
확인으로 남기며 완료 조건이 아니다. macOS 지원을 주장하지도 않는다.

| 요구 | 완료에 필요한 증거 | 현재 상태 |
|---|---|---|
| Pure가 실행·자원·연결을 소유 | 호스트 타입 없는 실행 코어, 실제 입출력 어댑터, 종료·완료·회수 검증 | 저장과 선택한 scalar 앱 입력·상태·복원을 같은 코어에 연결. [045](docs/research/045-owner-thread-async-effects.md)의 선택형 완료 원시 기능과 [048](docs/research/048-immutable-read-write-overlap.md)의 실제 HTTP 호스트에서 수명·기한·종료·쓰기 중 읽기를 검증했다. 인증 피어 통합과 일반 앱 표현은 미완료 |
| 마이크로 기본 비용 | 실행 전용 구성과 선택 기능의 분리, 코드·메모리·작업별 비용과 전체 앱 비용 공개 | [069](docs/research/069-atomic-authored-size-policy.md)에서 기본 코어 `text` 4,632 B, 유휴 영역 120 B, 작업 슬롯 216 B와 준비된 문서 정의 680 B를 유지하며 실제 문서 크기 정책을 연결했다. 정책을 선택한 소스 CLI/HTTP `text`는 36,135/44,342 B, 고정 CLI/HTTP는 19,268/26,689 B다. 현재 소스·고정 CLI 압축 묶음은 22,862/12,966 B, 고정·소스 CLI+TUI는 32,144/51,398 B이고 모두 이동 후 검증했다. Linux 1 MiB 편집 피크 RSS는 소스 CLI 2,080 KiB, 고정 CLI 2,112 KiB다. 혼합 저장소 HTTP는 456 B 커져 선택형 상한만 검토해 조정했다. HTTP·TLS·GTK는 선택 기능이며 큰 앱의 전체 피크·작업당 비용은 미완료 |
| 작성 가능한 실용 앱 | 사용자가 입력·연산·연결을 수정해 동작을 바꾸고, 새 실행기에서 상태 복원 | [069](docs/research/069-atomic-authored-size-policy.md)의 작성 가능한 문서 크기 정책이 실제 CLI·HTTP·TUI·TLS 효과를 제어하고 새 실행에서 바뀜. 기존 .pure의 네이티브 빌드·실시간 소스 적용·새 프로세스 복원 연결; JSON 프로젝트 변환·큰 앱 표현은 미완료 |
| 네이티브 사용 경험 | 실제 편집·입력·크기 변경·저장·오류 복구를 네이티브 UI에서 실행 | [035](docs/research/035-native-multi-document-window.md)의 Linux GTK 창은 여러 문서·정의 초안과 충돌을 다루지만 [036](docs/research/036-gtk-memory-boundary.md)에서 확인한 약 44 MiB RSS와 전환 중 증가 때문에 저메모리 프로필은 아니다. [044](docs/research/044-terminal-viewport-cost.md)의 선택형 네이티브 TUI는 같은 준비된 문서 앱에서 한국어·여러 행·탭 입력, 키 전환·저장·충돌·크기 변경·1 MiB 작업을 처리한다. [050](docs/research/050-bounded-document-prefix-find.md)은 1,024개 실제 문서의 목록·전환·접두어 탐색을 검사했다. 고정 TUI의 이전 Linux PTY 검사에서 1 MiB 피크 RSS는 2,832~3,168 KiB였다. 최신 고정 CLI+TUI 묶음은 32,144 B다. [053](docs/research/053-source-loaded-native-terminal-editor.md)의 선택형 소스 TUI는 같은 문서 앱을 `.pure`에서 읽고 다른 필드 배치·1 MiB 편집·1,024개 문서 탐색을 실제 PTY에서 확인했다. 현재 소스 CLI+TUI 묶음은 51,398 B다. [065](docs/research/065-native-save-latency.md)는 선택형 페이지 저장소 TUI의 실제 저장 완료 시간·코드·RSS를 검증했다. 본문 검색, 저메모리 GUI, macOS·IME·복합 문자소 처리는 남음. 정의 작성은 별도 작성 호스트가 맡는다 |
| 내부 연결과 Pure 간 RPC | 같은 앱 의미의 내부 연결·원격 연결, 수명·형식·충돌·기한·종료 처리 | [063](docs/research/063-real-in-process-routing.md)에서 실제 1 MiB 문서를 한 프로세스의 두 저장 영역에 추가 입력 버퍼 없이 전달하고 독립 충돌·회수를 검증했다. [030](docs/research/030-authenticated-document-peer.md)의 선택형 TLS 1.3 문서 피어가 CLI·HTTP·창의 같은 `editor.pure` 읽기·변경·충돌·복원을 처리. [055](docs/research/055-authenticated-peer-transport-cost.md)에서 서버 전용 선택 빌드와 같은 보안 전송의 직접 기준선을 추가하고 공통 TCP 지연을 줄였다. [058](docs/research/058-portable-authenticated-peer-check.md)은 인증·1 MiB 문서·실패 시나리오를 Linux 전용 RSS 수집 없이 실행했다. 장기 실행 영역 관리자·macOS·실패망·대규모 부하는 미완료 |
| Pure 외부와의 연결 | 일반 클라이언트와 양방향 입출력, 프로토콜 오류·호환성 검증 | 문서 창·CLI·HTTP·C TLS 피어가 같은 저장 버전과 편집 효과를 공유. 별도 Python TLS 클라이언트가 같은 문서 형식으로 읽고 쓰며 1 MiB 본문을 교환. 실서비스 상호운용성은 미완료 |
| 성능·크기 주장에 대한 근거 | 실제 앱 작업으로 동등한 직접 구현·해당 RPC 기준과 비교, 기능·보안 조건 공개 | [034](docs/research/034-hybrid-document-storage.md)에서 같은 문서 앱의 파일 쓰기 요청량은 큰 문서 약 61.9배 감소, 1 B 문서는 기본과 동일함을 확인했다. 코드 크기는 증가했다. [049](docs/research/049-idle-app-work-slots.md)에서는 실제 네이티브 앱 1,000개의 입력·저장·복원 후 메모리 비용을 비교했다. [040](docs/research/040-document-service-capacity.md)에서는 같은 문서 앱으로 호스트당 1,024개 객체·4,096 HTTP 작업·8동시 연결의 정확성·RSS·회수를 확인했다. [054](docs/research/054-direct-document-path.md)는 같은 저장 작업의 직접 호출과 Pure 경로에서 결과·최종 파일이 같음을 검사하고, Pure 비교 실행물의 `text`가 8,276 B 더 큼을 확인했다. [055](docs/research/055-authenticated-peer-transport-cost.md)는 인증 TLS 문서 읽기에서 직접 서버와 Pure 서버가 같은 결과를 내며, 공통 TCP 지연을 제거하자 짧은 루프백 배치가 크게 단축됨을 보였다. 서버 전용 Pure 빌드의 `text`는 클라이언트 겸용보다 3,472 B 작고, 동일 실험 빌드에서는 직접 서버보다 5,592 B 크다. 직접 기준선은 런타임 전체 계약과 동등하지 않고 속도 우열도 아직 미판정이다. 물리 쓰기량·장시간 서비스 부하·동등 서버 지연·Kitex 수준 성능은 미입증 |
| 공개 가능한 패키지 | 깨끗한 환경의 빌드·실행, 예제 수정 안내, 실패·한도·플랫폼별 검증 자료 | [069](docs/research/069-atomic-authored-size-policy.md)의 현재 소스 묶음을 다른 디렉터리에서 빌드해 C·TUI·실제 문서 RSS·수정한 `.pure` 동작·프로세스 내 실제 문서 전달·선택형 저장 형식 내보내기·Linux 릴리스 검사를 통과했다. 앞선 [059](docs/research/059-immutable-file-copy-path.md) 묶음은 선택형 TLS 피어·Linux 파일 간 복사도 검사했다. [056](docs/research/056-server-only-document-package.md)의 Linux 서버 전용 TLS 묶음은 클라이언트 겸용보다 압축 전체가 1,696 B 작고 두 구성 모두 검토한 상한 안에서 이동 후 인증·1 MiB 문서·복원 작업을 통과했다. 시스템 공유 라이브러리, macOS 빌드·검증, 서명·라이선스와 플랫폼별 설치 절차는 미완료 |

현재 검증하지 못한 성능·격리·자동 최적화 능력을 소개 문구로 대신하지 않는다.
문서 앱과 scalar 앱은 수명·예산 코어를 공유하지만 아직 하나의 작성 가능한
앱 모델이 아니다(이 기록 뒤 모듈 형식 v5와 aup가 하나의 앱 모델이 됐다: 위의 "현재 앱 모델과 근거"). [069](docs/research/069-atomic-authored-size-policy.md)의 숫자
상한은 실제 문서 효과를 제어하는 첫 좁은 정책이지 범용 산술 엔진은 아니다. 저장 전용 해석은 [068](docs/research/068-opaque-app-core.md)에서 선택형 어댑터로
옮겼다. 실제 문서 수정 정책으로 scalar 규칙을 통합할 합격 조건은
[066 경계 조사](docs/research/066-application-boundary.md)에 기록했다.
코어 수치만으로 범용 앱 런타임을 완성했다고 주장하지 않는다.
macOS 저장 어댑터의 `O_NOFOLLOW` 헤더 노출 문제를 고쳤고, Linux 전용 크기·RSS
도구 없이 C 동작을 실행하는 `make -C prototypes/connections check-portable` 경로를
추가했다. Linux AArch64에서 통과했으며 Apple Silicon 빌드·실행은 아직 확인하지 않았다.
실제 터미널 작업을 검사하는 `check-portable-tui`도 Linux AArch64에서 통과했다.
이 경로는 메모리 수치를 수집하지 않으므로 기존 Linux RSS 상한 검사를 대신하지 않는다.
[Apple Silicon CI 작업](.github/workflows/native-macos-arm64.yml)은 네이티브 앱·
터미널·1 MiB RSS·프로세스 내 전달을 실제 macOS arm64에서 검사하도록 작성했다.
현재 Git 원격은 로컬 저장소라 작업 실행 결과가 없으며, macOS 통과로 표기하지 않는다.
일반 OS 전체·모든 미디어 코덱·클러스터 운영기를 한꺼번에 만드는 것도 이 체크포인트의
완료 조건으로 추가하지 않는다. 위 실제 앱에 필요한 연결을 깊게 완성한다.
코어의 4,864 B 검사를 통과하더라도 필수 실행물이나 선택한 앱의 실제 메모리가 커지면
마이크로 목표를 달성한 것이 아니다. 새 프로토콜·UI는 선택 빌드로 분리하고,
전체 앱 비용을 확인한 뒤에만 작은 앱이라는 표현을 쓴다. [043](docs/research/043-micro-package-boundary.md)부터
고정 정의 CLI·HTTP·TUI의 선택별 압축 묶음 크기도 별도 상한으로 검증한다. 작성 기능을
고정 실행 TUI에 중복해서 넣지 않고, 현재의 소스 CLI·선택형 창 경로에서 다룬다.
새 기능은 기본 코어의 `text`·인스턴스 상태, 선택 실행물의 링크 크기,
실제 작업의 전체 RSS와 배포 묶음 증가를 각각 기록한 뒤 받아들인다.
첫 두 항목만 작거나 소스 배포 파일이 작다는 이유로 마이크로라고 부르지 않는다.
기본 `make all`은 코어와 파서 없는 고정 정의 CLI만 빌드한다. 실행 중 정의를
바꾸는 소스 CLI는 `make editor`, HTTP 서버는 `make server`를 선택해야 한다.
프로세스 관리자나 VM 계층도 첫 체크포인트의 필수 비용으로 추가하지 않는다.
[059](docs/research/059-immutable-file-copy-path.md)의 Linux 선택형 파일 복사는
기존 불변 형식의 큰 편집에서 사용자 공간 호출을 줄였지만, 실제 디스크 쓰기량이나
지연 개선을 증명하지 않았으므로 기본 저장 경로로 승격하지 않는다.
[061](docs/research/061-durable-document-edit-latency.md)의 실제 1 MiB 문서
리비전 수정 비교에서는 페이지 저장이 이 Linux/ext4 호스트에서 훨씬 짧은
중앙값을 보였다. 파일 간 복사의 지연 이득은 구분되지 않았다. 페이지 형식은
추가 코드·꼬리 지연·macOS 검증이 남아 선택형으로 유지한다.
[065](docs/research/065-native-save-latency.md)는 같은 효과를 실제 네이티브
TUI의 Ctrl+S부터 저장 완료 표시까지 확인했다. 1 MiB 마지막 1 B 수정의
중앙값은 기본 93.17 ms, 선택형 페이지 22.04 ms였고, 256 B 문서는
8.54·9.23 ms였다. 선택형 소스 TUI는 코드가 4,958 B 크고 큰 문서의 저장
형식도 달라 기본으로 올리지 않았다. 기본 코어와 기본 TUI의 실행 비용은
이 선택형 실험 때문에 늘리지 않는다.
[062](docs/research/062-paged-store-rollback.md)에서 별도 디렉터리로 본문과
리비전을 내보내 기본 저장기로 다시 읽는 경로를 검증했다. 운영 중 투명한
형식 전환을 뜻하지 않는다.

## 구현 순서와 현재 근거

1. **실행 코어와 호스트 책임 분리.** 기존 저장·응답 경로를 그대로 사용해 영역의 작업 접수,
   준비·실행·완료·회수와 내부 결과 전달을 구현한다. 현재 [022](docs/research/022-execution-core.md)에
   기록한 코드와 검증이 있다. 첫 체크포인트 전체 완료가 아니다.
2. **앱 작성 모델 통합.** get/put 전용 프로필을 실제 작업물의 입력·계산·상태·효과에 필요한
   범위로 확장하고, CLI·UI·원격 입력이 같은 준비된 앱을 실행하도록 만든다. [023](docs/research/023-document-edits.md)은
   네이티브 CLI와 HTTP의 같은 부분 변경 효과, 코어 비용 경계까지 구현한 중간 결과다.
   [024](docs/research/024-native-app-instances.md)에서는 기존 Workbench 작성물을 독립된 네이티브
   계산 인스턴스로 실행하고 같은 코어에서 입력·상태·조건부 저장·복원을 연결했다.
   [025](docs/research/025-native-authoring-shell.md)에서는 선택형 Linux 네이티브 작성 창에서
   정의 교체까지 같은 코어의 작업으로 연결하고 CLI·창 비용을 각각 확인했다.
   [026](docs/research/026-secure-peer-link.md)에서는 선택형 인증 피어에서 같은 앱 작업을
   원격으로 실행하고 CLI·피어 비용을 분리했다.
   [027](docs/research/027-fixed-native-capacity.md)에서는 고정 정의의 실제 용량만 예약해
   반복 메모리를 줄이고 11개 작성물의 동작 일치를 확인했다.
   [028](docs/research/028-shared-fixed-program.md)에서는 CLI·피어의 정의를 한 번 링크하고
   인스턴스에는 가변 상태만 남겨 반복 비용을 더 줄였다.
   [029](docs/research/029-native-document-window.md)에서는 기존 문서 편집 효과를 Linux
   GTK 창에 붙여 CLI·HTTP와 같은 문서 변경·충돌·복원을 검증했다. UI 비용은 별도다.
   [030](docs/research/030-authenticated-document-peer.md)에서는 같은 문서 편집 효과를
   인증된 Pure 피어와 일반 Python TLS 클라이언트에 연결하고, 소비하지 않는 응답 본문을
   선택적으로 전송하지 않는 경로와 선택 빌드 전체 비용을 확인했다.
   [031](docs/research/031-relocatable-document-preview.md)에서는 같은 앱을 선택형 Linux
   묶음으로 만들어 다른 디렉터리에서 전체 경로를 재검증하고, 1바이트 편집도 저장 파일
   전체를 다시 쓰는 현재 한계를 계산했다.
   [032](docs/research/032-micro-native-authoring-boundary.md)에서는 문서 창과 정의 적용을
   통합한 뒤, 기본 배포를 CLI만 선택하도록 줄이고 실제 CLI RSS 회귀 검사를 추가했다.
   [033](docs/research/033-paged-document-storage.md)에서는 같은 문서 앱에 선택형
   페이지 저장 경로를 연결해 큰 문서의 쓰기량 감소와 작은 문서의 손해를 함께 확인했다.
   [034](docs/research/034-hybrid-document-storage.md)에서는 작은 값에 기존 파일 형식을
   적용해 그 손해를 제거하고, 늘어난 코드 크기와 역호환 경계를 검증했다.
   [035](docs/research/035-native-multi-document-window.md)에서는 선택형 네이티브 창에서
   여러 문서를 열고 초안·정의·읽기 실패를 보호해 실제 앱 조작 범위를 넓혔다.
   [036](docs/research/036-gtk-memory-boundary.md)에서는 반복 문서 전환의 힙 증가를
   GTK 텍스트 갱신 경계까지 분리했다. 작은 기본 실행물의 비용과 선택형 GUI의
   비용을 혼동하지 않으며, 이 창을 저메모리 목표 달성으로 취급하지 않는다.
   [037](docs/research/037-build-time-prepared-runner.md)에서는 기존 문서 앱의 검증된
   연결을 빌드 시 준비해 파서 없는 실행기를 만들었다. 기존 문서 작업과 세 가지
   정의의 결과 일치를 확인하고, 링크 코드 크기·실제 RSS·이동 패키지 비용을
   별도로 기록했다. 실행 중 작성 기능은 기존 소스 경로에 남아 있다.
   [038](docs/research/038-fixed-http-document-host.md)에서는 같은 준비된 문서
   앱을 파서 없는 HTTP 호스트에도 연결했다. CLI·소스 HTTP와 저장 버전·1 MiB
   본문·충돌을 교차 검증하고, 느린 수신자의 응답 대여가 예산을 차지하다가
   전송 후 회수됨을 확인했다. CLI+HTTP 묶음을 다른 디렉터리에서도 실행했다.
   [039](docs/research/039-idle-http-wakeups.md)에서는 두 HTTP 호스트가 유휴 중
   50ms마다 깨어나던 경로를 연결 기한에 따른 대기로 바꿨다. 신호가 대기 직전에
   도착해 종료가 멈출 수 있는 경합도 Linux `ppoll`의 원자적 마스크로 막았다. 보관된 실행물과
   현재 실행물의 시스템 호출 추적으로 차이를 확인하고 기한·종료·패키지 동작을
   재검증했다.
   [040](docs/research/040-document-service-capacity.md)에서는 같은 문서 앱을
   1,024개 저장 객체와 8개 동시 클라이언트에 적용했다. 소스 정의·고정 정의
   HTTP 호스트에서 각 4,096개 작업의 응답과 최종 문서 해시를 검증하고,
   프로세스 메모리·버퍼 회수·남은 입력 복사량을 기록했다.
   [041](docs/research/041-posix-idle-signal-wakeup.md)에서는 Linux 전용
   `ppoll` 대기를 POSIX 신호 알림 파이프와 `poll`로 교체하고, 추가된 코드·
   파일 디스크립터 비용과 실제 종료·문서 작업을 검증했다. macOS 빌드는 남는다.
   [042](docs/research/042-micro-native-terminal-editor.md)에서는 같은 준비된
   문서 앱을 선택형 저메모리 터미널 편집기로 연결했다. 실제 PTY의 한국어
   입력·문서 전환·저장·충돌·크기 변경·1 MiB 수정과 터미널 복구를 검사하고,
   실행물·프로세스·이동 가능한 CLI+TUI 묶음의 비용을 따로 기록했다.
   [043](docs/research/043-micro-package-boundary.md)에서는 고정 정의 CLI·HTTP·TUI의
   네 가지 선택 묶음 전체 크기에 상한을 적용하고, 모두 다른 위치에서 다시 실행했다.
   [044](docs/research/044-terminal-viewport-cost.md)에서는 실제 TUI의 1 MiB 화면 갱신
   비용을 줄이고, 여러 행·탭 문서를 다시 열지 못하던 결함을 고쳤다. 동일 PTY 화면
   바이트와 편집·복원 결과를 확인하고, 추가 코드·RSS·전체 묶음 비용을 기록했다.
   [045](docs/research/045-owner-thread-async-effects.md)에서는 기본 코어를 키우지 않고
   선택형 비동기 효과 완료 경로를 만들었다. 실제 문서 저장소와 기존 `editor.pure`로
   요청 해제 후 완료, 다른 작업의 선행 완료, 영역 간 본문 전달, 예산·종료를 확인했다.
   [046](docs/research/046-optional-async-document-http.md)에서는 선택형 HTTP 호스트의
   단일 저장 작업자와 완료 파이프를 실제 문서 앱에 붙였다. 지연된 저장 효과 중
   다른 연결의 응답·기한·종료와 1,024개 문서 작업의 결과·메모리 비용을 확인했다.
   [047](docs/research/047-bounded-native-document-browser.md)에서는 저메모리 TUI가
   실제 저장 문서를 목록에서 다시 찾도록 했다. 1,024개 문서의 탐색·전환·회수와
   증가한 TUI 코드·이동 묶음 비용을 검증했고 기본 CLI·실행 코어는 유지했다.
   [048](docs/research/048-immutable-read-write-overlap.md)에서는 기존 문서의 쓰기를
   파일 동기화 직전에 멈춘 동안 이전 버전 읽기와 이후 새 버전 읽기를 검증했다.
   선택형 호스트만 작업자 둘로 늘리고 기본 코어·HTTP 크기를 유지했으며,
   1,024개 문서 작업에서 선택형 호스트의 전체 프로세스 메모리 비용을 기록했다.
   [049](docs/research/049-idle-app-work-slots.md)에서는 동기 네이티브 앱의 작업 슬롯을
   호출 중에만 연결해 유휴 앱 1,000개의 반복 메모리를 줄였다. 코어·스택·실행물
   증가 비용과 저장·복원·피어·TUI의 기존 동작을 함께 확인했다.
   [050](docs/research/050-bounded-document-prefix-find.md)에서는 선택형 TUI의 정렬된
   실제 문서 1,024개에서 추가 인덱스 없이 이름 접두어를 찾아 열었다. 코어를
   늘리지 않고 TUI 코드·RSS·이동 묶음 비용을 확인했으며, macOS TUI 헤더
   노출을 수정했다.
   [051](docs/research/051-optional-runtime-metrics.md)에서는 기본 영역의 누적 계측을
   선택형으로 분리했다. 같은 실제 앱 1,000개의 입력·저장·복원에서 영역 크기,
   전체 RSS와 이동 가능한 CLI·TUI 묶음 비용을 확인했다.
   [052](docs/research/052-clean-native-source-preview.md)에서는 C 실행 경로와 작성
   소스의 재현 가능한 묶음을 만들고, 깨끗한 디렉터리에서 빌드·테스트·실제
   문서 편집과 `.pure` 규칙 수정을 검증했다. macOS 실행과 라이선스는 남는다.
   [053](docs/research/053-source-loaded-native-terminal-editor.md)에서는 같은 TUI가
   선택형 `.pure` 소스 정의를 실행하도록 연결했다. 필드 배치를 바꾼 정의의
   실제 편집·저장, 이동 묶음의 PTY 작업과 선택 비용을 확인했다.
   [054](docs/research/054-direct-document-path.md)에서는 기존 문서 앱의 생성·읽기·
   부분 편집·재읽기를 직접 저장 호출과 Pure 경로로 각각 실행했다. 같은 결과와
   저장 파일을 확인했으며, 실행물 코드 비용과 ext4 지연의 변동을 공개했다.
   [055](docs/research/055-authenticated-peer-transport-cost.md)에서는 같은 정의와
   인증 TLS 경계의 직접 서버를 비교했다. 양쪽 공통 TCP 지연을 제거하고
   서버 전용 선택 빌드의 크기·RSS·보안 시나리오를 검사했다.
   [056](docs/research/056-server-only-document-package.md)에서는 서버 전용 TLS
   피어와 CLI를 선택한 압축 묶음으로 만들고 전체 크기 상한을 적용했다.
   다른 위치에 풀어 외부 클라이언트의 인증·1 MiB 편집·복원 및 CLI 공유를 확인했다.
   [057](docs/research/057-portable-native-terminal-check.md)에서는 실제 PTY 편집
   검사의 Linux 전용 RSS 수집을 분리하고 고정·소스 정의 TUI를 휴대형 경로에서
   실행했다. 소스 묶음의 이동 후 검사와 기존 Linux RSS 상한도 다시 통과했다.
   새 검사는 개발용 Python·Make 경로에만 있으며 C 런타임 코드는 키우지 않았다.
   [058](docs/research/058-portable-authenticated-peer-check.md)에서는 선택형
   TLS 피어 검사에서도 Linux 전용 RSS 수집을 분리했다. 범용·서버 전용
   C 피어와 외부 클라이언트의 인증·문서·실패 작업을 실제 소켓과 이동한
   소스 묶음에서 실행하고 기존 Linux 코드·RSS 비용을 별도로 확인했다.
   [059](docs/research/059-immutable-file-copy-path.md)에서는 기존 1 MiB
   불변 문서의 1 B 수정에서 원본 읽기·새 파일 쓰기 호출을 추적했다.
   Linux 파일 간 복사를 선택하면 같은 결과·프로세스 RSS로 호출 수를
   줄였으며, 기본 코어는 그대로 두고 선택형 코드 증가를 기록했다.
   [060](docs/research/060-host-native-checkpoint.md)에서는 기존 C·실제 PTY 검사와
   고정/소스 CLI의 1 MiB 문서 생성·읽기·1 B 수정을 한 명령으로 재현하도록 했다.
   Linux AArch64에서는 통과했고 기존 RSS 상한도 유지했다. 측정 도우미는 macOS
   `wait4` 원시 단위를 KiB로 정규화하지만 Apple Silicon 실측은 아직 없다.
   [061](docs/research/061-durable-document-edit-latency.md)에서는 같은 앱의
   1 MiB 내구성 문서 편집을 기본·파일 간 복사·페이지 저장으로 직접 실행했다.
   각 방식 18회와 연속 70회 수정에서 결과·리비전·저장 공간·완료 시간·
   페이지 재구성을 확인하고 선택형 저장 전략의 이득과 제한을 분리했다.
   [062](docs/research/062-paged-store-rollback.md)에서는 선택형 페이지 저장소의
   문서 본문·리비전을 기존 기본 형식의 새 저장 디렉터리로 내보냈다.
   실제 다섯 문서와 1,024개 추가 문서의 재읽기, 변환 중 쓰기 차단·실패 시
   미공개·이후 기본 저장기 수정까지 확인했다. 런타임 코드는 그대로다.
   [063](docs/research/063-real-in-process-routing.md)에서는 실제 1 MiB 문서를
   한 프로세스의 두 독립된 Pure 영역으로 전달했다. 대상 입력 할당기를
   비활성화해도 결과가 같고, 원본 티켓 해제 후 수명·부분 충돌·예산 거절·복원·회수를
   확인했다. 코어 변경 없이 선택형 네이티브 명령만 추가했다.
   [064](docs/research/064-release-callback-reentrancy.md)에서는 자원 해제 콜백이
   같은 영역에 재진입해 슬롯 회수 상태를 바꿀 수 있던 경로를 막았다. 기본
   코드는 72 B 늘었으나 6 KiB 상한과 영역 128 B를 유지했고, 실제 앱·
   새 소스 묶음의 검증을 통과했다.
   [065](docs/research/065-native-save-latency.md)에서는 기존 선택형 페이지
   저장소를 실제 네이티브 TUI에 연결해 Ctrl+S 완료 시간·코드·RSS를 비교했다.
   기본·선택형 CLI+TUI를 각각 이동해 검증하고, 선택형 경로도 깨끗한 소스
   빌드에서 다시 실행했다. 기본 코어와 기본 저장 경로는 유지했다.
   [066](docs/research/066-application-boundary.md)에서는 문서의 저장 전용
   연산과 scalar 앱의 이벤트 우회 연결이 아직 하나의 작성 모델이 아님을
   코드에서 확인하고, 기존 문서 수정에 계산 정책을 결합할 합격 조건을 정했다.
   [067](docs/research/067-work-slot-layout.md)에서는 활성 작업 슬롯을
   224→216 B로 줄이면서 기본 코어 코드를 5,980 B로 유지했다. 비동기·
   네이티브 C 앱·TUI·실제 문서 전달을 검증하고 당시 소스 묶음과 네 가지
   실행 묶음을 다른 위치에서 확인했다. macOS 실행은 남아 있다.
   [068](docs/research/068-opaque-app-core.md)에서는 문서 해석을 기본 코어 밖의
   선택형 어댑터로 옮겼다. 기본 코어 `text` 4,632 B, 유휴 영역 120 B를
   검증하고, 선택형 HTTP의 증가 비용까지 공개했다. 동기·비동기 서비스와
   인증 피어의 실제 문서 작업, 이동한 새 소스·실행 묶음을 검사했다.
   [069](docs/research/069-atomic-authored-size-policy.md)에서는 작성한 최종 문서
   크기 상한을 같은 저장 버전의 쓰기 잠금 안에서 검사했다. 기본 코어와
   준비된 정의 크기를 늘리지 않고 CLI·HTTP·TUI·인증 피어의 결과와
   선택형 호스트 코드 비용을 함께 검증했다. 일반 계산 모델은 남아 있다.
   [070](docs/research/070-micro-default-build.md)에서는 기본 빌드를 코어와
   파서 없는 고정 정의 CLI로 제한하고 깨끗한 소스 추출에서 선택 기능이
   기본 출력에 섞이지 않는지 확인했다. 링크 코드가 줄었지만 RSS 개선은
   입증하지 않았다.
   [071](docs/research/071-logical-page-portability.md)에서는 선택형 페이지
   저장의 16 KiB 논리 페이지와 OS 페이지를 구분해, OS 페이지가 16 KiB를
   나누면 초기화를 허용하도록 좁게 바꿨다. 현재 16 KiB Linux의 실제 문서·
   TUI·형식 내보내기는 통과했다. 4 KiB Linux와 macOS의 CI는 작성만 했고
   실행 결과는 아직 없다.
   [072](docs/research/072-qemu-4k-page-store.md)에서는 실제 4 KiB AArch64
   Linux 커널을 QEMU에서 부팅해 선택형 저장소 단언과 1 MiB 고정 정의 CLI의
   생성·수정·충돌을 통과했다. tmpfs 게스트이므로 물리 디스크 내구성·지연이나
   macOS 결과까지 증명하지 않는다.
3. **실제 네이티브 앱과 통신.** 작성·편집·저장·복원·충돌 처리를 연결하고 자체 통신 형식과
   검증된 보안 전송을 붙인다. 외부 호환 입력·출력도 유지한다.
4. **규모·실패·사용성 확인과 공개 자료.** 큰 작업물·느린 소비자·혼합 작업에서 구조가
   유효한지 확인한다. 기능·정확성·필요한 보안을 유지한 비교만 수행하고 약한 부분을 수정한다.

각 단계는 앞 단계의 실제 작업물을 확장한다. 새로운 빈 데모나 별개의 미니 언어를 반복해서
만들지 않는다. 목표를 한 번의 작업 분량이나 이미 통과한 테스트 범위로 축소하지 않는다.
