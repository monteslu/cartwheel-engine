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
//                       3: penumbra cos, 0, 0, 0
//   ext.clusterRecords  R32UI, one texel a froxel (froxel f at (f % width, f / width)):
//                       first item << 8 | count
//   ext.clusterItems    R32UI, two 16-bit light indices a texel (item k: texel k >> 1, its low half when k is even)
//   ext.clusterGrid     vec4: froxels x, y, z, records texture width
//   ext.clusterDepth    vec4: z slice = floor(log(view depth) * x + y); z: items texture width; w: unused
// The froxel of a fragment comes from its view position through the camera's
// projection (froxel x = floor((ndc.x * 0.5 + 0.5) * nx), y the same from the
// bottom row up), so a render target's orientation never matters.

export const part = 'cluster';
// lighting parts come after surface parts (they read the final normal) and before indirect ones
export const order = 200;

export const LIGHT_ROWS = 4;
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
    Fn, If, Loop, uniform, sharedUniformGroup, textureLoad, directPointLight, positionView,
    int, uint, float, ivec2, smoothstep, dot, normalize, select,
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
  const lights = new THREE.DataTexture(new Float32Array(2 * LIGHT_ROWS * 4), 2, LIGHT_ROWS, THREE.RGBAFormat, THREE.FloatType);
  lights.name = 'ext.clusterLights';
  lights.needsUpdate = true;

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
            lightsNode.setupDirectLight(builder, this, { lightDirection: point.lightDirection, lightColor: point.lightColor.mul(cone) });
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
