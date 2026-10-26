# 030 P1's measurements (2026-10-26)

From the repository's root, on builds in `~/.cache/pure-040` (`variant.sh NAME COMMIT [PATCH... | nocompact]`, as 040's
`runs.sh serve` with patches; `nocompact` raises the compaction trigger), each run alone on the Pi:

- `trace.sh OUT NAME`: one TPC-C run (W=1, 64 terminals, 20 s) with `strace -c` on the owner thread during the run step
  (`OUT-NAME.strace`, `.faults`); `bytes.sh OUT NAME`: the owner's pwrites and syncs for 3 s mid-run (bytes a save).
- `profile.py OUT NAME`: one TPC-C run with the owner's user-mode instructions sampled (`osample.c`: perf_event_open on
  one thread, user mode, as paranoid 2 allows) and its faults, user and kernel ticks (`OUT-NAME.profile.json`).
- `mech.py DIR`: store calls a save, faults a burst, instructions, user and kernel time an event, from the two above.
- `analyze.py A B FIRST LAST`: TPC-C and bank-query medians of 040's `runs.sh pairs` on 2026-10-26;
  `queries.sh A B FIRST LAST`: the bank's queries alone in turns.

The builds: `z0` today (dadeb36's code), `z1` B0 on it (`../b0.patch`), `z3` the first P1 build (955f18b) with B0,
`z5` the reviewed WIP (b3390c3) with B0, `z6` the committed P1 (f7d32fc), `z7` f7d32fc with B0, `z7nc` that with compaction
off. Files: `*-stage0` (B0 against today, where B0 loses), `strace-z3-wip` (after the fork count replaced getpid),
`strace-z0/z6/z7` and `profile-z0/z6/z7` (the committed code), `profile-repeat-*` (the user-time bar's repeats, and z7nc),
`046-*` (identity, takeover, prefixes on f7d32fc), `pwrites-*` (bytes a save: today's log, B0's tree). The pairs' own
files are in `../../040-against-c/2026-10-26-*`; 041's counts in `../../041-batches/2026-10-26-counts.jsonl`. Results:
docs/design/030-one-path.md, "P1의 결과".
