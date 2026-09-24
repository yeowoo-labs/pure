# 네이티브 인스턴스와 기존 작성 동작의 연결

2026-09-23, Linux AArch64 / Zig 0.15.2 / GCC 14.2.0.

- `native.json`, `sanitized.json`: 작성물 9개, 입력 156단계의 native/Wasm 상태·표시 비교,
  새 프로세스 복원, 동일 프로세스 인스턴스 3개의 독립성·충돌·실패 검사와 소스 해시.
- `browser-tests.log`: 기존 브라우저/Wasm 및 Node CLI 검사 87개.
- `browser-build.json`: Zig 의미 검사 37개와 Wasm 빌드 기록.
- `application-build.json`: 사용자가 실행할 수 있는 네이티브 라우팅 예제 빌드 기록.
- `verification.json`: 결과의 소스·실행물 확인, 선택 모듈과 전체 실행물 비용.
- `default-linkage.json`: 초기 기본 panic/compiler-rt 설정의 링크 전 아카이브 기록. 최종 앱 크기와 비교하는 수치가 아니다.
- `sources/`: 결과에 기록한 구현과 재현에 필요한 작성물·검사 소스.
- `auxiliary-sources.json`: 재현용 작성물·기존 검사 소스의 별도 해시 목록.

저장소 루트에서 실행한 빌드:

```sh
python3 prototypes/workbench/manage.py build --zig /path/to/zig-0.15.2
python3 prototypes/workbench/manage.py native prototypes/workbench/examples/routed-layout.pure --zig /path/to/zig-0.15.2
```

그 뒤 `prototypes/workbench`에서:

```sh
node --test --test-reporter=spec *.test.mjs
node native/check.mjs --output results/2026-09-23-native-instances/native.json
node native/check.mjs --sanitize --output results/2026-09-23-native-instances/sanitized.json
```

기존 결과는 덮어쓰지 않는다. 재현할 때 `--output`을 생략하거나 새 경로를 사용한다.
동결 소스의 `sources`를 저장소 루트로 삼아 같은 명령을 실행할 수 있다. Node·C17 컴파일러·
Zig 0.15.2 및 로컬 자식 프로세스 실행이 필요하다. 비용 확인에는 GNU ar/nm/size/strip/readelf를 사용했다.

sanitizer는 C 호스트에 적용됐다. Zig 모듈은 두 빌드 모두 ReleaseSafe다. 네이티브 창·macOS·
스레드 간 동시 실행·처리량·RSS 검증 자료는 아니다.
[구현과 남은 범위](../../../../docs/research/024-native-app-instances.md).
