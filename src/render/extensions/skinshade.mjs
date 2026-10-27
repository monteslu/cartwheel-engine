// The "skinshade" shading part (cartwheel/skin_shading.h; alone as xskinshade, composed as
// xcluster_csm_skinshade_gi and so on): skin shading on a MeshStandardMaterial or
// MeshPhysicalMaterial. It wraps the material's own r186 lighting model so every direct light
// (the scene's lights, the cluster part's lights, the cascaded sun: they all reach the model's
// direct()) adds two terms before r186's own BRDF runs:
//
//   wrap          the diffuse light past the terminator, per channel w = scatter.rgb * wrap:
//                 ((N.L + w) / (1 + w) clamped - N.L clamped) * light * BRDF_Lambert(diffuse)
//   translucency  r186 MeshSSSNodeMaterial's SSSLightingModel term (fast translucency):
//                 (pow(saturate(V . -normalize(L + N * distortion)), power) + ambient)
//                 * translucency * scatter.rgb * light * diffuse
//
// Values (filled per material by src/render/skin_shading.c):
//   ext.skinScatter       vec4: scatter colour (linear rgb), wrap
//   ext.skinTranslucency  vec4: translucency, distortion, power, ambient
// The depth (shadow caster) program carries nothing of this part.
export const name = 'skinshade';
export const part = 'skinshade';

const LIT = new Set(['standard', 'physical']);

export async function apply(ctx, next) {
  const { TSL, THREE, mat, props, sentinel, kind } = ctx;
  if (kind === 'depth' || !LIT.has(kind)) return next ? next(ctx) : undefined;
  const { uniform, normalView, positionViewDirection, diffuseContribution, BRDF_Lambert, float, max } = TSL;
  const v4 = (n) => {
    const s = [sentinel(), sentinel(), sentinel(), sentinel()];
    const u = uniform(new THREE.Vector4(...s)).setName(n);
    props['ext.' + n] = { kind: 'vec4', value: s };
    return u;
  };
  const scatter = v4('skinScatter');
  const trans = v4('skinTranslucency');

  // the renderer draws a NodeMaterial copied from mat (own properties copied), so the
  // override reaches the copy and runs with it as `this`; an earlier part's override runs first
  const prev = mat.setupLightingModel;
  mat.setupLightingModel = function (builder) {
    const model = prev ? prev.call(this, builder) : Object.getPrototypeOf(this).setupLightingModel.call(this, builder);
    const direct = model.direct.bind(model);
    model.direct = (data, b) => {
      const { lightDirection, lightColor, reflectedLight } = data;
      const dotNL = normalView.dot(lightDirection);
      const w = scatter.xyz.mul(scatter.w);
      const wrapped = dotNL.add(w).div(w.add(1)).clamp();
      const extra = max(wrapped.sub(dotNL.clamp()), float(0));
      reflectedLight.directDiffuse.addAssign(extra.mul(lightColor).mul(BRDF_Lambert({ diffuseColor: diffuseContribution })));
      const h = lightDirection.add(normalView.mul(trans.y)).normalize();
      const glow = positionViewDirection.dot(h.negate()).clamp().pow(trans.z).add(trans.w).mul(trans.x);
      reflectedLight.directDiffuse.addAssign(scatter.xyz.mul(glow).mul(lightColor).mul(diffuseContribution));
      direct(data, b);
    };
    return model;
  };
  return next ? next(ctx) : undefined;
}
