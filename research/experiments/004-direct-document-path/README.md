# Direct store calls versus the Pure document path

This is an end-to-end comparison for the existing `editor.pure` document app.
Both variants perform, for each document, a conditional create, a full read,
a conditional one-byte replacement at the end, and another full read. They
check revisions and complete bodies and compute the same digest. Both use the
same `store.c`: a write creates and synchronizes a replacement file, renames
it, then synchronizes the directory. The harness verifies equal digests and
equal final file bytes for each pair of runs.

The direct executable calls `pure_store_splice` and `pure_store_select`; the
Pure executable runs the prepared `editor.pure` plan through `PureRuntime` and
`pure_store_host`. The direct executable mirrors only this valid-operation
workflow. It does **not** provide the runtime's admission, ownership, budget,
completion, or cancellation contract and is not a feature-equivalent runtime.
The comparison isolates the cost of choosing that contract for this app path;
it does not compare two complete products.

Run on a Linux host with a C17 compiler, Python 3, GNU `size`, and `df`:

```sh
python3 research/experiments/004-direct-document-path/run.py \
  --documents 256 --body-bytes 8192 --pairs 4 \
  --output research/experiments/004-direct-document-path/results/local-8k.json
python3 research/experiments/004-direct-document-path/run.py \
  --documents 32 --body-bytes 1048576 --pairs 4 \
  --output research/experiments/004-direct-document-path/results/local-1m.json
```

The runner uses a fresh temporary directory for each variant and alternates
direct/Pure order between pairs. Its default `--work-root` is this directory's
`results/`, so the store is on the repository filesystem. It records the
filesystem type, compiler, source hashes, executable hashes, linked sections,
operation counts, four stage times, wall time, user/system CPU time, and peak
child RSS. A small native parent collects RSS because a Python process can
otherwise contaminate the reported child high-water mark across `exec`.
Warmups are untimed, and the runner refuses to compare against a changed
`editor.pure` definition until the direct mirror is reviewed.

The dated `ext4` JSON files are the report's evidence. The dated `tmpfs`
files are exploratory runs made before filesystem recording was added; their
store was under `/tmp` on this host. They exercised the same store calls but
cannot represent persistent-media latency. One run, even with alternating
order, cannot control host I/O scheduling, cache state, storage firmware, or
clock changes. The byte digest proves this workload's result equality, not
full behavioral equivalence in failures or concurrency.

The [result and interpretation](../../../docs/research/054-direct-document-path.md)
distinguish linked executable size from the 5,908-byte standalone core and
avoid a speed claim unsupported by the samples.
