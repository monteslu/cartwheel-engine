// The "foliage" shading part (cartwheel/foliage.h; alone as xfoliage, composed as xcluster_csm_foliage_gi and so on):
// wind on the vertices, two-sided lighting without the back-face flip, dithered fades by distance, and a root colour
// from the instance colour. Every feature is switched by its values, so one program serves them all; with every value
// at its default the material draws as without the part.
//
// Wind (vertex stage, the shadow caster's program too): the lean of a vertex `share` of the way up its mesh
// (positionGeometry.y / height, clamped to 0..1), at world (x, z), instance scale s:
//   k     = strength * share^2 * s
//   th    = 2 pi (frequency * time + 2 * noise(0.9 x, 0.9 z))        smooth value noise, about a metre across
//   gw    = (0.5 + 0.5 sin(2 pi ((x, z) . dir / gust_scale - 0.25 frequency * time)))^2
//   along = k ((1 - gust) (0.5 + 0.5 sin th) + gust * gw * (0.75 + 0.25 sin th))
//   across= k * 0.125 sin(2.3 th + 1.7)
//   lean  = dir * along + (-dir.z, dir.x) * across, y lowered by (along^2 + across^2) / (2 height s) * share
// src/render/foliage.c cw_wind_lean is the same on the CPU. The instance scale is read back from r187's instance
// normal transform (an InstancedMesh's normal comes out divided by its scale).
//
// Values (filled per material by src/render/foliage.c):
//   ext.folFrame  vec4 (frame group): foliage clock (s), dither offset (IGN frame step, 0 without TAA), unused, phase
//                 spread (2: the noise turns a sway two whole cycles across its range; 0 under CW_TEST_FOLIAGE_LOCKSTEP)
//   ext.folWind   vec4: strength (m), frequency (Hz), direction x, z (unit)
//   ext.folWind2  vec4: gust share, gust scale (m), 1 / height, height (m)
//   ext.folMode   vec4: two-sided lighting (0/1), root colour (0/1), tip jitter (0..1), fades on (0/1)
//   ext.folFade   vec4: fade in from, to; fade out from, to (metres from the camera; to <= from: no band)
//   ext.folRoot   vec4: root colour height (m), unused x3 (states with an instance colour only)
export const name = 'foliage';
export const part = 'foliage';

const LIT = new Set(['lambert', 'phong', 'standard', 'physical', 'toon']);

export async function apply(ctx, next) {
  const { TSL, THREE, mat, props, sentinel, kind, features } = ctx;
  if (kind !== 'depth' && !LIT.has(kind)) return next ? next(ctx) : undefined;
  const {
    Fn, uniform, sharedUniformGroup, positionLocal, positionGeometry, normalLocal, normalGeometry,
    modelWorldMatrix, positionView, screenCoordinate, interleavedGradientNoise, faceDirection,
    materialColor, materialNormal, mat3, transpose, vertexColor, instanceColor, vec2, vec3, vec4, float, sin, floor, fract, dot, clamp,
    max, mix, select, smoothstep, length,
  } = TSL;
  const frameGroup = sharedUniformGroup('foliageFrame', 0, THREE.NodeUpdateType.RENDER);
  const v4 = (n, group) => {
    const s = [sentinel(), sentinel(), sentinel(), sentinel()];
    const u = uniform(new THREE.Vector4(...s)).setName(n);
    if (group) u.setGroup(group);
    props['ext.' + n] = { kind: 'vec4', value: s };
    return u;
  };
  const frame = v4('folFrame', frameGroup);
  const wind = v4('folWind'), wind2 = v4('folWind2');
  const TAU = 6.283185307;

  // ── wind ─────────────────────────────────────────────────────────────
  const hash2 = (p) => fract(sin(p.x.mul(127.1).add(p.y.mul(311.7))).mul(43758.5453));
  const vnoise = (p) => {
    const i = floor(p), f = fract(p);
    const u = f.mul(f).mul(vec2(3).sub(f.mul(2)));
    const a = hash2(i), b = hash2(i.add(vec2(1, 0))), c = hash2(i.add(vec2(0, 1))), d = hash2(i.add(vec2(1, 1)));
    return a.add(b.sub(a).mul(u.x)).add(c.sub(a).mul(u.y)).add(a.sub(b).sub(c).add(d).mul(u.x).mul(u.y));
  };
  const instanced = features.includes('instbig') || features.includes('inst');
  const prevPosition = mat.positionNode ?? positionLocal;
  mat.positionNode = Fn(() => {
    const p = prevPosition;
    const w = modelWorldMatrix.mul(vec4(p, 1)).xyz.toVar();
    const share = clamp(positionGeometry.y.mul(wind2.z), 0, 1);
    // r187's instancing divides the normal by the instance's scale
    const s = instanced ? clamp(length(normalGeometry).div(max(length(normalLocal), 1e-4)), 0, 8) : float(1);
    const k = wind.x.mul(share).mul(share).mul(s).toVar();
    const dx = wind.z, dz = wind.w;
    const t = frame.x;
    const th = float(TAU).mul(wind.y.mul(t).add(vnoise(w.xz.mul(0.9)).mul(frame.w))).toVar();
    const s1 = sin(th);
    const gw0 = sin(float(TAU).mul(dot(w.xz, vec2(dx, dz)).div(wind2.y).sub(wind.y.mul(t).mul(0.25)))).mul(0.5).add(0.5);
    const gw = gw0.mul(gw0);
    const g = wind2.x;
    const along = k.mul(float(1).sub(g).mul(s1.mul(0.5).add(0.5)).add(g.mul(gw).mul(s1.mul(0.25).add(0.75)))).toVar();
    const across = k.mul(0.125).mul(sin(th.mul(2.3).add(1.7))).toVar();
    const drop = along.mul(along).add(across.mul(across)).div(max(wind2.w.mul(s), 0.05).mul(2)).mul(share);
    const lean = vec3(dx.mul(along).sub(dz.mul(across)), drop.negate(), dz.mul(along).add(dx.mul(across)));
    // back into the mesh's space (the identity for the terrain's scatter): the transpose over the squared scale undoes
    // a rotation and a uniform scale (r187 gives no inverse world matrix three.c fills)
    const m3 = mat3(modelWorldMatrix);
    const ax = m3.mul(vec3(1, 0, 0));
    const back = transpose(m3).mul(lean).div(max(dot(ax, ax), 1e-8));
    return p.add(back);
  })();
  if (kind === 'depth') return next ? next(ctx) : undefined;

  const mode = v4('folMode'), fade = v4('folFade');

  // ── two-sided lighting: the back face keeps the front face's normal ──
  const nrm = mat.normalNode ?? materialNormal;
  mat.normalNode = nrm.mul(select(mode.x.greaterThan(0.5), faceDirection, float(1)));

  // ── root colour: the lower part takes the instance colour ──────────
  if (features.includes('icol')) {
    const root = v4('folRoot');
    const base = mat.colorNode ? vec4(mat.colorNode) : materialColor;
    const vc = features.includes('vcol') ? vec3(vertexColor().xyz) : vec3(1);
    const ic = vec3(instanceColor);
    // per-instance variation of the upper colour, from the instance colour itself (the CPU varied it per instance)
    const j = fract(sin(dot(ic, vec3(12.9898, 78.233, 37.719))).mul(43758.5453)).sub(0.5).mul(mode.z).mul(2).add(1);
    const up = clamp(positionGeometry.y.div(max(root.x, 1e-3)), 0, 1);
    const want = mix(ic, base.xyz.mul(vc).mul(j), smoothstep(0, 1, up));
    // r187 multiplies the vertex and instance colours into colorNode after it: divide them out
    const rooted = want.div(max(vc.mul(ic), vec3(1e-4)));
    mat.colorNode = vec4(mix(base.xyz, rooted, mode.y), base.w);
  }

  // ── dithered fades: a screen-door on interleaved gradient noise, moved every frame under TAA ────
  const d = length(positionView);
  const band = (a, b) => select(b.greaterThan(a), clamp(d.sub(a).div(max(b.sub(a), 1e-4)), 0, 1), float(0));
  const tOut = band(fade.z, fade.w);
  const tIn = select(fade.y.greaterThan(fade.x), clamp(d.sub(fade.x).div(max(fade.y.sub(fade.x), 1e-4)), 0, 1), float(1));
  const noise = interleavedGradientNoise(screenCoordinate.xy.add(vec2(frame.y)));
  const keep = mode.w.lessThan(0.5).or(noise.greaterThanEqual(tOut).and(noise.lessThan(tIn)));
  mat.maskNode = mat.maskNode ? mat.maskNode.and(keep) : keep;
  return next ? next(ctx) : undefined;
}
