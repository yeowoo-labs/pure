# Pointer group routing evidence — 2026-09-23

This is functional validation and compiled-representation accounting, not a
throughput, latency, browser-heap, or large-application benchmark.

- `build.json`: Zig 0.15.2 native tests (35), final core/Wasm hashes and build flags.
- `node-tests.log`, `verification.json`: final 83 Node tests, including 8 route tests
  and existing compiler/runtime/CLI/display/persistence behavior.
- `representation.json`: shared-layout and routed-layout compiled on the same final
  core. Includes used route bytes, fixed engine reservation, and unchanged 64 KiB memory.
- `browser.json`: 30 real Chromium checks on grouped input, including two CDP contacts,
  rejecting a second contact on one view, releasing one then moving/canceling the other,
  actual mouse placement, source editing/reordering, file download/new-page restoration,
  stale file-read rejection and mobile width.
- `authoring.json`: 44 existing authoring-flow checks.
- `legacy-browser.json`: 23 existing separate-pointer-input/shared-rule checks.
- `pure-routed-layout-saved.json`: actual downloaded document, including unfinished draft.
- `pure-routed-layout-{desktop,mobile}.png`: final grouped example screenshots.
- `sources/`: final implementation, contract and relevant test snapshots.
- `before/`: pre-change core/compiler/runtime/Wasm for auditing; no timing comparison.

Reproduce from repository root:

```sh
python3 prototypes/workbench/manage.py build --zig /path/to/zig-0.15.2
node --test prototypes/workbench/*.test.mjs
python3 prototypes/workbench/manage.py serve --port 8766
```

Open a fresh Chromium page with Playwright CLI, then pass the function in
`prototypes/workbench/qa-routed-input.js` to `run-code`. Read
`window.__pureRoutedInputEvidence` after the function completes. Dialog/download
handling may cause CLI to yield early; a CLI return is not proof of completion.
`qa-authoring.js` and `qa-legacy-pointer.js` preserve the regression functions;
run authoring on a blank page. They expose `__pureWorkbenchQAEvidence` and
`__pureSharedLayoutEvidence` respectively.

The initial partial-release QA used the wrong contact in the CDP touchEnd call.
After correcting it, the following move check also read before pointermove had
arrived. `browser-initial-failure.json` retains that intermediate failure;
`touch-evidence.json` records actual down/up/move events and retained owners.
Final QA identifies the ending contact and waits for observed movement. These were
QA input/observation corrections; product code was not changed to suppress the checks.
`touch-reproduction.js` is a debugging artifact for this Chromium CDP behavior;
its protocol behavior is not a general claim about mobile hardware.

All successful browser reports are from the final runtime and Wasm. Browser is
Linux Chromium; touch is injected through CDP, not a physical touchscreen or a
user interaction in the Codex embedded browser. Static route addresses are rebuilt
on apply/restore, not persisted as dynamic lifetime identities. Keyboard selection
still uses the authored ID conditions.
