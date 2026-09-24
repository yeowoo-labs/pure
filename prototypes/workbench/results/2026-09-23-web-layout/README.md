# Native web layout boundary — 2026-09-23

Functional and ownership evidence, not a performance or total-memory benchmark.

- `build.json` / `native-build.log`: pinned Zig 0.15.2, 37 native tests, source/Wasm hashes.
- `node-tests.log`: final 87 tests (compiler, runtime, numeric/pointer routes, CLI,
  persistence, display evaluation/publication and previous examples).
- `browser.json`: 32 document-workspace checks, with source/preview text and counters.
- `authoring.json`: 44 legacy authoring-flow checks.
- `value-control.json`: 3 numeric-control checks in the original workbench UI.
- `guards.json`: 6 focused checks including fixture validation, rejected out-of-range
  output, arithmetic rollback/control reconciliation, active-gesture apply refusal,
  and oversized-file rejection.
- `pure-document-saved.txt`: actual browser download, byte/text checked against the
  UTF-8 LF fixture in `browser.json`, also reopened in a fresh page.
- `pure-document-{desktop,mobile}.png`: final screenshots, visually inspected.
- `sources/`: implementation and test snapshots; `before/`: previous core/host files.
- `verification.json`, `manifest.json`: bounded result counts and artifact hashes.

The 32-case document run precedes the final range-control reconciliation on rejected
host output. The focused guard run tests that final correction and records the final
screenshots (also incorporating a small heading-wrap adjustment). The core/runtime
used by the final snapshots have matching build hashes. No timing comparison with
before/ is made.

Reproduce from repository root:

```sh
python3 prototypes/workbench/manage.py build --zig /path/to/zig-0.15.2
node --test prototypes/workbench/*.test.mjs
python3 prototypes/workbench/manage.py serve --port 8766
```

Open `/document.html` with Playwright CLI, and pass `qa-document.js` to `run-code`.
After completion read `window.__pureDocumentEvidence`. `qa-document-guards.js`
exposes `__pureDocumentGuards`. The original workbench uses `qa-authoring.js` and
`qa-value-control.js` here, with `__pureWorkbenchQAEvidence` / `__pureValueEvidence`.
Dialogs/downloads can make the CLI return before the async function finishes;
read completion evidence rather than treating a CLI return as success.

Counters prove which application callbacks run, not whether a browser internally
performs layout or allocates memory. Native editor and preview both retain text;
there is no additional persistent JS/VM document model. Per-edit whole-preview text
publication remains. No physical mobile, OS IME, screen-reader, macOS/Metal or
large-document performance result is claimed. Composition checks use synthetic
composition events, touch uses Chromium CDP, other mouse/keyboard flows use actual
browser input. File exchange is UTF-8 text with native textarea newline normalization.
