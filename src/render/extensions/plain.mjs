// The "plain" shading part (effects detail Very Low, cartwheel/quality.h; alone as xplain, composed as
// xcluster_csm_plain_gi and so on): a cheaper lighting model for a MeshStandardMaterial or
// MeshPhysicalMaterial, with the material's own inputs (colour, maps, roughness, metalness, normal,
// emissive, ambient occlusion, light map) unchanged.
//
//   direct light   Lambert diffuse, and a normalized Blinn-Phong highlight whose exponent follows the
//                  roughness (n = 2 / a^2 - 2, a = roughness^2) tinted by the specular colour
//   indirect       the diffuse irradiance (probes, ambient and hemisphere lights, light maps, the
//                  environment's irradiance) on the diffuse colour; the environment's reflection is not
//                  sampled: the specular colour takes the same irradiance through an analytic split-sum
//                  approximation (no lookup table), so metals keep their tint
//   left out       r187's GGX and its multiple-scattering terms, the reflection sample of the
//                  environment map, clearcoat, sheen, iridescence, anisotropy and retroreflection
// A material with transmission keeps r187's full model (without it the glass would draw opaque).
// The depth (shadow caster) program carries nothing of this part. No values: the C side registers it
// with an empty fill hook (src/render/lighting.c).
export const name = 'plain';
export const part = 'plain';

const LIT = new Set(['standard', 'physical']);

export async function apply(ctx, next) {
  const { TSL, THREE, mat, kind } = ctx;
  if (kind === 'depth' || !LIT.has(kind) || mat.transmission > 0) return next ? next(ctx) : undefined;
  const {
    normalView, positionViewDirection, diffuseContribution, specularColorBlended, roughness, BRDF_Lambert, float, vec2, vec4,
    min, max, exp2, pow,
  } = TSL;

  class PlainLightingModel extends THREE.LightingModel {
    direct({ lightDirection, lightColor, reflectedLight }) {
      const dotNL = normalView.dot(lightDirection).clamp();
      const irradiance = dotNL.mul(lightColor);
      reflectedLight.directDiffuse.addAssign(irradiance.mul(BRDF_Lambert({ diffuseColor: diffuseContribution })));
      const h = lightDirection.add(positionViewDirection).normalize();
      const dotNH = normalView.dot(h).clamp();
      const a = roughness.mul(roughness);
      const n = min(max(float(2).div(max(a.mul(a), 1e-4)).sub(2), 1), 4096); // (at least 1: pow(0, 0) is undefined)
      // normalized Blinn-Phong with the implicit visibility of 1/4: (n + 2) / (8 pi)
      const d = n.add(2).mul(1 / (8 * Math.PI)).mul(pow(dotNH, n));
      reflectedLight.directSpecular.addAssign(irradiance.mul(specularColorBlended).mul(d));
    }

    indirect(builder) {
      const { irradiance, iblIrradiance, ambientOcclusion, reflectedLight } = builder.context;
      // r187 lights the diffuse layer with irradiance x BRDF_Lambert and the environment's iblIrradiance / pi x
      // the diffuse colour: the same thing
      const total = irradiance.add(iblIrradiance).toVar();
      reflectedLight.indirectDiffuse.addAssign(total.mul(BRDF_Lambert({ diffuseColor: diffuseContribution })));
      // Karis's analytic environment BRDF (mobile): the split sum's scale and bias from roughness and N.V
      const dotNV = normalView.dot(positionViewDirection).clamp();
      const r = roughness.mul(vec4(-1, -0.0275, -0.572, 0.022)).add(vec4(1, 0.0425, 1.04, -0.04));
      const a004 = min(r.x.mul(r.x), exp2(dotNV.mul(-9.28))).mul(r.x).add(r.y);
      const ab = vec2(-1.04, 1.04).mul(a004).add(r.zw);
      const env = specularColorBlended.mul(ab.x).add(ab.y);
      reflectedLight.indirectSpecular.addAssign(total.mul(1 / Math.PI).mul(env));
      reflectedLight.indirectDiffuse.mulAssign(ambientOcclusion);
      reflectedLight.indirectSpecular.mulAssign(ambientOcclusion);
    }
  }

  mat.setupLightingModel = () => new PlainLightingModel();
  ctx.plain = true; // (parts after this one read it: the GI grid reads its first bands only, src/gi/extensions/gi_shading.mjs)
  return next ? next(ctx) : undefined;
}
