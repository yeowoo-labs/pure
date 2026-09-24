# Shared rule bodies — 2026-09-23

- build.json: final contiguous-range binding ABI, matching source/Wasm hashes,
  32 native tests passed.
- verification.json: 75 Node tests passed. Compiler/rules suites include the final
  parameter-prefix shadowing guard and the generated-name document size boundary.
- shared-browser.json: 23 checks on the final contiguous-binding build; mouse,
  two-touch ownership/cancel, editing one shared rule, reordered declarations,
  file download and fresh-page restoration, async read conflicts.
- authoring-browser.json: 44 existing authoring checks passed.
- representation.json: compile/layout accounting on the same final core for
  explicit and shared versions. Not a throughput, latency or process RAM claim.
- before/: previous implementation references. sources/ and manifest.json:
  final implementation and identities. Screenshots and actual downloaded layout
  are retained beside the JSON results.

The browser scenarios precede the final compiler-only prefix-shadow rejection
check; that diagnostic change is covered by the final Node compiler/rules suites.
No browser performance benchmark was run. Browser is Linux Chromium; touches use
CDP emulation, not physical hardware. Codex embedded browser is not directly driven.

Build with Zig 0.15.2 via manage.py build. Run *.test.mjs with Node child-process
support. Serve on port 8766 and run qa-shared-rules.js through Playwright CLI on a
fresh blank workbench page. The ordinary qa-browser.js changes only artifact names.
