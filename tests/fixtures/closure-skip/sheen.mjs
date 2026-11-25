// A game's own shading part for tests/programs-closure-skip.mjs: a cool rim on the material's emissive, as a game
// would put on a character's plumage or cloth (nothing on the caster).
export const name = 'sheen';
export async function apply({ TSL, mat, kind }) {
  if (kind === 'depth') return;
  const { vec3, float, smoothstep, max, dot, normalView, positionViewDirection, materialEmissive } = TSL;
  const rim = smoothstep(0.6, 1.0, float(1).sub(max(dot(normalView, positionViewDirection), 0)));
  mat.emissiveNode = materialEmissive.add(vec3(0.04, 0.05, 0.09).mul(rim));
}
