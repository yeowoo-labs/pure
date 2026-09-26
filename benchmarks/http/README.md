# HTTP 벤치마크

Pure의 문서 서버들과 같은 계약의 axum 서비스를 같은 기기·코어 배정·부하 생성기로 잰다([075](../../docs/research/075-http-host-against-axum.md)).

```sh
cc -O2 -std=c17 -o load load.c
(cd axum-baseline && cargo build --release)
make -C ../../prototypes/connections build/pure-serve
make -C ../../prototypes/connections BUILD=build/tree STORE=store-tree.c build/tree/pure-connections
aup build ../../../aup/examples/editor.aup -o editor.pmod
python3 run.py --module editor.pmod --directory /var/tmp   # 저장소를 둘 장치에서
```
