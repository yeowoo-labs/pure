# Layout persistence — 2026-09-23

Functional validation, not throughput/latency benchmarking.

- build.json: matching core/Wasm hashes, 29 passing native tests.
- verification.json: 68 passing Node tests.
- layout-browser.json: 17 browser checks, actual download and fresh-page restore,
  independent editing, malformed documents and asynchronous file-read races.
- authoring-browser.json: 44 passing legacy authoring checks.
- pure-layout-saved.json: actual downloaded document, including unfinished draft.
- pure-layout-desktop.png / pure-layout-mobile.png: browser captures.
- sources/ and manifest.json: exact implementation, contracts and source hashes.

Browser: Linux Chromium via Playwright CLI. qa-layout.js uses real mouse input,
file download/upload and explicit delayed File.text injection for race cases.
The legacy qa-browser.js changes only output prefixes to preserve prior artifacts.
No claim of embedded Codex browser or physical touchscreen verification is made.

Reproduce: build with Zig 0.15.2 using manage.py build; run Node *.test.mjs with
child-process support. Serve manage.py serve --port 8766 and execute qa-layout.js
with Playwright CLI run-code against a fresh blank workbench page.
