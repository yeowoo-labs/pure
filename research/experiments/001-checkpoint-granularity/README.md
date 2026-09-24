# Experiment 001: checkpoint granularity

This is a native measurement apparatus for one known space/time tradeoff. It is
not a new algorithm, a general dependency-graph runtime, or the product's language
choice. The apparatus uses C17 and Python's standard library so it can be run
with the tools present on the initial Linux host and with a C compiler on macOS.

## Question and pre-measurement expectations

How much does storing intermediate results at different intervals save, once the
editable program and scheduling state are included? How much extra work does it
cause when a rule changes?

Expected, not measured at the time of writing this section:

- Uniformly spaced checkpoints shrink the result cache at the cost of starting
  recomputation before the changed instruction.
- Root input changes offer no prefix to reuse. Fine checkpoints may only add
  cache writes and comparisons in this workload.
- Edits near the end favor fine checkpoints. This deliberately favorable case
  must not stand in for all edits.
- The editable model stays resident, so shrinking the cache 16-fold will not
  shrink total core storage 16-fold.

## Semantics and comparison

The model consists of independent, statically connected chains. Each instruction
is an 8-byte record with a scalar operator and parameter. Values are uint64_t;
overflow wraps. Supported operators are add, XOR, multiply, and rotate/XOR.
Only the final result of each chain is observed.

One update applies a batch of changes, then computes all affected observed
outputs. Changes alter roots, operators, and parameters, not graph topology.
There is no renderer, effectful I/O, concurrent execution, or state migration.

- Block 1 stores every intermediate result (fine incremental baseline).
- Blocks 4, 16, and 64 store results at block ends.
- A block equal to chain length recomputes the whole affected chain, retaining
  only its final output (full recomputation baseline).

All variants share program representation, input handling and per-chain dirty
ranges. Unaffected chains are skipped by every variant. Fine and full variants
use dedicated loops. Fine/block variants stop at an unchanged cached boundary
only after passing the last changed instruction in the batch.

The chain structure makes dependency information implicit. No general graph
edge storage or graph partitioning is needed. This is an optimistic case for
compact checkpoints; it cannot establish results for arbitrary graphs.

For G chains of length L and checkpoint spacing B, the requested persistent
storage in this implementation is:

```text
sizeof(Engine) + 8*G*L + 16*G + 8*G*ceil(L/B)
                 model   inputs+   cached values,
                         dirty    including outputs
```

This includes all live core arrays and the engine descriptor, but not allocator
bookkeeping, stack, executable code, trace storage or OS pages. Trace size is
recorded separately. It is not RSS, physical footprint, or total application RAM.
There are no allocations in the timed update path.

## Workloads

The fixed initial matrix uses 16 chains, lengths 256 and 1024, and 256 updates per
trace. Workload seed: 20260922.

- `roots`: every chain's input changes on every update.
- `uniform`: one uniformly selected instruction changes per update.
- `tail`: one instruction in the last eight positions changes per update.
- `dense`: eight instruction edits per chain per update, with replacement.

Changes are XOR deltas so repeated traces continue changing the model. Two
warm-up passes precede timing. Even measured replay counts leave identical final
models across strategies; the runner checks their output checksums. Differential
tests additionally compare every output after every update against independent
from-scratch evaluation across multiple seeds, odd sizes and edge cases.

## Reproduce

From this directory, with Python 3 and a C compiler with ASan/UBSan support:

```sh
python3 run.py --check-only
python3 run.py --output results/local-run
```

The runner refuses to overwrite an existing result directory. `CC` can select a
compiler, such as `CC=clang`. It builds into a temporary directory. On Linux it
pins itself and child processes to one available CPU and records that choice.

Default measurements use seven trials with randomized strategy order within
each trial. Pilot timings choose an even replay count per case, targeting 20 ms
per measurement. All raw samples, memory counts, instruction counts, compiler
flags, source hashes and host metadata are retained. Timing excludes program
creation, trace creation, warm-up, and output verification. It includes change
application, scheduling and recomputation.

`median_ns_per_tick` is the median of batch-average processing times. It is not a
P50/P99 input-to-display latency measurement. A shared host without controlled
power or temperature permits preliminary comparisons, not hardware guarantees.

The reported storage reduction is largely predicted by the formula. The useful
observations are the whole-core reduction, processing cost and losing workloads.
This experiment cannot establish automatic policy quality, novel research,
editor usability, M3 performance, or universal memory savings.

The first run and its interpretation are available in the
[Korean research report](../../../docs/research/002-checkpoint-results.md).
