// The text size check (cartwheel/render2d.h "text size"): a run's debug state against the floor.
// The engine records the smallest text each frame draws to the window, as a share of the canvas height
// (text.min_share), and the smallest since the run began (text.run_min_share), each with the string's
// first characters (text.min_where, text.run_min_where).
//
// Used by tests/gate.mjs (option textFloor) and tools/check-text.mjs.

/** smallest text anywhere: 18 px at 1080 lines (CW_TEXT_FLOOR_MIN) */
export const FLOOR_MIN = 18 / 1080;
/** body text (HUD values, menus, hints, captions): 26 px at 1080 lines (CW_TEXT_FLOOR_BODY) */
export const FLOOR_BODY = 26 / 1080;
// the engine stores the share as a 32-bit float: a size exactly at the floor must pass
const EPS = 1e-5;

/** a debug BYTES field (hex string or byte array) as text */
export function fieldText(v) {
  if (v === undefined || v === null) return '';
  const bytes = Array.isArray(v) ? v : typeof v === 'string' && /^[0-9a-f]*$/i.test(v) ? (v.match(/../g) || []).map((b) => parseInt(b, 16)) : null;
  if (!bytes) return String(v);
  const end = bytes.indexOf(0);
  return Buffer.from(end >= 0 ? bytes.slice(0, end) : bytes).toString('utf8');
}

export const pct = (s) => `${(s * 100).toFixed(2)}%`;
/** px at 1080 lines */
export const px1080 = (s) => `${(s * 1080).toFixed(1)} px at 1080 lines`;

/**
 * The run's smallest text against a floor: null when it passes (or the run drew no text), else the problem.
 * @param {Record<string, any>} state a run's debug state (run-cart --state)
 * @param {number} [floor]
 */
export function textFloorProblem(state, floor = FLOOR_MIN) {
  const s = state['text.run_min_share'];
  if (typeof s !== 'number') return 'no text.run_min_share in the state (an engine before the text size check, or a run that drew no 2D)';
  if (s <= 0 || s >= floor - EPS) return null;
  return `text drawn at ${pct(s)} of the canvas height (${px1080(s)}) is under the ${pct(floor)} floor: "${fieldText(state['text.run_min_where'])}"` +
    ' (cartwheel/render2d.h "text size": make it larger, or mark art text fixed_size)';
}
