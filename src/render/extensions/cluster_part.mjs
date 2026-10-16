// The "cluster" shading part (composed into extension states x<parts>, e.g.
// standard+l1001+xcluster): clustered omni and spot lights, shaded as three.js
// r186's ClusteredLightsNode shades its point lights.
//
// The CPU bins the lights into a froxel grid every frame (src/render/froxel.c,
// cartwheel/froxel.h) and src/render/lighting.c fills what this part declares
// (its fill hook sets every "ext." name below). r186 bins on the GPU with a
// compute pass (WebGPU only); binning on the CPU gives GLES3 and WebGPU the
// same lists. The per-fragment loop is r186's: each light goes through
// LightsNode.setupDirectLight with directPointLight (r186's distance
// attenuation and its BRDF for the material's lighting model), skipped past
// its cutoff distance. Spot lights (which r186 does not cluster) add
// SpotLightNode's cone: smoothstep(coneCos, penumbraCos, angleCos).
//
// Data (all read with textureLoad, exact on both backends):
//   ext.clusterLights   RGBA32F, one column a light, 4 rows:
//                       0: view-space position, cutoff distance (0 = none)
//                       1: colour x intensity, decay exponent
//                       2: view-space direction from the target to the light, cone cos (< -1: an omni light)
//                       3: penumbra cos, shadow (0 none, 1 spot, 2 omni), bias, normal bias
//                       4..7: spot shadow: world -> atlas uv (before r186's y flip) and depth, one column a row
//                       4: omni shadow: world position, near; 5: far
//                       8: the shadow's tile in the atlas (texels, rows top-down): x, y, face size; PCF radius (texels)
//                       9: shadow intensity
//   ext.clusterShadows  R32F (WebGPU: a copy of the depth; GLES: the depth texture), the atlas (src/render/lighting_atlas.c), read with textureLoad:
//                       r186's PCF filter (five Vogel-disk taps rotated by interleaved gradient
//                       noise), each tap a 2 x 2 bilinear depth compare done by hand (WGSL forbids
//                       textureSampleCompare in the per-light loop, a branch on the fragment's data)
//   ext.clusterRecords  R32UI, one texel a froxel (froxel f at (f % width, f / width)):
//                       first item << 8 | count
//   ext.clusterItems    R32UI, two 16-bit light indices a texel (item k: texel k >> 1, its low half when k is even)
//   ext.clusterGrid     vec4: froxels x, y, z, records texture width
//   ext.clusterDepth    vec4: z slice = floor(log(view depth) * x + y); z: items texture width; w: atlas size
// The froxel of a fragment comes from its view position through the camera's
// projection (froxel x = floor((ndc.x * 0.5 + 0.5) * nx), y the same from the
// bottom row up), so a render target's orientation never matters.

export const part = 'cluster';
// lighting parts come after surface parts (they read the final normal) and before indirect ones
export const order = 200;

export const LIGHT_ROWS = 10;

// the omni shadow faces (src/render/lighting_atlas.c FACE_DIR / FACE_UP): each face camera's
// right (X), up (Y) and view direction (D), as three.js lookAt builds them
const FACE_DIR = [[1, 0, 0], [-1, 0, 0], [0, 1, 0], [0, -1, 0], [0, 0, 1], [0, 0, -1]];
const FACE_UP = [[0, -1, 0], [0, -1, 0], [0, 0, 1], [0, 0, -1], [0, -1, 0], [0, -1, 0]];
const cross = (a, b) => [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]];
const FACES = FACE_DIR.map((d, f) => {
  const z = d.map((v) => -v), x = cross(FACE_UP[f], z), y = cross(z, x);
  return { x, y, d };
});
// the material kinds with a lighting model (direct lights)
const LIT_KINDS = new Set(['lambert', 'phong', 'standard', 'physical', 'toon']);

/** the froxel of the fragment being shaded, and its record (TSL), for any part that reads the grid */
export function froxelRecord(TSL, { records, grid, depth }) {
  const { cameraProjectionMatrix, positionView, vec4, int, float, floor, clamp, log, max, ivec2, textureLoad } = TSL;
  const clip = cameraProjectionMatrix.mul(vec4(positionView, 1));
  const ndc = clip.xy.div(clip.w);
  const tx = clamp(floor(ndc.x.mul(0.5).add(0.5).mul(grid.x)), 0, grid.x.sub(1));
  const ty = clamp(floor(ndc.y.mul(0.5).add(0.5).mul(grid.y)), 0, grid.y.sub(1));
  const vz = max(positionView.z.negate(), float(1e-4));
  const tz = clamp(floor(log(vz).mul(depth.x).add(depth.y)), 0, grid.z.sub(1));
  const f = int(tx).add(int(grid.x).mul(int(ty).add(int(grid.y).mul(int(tz))))).toVar();
  const w = int(grid.w);
  return textureLoad(records, ivec2(f.mod(w), f.div(w))).r;
}

export async function apply(ctx, next) {
  const { TSL, THREE, mat, props, sentinel, kind } = ctx;
  // only lit surfaces gather lights; a caster (depth+x...) carries nothing of this part
  if (kind === 'depth' || !LIT_KINDS.has(kind)) return next ? next(ctx) : undefined;
  const {
    Fn, If, Loop, uniform, sharedUniformGroup, textureLoad, directPointLight, positionView, positionWorld, normalWorld,
    screenCoordinate, interleavedGradientNoise, vogelDiskSample,
    int, uint, float, vec2, vec3, vec4, mat4, ivec2, smoothstep, dot, normalize, select, abs, floor, clamp, mix, min, max,
  } = TSL;

  const frame = sharedUniformGroup('clusterFrame', 0, THREE.NodeUpdateType.RENDER);
  const v4 = (name) => {
    const s = [sentinel(), sentinel(), sentinel(), sentinel()];
    const u = uniform(new THREE.Vector4(...s)).setName(name);
    u.setGroup(frame);
    props['ext.' + name] = { kind: 'vec4', value: s };
    return u;
  };
  const grid = v4('clusterGrid');
  const depth = v4('clusterDepth');

  const uintTex = (name) => {
    const t = new THREE.DataTexture(new Uint32Array(4), 2, 2, THREE.RedIntegerFormat, THREE.UnsignedIntType);
    t.name = 'ext.' + name;
    t.needsUpdate = true;
    return t;
  };
  const records = uintTex('clusterRecords');
  const items = uintTex('clusterItems');
  // (a float texture: compatibility-mode WebGPU reads depth only through a comparison sampler,
  // which WGSL allows only outside the light loop, so the engine copies the atlas into R32Float
  // there; on GLES it binds the depth texture itself, read the same way)
  const atlas = new THREE.DataTexture(new Float32Array(16), 4, 4, THREE.RedFormat, THREE.FloatType);
  atlas.name = 'ext.clusterShadows';
  atlas.needsUpdate = true;
  const lights = new THREE.DataTexture(new Float32Array(2 * LIGHT_ROWS * 4), 2, LIGHT_ROWS, THREE.RGBAFormat, THREE.FloatType);
  lights.name = 'ext.clusterLights';
  lights.needsUpdate = true;

  // one tap of the filter: the 2 x 2 texels around pt (atlas texels, rows top-down), each
  // compared with z (LessEqual: lit when z <= the stored depth) and blended bilinearly, as a
  // LINEAR comparison sampler does; the texels clamped into the tile (x0, y0, x1, y1)
  const tap = (pt, z, tile) => {
    const q = pt.sub(0.5);
    const i0 = floor(q).toVar();
    const fr = q.sub(i0);
    const ld = (dx, dy) => {
      const x = int(clamp(i0.x.add(dx), tile.x, tile.z.sub(1)));
      const y = int(clamp(i0.y.add(dy), tile.y, tile.w.sub(1)));
      return select(z.lessThanEqual(textureLoad(atlas, ivec2(x, y)).x), float(1), float(0));
    };
    return mix(mix(ld(0, 0), ld(1, 0), fr.x), mix(ld(0, 1), ld(1, 1), fr.x), fr.y);
  };
  // a clustered light's shadow (row 3 is d: type in d.y, bias d.z, normal bias d.w)
  const lightShadow = (li, d) => {
    const r8 = textureLoad(lights, ivec2(li, 8)).toVar();
    const r9 = textureLoad(lights, ivec2(li, 9));
    const n = depth.w;
    const wp = positionWorld.add(normalWorld.mul(d.w)).toVar();
    const coord = vec3(0).toVar(); // atlas texel x, y (rows top-down) and depth
    const tile = vec4(0).toVar();  // the face's texels: x0, y0, x1, y1
    If(d.y.lessThan(1.5), () => {
      // spot: r186 ShadowNode (the matrix holds the tile; y = 1 - y; the depth bias added)
      const m = mat4(textureLoad(lights, ivec2(li, 4)), textureLoad(lights, ivec2(li, 5)),
        textureLoad(lights, ivec2(li, 6)), textureLoad(lights, ivec2(li, 7)));
      const p = m.mul(vec4(wp, 1)).toVar();
      const w = p.w;
      coord.assign(vec3(p.x.div(w).mul(n), float(1).sub(p.y.div(w)).mul(n), p.z.div(w).add(d.z)));
      tile.assign(vec4(r8.x, r8.y, r8.x.add(r8.z), r8.y.add(r8.z)));
    }).Else(() => {
      // omni: the cube face the fragment is in (the major axis), its 90 degree projection
      const r4 = textureLoad(lights, ivec2(li, 4)).toVar();
      const far = textureLoad(lights, ivec2(li, 5)).x;
      const v = wp.sub(r4.xyz).toVar();
      const av = abs(v);
      const face = select(av.x.greaterThanEqual(av.y).and(av.x.greaterThanEqual(av.z)), select(v.x.greaterThan(0), float(0), float(1)),
        select(av.y.greaterThanEqual(av.z), select(v.y.greaterThan(0), float(2), float(3)), select(v.z.greaterThan(0), float(4), float(5)))).toVar();
      const pick = (k) => FACES.slice(1).reduce((acc, F, j) => select(face.equal(float(j + 1)), vec3(...F[k]), acc), vec3(...FACES[0][k]));
      const dd = dot(v, pick('d')).toVar();
      const nx = dot(v, pick('x')).div(dd), ny = dot(v, pick('y')).div(dd);
      // three.js PerspectiveCamera depth (GL clip z), into [0, 1] as the shadow maps store it
      const near = r4.w;
      const zn = far.add(near).div(far.sub(near)).mul(dd).sub(far.mul(near).mul(2).div(far.sub(near))).div(dd);
      const s = r8.z;
      const x0 = r8.x.add(face.mod(3).mul(s)), y0 = r8.y.add(floor(face.div(3)).mul(s));
      coord.assign(vec3(x0.add(nx.mul(0.5).add(0.5).mul(s)), y0.add(float(1).sub(ny.mul(0.5).add(0.5)).mul(s)), zn.mul(0.5).add(0.5).add(d.z)));
      tile.assign(vec4(x0, y0, x0.add(s), y0.add(s)));
    });
    // r186 PCFShadowFilter: five Vogel-disk taps, rotated per pixel by interleaved gradient noise
    const phi = interleavedGradientNoise(screenCoordinate.xy).mul(6.28318530718);
    // (a loop, not five copies: each textureLoad site of a texture costs three.c a flipY slot)
    const sum = float(0).toVar();
    Loop(5, ({ i }) => {
      sum.addAssign(tap(coord.xy.add(vogelDiskSample(int(i), int(5), phi).mul(r8.w)), coord.z, tile));
    });
    // r186's frustum test: outside the map (or past its far plane) is lit
    const inside = coord.x.greaterThanEqual(tile.x).and(coord.x.lessThanEqual(tile.z)).and(coord.y.greaterThanEqual(tile.y))
      .and(coord.y.lessThanEqual(tile.w)).and(coord.z.lessThanEqual(1));
    return mix(float(1), select(inside, sum.mul(1 / 5), float(1)), r9.x);
  };

  class ClusterLightingNode extends THREE.LightingNode {
    static get type() { return 'ClusterLightingNode'; }
    setup(builder) {
      const lightsNode = builder.lightsNode;
      const reflected = builder.context.reflectedLight;
      // declared before the loop (r186's ClusteredLightsNode.setupLights does the same)
      reflected.directDiffuse.toStack();
      reflected.directSpecular.toStack();
      Fn(() => {
        const rec = froxelRecord(TSL, { records, grid, depth }).toVar();
        const first = int(rec.shiftRight(uint(8))).toVar();
        const count = int(rec.bitAnd(uint(255))).toVar();
        const iw = int(depth.z);
        Loop({ start: int(0), end: count, type: 'int', condition: '<' }, ({ i }) => {
          const k = first.add(i);
          const word = textureLoad(items, ivec2(k.shiftRight(1).mod(iw), k.shiftRight(1).div(iw))).r;
          const li = int(word.shiftRight(uint(k.bitAnd(1).mul(16))).bitAnd(uint(0xffff))).toVar();
          const a = textureLoad(lights, ivec2(li, 0)).toVar();
          const lightVector = a.xyz.sub(positionView).toVar();
          const cutoff = a.w;
          If(cutoff.equal(0).or(dot(lightVector, lightVector).lessThanEqual(cutoff.mul(cutoff))), () => {
            const b = textureLoad(lights, ivec2(li, 1));
            const c = textureLoad(lights, ivec2(li, 2));
            const d = textureLoad(lights, ivec2(li, 3));
            const point = directPointLight({ color: b.rgb, lightVector, cutoffDistance: cutoff, decayExponent: b.w });
            // a spot light's cone (r186 SpotLightNode.getSpotAttenuation); omni lights carry cone cos -2
            const cone = select(c.w.lessThan(-1.5), float(1), smoothstep(c.w, d.x, dot(normalize(lightVector), c.xyz)));
            const shadow = float(1).toVar();
            If(d.y.greaterThan(0.5), () => shadow.assign(lightShadow(li, d)));
            lightsNode.setupDirectLight(builder, this, { lightDirection: point.lightDirection, lightColor: point.lightColor.mul(cone).mul(shadow) });
          });
        });
      }, 'void')();
    }
  }

  const node = new ClusterLightingNode();
  // the renderer draws a NodeMaterial copied from mat (NodeLibrary.fromMaterial copies its own
  // properties), so the override reaches the copy and runs with it as `this`; an earlier
  // lighting part's override runs first
  const prev = mat.setupMaterialLightings;
  mat.setupMaterialLightings = function (builder) {
    const base = prev ? prev.call(this, builder) : Object.getPrototypeOf(this).setupMaterialLightings.call(this, builder);
    return [...base, node];
  };
  return next ? next(ctx) : undefined;
}
