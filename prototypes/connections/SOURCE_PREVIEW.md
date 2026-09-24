# Pure native source preview

This bundle contains the C17 execution core, the authored document app, its
CLI and optional terminal/HTTP/TLS hosts, plus the Workbench sources used by
the integration checks. It excludes prior benchmark results and build outputs.
`manifest.json` records the exact files and their SHA-256 hashes.

Build and run the small default path from the extracted directory:

```sh
make -C prototypes/connections check-portable
make -C prototypes/connections check-portable-tui
make -C prototypes/connections check-policy
make -C prototypes/connections fixed-editor fixed-tui
make -C prototypes/connections source-tui
mkdir -p /tmp/pure-source-store
printf 'hello Pure\n' > /tmp/pure-source-insert.txt
prototypes/connections/build/pure-edit-fixed --store /tmp/pure-source-store \
  edit note 0 0 0 /tmp/pure-source-insert.txt
prototypes/connections/build/pure-edit-fixed --store /tmp/pure-source-store show note
prototypes/connections/build/pure-edit-tui --program \
  prototypes/connections/examples/editor.pure --store /tmp/pure-source-store note
```

`check-portable` builds the C hosts and runs the host-neutral C behavior tests.
`check-portable-tui` runs the fixed and source-loaded terminal editors through
real PTYs, including UTF-8 input, resize, 1 MiB edits, conflicts and document
browsing. It records no Linux `/proc` RSS number; on macOS it is a behavior
check, not a proof of the 4 MiB Linux memory ceiling.
For a single native checkpoint command, run from the extracted directory:

```sh
make -C prototypes/connections BUILD=build/native-checkpoint check-native-checkpoint
```

This requires a C17 compiler, Make and Python 3. It builds the core, CLI and
TUI, exercises both terminal variants, then records whole-process peak RSS for
the fixed and source-loaded CLIs during a real 1 MiB document creation, read
and 1-byte revision edit. To save the two memory results, add
`PORTABLE_SOURCE_MEMORY_EVIDENCE=/path/source.json` and
`PORTABLE_FIXED_MEMORY_EVIDENCE=/path/fixed.json` to the `make` command.
The RSS helper reports the OS raw unit and normalized KiB. No macOS RSS budget
has been established; compare macOS results only with the same host and build.
The optional `make -C prototypes/connections check-portable-peer` builds the
TLS client and two server profiles, then checks mutual authentication, document
editing, invalid frames and restart over real sockets. It requires OpenSSL
headers, libraries and command-line tool, and records no RSS in portable mode.
On Linux AArch64, `make -C prototypes/connections check-release` additionally
checks the measured code, dependency and process-memory ceilings and the actual
document/HTTP paths; it needs Python 3, Node.js, and GNU `size`/`readelf`.
`check-policy` runs the authored final-size guard through the CLI, HTTP and
terminal hosts; `check-policy-peer` additionally requires OpenSSL and checks
the same policy over mutually authenticated TLS.
The source-loading CLI can run a changed `.pure` definition with
`make -C prototypes/connections editor` and `--program PATH`.

The selected 16 KiB-page document store can be exported into a **new** default
store directory without linking it into the small default app:

```sh
make -C prototypes/connections fixed-editor
make -C prototypes/connections BUILD=build/hybrid STORE=store-paged.c fixed-editor
python3 prototypes/connections/export_flat_store.py \
  --source /path/to/paged-store --destination /path/to/new-flat-store \
  --paged-editor prototypes/connections/build/hybrid/pure-edit-fixed \
  --flat-editor prototypes/connections/build/pure-edit-fixed
```

The source stays intact. The tool holds its writer lock during conversion and
checks every object with the default editor before publishing the destination.
Writers may receive `busy` until it finishes; schedule this as maintenance.
The destination must not exist. `make -C prototypes/connections check-flat-export`
exercises the conversion with real mixed-size and repeatedly edited documents.

On a Linux host whose OS page size divides the 16 KiB logical page, the selected hybrid store can also run
the source-loaded native terminal editor without adding it to the default app:

```sh
make -C prototypes/connections BUILD=build/hybrid STORE=store-paged.c editor source-tui
python3 prototypes/connections/package_native.py \
  --build prototypes/connections/build/hybrid --hybrid-storage --with-tui \
  --output /tmp/pure-hybrid-tui.tar.gz
python3 prototypes/connections/check_package.py /tmp/pure-hybrid-tui.tar.gz
```

Use a new store directory for this profile; large documents use `PUREP001`
and need the offline exporter above before the default store can read them.

For a same-process connection between real document stores, build the selected
`storage.pure` mirror and send one source snapshot to one or more destinations:

```sh
make -C prototypes/connections mirror
prototypes/connections/build/pure-mirror \
  /path/to/source-store document /path/to/target-store document 0
make -C prototypes/connections check-mirror
```

The final number is the expected target revision. Up to eight targets may be
specified as store/key/revision triples. Each target commits independently;
the JSON result reports conflicts and partial success. The mapped source body
stays alive until every forwarded target releases it. No socket or additional
target input buffer is involved, and this host is a one-shot command rather
than a long-running application supervisor. An optional leading
`--target-budget BYTES` bounds each destination's admitted body size; an
insufficient budget rejects the transfer before any document write.

The fixed CLI and TUI contain one build-time prepared `editor.pure` and do not
reload a changed definition. The source TUI loads `.pure` at startup; edit the
file and start a new session to apply a different definition. The Workbench
scalar-native build additionally requires Zig 0.15.2; the GTK and TLS hosts
need their platform libraries.
On Linux, `make -C prototypes/connections check-copy-range` compares an
experimental large-extent file copy path with the ordinary bounded-scratch
build. It needs `strace`, `findmnt` and GNU size tools and has only been validated on the
recorded Linux kernel and filesystem.
macOS source compilation and execution are not yet verified. This preview is
not a signed release, does not bundle system libraries, and does not establish
a redistribution license.
