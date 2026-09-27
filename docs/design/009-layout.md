# 009 — 배치: 코어와 고르는 부품

2026-09-27. 저장소 경계와 범위가 일관되지 않다는 지적에서 정한 것.

## 문제

- README의 원칙은 "항상 있는 코어는 실행, 소유권, 자원 한도, 완료 규칙만 다룬다. 프로토콜, 저장소 구현, UI, 코덱은 앱에
  맞게 고른다"였다. 그런데 소스는 원칙과 다르게 놓여 있었다.
- 지금 세대의 코어(모듈 검증·실행 `module*.c`), 트리 저장소(`store-tree.c`), HTTP 호스트(`serve.c`, `deliver.c`,
  `message-json.c`)가 모두 `prototypes/connections/` 한 디렉터리에 이전 세대(000–077의 문서 런타임: `program.c`,
  `store.c`, `server.c`, 편집기들)와 섞여 있었다. 이름만 보면 저장소는 형식 매직(`PURETRE2`)으로만 드러났고, 코어가
  어디서 끝나는지 알 수 없었다.
- 소비자는 소스 파일 목록으로 연결했다. Vega는 Pure의 `.c` 여섯 개를 경로로 나열해 컴파일했다. Pure에 파일이 하나
  늘면 Vega가 깨진다.
- 반면 사람을 위한 인터페이스(Vega)는 따로 된 저장소였다. 저장소와 네트워크는 "호스트 안"처럼 보이고 UI만 부품처럼
  보였다.

## 규칙

1. **역할로 나누고 역할로 부른다.** 코어, 저장소, HTTP 호스트, 명령행 호스트. 형식 이름(`PURETRE3`, `PUREREC1`)은
   형식의 이름일 뿐 부품의 이름이 아니다.
2. **부품은 라이브러리와 헤더로 쓴다.** 소비자는 파일 목록이 아니라 `libpure.a`와 `core/`의 헤더로 연결한다.
3. **저장소(git) 경계는 결합도로 정한다.** 함께 바뀌는 것은 한 저장소에 두고 디렉터리로 나누며, 따로 바뀌는 것은 저장소를
   나눈다. 부품이 된 디렉터리는 나중에 `git subtree split`으로 그대로 떼어 낼 수 있다.

## 배치

| 디렉터리 | 역할 | 만드는 것 |
|---|---|---|
| `core/` | 실행·소유권·한도·완료(`runtime.c`, `flow.c`), 앱 모델(`module*.c`: 실행 전에 검증한 모듈, 트랜잭션인 이벤트, draft인 상태), 코어가 저장소에 요구하는 것(`state.h`, `store.h`) | `build/libpure.a`(저장소와 함께) |
| `store/` | 트리 저장소. 한 파일, 커밋은 한 연속 쓰기와 동기화 한 번 | `libpure.a` 안 |
| `serve/` | `pure-serve`: HTTP 호스트, 나가는 전달, 제자리 JSON. OpenSSL을 쓰는 부분(https 수신자로의 TLS, 봉인한 전달)은 따로 두어 쓰는 서버만 매핑한다 | `build/pure-serve`, `build/pure-crypto.so` |
| `cli/` | 명령행 호스트 | `build/pure-run`, `build/pure-edit-module` |
| `prototypes/` | 이전 문서 런타임. `core/`와 `store/`를 가져다 빌드한다 | `make -C prototypes/connections` |

OpenSSL은 pure-serve가 쓰는 것 중 가장 크다. 링크만 해도 아무 일도 하지 않는 프로세스의 상주 메모리가 2.3 MiB 늘고,
그중 0.66 MiB는 그 프로세스만의 것(재배치한 데이터)이다. 그래서 OpenSSL을 쓰는 코드(`serve/crypto-openssl.c`)는
`pure-crypto.so`로 따로 빌드하고, pure-serve는 https 수신자나 봉인을 처음 쓸 때 실행 파일 옆의 그것을 연다
(`serve/crypto.c`). 쓰지 않는 서버는 유휴 RSS가 4.5 → 2.3 MiB, 자기만의 메모리가 1.3 → 0.86 MiB다. 정적 링크는 RSS가 조금
줄지만(3.8 MiB) 자기만의 메모리가 2.3 MiB로 는다. OpenSSL의 표를 모두 재배치하기 때문이다.

검사도 부품을 따른다. 부품의 단위 검사는 그 옆에(`core/test-module.c`, `store/test-tree-store.c`), 저장소 전체를 끝에서
끝까지 보는 검사는 `tests/check.py`에 둔다. aup와 Vega도 같다(aup `compiler/`, Vega `lib/`·`surfaces/`·`symbols/`, 둘 다
`tests/check.py`). pure-serve의 HTTP 검사는 전에 aup의 검사 안에 있었다. 호스트를 바꾸면 aup에서 확인해야 했던 것이다.
이제 Pure의 `make check`가 aup로 앱을 빌드해 돌린다(Vega가 aup와 pure-serve를 쓰는 것과 같은 방향이다).

저장소 계약(`state.h`, `store.h`)은 코어 쪽에 둔다. 코어가 필요한 것을 정하고 저장소 구현이 그것을 채운다. 트리
저장소는 그 계약의 한 구현이다(이전 세대의 `store.c`, `store-paged.c`도 `store.h`의 구현이다).

## 왜 저장소를 따로 된 git 저장소로 떼지 않았나

트리 저장소를 바꾼 최근 커밋 12개 중 9개가 계약(`state.h`, `store.h`)을 바꿨고, 그중 5개는 같은 커밋에서 코어나 호스트도
바꿨다. 스냅숏(`571824a`: 저장소 동결 + 모듈 리스트 공유 + 모듈 스냅숏), 대여(`1a3586d`: 저장소 익스텐트 + serve), 그룹
커밋(`d4b01a7`), 샤딩(`694b60f`), 매핑을 나누는 핸들(`89d1777`: + 모듈 파일 읽기). 계약이 아직 코어와 함께 움직이고
소비자가 Pure 하나뿐이다. 따로 된 저장소면 이런
변경마다 두 저장소의 버전을 맞춰야 한다. 두 번째 소비자(예: 메신저의 메시지 저장)가 생기거나 계약이 멈추면 `store/`를
그대로 떼어 낸다.

## 왜 Vega는 따로이고 이름이 pure-ui가 아닌가

Vega는 두 가지다. aup 라이브러리(`use vega`: 목록·표·포커스·톤 같은 인터페이스 어휘)와 그 어휘를 사람에게 보이는
표면들(TUI, CLI, NUI, 웹, 심볼과 GPU)이다. 어휘는 aup의 언어 기능과 함께 바뀌고, 표면은 Pure의 모듈 API(`libpure.a`)
위에 있다. Vega의 최근 커밋은 모두 Vega 안에서 끝났다. `pure-serve`가 Pure 안에 있는 것은 프로그램을 위한 표면(보기가
GET, 입력이 POST)에는 어휘가 필요 없기 때문이다. 모듈 형식만으로 정해진다. 사람을 위한 표면은 어휘가 필요하다. JDK가
`java.net`과 HTTP 서버를 담고 JavaFX를 따로 두는 것과 같은 선이다. `pure-ui`라는 이름은 UI가 런타임의 일부라는 뜻이
되어, 런타임이 화면을 모른다는 분리와 어긋난다.

## 비용

같은 소스를 옮기기 전(`5239052`)처럼 한 명령으로 빌드한 것과 라이브러리로 링크한 것을 번갈아 쟀다(Pi 5, 점수판 앱,
연결 스레드 3, 라운드 6번씩 3초, 요청당 CPU). 라이브러리라는 구조의 비용은 없다. 한 명령 빌드도 오브젝트 순서를 라이브러리
빌드와 같게 하면 크기(228,041 B)와 효율이 같다(total +0.4%, page +0.2%). 남은 차이는 코드 배치다. `runtime.c`를 예전처럼
마지막에 링크하면 예전 순서와 total −0.4%, page +1.2%, mixed −0.1%로 같은 빌드끼리의 라운드 차이(±1%) 안이다. 크기는
5,008 B 줄었다. 쓰지 않는 `flow.c` 함수(516 B)를 라이브러리에서 끌어오지 않고, 예전 바이너리에 있던 링커의 Cortex-A53
오류 회피 분기 자리(4 KiB 정렬 여백)가 없다. 벤치마크의 `--baseline`으로 다시 잴 수 있다.

## 확인

최상위 `make check`(저장소와 앱 모델의 검사), `prototypes/connections`의 `make check`·`check-tree`·`check-core`, aup
`make check`, Vega `make check`가 새 배치에서 통과한다. Vega는 이제 `libpure.a`를 링크한다.
