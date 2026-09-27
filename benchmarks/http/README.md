# HTTP 벤치마크

Pure의 문서 서버들과 같은 계약의 axum(Rust)·drogon(C++) 서비스를 같은 기기·코어 배정·부하 생성기로 잰다
([075](../../docs/research/075-http-host-against-axum.md)).

```sh
cc -O2 -std=c17 -o load load.c
(cd axum-baseline && cargo build --release)
drogon-baseline/build.sh   # Debian의 drogon 패키지를 설치하지 않고 .deps에 풀어 빌드한다
make -C ../../prototypes/connections build/pure-serve
make -C ../../prototypes/connections BUILD=build/tree STORE=store-tree.c build/tree/pure-connections
aup build ../../../aup/examples/editor.aup -o editor.pmod
python3 run.py --module editor.pmod --directory /var/tmp   # 저장소를 둘 장치에서
```

저장 보장 쓰기는 장치 상태가 같을 때만 비교된다. SD 카드는 동기 쓰기가 이어지면 느려지므로 실행 순서만으로
결과가 몇 배 달라진다. 쓰기를 비교할 때는 라운드를 여러 번 돌린다. 라운드마다 서버 순서를 회전하고, 서버마다
새 저장소를 쓰며, 쓰기 작업 직전에 장치의 fdatasync 지연(제자리 4 KiB)을 재서 결과 옆에 적는다. 표는 라운드의
중앙값이다.

```sh
python3 run.py --module editor.pmod --workloads put --rounds 5 --seconds 3 \
    --only pure-serve,pure-serve-3,axum-durable,axum-atomic
```

앱 벤치마크([077](../../docs/research/077-apps-as-http-services.md))는 aup로 쓴 점수판(`scores.aup`)을 pure-serve가 보기와
입력으로 내놓는 것과, 같은 점수판을 axum·drogon으로 손수 쓴 것을 비교한다.

```sh
aup build scores.aup -o scores.pmod
python3 run.py --module scores.pmod --suite app --rounds 3 --seconds 4 \
    --only pure-serve,pure-serve-2,axum-memory,axum-durable,drogon-memory,drogon-durable
```

JSON 본문의 비용은 상태 없는 앱 [echo.aup](echo.aup)에 `load PORT 연결 초 json 1 keep-alive 본문바이트`로 잰다
(`{"name":"k","text":"x…"}`를 `POST /inputs/echo`로, [082](../../docs/research/082-json-in-place.md)).

앱 벤치마크의 마지막 작업(`mixed`)은 내구성 점수 POST 8연결과 합계 GET 56연결을 부하 생성기 둘로 함께 보내, 쓰기가
동기화하는 동안 읽기가 기다리는지 본다([080](../../docs/research/080-snapshot-reads.md)). 두 행(posts, reads)으로
나오고, CPU 1초당 답은 두 행을 합친 것이다.
