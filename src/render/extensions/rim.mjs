// The "rim" shading part (cartwheel/rim_light.h; alone as xrim, composed as xcluster_rim_gi and
// so on): a coloured glow along a surface's silhouette, added to the material's emissive light.
//
//   glow = rimColor.rgb * rimColor.w * (1 - saturate(N . V)) ^ rimShape.x
//          * mix(rimShape.w, 1, smoothstep(rimShape.y, rimShape.z, distance to the camera))
//
// Values (filled per material by src/render/rim_light.c):
//   ext.rimColor  vec4: colour (linear rgb), strength
//   ext.rimShape  vec4: power, fade-in near, fade-in far (metres), the share kept below near
// The depth (shadow caster) program carries nothing of this part.
export const name = 'rim';
export const part = 'rim';

const LIT = new Set(['standard', 'physical']);

export async function apply(ctx, next) {
  const { TSL, THREE, mat, props, sentinel, kind } = ctx;
  if (kind === 'depth' || !LIT.has(kind)) return next ? next(ctx) : undefined;
  const { uniform, normalView, positionView, positionViewDirection, materialEmissive, float, smoothstep, mix } = TSL;
  const v4 = (n) => {
    const s = [sentinel(), sentinel(), sentinel(), sentinel()];
    const u = uniform(new THREE.Vector4(...s)).setName(n);
    props['ext.' + n] = { kind: 'vec4', value: s };
    return u;
  };
  const color = v4('rimColor');
  const shape = v4('rimShape');
  const facing = normalView.dot(positionViewDirection).clamp();
  const edge = float(1).sub(facing).pow(shape.x);
  const fade = mix(shape.w, float(1), smoothstep(shape.y, shape.z, positionView.length()));
  const glow = color.xyz.mul(color.w).mul(edge).mul(fade);
  // over the material's own emissive (an earlier part's emissiveNode, or the material's colour x intensity x map)
  mat.emissiveNode = (mat.emissiveNode ?? materialEmissive).add(glow);
  return next ? next(ctx) : undefined;
}
