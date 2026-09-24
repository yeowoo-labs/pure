// Only gesture ownership and transient geometry live here. Layout belongs to CSS.
export function bindSplit({container, handle, readRatio, writeRatio, narrow, stats}) {
  const events=container.ownerDocument.defaultView, abort=new AbortController();
  const listen=(node,type,callback,options={})=>node.addEventListener(type,callback,{...options,signal:abort.signal});
  let drag=null, frame=0, dirty=true, rect=null, handleWidth=0;
  const invalidate=()=>{dirty=true;};
  const observer=new ResizeObserver(invalidate);observer.observe(container);observer.observe(handle);
  function measure() {
    rect=container.getBoundingClientRect();const h=handle.getBoundingClientRect();
    handleWidth=h.width;stats.geometryReads+=2;dirty=false;return h;
  }
  function flush() {
    if(frame){cancelAnimationFrame(frame);frame=0;}
    if(!drag || drag.x===null)return;
    if(dirty)measure();
    const width=rect.width-handleWidth;
    if(width>0)writeRatio(100*(drag.x-rect.left-handleWidth/2-drag.offset)/width,'pointer');
    drag.x=null;
  }
  function finish(cancel) {
    if(!drag)return;
    if(cancel){if(frame)cancelAnimationFrame(frame);frame=0;}else flush();
    const previous=drag;drag=null;
    if(handle.hasPointerCapture(previous.id))handle.releasePointerCapture(previous.id);
    handle.classList.remove('dragging');
    if(cancel)writeRatio(previous.before,'cancel');
  }
  listen(handle,'pointerdown',event=>{
    if(event.button!==0 || drag || narrow.matches)return;
    const h=measure();if(rect.width<=handleWidth)return;
    event.preventDefault();handle.focus();
    handle.setPointerCapture(event.pointerId);
    drag={id:event.pointerId,type:event.pointerType,before:readRatio(),offset:event.clientX-(h.left+h.width/2),x:null};
    handle.classList.add('dragging');
  });
  listen(handle,'pointermove',event=>{
    if(!drag || drag.id!==event.pointerId)return;
    event.preventDefault();drag.x=event.clientX;
    if(!frame)frame=requestAnimationFrame(()=>{frame=0;flush();});
  });
  listen(events,'pointerup',event=>{if(drag?.id===event.pointerId)finish(false);},{capture:true});
  listen(events,'pointercancel',event=>{if(drag?.id===event.pointerId)finish(true);},{capture:true});
  listen(events,'mouseup',event=>{if(event.button===0&&drag?.type==='mouse')finish(false);},{capture:true});
  listen(handle,'lostpointercapture',event=>{if(event.target===handle&&drag?.id===event.pointerId)finish(true);});
  listen(events,'blur',()=>finish(true));
  listen(container.ownerDocument,'visibilitychange',()=>{if(container.ownerDocument.hidden)finish(true);});
  listen(events,'resize',invalidate);listen(events,'scroll',invalidate,{capture:true,passive:true});
  listen(narrow,'change',()=>{invalidate();if(narrow.matches)finish(true);});
  listen(handle,'keydown',event=>{
    if(event.key==='Escape'){event.preventDefault();finish(true);return;}
    if(!['ArrowLeft','ArrowRight','Home','End'].includes(event.key))return;
    event.preventDefault();finish(true);
    writeRatio(event.key==='Home'?20:event.key==='End'?80:readRatio()+(event.key==='ArrowLeft'?-1:1)*(event.shiftKey?10:1),'keyboard');
  });
  return Object.freeze({cancel:()=>finish(true),isDragging:()=>drag!==null,
    dispose(){finish(true);observer.disconnect();abort.abort();}});
}
