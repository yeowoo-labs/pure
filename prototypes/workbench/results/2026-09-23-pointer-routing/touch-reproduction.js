async page => {
  page.on('dialog',d=>d.accept());
  await page.reload();await page.waitForFunction(()=>window.pureWorkbench?.snapshot().ready);
  await page.locator('#example').selectOption('routed-layout');await page.locator('#load-example').click();await page.locator('#apply').click();await page.locator('#reset').click();
  await page.locator('#scene').scrollIntoViewIfNeeded();
  await page.evaluate(()=>{window.pointerLog=[];for(const type of ['pointerdown','pointerup','pointercancel','pointermove'])window.addEventListener(type,e=>window.pointerLog.push({type,id:e.pointerId,x:e.clientX,y:e.clientY}),true);});
  const session=await page.context().newCDPSession(page);
  const xy=(x,y)=>page.evaluate(({x,y})=>{const p=new DOMPoint(x,y).matrixTransform(document.querySelector('#scene').getScreenCTM());return{x:p.x,y:p.y}},{x,y});
  const results=[];const snap=async label=>results.push({label,...await page.evaluate(()=>{const s=window.pureWorkbench.snapshot();return{log:window.pointerLog.slice(),held:s.held,states:s.states};})});
  await session.send('Emulation.setTouchEmulationEnabled',{enabled:true,maxTouchPoints:2});
  const p1={id:1,...await xy(90,130)},p2={id:2,...await xy(290,170)};
  await session.send('Input.dispatchTouchEvent',{type:'touchStart',touchPoints:[p1]});
  await session.send('Input.dispatchTouchEvent',{type:'touchStart',touchPoints:[p1,p2]});await snap('start');
  const m1={id:1,...await xy(160,180)},m2={id:2,...await xy(360,220)};
  await session.send('Input.dispatchTouchEvent',{type:'touchMove',touchPoints:[m1,m2]});await snap('move');
  await session.send('Input.dispatchTouchEvent',{type:'touchEnd',touchPoints:[m1]});await snap('remaining blue');
  await session.send('Input.dispatchTouchEvent',{type:'touchMove',touchPoints:[{id:2,...await xy(400,250)}]});await page.waitForFunction(()=>Math.abs(window.pureWorkbench.snapshot().states['blue.x']-390)<0.1);await snap('move remaining blue');
  await session.send('Input.dispatchTouchEvent',{type:'touchCancel',touchPoints:[]});
  await session.send('Emulation.setTouchEmulationEnabled',{enabled:false});await session.detach();
  await page.evaluate(e=>window.touchDebug=e,results);return results;
}
