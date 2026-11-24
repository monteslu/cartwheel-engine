// The variant closure of a program table (tools/programs.mjs): the states an app can switch a surveyed material into
// without a new survey.
//
// A state is a material (its kind, its own features, its own shading parts) drawn in an environment (fog, shadows, the
// scene's environment map, the lighting parts the graphics preset puts on every lit material: cluster, clusteries, csm,
// plain, hfog, gi; and the light counts). A survey sees the pairs a run happened to draw. The app can draw any of its
// materials in any environment it reaches with another: a preset switched in play, a level only some materials appear
// in, a weather or a Backdrop's world. The closure pairs each surveyed material with every environment surveyed for its
// class of material, at two light vectors: the most lights of each kind seen with that environment, and the most seen
// in the class at all (content built on High, with its omni lights, drawn after a switch to Very Low, whose shadow and
// lighting setup no run saw with that many lights). three.c draws fewer lights with a program for more (the extra ones
// are inert) and picks the smallest program that covers a draw, so the first vector keeps the cheaper program for the
// usual case. Each near terrain material is paired with every terrain quality too.
//
// A class is the kind group (standard and physical share their environments; each other kind its own), whether the
// material blends (only blended materials take the height fog's part), and the pass a part makes (a depth caster,
// the G-buffer, the velocity buffer and the light probe projection are passes of their own). An environment that
// cannot apply to a material is left out: the scene's environment map for a material with an envMap of its own, and the
// engine's lighting parts for a material whose extension does not compose with them (water).
//
// Pure: closure(states, featureOrder) -> the states to add, sorted. Idempotent: the closure of a table with its closure
// adds nothing.

/** the environment's own features (the scene and the preset decide them, not the material) */
export const ENV_FEATURES = new Set(['fog', 'fog2', 'shadow', 'rdepth', 'senv']);
/** the shading parts the engine puts on every lit material by itself (src/render/lighting.c, fog.c, gi/) */
export const ENV_PARTS = new Set(['cluster', 'clusteries', 'csm', 'plain', 'hfog', 'gi']);
/** the near terrain's parts, one per quality (src/terrain/terrain_render.c near_ext): a preset switch swaps them */
export const TERRAIN_NEAR = ['terrain', 'terrainlow', 'terrainmin', 'terrainfx', 'terrainfxlow', 'terrainfxmin'];
/** parts that make a pass of their own: their states are a class apart */
const PASS_PARTS = new Set(['gibuf', 'giproj', 'velbuf']);
/** parts set as a plain extension (not cw_shading_part): the lighting parts never compose with them */
const ALONE_PARTS = new Set(['water', 'waterlow', 'velbuf']);
/** the engine's part order (src/render/shading.c rank_of) */
const SURFACE_PARTS = new Set([...TERRAIN_NEAR, 'terrainfar', 'water', 'waterlow']);
const rank = (p) => (SURFACE_PARTS.has(p) ? 0 : p === 'cluster' || p === 'clusteries' ? 1 : p === 'gi' || p === 'gibuf' ? 3 : 2);
export const partOrder = (a, b) => rank(a) - rank(b) || (a < b ? -1 : a > b ? 1 : 0);
const LIT = new Set(['lambert', 'phong', 'standard', 'physical', 'toon']);

/** @param {string} s a state name (three.c's syntax: kind+features+lDPSH+sDPS+ies<hex>+x<parts>) */
export function parseState(s) {
  const t = s.split('+');
  const st = { kind: t[0], feats: [], lights: null, cast: null, ies: '', parts: [] };
  for (const x of t.slice(1)) {
    if (/^l\d{4}$/.test(x)) st.lights = [...x.slice(1)].map(Number);
    else if (/^s\d{3}$/.test(x)) st.cast = [...x.slice(1)].map(Number);
    else if (/^ies[0-9a-f]+$/.test(x)) st.ies = x;
    else if (/^x[a-z0-9_]+$/.test(x)) st.parts = x.slice(1).split('_');
    else st.feats.push(x);
  }
  // the casters: every light of the vector when the state names none
  if (st.feats.includes('shadow') && !st.cast && st.lights) st.cast = st.lights.slice(0, 3);
  return st;
}

/** a state's name, as three.c writes it (renderer_gen.inc gen_state_name): features in the table's bit order */
export function stateName(st, order) {
  const at = (f) => { const i = order.indexOf(f); if (i < 0) throw new Error(`unknown feature ${f}`); return i; };
  let n = [st.kind, ...[...st.feats].sort((a, b) => at(a) - at(b))].join('+');
  if (st.lights) n += '+l' + st.lights.join('');
  if (st.feats.includes('shadow') && st.cast && st.lights && st.cast.some((c, i) => c !== st.lights[i])) n += '+s' + st.cast.join('');
  if (st.ies) n += '+' + st.ies;
  if (st.parts.length) n += '+x' + [...st.parts].sort(partOrder).join('_');
  return n;
}

const group = (kind) => (kind === 'standard' || kind === 'physical' ? 'pbr' : kind);
/** the class a material's environments are shared within */
function classOf(st, matFeats, matParts) {
  const pass = matParts.find((p) => PASS_PARTS.has(p)) || (st.kind === 'depth' ? 'depth' : '');
  const blend = matFeats.includes('trans') || matFeats.includes('transm') ? 'T' : 'O';
  return `${group(st.kind)}/${blend}/${pass}`;
}

/**
 * @param {string[]} states the table's states
 * @param {string[]} order three.c's feature names in bit order (T3_GEN_FEATURE_NAMES)
 * @returns {{ add: string[], materials: number, environments: number }}
 */
export function closure(states, order) {
  const have = new Set(states);
  const mats = new Map(); // material key -> { kind, feats, parts, cls }
  const envs = new Map(); // class -> Map(env key -> { feats, parts, cast, ies, lights })
  const most = new Map(); // class -> the most lights of each kind any of its states has
  for (const s of states) {
    const st = parseState(s);
    const matFeats = st.feats.filter((f) => !ENV_FEATURES.has(f)).sort();
    const matParts = st.parts.filter((p) => !ENV_PARTS.has(p)).sort();
    const envFeats = st.feats.filter((f) => ENV_FEATURES.has(f)).sort();
    const envParts = st.parts.filter((p) => ENV_PARTS.has(p)).sort();
    const cls = classOf(st, matFeats, matParts);
    const mk = [st.kind, ...matFeats, 'x', ...matParts].join('+');
    if (!mats.has(mk)) mats.set(mk, { kind: st.kind, feats: matFeats, parts: matParts, cls });
    const ek = [...envFeats, 'x', ...envParts, 'c', String(st.cast ?? ''), st.ies].join('+');
    const byEnv = envs.get(cls) ?? envs.set(cls, new Map()).get(cls);
    const e = byEnv.get(ek);
    if (!e) byEnv.set(ek, { feats: envFeats, parts: envParts, cast: st.cast, ies: st.ies, lights: st.lights ? [...st.lights] : null });
    else if (st.lights) e.lights = e.lights ? e.lights.map((v, i) => Math.max(v, st.lights[i])) : [...st.lights];
    if (st.lights) most.set(cls, most.has(cls) ? most.get(cls).map((v, i) => Math.max(v, st.lights[i])) : [...st.lights]);
  }
  const add = new Set();
  for (const m of mats.values()) {
    const terrains = m.parts.some((p) => TERRAIN_NEAR.includes(p)) ? TERRAIN_NEAR : [null];
    for (const tq of terrains) {
      const parts = tq ? [...m.parts.filter((p) => !TERRAIN_NEAR.includes(p)), tq] : m.parts;
      for (const e of (envs.get(m.cls) ?? new Map()).values()) {
        // what cannot apply to this material: the scene's environment over its own envMap, lighting parts on water,
        // lights and shadows on an unlit kind, the plain model on a kind that is not physically based
        if (e.feats.includes('senv') && (m.feats.includes('env') || m.feats.includes('cenv'))) continue;
        if (e.parts.length && parts.some((p) => ALONE_PARTS.has(p))) continue;
        if (!LIT.has(m.kind) && (e.lights || e.feats.includes('shadow'))) continue;
        if (e.parts.includes('plain') && group(m.kind) !== 'pbr') continue;
        if (LIT.has(m.kind) && !e.lights) continue;
        const vectors = [e.lights];
        if (e.lights && most.get(m.cls).some((v, i) => v !== e.lights[i])) vectors.push(most.get(m.cls));
        for (const lights of vectors) {
          const name = stateName({ kind: m.kind, feats: [...m.feats, ...e.feats], lights, cast: e.cast, ies: e.ies,
            parts: [...parts, ...e.parts] }, order);
          if (!have.has(name)) add.add(name);
        }
      }
    }
  }
  return { add: [...add].sort(), materials: mats.size, environments: [...envs.values()].reduce((a, m) => a + m.size, 0) };
}

/** three.c's feature names in bit order, from its generated header */
export function featureOrder(src) {
  const m = /#define T3_GEN_FEATURE_NAMES \{([^}]*)\}/.exec(src);
  if (!m) throw new Error('T3_GEN_FEATURE_NAMES not found in programs.h');
  return [...m[1].matchAll(/"([^"]+)"/g)].map((x) => x[1]);
}
