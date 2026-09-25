# Pure

A standalone native micro runtime being designed as a small operating
system. Pure owns the application model for execution, memory, protection,
resource lifetimes, storage, and communication. Efficient connections between
inputs, computation, state, and outputs guide that design, with the aim of
reducing unnecessary representations, copies, retained data, and waiting.

Micro size is a primary constraint. The always-present core covers only the
required execution, ownership, resource limits, and completion rules. Protocols,
storage implementations, UI, and codecs are selected for the application;
supporting them does not mean bundling or starting them all. Prefer build-time
composition without requiring a dynamic plugin loader. Track core size, the
complete selected build, idle memory, and per-active-work overhead separately;
moving costs into dependencies does not count as a reduction.

The current [12,966-byte Linux AArch64 fixed document preview](prototypes/connections/results/2026-09-24-atomic-policy/pure-document-fixed-cli-linux-aarch64.tar.gz)
contains a parser-free native CLI. The [22,862-byte source-editable preview](prototypes/connections/results/2026-09-24-atomic-policy/pure-document-source-cli-linux-aarch64.tar.gz)
also includes an editable `editor.pure`. HTTP, authenticated
peer, and GTK authoring are [explicit selections](docs/research/032-micro-native-authoring-boundary.md).
The archive was extracted elsewhere and passed an edit and restore check.
The [clean default-build check](docs/research/070-micro-default-build.md)
keeps the parser, HTTP, TUI and TLS out of `make all` runtime outputs.
An [authored final-size policy](docs/research/069-atomic-authored-size-policy.md) now
guards the real document write under its storage lock. Source and fixed CLI,
HTTP and terminal hosts share the policy; an optional authenticated peer was
checked separately. The default core remains 4,632 B of object `text`.
The selected GTK process still uses about 43 MiB RSS; compressed archive size
is not a memory claim. The public checkpoint remains in progress.

The [optional native document window](docs/research/035-native-multi-document-window.md)
can now switch among stored documents in one session while preserving unsaved
document and definition drafts. It still uses GTK and remains outside the micro
default. In one Linux test, switching two 1 MiB documents 64 times raised the
window's RSS from 43,920 to 58,752 KiB. A controlled follow-up isolated the
growth to repeated GTK text-view replacement; the optional window is not a
low-memory runtime profile. See [the memory analysis](docs/research/036-gtk-memory-boundary.md).
The GTK window has no document list or multi-object update operation.

The [optional native terminal editor](docs/research/050-bounded-document-prefix-find.md)
uses the same build-time prepared document app as the CLI and HTTP host. It
handles UTF-8 input, a bounded list of stored keys with prefix lookup, resize,
revision conflicts, and a 1 MiB edit without GTK. Its current linked `text` is
32,023 B and its measured 1 MiB peak RSS was 3,168 KiB on the recorded Linux host. The
current [CLI+TUI artifact](prototypes/connections/results/2026-09-24-atomic-policy/pure-document-fixed-cli-tui-linux-aarch64.tar.gz)
is 32,144 B compressed. It does not edit
the `.pure` definition or establish a low-memory GUI result.

The [source-loaded terminal variant](docs/research/053-source-loaded-native-terminal-editor.md)
reads `editor.pure` at startup and shares the CLI's document store. It is an
optional 48,972 B linked-`text` host; the [CLI+TUI archive](prototypes/connections/results/2026-09-24-atomic-policy/pure-document-source-cli-tui-linux-aarch64.tar.gz)
is 51,398 B compressed. A changed definition takes effect in a new session.
The default core and parser-free TUI do not link the `.pure` parser.

The [selected 16 KiB-page native terminal app](docs/research/065-native-save-latency.md)
uses the same authored document program. On the recorded Linux/ext4 host,
Ctrl+S for a one-byte edit in a 1 MiB document reached the completed screen
in 93.17 ms with the default store and 22.04 ms with the selected page store
(24 saves per profile). The 256 B case was 8.54 versus 9.23 ms. The selected
source TUI adds 4,958 B linked `text`; its [CLI+TUI archive](prototypes/connections/results/2026-09-24-native-save/pure-document-hybrid-cli-tui-linux-aarch64.tar.gz)
is 57,788 B compressed and passed a relocated app check. Large documents use
an incompatible on-disk page format, so this remains an opt-in Linux storage
research profile. Its 16 KiB logical pages now admit OS page sizes that divide
16 KiB. The 16 KiB host passed the full selected suite, and a
[real 4 KiB QEMU guest](docs/research/072-qemu-4k-page-store.md) passed the
page-store assertions plus a 1 MiB native CLI edit and stale-revision check.
Physical-disk durability, Linux x86_64 CI and macOS remain unverified; the
default core and storage path are unchanged.

The [selected single-file persistent tree](docs/research/073-single-file-persistent-tree.md)
questions the one-contiguous-file-per-document model instead. All documents
share one append-only file of immutable extents. An insert, delete or append
writes only the changed leaf and path, and unchanged extents are shared
between versions. On the same ext4 host, a durable 1-byte middle insert into a
1 MiB document took 8.58 ms against 78.51 ms for the default store. The real
TUI's Ctrl+S after typing one character took 16.60 ms against 84.59 ms. The
unchanged runtime, splice, async and real-PTY TUI checks pass on it
(`make -C prototypes/connections check-tree`). Its linked CLI is 12,710 B
larger, contiguous reads of multi-leaf documents copy, and space is reclaimed
only by compaction, so it remains an opt-in profile.

The current execution profile has separate `core`, `fixed-editor`, `editor`, and
`server` build targets. `make all` produces the core and the parser-free CLI
from the build-time prepared `editor.pure`; it also builds a compiler tool that
is not linked into the CLI. Source-loaded rule editing requires
an explicit `make editor`, and HTTP requires `make server`. `(cd prototypes/connections && make check-release)` runs behavior
tests and checks both the core and default linked hosts against reviewed release
size and dependency ceilings. The default core is 4,632 B of object `text` and
120 B per idle domain and 216 B per active work slot on Linux AArch64.
It contains no document interpreter; that code is linked with a selected host.
The optional metrics profile uses 192 B per domain on this toolchain.
[The earlier 1,000-app comparison](docs/research/051-optional-runtime-metrics.md)
measured 80 KiB lower RSS for the default profile. These figures exclude the
parser, selected backends, stacks, payloads and process memory. The default
document CLI and HTTP host are separately checked at 36,135 B and 44,342 B of
linked `text`, with libc as their only direct shared
library. These are regression boundaries for the recorded toolchain, not total
memory targets. [The current authored policy and total cost](docs/research/069-atomic-authored-size-policy.md).
`make -C prototypes/connections check-portable` runs the C behavior checks
without Linux-only size/RSS tools; it is the first check to run on Apple Silicon
macOS with Apple Clang. `make -C prototypes/connections check-portable-tui`
runs both terminal editors through real PTYs without Linux `/proc`; it does not
measure their RSS. `make -C prototypes/connections BUILD=build/native-checkpoint
check-native-checkpoint` combines those checks with real CLI document RSS samples
for 1 MiB creation, read and 1 B revision edit. It adds no runtime code or
macOS memory ceiling. The storage adapter now exposes the Darwin open flags it uses.
These checks have passed on Linux AArch64, but no macOS SDK build or
native run has been completed here, so this is not a macOS support claim.
The [Apple Silicon CI job](.github/workflows/native-macos-arm64.yml) runs those
native app, PTY, authored policy, RSS and in-process document-routing checks on a macOS arm64
runner when this source is hosted on GitHub; a passing run is still required.
The optional `make -C prototypes/connections check-portable-peer` runs real
authenticated TLS document work for both native peer profiles without Linux
`/proc`; it requires OpenSSL and has [separate Linux cost evidence](docs/research/058-portable-authenticated-peer-check.md).
The current [source-editable CLI archive](prototypes/connections/results/2026-09-24-atomic-policy/pure-document-source-cli-linux-aarch64.tar.gz)
includes `editor.pure` and is 22,862 B compressed.
[A separate native source preview](prototypes/connections/results/2026-09-24-atomic-policy/pure-source-preview.tar.gz)
contains the C execution path and Workbench sources without prior build/results
trees. It was rebuilt and tested after extraction, including both PTY editors,
native CLI RSS samples, in-process document fan-out, selected page-to-flat
export and Linux release gates; see the
[clean-build evidence](prototypes/connections/results/2026-09-24-atomic-policy/source-package-check.json).
Its 337,937 B compressed size is a source-distribution cost, not resident runtime memory.
[The CLI whole-process RSS gate](prototypes/connections/results/2026-09-24-native-definition-authoring/micro-memory.json)
checks a 1 B edit and 1 MiB edit/read on the recorded Linux AArch64 system.
[Document editing and the complete measured scope](docs/research/023-document-edits.md).

A [build-time prepared runner](docs/research/037-build-time-prepared-runner.md)
executes the same document definition without linking the `.pure` parser: its
linked `text` is now 19,268 B versus 36,135 B for the source-loading CLI. Its
1 MiB edit peak RSS was about 2.1 MiB in these runs, so this is a code-size gain, not a RAM
gain. The [parser-free HTTP host](docs/research/038-fixed-http-document-host.md)
uses the same prepared app and now 26,689 B of linked `text`; the source-loading
HTTP host uses 44,342 B. Source editing still uses the separate source-loading
path. The [previous CLI+HTTP artifact](prototypes/connections/results/2026-09-24-idle-work-binding/cli-http.tar.gz)
is 28,719 B compressed and was
exercised after extraction; it has no dynamic definition editing. Both HTTP
hosts now [sleep until an event or connection deadline](docs/research/041-posix-idle-signal-wakeup.md)
instead of waking every 50 ms while idle. A small POSIX signal pipe also wakes
the host if shutdown arrives just before sleeping; macOS execution is untested.

The [document service capacity check](docs/research/040-document-service-capacity.md)
ran the same app with 1,024 stored 8 KiB documents and eight simultaneous
clients: 4,096 checked HTTP operations per host, matching final documents,
and 1,680 KiB peak RSS for the fixed-definition server in that run. This is
a bounded correctness and resource result, not a throughput claim or a
large-service comparison.

The [optional asynchronous HTTP host](docs/research/048-immutable-read-write-overlap.md)
keeps one read worker and one write worker. A deterministic test pauses a write
before publication: the same document remains readable at its old revision,
then exposes the new revision after the write completes. This adds 4,711 B of
linked `text` over the default source HTTP host. The latest 1,024-document run
observed 1,776 KiB peak RSS for this selected host versus 1,728 KiB for the
default source host. The core also now
[binds work slots only while an app needs them](docs/research/049-idle-app-work-slots.md):
1,000 real native apps retained 448 B less per controller and used 432 KiB less
process RSS in the recorded comparison, while the core grew by 144 B.

The current host OS supplies resources and imposes execution constraints.
Its processes, threads, file descriptors, and sockets are backend mechanisms,
not the organizing model for Pure applications. Development starts hosted on
existing systems; direct hardware execution remains a separate implementation
target, not a prerequisite for defining Pure's own operating model.

Networking, RPC, databases, and UI are domains where this model applies. The
research concerns how their data and work connect, including representation,
ownership, lifetime, and execution timing. Servers and CLI tools must be able to
use the runtime without loading UI. Web interoperability and standard formats
remain compatibility requirements. End-to-end efficiency is a research goal,
not an established result of the current prototypes.

Communication with non-Pure services, runtimes, and programs is a core
requirement in both directions. Boundary adapters support the peer's protocols
and data formats without requiring the peer to run Pure or making those formats
the mandatory internal representation.

SQL and relational schemas are not Pure's internal data model. Memory and
storage representations follow the data's access, update, and lifetime needs;
the runtime must not require data to pass through a SQL engine.
An [optional inline/page storage experiment](docs/research/034-hybrid-document-storage.md)
preserves the same contiguous read interface while reducing a 1 MiB document's
one-byte replacement from 1,048,600 to 16,944 user-space file-write bytes.
Unlike the earlier [page-only experiment](docs/research/033-paged-document-storage.md),
a one-byte document still writes 25 bytes. In the current build the selected CLI
grows from 35,783 to 41,040 linked text bytes. Large objects become unreadable
by the default store after conversion. A separate
[offline export](docs/research/062-paged-store-rollback.md) now preserves bodies
and revisions in a new default-format directory, but this backend is not the
micro default.
A separate [Linux file-copy experiment](docs/research/059-immutable-file-copy-path.md)
keeps the existing immutable file format and reduces user-space read/write calls
for a one-byte edit of a 1 MiB document. It adds 269 B of linked CLI `text`
when explicitly selected; the default core and build stay unchanged. This is
syscall evidence, not a claim of less physical I/O or lower latency.
[A durable edit comparison](docs/research/061-durable-document-edit-latency.md)
now times the same 1 MiB native document action on ext4: the page profile has
a much shorter median edit, while the file-copy path is not distinguishable
from the default on this host. A 70-edit run exposes the page rebuild and
occasional long delays, so the page format remains optional.

HTTP is a compatibility boundary, not the required model for Pure-to-Pure
communication. Selected scalar and document apps now use bounded binary peer
protocols over mutually authenticated TLS 1.3/TCP. The document peer shares
`editor.pure` work and storage with the CLI, HTTP adapter, and GTK window;
its request can omit an output body that the caller will not consume. The
plaintext HTTP/1.1 loopback adapter remains a separate compatibility path;
neither peer is a QUIC implementation. See the [native protocol design](docs/design/006-native-protocol.md),
[scalar peer evidence](docs/research/026-secure-peer-link.md), and
[document peer evidence and full cost](docs/research/030-authenticated-document-peer.md).

Execution domains are internal units of Pure's operating model. The core
defines their scheduling, ownership, authority, connections, and lifetimes;
platform backends provide the mechanisms available to enforce those contracts.
This is a [design direction](docs/design/007-execution-domains.md), not an
implemented complete OS or sandbox. A limited [execution core](docs/research/022-execution-core.md)
now owns admission, input budgets, ready work, completion, and borrowed results
for the storage profile. The HTTP adapter uses the same core as local domain
connections. Current host limits and native-code fault boundaries still apply.
The optional [in-process document mirror](docs/research/063-real-in-process-routing.md)
now forwards a real 1 MiB stored value to two domains without allocating target
input buffers. Each target writes independently and reports its own revision
conflict; the source mapping is released after both finish. This is one-shot
domain composition, not yet a long-running app supervisor.

The [first public checkpoint](CHECKPOINT.md) remains in progress. Its requirements
include a real authored native app, internal and external communication,
reproducible cost evidence, and a usable build and distribution path.
An [actual document-path comparison](docs/research/054-direct-document-path.md)
now checks direct store calls against the prepared Pure app on equal final files.
The Pure benchmark executable links 8,276 B more `text`; the ext4 timing samples
are too variable to establish a speed advantage. This comparison covers the
valid document workflow, not a feature-equivalent direct runtime or RPC service.
An [authenticated document-peer comparison](docs/research/055-authenticated-peer-transport-cost.md)
found a shared TCP stall in both direct and Pure servers; setting TCP_NODELAY
removed it in the local repeated-read test. The optional server-only build
links 3,472 B less `text` than the combined native peer. Its selected TLS
process still occupies roughly 8–10 MiB here, so it is not the micro default.
The [server-only TLS archive](docs/research/056-server-only-document-package.md)
is 47,686 B compressed, includes the source-editable CLI, and passed extracted
external-client and document recovery checks. The corresponding combined
client/server archive is 49,382 B; neither bundles OpenSSL or libc.

The browser, terminal, and optional Linux authoring window validate parts of the execution and
authoring model. They do not yet implement the full application runtime or its
network, RPC, database, and general native UI subsystems. See the
[native runtime roadmap](docs/research/010-roadmap.md) for the current direction.

Current experiment: [author a work from a blank document](prototypes/workbench/README.md).

The same authored scalar definitions can now be built as a
[standalone native CLI, optional Linux GTK authoring window, or optional authenticated peer](prototypes/workbench/native/README.md).
Independent calculation instances, inputs, snapshots, plan replacement, and conditional
storage use the execution core. The CLI runs without Node, GTK, or Wasm. The authoring
window uses Node and a C compiler to apply edits, and its complete memory cost is much
larger than the execution-only CLI. [Native window evidence and separate costs](docs/research/025-native-authoring-shell.md).
Fixed-definition CLI and peer builds now share one immutable program across
independent scalar instances. For the routed layout, each instance occupies 280 B
and the compiled program occupies 2,480 B once; [the measured 1,000-instance
comparison and complete build costs](docs/research/028-shared-fixed-program.md)
keep the authoring window and Wasm at full capacity.
The native app controller now occupies 408 B per idle instance, down from
928 B after [idle work-slot binding](docs/research/049-idle-app-work-slots.md)
and [optional metrics](docs/research/051-optional-runtime-metrics.md).
The peer host uses the same app operations without adding OpenSSL to the CLI or core;
its [complete process cost](docs/research/026-secure-peer-link.md) is measured separately.

The native path is runnable: [author storage and response connections](prototypes/connections/README.md).
It resolves named boundary fields, constant byte ranges and aliases before
serving local TCP requests, evaluates ranges that depend on input values,
preserves bounds failures and storage effects, and holds source data through
transfer completion. A text-selection client exercises this path with UTF-8 files.
This is a limited native authoring profile, not yet a
general-purpose I/O execution model or an end-to-end performance result.

The authored document splice program now also runs through an [optional Linux
GTK document window](docs/research/029-native-document-window.md). The same
document is edited by the native CLI and HTTP boundary with revision conflicts;
GTK and its measured 44 MiB process cost stay outside the micro core. This is
a hosted native app experiment, not the final low-memory UI backend.
The window also edits and safely applies its `.pure` definition in place;
[the selected app cost and recovery checks](docs/research/032-micro-native-authoring-boundary.md)
are reported separately from the default runtime.
An [optional authenticated document peer](docs/research/030-authenticated-document-peer.md)
now performs the same edit and read over TLS without adding TLS to the default
CLI, HTTP host, or execution core.

```sh
python3 prototypes/workbench/manage.py serve --port 8766
```

Open <http://127.0.0.1:8766>. Declare your own states, inputs, shapes, and event
handlers. Apply edits while preserving states by name. Browser and terminal use
the same Zig execution core, with separate instances. Optional examples never run
automatically. The prototype is an authoring experiment, not a claim of superior
developer experience or performance.

The earlier [Dino demo](prototypes/live-rules/README.md) tests changing predefined
rules while keeping state; it does not establish development experience.

Research in progress:

- [Bounded prefix lookup across 1,024 documents without a second index](docs/research/050-bounded-document-prefix-find.md)
- [Idle work-slot binding across 1,000 complete native apps](docs/research/049-idle-app-work-slots.md)
- [Optional read/write overlap and snapshot correctness in the document HTTP host](docs/research/048-immutable-read-write-overlap.md)
- [Bounded native document browser over 1,024 real documents](docs/research/047-bounded-native-document-browser.md)
- [Optional asynchronous document HTTP host and measured process cost](docs/research/046-optional-async-document-http.md)
- [Low-memory native terminal editor for the same document app](docs/research/042-micro-native-terminal-editor.md)
- [POSIX idle HTTP shutdown wakeup with two host-only file descriptors](docs/research/041-posix-idle-signal-wakeup.md)
- [One document app across 1,024 objects, 4,096 operations and eight clients](docs/research/040-document-service-capacity.md)
- [Idle HTTP wakeups: 25 timed polls become one blocking wait](docs/research/039-idle-http-wakeups.md)
- [One prepared app in native CLI and external HTTP, with bounded slow-client output](docs/research/038-fixed-http-document-host.md)
- [Build-time prepared runner: same document operations with 46% less linked code](docs/research/037-build-time-prepared-runner.md)
- [GTK document-switch memory boundary and micro runtime cost separation](docs/research/036-gtk-memory-boundary.md)
- [Selected page storage: 1 MiB write reduction, small-value regression, and real app checks](docs/research/033-paged-document-storage.md)
- [Selected inline/page storage: small-value cost fixed, code cost and format boundary measured](docs/research/034-hybrid-document-storage.md)
- [Native multi-document window: switch stored objects without losing drafts](docs/research/035-native-multi-document-window.md)
- [Micro default package, whole-process memory gate, and optional native authoring](docs/research/032-micro-native-authoring-boundary.md)
- [Relocatable Linux document preview, modified source behavior, and remaining storage rewrite cost](docs/research/031-relocatable-document-preview.md)
- [Authenticated document peer: same authored edit, 1 MiB ingress and output demand](docs/research/030-authenticated-document-peer.md)
- [Native document window: one authored edit path through GTK, CLI, and HTTP](docs/research/029-native-document-window.md)
- [Shared immutable native program, 280-byte scalar instances, and full selected build costs](docs/research/028-shared-fixed-program.md)
- [Fixed-definition native capacity, 11-app parity and 1,000-instance memory cost](docs/research/027-fixed-native-capacity.md)
- [Optional authenticated binary Pure-peer connection and its complete cost](docs/research/026-secure-peer-link.md)
- [Optional native authoring window, core-owned plan replacement, and full process cost](docs/research/025-native-authoring-shell.md)
- [Native application instances, input and storage parity](docs/research/024-native-app-instances.md)
- [Execution core: owned work, bounded admission, and shared results between domains](docs/research/022-execution-core.md)

- [Named fields and input-dependent selection: document → computation → storage and response](docs/research/021-field-connections.md)

- [Authored native connections: prepare range access while preserving effects and lifetimes](docs/research/020-prepared-connections.md)

- [SQL-free byte storage: mapped ranges, publication, and persistence boundaries](docs/research/019-byte-storage.md)

- [Native request → database → response: historical SQLite implementation, replaced by 019](docs/research/018-native-connections.md)

- [Browser layout and text editing experiment; architecture conclusion superseded](docs/research/017-web-layout-boundary.md)

- [One pointer input, independently owned targets: shared event routing](docs/research/016-pointer-routing.md)

- [One rule body, separate object fields: shared execution without code expansion](docs/research/015-shared-rules.md)

- [Editable multi-object layouts and atomic document persistence](docs/research/014-layout-persistence.md)

- [Authored direct manipulation: pointer coordinates, cancellation, and shared execution](docs/research/013-authored-pointer-input.md)

- [Separate execution refresh from editor work: measured input-path improvement](docs/research/012-refresh-ownership.md)
- [Direct input-to-output publication: removed scene reconstruction, with measured limits](docs/research/011-direct-presentation.md)
- [Next roadmap: efficient interfaces with fewer representation and execution layers](docs/research/010-roadmap.md)
- [Bounded display evaluation integrated with authored work](docs/research/009-bounded-display-integration.md)
- [Forgetful execution core: bounded records and work, with verified semantics](prototypes/forgetful-core/README.md)
- [Research brief](docs/research/001-research-brief.md)
- [First experiment and results](docs/research/002-checkpoint-results.md)
- [Live rules prototype and storage report](docs/research/003-live-rules.md)
- [Blank authoring experiment and comparison](docs/research/004-authoring.md)
- [Operation, memory, and module contract draft](docs/research/005-foundation-contract.md)
- [Problem landscape and research selection strategy](docs/research/006-problem-landscape.md)
- [Buffer retention comparison and measured limits](docs/research/007-buffer-retention-results.md)
- [JSON parser and shared-buffer fan-out results](docs/research/008-json-fanout-results.md)
- [Working method](docs/research/000-working-method.md)
