// A game's own shading part for tests/programs-closure-skip.mjs that assumes the cascaded sun: its capture fails in any
// state without the csm part, as a part written for one lighting setup would. The closure pairs it with an environment
// without the sun; that state's capture fails, and the regeneration skips it and says why.
export const name = 'needsun';
export async function apply({ TSL, mat, kind, state }) {
  if (kind === 'depth') return;
  if (!/\+x(?:[a-z0-9]+_)*csm(?:_|$)/.test(state)) throw new Error(`needsun: ${state} has no cascaded sun (csm), which this part assumes`);
  const { vec3, materialEmissive } = TSL;
  mat.emissiveNode = materialEmissive.add(vec3(0.01, 0.01, 0.0));
}
