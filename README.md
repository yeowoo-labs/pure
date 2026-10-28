# Pure

An application runtime that proves a program before it runs.

Every app is a module that Pure verifies — types, lifetimes, stack depth, cursor ownership, work
and memory budgets — before a single instruction executes. The same bytes are the wire format, the
in-memory representation, the on-disk record and the replication unit: there is no ORM, no query
planner and no serialisation layer between a request and a stored value.

**Status: pre-1.0, not for deployment.** No release, no package, no ABI promise. The app contract
is being frozen for 1.0; anything can change in the next commit. There has been no external
security audit.

## What it is

- **Verified modules.** An app arrives as a `.pmod` module built by [aup](https://github.com/yeowoo-labs/aup).
  Pure checks it before loading: types, value lifetimes, stack depth, the ownership of cursors and
  indexes, and a per-event work budget and memory budget.
- **Declared state is persisted state.** A `state` value goes to a single-file immutable tree or to
  a record log. One commit is one contiguous write and one `fdatasync`.
- **One writer per store.** Events are total, atomic and deterministic. Time is an input rather than
  an ambient read, so replaying a log reproduces the store exactly.
- **Views are pure functions of committed state.** They run against a snapshot while the owner keeps
  writing; a reader never waits for a writer's sync.
- **HTTP without router code.** A view is a `GET`, an input is a `POST`. The body is read in place and
  the reply is sent after the commit.
- **Nodes talk without connections.** Links seal each packet with both nodes' keys from the first
  one: no handshake, no session to lose, exactly-once delivery across `kill -9` on either side.
- **One program, several surfaces.** The same module runs as a TUI, a CLI, a speech surface, a web
  page or on a framebuffer through [Vega](https://github.com/yeowoo-labs/vega).

## Quick start

A C17 toolchain and `make` are enough. Tested on Linux aarch64 (Raspberry Pi 5) and x86-64.

```sh
git clone https://github.com/yeowoo-labs/pure.git
cd pure
make            # build/libpure.a, libpure-link.a, pure-run, pure-serve, pure-crypto.so
make check      # verification, determinism, crash, transport, query and reference-model tests
```

Running an app needs a module; aup builds one:

```sh
git clone https://github.com/yeowoo-labs/aup.git ../aup
make -C ../aup
../aup/build/aup build ../aup/examples/tasks.aup -o tasks.pmod

printf 'type typed %s\ndown again\ndown toggle\nshow\n' "$(printf 'milk' | xxd -p)" \
  | build/pure-run tasks.pmod --store /tmp/tasks
```

The same module as an HTTP service — views are `GET`, inputs are `POST`, and no route code is
written:

```sh
build/pure-serve --module tasks.pmod --store /tmp/tasks --threads 2 &
curl -s localhost:8767/views/count
curl -s -X POST -H 'Content-Type: application/json' \
  -d '{"text": "ship it"}' localhost:8767/inputs/typed
```

## Layout

| Path | What lives there |
|---|---|
| `core/` | Module format, verifier, executor and JIT tier, values, limits |
| `store/` | The single-file immutable tree store and the record log |
| `serve/` | `pure-serve`: HTTP host, several apps on one host, snapshots, pushed views |
| `link/` | Connectionless sealed transport and its specification |
| `cli/` | `pure-run` and the command line |
| `tests/` | Verification, determinism, crash, sanitizer and reference-model tests |
| `benchmarks/` | HTTP, TPC-C, TPC-B and query benchmarks with their drivers |
| `research/` | Benchmark sources and raw results, one directory per experiment |
| `docs/design/` | The design record: format, lifetimes, transport, surfaces |
| `docs/security/` | Advisories for fixed issues |

## Verification

`make check` is the gate, not a formality: module verification including adversarial modules,
determinism across the JIT and the interpreter, crash recovery at every commit stage, transport
loss and reordering, and comparisons against reference models. The sanitizer builds run the same
suites.

## Measured

Measured on a Raspberry Pi 5 (Linux aarch64, SD card) on 2026-09-27 against axum and Drogon on the
same device. Sources and raw results are in `benchmarks/` and `research/`.

| Workload | Pure | axum | Drogon |
|---|---|---|---|
| Durable write, 64 connections | 21.3k/s | ~200/s | ~200/s |
| 64 B GET, one thread | 139k/s | 127k/s | 147k/s |
| Idle RSS | 2.3 MiB | 2.8 MiB | 10.9 MiB |

TPC-C through the same app path, HTTP and JSON included, runs at 40,976 transactions/s in RAM and
4,980/s durable on the SD card, with every row matching SQLite for the same input. See
`benchmarks/tpcc/`.

## Documentation

- [Module format](https://github.com/yeowoo-labs/pure/blob/master/docs/design/008-module-format.md)
- [App contract](https://github.com/yeowoo-labs/pure/blob/master/docs/design/026-app-contract.md)
- [Transport](https://github.com/yeowoo-labs/pure/blob/master/docs/design/011-pure-transport.md) and
  [link specification](https://github.com/yeowoo-labs/pure/blob/master/docs/design/012-link-protocol.md)
- [Surfaces](https://github.com/yeowoo-labs/pure/blob/master/docs/design/010-surfaces.md)
- [Several apps on one host](https://github.com/yeowoo-labs/pure/blob/master/docs/design/017-apps-on-one-host.md)
- [Security advisories](https://github.com/yeowoo-labs/pure/tree/master/docs/security)

The design record cites measured notes for most numbers. Those notes are the author's working log
and are not published; the code and raw results that produce the numbers are in `benchmarks/` and
`research/`.

## Related

- [aup](https://github.com/yeowoo-labs/aup) — the language and toolchain that builds modules
- [vega](https://github.com/yeowoo-labs/vega) — surfaces for the same module
- `kay` (kernel) and `lunic` (OS) are reserved for after 1.0; nothing is implemented there yet.

## License

Apache-2.0. See [LICENSE](LICENSE).
