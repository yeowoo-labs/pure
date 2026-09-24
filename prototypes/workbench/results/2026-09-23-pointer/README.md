# Pointer input verification — 2026-09-23

Functional validation, not a speed or memory comparison.

- `build.json`: exact source/Wasm hashes and 26 passing native tests.
- `verification.json`: 59 passing Node tests; each suite runs with child-process support.
- `pointer-browser.json`: 26 direct manipulation checks, no page errors.
- `authoring-browser.json`: 44 existing authoring checks, no page errors.
- `before/`, `after/`, `manifest.json`: source artifacts and SHA-256 identities.
- `qa-pointer.js`, `qa-browser.js`: executed browser scripts. The authoring script
  changes only artifact path prefixes to avoid overwriting earlier evidence.
- `pure-pointer-desktop.png`, `pure-pointer-mobile.png`: actual browser captures.

Build using Zig 0.15.2 with `python3 prototypes/workbench/manage.py build --zig PATH`.
Run `node --test prototypes/workbench/*.test.mjs` with Node child-process support.
Serve with `python3 prototypes/workbench/manage.py serve --port 8766` and execute
`qa-pointer.js` as a Playwright CLI run-code expression against Chromium.
Browser tests exercise real mouse input and CDP touch emulation, including two
contacts. Blur/single-pointer cancellation use explicit event injection. They do
not validate physical touchscreen hardware or every browser engine.

Test runner elapsed times and build artifact sizes are incidental reproducibility
metadata, not performance conclusions. No throughput/latency benchmark was run.
