// The browser's gamepads as wasmcart's four pads (tests/web-pads.mjs).
//
// A gamepad keeps the pad slot (the player) it got when it appeared until it
// goes away: the first free slot, so the first pad is pad 0 whatever index the
// browser gave it, and a pad that goes away leaves the others where they are.
// The pads used to be packed in the browser's order every frame, so when one
// went away every later player moved down a slot and took over another
// player's character.

// standard-mapping gamepad button index -> wasmcart pad bit
const PAD_BITS = { 0: 0, 1: 1, 2: 2, 3: 3, 4: 4, 5: 5, 9: 6, 8: 7, 12: 8, 13: 9, 14: 10, 15: 11, 10: 12, 11: 13, 16: 14 };
const axis = (v) => Math.round(Math.max(-1, Math.min(1, Math.abs(v) < 0.08 ? 0 : v)) * 32767);

/** readPads() for runFrame: four pads from getGamepads() (navigator.getGamepads in a page) */
export function padReader(getGamepads) {
  const slot = new Map(); // Gamepad.index -> pad slot 0-3
  return function readPads() {
    const out = [{ connected: false }, { connected: false }, { connected: false }, { connected: false }];
    const gps = [...(getGamepads() || [])].filter((gp) => gp && gp.connected);
    for (const idx of [...slot.keys()]) if (!gps.some((gp) => gp.index === idx)) slot.delete(idx);
    for (const gp of gps) {
      if (!slot.has(gp.index)) {
        const used = new Set(slot.values());
        const free = [0, 1, 2, 3].find((s) => !used.has(s));
        if (free === undefined) continue; // four players already
        slot.set(gp.index, free);
      }
      let buttons = 0;
      gp.buttons.forEach((b, i) => { if (b.pressed && i in PAD_BITS) buttons |= 1 << PAD_BITS[i]; });
      out[slot.get(gp.index)] = { connected: true, name: gp.id, buttons, leftX: axis(gp.axes[0] || 0), leftY: axis(gp.axes[1] || 0),
        rightX: axis(gp.axes[2] || 0), rightY: axis(gp.axes[3] || 0),
        leftTrigger: Math.round((gp.buttons[6]?.value || 0) * 32767), rightTrigger: Math.round((gp.buttons[7]?.value || 0) * 32767) };
    }
    return out;
  };
}
