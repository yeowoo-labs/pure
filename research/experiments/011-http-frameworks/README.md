# 011 — HTTP 프레임워크 비교 원시 결과

[docs/research/075](../../../docs/research/075-http-host-against-axum.md)의 5단계 표의 원시 값이다.
`benchmarks/http/run.py --rounds 3 --seconds 3`(서버 순서를 라운드마다 회전)으로 Pure `156e1ad`,
axum 0.8.9, drogon 1.9.0을 같은 Raspberry Pi 5(서버 CPU 0–2, 부하 CPU 3, 저장소 ext4 SD)에서 쟀다.
한 줄이 (서버, 작업, 라운드) 하나이며 `ok_per_cpu_second`는 서버 CPU 시간(모든 스레드, user+system)
1초당 성공 응답 수다.
