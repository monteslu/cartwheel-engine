// The "waterline" shading part (cartwheel/waterline.h; alone as xwaterline, composed as xcluster_csm_waterline_gi and
// so on): where an object meets the water. Against the water's surface at the object (a plane, filled each tick from
// cw_water_sample_at), each pixel's signed height h over the surface decides:
//
//   under      h < 0: wet, and the caustics the water's floor shows (its own tile and recipe, so legs and floor match),
//              fading with depth by the water's clarity. The water draws this part through its refraction.
//   band       under the high-water mark (plus a wick for porous surfaces): wet, its top drying first and unevenly
//   soak       a wetness over the whole surface (spray), drying over time
//   wet        Lagarde's wet surface: albedo x mix(1, 0.2, porosity); a surface that can shine (shine > 0) gets a lower
//              roughness and beaded drops; cloth (shine 0) stays matte and only darkens
//   meniscus   0 <= h < max(meniscus, 1.5 px): the sky light the water's curve up to the object reflects
//
// Values (filled per material by src/waterline/waterline.c):
//   ext.wlPlane  vec4: the surface's unit normal (world), w: dot(normal, a surface point): h = dot(n, P) - w
//   ext.wlMark   vec4: world height of the high-water mark, soak 0..1, the line's wobble (m), the water's clock (s)
//   ext.wlLook   vec4: porosity, shine, meniscus height (m), meniscus strength
//   ext.wlSky    vec4: what the meniscus reflects (linear rgb), caustic strength
//   ext.wlCaus   vec4: direction to the sun (unit), 1 / caustic tile (m)
//   ext.wlExtra  vec4: caustic clock (s), 1.6 / clarity (per metre), wick (m), unused
//   ext.wlCaustic texture: the caustic tile (red: the sharp web, green: a soft copy, both / 4)
// Every term is plain arithmetic and implicit-LOD reads in the fragment's top level (no branch), so fwidth and the
// texture reads are in uniform control flow on WebGPU. The depth (shadow caster) program carries nothing of this part.
export const name = 'waterline';
export const part = 'waterline';

const LIT = new Set(['standard', 'physical']);

export async function apply(ctx, next) {
  const { TSL, THREE, mat, props, sentinel, kind } = ctx;
  if (kind === 'depth' || !LIT.has(kind)) return next ? next(ctx) : undefined;
  const { uniform, texture, positionWorld, positionLocal, materialColor, materialRoughness, materialEmissive, float, vec2, vec3, vec4,
    dot, max, min, mix, smoothstep, step, exp, sin, fwidth, fract, floor, length, clamp, normalWorld } = TSL;
  const v4 = (n) => {
    const s = [sentinel(), sentinel(), sentinel(), sentinel()];
    const u = uniform(new THREE.Vector4(...s)).setName(n);
    props['ext.' + n] = { kind: 'vec4', value: s };
    return u;
  };
  const plane = v4('wlPlane');
  const mark = v4('wlMark');
  const look = v4('wlLook');
  const sky = v4('wlSky');
  const caus = v4('wlCaus');
  const extra = v4('wlExtra');
  const tile = new THREE.DataTexture(new Uint8Array(16).fill(128), 2, 2);
  tile.name = 'ext.wlCaustic';
  tile.magFilter = THREE.LinearFilter;
  tile.minFilter = THREE.LinearMipmapLinearFilter;
  tile.generateMipmaps = true;
  tile.wrapS = tile.wrapT = THREE.RepeatWrapping;
  tile.needsUpdate = true;

  const P = positionWorld;
  // the signed height over the water, with a small moving ripple along the object
  const h = dot(plane.xyz, P).sub(plane.w).sub(mark.z.mul(sin(dot(P.xz, vec2(9.1, 7.3)).add(mark.w.mul(2.4)))));
  const px = max(fwidth(h), float(1e-4));
  const under = float(1).sub(smoothstep(px.negate(), px, h));
  // the wet band: under the mark (a porous surface wicks it higher), its top drying first, unevenly
  const top = mark.x.add(extra.z.mul(look.x));
  const above = h.sub(P.y.sub(top)); // the band's height over the water here (m)
  const fadeW = max(max(above.mul(0.45), px.mul(3)), float(0.006));
  const L = positionLocal;
  const ragged = sin(dot(L.xz, vec2(7.3, 5.1))).mul(0.65).add(sin(dot(L.xz, vec2(-17.9, 13.1))).mul(0.35)).mul(0.3);
  const inBand = float(1).sub(smoothstep(fadeW.negate(), float(0), P.y.sub(top).add(fadeW.mul(ragged))));
  // drier toward the band's top
  const bandWet = inBand.mul(float(1).sub(smoothstep(float(0), max(above, float(0.01)), h).mul(0.4)));
  const wet = clamp(max(max(under, bandWet), mark.y), 0, 1);
  // beaded drops on a surface that can shine, over the line (3D cells on the object, 1.8 cm)
  const q = L.mul(55);
  const cell = floor(q);
  const rnd = fract(sin(dot(cell, vec3(12.9898, 78.233, 37.719))).mul(43758.5453));
  const off = vec3(rnd, fract(rnd.mul(7.31)), fract(rnd.mul(3.17))).sub(0.5).mul(0.4);
  const dropR = length(fract(q).sub(0.5).sub(off));
  const drop = float(1).sub(smoothstep(0.17, 0.25, dropR)).mul(step(0.7, rnd))
    .mul(max(bandWet, mark.y)).mul(float(1).sub(under)).mul(look.y);
  // a drop is a little lens: whichever way it faces, a point on it shows the sky or the sun (brighter on the sunny side)
  const facing = smoothstep(float(-0.1), float(0.5), dot(normalWorld, caus.xyz));
  const glint = float(1).sub(smoothstep(0.03, 0.08, dropR)).mul(drop).mul(facing.mul(0.7).add(0.3));
  // caustics on the part under the surface (the water floor's recipe)
  const d = max(h.negate(), float(0));
  const cq = P.xz.add(caus.xz.div(max(caus.y, 0.3)).mul(d).mul(0.75)).mul(caus.w);
  const ct = extra.x;
  const k1 = texture(tile, cq.add(vec2(0.013, 0.007).mul(ct))).rg;
  const k2 = texture(tile, cq.mul(1.31).add(vec2(0.41, 0.17)).sub(vec2(0.006, 0.011).mul(ct))).rg;
  const soft = smoothstep(0.5, 4, d);
  const kc = dot(min(k1, k2), vec2(float(1).sub(soft), soft)).mul(4);
  const cfade = smoothstep(0.02, 0.5, d).mul(exp(d.mul(extra.y).negate())).mul(smoothstep(0, 0.25, caus.y)).mul(under).mul(facing.mul(0.8).add(0.2));
  const light = float(1).add(sky.w.mul(cfade).mul(kc.mul(2.2).sub(1)));

  const base = mat.colorNode ? vec4(mat.colorNode) : materialColor;
  const dark = mix(float(1), mix(float(1), float(0.2), look.x), wet).mul(float(1).sub(drop.mul(0.15)));
  mat.colorNode = vec4(base.xyz.mul(dark).mul(light), base.w);
  if ('roughness' in mat) {
    const r0 = mat.roughnessNode ?? materialRoughness;
    const rWet = mix(r0, r0.mul(mix(float(1), float(0.3), look.y)), wet);
    mat.roughnessNode = mix(rWet, float(0.05), drop);
  }
  // the meniscus: a band at least 1.5 px tall just over the line
  const men = float(1).sub(smoothstep(float(0), max(look.z, px.mul(1.5)), h)).mul(step(float(0), h)).mul(look.w);
  mat.emissiveNode = (mat.emissiveNode ?? materialEmissive).add(sky.xyz.mul(men.add(glint.mul(2.5))));
  return next ? next(ctx) : undefined;
}
