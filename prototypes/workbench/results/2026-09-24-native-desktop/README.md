# Linux 네이티브 작성 창 검증 — 2026-09-24

이 폴더는 성능 벤치마크가 아니라 실제 GTK 창의 동작, 실행 전용 비용과 선택형 편집기 비용의
구분을 기록한다. 환경·바이너리 해시·섹션 크기·프로세스 메모리 관찰 값은 `manifest.json`에,
재실행 가능한 네이티브/Wasm 동작 비교와 C 호스트 sanitizer 결과는 `native-parity*.json`에 있다.
`core-budget.json`은 기존 6 KiB 코어 한도 재검사다. `sources/`는 검증 당시 변경 파일 사본이다.

Xvfb 1400×800×24, `GTK_A11Y=none GDK_BACKEND=x11 GSK_RENDERER=cairo GDK_DISABLE=gl,vulkan`에서
GTK 창을 열었다. `xdotool`로 다음을 실제 창에 보냈다: 도형 누름·이동·놓기, 저장, `GtkTextView`
끝에 새 `text` 출력 추가와 적용, 잘못된 선언 적용 및 수정, 다시 저장, 900×600으로 크기 변경.
새 CLI 프로세스에서 `load` 결과를 확인하고, 새 창에서도 수정된 소스와 저장 상태를 복원했다.
수치 입력은 별도 `document.pure` 창에서 99를 넣어 작성 규칙의 저장 결과 80을 확인했다.

화면 자료:

- `drag-and-save.png`: 놓은 위치와 저장 성공.
- `rejected-edit.png`: 잘못된 선언을 거절하고 직전 실행 화면을 유지.
- `recovered-edit.png`: 소스 복구 후 새 정의의 상태를 저장.
- `resized.png`, `resized-input.png`: 축소 후 화면 및 별도 드래그·키 입력.
- `reopened-state.png`: 새 프로세스의 새 정의·상태 복원.
- `numeric-input.png`: native 값 입력 및 상태 저장.

화면은 `/tmp`에서 빌드한 `capture-x11.c`로 X 서버의 실제 픽셀을 PNG로 기록했다.
최종 작성 소스는 `authored-after.pure`에 있다. 프로세스 RSS/PSS는 작업 완료 후
`/proc/<pid>/status`와 `smaps_rollup`에서 관찰했다. 컴파일러 자식 프로세스·Xvfb·운영체제
전체 메모리를 합친 값이 아니며, macOS의 비용도 아니다.

선택 빌드 확인:

```sh
python3 prototypes/workbench/manage.py native prototypes/workbench/examples/routed-layout.pure --zig /path/to/zig
python3 prototypes/workbench/manage.py native prototypes/workbench/examples/routed-layout.pure --desktop --zig /path/to/zig
node prototypes/workbench/native/check.mjs
node prototypes/workbench/native/check.mjs --sanitize
```

CLI의 직접 공유 라이브러리 의존성은 libc 하나였다. GTK 작성 창에는 GTK/Pango/Cairo/GLib가
추가된다. 문서의 해석과 남은 범위는 [025 보고서](../../../../docs/research/025-native-authoring-shell.md)에 있다.
