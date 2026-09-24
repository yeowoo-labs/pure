# Experiment 003: actual JSON parsing and shared-buffer fan-out

This apparatus extends the [single-consumer experiment](../002-buffer-retention/README.md)
with a real JSON parser and up to three independently paced consumers. It is C17
code for comparing ordinary ownership policies, not the Pure execution core.

## Dependency and semantics

The MIT-licensed **yyjson 0.12.0** source and license are vendored unchanged at
commit [`8b4a38dc994a110abaec8a400615567bd996105f`](https://github.com/ibireme/yyjson/tree/8b4a38dc994a110abaec8a400615567bd996105f).
[UPSTREAM.json](vendor/yyjson/UPSTREAM.json) records source URLs and hashes, which
the runner verifies. Reproduction requires no dependency download.

Input is newline-delimited JSON, with a maximum of 1 MiB per record excluding its
newline. A complete last JSON object may omit the newline. Blank lines are errors.
Each object must have string-valued selected fields (`body`, `tag`, `tag` by
default). Other members are parsed and validated too. Duplicate selected names
use yyjson's first matching member; the Python reference uses the same explicit
first-member rule. This is an experiment schema, not a general JSON application.

All variants use `YYJSON_READ_INSITU`, with required zero padding. The parser
decodes strings into the input buffer. We extract offsets and byte lengths, then
immediately free document metadata. Queued views retain their input allocation
or a compact copy. No document node outlives the document. Unicode escapes,
embedded NULs and empty strings are compared as decoded UTF-8 bytes, not C strings.
The pinned [header](vendor/yyjson/yyjson.h) and [implementation](vendor/yyjson/yyjson.c)
are the authority for this lifetime behavior; the official
[API guide](https://ibireme.github.io/yyjson/doc/doxygen/html/api.html) explains
in-situ input ownership and the allocator interface.

Output is an experimental multiplexed binary stream. Each selection has a
16-byte little-endian header: uint32 consumer index, uint32 byte length, uint64
input sequence number, followed by the decoded selected bytes. Interleaving can
change with scheduling; each consumer's sequence and contents must match exactly.
The verifier checks separate per-consumer digests and counts rather than requiring
an identical whole-file interleaving.

## Compared policies

| Name | Choice |
|---|---|
| borrow | Every consumer keeps a view of the original input allocation. |
| copy | Copy each consumer's selection independently. |
| shared_copy | Copy each distinct selected range once; consumers of that range share the copy. |
| threshold2/8 | Copy when rounded input capacity / selected capacity reaches 2/8, sharing identical copies. |
| compact | Initially borrow; after a consumer finishes, compact a smaller final remaining view if the ratio reaches 2 and byte credits allow it. |
| copy_no_pool | Independent copies with buffer caching disabled, using the same capacity classes. |

Identical selections mean the same offset and length in one parsed record. This
does not hash or intern equal string contents across unrelated fields/records.
The compact heuristic uses current ownership only, never future consumer times.
It does not move storage still referenced by multiple consumers. No particular
heuristic is claimed to be new or to require a language/compiler change.

Every allocation has a reference count and a list of its queued views. A buffer
is charged once, even when several consumers reference it. Completion unlinks its
view; only the final owner can return the allocation to the pool. This is a
single-threaded implementation with non-atomic counts, not a concurrency proof.

## Memory accounting

One shared requested-byte budget includes:

- Engine state, input read buffer, latency histograms and all consumer queue slots.
- Input allocations and simultaneous old/new allocations during reader growth.
- Compact selection allocations, including overlap with their original input.
- Parser allocations through yyjson's custom malloc/realloc/free callbacks.
- Allocation headers and cached idle allocations.

Capacity classes are powers of two, starting at 64 bytes. The pool caches at most
16 idle blocks and at most one quarter of the total budget. Reuse requires an
exact class match; idle blocks are evicted before an allocation is refused. Parser,
input and selection allocations can reuse the same class after their owners
release them. This is a small baseline cache, not a claim about the best allocator.

When byte credits run out, existing queued outputs are serviced before retrying.
This can occur inside the parser allocator. If no queued consumer can release
enough space, parsing fails explicitly within the cap. Reallocation charges both
old and new storage until the copy finishes. Shrinking may retain existing capacity.

The budget is **not process RAM**: allocator overhead, executable/library pages,
other stack frames, kernel buffering and filesystem cache are outside it. Linux
current-image `VmHWM` is reported separately as approximate RSS; zero means the
RSS metric is unavailable on another OS. Peak SOURCE/OUTPUT/PARSER counters occur
at potentially different times and must not be summed as a simultaneous peak.

## Scheduling and metrics

Every variant uses the same event loop and per-consumer FIFO slot limit. At each
iteration, an eligible queue head is served before another input record. If any
queue is full or the byte budget prevents allocation, the earliest eligible
consumer is serviced, sleeping until its deadline if necessary. The input waits
until all consumers have room for the next record. This is ordinary backpressure.

The corpus and delays are synthetic. Reads, JSON parsing, copies, writes, buffer
ownership and sleeps are real. Consumers run in one thread; independent delays
do not mean concurrent CPU execution. There are no external open-loop arrivals,
network connections, durable writes, or production latency targets.

- `peak_reserved_bytes`: peak requested apparatus storage including idle cache.
- `source_peak_bytes`, `output_peak_bytes`, `parser_peak_bytes`: active charges by
  role, including headers; cache reuse changes the role for the next lease.
- `*_byte_seconds`: charged active allocation bytes times lifetime, counted once
  until the last reference releases a block. Fixed storage and idle cache time
  are excluded.
- `selection_copy_bytes`: explicit policy copies only. Reader assembly/growth and
  parser realloc copies have separate counters. yyjson's decoding, kernel and
  other hidden data movements are not included in these counters.
- `parse_call_ns`: wall time inside `yyjson_read_opts`. Because allocator callbacks
  can service consumers under pressure, this includes their work and waiting.
  It is not isolated parser CPU time and is a subset of elapsed time.
- `elapsed_ns`: input, parsing, policies, consumers, instrumentation and buffer
  cleanup. Excludes build, process startup, fixed setup and output verification.
- `cpu_ns`: process CPU clock for the same timed region.
- Per-consumer latency starts before its input record begins to be assembled and
  ends after that consumer's output write. Time waiting upstream before that
  start is excluded. P99 is a histogram upper bound, with at most 12.5% normal
  bin rounding. These are not end-to-end service latency measurements.

Five fresh-process trials per case/policy follow one warm-up per policy. Variant
order is randomized within each trial; case order is fixed. All measurements are
instrumented. The host CPU affinity is recorded, but thermals and power are not
controlled. Output is a cached temporary file with no `fsync`. Medians and ranges
describe this apparatus; no significance or universal performance claim is made.

## Reproduction and verification

From the repository root:

```sh
python3 research/experiments/003-json-fanout/run.py --check-only
python3 research/experiments/003-json-fanout/run.py --output /tmp/pure-json-fanout-results
```

Use a new result directory; the runner refuses to overwrite one. `CC` can choose
a compiler. Native and ASan/UBSan checks run before measurements. LeakSanitizer
requires an environment without ptrace tracing; its diagnostics are not hidden.
Compiler flags, source snapshots (including the parser), hashes, input rules and
digests, every measurement and verification results are saved. Binary sizes
include debug information. `complete.json` marks successful completion.
The runner disables yyjson's unused writer, utilities and incremental-reader
modules using upstream build switches. The normal reader remains enabled for all
policies. Native measurement uses `-O2`; the sanitizer check uses `-O1`.

Checks cover decoded byte equality with Python, 1/2/3 consumers, UTF-8 and escapes,
queue-slot reuse, shared-copy final ownership, final-owner compaction, malformed
JSON/schema, parser budget refusal, long records, partial-work cancellation,
SIGTERM and real output failure where `/dev/full` exists. These are functional
tests, not an exhaustive audit of yyjson or thread/device cancellation races.

For direct use:

```sh
cc -std=c17 -O2 -DYYJSON_DISABLE_WRITER=1 -DYYJSON_DISABLE_UTILS=1 -DYYJSON_DISABLE_INCR_READER=1 research/experiments/003-json-fanout/benchmark.c research/experiments/003-json-fanout/vendor/yyjson/yyjson.c -o /tmp/pure-json-fanout
/tmp/pure-json-fanout --policy threshold --ratio 2 < records.ndjson > selections.bin 2> metrics.json
```

Exit 0 means completion, 2 means failure, 3 means cancellation. Earlier completed
selections are not rolled back. Output failure or a signal during a write may
leave a partial last output frame. Cleanup releases all apparatus-owned buffers;
it is not an atomic-output or asynchronous backend guarantee.

Measured results and their interpretation are in
[Korean report 008](../../../docs/research/008-json-fanout-results.md).
