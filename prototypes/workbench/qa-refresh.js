// Run through qa-refresh.py. Only the archived app module is replaced for the
// baseline; runtime, compiler, output adapter and Wasm stay identical.
async (page) => {
  const baselineApp = /* BASELINE_APP */ null;
  const checks = [], samples = [], errors = [];
  const check = (ok, name) => { if (!ok) throw new Error(name); checks.push(name); };
  const onDialog = dialog => dialog.accept();
  const onError = error => errors.push(error.message);
  page.on('dialog', onDialog); page.on('pageerror', onError);
  let variant = 'after';
  const routeApp = async route => {
    const response = await route.fetch();
    await route.fulfill({response, body:variant === 'before' ? baselineApp : await response.text()});
  };
  const client = await page.context().newCDPSession(page);
  const metrics = async () => Object.fromEntries((await client.send('Performance.getMetrics')).metrics.map(m => [m.name, m.value]));
  const source = ['state x = 40', 'input move = KeyM',
    'box moving(x, 100, 30, 30, "#7ed9ae")', 'text label(20, 40, x)',
    ...Array.from({length:22}, (_, i) => `box static${i}(${20 + (i % 7) * 70}, ${200 + Math.floor(i / 7) * 30}, 20, 20, "#8da5bb")`),
    'on move.down { x = x + 0.25 }'].join('\n');
  const apply = async value => { await page.locator('#source').fill(value); await page.locator('#apply').click(); };
  const snapshot = () => page.evaluate(() => window.pureWorkbench.snapshot());
  const reload = async () => {
    await page.reload(); await page.waitForFunction(() => window.pureWorkbench?.snapshot().ready);
    await apply(source); await page.locator('#scene').focus();
  };
  try {
    if (!baselineApp) throw new Error('Archived app source is required.');
    await page.route('**/app.js', routeApp);
    for (const mode of ['before', 'after', 'after', 'before', 'before', 'after']) {
      variant = mode; await reload();
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
      const sample = await page.evaluate(() => {
        const observer = new MutationObserver(() => {});
        for (const id of ['line-numbers','source-size','cursor-position','draft-status'])
          observer.observe(document.getElementById(id), {subtree:true, childList:true, characterData:true, attributes:true});
        const originalEncode = TextEncoder.prototype.encode;
        let encodingCalls = 0, encodedBytes = 0;
        TextEncoder.prototype.encode = function(...args) {
          const value = originalEncode.apply(this, args); encodingCalls++; encodedBytes += value.byteLength; return value;
        };
        let elapsedMs;
        try {
          const start = performance.now();
          for (let i = 0; i < 100; i++) window.__tap();
          elapsedMs = performance.now() - start;
        } finally { TextEncoder.prototype.encode = originalEncode; }
        const editorMutations = observer.takeRecords().length;
        observer.disconnect();
        return {elapsedMs, encodingCalls, encodedBytes, editorMutations};
      });
      const after = await metrics();
      const s = await snapshot();
      samples.push({variant:mode, taps:100, events:200, ...sample,
        metrics:Object.fromEntries(['LayoutCount','LayoutDuration','RecalcStyleCount','RecalcStyleDuration','ScriptDuration']
          .map(key => [key, after[key]-before[key]])),
        final:{states:s.states, views:s.views, successfulEvents:s.successfulEvents, tick:s.tick, lastError:s.lastError},
        renderedX:await page.locator('[data-view-id="moving"]').getAttribute('x')});
    }
    check(samples.every(s => JSON.stringify(s.final) === JSON.stringify(samples[0].final) && s.renderedX === '70'),
      'six runs preserve exact authored states, outputs, event counts and rendered position');
    check(samples.filter(s => s.variant === 'before').every(s => s.encodingCalls === 400 && s.editorMutations > 0),
      'archived path reproduces unrelated editor work on every event');
    check(samples.filter(s => s.variant === 'after').every(s => s.encodingCalls === 0 && s.editorMutations === 0),
      'runtime input performs no source encoding or editor metadata mutations');

    variant = 'after'; await reload();
    const draft = source + '\n# 변경🙂\n';
    await page.locator('#source').fill(draft);
    const editor = () => page.evaluate(() => ({
      source:document.getElementById('source').value,
      size:document.getElementById('source-size').textContent,
      lines:document.getElementById('line-numbers').textContent,
      status:document.getElementById('draft-status').textContent,
      cursor:document.getElementById('cursor-position').textContent,
      expectedBytes:new TextEncoder().encode(document.getElementById('source').value).byteLength,
    }));
    let e = await editor();
    check(e.status === '미적용 변경' && e.size.startsWith(e.expectedBytes.toLocaleString('en-US')) &&
      e.lines.split('\n').length === draft.split('\n').length,
      'Unicode source editing updates byte count, line numbers and dirty status');
    await page.locator('#scene').focus();
    const prior = await editor();
    await page.keyboard.press('KeyM');
    await page.locator('#step').click();
    check(JSON.stringify(await editor()) === JSON.stringify(prior) && (await snapshot()).states.x === 40.25 && (await snapshot()).tick === 1,
      'keyboard input and frame stepping preserve unapplied editor metadata');
    await page.locator('#reset').click();
    check(JSON.stringify(await editor()) === JSON.stringify(prior) && (await snapshot()).states.x === 40,
      'reset preserves draft and its metadata while restoring active state');
    await page.locator('#apply').click();
    check((await editor()).status === '적용됨' && !(await snapshot()).dirty,
      'successful apply updates draft status without requiring a source edit');
    await apply(draft + 'state broken =');
    check((await editor()).status === '미적용 변경' && (await snapshot()).appliedSource === draft &&
      await page.locator('#error-location').isVisible(),
      'failed apply preserves dirty status and active work with a source diagnostic');
    await page.locator('#error-location').click();
    check((await editor()).cursor.startsWith(`${draft.split('\n').length}행`),
      'diagnostic navigation still updates the cursor position');
    await page.locator('#source').fill('state count = 1\n');
    await page.locator('#source').press('End');
    await page.locator('#source').press('Tab');
    e = await editor();
    check(e.source.endsWith('  ') && e.size.startsWith(e.expectedBytes.toLocaleString('en-US')),
      'programmatic Tab insertion updates source metadata');
    await apply('');
    check((await editor()).status === '적용됨' && (await snapshot()).views.length === 0,
      'applying an empty program has a correct applied status');
    await page.locator('[data-snippet="state"]').click();
    e = await editor();
    check(e.status === '미적용 변경' && e.source.includes('state count') &&
      e.size.startsWith(e.expectedBytes.toLocaleString('en-US')),
      'snippet insertion updates source metadata');
    check(errors.length === 0, 'no uncaught browser errors');
    await page.unroute('**/app.js', routeApp);
    await page.reload(); await page.waitForFunction(() => window.pureWorkbench?.snapshot().ready);
    const evidence = {utc:new Date().toISOString(), checks, errors, samples, source,
      browser:await page.evaluate(() => navigator.userAgent),
      scope:'100 synchronous synthetic keyboard taps after 20 warm-up taps. Timings include encoding/mutation instrumentation, exclude actual input arrival and presentation latency. CDP metrics may include browser work around the batch.',
      setup:'Only app.js is substituted from the verified archive for before runs. Other production modules are identical. No phase-timing wrappers in this comparison.'};
    await page.evaluate(value => {window.__pureRefreshEvidence = value;}, evidence);
    return evidence;
  } catch (error) {
    const evidence = {failed:true, message:error.stack || error.message, checks, errors, samples};
    await page.evaluate(value => {window.__pureRefreshEvidence = value;}, evidence);
    return evidence;
  } finally {
    await page.unroute('**/app.js', routeApp);
    page.off('dialog', onDialog); page.off('pageerror', onError); await client.detach();
  }
}
