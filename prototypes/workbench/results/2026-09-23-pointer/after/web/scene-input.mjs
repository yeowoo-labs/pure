// This adapter owns browser capture only. Authored state changes stay in the VM.
export function bindSceneInput(scene, runtime, act) {
  const captures = new Map();
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
  function finish(event, phase) {
    const entry = captures.get(event.pointerId);
    if (!entry) return;
    event.preventDefault();
    // Capture loss/cancel may not carry a meaningful position. Use the last
    // delivered coordinates. Up must use its own final position.
    const p = phase === 'up' ? coordinates(event) : null;
    if (p) { entry.x = p.x; entry.y = p.y; }
    if (phase === 'up' && !p) phase = 'cancel';
    captures.delete(event.pointerId);
    release(event.pointerId);
    act(() => runtime().point(entry.name, phase, entry.x, entry.y, entry.source));
  }
  scene.addEventListener('pointerdown', event => {
    if (event.button !== 0 || !runtime() || captures.has(event.pointerId)) return;
    const target = event.target.closest?.('[data-point-input]');
    if (!target || !scene.contains(target)) return;
    const name = target.getAttribute('data-point-input');
    // A logical input has one coordinate owner. A second contact on it must not
    // steal the gesture; other authored targets can be manipulated independently.
    if ([...captures.values()].some(entry => entry.name === name)) return;
    const p = coordinates(event);
    if (!p) return;
    event.preventDefault(); scene.focus();
    act(() => {
      scene.setPointerCapture(event.pointerId);
      const entry = {name, source:`scene:${event.pointerId}`, x:p.x, y:p.y};
      captures.set(event.pointerId, entry);
      try { runtime().point(name, 'down', p.x, p.y, entry.source); }
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
    act(() => runtime().point(entry.name, 'move', p.x, p.y, entry.source));
  });
  scene.addEventListener('pointerup', event => finish(event, 'up'));
  scene.addEventListener('pointercancel', event => finish(event, 'cancel'));
  scene.addEventListener('lostpointercapture', event => finish(event, 'cancel'));
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
