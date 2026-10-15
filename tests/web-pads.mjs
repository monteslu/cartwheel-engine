// The web export's gamepads (tools/cwexport/templates/web/pads.js): the
// browser's Gamepad API to wasmcart's four pads. Each gamepad keeps the slot
// (the player) it got when it appeared: the first free one, whatever index the
// browser gave it; one going away leaves the others in their slots; a fifth
// waits for a free slot. Before, the pads were packed in the browser's order
// every frame, so when pad 1 went away player 3's gamepad became player 2.
// CONTROL: the packed mapping (the old player.js) fails the hot-plug checks.
//   node tests/web-pads.mjs
import { join, resolve, dirname } from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const { padReader } = await import(pathToFileURL(join(ROOT, 'tools/cwexport/templates/web/pads.js')).href);

/** a fake Gamepad: index, the standard button pressed (0 = south ...), a left stick x */
const gp = (index, button = -1, lx = 0) => ({ index, id: `pad ${index}`, connected: true, axes: [lx, 0, 0, 0],
  buttons: Array.from({ length: 17 }, (_, i) => ({ pressed: i === button, value: i === button ? 1 : 0 })) });

// the old player.js: connected gamepads packed in the browser's order
const packed = (get) => () => {
  const out = [];
  for (const g of get()) if (g && g.connected && out.length < 4) out.push({ connected: true, buttons: g.buttons.reduce((m, b, i) => (b.pressed && i < 4 ? m | (1 << i) : m), 0) });
  return out;
};

function scenario(makeReader) {
  let pads = [];
  const read = makeReader(() => pads);
  const view = () => { const r = read(); return [0, 1, 2, 3].map((i) => (r[i] && r[i].connected ? r[i].buttons : '-')).join(' '); };
  const steps = [];
  // the browser lists a gamepad at index 1 with index 0 empty (a pad reconnected)
  pads = [null, gp(1, 0)];
  steps.push(['a lone gamepad at browser index 1 is pad 0', view(), '1 - - -']);
  pads = [gp(0, 1), gp(1, 0), gp(2, 2), gp(3, 3)];
  steps.push(['three more: the next free slots, in the order they appear', view(), '1 2 4 8']);
  pads = [gp(0, 1), null, gp(2, 2), gp(3, 3)];
  steps.push(['the first one goes away: the others stay put', view(), '- 2 4 8']);
  pads = [gp(0, 1), gp(4, 0), gp(2, 2), gp(3, 3), gp(5, 1)];
  steps.push(['two appear with one slot free: the first takes it, the other waits', view(), '1 2 4 8']);
  return steps;
}

let failed = 0;
for (const [name, got, want] of scenario(padReader)) {
  if (got === want) console.log(`ok   ${name}: ${got}`);
  else { failed++; console.log(`FAIL ${name}: ${got}, want ${want}`); }
}
const control = scenario(packed);
if (control.every(([, got, want]) => got === want)) { failed++; console.log('FAIL control: the packed mapping passes, the checks see nothing'); }
else console.log(`ok   control: the packed mapping fails (${control.filter(([, g, w]) => g !== w).map(([n]) => n).join('; ')})`);
process.exit(failed ? 1 : 0);
