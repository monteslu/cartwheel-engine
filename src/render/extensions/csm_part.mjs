// The "csm" shading part (composed into extension states x<parts>): the sun's
// cascaded shadow maps, as three.js r187's CSMShadowNode draws them
// (examples/jsm/csm/CSMShadowNode.js): the view split into up to four
// cascades by r187's practical split, each cascade an orthographic shadow map
// fitted around its slice of the view, picked per fragment by its linear depth
// between the camera's near and the shadow distance, cross-faded over r187's
// fade margin. Each cascade is filtered with r187's PCFShadowFilter (five
// Vogel-disk taps rotated by interleaved gradient noise, hardware 2x2 compare
// on each) and its shadow coordinates are r187 ShadowNode's (the normal bias
// along the world normal, y = 1 - y, the depth bias added).
//
// The sun itself is drawn here: src/render/lighting.c takes the cascaded
// DirectionalLight3D out of three.c's light list and this part adds it to the
// material's lighting (LightsNode.setupDirectLight with the sun's direction
// and colour, times mix(1, shadow, intensity)), so its BRDF is r187's own.
// The texture reads run in uniform control flow (no branch around them), as
// WGSL requires of textureSampleCompare: every cascade's coordinate is
// computed, the cascade holding the fragment and its fade partner selected,
// and both filtered (ten taps).
//
// What the fill hook (src/render/lighting.c) sets:
//   ext.csmMap      DepthTexture (compare LessEqual, linear): the cascades in a
//                   2 x 2 atlas, cascade c in the quarter (c & 1, c >> 1)
//   ext.csmMatrix0..3  world -> shadow uv (before r187's y flip) and depth, per cascade
//                   (r187 LightShadow.matrix with the atlas quarter folded in)
//   ext.csmDir      xyz: view-space direction to the sun; w: shadow intensity
//   ext.csmColor    rgb: colour x intensity; w: cascades in use (1..4)
//   ext.csmBias     x: bias, y: normal bias (world units), z: 1 / atlas size, w: PCF radius (texels)
//   ext.csmDepth    x: camera near, y: shadow far, z: fade (1 on), w: 0
//   ext.csmRanges0  cascades 0 and 1 as (start, end) of linear depth (0 at near, 1 at shadow far)
//   ext.csmRanges1  cascades 2 and 3
//   ext.csmReceive  (per object) 1 when the mesh receives shadows, else 0

export const part = 'csm';
// after the clustered lights (200): both are direct light, csm comes second
export const order = 210;

const LIT_KINDS = new Set(['lambert', 'phong', 'standard', 'physical', 'toon']);

export async function apply(ctx, next) {
  const { TSL, THREE, mat, props, sentinel, kind } = ctx;
  if (kind === 'depth' || !LIT_KINDS.has(kind)) return next ? next(ctx) : undefined;
  const {
    Fn, uniform, sharedUniformGroup, texture, positionView, positionWorld, normalWorld, screenCoordinate,
    vec2, vec4, float, int, select, clamp, min, max, mix, interleavedGradientNoise, vogelDiskSample,
  } = TSL;

  const frame = sharedUniformGroup('lightingFrame', 0, THREE.NodeUpdateType.RENDER);
  const v4 = (name) => {
    const s = [sentinel(), sentinel(), sentinel(), sentinel()];
    const u = uniform(new THREE.Vector4(...s)).setName(name);
    u.setGroup(frame);
    props['ext.' + name] = { kind: 'vec4', value: s };
    return u;
  };
  const m4 = (name) => {
    const s = Array.from({ length: 16 }, () => sentinel());
    const u = uniform(new THREE.Matrix4().fromArray(s)).setName(name);
    u.setGroup(frame);
    props['ext.' + name] = { kind: 'mat4', value: s };
    return u;
  };
  const dir = v4('csmDir'), color = v4('csmColor'), bias = v4('csmBias'), depth = v4('csmDepth');
  const ranges0 = v4('csmRanges0'), ranges1 = v4('csmRanges1');
  const mats = [m4('csmMatrix0'), m4('csmMatrix1'), m4('csmMatrix2'), m4('csmMatrix3')];
  // per object (r187: a mesh that does not receive shadows is lit unshadowed)
  const rs = sentinel();
  const receive = uniform(rs).setName('csmReceive');
  props['ext.csmReceive'] = { kind: 'f32', value: [rs] };

  const map = new THREE.DepthTexture(4, 4);
  map.compareFunction = THREE.LessEqualCompare;
  map.minFilter = map.magFilter = THREE.LinearFilter;
  map.name = 'ext.csmMap';

  // r187 PCFShadowFilter on coord (xy uv, z depth; a vec4 read once, no swizzle of a swizzle,
  // which Dawn's Tint rejects), the taps clamped into the cascade's quarter (lo, hi) so none reads a neighbour
  const pcf = (coord, lo, hi) => {
    const texel = bias.z, radius = bias.w.mul(texel);
    const phi = interleavedGradientNoise(screenCoordinate.xy).mul(6.28318530718);
    const pad = radius.add(texel);
    let sum = float(0);
    for (let k = 0; k < 5; k++) {
      const uv = clamp(coord.xy.add(vogelDiskSample(int(k), int(5), phi).mul(radius)), lo.add(pad), hi.sub(pad));
      sum = sum.add(texture(map, uv).compare(coord.z));
    }
    return sum.mul(1 / 5);
  };
  // r187 ShadowNode for cascade c: the shadow position with the normal bias, y flipped, the depth
  // bias added; w = 1 inside the cascade's map (r187's frustum test), else 0
  const cascadeCoord = (c) => {
    const p = mats[c].mul(vec4(positionWorld.add(normalWorld.mul(bias.y)), 1));
    const xyz = p.xyz.div(p.w);
    const qx = (c % 2) * 0.5, qy = (1 - Math.floor(c / 2)) * 0.5;
    const lx = xyz.x.sub(qx).mul(2), ly = xyz.y.sub(qy).mul(2);
    const z = xyz.z.add(bias.x);
    const inside = lx.greaterThanEqual(0).and(lx.lessThanEqual(1)).and(ly.greaterThanEqual(0)).and(ly.lessThanEqual(1))
      .and(z.lessThanEqual(1));
    return vec4(xyz.x, float(1).sub(xyz.y), z, select(inside, float(1), float(0)));
  };
  // the quarter of cascade c after the flip: x from (c & 1) / 2, y from (c >> 1) / 2
  const quarter = (c) => vec2((c % 2) * 0.5, Math.floor(c / 2) * 0.5);
  // nodes[i] for a float index node
  const pick = (nodes, idx) => nodes.slice(1).reduce((acc, n, k) => select(idx.equal(float(k + 1)), n, acc), nodes[0]);

  class CsmLightingNode extends THREE.LightingNode {
    static get type() { return 'CsmLightingNode'; }
    setup(builder) {
      const lightsNode = builder.lightsNode;
      const reflected = builder.context.reflectedLight;
      reflected.directDiffuse.toStack();
      reflected.directSpecular.toStack();
      Fn(() => {
        // r187 CSMShadowNode: linear depth between the camera's near and the shadow far
        const linear = positionView.z.negate().sub(depth.x).div(depth.y.sub(depth.x)).toVar();
        const count = color.w;
        // (components read one at a time: Dawn's Tint rejects a swizzle of a swizzle of a uniform)
        const range = [{ x: ranges0.x, y: ranges0.y }, { x: ranges0.z, y: ranges0.w }, { x: ranges1.x, y: ranges1.y }, { x: ranges1.z, y: ranges1.w }];
        // r187 _setupFade per cascade: margin = 0.25 * closestEdge^2 (0 without fade), the range
        // widened by half of it on each side but the last's far edge; the weight is the distance to
        // the widened edges over the margin (1 without fade, and at the first cascade's near side)
        const weight = [], center = [];
        let index = float(0);
        for (let c = 0; c < 4; c++) {
          const r = range[c];
          const used = float(c).lessThan(count);
          const last = float(c).equal(count.sub(1));
          center.push(r.x.add(r.y).div(2));
          const closest = select(linear.lessThan(center[c]), r.x, r.y);
          const margin = float(0.25).mul(closest.mul(closest)).mul(depth.z);
          const x0 = r.x.sub(margin.div(2));
          const y0 = select(last, r.y, r.y.add(margin.div(2)));
          const inRange = used.and(linear.greaterThanEqual(x0)).and(linear.lessThanEqual(y0));
          let ratio = select(margin.greaterThan(0), clamp(min(linear.sub(x0), y0.sub(linear)).div(max(margin, 1e-6)), 0, 1), float(1));
          if (c === 0) ratio = select(linear.greaterThan(center[c]), ratio, float(1));
          weight.push(select(inRange, ratio, float(0)).toVar());
          // the cascade holding the point: the last one whose range starts at or before it
          if (c > 0) index = select(used.and(linear.greaterThanEqual(r.x)), float(c), index);
        }
        const a = index.toVar();
        // its fade partner: the neighbour on the side of the nearer edge
        const b = clamp(select(linear.lessThan(pick(center, a)), a.sub(1), a.add(1)), 0, 3).toVar();
        const coords = [0, 1, 2, 3].map((c) => cascadeCoord(c));
        const quarters = [0, 1, 2, 3].map(quarter);
        const ca = pick(coords, a).toVar(), cb = pick(coords, b).toVar();
        const qa = pick(quarters, a), qb = pick(quarters, b);
        // two filters, always run (the texture reads stay in uniform control flow)
        // (filtered into variables first: TSL turns a select around a call into a branch)
        const fa = pcf(ca, qa, qa.add(0.5)).toVar(), fb = pcf(cb, qb, qb.add(0.5)).toVar();
        const sa = select(ca.w.greaterThan(0.5), fa, float(1));
        const sb = select(cb.w.greaterThan(0.5), fb, float(1));
        const wb = select(b.equal(a), float(0), pick(weight, b));
        const ret = float(1).sub(float(1).sub(sa).mul(pick(weight, a))).sub(float(1).sub(sb).mul(wb));
        const shadow = mix(float(1), clamp(ret, 0, 1), dir.w.mul(receive));
        lightsNode.setupDirectLight(builder, this, { lightDirection: dir.xyz, lightColor: color.rgb.mul(shadow) });
      }, 'void')();
    }
  }
  const node = new CsmLightingNode();
  const prev = mat.setupMaterialLightings;
  mat.setupMaterialLightings = function (builder) {
    const base = prev ? prev.call(this, builder) : Object.getPrototypeOf(this).setupMaterialLightings.call(this, builder);
    return [...base, node];
  };
  return next ? next(ctx) : undefined;
}
