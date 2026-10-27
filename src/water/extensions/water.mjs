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
    floor, length, smoothstep, varying, step, exp, sqrt, output, sRGBTransferEOTF, materialColor, materialEmissive,
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
  const sun = v4('waterSun');       // xyz: the direction toward the sun (world); w: the waves' summed amplitude
  const sunCol = v4('waterSunCol'); // rgb: the sun's colour x energy (0 without a sky); w: scatter strength
  const rip = v4('waterRipple');    // x: ripple normal strength (0: no grid); y, z: the grid's size (m); w: glint
  const ripR = v4('waterRippleRect'); // x, y: the grid's centre (water-local x, z); z, w: 1 / cells along x, z
  const refrInfo = v4('waterRefr'); // x: the refraction's depth is valid (1); y, z: depth conventions under test (z of the clip = d or 2d - 1; y flipped)
  const cam = v4('waterCam');       // xyz: the camera's world position (three.c fills no cameraPosition); w: 1 when the view is drawn rows bottom-up (cartwheel's WebGPU scene target) but the planar targets top-down

  const m4 = (n) => {
    const sv = Array.from({ length: 16 }, () => sentinel());
    const u = uniform(new THREE.Matrix4().fromArray(sv)).setName(n);
    u.setGroup(frame);
    props['ext.' + n] = { kind: 'mat4', value: sv };
    return u;
  };
  const refrInv = m4('waterRefrInv'); // the refraction camera's inverse projection (oblique near plane included)

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
  const rippleMap = tex('waterRippleMap', true, false, true);
  const refrDepth = new THREE.DepthTexture(2, 2);
  refrDepth.name = 'ext.waterRefractionDepth';

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
  const dist = length(cam.xyz.sub(positionWorld));
  const mapFlow = texture(flowMap, rest.div(flowDir.zw).add(0.5)).rg.mul(2).sub(1);
  const fm = mix(flowDir.xy, mapFlow, flow.w);
  const f = vec2(fm.x.negate(), fm.y);
  const uvs = rest.mul(look.w);
  const n0 = texture(normalMap, uvs.add(f.mul(flow.x)));
  const n1 = texture(normalMap, uvs.mul(0.73).add(vec2(0.31, 0.57)).add(f.mul(flow.y)));
  const lerpF = abs(flow.z.sub(flow.x)).div(flow.z);
  const nc = mix(n0, n1, lerpF);
  // a third, finer octave running against the flow: the small chop that breaks up the highlights
  const nf = texture(normalMap, uvs.mul(2.9).add(vec2(0.77, 0.13)).sub(f.mul(flow.y).mul(1.7)));
  // tangent-space map, +Z out of the surface (gen://noise out=normal): x along +x, y along -z
  const d = vec3(nc.r.mul(2).sub(1), nc.b.mul(2).sub(1), nc.g.mul(2).sub(1).negate());
  const df = vec2(nf.r.mul(2).sub(1), nf.g.mul(2).sub(1).negate());
  // Tiny repeating ripples alias at flight distances; keep the broad swell
  // and fade the detail before its repetitions become a visible grid.
  const detailWeight = float(1).sub(smoothstep(150, 900, dist)).mul(look.z);
  const fineWeight = float(1).sub(smoothstep(30, 220, dist)).mul(look.z).mul(0.6);
  // the interactive ripples: the height field's slope bends the normal
  const uvR = vRest.sub(ripR.xy).div(rip.yz).add(0.5);
  const inR = float(1).sub(step(0.5, max(abs(uvR.x.sub(0.5)), abs(uvR.y.sub(0.5))))).mul(step(0.001, rip.x));
  const hL = texture(rippleMap, uvR.sub(vec2(ripR.z, 0))).r;
  const hR = texture(rippleMap, uvR.add(vec2(ripR.z, 0))).r;
  const hD = texture(rippleMap, uvR.sub(vec2(0, ripR.w))).r;
  const hU = texture(rippleMap, uvR.add(vec2(0, ripR.w))).r;
  const rcell = rip.yz.mul(vec2(ripR.z, ripR.w));
  const rslope = vec2(hR.sub(hL).div(rcell.x.mul(2)), hU.sub(hD).div(rcell.y.mul(2))).mul(rip.x).mul(inR).mul(2);
  const rfoam = texture(rippleMap, uvR).g.mul(inR);
  const n = normalize(vec3(wn.x.add(d.x.mul(detailWeight)).add(df.x.mul(fineWeight)).sub(rslope.x), wn.y,
    wn.z.add(d.z.mul(detailWeight)).add(df.y.mul(fineWeight)).sub(rslope.y)));
  const theta = max(dot(toEye, n), 0);
  const fres = pow(float(1).sub(theta), 5).mul(float(1).sub(look.x)).add(look.x);
  const offset = n.xz.mul(look.y);
  const suv = vec2(screenUV.x, mix(screenUV.y, float(1).sub(screenUV.y), cam.w));
  const refl = texture(reflectMap, vec2(float(1).sub(suv.x), suv.y).add(offset)).rgb.mul(tint.w);
  // depth over the baked floor (a map outside its rectangle, or none: deep)
  const uvF = vRest.div(floorRect.xy).add(0.5);
  const outside = step(0.5, max(abs(uvF.x.sub(0.5)), abs(uvF.y.sub(0.5))));
  const mapOn = shore.x.mul(float(1).sub(outside));
  const bakedDepth = mix(float(100), max(texture(floorMap, uvF).r.add(vSurf), 0), mapOn);
  // the refraction's own depth: the water between the surface and whatever is under it, along the view ray, then
  // taken to a vertical depth. The clip depth (d or 2d - 1) and the row direction are what the host's targets use.
  const uvS = suv.add(offset);
  const dz = texture(refrDepth, uvS).r;
  const zClip = mix(dz.mul(2).sub(1), dz, refrInfo.y);
  const yClip = mix(uvS.y.mul(2).sub(1), float(1).sub(uvS.y.mul(2)), refrInfo.z);
  const vp = refrInv.mul(vec4(uvS.x.mul(2).sub(1), yClip, zClip, 1));
  const scenePos = vp.xyz.div(vp.w);
  const surfPos = cameraViewMatrix.mul(vec4(positionWorld, 1)).xyz;
  const hit = float(1).sub(step(0.99999, dz));
  const ray = max(length(scenePos).sub(length(surfPos)), 0);
  const zDepth = mix(float(100), ray.mul(max(toEye.y, 0.15)), hit);
  const useZ = refrInfo.x.mul(deep.w);
  const depth = mix(bakedDepth, zDepth, useZ);
  const dOn = max(mapOn, useZ);
  // Beer-Lambert style: the shallow colour turns into the deep one with depth, red first (water eats red light)
  const absorb = mix(float(1), float(1).sub(exp(depth.negate().mul(shore.z))), dOn);
  const ab3 = vec3(pow(absorb, 0.6), pow(absorb, 0.85), absorb);
  // the sun lights the water's body: nothing under a low sun, the sun's own colour (dusk is red) under a high one
  const hasSun = step(0.0001, sunCol.r.add(sunCol.g).add(sunCol.b));
  const sunUp = max(sun.y, 0);
  const lit = mix(vec3(1), max(vec3(0.16).add(sunCol.rgb.mul(sunUp).mul(0.42)), vec3(0.1)), hasSun);
  const body = mix(shallow.rgb, deep.rgb, ab3).mul(lit);
  // what is seen through the water, tinted, dimmed by the depth it went through, then lost into the body colour by murk
  const trans = mix(vec3(1), vec3(1).sub(ab3.mul(0.9)), dOn);
  const seen = mix(texture(refractMap, suv.add(offset)).rgb.mul(tint.rgb).mul(trans), body, dbg.w.mul(absorb));
  const refr = mix(body, seen, deep.w);
  // light through the crests toward the eye: the turquoise glow of a wave against the sun
  const toSun = normalize(sun.xyz);
  const fwd = pow(max(dot(toSun, toEye.negate()), 0), 3);
  const h01 = clamp(vSurf.div(max(sun.w, 0.05)).mul(0.5).add(0.5), 0, 1);
  const crest = smoothstep(0.4, 1, h01).mul(min(float(0.35).add(float(0.65).mul(float(1).sub(vBunch)).mul(4)), 1));
  const sss = vec3(0.04, 0.5, 0.42).mul(sunCol.rgb).mul(fwd).mul(crest).mul(sunCol.w).mul(float(1).sub(smoothstep(300, 1500, dist)));
  // the sun's glitter: a narrow highlight on the fine chop (lights give the broad one)
  const nFine = normalize(vec3(wn.x.add(df.x.mul(fineWeight).mul(2.2)).add(d.x.mul(detailWeight)), wn.y, wn.z.add(df.y.mul(fineWeight).mul(2.2)).add(d.z.mul(detailWeight))));
  const hv = normalize(toSun.add(toEye));
  const vdh = max(dot(toEye, hv), 0);
  const gl = pow(max(dot(nFine, hv), 0), 900).mul(pow(float(1).sub(vdh), 5).mul(0.9).add(0.1)).mul(sunCol.rgb).mul(rip.w).mul(10).mul(float(1).sub(smoothstep(60, 500, dist)));
  // foam: where the water meets the floor, on crests (the wave normal's bunching), and what splashes and wakes leave; broken up by noise at two scales
  const noise = texture(normalMap, uvs.mul(0.6).add(f.mul(flow.x))).a;
  const noise2 = texture(normalMap, uvs.mul(1.9).add(f.mul(flow.y)).add(vec2(0.4, 0.9))).a;
  const lumpy = smoothstep(0.3, 0.7, noise);
  const wob = sin(shallow.w.mul(1.4).add(noise.mul(6.283))).mul(0.5).add(0.5);
  const sw = max(shore.y, 0.001);
  const shoreOn = dOn.mul(step(0.001, shore.y));
  const shoreFoam = float(1).sub(smoothstep(0, sw, depth.add(noise.sub(0.5).mul(sw).mul(0.9)).sub(wob.mul(sw).mul(0.35)))).mul(shoreOn);
  // thinner lines of foam sliding in over the shallows
  const lines = smoothstep(0.62, 0.8, sin(depth.mul(7.0 / 1).sub(shallow.w.mul(1.6)).add(noise.mul(5))).mul(0.5).add(0.5))
    .mul(float(1).sub(smoothstep(sw.mul(0.8), sw.mul(3.2), depth))).mul(shoreOn).mul(0.55);
  const thr = float(0.62).sub(shore.w.mul(0.5));
  const farFade = float(1).sub(smoothstep(200, 700, dist));
  const crestFoam = smoothstep(thr, thr.add(0.12), float(1).sub(vBunch).add(noise.sub(0.5).mul(0.25))).mul(step(0.001, shore.w)).mul(lumpy).mul(farFade);
  const splashFoam = smoothstep(0.08, 0.5, rfoam.mul(mix(0.55, 1.2, noise2)));
  const foamRaw = max(max(shoreFoam.mul(mix(0.55, 1, lumpy)), lines.mul(noise2)), max(crestFoam, splashFoam));
  const foam = clamp(foamRaw.mul(mix(0.78, 1, noise2)), 0, 1);
  const emit = mix(refr, refl, fres).add(sss.mul(float(1).sub(fres))).add(gl);
  // the material colour (black: no diffuse; the C side keeps it so) and emissive (white) stay inputs
  mat.colorNode = vec4(materialColor.rgb.add(vec3(0.9, 0.92, 0.95).mul(foam)), 1);
  mat.normalNode = normalize(cameraViewMatrix.mul(vec4(n, 0)).xyz);
  mat.emissiveNode = emit.mul(materialEmissive).mul(float(1).sub(foam));
  // debug view HEIGHT: the world height over the range as 16 bits in R and G, pre-decoded so
  // that the sRGB output transfer (tone mapping NONE) lands each on its exact byte
  const hn = clamp(positionWorld.y.sub(dbg.y).div(dbg.z.sub(dbg.y)), 0, 1).mul(65535);
  const hi = floor(hn.div(256)), lo = floor(hn.sub(hi.mul(256)));
  const enc = sRGBTransferEOTF(vec3(hi.div(255), lo.div(255), 1));
  // debug view REFRACTION_DEPTH: R = the vertical depth over 20 m, G = the raw depth texel, B = hit
  const encZ = sRGBTransferEOTF(vec3(clamp(zDepth.div(20), 0, 1), dz, hit));
  mat.outputNode = mix(output, vec4(mix(enc, encZ, step(1.5, dbg.x)), 1), min(dbg.x, 1));
}
