"use strict";

(() => {
  const $ = (id) => document.getElementById(id);
  const field = Object.freeze({ tick: 0, y: 1, vy: 2, distance: 3, obstacleX: 4, passed: 5, hits: 6, jumps: 7, running: 8, revision: 9, reason: 10, source: 11, inputs: 12, accepted: 13, mode: 14, impulse: 15, enabled: 16, grounded: 17, obstacleHeight: 18 });
  const reasonText = ["아직 입력 없음", "점프를 받아들였어요.", "점프 입력이 꺼져 있어요.", "땅에 닿아야 다시 뛸 수 있어요.", "공중에서 쓸 점프를 모두 썼어요.", "알 수 없는 입력이에요."];
  const sourceText = ["키보드", "점프 버튼", "CLI"];
  const frameMs = 1000 / 60;
  let core = null;
  let ready = false;
  let applied = null;
  let lastTime = null;
  let accumulator = 0;
  let droppedTicks = 0;
  let renderCount = 0;
  let historySignature = "";
  let notice = "";
  const nodes = [...document.querySelectorAll("[data-node]")];

  function text(id, value) {
    const element = $(id);
    const next = String(value);
    if (element.textContent !== next) element.textContent = next;
  }

  function get(name) { return core.get(field[name]); }
  function counter(name) { return get(name) >>> 0; }
  function say(message) { text("live-status", message); }
  function formatBytes(value) { return `${(value >>> 0).toLocaleString("ko-KR")} B`; }
  function draft() {
    return { mode: Number(document.querySelector('input[name="mode"]:checked').value), impulse: Number($("impulse").value), enabled: $("enabled").checked ? 1 : 0 };
  }
  function isDirty() {
    if (!applied) return false;
    const next = draft();
    return next.mode !== applied.mode || next.impulse !== applied.impulse || next.enabled !== applied.enabled;
  }
  function draftChanged() {
    text("impulse-output", $("impulse").value);
    const dirty = isDirty();
    text("draft-status", dirty ? "아직 적용하지 않은 변경" : "현재 규칙과 같음");
    $("draft-status").parentElement.classList.toggle("dirty", dirty);
    $("apply-button").disabled = !ready || !dirty;
    if (dirty) text("apply-feedback", "아직 이전 규칙으로 움직입니다. 적용하면 위치와 시간은 그대로 이어집니다.");
    else if (ready) text("apply-feedback", "바꾼 뒤 적용을 눌러주세요. 위치와 진행 시간은 이어집니다.");
  }
  function syncApplied() {
    applied = { mode: get("mode"), impulse: get("impulse"), enabled: get("enabled") };
    document.querySelector(`input[name="mode"][value="${applied.mode}"]`).checked = true;
    $("impulse").value = String(applied.impulse);
    $("enabled").checked = Boolean(applied.enabled);
    draftChanged();
  }

  function renderHistory() {
    const entries = [];
    for (let i = 0, count = Math.min(8, core.history_count()); i < count; i++) {
      entries.push([0, 1, 2, 3].map((part) => core.history_get(i, part) >>> 0));
    }
    const signature = JSON.stringify(entries);
    if (signature === historySignature) return;
    historySignature = signature;
    const list = $("history-list");
    list.replaceChildren();
    if (entries.length === 0) {
      const empty = document.createElement("li");
      empty.className = "empty-history";
      empty.textContent = "점프하거나 규칙을 적용하면 이곳에 기록됩니다.";
      list.append(empty);
      return;
    }
    for (const [tick, kind, detail, revision] of entries) {
      const row = document.createElement("li");
      const time = document.createElement("span");
      time.className = "history-time";
      time.textContent = `${tick}t`;
      const label = document.createElement("span");
      label.className = "history-label";
      const descriptions = { 1: "점프 허용", 2: reasonText[detail] || "점프 거절", 3: "규칙 적용", 4: "땅에 착지", 5: "장애물에 닿음", 6: "장애물을 지남" };
      label.textContent = `${descriptions[kind] || "상태 변경"} · v${revision}`;
      row.append(time, label);
      list.append(row);
    }
  }

  function renderDecision() {
    const reason = get("reason");
    const result = $("decision-result");
    result.classList.toggle("accepted", reason === 1);
    result.classList.toggle("rejected", reason > 1);
    text("decision-result", reason === 0 ? "첫 점프를 기다리고 있어요." : reasonText[reason] || "입력을 확인해주세요.");
    if (reason === 0) {
      text("decision-explanation", "한 번 누르면 입력이 규칙을 통과하는 과정을 볼 수 있습니다.");
      text("decision-meta", "아직 입력 없음");
      $("stale-decision").hidden = true;
    } else {
      const preY = core.decision_meta(2);
      const preJumps = core.decision_meta(3);
      const revision = core.decision_meta(1) >>> 0;
      text("decision-explanation", `누른 순간: 높이 ${(preY / 100).toFixed(1)} px · 사용한 점프 ${preJumps}회. ${get("running") ? "이후에도 세계는 움직입니다." : "멈춘 상태에서 판단을 살펴보고 있어요."}`);
      text("decision-meta", `${sourceText[get("source")] || "알 수 없는 입력"} · ${core.decision_meta(0) >>> 0} tick · v${revision}`);
      $("stale-decision").hidden = revision === counter("revision");
    }
    for (const node of nodes) {
      const value = core.node_value(Number(node.dataset.node));
      if (value < 0) delete node.dataset.result;
      else node.dataset.result = String(Boolean(value));
      const output = node.querySelector(".node-value");
      const next = value < 0 ? "대기" : value ? "예" : "아니요";
      if (output.textContent !== next) output.textContent = next;
    }
  }

  function render() {
    if (!ready) return;
    renderCount++;
    const y = get("y") / 100;
    const velocity = get("vy") / 100;
    const tick = counter("tick");
    const running = Boolean(get("running"));
    const obstacleHeight = get("obstacleHeight");
    const cameraY = Math.min(0, 180 - y);
    const viewWidth = $("stage").clientWidth < 500 ? 500 : 960;
    $("world").setAttribute("viewBox", `0 ${cameraY} ${viewWidth} 350`);
    $("dino").setAttribute("transform", `translate(120 ${230 - y})`);
    $("dino-shadow").setAttribute("rx", String(Math.max(10, 27 - y / 12)));
    $("dino-shadow").setAttribute("opacity", String(Math.max(0.05, 0.15 - y / 1600)));
    $("obstacle").setAttribute("transform", `translate(${get("obstacleX")} ${278 - obstacleHeight})`);
    $("obstacle-body").setAttribute("height", String(obstacleHeight));
    $("obstacle-mark").setAttribute("d", `M11 10v${Math.max(0, obstacleHeight - 20)}m10-${Math.max(0, obstacleHeight - 27)}v${Math.max(0, obstacleHeight - 27)}`);
    text("running-badge", running ? "실행 중" : "멈춰 있음");
    $("running-badge").classList.toggle("running", running);
    text("run-button", running ? "일시정지" : "실행");
    $("run-button").setAttribute("aria-pressed", String(running));
    $("step-button").disabled = running;
    text("stage-hint", cameraY < 0 ? "높이에 맞춰 시점이 공룡을 따라갑니다." : running ? "Space 또는 점프 버튼 · 멈춰서 규칙을 바꿔도 좋아요." : "일시정지 상태에서도 규칙과 입력을 확인할 수 있어요.");
    text("state-height", `${y.toFixed(1)} px`);
    text("state-velocity", `${velocity > 0 ? "+" : ""}${velocity.toFixed(1)} px/틱`);
    text("state-ground", get("grounded") ? "땅 위" : "공중");
    text("state-jumps", `${counter("jumps")}회`);
    text("state-distance", `${counter("distance").toLocaleString("ko-KR")} px`);
    text("state-time", `${(tick / 60).toFixed(2)}초`);
    text("state-tick", tick);
    text("state-passed", counter("passed"));
    text("state-hits", counter("hits"));
    text("revision-badge", `v${counter("revision")}`);
    text("applied-label", `적용 v${counter("revision")}`);
    text("runtime-note", notice || (droppedTicks > 0 ? `화면 지연으로 ${droppedTicks.toLocaleString("ko-KR")}틱을 생략했습니다. 진행 시간은 실제 시간보다 느릴 수 있어요.` : "실행하면 시간이 흐릅니다. 멈춘 뒤 한 프레임씩 살펴볼 수도 있어요."));
    text("scheduling-detail", `고정 60Hz · 한 화면 갱신당 최대 6틱 · 생략한 틱 ${droppedTicks.toLocaleString("ko-KR")}`);
    text("bytes-linear", core.memory ? formatBytes(core.memory.buffer.byteLength) : "메모리 내보내기 없음");
    renderDecision();
    renderHistory();
  }

  function jump(source) {
    if (!ready) return;
    core.input(source);
    render();
    say(reasonText[get("reason")] || "입력 처리 완료");
  }

  function setRunning(next) {
    core.set_running(next ? 1 : 0);
    accumulator = 0;
    lastTime = null;
    notice = "";
    render();
    say(next ? "실행했습니다." : "멈췄습니다. 규칙을 바꾸거나 한 프레임씩 살펴볼 수 있어요.");
  }

  function frame(now) {
    if (ready) {
      if (get("running") && !document.hidden) {
        if (lastTime !== null) {
          accumulator += Math.max(0, now - lastTime);
          const due = Math.floor(accumulator / frameMs);
          if (due > 0) {
            const ticks = Math.min(6, due);
            droppedTicks += due - ticks;
            accumulator -= due * frameMs;
            core.advance(ticks);
            render();
          }
        }
        lastTime = now;
      } else {
        lastTime = null;
        accumulator = 0;
      }
    }
    window.requestAnimationFrame(frame);
  }

  $("rules-form").addEventListener("input", draftChanged);
  $("rules-form").addEventListener("change", draftChanged);
  $("rules-form").addEventListener("submit", (event) => {
    event.preventDefault();
    if (!ready || !isDirty()) return;
    const next = draft();
    if (core.apply_rules(next.mode, next.impulse, next.enabled) !== 0) {
      text("apply-feedback", "적용하지 못했습니다. 점프 세기는 6~16이어야 합니다.");
      say("규칙을 적용하지 못했습니다.");
      return;
    }
    applied = next;
    draftChanged();
    const feedback = `v${counter("revision")} 적용 완료. 위치·속도·진행 시간은 그대로 이어집니다.`;
    text("apply-feedback", feedback);
    render();
    say(feedback);
  });
  $("run-button").addEventListener("click", () => { if (ready) setRunning(!get("running")); });
  $("step-button").addEventListener("click", () => {
    if (!ready || get("running")) return;
    core.step();
    render();
    say(`한 프레임 진행. 높이 ${(get("y") / 100).toFixed(1)} 픽셀.`);
  });
  $("jump-button").addEventListener("click", () => jump(1));
  $("reset-button").addEventListener("click", () => {
    if (!ready) return;
    core.reset();
    core.set_running(0);
    accumulator = 0;
    lastTime = null;
    droppedTicks = 0;
    notice = "처음 상태로 돌아왔습니다. 멈춘 상태에서 다시 시작할 수 있어요.";
    syncApplied();
    text("apply-feedback", "위치·진행 시간·규칙·기록을 초기 상태로 되돌렸습니다.");
    render();
    say("세계와 규칙을 초기 상태로 되돌렸습니다.");
  });
  document.addEventListener("keydown", (event) => {
    if (!ready || (event.code !== "Space" && event.key !== " ") || event.isComposing || event.altKey || event.ctrlKey || event.metaKey || event.shiftKey) return;
    const target = event.target;
    if (target instanceof Element && (target.isContentEditable || target.closest("input, textarea, select, button, a[href], summary, [contenteditable], [role=button]"))) return;
    event.preventDefault();
    if (event.repeat) return;
    jump(0);
  });
  document.addEventListener("visibilitychange", () => {
    if (!ready || !document.hidden) return;
    const wasRunning = Boolean(get("running"));
    core.set_running(0);
    lastTime = null;
    accumulator = 0;
    if (wasRunning) {
      notice = "다른 탭으로 이동해 자동으로 멈췄습니다. 실행을 누르면 이어집니다.";
      render();
      say(notice);
    }
  });
  window.addEventListener("resize", render);

  Object.defineProperty(window, "pureDemo", { value: Object.freeze({
    snapshot() {
      if (!ready) return Object.freeze({ ready: false });
      return Object.freeze({ ready: true, ...Object.fromEntries(Object.keys(field).map((name) => [name, get(name)])), draftDirty: isDirty(), droppedTicks, renderCount, wasmMemoryBytes: core.memory?.buffer.byteLength ?? null, logicalBytes: core.bytes(6), decisionTick: core.decision_meta(0), decisionRevision: core.decision_meta(1), decisionNodes: Object.freeze(nodes.map((node) => core.node_value(Number(node.dataset.node)))) });
    }
  }), writable: false, configurable: false });

  async function boot() {
    try {
      const response = await fetch("core.wasm", { cache: "no-store" });
      if (!response.ok) throw new Error(`core.wasm HTTP ${response.status}`);
      const { instance } = await WebAssembly.instantiate(await response.arrayBuffer(), {});
      core = instance.exports;
      for (const name of ["reset", "set_running", "advance", "step", "input", "apply_rules", "get", "node_value", "decision_meta", "history_count", "history_get", "bytes"]) {
        if (typeof core[name] !== "function") throw new Error(`필요한 코어 함수가 없습니다: ${name}`);
      }
      core.reset();
      core.set_running(0);
      ready = true;
      $("rule-controls").disabled = false;
      for (const id of ["run-button", "step-button", "jump-button", "reset-button"]) $(id).disabled = false;
      $("workspace").setAttribute("aria-busy", "false");
      $("load-message").hidden = true;
      syncApplied();
      ["rules", "state", "decision", "history", "engine", "graph", "total"].forEach((name, i) => text(`bytes-${name}`, formatBytes(core.bytes(i))));
      render();
      say("준비되었습니다. 멈춘 상태입니다. 점프 버튼을 눌러보세요.");
      window.requestAnimationFrame(frame);
    } catch (error) {
      ready = false;
      $("workspace").setAttribute("aria-busy", "false");
      $("load-message").hidden = true;
      $("load-error").hidden = false;
      text("running-badge", "실행기 없음");
      text("error-detail", error instanceof Error ? error.message : String(error));
      for (const control of document.querySelectorAll("button, #rule-controls")) control.disabled = true;
    }
  }
  boot();
})();
