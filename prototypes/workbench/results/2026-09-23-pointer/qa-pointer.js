// Playwright CLI run-code: real mouse / CDP touch input; no timing benchmark.
async (page) => {
  const checks = [], errors = [];
  const check = (condition, label) => { if (!condition) throw new Error(label); checks.push(label); };
  const close = (a, b) => Math.abs(a - b) < 0.1;
  const state = () => page.evaluate(() => window.pureWorkbench.snapshot());
  const client = (x, y) => page.evaluate(({x,y}) => {
    const p = new DOMPoint(x,y).matrixTransform(document.querySelector('#scene').getScreenCTM());
    return {x:p.x,y:p.y};
  }, {x,y});
  const move = async (x,y) => { const p = await client(x,y); await page.mouse.move(p.x,p.y); };
  const start = async (x,y) => { await move(x,y); await page.mouse.down(); };
  const reset = async () => { await page.locator('#reset').click(); await page.locator('#scene').scrollIntoViewIfNeeded(); };
  const apply = async source => { await page.locator('#source').fill(source); await page.locator('#apply').click(); await page.locator('#scene').scrollIntoViewIfNeeded(); };
  const onError = error => errors.push(error.message);
  const onDialog = dialog => dialog.accept();
  page.on('pageerror', onError); page.on('dialog', onDialog);
  let session;
  try {
    await page.setViewportSize({width:1440,height:1000});
    await page.reload();
    await page.waitForFunction(() => window.pureWorkbench?.snapshot().ready);
    await page.locator('#example').selectOption('direct');
    await page.locator('#load-example').click();
    const source = await page.locator('#source').inputValue();
    check((await state()).views.length === 0, 'loading the pointer example leaves the running program unchanged until Apply');
    await page.locator('#apply').click(); await page.locator('#scene').scrollIntoViewIfNeeded();
    check(await page.locator('[data-point-input="drag"]').count() === 1, 'authored pointer declaration binds the visible target');
    check(await page.locator('#input-controls button').count() === 2, 'keyboard alternatives remain available without a fake pointer button');
    await page.evaluate(() => {
      window.__pointerTile = document.querySelector('[data-point-input="drag"]');
      document.querySelector('#scene').addEventListener('pointerdown', e => { window.__pointerID = e.pointerId; });
    });
    await start(90,170);
    let s = await state();
    check(s.states.dragging === 1 && s.held.includes('drag'), 'real mouse down reaches the authored rule');
    await move(370,230); s = await state();
    check(close(s.states.x,360) && close(s.states.y,220), 'drag retains grab offset in scene coordinates');
    check(s.views.find(v => v.id === 'region').value === 1 && await page.locator('[data-view-id="region"]').textContent() === '오른쪽 영역: 1', 'linked output updates when the authored boundary is crossed');
    check(await page.evaluate(() => window.__pointerTile === document.querySelector('[data-point-input="drag"]')), 'drag keeps its bound DOM target');
    // Physical mouse outside the SVG still reaches the stable captured scene.
    await move(750,400); s = await state();
    check(close(s.states.x,564) && close(s.states.y,276), 'capture delivers movement outside the scene and authored clamp applies');
    await page.mouse.up(); s = await state();
    check(s.held.length === 0 && s.states.dragging === 0 && s.lastEvent.phase === 'up', 'release outside the scene ends the gesture once');
    await reset();
    await start(90,170); await move(380,240);
    await page.evaluate(() => window.dispatchEvent(new Event('blur')));
    s = await state();
    check(close(s.states.x,80) && close(s.states.y,160) && s.states.dragging === 0 && s.held.length === 0 && s.lastEvent.phase === 'cancel', 'focus loss invokes authored rollback and clears ownership');
    const ended = s.successfulEvents;
    await page.mouse.up();
    check((await state()).successfulEvents === ended, 'late mouse release after cancel does not dispatch a second terminal event');
    await start(90,170); await move(350,220);
    await page.evaluate(() => document.querySelector('#scene').releasePointerCapture(window.__pointerID));
    await move(360,230); s = await state();
    check(close(s.states.x,80) && s.held.length === 0 && s.lastEvent.phase === 'cancel', 'explicit capture loss cancels instead of committing a drop');
    await page.mouse.up();
    await start(90,170); await move(300,220);
    await page.evaluate(() => document.querySelector('#scene').dispatchEvent(new PointerEvent('pointercancel', {pointerId:window.__pointerID,bubbles:true})));
    check((await state()).held.length === 0 && close((await state()).states.x,80), 'browser cancellation routes to the authored cancel handler');
    await page.mouse.up();
    // A source edit, not a renderer option, changes the movement constraint.
    await apply(source.replaceAll('20, 564','20, 200'));
    await start(90,170); await move(500,220); await page.mouse.up(); s = await state();
    check(close(s.states.x,200), 'editing the authored rule changes the drag limit');
    await apply(source + '\non drag.move { x = 1 / 0 }\n'); await reset();
    await start(90,170); await move(300,220); s = await state();
    check(s.lastError === 4 && close(s.states.x,80) && close(s.states.y,160), 'later handler failure rolls back all earlier movement writes');
    await page.evaluate(() => window.dispatchEvent(new Event('blur'))); await page.mouse.up();
    s = await state();
    check(s.lastError === 4 && s.held.length === 0 && s.states.dragging === 0, 'cleanup releases a failed gesture while preserving its error');
    await apply(source); await reset();
    await page.screenshot({path:'output/playwright/pure-pointer-desktop.png',fullPage:true});

    await page.setViewportSize({width:390,height:844});
    await page.locator('#scene').scrollIntoViewIfNeeded();
    session = await page.context().newCDPSession(page);
    await session.send('Emulation.setTouchEmulationEnabled',{enabled:true,maxTouchPoints:2});
    const touch = async (type, points) => session.send('Input.dispatchTouchEvent',{type,touchPoints:points});
    const a = await client(90,170), b = await client(270,210);
    const scroll = await page.evaluate(() => window.scrollY);
    await touch('touchStart',[{id:1,...a}]);
    check((await state()).states.dragging === 1, 'touch contact starts the same authored interaction');
    await touch('touchMove',[{id:1,...b}]); s = await state();
    check(close(s.states.x,260) && close(s.states.y,200), 'mobile scaled SVG maps touch coordinates back to world coordinates');
    check(await page.evaluate(() => window.scrollY) === scroll, 'touch drag manipulates the target without scrolling the page');
    await touch('touchEnd',[]); s = await state();
    check(s.held.length === 0 && s.states.dragging === 0 && s.lastEvent.phase === 'up', 'touch release clears capture ownership');
    await page.screenshot({path:'output/playwright/pure-pointer-mobile.png',fullPage:true});
    // Independent contacts are not collapsed into one global drag state.
    const two = 'state x=0\nstate y=0\ninput a=pointer(one)\ninput b=pointer(two)\nbox one(20,100,100,80,"#c9f08b")\nbox two(300,100,100,80,"#c9f08b")\non a.move { x=event_x }\non b.move { y=event_y }';
    await apply(two);
    const p1 = {id:1,...await client(50,140)}, p2 = {id:2,...await client(330,140)};
    await touch('touchStart',[p1]); await touch('touchStart',[p1,p2]);
    check((await state()).held.length === 2, 'two contacts on different authored targets retain separate owners');
    await touch('touchMove',[{id:1,...await client(180,160)},{id:2,...await client(430,200)}]); s = await state();
    check(close(s.states.x,180) && close(s.states.y,200), 'independent touch payloads reach their own rules');
    await touch('touchCancel',[]);
    check((await state()).held.length === 0, 'touch cancellation releases both targets');
    // A second contact on the same target cannot steal the first one.
    await touch('touchStart',[p1]);
    await touch('touchStart',[p1,{id:2,...await client(70,150)}]);
    check((await state()).held.length === 1, 'second contact on an already owned target does not steal it');
    await touch('touchCancel',[]);
    check((await state()).held.length === 0, 'ignored second contact cannot strand a logical input');
    check(errors.length === 0,'no browser page errors');
    await session.send('Emulation.setTouchEmulationEnabled',{enabled:false});
    await session.detach(); session = null;
    await page.setViewportSize({width:1440,height:1000}); await apply(source); await reset();
    await page.evaluate(result => { window.__purePointerEvidence = result; }, {checks,errors,kind:'functional input/output checks; no performance benchmark'});
    return {checks:checks.length,errors};
  } catch (error) {
    await page.evaluate(result => { window.__purePointerEvidence = result; }, {failed:String(error),checks,errors});
    throw error;
  } finally {
    if (session) { await session.send('Emulation.setTouchEmulationEnabled',{enabled:false}); await session.detach(); }
    page.off('pageerror',onError); page.off('dialog',onDialog);
  }
}
