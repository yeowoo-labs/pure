// Run this expression with Playwright CLI run-code after opening the local host.
// All behavior is authored through the editor; snapshot() only observes results.
async (page) => {
  const checks = [];
  const errors = [];
  const onError = error => errors.push(error.message);
  const acceptDialog = dialog => dialog.accept();
  const check = (condition, label) => {
    if (!condition) throw new Error(`Check ${checks.length + 1} failed: ${label}`);
    checks.push(label);
  };
  const state = () => page.evaluate(() => window.pureWorkbench.snapshot());
  const apply = async text => { await page.locator('#source').fill(text); await page.locator('#apply').click(); };
  const authorSource = [
    '# A new authored interaction, not a bundled example.',
    'state x = 90', 'state moving = 0', 'state count = 0',
    'input advance = ArrowRight', 'input pulse = Space',
    'box marker(x, 170, 48, 48, "#7ed9ae")',
    'text reading(40, 70, count, "Signals: ")',
    'on advance.down { moving = 1 }', 'on advance.up { moving = 0 }',
    'on pulse.down { count = count + 1 }',
    'on frame { x = clamp(x + moving * 120 * dt, 0, 580) }', '',
  ].join('\n');
  let lastAuthored = null;
  let mobile = null;
  let downloaded = null;
  const shots = ['output/playwright/pure-document-authoring-blank.png', 'output/playwright/pure-document-authoring-authored.png', 'output/playwright/pure-document-authoring-mobile.png'];
  page.on('pageerror', onError);
  page.on('dialog', acceptDialog);
  try {
    await page.goto('http://127.0.0.1:8766/');
    await page.setViewportSize({ width: 1440, height: 1000 });
    await page.reload();
    await page.waitForFunction(() => window.pureWorkbench?.snapshot().ready);
    let s = await state();
    check(s.source === '' && s.appliedSource === '' && !s.dirty, 'initial editor has literally empty source');
    check(s.views.length === 0 && Object.keys(s.states).length === 0 && s.held.length === 0 && !s.running, 'initial scene has no built-in entities, states, inputs, or running clock');
    check(await page.locator('#input-controls button').count() === 0 && await page.locator('#play').isDisabled(), 'controls require an authored program');
    await page.screenshot({ path: shots[0], fullPage: true });

    await page.locator('#source').fill(authorSource);
    s = await state();
    check(s.dirty && s.views.length === 0 && s.appliedSource === '', 'authoring a new program is a draft until Apply');
    await page.locator('#apply').click();
    s = await state();
    check(!s.dirty && s.appliedSource === authorSource && s.views.length === 2 && s.states.x === 90 && s.states.count === 0, 'new custom program constructs states and multiple views from source');
    check(await page.locator('#input-controls button').count() === 2 && await page.locator('[data-input="advance"]').count() === 1 && await page.locator('[data-input="pulse"]').count() === 1, 'input buttons are generated from the authored declarations');
    check(await page.locator('#scene-content [data-view-id="marker"]').getAttribute('x') === '90', 'view bindings render the authored state value');

    await page.locator('#source').focus();
    await page.keyboard.press('End');
    await page.keyboard.press('Space');
    await page.keyboard.press('ArrowRight');
    check((await state()).states.count === 0 && (await state()).states.moving === 0, 'Space and arrows in the editor never reach authored inputs');
    await page.locator('#source').fill(authorSource);

    const pulse = page.locator('[data-input="pulse"]');
    let box = await pulse.boundingBox();
    await page.mouse.move(box.x + box.width / 2, box.y + box.height / 2);
    await page.mouse.down();
    s = await state();
    check(s.states.count === 1 && s.held.includes('pulse') && !s.running && s.tick === 0, 'pointer down dispatches once while the clock is paused');
    await page.mouse.up();
    check((await state()).held.length === 0 && (await state()).states.count === 1, 'pointer up releases without double dispatch from click');

    await page.locator('#scene').focus();
    await page.keyboard.down('Space');
    await page.keyboard.down('Space');
    await page.keyboard.up('Space');
    check((await state()).states.count === 2, 'keyboard repeat is ignored');
    await pulse.focus();
    await page.keyboard.press('Enter');
    check((await state()).states.count === 3 && (await state()).held.length === 0, 'keyboard activation on a generated button is one complete tap');
    await pulse.dispatchEvent('click', { detail: 0 });
    check((await state()).states.count === 4 && (await state()).held.length === 0, 'assistive click activation is one complete tap');

    await page.locator('#scene').focus();
    await page.keyboard.down('Space');
    box = await pulse.boundingBox();
    await page.mouse.move(box.x + box.width / 2, box.y + box.height / 2);
    await page.mouse.down();
    check((await state()).states.count === 5 && (await state()).held.includes('pulse'), 'keyboard and pointer sources share one logical input');
    await page.mouse.up();
    check((await state()).held.includes('pulse'), 'releasing one source does not release the other source');
    await page.keyboard.up('Space');
    check((await state()).held.length === 0, 'last source releases the logical input');

    await page.locator('#scene').focus();
    await page.keyboard.down('ArrowRight');
    await page.locator('#step').click();
    await page.locator('#step').click();
    s = await state();
    check(s.states.x === 94 && s.states.moving === 1 && s.tick === 2, 'authored down/frame behavior uses the declared dt without built-in motion');
    await page.locator('#source').focus();
    await page.keyboard.up('ArrowRight');
    check((await state()).states.moving === 0 && (await state()).held.length === 0, 'keyup releases even after focus moves into the editor');
    await page.locator('#scene').focus();
    await page.keyboard.down('ArrowRight');
    await page.evaluate(() => window.dispatchEvent(new Event('blur')));
    await page.keyboard.up('ArrowRight');
    check((await state()).states.moving === 0 && !(await state()).running && (await state()).held.length === 0, 'focus loss sends release and pauses the clock');

    await page.locator('#play').click();
    await page.waitForFunction(() => window.pureWorkbench.snapshot().tick >= 5);
    await page.locator('#play').click();
    const paused = await state();
    await page.waitForTimeout(100);
    check(!paused.running && paused.tick === (await state()).tick && paused.tick >= 5, 'Play advances and Pause freezes the clock');

    const beforeFailure = await state();
    await apply(authorSource + 'state broken =\n');
    s = await state();
    check(s.appliedSource === authorSource && s.revision === beforeFailure.revision && JSON.stringify(s.states) === JSON.stringify(beforeFailure.states), 'invalid code preserves the previous executable program and states');
    check(await page.locator('#error-location').isVisible() && /행/.test(s.diagnostic) && await page.locator('#source').getAttribute('aria-invalid') === 'true', 'compile error exposes a source-line diagnostic');
    await page.locator('#error-location').click();
    check(await page.locator('#source').evaluate(el => el.selectionStart >= el.value.indexOf('state broken')), 'diagnostic location moves the editor caret to the problem');

    const expanded = authorSource.replace('state x = 90', 'state x = 500').replace('count = count + 1', 'count = count + 5') + [
      'state bonus = 7', 'input reverse = KeyA',
      'circle beacon(300, 190, 15 + bonus, "#c9f08b")',
      'on reverse.down { bonus = bonus + 2 }', '',
    ].join('\n');
    await apply(expanded);
    s = await state();
    check(s.states.x === beforeFailure.states.x && s.states.count === 5 && s.states.bonus === 7 && s.views.length === 3, 'apply preserves matching state names and initializes newly authored states');
    await page.locator('[data-input="reverse"]').click();
    check((await state()).states.bonus === 9 && await page.locator('#scene-content [data-view-id="beacon"]').getAttribute('r') === '24', 'new input, entity, binding, and behavior work with source changes alone');
    await page.locator('[data-input="pulse"]').click();
    check((await state()).states.count === 10, 'changed behavior takes effect without resetting existing values');

    const withDraft = expanded + '# This un-applied draft must survive Reset.\n';
    await page.locator('#source').fill(withDraft);
    await page.locator('#reset').click();
    s = await state();
    check(s.states.x === 500 && s.states.count === 0 && s.states.bonus === 7 && s.tick === 0 && s.source === withDraft && s.dirty, 'explicit Reset restores active initializers and keeps the draft');
    await page.locator('#scene').focus();
    await page.keyboard.down('ArrowRight');
    const heldRevision = (await state()).revision;
    await page.locator('#apply').click();
    check((await state()).revision === heldRevision && /놓은/.test((await state()).diagnostic), 'Apply is refused while an input is held');
    await page.keyboard.up('ArrowRight');

    const downloadPromise = page.waitForEvent('download');
    await page.locator('#download-project').click();
    const projectDownload = await downloadPromise;
    const downloadPath = 'output/playwright/pure-document-authoring-draft.json';
    await projectDownload.saveAs(downloadPath);
    downloaded = { filename: projectDownload.suggestedFilename(), path: downloadPath, expectedSource: withDraft };
    await page.locator('#new').click();
    s = await state();
    check(s.source === '' && s.views.length === 3 && s.appliedSource === expanded, 'clear editor is explicit and does not clear the running scene before Apply');
    await page.locator('#file-input').setInputFiles(downloadPath);
    await page.waitForFunction(expected => window.pureWorkbench.snapshot().source === expected, withDraft);
    check((await state()).source === withDraft && (await state()).appliedSource === expanded && (await state()).dirty, 'downloaded project restores the exact un-applied draft without execution');
    await page.locator('#file-input').evaluate(input => {
      const transfer = new DataTransfer();
      transfer.items.add(new File(['{"format":"other","version":1,"source":"state no = 1"}'], 'invalid.json', { type: 'application/json' }));
      input.files = transfer.files;
      input.dispatchEvent(new Event('change', { bubbles: true }));
    });
    await page.waitForFunction(() => window.pureWorkbench.snapshot().diagnostic.includes('형식'));
    check((await state()).source === withDraft && (await state()).appliedSource === expanded, 'invalid JSON project schema cannot replace the draft');

    await apply('state n = 1\ninput fail = Space\ntext label(40, 60, n)\non fail.down { n = n / 0 }\n');
    await page.locator('[data-input="fail"]').dispatchEvent('click', { detail: 0 });
    s = await state();
    check(s.states.n === 1 && s.lastError === 4 && s.held.length === 0 && s.lastEvent.phase === 'up', 'failed assistive down event rolls back and its error survives the following successful up');
    check(await page.locator('#runtime-error').isVisible() && (await page.locator('#runtime-error').textContent()).includes('마지막 실패'), 'latched runtime failure is visible and labeled separately from the latest input');
    await page.locator('#reset').click();
    check((await state()).lastError === 0 && await page.locator('#runtime-error').isHidden(), 'explicit Reset clears the failure record');

    await page.locator('#new').click();
    await page.locator('#apply').click();
    s = await state();
    check(s.source === '' && s.appliedSource === '' && s.views.length === 0 && Object.keys(s.states).length === 0 && await page.locator('#input-controls button').count() === 0, 'applying empty source removes all authored states, inputs, and scene contents');
    await page.locator('#example').selectOption('counter');
    await page.locator('#load-example').click();
    await page.waitForFunction(() => window.pureWorkbench.snapshot().source.includes('state count'));
    check((await state()).views.length === 0 && (await state()).appliedSource === '' && (await state()).dirty, 'explicit example loading only fills the editor and never auto-applies');

    await apply(authorSource);
    await page.locator('[data-input="pulse"]').click();
    await page.locator('[data-input="pulse"]').click();
    await page.locator('#scene').focus();
    await page.keyboard.down('ArrowRight');
    for (let i = 0; i < 12; i++) await page.locator('#step').click();
    await page.keyboard.up('ArrowRight');
    lastAuthored = await state();
    check(lastAuthored.states.x === 114 && lastAuthored.states.count === 2 && lastAuthored.memory.linearBytes === 65536, 'authored scene executes in the actual fixed 64 KiB Wasm memory');
    await page.locator('.cost-details summary').click();
    check((await page.locator('#cost-values').textContent()).includes('65,536 B'), 'cost inspector shows measured Wasm memory without claiming browser heap totals');
    await page.locator('.cost-details summary').click();
    await page.screenshot({ path: shots[1], fullPage: true });

    await page.setViewportSize({ width: 390, height: 844 });
    mobile = await page.evaluate(() => ({ viewport: innerWidth, content: document.documentElement.scrollWidth }));
    check(mobile.content <= mobile.viewport, '390px mobile viewport has no horizontal overflow');
    await page.locator('[data-input="pulse"]').click();
    check((await state()).states.count === 3, 'generated mobile input buttons remain usable');
    await page.screenshot({ path: shots[2], fullPage: true });
    const comparison = await page.request.get('http://127.0.0.1:8766/baseline.html');
    const equivalent = await page.request.get('http://127.0.0.1:8766/examples/charge.pure');
    check(comparison.ok() && equivalent.ok() && await page.locator('a[href="./baseline.html"]').count() === 1, 'ordinary-JavaScript comparison and equivalent Pure example are linked and served');

    await page.route('**/core.wasm', route => route.abort());
    await page.reload();
    await page.waitForFunction(() => document.getElementById('ready-status').textContent.includes('실패'));
    check(await page.locator('#apply').isDisabled() && await page.locator('#play').isDisabled() && await page.locator('#step').isDisabled() && /실행기를 열지/.test(await page.locator('#diagnostic').textContent()), 'missing Wasm shows an actionable load failure and disables execution controls');
    await page.unroute('**/core.wasm');
    await page.setViewportSize({ width: 1440, height: 1000 });
    await page.reload();
    await page.waitForFunction(() => window.pureWorkbench?.snapshot().ready);
    check((await state()).source === '' && (await state()).views.length === 0, 'fresh load always returns to a blank authoring environment');
    check(errors.length === 0, 'no uncaught page errors during the browser scenarios');
    const evidence = { recordedUtc: new Date().toISOString(), browser: await page.evaluate(() => navigator.userAgent),
      checks, authoredSource: authorSource, authoredSnapshot: lastAuthored, mobile, downloaded, errors, screenshots: shots };
    // CLI can yield early when native dialogs/download events occur. Retain the
    // observation report so a follow-up CLI call can retrieve the complete run.
    await page.evaluate(value => { window.__pureWorkbenchQAEvidence = value; }, evidence);
    return evidence;
  } catch (error) {
    const evidence = { failed: true, error: error.message, stack: error.stack, checks, errors };
    await page.evaluate(value => { window.__pureWorkbenchQAEvidence = value; }, evidence);
    return evidence;
  } finally {
    page.off('pageerror', onError);
    page.off('dialog', acceptDialog);
  }
}
