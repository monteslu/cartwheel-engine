// A cart's manifest `controls` (wasmcart SPEC.md: a presentation hint, the parts of the standard pad a host
// drawing on-screen controls should show), worked out from the project's input map (project.cwproj).
//
// In a cart the host draws every on-screen pad control (cartwheel/input.h, "touch controls"), so the
// manifest is how a game says which ones it needs:
//   - every `pad:` and `axis:` binding of every `input.<action> = ...` line names a part of the pad;
//   - the shell's own actions (cartwheel/shell.h: accept, back, menu navigation, pause) add the d-pad,
//     a, b and start;
//   - a project key `controls = a, b, start` replaces all of that, and `controls = none` gives an empty
//     list (touch is the game's own: the host draws no pad);
//   - a project without an input map gets no field (the host picks its default).
//
//   projectControls(text) -> string[] (possibly empty), or null for "leave the field out"
//   parseProject(text)    -> [[key, value], ...] in file order

/** The SPEC's tokens, in the SPEC's order (the order a derived list is written in). */
export const SPEC_CONTROLS = ['dpad', 'a', 'b', 'x', 'y', 'l', 'r', 'start', 'select',
  'left_stick', 'right_stick', 'left_trigger', 'right_trigger', 'l3', 'r3'];

/** the engine's pad button names (cartwheel/input.h, src/input/bindings.c) -> SPEC tokens; guide is the
 *  host's own button and draws nothing */
const PAD_TOKEN = {
  south: 'a', east: 'b', west: 'x', north: 'y', back: 'select', start: 'start',
  lstick: 'l3', rstick: 'r3', lshoulder: 'l', rshoulder: 'r',
  up: 'dpad', down: 'dpad', left: 'dpad', right: 'dpad',
};
/** the engine's axis names -> SPEC tokens */
const AXIS_TOKEN = { lx: 'left_stick', ly: 'left_stick', rx: 'right_stick', ry: 'right_stick', lt: 'left_trigger', rt: 'right_trigger' };

/** what the shell's menus and pause read (src/shell/shell.c, src/ui/input.c) */
export const SHELL_CONTROLS = ['dpad', 'a', 'b', 'start'];

/** key = value lines; '#' comments and blank lines skipped; surrounding quotes and spaces stripped */
export function parseProject(text) {
  const out = [];
  for (const raw of String(text).split(/\r?\n/)) {
    const line = raw.trim();
    if (!line || line.startsWith('#')) continue;
    const eq = line.indexOf('=');
    if (eq <= 0) continue;
    let v = line.slice(eq + 1).trim();
    if (v.length >= 2 && v.startsWith('"') && v.endsWith('"')) v = v.slice(1, -1);
    out.push([line.slice(0, eq).trim(), v]);
  }
  return out;
}

/** the SPEC token one binding names, or null (a key, the mouse, a touch control, a typo) */
export function bindingToken(binding) {
  const b = binding.trim();
  let m = /^pad:([a-z]+)$/.exec(b);
  if (m) return PAD_TOKEN[m[1]] ?? null;
  m = /^axis:([a-z]+)[+-](@[0-9.]+)?$/.exec(b);
  if (m) return AXIS_TOKEN[m[1]] ?? null;
  return null;
}

const ordered = (set) => SPEC_CONTROLS.filter((t) => set.has(t));

/** the input map's actions in file order: [[action, [binding, ...]], ...] */
function inputMap(entries) {
  return entries.filter(([k]) => k.startsWith('input.') && k.length > 6)
    .map(([k, v]) => [k.slice(6), v.split(',').map((s) => s.trim()).filter(Boolean)]);
}

/** a project's override: `controls = a, b` (a list), `controls = none` ([]), or undefined when not set */
function override(entries) {
  const e = entries.filter(([k]) => k === 'controls').pop();
  if (!e) return undefined;
  const list = e[1].split(/[\s,]+/).filter(Boolean);
  if (list.length === 1 && list[0] === 'none') return [];
  return list;
}

export function projectControls(text) {
  const entries = parseProject(text);
  const o = override(entries);
  if (o !== undefined) return o;
  const map = inputMap(entries);
  if (!map.length) return null;
  const set = new Set(SHELL_CONTROLS);
  for (const [, binds] of map) for (const b of binds) {
    const t = bindingToken(b);
    if (t) set.add(t);
  }
  return ordered(set);
}

