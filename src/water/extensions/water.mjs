// three.c generator extension "water" (state standard+xwater and its light
// variants, depth+xwater for shadows): Water3D's surface (cartwheel/water.h,
// src/water/water.c fills the values), written in three.js r186 TSL.
//
// Vertex: up to four Gerstner waves (GPU Gems 1, chapter 1) move the flat
// grid; the phases come reduced from the CPU (the same numbers
// cw_waves_displace uses), so the drawn surface is the CPU's to float
// rounding. The analytic wave normal goes to the fragment stage.
//
// Fragment: r186's Water2 / Water2Mesh, on a MeshStandardMaterial so the
// scene's lights give the glints (and shadows land on the water): two
// scrolling normal maps blended by the two-phase flow (Water2's
// flowConfig), on the wave normal; the planar reflection sampled at
// screenUV.flipX() (r186's ReflectorNode) and the refraction at screenUV,
// both bent by the normal (Water2's offset); a Schlick Fresnel term mixes
// them into the emissive, the diffuse colour is black, and the
// environment's specular stands in for the reflection when there is no
// planar one (the C side sets envMapIntensity).
//
// Names after "ext." are what the C fill hook sets (water.c).
export const name = 'water';
export async function apply({ TSL, THREE, mat, props, sentinel }) {
  const {
    Fn, uniform, texture, sharedUniformGroup, positionLocal, positionWorld, cameraViewMatrix,
    modelWorldMatrix, screenUV, vec2, vec3, vec4, float, normalize, dot, max, min, mix, pow, abs, sin, cos, clamp,
    floor, length, smoothstep, varying, step, exp, output, sRGBTransferEOTF, materialColor, materialEmissive,
  } = TSL;

  const frame = sharedUniformGroup('waterFrame', 0, THREE.NodeUpdateType.RENDER);
  const v4 = (n) => {
    const s = [sentinel(), sentinel(), sentinel(), sentinel()];
    const u = uniform(new THREE.Vector4(...s)).setName(n);
    u.setGroup(frame);
    props['ext.' + n] = { kind: 'vec4', value: s };
    return u;
  };
  // per wave i: waterWave<i> = (dir x, dir z, k = 2 pi / wavelength, amplitude)
  const waves = [0, 1, 2, 3].map((i) => v4('waterWave' + i));
  const steep = v4('waterSteep');   // Q per wave (already shared out: sum of Q k A <= 1)
  const phase = v4('waterPhase');   // each wave's time phase, reduced to [0, 2 pi)
  const look = v4('waterLook');     // reflectivity F0, distortion, detail strength, normal map repeats per metre
  const tint = v4('waterTint');     // rgb: colour; w: reflection on (1) / off (0)
  const deep = v4('waterDeep');     // rgb: deep colour (the refraction when it is off); w: refraction on / off
  const flow = v4('waterFlow');     // flow offset 0, offset 1, half cycle, flow map on (1) / off (0)
  const flowDir = v4('waterFlowDir'); // flow x, flow z (without a map), rectangle size x, size z
  const dbg = v4('waterDebug');     // x: debug view (1 = height); y, z: height range; w: murk
  const shore = v4('waterShore');   // x: floor map on (1) / off (0); y: shore foam width (m); z: 1 / clarity (1 / m); w: crest foam amount
  const shallow = v4('waterShallow'); // rgb: shallow colour; w: the foam clock (s, mod 1000)
  const floorRect = v4('waterFloorRect'); // x, y: the rectangle the floor map covers (m)
  const cam = v4('waterCam');       // xyz: the camera's world position (three.c fills no cameraPosition); w: 1 when the view is drawn rows bottom-up (cartwheel's WebGPU scene target) but the planar targets top-down

  const tex = (n, linear, repeat, half) => {
    const t = half ? new THREE.DataTexture(new Uint16Array(16).fill(0x3c00), 2, 2, THREE.RGBAFormat, THREE.HalfFloatType)
      : new THREE.DataTexture(new Uint8Array(16).fill(128), 2, 2);
    t.name = 'ext.' + n;
    t.magFilter = linear ? THREE.LinearFilter : THREE.NearestFilter;
    t.minFilter = linear && repeat ? THREE.LinearMipmapLinearFilter : linear ? THREE.LinearFilter : THREE.NearestFilter;
    t.generateMipmaps = linear && repeat;
    t.wrapS = t.wrapT = repeat ? THREE.RepeatWrapping : THREE.ClampToEdgeWrapping;
    t.needsUpdate = true;
    return t;
  };
  const normalMap = tex('waterNormal', true, true, false);
  const flowMap = tex('waterFlowMap', true, false, false);
  // the planar targets: half float, linear, clamped, no mips (a RenderTarget's texture)
  const reflectMap = tex('waterReflection', true, false, true);
  const refractMap = tex('waterRefraction', true, false, true);
  const floorMap = tex('waterFloorMap', true, false, true);

  // ── vertex: Gerstner ───────────────────────────────────────────────
  // plain node expressions (no Fn: the shadow pass rebuilds the vertex stage from positionNode alone)
  const p0 = positionLocal;
  let pos = vec3(p0.x, float(0), p0.z);
  let nrm = vec3(0, 1, 0);
  const Q = [steep.x, steep.y, steep.z, steep.w];
  const PH = [phase.x, phase.y, phase.z, phase.w];
  for (let i = 0; i < 4; i++) {
    const w = waves[i];
    const th = w.z.mul(w.x.mul(p0.x).add(w.y.mul(p0.z))).sub(PH[i]);
    const c = cos(th), s = sin(th);
    const qa = Q[i].mul(w.w);
    pos = pos.add(vec3(qa.mul(w.x).mul(c), w.w.mul(s), qa.mul(w.y).mul(c)));
    const wa = w.z.mul(w.w);
    nrm = nrm.sub(vec3(w.x.mul(wa).mul(c), Q[i].mul(wa).mul(s), w.y.mul(wa).mul(c)));
  }
  mat.positionNode = pos;
  const waveNormalLocal = varying(nrm, 'vWaterNormal');
  const vRest = varying(vec2(p0.x, p0.z), 'vWaterRest');   // where the surface point rests (the floor map's coordinates)
  const vSurf = varying(pos.y, 'vWaterSurf');                // its height over the rest level
  const vBunch = varying(nrm.y, 'vWaterBunch');              // the wave normal's y: small where the surface bunches up (crests)

  // ── fragment ───────────────────────────────────────────────────────
  // Broad wave normals also need filtering once waves are smaller than a
  // pixel. Avoid far-grid triangle glints stretching toward the horizon.
  const swellWeight = float(1).sub(smoothstep(2000, 12000, length(cam.xyz.sub(positionWorld))));
  const wn = normalize(mix(vec3(0, 1, 0), normalize(modelWorldMatrix.mul(vec4(waveNormalLocal, 0)).xyz), swellWeight));
  const toEye = normalize(cam.xyz.sub(positionWorld));
  // Water2's two-phase flow over the normal maps
  const rest = positionWorld.xz;
  const mapFlow = texture(flowMap, rest.div(flowDir.zw).add(0.5)).rg.mul(2).sub(1);
  const fm = mix(flowDir.xy, mapFlow, flow.w);
  const f = vec2(fm.x.negate(), fm.y);
  const uvs = rest.mul(look.w);
  const n0 = texture(normalMap, uvs.add(f.mul(flow.x)));
  const n1 = texture(normalMap, uvs.mul(0.73).add(vec2(0.31, 0.57)).add(f.mul(flow.y)));
  const lerpF = abs(flow.z.sub(flow.x)).div(flow.z);
  const nc = mix(n0, n1, lerpF);
  // tangent-space map, +Z out of the surface (gen://noise out=normal): x along +x, y along -z
  const d = vec3(nc.r.mul(2).sub(1), nc.b.mul(2).sub(1), nc.g.mul(2).sub(1).negate());
  // Tiny repeating ripples alias at flight distances; keep the broad swell
  // and fade the detail before its repetitions become a visible grid.
  const detailWeight = float(1).sub(smoothstep(150, 900, length(cam.xyz.sub(positionWorld)))).mul(look.z);
  const n = normalize(vec3(wn.x.add(d.x.mul(detailWeight)), wn.y, wn.z.add(d.z.mul(detailWeight))));
  const theta = max(dot(toEye, n), 0);
  const fres = pow(float(1).sub(theta), 5).mul(float(1).sub(look.x)).add(look.x);
  const offset = n.xz.mul(look.y);
  const suv = vec2(screenUV.x, mix(screenUV.y, float(1).sub(screenUV.y), cam.w));
  const refl = texture(reflectMap, vec2(float(1).sub(suv.x), suv.y).add(offset)).rgb.mul(tint.w);
  // what is seen through the water, tinted, then lost into the deep colour by murk (no refraction: all deep colour)
  // depth over the baked floor (a map outside its rectangle, or none: deep)
  const uvF = vRest.div(floorRect.xy).add(0.5);
  const outside = step(0.5, max(abs(uvF.x.sub(0.5)), abs(uvF.y.sub(0.5))));
  const mapOn = shore.x.mul(float(1).sub(outside));
  const depth = mix(float(100), max(texture(floorMap, uvF).r.add(vSurf), 0), mapOn);
  // Beer-Lambert style: the shallow colour turns into the deep one with depth; murk acts only as the water deepens
  const absorb = mix(float(1), float(1).sub(exp(depth.negate().mul(shore.z))), mapOn);
  const body = mix(shallow.rgb, deep.rgb, absorb);
  const seen = mix(texture(refractMap, suv.add(offset)).rgb.mul(tint.rgb), body, dbg.w.mul(absorb));
  const refr = mix(body, seen, deep.w);
  // foam: where the water meets the floor, and on crests (the wave normal's bunching), broken up by the map's height noise
  const noise = texture(normalMap, uvs.mul(0.6).add(f.mul(flow.x))).a;
  const lumpy = smoothstep(0.3, 0.7, noise);
  const wob = sin(shallow.w.mul(1.4).add(noise.mul(6.283))).mul(0.5).add(0.5);
  const sw = max(shore.y, 0.001);
  const shoreFoam = float(1).sub(smoothstep(0, sw, depth.add(noise.sub(0.5).mul(sw).mul(0.9)).sub(wob.mul(sw).mul(0.35)))).mul(mapOn).mul(step(0.001, shore.y));
  const thr = float(0.62).sub(shore.w.mul(0.5));
  const farFade = float(1).sub(smoothstep(200, 700, length(cam.xyz.sub(positionWorld))));
  const crestFoam = smoothstep(thr, thr.add(0.12), float(1).sub(vBunch).add(noise.sub(0.5).mul(0.25))).mul(step(0.001, shore.w)).mul(lumpy).mul(farFade);
  const foam = clamp(max(shoreFoam.mul(mix(0.55, 1, lumpy)), crestFoam), 0, 1);
  const emit = mix(refr, refl, fres);
  // the material colour (black: no diffuse; the C side keeps it so) and emissive (white) stay inputs
  mat.colorNode = vec4(materialColor.rgb.add(vec3(0.9, 0.92, 0.95).mul(foam)), 1);
  mat.normalNode = normalize(cameraViewMatrix.mul(vec4(n, 0)).xyz);
  mat.emissiveNode = emit.mul(materialEmissive).mul(float(1).sub(foam));
  // debug view HEIGHT: the world height over the range as 16 bits in R and G, pre-decoded so
  // that the sRGB output transfer (tone mapping NONE) lands each on its exact byte
  const hn = clamp(positionWorld.y.sub(dbg.y).div(dbg.z.sub(dbg.y)), 0, 1).mul(65535);
  const hi = floor(hn.div(256)), lo = floor(hn.sub(hi.mul(256)));
  const enc = sRGBTransferEOTF(vec3(hi.div(255), lo.div(255), 1));
  mat.outputNode = mix(output, vec4(enc, 1), min(dbg.x, 1));
}
