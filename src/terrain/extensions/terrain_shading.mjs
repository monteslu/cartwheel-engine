// Terrain shading for three.c's extension programs, written in three.js r186
// TSL (see terrain.mjs, terrainlow.mjs and terrainfar.mjs, the extensions
// themselves). Not an extension: the three import it.
//
// The mesh is a CDLOD patch: an InstancedMesh of one flat grid whose instance
// matrices place each selected quadtree node (src/terrain/terrain_render.c).
// The grid's vertices sit at y = 1 and the instance matrix's Y column carries
// the node's LOD level plus one, so after r186's instancing positionLocal is
// (world x, lod + 1, world z): everything the vertex stage needs, with no
// attribute of its own. (Plus one: a zero Y scale would make r186's instance
// normal transform divide by zero, and the NaN geometry normal would poison
// the roughness r186 derives from it.)
//
// Vertex stage: heights are the uint16 steps in an R32UI texture (r186's TSL
// types only 32-bit textures as integer ones), read with textureLoad at
// integer texel coordinates, so every fetch is exact and equal on GL and
// WebGPU. Morphing is vertical:
// a vertex on an odd row or column of its node moves toward the height of
// the coarser level's triangle under it (the midpoint of the two even
// neighbours along its row, column, or the quad diagonal Box3D's height
// field uses), so it never moves sideways and the fully morphed patch is
// exactly the next level's surface: no cracks between levels.
//
// Fragment stage: normals from a baked normal map; colour and roughness from
// Terrain3D's control map, a uint32 a sample in an R32UI texture (base and
// overlay layer ids, their blend, uv rotation and scale, hole), over two
// layer array textures (albedo + height, normal + roughness: Terrain3D's
// packing; repeat-wrapped and mipmapped by the hardware), with triplanar
// sampling on steep slopes. Past the far distance
// only a baked macro colour and the normal map are read.
//
// Names after "ext." are what the C fill hook sets (terrain_render.c).

export async function applyTerrain({ TSL, THREE, mat, props, sentinel }, mode) {
  const {
    Fn, If, uniform, texture, textureLoad, sharedUniformGroup, positionLocal, positionWorld, cameraViewMatrix,
    vec2, vec3, vec4, float, ivec2, clamp, fract, floor, round, exp2, mix, max, min, abs, sqrt, length, normalize,
    smoothstep, dFdx, dFdy, dot, select, sin, cos, pow, property, uint, int, materialColor, materialRoughness, materialMetalness, sRGBTransferEOTF,
  } = TSL;

  // ── per-frame values: one shared group, uploaded when they change ──
  const frame = sharedUniformGroup('terrainFrame', 0, THREE.NodeUpdateType.RENDER);
  const v4 = (name) => {
    const s = [sentinel(), sentinel(), sentinel(), sentinel()];
    const u = uniform(new THREE.Vector4(...s)).setName(name);
    u.setGroup(frame);
    props['ext.' + name] = { kind: 'vec4', value: s };
    return u;
  };
  const cam = v4('terrainCam');       // xyz: camera position (world)
  const grid = v4('terrainGrid');     // origin x, origin z, 1 / spacing, spacing
  const hgt = v4('terrainHeight');    // base y, metres per height step, texels x - 1, texels z - 1
  const lodp = v4('terrainLod');      // range of LOD 0, log2(ratio), morph start (0..1), morph end factor
  const far = v4('terrainFar');       // far start, 1 / fade length, macro variation, quality (0 low, 1 medium, 2 high)

  // textures: what the C side binds by these names
  // the C side's textures must be made with these settings (r186 bakes filters and wrapping into the code)
  const uintTex = (name) => {
    const t = new THREE.DataTexture(new Uint32Array(4), 2, 2, THREE.RedIntegerFormat, THREE.UnsignedIntType);
    t.name = 'ext.' + name;
    t.needsUpdate = true;
    return t;
  };
  const arrayTex = (name) => {
    const t = new THREE.DataArrayTexture(new Uint8Array(2 * 2 * 4 * 2).fill(128), 2, 2, 2);
    t.name = 'ext.' + name;
    t.magFilter = THREE.LinearFilter; t.minFilter = THREE.LinearMipmapLinearFilter; t.generateMipmaps = true;
    t.wrapS = t.wrapT = THREE.RepeatWrapping;
    t.needsUpdate = true;
    return t;
  };
  const dataTex = (name, linear, srgb) => {
    const t = new THREE.DataTexture(new Uint8Array([128, 128, 128, 255, 128, 128, 128, 255, 128, 128, 128, 255, 128, 128, 128, 255]), 2, 2);
    t.name = 'ext.' + name;
    if (linear) {
      t.magFilter = THREE.LinearFilter; t.minFilter = THREE.LinearMipmapLinearFilter; t.generateMipmaps = true;
    } else {
      t.magFilter = t.minFilter = THREE.NearestFilter; t.generateMipmaps = false;
    }
    t.wrapS = t.wrapT = THREE.ClampToEdgeWrapping;
    if (srgb) t.colorSpace = THREE.SRGBColorSpace;
    t.needsUpdate = true;
    return t;
  };
  const heightMap = uintTex('terrainHeights');

  // ── vertex: CDLOD position ─────────────────────────────────────────
  const fetchH = (x, z) => {
    return float(textureLoad(heightMap, ivec2(clamp(x, 0, hgt.z), clamp(z, 0, hgt.w))).r);
  };
  mat.positionNode = Fn(() => {
    const p = positionLocal;
    const lod = p.y.sub(1).toVar();
    const step = exp2(lod).toVar();
    const tx = round(p.x.sub(grid.x).mul(grid.z)).toVar();
    const tz = round(p.z.sub(grid.y).mul(grid.z)).toVar();
    // 1 on an odd column / row of this level's grid, else 0
    const ox = fract(tx.div(step.mul(2))).mul(2).toVar();
    const oz = fract(tz.div(step.mul(2))).mul(2).toVar();
    const h0 = fetchH(tx, tz).toVar();
    const ha = fetchH(tx.sub(ox.mul(step)), tz.add(oz.mul(step)));
    const hb = fetchH(tx.add(ox.mul(step)), tz.sub(oz.mul(step)));
    const wx = grid.x.add(tx.mul(grid.w));
    const wz = grid.y.add(tz.mul(grid.w));
    const y0 = hgt.x.add(h0.mul(hgt.y));
    // morph by 3D distance: 0 inside morph start, 1 at the level's range
    const d = length(vec3(wx, y0, wz).sub(cam.xyz));
    const range = lodp.x.mul(exp2(lod.mul(lodp.y))).toVar();
    const prev = select(lod.greaterThan(0.5), range.mul(exp2(lodp.y.negate())), float(0));
    const end = range.mul(lodp.w);
    const start = prev.add(end.sub(prev).mul(lodp.z));
    const k = clamp(d.sub(start).div(max(end.sub(start), 1e-3)), 0, 1);
    const h = mix(h0, ha.add(hb).mul(0.5), k);
    // skirt vertices (the grid's border repeated with uv.x + 2) hang a quarter of a vertex step below
    // their edge: levels meet exactly, but where a fine edge's midpoint meets a coarse edge (a
    // T-junction) the rasterizer can leave a pinhole; the skirt behind it fills it
    const skirt = select(TSL.uv().x.greaterThan(1.5), step.mul(grid.w).mul(0.25), float(0));
    return vec3(wx, hgt.x.add(h.mul(hgt.y)).sub(skirt), wz);
  })();
  // the material's colour, roughness and metalness scale the terrain's own (white, 1 and 0 by default)
  mat.metalnessNode = materialMetalness;

  // ── fragment ───────────────────────────────────────────────────────
  // normal x, z in R, G (0.5 = 0), linear filtered and mipmapped
  const normalMap = dataTex('terrainNormals', true, false);
  // sRGB-encoded data in a linear texture, decoded here (as the layers' albedo): the same on every backend
  const macroMap = dataTex('terrainMacro', true, false);
  const wp = positionWorld;
  // texel-centre uv over the height grid
  const gridUv = Fn(() => {
    const t = wp.xz.sub(grid.xy).mul(grid.z);
    return t.add(0.5).div(vec2(hgt.z.add(1), hgt.w.add(1)));
  }).once()();
  const worldNormal = Fn(() => {
    const e = texture(normalMap, gridUv).xy.mul(2).sub(1);
    return normalize(vec3(e.x, sqrt(max(float(1).sub(dot(e, e)), 0)), e.y));
  }).once()();
  const dist = length(wp.sub(cam.xyz));
  // Smooth hash noise avoids the periodic crosshatch of multiplied sines.
  const hash2 = (p) => fract(sin(dot(p, vec2(127.1, 311.7))).mul(43758.5453));
  const valueNoise = (p) => {
    const cell = floor(p), f = fract(p);
    const u = f.mul(f).mul(float(3).sub(f.mul(2)));
    return mix(mix(hash2(cell), hash2(cell.add(vec2(1, 0))), u.x),
      mix(hash2(cell.add(vec2(0, 1))), hash2(cell.add(vec2(1, 1))), u.x), u.y);
  };
  const variation = Fn(() => {
    const q = wp.xz.mul(0.008);
    const oblique = vec2(q.x.mul(0.8).sub(q.y.mul(0.6)), q.x.mul(0.6).add(q.y.mul(0.8)));
    const n = valueNoise(q).mul(0.65).add(valueNoise(oblique.mul(2.13).add(vec2(19.3, 7.1))).mul(0.35));
    return mix(float(1).sub(far.z), float(1).add(far.z), n);
  });
  const macroColor = Fn(() => sRGBTransferEOTF(texture(macroMap, gridUv).rgb).mul(variation())).once()();
  const viewNormal = (n) => normalize(cameraViewMatrix.mul(vec4(n, 0)).xyz);

  if (mode === 'far') {
    mat.colorNode = vec4(macroColor.mul(materialColor.rgb), 1);
    mat.roughnessNode = materialRoughness.mul(0.9);
    mat.normalNode = viewNormal(worldNormal);
    return;
  }

  const ctlMap = uintTex('terrainControl');
  // the layers: albedo + height (sRGB-encoded data, decoded here) and normal + roughness, one array
  // layer each
  const albedoArr = arrayTex('terrainAlbedo'), normalArr = arrayTex('terrainLayerNormals');
  const uvs0 = v4('terrainUv0');      // uv scale of layers 0..3 (repeats per metre)
  const uvs1 = v4('terrainUv1');      // 4..7
  const slope = v4('terrainSlope');   // triplanar from normal.y = x down to x - y; (unused); height blend sharpness

  const layerScale = (id) => {
    const a = select(id.lessThan(0.5), uvs0.x, select(id.lessThan(1.5), uvs0.y, select(id.lessThan(2.5), uvs0.z, uvs0.w)));
    const b = select(id.lessThan(4.5), uvs1.x, select(id.lessThan(5.5), uvs1.y, select(id.lessThan(6.5), uvs1.z, uvs1.w)));
    return select(id.lessThan(3.5), a, b);
  };
  // Terrain3D control bits from an RGBA8 texel (byte 0 = bits 31..24)
  const decode = (word) => {
    const c = word.r; // the control word (uint)
    const f = (shift, mask) => float(c.shiftRight(uint(shift)).bitAnd(uint(mask)));
    const base = min(f(27, 31), 7), over = min(f(22, 31), 7); // (5-bit ids; 8 layers)
    return { base, over, blend: f(14, 255).div(255), angle: f(10, 15), scl: f(7, 7) };
  };

  // Terrain3D's uv scale steps: 0 = 1x, then +20 % .. -60 %
  const scaleOf = (s) => select(s.lessThan(0.5), float(1), select(s.lessThan(4.5), float(1).add(s.mul(0.2)), float(1).sub(s.sub(4).mul(0.2))));

  // one layer sample, by explicit gradients (legal in non-uniform control flow on WebGPU)
  // Triangular stochastic blending: adjacent cells share the same three
  // random offsets, so scanned material detail stays continuous without a
  // square grid of identical repeated landmarks.
  const sampleLayer = (nrm, id, uv, gx, gy) => {
    const skew = vec2(uv.x.sub(uv.y.mul(0.57735027)), uv.y.mul(1.15470054));
    const cell = floor(skew), f = fract(skew), upper = f.x.add(f.y).greaterThan(1);
    const a = select(upper, cell.add(vec2(1, 1)), cell);
    const b = select(upper, cell.add(vec2(0, 1)), cell.add(vec2(1, 0)));
    const c = select(upper, cell.add(vec2(1, 0)), cell.add(vec2(0, 1)));
    const w = select(upper, vec3(f.x.add(f.y).sub(1), float(1).sub(f.x), float(1).sub(f.y)),
      vec3(float(1).sub(f.x).sub(f.y), f.x, f.y));
    const offset = (p) => fract(sin(vec2(dot(p, vec2(127.1, 311.7)), dot(p, vec2(269.5, 183.3)))).mul(43758.5453));
    const image = nrm ? normalArr : albedoArr;
    const tex = (p) => texture(image, uv.add(offset(p))).depth(int(id)).grad(gx, gy);
    const t = tex(a).mul(w.x).add(tex(b).mul(w.y)).add(tex(c).mul(w.z));
    return nrm ? t : vec4(sRGBTransferEOTF(t.rgb), t.a);
  };

  const rot = (uv, angle) => {
    const a = angle.mul(Math.PI * 2 / 16);
    const c = cos(a), s = sin(a);
    return vec2(uv.x.mul(c).sub(uv.y.mul(s)), uv.x.mul(s).add(uv.y.mul(c)));
  };

  // a layer's albedo (rgb + height in a) and normal-roughness at a world point, planar or (steep) triplanar;
  // g holds the derivatives, taken before any branch (WGSL allows them only in uniform control flow)
  const layerAt = (id, ctl, g) => {
    const sc = layerScale(id).mul(scaleOf(ctl.scl));
    const uv = rot(wp.xz.mul(sc), ctl.angle);
    const gx = rot(g.dx.mul(sc), ctl.angle), gy = rot(g.dy.mul(sc), ctl.angle);
    const alb = sampleLayer(false, id, uv, gx, gy).toVar();
    const nrm = sampleLayer(true, id, uv, gx, gy).toVar();
    // steep: blend in the two side projections, weighted by the normal (medium and up)
    If(g.tri.greaterThan(0), () => {
      const n = worldNormal;
      const ax = abs(n.x), az = abs(n.z);
      const wx2 = ax.div(ax.add(az).add(1e-4));
      const uvX = wp.zy.mul(sc), uvZ = wp.xy.mul(sc);
      const dxs = g.dx3.mul(sc), dys = g.dy3.mul(sc);
      const ax1 = sampleLayer(false, id, uvX, dxs.zy, dys.zy), az1 = sampleLayer(false, id, uvZ, dxs.xy, dys.xy);
      const nx1 = sampleLayer(true, id, uvX, dxs.zy, dys.zy), nz1 = sampleLayer(true, id, uvZ, dxs.xy, dys.xy);
      alb.assign(mix(alb, mix(az1, ax1, wx2), g.tri));
      nrm.assign(mix(nrm, mix(nz1, nx1, wx2), g.tri));
    });
    return { alb, nrm };
  };

  // height blend (Terrain3D's): the layer whose height (albedo alpha) stands out wins the blend
  const heightBlend = (a, b, t) => {
    const ha = a.alb.a.mul(float(1).sub(t)), hb = b.alb.a.mul(t);
    const m = max(ha, hb).sub(slope.w);
    const wa = max(ha.sub(m), 0), wb = max(hb.sub(m), 0);
    return wb.div(max(wa.add(wb), 1e-4));
  };
  const blendTexel = (c, g) => {
    const ctl = decode(c);
    const a = layerAt(ctl.base, ctl, g);
    const b = layerAt(ctl.over, ctl, g);
    const t = heightBlend(a, b, ctl.blend);
    return { alb: mix(a.alb, b.alb, t), nrm: mix(a.nrm, b.nrm, t) };
  };

  // the splat, into properties (declared at the top of the shader, so the
  // branches can write them): albedo + height, normal + roughness, and the
  // share of the macro colour (1 past the far distance)
  const pAlb = property('vec4', 'terrainAlb'), pNrm = property('vec4', 'terrainNrm');
  const shade = Fn(() => {
    const g = {
      dx: dFdx(wp.xz).toVar(), dy: dFdy(wp.xz).toVar(), dx3: dFdx(wp).toVar(), dy3: dFdy(wp).toVar(),
      // triplanar weight: 0 on gentle slopes and at low quality
      tri: select(far.w.greaterThan(0.5), clamp(slope.x.sub(worldNormal.y).div(max(slope.y, 1e-3)), 0, 1), float(0)).toVar(),
    };
    const fade = clamp(dist.sub(far.x).mul(far.y), 0, 1).toVar();
    pAlb.assign(vec4(0));
    pNrm.assign(vec4(0.5, 0.5, 1, 0.9));
    If(fade.lessThan(1), () => {
      const tcoord = wp.xz.sub(grid.xy).mul(grid.z);
      if (mode === 'high') {
        // four control texels around the point, bilinearly weighted
        const t = tcoord.sub(0.5);
        const i0 = floor(t), f = fract(t);
        const ld = (ix, iz) => textureLoad(ctlMap, ivec2(clamp(i0.x.add(ix), 0, hgt.z), clamp(i0.y.add(iz), 0, hgt.w)));
        const s00 = blendTexel(ld(0, 0), g), s10 = blendTexel(ld(1, 0), g);
        const s01 = blendTexel(ld(0, 1), g), s11 = blendTexel(ld(1, 1), g);
        pAlb.assign(mix(mix(s00.alb, s10.alb, f.x), mix(s01.alb, s11.alb, f.x), f.y));
        pNrm.assign(mix(mix(s00.nrm, s10.nrm, f.x), mix(s01.nrm, s11.nrm, f.x), f.y));
      } else {
        // two layers, smoothly weighted: the four control texels around the point are read (point
        // loads, no filtering), their bilinear weights summed per layer for the nearest texel's base
        // layer (A) and the strongest other layer among the four (B), and A and B blended by height
        // with that weight. Four layer reads, the same transitions as High's, cheaper.
        const t = tcoord.sub(0.5);
        const i0 = floor(t), f = fract(t);
        const ld = (ix, iz) => {
          const c = decode(textureLoad(ctlMap, ivec2(clamp(i0.x.add(ix), 0, hgt.z), clamp(i0.y.add(iz), 0, hgt.w))));
          return { base: c.base.toVar(), over: c.over.toVar(), blend: c.blend.toVar(), angle: c.angle, scl: c.scl };
        };
        const taps = [ld(0, 0), ld(1, 0), ld(0, 1), ld(1, 1)];
        const fx = f.x, fz = f.y, gx1 = float(1).sub(fx), gz1 = float(1).sub(fz);
        const w = [gx1.mul(gz1), fx.mul(gz1), gx1.mul(fz), fx.mul(fz)];
        // the nearest texel (the weights' largest)
        const nx = fx.greaterThan(0.5), nz = fz.greaterThan(0.5);
        const pick = (k) => select(nz, select(nx, k(taps[3]), k(taps[2])), select(nx, k(taps[1]), k(taps[0])));
        const near = { base: pick((c) => c.base), over: pick((c) => c.over), angle: pick((c) => c.angle), scl: pick((c) => c.scl) };
        const share = (c, L) => select(c.base.equal(L), float(1).sub(c.blend), float(0)).add(select(c.over.equal(L), c.blend, float(0)));
        const weight = (L) => share(taps[0], L).mul(w[0]).add(share(taps[1], L).mul(w[1])).add(share(taps[2], L).mul(w[2])).add(share(taps[3], L).mul(w[3]));
        const A = near.base.toVar();
        const wA = weight(A).toVar();
        const B = A.toVar(), wB = float(0).toVar();
        for (const cand of [near.over, taps[0].base, taps[1].base, taps[2].base, taps[3].base, taps[0].over, taps[3].over]) {
          const wc = weight(cand);
          const better = cand.notEqual(A).and(wc.greaterThan(wB));
          B.assign(select(better, cand, B));
          wB.assign(select(better, wc, wB));
        }
        const a = layerAt(A, near, g), b = layerAt(B, near, g);
        const tb = heightBlend(a, b, wB.div(max(wA.add(wB), 1e-4)));
        pAlb.assign(mix(a.alb, b.alb, tb));
        pNrm.assign(mix(a.nrm, b.nrm, tb));
      }
    });
    return fade;
  }).once()();

  // detail normal (tangent space, +Y up: Terrain3D's normal maps) on the macro normal, a UDN blend in world space
  const shadedNormal = Fn(() => {
    const n = worldNormal;
    const d = pNrm.xyz.mul(2).sub(1);
    const nd = normalize(vec3(n.x.add(d.x), n.y, n.z.sub(d.y)));
    return normalize(mix(nd, n, shade));
  })();

  mat.colorNode = vec4(mix(pAlb.rgb, macroColor, shade).mul(materialColor.rgb), 1);
  mat.roughnessNode = mix(pNrm.a, float(0.9), shade).mul(materialRoughness);
  mat.normalNode = viewNormal(shadedNormal);
}
