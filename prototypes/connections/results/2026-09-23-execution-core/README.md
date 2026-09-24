# 실행 코어 검증 기록

2026-09-23, Linux AArch64, GCC 14.2.0. 첫 공개 체크포인트 진행 중의 증거다.

- `native.json`: 일반 빌드의 실제 TCP 28개 시나리오, 실행 코어의 수명·예산·세대·내부 전달·실제 저장 검사.
- `sanitized.json`: 같은 경로를 ASan·UBSan 및 기본 누수 검사로 통과한 결과.
- `verification.json`: 두 결과의 소스 일치, 바이너리 해시, 종료 시 자원 회수, 코어 외부 심볼과 크기 기록.
- `sources/`: 결과에 기록된 30개 소스·클라이언트 파일의 해시 검증 후 복사본.

당시 실행 명령은 `prototypes/connections`에서 다음과 같다.

```sh
make all
make check EVIDENCE=results/2026-09-23-execution-core/native.json
make check BUILD=build/sanitized CFLAGS='-std=c17 -O1 -g -Wall -Wextra -Wpedantic -Werror -fsanitize=address,undefined -fno-omit-frame-pointer' EVIDENCE=results/2026-09-23-execution-core/sanitized.json
size build/runtime.o build/evaluate.o build/pure-connections
nm -u build/libpure-execution.a
```

기존 증거 파일은 덮어쓰지 않는다. 재현할 때는 `EVIDENCE`를 생략하거나 새 경로를 사용한다.
동결된 소스는 `sources/prototypes/connections`에서 `make all`과 `make check`로 빌드·검사할 수 있다.
검사에는 C17 컴파일러·make·Python·Node와 로컬 소켓/자식 프로세스 실행이 필요하다.

영역 192 B, 작업 슬롯 224 B, 티켓 24 B는 구조체 크기다. 코어 오브젝트의 `size text` 합
5,672 B는 전체 RSS가 아니다. 준비 프로그램·작업 슬롯·실제 본문·호스트 자원 등은 별도이며,
서버 구조체는 기능 추가로 이전보다 1,664 B 증가했다. 처리량·지연의 우위를 측정한 자료가 아니다.
동기 효과·단일 소유 스레드·get/put 작성 프로필이라는 현재 한계는
[022 보고서](../../../../docs/research/022-execution-core.md)에 기록했다.
