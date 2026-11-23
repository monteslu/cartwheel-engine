// The "hfog" shading part (alone as xhfog, composed as xcluster_hfog and so on): a HeightFog applied to a
// transparent or additive surface at its own distance from the camera, in its own program. A transparent surface
// writes no depth, so the fog passes (which fog each pixel by the depth of what is behind it) draw before the
// transparent queue and leave it alone; each transparent material carries this part while a fog draws
// (src/render/fog.c marks them, environment_internal.h).
//
//   ray       from the camera to the fragment: length L, direction v
//   fog       f = min(1 - exp(-tau), maxOpacity), tau the exponential height fog's optical depth along v for
//             max(L - start, 0) (the fog pass's formula, src/render/fog.c)
//   light     (colour + sunlight x HG(v . sun, g)) x f, the fog's own in-scattered light
//   output    rgb x (1 - f) + (light x k + f x w) x coverage (alpha when premultiplied, else 1): k = 1 for an alpha-blended surface
//             (it turns into the fog), 0 for an additive or subtractive one (its light fades: added light in thick
//             fog adds nothing); w = 1 for a multiplied one (it fades toward white, multiplying by nothing)
//
// Values (filled per draw by src/render/fog.c from the drawing tree's fog and the material's blending):
//   ext.hfogA  vec4: density, height falloff, base height, max opacity
//   ext.hfogB  vec4: start distance, k, sun g, w
//   ext.hfogC  vec4: in-scattered colour (rgb), 1 when the material's output is premultiplied
//   ext.hfogD  vec4: toward the sun (xyz)
//   ext.hfogE  vec4: sunlight x scatter (rgb)
// The depth (shadow caster) program carries nothing of this part.
export const name = 'hfog';
export const part = 'hfog';

export async function apply(ctx, next) {
  const { TSL, THREE, mat, props, sentinel, kind } = ctx;
  if (kind === 'depth') return next ? next(ctx) : undefined;
  const { uniform, positionView, cameraWorldMatrix, output, float, vec3, vec4, exp, abs, min, max, select, pow, dot, mix } = TSL;
  const v4 = (n) => {
    const s = [sentinel(), sentinel(), sentinel(), sentinel()];
    const u = uniform(new THREE.Vector4(...s)).setName(n);
    props['ext.' + n] = { kind: 'vec4', value: s };
    return u;
  };
  const A = v4('hfogA');
  const B = v4('hfogB');
  const C = v4('hfogC');
  const D = v4('hfogD');
  const E = v4('hfogE');

  const fogged = (out) => {
    // the ray in world space from the view-space position (every kind has one: sprites and points too)
    const ray = cameraWorldMatrix.mul(vec4(positionView, 0)).xyz;
    const len = max(positionView.length(), float(1e-6));
    const v = ray.div(len);
    const l = max(len.sub(B.x), float(0));
    // the camera's height from its world matrix (three.c fills camera.worldMatrix; cameraPosition it has only for transmission)
    const camY = cameraWorldMatrix.element(3).y;
    const a = A.x.mul(exp(A.y.negate().mul(camY.sub(A.z))));
    const k = A.y.mul(v.y).mul(l);
    const big = abs(k).greaterThan(1e-4);
    const ks = select(big, k, float(1));
    const term = select(big, float(1).sub(exp(ks.negate())).div(ks), float(1).sub(k.mul(0.5)));
    const tau = a.mul(l).mul(term);
    const f = min(float(1).sub(exp(tau.negate())), A.w).max(0);
    const g = B.z;
    const g2 = g.mul(g);
    const mu = dot(v, D.xyz);
    const hg = float(1).sub(g2).div(pow(max(float(1).add(g2).sub(g.mul(2).mul(mu)), float(1e-4)), float(1.5)).mul(12.5663706));
    const light = C.xyz.add(E.xyz.mul(hg)).mul(f);
    // (a premultiplied output carries its coverage in its colour: the fog's light by coverage too)
    const cover = mix(float(1), out.a, C.w);
    const rgb = out.rgb.mul(float(1).sub(f)).add(light.mul(B.y).add(vec3(f).mul(B.w)).mul(cover));
    return vec4(rgb, out.a);
  };
  // over the material's whole output (r187 NodeMaterial: output, then a custom outputNode in its place)
  mat.outputNode = fogged(output);
  return next ? next(ctx) : undefined;
}
