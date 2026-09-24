import {createRuntime} from './runtime.mjs';
import {compile} from './compiler.mjs';
import {bindSplit} from './split-input.mjs';
const $=id=>document.getElementById(id), text=$('document-text'), reading=$('reading-text');
const MAX_BYTES=2*1024*1024, stats={geometryReads:0,previewWrites:0,ratioWrites:0};
let runtime=null, splitter=null, previewFrame=0, composing=false, generation=0, request=0, dirty=false;
const message=(id,value)=>{$(id).textContent=value;};
const narrow=matchMedia('(max-width:720px)');
const sample=`작업 노트 — 인터페이스의 경계

1. 이 문서에서 해볼 일

창의 폭을 줄여보세요. 읽기 화면의 글자는 새 폭에 맞춰 줄바꿈됩니다. 화면이 좁아지면 작성과 읽기가 위아래로 놓입니다. 문서 자체를 다른 형식으로 바꿀 필요는 없습니다.

넓은 화면에서는 두 영역 사이의 경계를 잡아 움직일 수 있습니다. 경계에 키보드 초점을 두고 방향키를 눌러도 됩니다. 손을 놓으면 현재 비율을 유지하고, 움직이는 도중 Esc를 누르면 시작한 비율로 돌아갑니다.

2. 작성과 읽기

왼쪽은 브라우저의 텍스트 입력입니다. 이 문장을 고치고, 몇 줄을 더 적어보세요. 한글을 조합하고 복사하거나 붙여넣고, 실행 취소를 해볼 수 있습니다. 읽기 화면에서는 같은 글을 다른 폭으로 살펴봅니다.

양쪽의 스크롤은 독립적입니다. 읽던 곳을 옮긴다고 작성 커서가 움직이지 않습니다. 분할 비율을 바꿀 때도 글자나 커서, 문서를 새로 만들지 않습니다.

3. 연결을 유지하기

일반 UTF-8 텍스트 파일을 열 수 있습니다. 작성한 결과도 .txt로 내려받습니다. 다른 편집기에서 그대로 읽을 수 있는 경로를 유지하는 것이 이 작업의 조건입니다.

아래 문자열은 코드가 아니라 문서의 일부입니다.
<div>이 글은 화면의 HTML 요소로 해석되지 않습니다.</div>

4. 작업 메모

여기에 실제 프로젝트의 메모를 적어도 됩니다.

하려던 일:

실제로 일어난 일:

기대했던 일:

다음 수정:

5. 남겨둘 질문

입력이 바뀌었을 때 꼭 다시 계산해야 하는 것은 무엇일까요? 문서가 바뀌지 않았는데 문서를 다시 전달할 이유가 있을까요? 화면의 폭과 문서의 의미를 같은 상태로 보관할 필요가 있을까요?

이 화면이 답하는 범위는 작습니다. 실제 브라우저의 편집과 배치를 사용하면서, 조작 규칙을 어디에 둘 수 있는지를 확인합니다.
`;
function publishText() {
  previewFrame=0;
  if(composing)return;
  reading.textContent=text.value;stats.previewWrites++;
  $('reading-empty').hidden=text.value.length!==0;
}
function scheduleText(){if(!previewFrame)previewFrame=requestAnimationFrame(publishText);}
text.addEventListener('compositionstart',()=>{composing=true;});
text.addEventListener('compositionend',()=>{composing=false;scheduleText();});
text.addEventListener('input',()=>{generation++;dirty=true;scheduleText();message('document-status','편집 중 · 저장하지 않은 변경이 있습니다.');});
function replaceText(value,name) {
  text.value=value;text.scrollTop=0;$('reading-scroll').scrollTop=0;
  if(name)$('filename').value=name;
  generation++;dirty=false;scheduleText();
}
$('sample').addEventListener('click',()=>{
  if(dirty&&!confirm('편집한 내용을 예제 문서로 바꿀까요?'))return;
  request++;replaceText(sample,'interface-notes.txt');dirty=true;message('document-status','예제 문서를 불러왔습니다. 자유롭게 고쳐보세요.');
});
$('open').addEventListener('click',()=>$('file-input').click());
$('file-input').addEventListener('change',async event=>{
  const file=event.target.files[0];event.target.value='';if(!file)return;
  if(dirty&&!confirm('저장하지 않은 내용을 파일의 내용으로 바꿀까요?'))return;
  const ticket=++request, version=generation;
  try {
    if(file.size>MAX_BYTES)throw new Error('2 MiB 이하의 텍스트 파일을 선택하세요.');
    const bytes=await file.arrayBuffer();
    if(ticket!==request||version!==generation)throw new Error('파일을 읽는 동안 문서가 바뀌어 열기를 취소했습니다.');
    if(bytes.byteLength>MAX_BYTES)throw new Error('파일 크기 한도를 넘었습니다.');
    const value=new TextDecoder('utf-8',{fatal:true}).decode(bytes);
    replaceText(value,file.name);message('document-status',`${file.name} 파일을 열었습니다.`);
  } catch(error){if(ticket===request)message('document-status',error instanceof TypeError?'UTF-8 텍스트로 읽을 수 없습니다. 현재 문서는 유지됩니다.':error.message);}
});
$('save').addEventListener('click',()=>{
  const bytes=new TextEncoder().encode(text.value);
  if(bytes.byteLength>MAX_BYTES){message('document-status','저장은 UTF-8 기준 2 MiB까지 지원합니다. 문서는 그대로 유지됩니다.');return;}
  const name=$('filename').value.trim()||'notes.txt';
  const url=URL.createObjectURL(new Blob([bytes],{type:'text/plain;charset=utf-8'}));
  const link=document.createElement('a');link.href=url;link.download=name.endsWith('.txt')?name:`${name}.txt`;
  link.click();setTimeout(()=>URL.revokeObjectURL(url),1000);dirty=false;message('document-status','텍스트 파일을 내려받았습니다.');
});
window.addEventListener('beforeunload',event=>{if(dirty){event.preventDefault();event.returnValue='';}});
const output={
  begin(){},view(){},value(){},end(){},
  state(index,value){
    if(runtime.currentProgram.states[index].name!=='ratio')return;
    // Invalid authored output never produces a broken grid or invalid ARIA value.
    if(value<20||value>80)throw new Error('이 화면의 분할 비율은 20~80% 범위여야 합니다. 규칙을 수정하세요.');
    const ratio=String(value);
    if($('divider').getAttribute('aria-valuenow')===ratio)return;
    $('divider').setAttribute('aria-valuenow',ratio);
    $('panes').style.setProperty('--split',String(value/100));
    $('split-range').value=ratio;$('split-output').value=`${Math.round(value)}%`;stats.ratioWrites++;
  },
};
function publish(){const result=runtime.present(output);if(result.lastError)throw new Error(result.errorMessage);}
function writeRatio(value,source) {
  if(!runtime)return;
  try {runtime.change('split',value,source);publish();message('policy-status','적용된 규칙으로 분할 비율을 조절합니다.');}
  catch(error){$('split-range').value=$('divider').getAttribute('aria-valuenow');message('policy-status',error.message);}
}
function applyPolicy() {
  if(splitter?.isDragging())throw new Error('경계를 놓거나 취소한 뒤 규칙을 적용하세요.');
  const source=$('policy-source').value, p=compile(source), ratio=p.states.find(state=>state.name==='ratio');
  if(!ratio||ratio.initial<20||ratio.initial>80||!p.inputs.some(input=>input.name==='split'&&input.kind==='value')||!p.inputs.some(input=>input.name==='reset'&&input.key==='KeyR')||p.views.length)
    throw new Error('ratio 상태(초깃값 20~80), split 숫자 입력, reset = KeyR 입력을 유지하세요. 이 화면은 HTML로 표시합니다.');
  runtime.apply(source);publish();message('policy-status','규칙을 적용했습니다. 문서와 편집 커서는 유지됩니다.');
}
$('apply-policy').addEventListener('click',()=>{try{applyPolicy();}catch(error){message('policy-status',error.message);}});
$('split-range').addEventListener('input',event=>{splitter?.cancel();writeRatio(event.target.valueAsNumber,'range');});
$('reset-split').addEventListener('click',()=>{
  splitter?.cancel();if(!runtime)return;
  try{runtime.down('reset','control');runtime.up('reset','control');publish();message('policy-status','분할 비율을 초기화했습니다.');}catch(error){message('policy-status',error.message);}
});
try {
  const [instance,response]=await Promise.all([createRuntime(),fetch('./examples/document.pure',{cache:'no-store'})]);
  if(!response.ok)throw new Error('분할 규칙을 불러올 수 없습니다.');
  runtime=instance;$('policy-source').value=await response.text();applyPolicy();
  splitter=bindSplit({container:$('panes'),handle:$('divider'),narrow,stats,
    readRatio:()=>Number($('divider').getAttribute('aria-valuenow')),writeRatio});
  for(const id of ['split-range','reset-split','apply-policy'])$(id).disabled=false;
  $('divider').setAttribute('aria-disabled','false');
} catch(error){message('policy-status',`분할 실행기를 열지 못했습니다: ${error.message} 문서 작성과 파일 저장은 계속 사용할 수 있습니다.`);}
window.pureDocument=Object.freeze({snapshot:()=>({ready:Boolean(splitter),stats:{...stats},
  runtime:runtime?.snapshot()??null,dragging:splitter?.isDragging()??false,composing,dirty,generation})});
