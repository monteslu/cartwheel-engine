// The "folalpha" shading part (cartwheel/foliage.h smooth_alpha; composed as xfolalpha_foliage, xcluster_csm_folalpha_foliage_gi
// and so on): smooth edges for an alpha-tested material (state +atest). The capture's material takes r187's
// alphaToCoverage, so its program keeps the alpha it computes instead of writing 1, and its alpha test becomes r187's
// alpha to coverage one: alpha = smoothstep(alphaTest, alphaTest + fwidth(alpha), alpha). Then, by the frame's mode:
//   1 (the scene pass is multisampled): that alpha as it is: three.c turns alpha to coverage on for the material, so it
//     becomes the pixel's sample coverage
//   2 (single sampled): hashed alpha: the pixel is kept with probability clamp(alpha / (2 alphaTest), 0, 1) (a half at
//     the cutoff, all at twice it) against interleaved gradient noise, moved every frame under TAA; a kept pixel
//     writes an alpha of 1 (its opacity raised past r187's step, with a small slope so fwidth is never 0)
// Values (filled by src/render/foliage.c):
//   ext.folAlphaFrame  vec4 (frame group): unused, dither offset (IGN frame step, 0 without TAA), mode (1 or 2), unused
// Depth programs (shadow casters, depth prepass) keep the plain alpha test.
export const name = 'folalpha';
export const part = 'folalpha';

const LIT = new Set(['lambert', 'phong', 'standard', 'physical', 'toon']);

export async function apply(ctx, next) {
  const { TSL, THREE, mat, props, sentinel, kind, features } = ctx;
  if (kind === 'depth' || !LIT.has(kind) || !features.includes('atest')) return next ? next(ctx) : undefined;
  const { uniform, sharedUniformGroup, diffuseColor, materialOpacity, materialAlphaTest, screenCoordinate,
    interleavedGradientNoise, vec2, float, clamp, max, select } = TSL;
  const group = sharedUniformGroup('foliageAlphaGroup', 0, THREE.NodeUpdateType.RENDER);
  const s = [sentinel(), sentinel(), sentinel(), sentinel()];
  const frame = uniform(new THREE.Vector4(...s)).setName('folAlphaFrame');
  frame.setGroup(group);
  props['ext.folAlphaFrame'] = { kind: 'vec4', value: s };

  mat.alphaToCoverage = true;
  const op = mat.opacityNode ?? materialOpacity;
  // r187 sets diffuseColor.a = diffuseColor.a * opacityNode: read the alpha so far inside the factor
  const a = diffuseColor.a.mul(op);
  const p = clamp(a.div(max(materialAlphaTest.mul(2), 1e-4)), 0, 1);
  const noise = interleavedGradientNoise(screenCoordinate.xy.add(vec2(frame.y)));
  // kept: alpha 4 (past alphaTest + fwidth); dropped: about 0 (under the cutoff: discarded). The slope keeps fwidth off 0.
  const target = select(p.greaterThan(noise), float(4), float(0)).add(screenCoordinate.x.mul(1e-4));
  const hashed = target.div(max(diffuseColor.a, 1e-6));
  mat.opacityNode = select(frame.z.greaterThan(1.5), hashed, op);
  return next ? next(ctx) : undefined;
}
