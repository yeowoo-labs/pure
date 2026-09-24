// This adapter owns browser capture only. Authored state changes stay in the VM.
export function bindSceneInput(scene, runtime, act) {
  const captures = new Map();
  const events = scene.ownerDocument.defaultView;
  const coordinates = event => {
    const matrix = scene.getScreenCTM();
    if (!matrix) return null;
    try {
      const p = new DOMPoint(event.clientX, event.clientY).matrixTransform(matrix.inverse());
      return Number.isFinite(p.x) && Number.isFinite(p.y) ? p : null;
    } catch { return null; }
  };
  const release = id => {
    if (scene.hasPointerCapture(id)) scene.releasePointerCapture(id);
  };
  function finish(event, phase, pointerId = event.pointerId) {
    const entry = captures.get(pointerId);
    if (!entry) return;
    event.preventDefault();
    // Capture loss/cancel may not carry a meaningful position. Use the last
    // delivered coordinates. Up must use its own final position.
    const p = phase === 'up' ? coordinates(event) : null;
    if (p) { entry.x = p.x; entry.y = p.y; }
    if (phase === 'up' && !p) phase = 'cancel';
    captures.delete(pointerId);
    release(pointerId);
    act(() => runtime().point(entry.name, phase, entry.x, entry.y, entry.source, entry.target));
  }
  scene.addEventListener('pointerdown', event => {
    if (event.button !== 0 || !runtime() || captures.has(event.pointerId)) return;
    const target = event.target.closest?.('[data-point-input]');
    if (!target || !scene.contains(target)) return;
    const name = target.getAttribute('data-point-input');
    const targetId = target.getAttribute('data-point-target') ?? undefined;
    // Each target has one coordinate owner. A second contact on it must not
    // steal the gesture; other authored targets can be manipulated independently.
    if ([...captures.values()].some(entry => entry.name === name && entry.target === targetId)) return;
    const p = coordinates(event);
    if (!p) return;
    event.preventDefault(); scene.focus();
    act(() => {
      scene.setPointerCapture(event.pointerId);
      const entry = {name, target:targetId, source:`scene:${event.pointerId}`, pointerType:event.pointerType, x:p.x, y:p.y};
      captures.set(event.pointerId, entry);
      try { runtime().point(name, 'down', p.x, p.y, entry.source, entry.target); }
      catch (error) { captures.delete(event.pointerId); release(event.pointerId); throw error; }
    });
  });
  scene.addEventListener('pointermove', event => {
    const entry = captures.get(event.pointerId);
    if (!entry) return;
    const p = coordinates(event);
    if (!p) { finish(event, 'cancel'); return; }
    event.preventDefault();
    entry.x = p.x; entry.y = p.y;
    act(() => runtime().point(entry.name, 'move', p.x, p.y, entry.source, entry.target));
  });
  // A host may deliver release outside the SVG even while it owns capture.
  // Observe termination before target handlers or capture cleanup can cancel it.
  events.addEventListener('pointerup', event => finish(event, 'up'), true);
  events.addEventListener('pointercancel', event => finish(event, 'cancel'), true);
  events.addEventListener('mouseup', event => {
    if (event.button !== 0) return;
    // Mouse-only fallback when the host delivers mouseup without pointerup.
    // A normal pointerup already removed the owner; touch compatibility events
    // must never finish a different contact.
    for (const [id, entry] of captures) {
      if (entry.pointerType === 'mouse') finish(event, 'up', id);
    }
  }, true);
  scene.addEventListener('lostpointercapture', event => {
    // Descendant capture transitions bubble here but do not release our owner.
    if (event.target === scene) finish(event, 'cancel');
  });
  return Object.freeze({
    releaseCaptures() {
      const ids = [...captures.keys()];
      captures.clear();
      for (const id of ids) release(id);
      // The caller subsequently invokes runtime.releaseAll(), which dispatches
      // each pending cancel once. Clearing first makes lostcapture harmless.
    },
  });
}
