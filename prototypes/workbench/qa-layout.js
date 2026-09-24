// Functional end-to-end persistence checks. Run with Playwright CLI run-code.
async page => {
  const checks=[],errors=[];
  const check=(ok,label)=>{if(!ok)throw new Error(label);checks.push(label);};
  const state=()=>page.evaluate(()=>window.pureWorkbench.snapshot());
  const onDialog=dialog=>dialog.accept(), onError=error=>errors.push(error.message);
  page.on('dialog',onDialog);page.on('pageerror',onError);
  const same=(a,b)=>JSON.stringify(a)===JSON.stringify(b);
  const client=(x,y)=>page.evaluate(({x,y})=>{const p=new DOMPoint(x,y).matrixTransform(document.querySelector('#scene').getScreenCTM());return{x:p.x,y:p.y};},{x,y});
  const move=async(x,y)=>{const p=await client(x,y);await page.mouse.move(p.x,p.y);};
  const drag=async(x,y,nx,ny)=>{await page.locator('#scene').scrollIntoViewIfNeeded();await move(x,y);await page.mouse.down();await move(nx,ny);await page.mouse.up();};
  const upload=async(text,name='layout.json')=>{
    await page.locator('#file-input').evaluate((input,{text,name})=>{
      const transfer=new DataTransfer();transfer.items.add(new File([text],name,{type:'application/json'}));
      input.files=transfer.files;input.dispatchEvent(new Event('change',{bubbles:true}));
    },{text,name});
  };
  try {
    await page.setViewportSize({width:1440,height:1000});
    await page.waitForFunction(()=>window.pureWorkbench?.snapshot().ready);
    check(await page.locator('#download-layout').isDisabled(),'placement save requires an applied definition');
    await page.locator('#example').selectOption('layout');await page.locator('#load-example').click();await page.locator('#apply').click();
    const source=await page.locator('#source').inputValue();
    await drag(90,130,160,180);await drag(290,170,360,220);await drag(460,230,530,270);
    await page.keyboard.press('ArrowLeft');
    let s=await state();
    check(Math.abs(s.states.x1-150)<0.1&&Math.abs(s.states.x2-350)<0.1&&Math.abs(s.states.x3-510)<0.1,'three independently authored targets move and selection directs keyboard adjustment');
    const expected={states:s.states,views:s.views};
    const draft=source+'\nstate unfinished =';
    await page.locator('#source').fill(draft);
    const pending=page.waitForEvent('download');await page.locator('#download-layout').click();
    const download=await pending;const saved='output/playwright/pure-layout-saved.json';await download.saveAs(saved);
    check(download.suggestedFilename()==='my-layout.pure.json','placement download is a separate document file');
    check((await state()).source===draft&&(await state()).appliedSource===source,'saving neither applies nor discards the unfinished draft');
    await page.locator('#reset').click();
    check((await state()).states.x1===80,'reset returns to authored initial positions');
    await page.locator('#file-input').setInputFiles(saved);
    await page.waitForFunction(()=>window.pureWorkbench.snapshot().diagnostic.includes('배치를 복원'));
    s=await state();
    check(same(s.states,expected.states)&&same(s.views,expected.views),'downloaded file restores all states and visible positions');
    check(s.source===draft&&s.appliedSource===source&&s.dirty,'the draft remains separate from the restored executable definition');
    check(!s.running&&!s.held.length&&s.tick===0&&s.successfulEvents===0,'restored work starts stopped with no replayed inputs or clock');
    await page.locator('#scene').focus();await page.keyboard.press('ArrowRight');
    check(Math.abs((await state()).states.x3-(expected.states.x3+10))<0.1,'restored selection and bindings remain editable');
    // Use a fresh page/runtime, not just preservation inside the original instance.
    const fresh=await page.context().newPage();await fresh.goto(page.url());
    await fresh.waitForFunction(()=>window.pureWorkbench?.snapshot().ready);
    await fresh.locator('#file-input').setInputFiles(saved);
    await fresh.waitForFunction(()=>window.pureWorkbench.snapshot().diagnostic.includes('배치를 복원'));
    const reopened=await fresh.evaluate(()=>window.pureWorkbench.snapshot());
    check(same(reopened.states,expected.states)&&same(reopened.views,expected.views)&&reopened.source===draft,'a fresh browser page reconstructs the saved work without old runtime state');
    await fresh.close();await page.bringToFront();

    const before=await state();
    await upload(JSON.stringify({format:'pure-workbench',version:2,source:'state changed=99',draft:'replacement',states:[]}));
    await page.waitForFunction(()=>window.pureWorkbench.snapshot().diagnostic.includes('상태 목록'));
    s=await state();
    check(same(s.states,before.states)&&s.source===before.source&&s.appliedSource===before.appliedSource&&s.revision===before.revision,'invalid saved state cannot partially replace code, draft or scene');
    // Delay File.text, then edit before it completes. Restoring must not overwrite the edit.
    await page.locator('#file-input').evaluate(input=>{
      const document={format:'pure-workbench',version:2,source:'state other=4',draft:'state other=4',states:[{name:'other',value:55}]};
      const transfer=new DataTransfer(),file=new File([JSON.stringify(document)],'delayed.json',{type:'application/json'});
      const text=JSON.stringify(document);file.text=()=>new Promise(resolve=>{window.__finishLayoutRead=()=>resolve(text);});
      transfer.items.add(file);input.files=transfer.files;input.dispatchEvent(new Event('change',{bubbles:true}));
    });
    await page.locator('#source').fill(draft+'\n# edit during file read');
    await page.evaluate(()=>window.__finishLayoutRead());
    await page.waitForFunction(()=>window.pureWorkbench.snapshot().diagnostic.includes('작업이 바뀌'));
    s=await state();check(s.source.endsWith('# edit during file read')&&same(s.states,before.states),'a delayed file read cannot overwrite a newer draft');
    // Repeat with an input event while file content is pending.
    await page.locator('#file-input').evaluate(input=>{
      const text=JSON.stringify({format:'pure-workbench',version:2,source:'state other=4',states:[{name:'other',value:55}]});
      const file=new File([text],'delayed.json',{type:'application/json'}),transfer=new DataTransfer();
      file.text=()=>new Promise(resolve=>{window.__finishLayoutRead=()=>resolve(text);});
      transfer.items.add(file);input.files=transfer.files;input.dispatchEvent(new Event('change',{bubbles:true}));
    });
    await page.locator('#scene').focus();await page.keyboard.press('ArrowRight');const changed=await state();
    await page.evaluate(()=>window.__finishLayoutRead());
    await page.waitForFunction(()=>window.pureWorkbench.snapshot().diagnostic.includes('작업이 바뀌'));
    check(same((await state()).states,changed.states),'a delayed restore cannot overwrite placement changes made while reading');
    // UI-only run state is deliberately not persisted.
    await page.locator('#play').click();await page.locator('#download-layout').click();
    check((await state()).diagnostic.includes('정지한 뒤'),'saving while time advances requests a stopped document');
    await page.locator('#play').click();
    // Refuse while a logical input remains held, with no lost source or partial save.
    await page.locator('#scene').focus();await page.keyboard.down('ArrowRight');
    await page.locator('#download-layout').click();
    check((await state()).diagnostic.includes('조작을 끝낸 뒤'),'saving with a held input is refused');
    await page.keyboard.up('ArrowRight');
    await page.locator('#file-input').setInputFiles(saved);
    await page.waitForFunction(()=>window.pureWorkbench.snapshot().diagnostic.includes('배치를 복원'));
    await page.screenshot({path:'output/playwright/pure-layout-desktop.png',fullPage:true});
    await page.setViewportSize({width:390,height:844});
    check(await page.evaluate(()=>document.documentElement.scrollWidth<=innerWidth),'extra persistence control fits a mobile viewport');
    await page.screenshot({path:'output/playwright/pure-layout-mobile.png',fullPage:true});
    check(errors.length===0,'no uncaught page errors');
    const result={checks,errors,saved,expected,source,draft};
    await page.evaluate(result=>{window.__pureLayoutEvidence=result;},result);
    return {checks:checks.length,errors};
  } catch(error) {
    await page.evaluate(result=>{window.__pureLayoutEvidence=result;},{failed:String(error),checks,errors});throw error;
  } finally {page.off('dialog',onDialog);page.off('pageerror',onError);}
}
