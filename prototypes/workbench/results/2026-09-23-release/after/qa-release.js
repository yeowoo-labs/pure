// Playwright CLI run-code. Covers release routing and stale release coordinates.
async page => {
  const checks = [];
  const state = () => page.evaluate(() => window.pureWorkbench.snapshot());
  const check = (ok, label) => checks.push({label, passed:Boolean(ok)});
  const near = (a,b) => Math.abs(a-b)<0.1;
  const client = (x,y) => page.evaluate(({x,y}) => {
    const p = new DOMPoint(x,y).matrixTransform(document.querySelector('#scene').getScreenCTM());
    return {x:p.x,y:p.y};
  },{x,y});
  const move = async(x,y) => { const p=await client(x,y); await page.mouse.move(p.x,p.y); };
  const start = async() => {
    await page.locator('#reset').click(); await page.locator('#scene').scrollIntoViewIfNeeded();
    await move(90,170); await page.mouse.down(); await move(370,230);
  };
  await page.setViewportSize({width:1440,height:1000});
  await page.waitForFunction(() => window.pureWorkbench?.snapshot().ready);
  await page.locator('#example').selectOption('direct');
  await page.locator('#load-example').click(); await page.locator('#apply').click();
  await page.evaluate(() => document.querySelector('#scene').addEventListener('pointerdown',e=>{ window.__releaseID=e.pointerId; }));
  await start(); await page.mouse.up(); let s=await state();
  check(near(s.states.x,360)&&near(s.states.y,220)&&!s.held.length&&s.lastEvent.phase==='up','ordinary in-scene release retains the displayed position');
  await start(); await move(750,400); await page.mouse.up(); s=await state();
  check(near(s.states.x,564)&&near(s.states.y,276)&&!s.held.length&&s.lastEvent.phase==='up','ordinary release outside the scene retains the clamped position');
  for (const scenario of ['document-pointerup','child-nonbubbling-up','mouse-only-up','stale-scene-up']) {
    await start();
    const position=await client(90,170); // Deliberately stale press coordinates.
    await page.evaluate(({scenario,position})=>{
      const scene=document.querySelector('#scene'), tile=document.querySelector('[data-point-input="drag"]');
      const fields={pointerId:window.__releaseID,pointerType:'mouse',button:0,buttons:0,clientX:position.x,clientY:position.y,bubbles:scenario!=='child-nonbubbling-up'};
      if(scenario==='mouse-only-up') window.dispatchEvent(new MouseEvent('mouseup',fields));
      else (scenario==='document-pointerup'?document:scenario==='child-nonbubbling-up'?tile:scene).dispatchEvent(new PointerEvent('pointerup',fields));
      if(scene.hasPointerCapture(window.__releaseID)) scene.releasePointerCapture(window.__releaseID);
    },{scenario,position});
    await page.mouse.up(); s=await state();
    check(near(s.states.x,360)&&near(s.states.y,220)&&!s.held.length&&s.lastEvent.phase==='up',`${scenario}: release commits once without a coordinate jump or later cancel`);
    check(s.successfulEvents===3,`${scenario}: duplicate release/capture loss produces no extra authored event`);
  }
  await start();
  await page.evaluate(()=>document.querySelector('[data-point-input="drag"]').dispatchEvent(new PointerEvent('lostpointercapture',{pointerId:window.__releaseID,bubbles:true})));
  s=await state(); check(s.held.includes('drag')&&near(s.states.x,360),'descendant capture-loss notification cannot cancel the root owner');
  await page.mouse.up();
  await start();
  await page.evaluate(()=>document.querySelector('#scene').releasePointerCapture(window.__releaseID));
  await move(380,240); s=await state();
  check(!s.held.length&&s.states.x===80&&s.states.y===160&&s.lastEvent.phase==='cancel','actual root capture loss while held still cancels');
  await page.mouse.up();
  await start();
  await page.evaluate(()=>document.dispatchEvent(new PointerEvent('pointercancel',{pointerId:window.__releaseID,bubbles:true})));
  await page.mouse.up(); s=await state();
  check(!s.held.length&&s.states.x===80&&s.states.y===160&&s.lastEvent.phase==='cancel','cancellation outside the scene still rolls back and ignores the late up');
  await start();
  await page.evaluate(()=>window.dispatchEvent(new Event('blur')));
  await page.mouse.up(); s=await state();
  check(!s.held.length&&s.states.x===80&&s.states.y===160&&s.lastEvent.phase==='cancel','focus loss still cancels rather than committing');
  const result={checks,passed:checks.filter(c=>c.passed).length,failed:checks.filter(c=>!c.passed)};
  await page.evaluate(result=>{window.__pureReleaseEvidence=result;},result);
  return result;
}
