# Drag release regression

User reported rollback after mouse release in the Codex browser, with either up
or cancel displayed. The embedded browser event sequence was not directly observed.

`before-browser.json` reproduces rollback by delivering pointerup on document,
then releasing SVG capture. This event injection tests a delivery-path hypothesis;
it is not a recording from the user's browser.
`after-browser.json` contains 14 release regression checks using real mouse drags
and explicit release/cancel event injections. The example now commits the last
rendered position on up; authors still receive the original up coordinates.
Actual root capture loss, pointercancel and window blur still cancel.

Use `qa-release.js` with Playwright CLI run-code on the local workbench.
Implementation and test sources are under `after/`. Prior pointer results remain
unchanged in the sibling 2026-09-23-pointer directory.

`pointer-browser.json`: 27 passing mouse/touch checks, including ignoring mouseup
while a touch owns a target. Pointer and CLI Node suites: 12 tests passed.
