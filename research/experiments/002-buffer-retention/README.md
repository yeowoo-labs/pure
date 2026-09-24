# Experiment 002: buffer retention under backpressure

This is a native C17 comparison apparatus, **not an implementation of the Pure
runtime and not a Sonic, Node.js, or .NET benchmark**. It follows
[research question 006](../../../docs/research/006-problem-landscape.md).

Question: when a producer selects part of each record and a delayed consumer
retains that selection, what costs remain after applying ordinary copy policies,
bounded queues, pooling, a shared byte budget, and early field projection?

## Work and observable output

The CLI reads a sequence of experimental binary frames from stdin. Each frame
has three little-endian uint32 fields: payload byte length, selection offset,
and selection byte length, followed by the complete payload. The output is a
little-endian uint32 selection length followed by the selected bytes. Input order
is preserved. A payload is 1 byte to 8 MiB; a selection is nonempty and in bounds.
The format has no magic, version negotiation, JSON syntax, or production claims.

The Python generator materializes every byte, with deterministic seed, varying
offsets, and source-size/selection-size distributions. It independently computes
the expected output digest; tests additionally compare bytes against a Python
reference decoder. Every measured run's complete output is checked. Input data is
synthetic, not a production trace. Large temporary fixtures are reproducible from
the saved plan, generator hash, and fixture manifest; they are not checked in.

All variants read all payload bytes. `project` can discard bytes through a fixed
64 KiB scratch area because this fixture's header supplies the selected range.
That is an intentionally strong existing optimization when a format allows it.
Its results do not establish that arbitrary JSON, compression, or a random-access
document supports the same projection.

## Variants and allocator

| Variant | Decision |
|---|---|
| borrow | Keep the complete source buffer until output completes. |
| copy | Copy the selected bytes to an independent buffer, then release the source. |
| threshold2/4/8 | Copy when rounded source capacity / rounded selection capacity is at least 2/4/8. |
| pressure | Copy a smaller selection when current live buffer charges exceed half the usable budget. No future lifetime information. |
| project | Read the selection directly into its output buffer; read and discard the rest through scratch. |
| copy_no_pool | Same copy policy without the reusable buffer cache. |

`pressure` is a simple candidate online heuristic written in ordinary C. It is
not described as a novel policy or Pure advantage. All thresholds were recorded
before measurement; all results are reported, with no fitted or held-out claim.

Blocks use power-of-two capacity classes, minimum 64 bytes. Pool-enabled variants
reuse exactly matching classes. At most 16 idle blocks can be cached, consuming
at most one quarter of the total byte budget. Idle blocks are evicted as needed
before admitting another allocation. Pool-disabled runs use the same capacity
rounding so that this comparison isolates reuse rather than size classes.
This small explicit cache is an experimental baseline, not a best-in-class allocator.

Each record has one owner at a time and there is one FIFO consumer. There is no
fan-out, shared-owner reference counting, persistent result cache, or async device
backend. Copy temporarily needs both source and destination; this overlap is
charged. A budget too small to admit a record returns `budget_too_small` instead
of waiting forever. Existing accepted output may already have been written.

## Scheduling and budget

The same single-threaded scheduler serves every policy. A queued output becomes
eligible `hold_us` after enqueue. Consecutive output completions are separated by
at least `service_us`. Ready output takes priority over another input record;
otherwise the producer reads until a queue slot or byte credit is unavailable.
Backpressure then services the oldest queued output, sleeping if needed.

This uses actual reads, writes, allocations, copies, a monotonic clock, and
`nanosleep`; it is not a simulated memory counter. The consumer delays are
synthetic. A requested 20 us service interval is not a 20 us guarantee; kernel
wakeup granularity and host scheduling affect the realized interval.

The byte budget includes the engine, queue array, scratch, histogram, pool pointer
array, block headers, active buffers, and cached idle buffers. It bounds requested
storage owned by the apparatus, **not whole-process RSS**. It excludes allocator
bookkeeping, libc/loader, executable pages, other stack frames, kernel I/O buffers,
filesystem page cache, and the external Python verification process.

## Metrics and measurement limits

- `peak_reserved_bytes`: maximum fixed apparatus bytes plus active and cached
  block charges. `peak_active_bytes` excludes cached idle blocks but includes the
  fixed apparatus. Allocator bookkeeping is excluded from both.
- `active_byte_seconds`: sum over active block leases of charged bytes times
  elapsed lifetime. Includes transient input/output overlap and block headers;
  excludes fixed apparatus storage and idle cache residency. Reused leases are
  measured separately, without double counting a block's cached interval.
- `copy_bytes`: only explicit selection `memcpy` bytes. Reads, writes, kernel,
  allocator, and generator copies are outside this counter. Zero does not mean
  no data movement in the process or system.
- `latency_*`: starts immediately before reading a record header and ends after
  its selected output is written. Includes admission waits after reading that
  header. It excludes time a record waits outside the process before its header
  is read. There is no independent open-loop arrival source, so these are not
  service request P99 measurements. P99 is a histogram upper bound (up to 12.5%
  bin rounding at normal durations), not an exact percentile.
- `elapsed_ns`: file I/O, deliberate delays, scheduling, instrumentation and
  buffer cleanup. Excludes build, process startup, fixtures and verification.
  `cpu_ns` uses the process CPU clock; reports deliberate waiting separately.
- `rss_peak_bytes`: approximate current-image `VmHWM` from Linux
  `/proc/self/status`, read after the timed region. Zero with source `unavailable`
  means no current-image implementation on another OS. `rusage_maxrss_bytes` is
  retained separately (Linux KiB normalized to bytes; macOS already bytes).
  Linux rusage survives exec and was contaminated by launcher history in the
  first run; it cannot substitute for the child's current-image memory.
  Neither measurement is added to requested buffer storage.

Instrumentation is enabled equally in all variants, including block lifetime
timestamps. Its overhead is not separately calibrated in this first experiment.
Timing results must not be presented as uninstrumented application throughput.
Output goes to a regular temporary file without `fsync`; this measures cached
file I/O rather than durable storage. Each variant gets a warm-up run followed by
five fresh-process measured runs in randomized variant order within each case.
Cases run in fixed order. Linux runs pin to one available CPU; thermal and power
conditions are uncontrolled. Medians and ranges are descriptive, not significance
tests. The original plan predates the first measurement. Plan v2 adds an explicitly
exploratory 64-slot sensitivity case after the first run exposed queue occupancy
differences. Original data and source snapshots are preserved with an audit note;
the v2 run is the current result. Neither plan is a production SLO.

Lower peak RAM can permit a longer queue under the same byte cap and thereby
increase observed in-process latency. Compare queue occupancy and latency as
well as memory; equal byte/slot limits do not imply equal latency. No application
SLO or success threshold has been invented for these synthetic inputs.

## Reproduction and checks

From the repository root, using Python 3 and a C compiler with ASan/UBSan:

```sh
python3 research/experiments/002-buffer-retention/run.py --check-only
python3 research/experiments/002-buffer-retention/run.py --output /tmp/pure-retention-results
```

The result directory must not already exist. `CC` selects the compiler. The
runner builds into a temporary directory, verifies normal and sanitized builds,
and saves the plan, source hashes, build flags/hash/size, environment, fixture
digests, checks, every raw sample and summaries. Build size includes debug info.
`complete.json` appears only after all measurements and output hashes pass.
LeakSanitizer needs an execution environment without ptrace tracing; its fatal
diagnostics are treated as a failed verification, never suppressed.

For direct use:

```sh
cc -std=c17 -O2 -Wall -Wextra -Wpedantic -Werror research/experiments/002-buffer-retention/benchmark.c -o /tmp/pure-retention
/tmp/pure-retention --policy threshold --ratio 4 --budget 8388608 --hold-us 5000 < input.frames > selected.frames 2> metrics.json
```

Exit 0 indicates completion; exit 2 indicates an error; exit 3 indicates
cancellation. SIGINT/SIGTERM cancel synchronous work and release outstanding
buffers. The output may contain a completed prefix or a partially written final
frame on I/O failure. This is not an atomic-output guarantee. Tests include exact
output equivalence, empty input, malformed/truncated input, budget refusal,
deterministic cancellation, real signal cancellation, and `/dev/full` where
available. These synchronous tests do not verify async cancellation races.

The RSS correction follows the documented preservation of
[resource usage across exec](https://man7.org/linux/man-pages/man2/getrusage.2.html).
Linux documents [VmHWM as approximate](https://man7.org/linux/man-pages/man5/proc_pid_status.5.html);
requested storage counters remain the precise apparatus accounting used here.

The first result and its interpretation are recorded in
[report 007](../../../docs/research/007-buffer-retention-results.md).
