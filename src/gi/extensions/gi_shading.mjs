// GI shading for three.c's extension programs, written in three.js r186 TSL.
// Not an extension: gi.mjs (the "gi" shading part, cartwheel/shading.h)
// imports it.
//
// Reflection probes (cartwheel/gi.h, ReflectionProbe3D). Each draw carries
// up to two probes, picked on the CPU for the object (src/gi/reflection_probe.c fills the
// values below per draw; the material's envMapRotation, which r186's PMREMNode
// applies, should stay zero). A probe is a PMREM in r186's cube-UV layout
// (PMREMGenerator.fromScene with size, position and its own target), sampled
// by r186's own PMREMNode (its flip, rotation and textureCubeUV), given the
// probe's size values in place of its texture's, so two probes of different
// sizes can be bound.
//
//   - Box projection: r186's getParallaxCorrectNormal (the ray from the
//     surface along the reflection, cut by the probe's box), measured from
//     the capture point rather than the box centre so a probe captured off
//     centre (Godot's origin_offset) still lines up; with the capture at the
//     centre it is r186's function exactly.
//   - Blending (Godot's ReflectionProbe): a probe's weight is 1 inside its box
//     and falls to 0 over blend_distance at its faces, per pixel. The probes'
//     reflections are averaged by weight, and the sky's (the material's
//     envMap or scene.environment, as r186 would use) fills what the probes'
//     total weight leaves.
//   - Diffuse: a probe also gives the ambient light (r186's irradiance
//     context: the blurriest level along the normal, not box projected, as
//     Godot does), weighted the same way, unless its flags say not to.
//
// The values the C side sets, by name ("ext." + name):
//   giA0, giB0  vec4  box centre (world), blend distance
//   giA1, giB1  vec4  box half size, intensity (0 = no probe in this slot)
//   giA2, giB2  vec4  capture point (world), flags (1 box projection, 2 ambient)
//   giA3, giB3  vec4  cube-UV texel width, texel height, max mip, 0
//   giProbeA, giProbeB  the probes' PMREM textures (half float, linear, clamped)

export function giShading({ TSL, THREE, mat, props, sentinel }) {
  const { Fn, If, uniform, vec3, float, abs, clamp, fract, max, min, mix, normalize, select, positionWorld, normalWorld, pmremTexture } = TSL;

  // ── the probes' values ──
  const v4 = (name) => {
    const s = [sentinel(), sentinel(), sentinel(), sentinel()];
    const u = uniform(new THREE.Vector4(...s)).setName(name);
    props['ext.' + name] = { kind: 'vec4', value: s };
    return u;
  };
  const probeTex = (name) => {
    // as a PMREM target's texture: half float RGBA, linear, no mipmaps, clamped
    const t = new THREE.DataTexture(new Uint16Array(4 * 4 * 4).fill(0x3c00), 4, 4, THREE.RGBAFormat, THREE.HalfFloatType);
    t.name = 'ext.' + name;
    // a PMREM target's texture as r186 sees one: cube-UV, drawn by the GPU
    t.mapping = THREE.CubeUVReflectionMapping;
    t.isRenderTargetTexture = true;
    t.magFilter = t.minFilter = THREE.LinearFilter;
    t.generateMipmaps = false;
    t.wrapS = t.wrapT = THREE.ClampToEdgeWrapping;
    t.needsUpdate = true;
    return t;
  };
  const probe = (k) => ({ c: v4(`gi${k}0`), h: v4(`gi${k}1`), o: v4(`gi${k}2`), s: v4(`gi${k}3`), tex: probeTex(`giProbe${k}`) });
  const A = probe('A'), B = probe('B');

  // Godot's blend: 1 inside, 0 at the faces, linear over blend_distance
  const weight = (P) => {
    const d = P.h.xyz.sub(abs(positionWorld.sub(P.c.xyz)));
    const w = clamp(min(min(d.x, d.y), d.z).div(max(P.c.w, 1e-4)), 0, 1);
    return select(P.h.w.greaterThan(0), w, float(0));
  };
  // r186's getParallaxCorrectNormal, from the capture point
  const boxProject = (P, dir) => {
    const n = normalize(dir);
    const rbmax = P.c.xyz.add(P.h.xyz).sub(positionWorld).div(n);
    const rbmin = P.c.xyz.sub(P.h.xyz).sub(positionWorld).div(n);
    const t = vec3(select(n.x.greaterThan(0), rbmax.x, rbmin.x), select(n.y.greaterThan(0), rbmax.y, rbmin.y),
      select(n.z.greaterThan(0), rbmax.z, rbmin.z));
    const hit = positionWorld.add(n.mul(min(min(t.x, t.y), t.z)));
    return hit.sub(P.o.xyz);
  };
  const flag = (P, bit) => fract(P.o.w.div(bit * 2)).greaterThanEqual(0.5);

  // r186's EnvironmentNode builds envNode in two contexts: radiance (getUV
  // the world reflection, getTextureLevel the roughness) and irradiance
  // (getUV the world normal, level 1)
  mat.envNode = Fn((builder) => {
    const ctx = builder.context;
    const diffuse = ctx.getUV() === normalWorld;
    // everything the contexts give is evaluated here, before any branch: a node first built inside
    // an If is assigned inside it, and the sky sample below would read it unassigned
    const envTex = builder.material.envMap || (builder.scene && builder.scene.environment);
    const sky = (envTex ? pmremTexture(envTex).rgb : vec3(0)).toVar();
    const dir = ctx.getUV().toVar();
    const level = float(ctx.getTextureLevel()).toVar();
    const one = (P) => {
      const d = diffuse ? dir : select(flag(P, 1), boxProject(P, dir), dir);
      // r186's own PMREMNode samples it (flip, rotation, textureCubeUV), with the probe's size values in
      // place of the ones it would take from the texture's image
      const pm = pmremTexture(P.tex, d, level);
      pm.updateFromTexture = function (t) { this._texture.value = t; };
      pm._width = P.s.x;
      pm._height = P.s.y;
      pm._maxMip = P.s.z;
      return pm.rgb.mul(P.h.w);
    };
    let wA = weight(A), wB = weight(B);
    if (diffuse) {
      wA = select(flag(A, 2), wA, float(0));
      wB = select(flag(B, 2), wB, float(0));
    }
    const wa = wA.toVar(), wb = wB.toVar();
    const sum = wa.add(wb).toVar();
    const probes = vec3(0).toVar();
    If(wa.greaterThan(0), () => { probes.addAssign(one(A).mul(wa)); });
    If(wb.greaterThan(0), () => { probes.addAssign(one(B).mul(wb)); });
    return mix(sky, probes.div(max(sum, 1e-4)), min(sum, 1));
  })();

  gridShading({ TSL, THREE, mat, props, sentinel });
  gbufferOutput({ TSL, THREE, mat, props, sentinel });
}

// The screen-space passes' G-buffer (src/gi/ssr.c): the same programs draw
// the scene a second time into a half-resolution target with giMode.x = 1,
// and then write what r186's SSR and SSGI examples take from their MRT
// attachments instead of the lit colour: the view-space normal (after normal
// maps) octahedral-encoded in r and g, roughness in b, metalness in a; with
// giMode.x = 2 the diffuse colour times 1 - metalness (SSGI's albedo; alpha 1
// marks a surface). giMode.x = 0 is the normal
// draw.
function gbufferOutput({ TSL, THREE, mat, props, sentinel }) {
  const { Fn, If, uniform, vec2, vec4, abs, sign, select, float, output, normalView, roughness, metalness, diffuseColor } = TSL;
  const s = [sentinel(), sentinel(), sentinel(), sentinel()];
  const mode = uniform(new THREE.Vector4(...s)).setName('giMode');
  props['ext.giMode'] = { kind: 'vec4', value: s };
  const before = mat.outputNode; // (a part applied earlier may have set one)
  mat.outputNode = Fn(() => {
    const o = vec4(before ?? output).toVar();
    const m = mode.x.toVar();
    If(m.greaterThan(0.5), () => {
      If(m.lessThan(1.5), () => {
        const n = normalView.normalize().toVar();
        const p = n.xy.div(abs(n.x).add(abs(n.y)).add(abs(n.z))).toVar();
        const q = select(n.z.lessThan(0), vec2(float(1).sub(abs(p.y)), float(1).sub(abs(p.x))).mul(vec2(sign(p.x), sign(p.y))), p);
        o.assign(vec4(q, roughness, metalness));
      }).Else(() => {
        o.assign(vec4(diffuseColor.rgb.mul(float(1).sub(metalness)), 1));
      });
    });
    return o;
  })();
}

// The light probe grid (LightProbeGrid3D): r186's LightProbeGridNode, which
// adds the grid's L2 SH irradiance at the surface to the lighting's
// irradiance. three.c has no 3D textures, so the atlas is 2D: probe (ix, iy,
// iz)'s group g is texel (iz * nx + ix, g * ny + iy). Each group is read for
// the two z slices around the point with the hardware's bilinear filter in x
// and y (sample coordinates clamped to texel centres inside the slice, as
// r186's uvw remap does), and the slices are mixed by hand: 14 fetches where
// r186's 3D texture takes 7.
//
//   giGridMin  vec4  the grid's min corner, intensity (0 = no grid)
//   giGridMax  vec4  the grid's max corner, normal offset (in probe spacings: r186's 0.5)
//   giGridRes  vec4  probes along x, y, z, 0
//   giGrid     the atlas (half float RGBA, linear, clamped)
function gridShading({ TSL, THREE, mat, props, sentinel }) {
  const { Fn, If, uniform, texture, vec2, vec3, vec4, float, floor, clamp, min, max, mix, positionWorld, normalWorld,
    sharedUniformGroup } = TSL;
  const v4 = (name) => {
    const s = [sentinel(), sentinel(), sentinel(), sentinel()];
    const u = uniform(new THREE.Vector4(...s)).setName(name);
    props['ext.' + name] = { kind: 'vec4', value: s };
    return u;
  };
  const gmin = v4('giGridMin'), gmax = v4('giGridMax'), gres = v4('giGridRes');
  const atlas = new THREE.DataTexture(new Uint16Array(4 * 4 * 4), 4, 4, THREE.RGBAFormat, THREE.HalfFloatType);
  atlas.name = 'ext.giGrid';
  atlas.magFilter = atlas.minFilter = THREE.LinearFilter;
  atlas.generateMipmaps = false;
  atlas.wrapS = atlas.wrapT = THREE.ClampToEdgeWrapping;
  atlas.needsUpdate = true;

  const irradiance = Fn(() => {
    const result = vec3(0).toVar();
    If(gmin.w.greaterThan(0), () => {
      const res = gres.xyz.toVar();
      const lo = gmin.xyz.toVar(), hi = gmax.xyz.toVar();
      const range = hi.sub(lo);
      const spacing = range.div(max(res.sub(1), 1));
      // offset along the normal, then the position in probe units
      const p = positionWorld.add(normalWorld.mul(spacing).mul(gmax.w));
      const g = clamp(p.sub(lo).div(range), 0, 1).mul(res.sub(1)).toVar();
      const z0 = floor(min(g.z, res.z.sub(1))).toVar();
      const z1 = min(z0.add(1), res.z.sub(1));
      const fz = g.z.sub(z0);
      const size = vec2(res.x.mul(res.z), res.y.mul(7));
      const slice = (z, k) => {
        const uv = vec2(z.mul(res.x).add(g.x).add(0.5), float(k).mul(res.y).add(g.y).add(0.5)).div(size);
        return texture(atlas, uv).level(0);
      };
      const s = [];
      for (let k = 0; k < 7; k++) s.push(mix(slice(z0, k), slice(z1, k), fz));
      for (let k = 0; k < 7; k++) s[k] = s[k].toVar();
      const sh = [
        s[0].xyz, vec3(s[0].w, s[1].x, s[1].y), vec3(s[1].z, s[1].w, s[2].x), s[2].yzw,
        s[3].xyz, vec3(s[3].w, s[4].x, s[4].y), vec3(s[4].z, s[4].w, s[5].x), s[5].yzw, s[6].xyz,
      ];
      // r186 getShIrradianceAt, on the nine values (its array form trips Tint's WGSL lowering)
      const n = normalWorld.toVar();
      const x = n.x, y = n.y, z = n.z;
      const irr = sh[0].mul(0.886227)
        .add(sh[1].mul(2.0 * 0.511664).mul(y)).add(sh[2].mul(2.0 * 0.511664).mul(z)).add(sh[3].mul(2.0 * 0.511664).mul(x))
        .add(sh[4].mul(2.0 * 0.429043).mul(x).mul(y)).add(sh[5].mul(2.0 * 0.429043).mul(y).mul(z))
        .add(sh[6].mul(z.mul(z).mul(0.743125).sub(0.247708))).add(sh[7].mul(2.0 * 0.429043).mul(x).mul(z))
        .add(sh[8].mul(0.429043).mul(x.mul(x).sub(y.mul(y))));
      result.assign(irr.max(vec3(0)).mul(gmin.w));
    });
    return result;
  })();

  // a lighting part: one more LightingNode in the material's lightings. (The capture's material may
  // be a plain one the renderer turns into a node material by copying its own properties, so this is
  // an own property calling whatever came before: the prototype's, or another part's.)
  const prev = Object.prototype.hasOwnProperty.call(mat, 'setupMaterialLightings') ? mat.setupMaterialLightings : null;
  mat.setupMaterialLightings = function (builder) {
    const base = prev ? prev.call(this, builder) : Object.getPrototypeOf(this).setupMaterialLightings.call(this, builder);
    return [...base, new THREE.IrradianceNode(irradiance)];
  };
}
