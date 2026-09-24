# 문서 부분 변경과 micro 비용 경계

2026-09-23, Linux AArch64 / GCC 14.2.0. 정확성·구조 비용 검증이며 성능 순위 자료가 아니다.

- `native.json`, `sanitized.json`: 각 36개 통합 시나리오, 실행 코어와 splice 단위 검사, 바이너리·소스 해시.
- `core.json`: 실행 전용 아카이브의 섹션·심볼·구조체 비용과 검사 상한.
- `boundary-checks.json`: 코어만 선택한 빌드의 파일, 비용 증가 시 검사 실패, 전체 CLI/서버 ELF 비용.
- `verification.json`: 두 빌드의 소스와 바이너리 확인, 서버 종료 시 회수 확인.
- `sources/`: 해시가 일치하는 소스·기존 클라이언트 37개. 검증에 사용한 Workbench Wasm도 포함한다.

실행 명령 (`prototypes/connections` 기준):

```sh
make all
make check EVIDENCE=results/2026-09-23-document-edits/native.json
make check BUILD=build/sanitized CFLAGS='-std=c17 -O1 -g -Wall -Wextra -Wpedantic -Werror -fsanitize=address,undefined -fno-omit-frame-pointer' EVIDENCE=results/2026-09-23-document-edits/sanitized.json
make check-core CORE_EVIDENCE=results/2026-09-23-document-edits/core.json
```

기존 결과를 덮어쓰지 않는다. 재현 시 출력 인자를 생략하거나 새 경로를 사용한다.
동결 소스의 `sources/prototypes/connections`에서도 같은 빌드·검사가 가능하다.
C17 컴파일러·make·Python·Node·로컬 TCP/자식 프로세스 실행이 필요하며, 크기 검사는
GNU 호환 ar/nm/size를 사용한다. ASan 계측물에는 release 크기 상한을 적용하지 않는다.

1 MiB 문서 편집의 11 B는 입력 크레딧이다. 파일을 재작성하는 4 KiB 호스트 스택 버퍼,
커널·파일·allocator·파서·서버 비용을 포함한 전체 메모리라고 해석하지 않는다.
[구현, 수치 범위와 한계](../../../../docs/research/023-document-edits.md).
