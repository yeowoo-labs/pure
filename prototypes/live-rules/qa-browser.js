// Pass this expression to Playwright CLI's run-code after opening the local demo.
// Interactions use real UI controls; the read-only snapshot only observes results.
async (page) => {
  const checks = [];
  const errors = [];
  const onError = (error) => errors.push(error.message);
  const check = (condition, label) => {
    if (!condition) throw new Error(label);
    checks.push(label);
  };
  const state = () => page.evaluate(() => window.pureDemo.snapshot());
  const button = (name) => page.getByRole('button', { name, exact: true });
  const step = async (count) => { for (let i = 0; i < count; i++) await button('한 프레임').click(); };
  page.on('pageerror', onError);
  try {
    await page.setViewportSize({ width: 1440, height: 1000 });
    await page.reload();
    await page.waitForFunction(() => window.pureDemo?.snapshot().ready);
    const initial = await state();
    check(initial.running === 0 && initial.tick === 0 && initial.revision === 0, 'initial state is paused');

    await page.locator('#stage').focus();
    await page.keyboard.press('Space');
    let s = await state();
    check(s.accepted === 1 && s.inputs === 1 && s.source === 0 && s.y === 0, 'Space goes through keyboard input once while paused');
    await step(8);
    await button('점프 Space').click();
    const before = await state();
    check(before.tick === 8 && before.y === 6020 && before.vy === 560 && before.reason === 3, 'ground rule rejects a second airborne jump');
    await page.getByRole('radio', { name: '두 번 점프 공중에서 한 번 더 뛸 수 있어요.', exact: true }).check();
    const draft = await state();
    check(draft.mode === 0 && draft.revision === 0 && draft.draftDirty, 'draft changes do not execute before Apply');
    await button('규칙 적용').click();
    const applied = await state();
    check(['tick', 'y', 'vy', 'distance', 'obstacleX', 'jumps'].every((key) => applied[key] === before[key]), 'applying rules preserves exact simulation progress');
    check(applied.mode === 1 && applied.revision === 1 && applied.decisionRevision === 0 && await page.locator('#stale-decision').isVisible(), 'old decision retains and displays its revision');
    await button('점프 Space').click();
    const second = await state();
    check(second.reason === 1 && second.jumps === 2 && second.source === 1 && second.decisionRevision === 1, 'pointer input uses new rule and accepts second jump');
    await button('점프 Space').click();
    check((await state()).reason === 4, 'third jump is rejected');

    await page.getByRole('checkbox').uncheck();
    await button('규칙 적용').click();
    await button('점프 Space').click();
    check((await state()).reason === 2, 'disabled rule explains rejection');
    await button('처음부터').click();
    s = await state();
    check(s.tick === 0 && s.mode === 0 && s.impulse === 10 && s.enabled === 1 && s.inputs === 0 && s.running === 0, 'explicit reset restores rules and state');

    await page.getByRole('slider', { name: '점프 세기' }).focus();
    await page.keyboard.press('Space');
    await page.getByRole('checkbox').focus();
    await page.keyboard.press('Space');
    check((await state()).inputs === 0, 'Space in form controls never dispatches a jump');
    await button('처음부터').click();
    await button('점프 Space').focus();
    await page.keyboard.press('Space');
    check((await state()).inputs === 1, 'Space on the jump button does not double dispatch');
    await button('처음부터').click();
    await page.locator('#stage').focus();
    await page.keyboard.down('Space');
    await page.keyboard.down('Space');
    await page.keyboard.up('Space');
    check((await state()).inputs === 1, 'keyboard repeat does not add a second request');

    await button('처음부터').click();
    await button('실행').click();
    await page.waitForFunction(() => window.pureDemo.snapshot().tick >= 6);
    await button('일시정지').click();
    const paused = await state();
    await page.waitForTimeout(120);
    check(paused.tick >= 6 && paused.tick === (await state()).tick, 'running advances and pause freezes simulation');

    await button('처음부터').click();
    await page.getByRole('radio', { name: '두 번 점프 공중에서 한 번 더 뛸 수 있어요.', exact: true }).check();
    await page.getByRole('slider', { name: '점프 세기' }).focus();
    await page.keyboard.press('End');
    await button('규칙 적용').click();
    await button('점프 Space').click();
    await step(28);
    await button('점프 Space').click();
    await step(29);
    const high = await state();
    const visible = await page.evaluate(() => {
      const d = document.getElementById('dino').getBoundingClientRect();
      const w = document.getElementById('world').getBoundingClientRect();
      return d.top >= w.top && d.bottom <= w.bottom && d.left >= w.left && d.right <= w.right;
    });
    check(high.y > 40000 && visible, 'maximum-height double jump remains inside the scene');

    for (let i = 0; i < 25; i++) await button('점프 Space').click();
    check(await page.locator('#history-list li').count() === 8, 'visible history is bounded to eight rows');
    check((await state()).wasmMemoryBytes === 65536, 'browser instance retains fixed 64 KiB linear memory');
    await page.locator('.research-panel summary').click();
    check((await page.locator('#bytes-total').textContent()).includes('396'), 'research panel shows logical layout separately');

    await page.setViewportSize({ width: 390, height: 844 });
    await page.locator('.research-panel summary').click();
    await button('처음부터').click();
    const width = await page.evaluate(() => ({ viewport: innerWidth, content: document.documentElement.scrollWidth }));
    check(width.content <= width.viewport, '390px viewport has no horizontal overflow');
    check(await page.locator('#dino').evaluate((el) => el.getBoundingClientRect().height) >= 28, 'narrow view keeps the dinosaur large enough to inspect');
    await button('점프 Space').click();
    await step(1);
    check((await state()).y === 945, 'narrow viewport controls remain usable');
    await page.screenshot({ path: 'output/playwright/pure-live-mobile.png', fullPage: true });

    await page.route('**/core.wasm', (route) => route.abort());
    await page.reload();
    await page.locator('#load-error').waitFor({ state: 'visible' });
    check(await button('실행').isDisabled(), 'missing Wasm has an actionable error and disabled controls');
    await page.unroute('**/core.wasm');
    await page.reload();
    await page.waitForFunction(() => window.pureDemo?.snapshot().ready);
    await page.setViewportSize({ width: 1440, height: 1000 });
    await button('점프 Space').click();
    await step(8);
    await button('점프 Space').click();
    await page.getByRole('radio', { name: '두 번 점프 공중에서 한 번 더 뛸 수 있어요.', exact: true }).check();
    await button('규칙 적용').click();
    await button('점프 Space').click();
    await page.screenshot({ path: 'output/playwright/pure-live-desktop.png', fullPage: true });
    check(errors.length === 0, 'no uncaught page errors');
    return { recordedUtc: new Date().toISOString(), browser: await page.evaluate(() => navigator.userAgent),
      checks, before, applied, second, high, mobile: width, errors,
      screenshots: ['output/playwright/pure-live-desktop.png', 'output/playwright/pure-live-mobile.png'] };
  } finally {
    page.off('pageerror', onError);
  }
}
