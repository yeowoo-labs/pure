// Playwright CLI run-code function. The Python runner injects the archived
// app/runtime source at BASELINE_SOURCES; production files are never replaced.
async (page) => {
  const baseline = /* BASELINE_SOURCES */ null;
  const checks = [], errors = [], samples = [];
  const check = (ok, name) => { if (!ok) throw new Error(name); checks.push(name); };
  const onError = error => errors.push(error.message);
  const onDialog = dialog => dialog.accept();
  page.on('pageerror', onError); page.on('dialog', onDialog);
  let variant = 'after';
  const source = [
    'state x = 40', 'state radius = 12', 'input move = KeyM', 'input shrink = KeyS',
    'box moving(x, 100, 30, 30, "#7ed9ae")', 'text label(20, 40, x, "x: ")',
    'circle bubble(500, 150, radius, "#c9f08b")',
    ...Array.from({length:21}, (_, i) => `box static${i}(${20 + (i % 7) * 70}, ${200 + Math.floor(i / 7) * 40}, 20, 20, "#8da5bb")`),
    'on move.down { x = x + 0.25 }', 'on shrink.down { radius = radius - 24 }',
  ].join('\n');
  const apply = async value => { await page.locator('#source').fill(value); await page.locator('#apply').click(); };
  const state = () => page.evaluate(() => window.pureWorkbench.snapshot());
  const reload = async () => {
    await page.reload();
    await page.waitForFunction(() => window.pureWorkbench?.snapshot().ready);
    await apply(source);
  };
  const routeApp = async route => {
    const response = await route.fetch();
    await route.fulfill({response, body: variant === 'before' ? baseline.app : await response.text()});
  };
  const routeRuntime = async route => {
    const response = await route.fetch();
    const body = variant === 'before' ? baseline.runtime : await response.text();
    if (!body.includes('snapshot() {')) throw new Error('snapshot instrumentation does not match');
    await route.fulfill({response, body:body.replace('snapshot() {', 'snapshot() { globalThis.__snapshotCalls = (globalThis.__snapshotCalls || 0) + 1;')});
  };
  try {
    if (!baseline) throw new Error('Run through qa-presentation.py to supply the archived baseline.');
    await page.route('**/app.js', routeApp);
    await page.route('**/runtime.mjs', routeRuntime);
    await reload();
    await page.locator('#scene').focus();
    await page.keyboard.press('KeyM');
    check((await state()).states.x === 40.25, 'real keyboard event reaches the authored output');
    await page.evaluate(() => { window.__retained = document.querySelector('[data-view-id="moving"]'); });
    await page.locator('[data-input="move"]').click();
    check((await state()).states.x === 40.5 && await page.locator('[data-view-id="moving"]').getAttribute('x') === '40.5',
      'real pointer input and keyboard input use the same authored behavior');
    check(await page.evaluate(() => window.__retained === document.querySelector('[data-view-id="moving"]')),
      'the displayed element survives an input update');
    await apply(source + '\nstate broken =');
    check((await state()).states.x === 40.5 && await page.evaluate(() => window.__retained === document.querySelector('[data-view-id="moving"]')),
      'failed editing preserves active values and output destinations');
    await page.locator('[data-input="shrink"]').click();
    check((await state()).lastError === 100 && await page.locator('[data-view-id="bubble"]').getAttribute('display') === 'none' &&
      await page.locator('[data-view-id="moving"]').getAttribute('display') === null,
      'invalid view is hidden while valid views remain published');
    await page.locator('#reset').click();
    check((await state()).lastError === 0 && await page.locator('[data-view-id="bubble"]').getAttribute('display') === null,
      'reset recovers a hidden destination without recreating the scene');
    await apply(source.replace('box moving(x, 100, 30, 30, "#7ed9ae")', 'circle moving(x, 100, 15, "#7ed9ae")'));
    check(await page.locator('[data-view-id="moving"]').evaluate(node => node.localName) === 'circle',
      'successful editing replaces changed output structure');
    await apply('');
    check(await page.locator('#scene-content > *').count() === 0 && (await state()).views.length === 0,
      'applying empty work removes all old destinations');

    // Alternating whole-page runs avoid sharing a runtime or retained DOM across
    // variants. Timings cover synchronous dispatch through DOM submission, not
    // OS event arrival, layout, paint, or actual display scanout.
    for (const mode of ['before', 'after', 'after', 'before', 'before', 'after']) {
      variant = mode;
      await reload();
      await page.locator('#scene').focus();
      const result = await page.evaluate(() => {
        const scene = document.getElementById('scene');
        const tap = () => {
          scene.dispatchEvent(new KeyboardEvent('keydown', {code:'KeyM', bubbles:true}));
          scene.dispatchEvent(new KeyboardEvent('keyup', {code:'KeyM', bubbles:true}));
        };
        for (let i = 0; i < 20; i++) tap();
        const retained = [...document.querySelectorAll('#scene-content > *')];
        const scopes = ['scene-content', 'state-values', 'cost-values'];
        const observers = scopes.map(id => {
          const observer = new MutationObserver(() => {});
          observer.observe(document.getElementById(id), {subtree:true, childList:true, attributes:true, characterData:true});
          return observer;
        });
        const createElement = document.createElement, createElementNS = document.createElementNS;
        let createdElements = 0;
        document.createElement = function(...args) { createdElements++; return createElement.apply(this, args); };
        document.createElementNS = function(...args) { createdElements++; return createElementNS.apply(this, args); };
        window.__snapshotCalls = 0;
        let elapsedMs;
        try {
          const start = performance.now();
          for (let i = 0; i < 100; i++) tap();
          elapsedMs = performance.now() - start;
        } finally {
          document.createElement = createElement; document.createElementNS = createElementNS;
        }
        const snapshotCalls = window.__snapshotCalls;
        const mutations = Object.fromEntries(observers.map((observer, i) => {
          const records = observer.takeRecords(); observer.disconnect();
          return [scopes[i], {records:records.length,
            childList:records.filter(record => record.type === 'childList').length,
            attributes:records.filter(record => record.type === 'attributes').length}];
        }));
        const nodes = [...document.querySelectorAll('#scene-content > *')];
        const identityPreserved = nodes.every((node, i) => node === retained[i]);
        const final = window.pureWorkbench.snapshot(); // outside timed/count interval
        return {elapsedMs, createdElements, snapshotCalls, mutations, identityPreserved,
          final: {states:final.states, views:final.views, successfulEvents:final.successfulEvents},
          scene:document.getElementById('scene-content').outerHTML};
      });
      samples.push({variant:mode, taps:100, events:200, ...result});
    }
    check(samples.every(sample => JSON.stringify(sample.final) === JSON.stringify(samples[0].final)),
      'all six archived/current runs have identical final authored state and views');
    check(samples.filter(s => s.variant === 'after').every(s => s.createdElements === 0 && s.snapshotCalls === 0 && s.identityPreserved),
      'current hot path creates no DOM elements and no full snapshots');
    check(samples.filter(s => s.variant === 'before').every(s => s.createdElements > 0 && s.snapshotCalls === 200 && !s.identityPreserved),
      'archived hot path reproduces full snapshots and element replacement');
    check(samples.filter(s => s.variant === 'after').every(s => s.mutations['scene-content'].attributes === 100 && s.mutations['scene-content'].childList === 100),
      'each tap changes only the moving x attribute and label text in the scene');
    variant = 'after'; await reload();
    await page.screenshot({path:'output/playwright/pure-presentation.png', fullPage:true});
    check(errors.length === 0, 'no uncaught browser errors');
    const evidence = {utc:new Date().toISOString(), checks, errors, samples, source,
      timingScope:'100 synchronous synthetic keyboard taps after 20 warm-up taps; JS dispatch to DOM submission only, including instrumentation; not paint/input latency.',
      setup:'Archived app/runtime are supplied by test-only routing; snapshot calls and element creation are instrumented. Same Wasm/compiler and production configuration.',
      browser:await page.evaluate(() => navigator.userAgent), screenshot:'output/playwright/pure-presentation.png'};
    await page.evaluate(value => { window.__purePresentationEvidence = value; }, evidence);
    return evidence;
  } catch (error) {
    const evidence = {failed:true, message:error.stack || error.message, checks, errors, samples};
    await page.evaluate(value => { window.__purePresentationEvidence = value; }, evidence);
    return evidence;
  } finally {
    await page.unroute('**/app.js', routeApp); await page.unroute('**/runtime.mjs', routeRuntime);
    page.off('pageerror', onError); page.off('dialog', onDialog);
  }
}
