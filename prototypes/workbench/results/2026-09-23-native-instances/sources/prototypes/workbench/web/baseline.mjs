// Handwritten JavaScript reference for examples/charge.pure.
// This fixed app has no source compiler, live apply, or state-name migration.
// Its scene and behavior must be changed by editing this JavaScript file.
export function createChargeModel() {
  const held = new Set();
  let holding = 0, charge = 0, y = 240, velocity = 0, tick = 0;
  const checkInput = name => {
    if (name !== 'jump') throw new Error(`Unknown input: ${name}`);
  };

  const model = {
    down(name = 'jump', source = 'external') {
      checkInput(name);
      const wasDown = held.size > 0;
      held.add(source);
      if (!wasDown && y >= 240) {
        holding = 1;
        charge = 0;
      }
      return 0;
    },
    up(name = 'jump', source = 'external') {
      checkInput(name);
      if (!held.delete(source) || held.size) return 0;
      if (holding) {
        velocity = -(160 + charge * 300);
        holding = 0;
        charge = 0;
      }
      return 0;
    },
    frame(count = 1) {
      if (!Number.isInteger(count) || count < 0 || count > 600) {
        throw new Error('Frame count must be an integer from 0 through 600.');
      }
      const dt = 1 / 60;
      for (let frame = 0; frame < count; frame++) {
        if (holding) charge = Math.min(Math.max(charge + dt, 0), 1);
        velocity = velocity + 600 * dt;
        y = y + velocity * dt;
        if (y >= 240) { y = 240; velocity = 0; }
        tick++;
      }
      return 0;
    },
    releaseAll() {
      for (const source of [...held]) model.up('jump', source);
      return 0;
    },
    reset() {
      held.clear();
      holding = 0; charge = 0; y = 240; velocity = 0; tick = 0;
    },
    snapshot() {
      return {
        states: { holding, charge, y, velocity }, tick,
        held: held.size ? ['jump'] : [],
        views: [
          { id: 'floor', type: 'box', x: 40, y: 261, width: 560, height: 4, color: '#356548' },
          { id: 'actor', type: 'circle', x: 180, y, radius: 20, color: '#c78638' },
          { id: 'meter', type: 'box', x: 80, y: 80, width: charge * 200, height: 12, color: '#c78638' },
          { id: 'amount', type: 'text', x: 80, y: 60, value: charge, prefix: 'Charge: ' },
        ],
      };
    },
  };
  return Object.freeze(model);
}
