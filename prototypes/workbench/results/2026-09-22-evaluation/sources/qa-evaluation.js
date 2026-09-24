// Run with Playwright CLI run-code on the local workbench. The final UI case
// overrides only createRuntime's default options in the served module so that
// a deliberately tiny budget can exercise the normally unreachable limit UI.
async (page) => {
  const checks = [], errors = [];
  const onError = error => errors.push(error.message);
  const onDialog = dialog => dialog.accept();
  page.on('pageerror', onError);
  page.on('dialog', onDialog);
  const check = (condition, name) => {
    if (!condition) throw new Error(name);
    checks.push(name);
  };
  const source = 'state x = 3\ninput next = KeyN\ntext label(40, 60, (x + 1) * (x + 2) / 2)\non next.down { x = x + 0.25 }\n';
  const state = () => page.evaluate(() => window.pureWorkbench.snapshot());
  const apply = async source => {
    await page.locator('#source').fill(source);
    await page.locator('#apply').click();
  };
  try {
    await page.reload();
    await page.waitForFunction(() => window.pureWorkbench?.snapshot().ready);
    await apply(source);
    await page.locator('[data-input="next"]').click();
    check((await state()).states.x === 3.25, 'authored browser input commits fractional state');
    check((await state()).views[0].value === 11.15625, 'bounded evaluation renders the exact authored expression');
    await apply(source.replace('(x + 1)', '(x + 10)'));
    check((await state()).states.x === 3.25 && (await state()).views[0].value === 34.78125,
      'code replacement preserves state and changes binding result');

    const api = await page.evaluate(async source => {
      const { createRuntime } = await import('./runtime.mjs');
      const runtime = await createRuntime(undefined, {cacheEntries:32});
      runtime.apply(source); runtime.snapshot();
      const before = runtime.snapshot(), warm = runtime.evaluationStats();
      runtime.forgetEvaluation('all');
      const forgotten = runtime.snapshot();
      runtime.configureEvaluation({workLimit:1});
      const limited = runtime.snapshot(), limitStats = runtime.evaluationStats();
      runtime.configureEvaluation({workLimit:2048});
      return {before,warm,forgotten,limited,limitStats,recovered:runtime.snapshot()};
    }, source);
    check(api.warm.cacheHits > 0, 'browser Wasm reuses completed bindings');
    check(JSON.stringify(api.before) === JSON.stringify(api.forgotten), 'discarding records preserves the browser snapshot');
    check(api.limited.lastError === 101 && api.limited.views.length === 0 && api.limitStats.work <= 1,
      'browser API publishes no partial scene after budget exhaustion');
    check(JSON.stringify(api.before) === JSON.stringify(api.recovered), 'raising the budget recovers the browser snapshot');

    await page.route('**/runtime.mjs', async route => {
      const response = await route.fetch();
      const body = await response.text();
      if (!body.includes('evaluationOptions = {}')) throw new Error('configuration setup no longer matches');
      await route.fulfill({response, body:body.replace('evaluationOptions = {}', 'evaluationOptions = { workLimit: 1 }')});
    });
    await page.reload();
    await page.waitForFunction(() => window.pureWorkbench?.snapshot().ready);
    await apply(source);
    await page.locator('[data-input="next"]').click();
    const limitedUi = await state();
    const message = await page.locator('#runtime-error').textContent();
    check(limitedUi.states.x === 3.25 && limitedUi.views.length === 0,
      'configured UI keeps committed state while withholding incomplete display');
    check(await page.locator('#runtime-error').isVisible() && message.includes('표시 오류') &&
      message.includes('상태 변경은 유지'), 'UI distinguishes display limit from event rollback');
    await page.screenshot({path:'output/playwright/pure-evaluation-limit.png',fullPage:true});
    await page.unroute('**/runtime.mjs');
    await page.reload();
    await page.waitForFunction(() => window.pureWorkbench?.snapshot().ready);
    check((await state()).views.length === 0 && (await state()).lastError === 0, 'normal configuration reloads to a clean blank work');
    check(errors.length === 0, 'no uncaught browser errors');
    const evidence = {utc:new Date().toISOString(),checks,api,limitedUi,message,errors,
      uiLimitSetup:'Test-only module response overrides default workLimit to 1; production files are unchanged.',
      screenshot:'output/playwright/pure-evaluation-limit.png',
      browser:await page.evaluate(()=>navigator.userAgent)};
    await page.evaluate(value=>{window.__pureEvaluationEvidence=value;},evidence);
    return evidence;
  } catch (error) {
    const evidence = {failed:true,message:error.message,checks,errors};
    await page.evaluate(value=>{window.__pureEvaluationEvidence=value;},evidence);
    return evidence;
  } finally {
    await page.unroute('**/runtime.mjs');
    page.off('pageerror',onError);
    page.off('dialog',onDialog);
  }
}
