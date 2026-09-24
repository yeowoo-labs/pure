// Playwright CLI function: one diagnostic run, no implementation changes.
async (page) => {
  const onDialog = dialog => dialog.accept();
  page.on('dialog', onDialog);
  const routeApp = async route => {
    const response = await route.fetch();
    let body = await response.text();
    const measured = (key, expression) => `measure('${key}', () => ${expression})`;
    body = body.replace("const output = createOutput(document);", `const output = createOutput(document);
      function measure(name, action) {
        const start = performance.now();
        try { return action(); } finally {
          const all = globalThis.__phase ??= {};
          const item = all[name] ??= {calls:0, ms:0};
          item.calls++; item.ms += performance.now() - start;
        }
      }`);
    body = body.replace('function refresh() {', "function refresh() { return measure('refresh', refreshBody); }\nfunction refreshBody() {");
    body = body.replace('function draftChanged() {', "function draftChanged() { return measure('draft', draftBody); }\nfunction draftBody() {");
    body = body.replace('runtime.present(output)', measured('present', 'runtime.present(output)'));
    body = body.replace('output.costs(snapshot, runtime.currentProgram?.metrics, droppedTicks)',
      measured('costs', 'output.costs(snapshot, runtime.currentProgram?.metrics, droppedTicks)'));
    await route.fulfill({response, body});
  };
  const client = await page.context().newCDPSession(page);
  const metrics = async () => Object.fromEntries((await client.send('Performance.getMetrics')).metrics.map(m => [m.name, m.value]));
  const source = ['state x = 40', 'input move = KeyM',
    'box moving(x, 100, 30, 30, "#7ed9ae")', 'text label(20, 40, x)',
    ...Array.from({length:22}, (_, i) => `box static${i}(${20 + (i % 7) * 70}, ${200 + Math.floor(i / 7) * 30}, 20, 20, "#8da5bb")`),
    'on move.down { x = x + 0.25 }'].join('\n');
  try {
    await page.route('**/app.js', routeApp);
    await page.reload(); await page.waitForFunction(() => window.pureWorkbench?.snapshot().ready);
    await page.locator('#source').fill(source); await page.locator('#apply').click();
    await page.locator('#scene').focus();
    await page.evaluate(() => {
      window.__tap = () => {
        const scene = document.getElementById('scene');
        scene.dispatchEvent(new KeyboardEvent('keydown', {code:'KeyM', bubbles:true}));
        scene.dispatchEvent(new KeyboardEvent('keyup', {code:'KeyM', bubbles:true}));
      };
      for (let i = 0; i < 20; i++) window.__tap();
    });
    await client.send('Performance.enable');
    const before = await metrics();
    const run = await page.evaluate(() => {
      window.__phase = {};
      const start = performance.now();
      for (let i = 0; i < 100; i++) window.__tap();
      return {elapsedMs:performance.now()-start, phase:window.__phase};
    });
    const after = await metrics();
    const delta = Object.fromEntries(['LayoutCount','LayoutDuration','RecalcStyleCount','RecalcStyleDuration','ScriptDuration']
      .map(key => [key, after[key]-before[key]]));
    const evidence = {utc:new Date().toISOString(), taps:100, ...run, metrics:delta, source,
      scope:'Phase timings overlap: draft/present/costs are inside refresh. CDP counters bracket the synchronous batch and may include surrounding browser work.'};
    await page.evaluate(value => {window.__pureRefreshProfile = value;}, evidence);
    return evidence;
  } catch (error) {
    const evidence = {failed:true, message:error.stack};
    await page.evaluate(value => {window.__pureRefreshProfile = value;}, evidence);
    return evidence;
  } finally {
    await page.unroute('**/app.js', routeApp); page.off('dialog', onDialog); await client.detach();
  }
}
