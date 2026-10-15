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
// Vertex stage: heights are uint16 in a nearest-filtered RGBA8 texture (high
// byte in R, low in G), read with textureLoad at integer texel coordinates,
// so every fetch is exact and equal on GL and WebGPU. Morphing is vertical:
// a vertex on an odd row or column of its node moves toward the height of
// the coarser level's triangle under it (the midpoint of the two even
// neighbours along its row, column, or the quad diagonal Box3D's height
// field uses), so it never moves sideways and the fully morphed patch is
// exactly the next level's surface: no cracks between levels.
//
// Fragment stage: normals from the baked normals beside the heights; colour and roughness from
// Terrain3D's control map layout (base and overlay layer ids and their blend
// in a uint32 a texel, here the four bytes of an RGBA8 texel, big end in R)
// over one layer atlas (each layer's albedo + height cell beside its normal +
// roughness cell: Terrain3D's
// packing), with triplanar sampling on steep slopes. Past the far distance
// only a baked macro colour and the normal map are read.
//
// Names after "ext." are what the C fill hook sets (terrain_render.c).

export async function applyTerrain({ TSL, THREE, mat, props, sentinel }, mode) {
  const {
    Fn, If, uniform, texture, textureLoad, sharedUniformGroup, positionLocal, positionWorld, cameraViewMatrix,
    vec2, vec3, vec4, float, ivec2, clamp, fract, floor, round, exp2, mix, max, min, abs, sqrt, length, normalize,
    smoothstep, dFdx, dFdy, dot, select, sin, cos, pow, property, materialColor, materialRoughness, materialMetalness, sRGBTransferEOTF,
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
  // heights in R, G (read with textureLoad: exact, the sampler's filtering never applies) and the
  // normal's x, z in B, A (sampled with linear filtering and mipmaps): one texture for both keeps
  // the object group within three.c's eight textures beside its own (shadow maps, environment, LUT)
  const heightMap = dataTex('terrainHeights', true, false);
  const heightTex = texture(heightMap); // one node, loaded and sampled from it

  // ── vertex: CDLOD position ─────────────────────────────────────────
  const fetchH = (x, z) => {
    const t = heightTex.load(ivec2(clamp(x, 0, hgt.z), clamp(z, 0, hgt.w)));
    return round(t.r.mul(255)).mul(256).add(round(t.g.mul(255)));
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
    return vec3(wx, hgt.x.add(h.mul(hgt.y)), wz);
  })();
  // the material's colour, roughness and metalness scale the terrain's own (white, 1 and 0 by default)
  mat.metalnessNode = materialMetalness;

  // ── fragment ───────────────────────────────────────────────────────
  // sRGB-encoded data in a linear texture, decoded here (as the layer atlas): the same on every backend
  const macroMap = dataTex('terrainMacro', true, false);
  const wp = positionWorld;
  // texel-centre uv over the height grid
  const gridUv = Fn(() => {
    const t = wp.xz.sub(grid.xy).mul(grid.z);
    return t.add(0.5).div(vec2(hgt.z.add(1), hgt.w.add(1)));
  }).once()();
  const worldNormal = Fn(() => {
    const e = heightTex.sample(gridUv).zw.mul(2).sub(1);
    return normalize(vec3(e.x, sqrt(max(float(1).sub(dot(e, e)), 0)), e.y));
  }).once()();
  const dist = length(wp.sub(cam.xyz));
  // cheap variation over the macro colour: two octaves of value-ish noise from sines (render only)
  const variation = Fn(() => {
    const q = wp.xz.mul(0.071);
    const n = sin(q.x.add(sin(q.y.mul(1.7)))).mul(sin(q.y.mul(1.3).add(sin(q.x.mul(0.9))))).mul(0.5).add(0.5);
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

  const ctlMap = dataTex('terrainControl', false, false);
  // one atlas for both maps (a texture binding fewer: three.c's WebGPU groups hold eight): layer i's
  // albedo + height in cell (i % cols, i / cols), its normal + roughness cols cells to the right. The
  // albedo is sRGB-encoded data, decoded here (the atlas itself is linear)
  const layerAtlas = dataTex('terrainLayers', true, false);
  const atlas = v4('terrainAtlas');   // layer columns, rows, tile share of a cell, gutter share of a cell
  const uvs0 = v4('terrainUv0');      // uv scale of layers 0..3 (repeats per metre)
  const uvs1 = v4('terrainUv1');      // 4..7
  const slope = v4('terrainSlope');   // triplanar from normal.y = x down to x - y; atlas max gradient; height blend sharpness

  const layerScale = (id) => {
    const a = select(id.lessThan(0.5), uvs0.x, select(id.lessThan(1.5), uvs0.y, select(id.lessThan(2.5), uvs0.z, uvs0.w)));
    const b = select(id.lessThan(4.5), uvs1.x, select(id.lessThan(5.5), uvs1.y, select(id.lessThan(6.5), uvs1.z, uvs1.w)));
    return select(id.lessThan(3.5), a, b);
  };
  // Terrain3D control bits from an RGBA8 texel (byte 0 = bits 31..24)
  const decode = (c) => {
    const R = round(c.r.mul(255)), G = round(c.g.mul(255)), B = round(c.b.mul(255)), A = round(c.a.mul(255));
    const b5 = floor(R.div(8));
    const o5 = R.sub(b5.mul(8)).mul(4).add(floor(G.div(64)));
    const base = min(b5, 7), over = min(o5, 7); // (5-bit ids; the atlas holds 8 layers)
    const blend = G.sub(floor(G.div(64)).mul(64)).mul(4).add(floor(B.div(64))).div(255);
    const angle = floor(B.div(4)).sub(floor(B.div(64)).mul(16));
    const scl = B.sub(floor(B.div(4)).mul(4)).mul(2).add(floor(A.div(128)));
    return { base, over, blend, angle, scl };
  };
  // Terrain3D's uv scale steps: 0 = 1x, then +20 % .. -60 %
  const scaleOf = (s) => select(s.lessThan(0.5), float(1), select(s.lessThan(4.5), float(1).add(s.mul(0.2)), float(1).sub(s.sub(4).mul(0.2))));

  // one layer sample from an atlas, by explicit gradients (legal in non-uniform control flow on WebGPU)
  const cells = vec2(atlas.x.mul(2), atlas.y);
  const atlasUv = (id, uv, nrm) => {
    const row = floor(id.div(atlas.x));
    const col = id.sub(row.mul(atlas.x)).add(nrm ? atlas.x : 0);
    return vec2(col, row).add(atlas.w).add(fract(uv).mul(atlas.z)).div(cells);
  };
  // gradients in cell units (cells are square in texels), kept under the gutter's mip, then to atlas uv
  const clampGrad = (gx, gy) => {
    const a = gx.mul(atlas.z), b = gy.mul(atlas.z);
    const m = max(length(a), length(b));
    const s = min(float(1), slope.z.div(max(m, 1e-8)));
    return [a.mul(s).div(cells), b.mul(s).div(cells)];
  };
  const sampleLayer = (nrm, id, uv, gx, gy) => {
    const [cx, cy] = clampGrad(gx, gy);
    const t = texture(layerAtlas, atlasUv(id, uv, nrm)).grad(cx, cy);
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
