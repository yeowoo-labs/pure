// Bind authored declarations to concrete DOM destinations once per apply.
// The DOM holds the published values; no parallel scene/value model is kept.
const NS = 'http://www.w3.org/2000/svg';
const number = value => Number.isInteger(value) ? String(value) : Number(value.toPrecision(7)).toString();
const attributes = { box: { x: 'x', y: 'y', width: 'width', height: 'height' },
  circle: { x: 'cx', y: 'cy', radius: 'r' }, text: { x: 'x', y: 'y' } };

function text(node, value) {
  if (node.textContent !== value) node.textContent = value;
}
function attribute(node, name, value) {
  const next = String(value);
  if (node.getAttribute(name) !== next) node.setAttribute(name, next);
}

export function createOutput(document) {
  const get = id => document.getElementById(id);
  const scene = get('scene-content'), states = get('state-values');
  const empty = get('empty-scene'), description = get('scene-description');
  let active = undefined, nodes = [], amounts = [];
  const costNames = ['Wasm 선형 메모리', '코어 예약 공간', '사용 중인 명령·연결',
    '현재 상태 값', '새 코드 준비 공간 (예약에 포함)', '평가 스택 용량',
    '마지막 이벤트 실행 명령', '적용된 소스 크기'];
  const costs = costNames.map(label => {
    const dt = document.createElement('dt'), dd = document.createElement('dd');
    dt.textContent = label;
    return { dt, dd };
  });
  get('cost-values').replaceChildren(...costs.flatMap(({dt, dd}) => [dt, dd]));

  return {
    begin(program) {
      if (program === active) return;
      active = program;
      const targets = new Map();
      for (const input of program?.inputs ?? []) {
        if (input.target) targets.set(input.target, {name:input.name});
        for (const route of input.routes ?? []) targets.set(route.target, {name:input.name,target:route.target});
      }
      scene.ownerSVGElement.classList.toggle('direct-input', targets.size > 0);
      nodes = (program?.views ?? []).map(view => {
        const node = document.createElementNS(NS, {box:'rect', circle:'circle', text:'text'}[view.type]);
        node.setAttribute('data-view-id', view.id);
        if (targets.has(view.id)) {
          const input = targets.get(view.id);
          node.setAttribute('data-point-input', input.name);
          if (input.target) node.setAttribute('data-point-target', input.target);
        }
        node.setAttribute('fill', view.color ?? '#e7ece6');
        if (view.type === 'text') {
          node.setAttribute('font-family', 'ui-monospace, monospace');
          node.setAttribute('font-size', '20');
        }
        return node;
      });
      scene.replaceChildren(...nodes);
      const holders = [];
      amounts = (program?.states ?? []).map(state => {
        const dl = document.createElement('dl'), dt = document.createElement('dt'), dd = document.createElement('dd');
        dl.className = 'state-value'; dt.textContent = state.name; dd.dataset.state = state.name;
        dl.append(dt, dd); holders.push(dl);
        return dd;
      });
      if (!holders.length) {
        const note = document.createElement('p');
        note.className = 'empty-note'; note.textContent = '선언한 상태의 값이 표시됩니다.';
        holders.push(note);
      }
      states.replaceChildren(...holders);
    },
    state(index, value) { text(amounts[index], number(value)); },
    view(index, valid) {
      const node = nodes[index];
      if (valid) { if (node.hasAttribute('display')) node.removeAttribute('display'); }
      else attribute(node, 'display', 'none');
    },
    value(index, name, value) {
      const view = active.views[index], node = nodes[index];
      if (name === 'value') text(node, `${view.prefix || ''}${number(value)}`);
      else attribute(node, attributes[view.type][name], value);
    },
    end(visible) {
      const hidden = visible !== 0;
      if (empty.hidden !== hidden) empty.hidden = hidden;
      const failed = !visible && nodes.length > 0;
      text(empty.querySelector('strong'), failed ? '장면을 표시할 수 없습니다' : '아직 아무것도 없습니다');
      text(empty.querySelector('span:last-child'), failed ? '현재 상태 아래의 표시 오류를 확인하세요.' : '도형이나 글자를 선언하고 코드를 적용하세요.');
      text(description, visible ? `사용자가 만든 장면. 도형과 글자 ${visible}개.` : failed ? '표시 오류로 장면을 표시할 수 없습니다.' : '사용자가 코드로 만든 장면. 현재 비어 있습니다.');
    },
    costs(status, metrics, droppedTicks) {
      const m = status.memory;
      const values = [m.linearBytes, m.reservedEngineBytes, m.activeIRBytes, m.activeStateBytes,
        m.stagingReservedBytes, m.evalStackBytes, status.lastOps, metrics?.sourceBytes];
      for (let i = 0; i < values.length; i++) {
        const value = values[i], {dt, dd} = costs[i], hidden = !Number.isFinite(value);
        if (dt.hidden !== hidden) dt.hidden = hidden;
        if (dd.hidden !== hidden) dd.hidden = hidden;
        if (!hidden) text(dd, i === 6 ? `${value}개` : `${value.toLocaleString('en-US')} B`);
      }
      text(get('dropped-ticks'), `시간 지연으로 건너뛴 프레임: ${droppedTicks}`);
    },
  };
}
