// Water3D's surface for the three.c generator extensions "water" (water.mjs, state standard+xwater and its light
// variants, depth+xwater for shadows) and "waterlow" (waterlow.mjs, effects detail Very Low): Water3D's surface (cartwheel/water.h,
// src/water/water.c fills the values), written in three.js r187 TSL.
//
// Vertex: up to four Gerstner waves (GPU Gems 1, chapter 1) move the flat
// grid; the phases come reduced from the CPU (the same numbers
// cw_waves_displace uses), so the drawn surface is the CPU's to float
// rounding. The analytic wave normal goes to the fragment stage.
//
// Fragment: r187's Water2 / Water2Mesh, on a MeshStandardMaterial so the
// scene's lights give the glints (and shadows land on the water): two
// scrolling normal maps blended by the two-phase flow (Water2's
// flowConfig), on the wave normal; the planar reflection sampled at
// screenUV.flipX() (r187's ReflectorNode) and the refraction at screenUV,
// both bent by the normal (Water2's offset); a Schlick Fresnel term mixes
// them into the emissive, the diffuse colour is black, and the
// environment's specular stands in for the reflection when there is no
// planar one (the C side sets envMapIntensity).
//
// Very Low (low = true) keeps the waves, the detail normal map, the floor map's depth colour and shore foam, the
// environment's reflection and the sun's glitter, and leaves out what costs texture reads a pixel: the second and
// fine normal octaves, the ripples' slope (their foam, the wakes, stays), the refraction and its depth, caustics and
// the foam's lace.
//
// Names after "ext." are what the C fill hook sets (water.c); a name the low program lacks is skipped there.
export async function applyWater({ TSL, THREE, mat, props, sentinel }, low) {
  const {
    Fn, If, uniform, texture, sharedUniformGroup, positionLocal, positionWorld, cameraViewMatrix,
    modelWorldMatrix, screenUV, vec2, vec3, vec4, float, normalize, dot, max, min, mix, pow, abs, sin, cos, clamp,
    floor, length, smoothstep, varying, step, exp, sqrt, output, sRGBTransferEOTF, materialColor, materialEmissive,
    textureLoad, ivec2, int, select,
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
  const tint = low ? null : v4('waterTint'); // rgb: colour; w: reflection on (1) / off (0)
  const deep = v4('waterDeep');     // rgb: deep colour (the refraction when it is off); w: refraction on / off
  const flow = v4('waterFlow');     // flow offset 0, offset 1, half cycle, flow map on (1) / off (0)
  const flowDir = v4('waterFlowDir'); // flow x, flow z (without a map), rectangle size x, size z
  const dbg = v4('waterDebug');     // x: debug view (1 = height); y, z: height range; w: murk
  const shore = v4('waterShore');   // x: floor map on (1) / off (0); y: shore foam width (m); z: 1 / clarity (1 / m); w: crest foam amount
  const shallow = v4('waterShallow'); // rgb: shallow colour; w: the foam clock (s, mod 1000)
  const floorRect = v4('waterFloorRect'); // x, y: the rectangle the floor map covers (m); z: the camera is under the surface (1); w: the swell's share in the reflection
  const deepRgb = deep.rgb;
  const sun = v4('waterSun');       // xyz: the direction toward the sun (world); w: the waves' summed amplitude
  const sunCol = v4('waterSunCol'); // rgb: the sun's colour x energy (0 without a sky); w: scatter strength
  const rip = v4('waterRipple');    // x: ripple normal strength (0: no grid); y, z: the grid's size (m); w: glint
  const ripR = v4('waterRippleRect'); // x, y: the grid's centre (water-local x, z); z, w: 1 / cells along x, z
  const refrInfo = low ? null : v4('waterRefr'); // x: the refraction's depth is valid (1); y: caustic strength; z: 1 / caustic tile (m); w: camera under the water (the refraction target holds the view up through the surface)
  const refrSize = low ? null : v4('waterRefrSize'); // x, y: the refraction depth texture's size in texels; z: the foam's lace; w: soft foam
  const viewRect = low ? null : v4('waterView'); // split screen in one pass: the view's rectangle in screen uv (x, y from the top left, width, height); 0, 0, 1, 1 otherwise
  const grid = v4('waterGrid');     // x, y: the grid's centre (water-local; the camera's foot on a VIEW grid); z: VIEW grid on; w: the foam pattern's scale
  const shelter = v4('waterShelter'); // per wave: how much the wave mask holds it down
  const crestLight = v4('waterCrest'); // x: how much lighter crests are (0: as before); y: see_through (m, 0: off); z: the light reaching the water (0..1); w: 2 x tan(half the view's height angle)
  const contact = low ? null : v4('waterContact'); // x: the water over a point at which the refraction's offset is whole (m; 0: the offset whatever the depth); y: the leak bound (m; 0: no leak check); z: what is seen through the water fades along its two-way path (1) or by the vertical depth alone (0); w: foam stands back over objects as v0.87 had it (1: over their whole depth to 35 cm, the gates' control) or as now (0)
  const edge = low ? null : v4('waterEdge'); // x: the reflection read moved by the local height over y (1; 0: not moved, -1: the wrong way, a control); y: the height over the rest level the reflection target is mirrored about (m); z: the collar's glow (collar_glow); w: unused
  const fine = low ? null : v4('waterFine'); // x, y: the collar layer's centre (world x, z: read where the drawn surface is); z: its size (m); w: 1 / its cells (0: none)
  const cam = v4('waterCam');       // xyz: the camera's world position (three.c fills no cameraPosition); w: 1 when the view is drawn rows bottom-up (cartwheel's WebGPU scene target) but the planar targets top-down

  const m4 = (n) => {
    const sv = Array.from({ length: 16 }, () => sentinel());
    const u = uniform(new THREE.Matrix4().fromArray(sv)).setName(n);
    u.setGroup(frame);
    props['ext.' + n] = { kind: 'mat4', value: sv };
    return u;
  };
  const refrRowY = low ? null : v4('waterRefrRowY'); // the world height of a refraction point: row 1 of the camera's world matrix through the inverse projection, over refrRowW
  const refrRowW = low ? null : v4('waterRefrRowW'); // the inverse projection's w row
  const refrInv = low ? null : m4('waterRefrInv'); // the refraction camera's inverse projection (oblique near plane included)

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
  const reflectMap = low ? null : tex('waterReflection', true, false, true);
  const refractMap = low ? null : tex('waterRefraction', true, false, true);
  const floorMap = tex('waterFloorMap', true, false, true);
  const rippleMap = tex('waterRippleMap', true, false, true);
  const fineMap = low ? null : tex('waterFineMap', true, false, true);
  const causticMap = low ? null : tex('waterCaustic', true, true, false);
  const waveMask = tex('waterWaveMask', true, false, true);
  // the refraction's window depth, read texel by texel: a float texture (compatibility-mode WebGPU reads a depth
  // texture only through comparisons, so there the engine copies it into R32Float; GL binds the depth texture)
  const refrDepth = low ? null : new THREE.DataTexture(new Float32Array(4), 2, 2, THREE.RedFormat, THREE.FloatType);
  if (refrDepth) {
    refrDepth.name = 'ext.waterRefractionDepth';
    refrDepth.needsUpdate = true;
  }

  // ── vertex: Gerstner ───────────────────────────────────────────────
  // plain node expressions (no Fn: the shadow pass rebuilds the vertex stage from positionNode alone)
  const p0 = positionLocal;
  const rx = p0.x.add(grid.x), rz = p0.z.add(grid.y);    // where this vertex rests in the water's plane
  const fromCam = length(vec2(p0.x, p0.z));
  const maskV = texture(waveMask, vec2(rx, rz).div(floorRect.xy).add(0.5)).level(0).r;
  let pos = vec3(rx, float(0), rz);
  let nrm = vec3(0, 1, 0);
  const SH = [shelter.x, shelter.y, shelter.z, shelter.w];
  const Q = [steep.x, steep.y, steep.z, steep.w];
  const PH = [phase.x, phase.y, phase.z, phase.w];
  for (let i = 0; i < 4; i++) {
    const w = waves[i];
    const th = w.z.mul(w.x.mul(rx).add(w.y.mul(rz))).sub(PH[i]);
    const c = cos(th), s = sin(th);
    // a VIEW grid's rings are too far apart for a wave past five wavelengths (fromCam * k = 2 pi x wavelengths)
    const lod = mix(float(1), float(1).sub(smoothstep(31.4, 50.3, fromCam.mul(w.z))), grid.z);
    const a = w.w.mul(float(1).add(maskV.sub(1).mul(SH[i]))).mul(lod);
    const qa = Q[i].mul(a);
    pos = pos.add(vec3(qa.mul(w.x).mul(c), a.mul(s), qa.mul(w.y).mul(c)));
    const wa = w.z.mul(a);
    nrm = nrm.sub(vec3(w.x.mul(wa).mul(c), Q[i].mul(wa).mul(s), w.y.mul(wa).mul(c)));
  }
  mat.positionNode = pos;
  const waveNormalLocal = varying(nrm, 'vWaterNormal');
  // where this point of the surface has moved to in the water's plane, the displaced x, z: the floor map and the ripple grid are
  // maps of that plane, so a splash and a shore line stay where they were made while the waves pass through them
  const vRest = varying(vec2(pos.x, pos.z), 'vWaterRest');
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
  const lerpF = abs(flow.z.sub(flow.x)).div(flow.z);
  const nc = low ? n0 : mix(n0, texture(normalMap, uvs.mul(0.73).add(vec2(0.31, 0.57)).add(f.mul(flow.y))), lerpF);
  // a third, finer octave running against the flow: the small chop that breaks up the highlights (not on Very Low)
  const nf = low ? null : texture(normalMap, uvs.mul(2.9).add(vec2(0.77, 0.13)).sub(f.mul(flow.y).mul(1.7)));
  // tangent-space map, +Z out of the surface (gen://noise out=normal): x along +x, y along -z
  const d = vec3(nc.r.mul(2).sub(1), nc.b.mul(2).sub(1), nc.g.mul(2).sub(1).negate());
  const df = low ? vec2(0, 0) : vec2(nf.r.mul(2).sub(1), nf.g.mul(2).sub(1).negate());
  // Tiny repeating ripples alias at flight distances; keep the broad swell
  // and fade the detail before its repetitions become a visible grid.
  const detailWeight = float(1).sub(smoothstep(150, 900, dist)).mul(look.z);
  const fineWeight = float(1).sub(smoothstep(30, 220, dist)).mul(look.z).mul(0.6);
  // the interactive ripples: the height field's slope bends the normal (Very Low keeps only their foam: the wakes)
  let rslope = vec2(0, 0);
  const uvR = vRest.sub(ripR.xy).div(rip.yz).add(0.5);
  const inR = float(1).sub(step(0.5, max(abs(uvR.x.sub(0.5)), abs(uvR.y.sub(0.5))))).mul(step(0.001, rip.x));
  let rfoam = texture(rippleMap, uvR).g.mul(inR);
  let fineFoam = float(0);
  if (!low) {
    const hL = texture(rippleMap, uvR.sub(vec2(ripR.z, 0))).r;
    const hR = texture(rippleMap, uvR.add(vec2(ripR.z, 0))).r;
    const hD = texture(rippleMap, uvR.sub(vec2(0, ripR.w))).r;
    const hU = texture(rippleMap, uvR.add(vec2(0, ripR.w))).r;
    const rcell = rip.yz.mul(vec2(ripR.z, ripR.w));
    rslope = vec2(hR.sub(hL).div(rcell.x.mul(2)), hU.sub(hD).div(rcell.y.mul(2))).mul(rip.x).mul(inR).mul(2);
    // the collars' bubbles (cw_water_collar) on the collar layer near the player: one read, only inside its square
    // (explicit level: a branch), faded over its last 8 %
    const uvFi = positionWorld.xz.sub(fine.xy).div(fine.z).add(0.5);
    const inF = float(1).sub(smoothstep(0.42, 0.5, max(abs(uvFi.x.sub(0.5)), abs(uvFi.y.sub(0.5))))).mul(step(1e-6, fine.w));
    fineFoam = Fn(() => {
      const o = float(0).toVar();
      If(inF.greaterThan(0.001), () => { o.assign(texture(fineMap, uvFi).level(0).r.mul(inF)); });
      return o;
    })();
  }
  const n = normalize(vec3(wn.x.add(d.x.mul(detailWeight)).add(df.x.mul(fineWeight)).sub(rslope.x), wn.y,
    wn.z.add(d.z.mul(detailWeight)).add(df.y.mul(fineWeight)).sub(rslope.y)));
  const theta = max(dot(toEye, n), 0);
  const fres = pow(float(1).sub(theta), 5).mul(float(1).sub(look.x)).add(look.x);
  // Depth over the undistorted baked floor. Refraction displacement must
  // vanish where water meets land: otherwise its UV crosses into terrain
  // clipped out of the refraction target and exposes dark clear pixels.
  const uvF = vRest.div(floorRect.xy).add(0.5);
  const outside = step(0.5, max(abs(uvF.x.sub(0.5)), abs(uvF.y.sub(0.5))));
  const mapOn = shore.x.mul(float(1).sub(outside));
  const floorSample = texture(floorMap, uvF);
  const bakedDepth = mix(float(100), max(floorSample.r.add(vSurf), 0), mapOn);
  // The refraction pass clips at the rest plane: a crest must not make
  // clipped shore pixels eligible again. Troughs also reduce clearance.
  // G retains a deep sentinel for unhit floor rays, independently of range.
  const distortionDepth = mix(float(100), max(min(floorSample.g, floorSample.g.add(vSurf)), 0), mapOn);
  const shoreDistortion = smoothstep(0.5, 3, distortionDepth);
  if (low) {
    // ── Very Low: the baked floor's depth colours the body; no refraction, caustics or lace ──
    const depthL = bakedDepth;
    const absorb = mix(float(1), float(1).sub(exp(depthL.negate().mul(shore.z))), mapOn);
    const ab3 = vec3(pow(absorb, 0.6), pow(absorb, 0.85), absorb);
    const hasSun = step(0.0001, sunCol.r.add(sunCol.g).add(sunCol.b));
    const sunUp = max(sun.y, 0);
    const lit = mix(vec3(1), max(vec3(0.14).add(sunCol.rgb.mul(sunUp).mul(0.32)), vec3(0.1)), hasSun);
    const hBody = clamp(vSurf.div(max(sun.w, 0.05)).mul(0.5).add(0.5), 0, 1);
    const crestTop = smoothstep(0.55, 1, hBody).mul(crestLight.x);
    const body = mix(mix(shallow.rgb, deepRgb, ab3), shallow.rgb.mul(1.25), crestTop).mul(lit).mul(mix(0.75, 1.3, hBody));
    const refrL = mix(body, shallow.rgb.mul(lit).mul(1.5), crestTop.mul(0.75));
    const toSun = normalize(sun.xyz);
    const hv = normalize(toSun.add(toEye));
    const vdh = max(dot(toEye, hv), 0);
    const nG = normalize(vec3(wn.x.add(d.x.mul(detailWeight)), wn.y, wn.z.add(d.z.mul(detailWeight))));
    const glL = pow(max(dot(nG, hv), 0), 900).mul(pow(float(1).sub(vdh), 5).mul(0.9).add(0.1)).mul(sunCol.rgb).mul(rip.w).mul(10).mul(float(1).sub(smoothstep(60, 500, dist)));
    const noise = n0.a;
    const lumpy = smoothstep(0.3, 0.7, noise);
    const sw = max(shore.y, 0.001);
    const shoreOn = mapOn.mul(step(0.001, shore.y));
    const wob = sin(shallow.w.mul(1.4).add(noise.mul(6.283))).mul(0.5).add(0.5);
    const shoreFoam = float(1).sub(smoothstep(0, sw.mul(0.45), depthL.add(noise.sub(0.5).mul(sw).mul(0.6)).sub(wob.mul(sw).mul(0.3)))).mul(shoreOn);
    const lines = smoothstep(0.62, 0.8, sin(depthL.mul(7.0).sub(shallow.w.mul(1.6)).add(noise.mul(5))).mul(0.5).add(0.5))
      .mul(float(1).sub(smoothstep(sw.mul(0.8), sw.mul(3.2), depthL))).mul(shoreOn).mul(0.55);
    const thr = float(0.95).sub(shore.w.mul(0.4));
    const crestFoam = smoothstep(thr, thr.add(0.12), float(1).sub(vBunch).add(noise.sub(0.5).mul(0.25))).mul(step(0.001, shore.w)).mul(lumpy).mul(float(1).sub(smoothstep(200, 700, dist)));
    const splashFoam = smoothstep(0.08, 0.5, rfoam.mul(mix(0.55, 1.2, noise)));
    // foam by thickness (the full surface's soft foam, without its lace): solid where it is thick, breaking into soft
    // patches by the normal map's noise where it is thin
    const thick = max(max(shoreFoam.mul(mix(0.55, 1, lumpy)), lines.mul(noise)), max(crestFoam, rfoam.mul(1.25)));
    const cover = smoothstep(0.04, 0.62, thick.add(noise.sub(0.5).mul(0.55).mul(float(1).sub(smoothstep(0.5, 1, thick)))));
    const foamL = clamp(max(cover.mul(mix(0.8, 1, noise)).mul(smoothstep(0, 0.06, thick)), splashFoam), 0, 1); /* (none where none is: no specks) */
    // (no planar reflection on Very Low: the environment's specular stands in, as the C side sets it)
    const emitL = refrL.mul(float(1).sub(fres)).add(glL);
    // from under the water: the water's own colour (Very Low draws no view up through the surface)
    const under = clamp(floorRect.z, 0, 1);
    const bodyU = shallow.rgb.add(deepRgb).mul(0.5).mul(lit).mul(0.6);
    const foamV = foamL.mul(float(1).sub(under.mul(0.6)));
    mat.colorNode = vec4(materialColor.rgb.add(vec3(0.9, 0.92, 0.95).mul(foamV)), 1);
    mat.normalNode = normalize(cameraViewMatrix.mul(vec4(mix(n, n.negate(), under), 0)).xyz);
    mat.emissiveNode = select(under.greaterThan(0.5), bodyU, emitL).mul(materialEmissive).mul(float(1).sub(foamV));
    const hn = clamp(positionWorld.y.sub(dbg.y).div(dbg.z.sub(dbg.y)), 0, 1).mul(65535);
    const hi = floor(hn.div(256)), lo = floor(hn.sub(hi.mul(256)));
    const enc = sRGBTransferEOTF(vec3(hi.div(255), lo.div(255), 1));
    const belowOut = vec4(bodyU.mul(float(1).sub(foamV)).add(vec3(0.5, 0.6, 0.6).mul(lit).mul(foamV)), 1);
    mat.outputNode = select(dbg.x.greaterThan(0.5), vec4(enc, 1), mix(output, belowOut, under));
    return;
  }
  // the swell's own tilt moves the refraction less than the ripples do
  const offset0 = n.xz.sub(wn.xz.mul(0.6)).mul(look.y).mul(shoreDistortion);
  // (the view's own screen uv: its planar targets are its own, as a view drawn alone has them)
  const vuv = screenUV.sub(viewRect.xy).div(viewRect.zw);
  const suv = vec2(vuv.x, mix(vuv.y, float(1).sub(vuv.y), cam.w));
  // (the reflection is moved by the ripples only: the swell bending it reaches the ground under the mirrored horizon)
  // (the reflection target is mirrored about the height edge.y; the surface here mirrors about its own height vSurf, so
  // the mirrored world stands 2 (vSurf - edge.y) higher: read the target that much lower on the screen, a world height
  // taken to the screen by the view's depth here and how far world up leans into the picture's up. An object's
  // reflection then meets it at the line on a crest or in a trough instead of hanging under it or over it)
  const surfV = cameraViewMatrix.mul(vec4(positionWorld, 1)).xyz;
  const upV = cameraViewMatrix.mul(vec4(0, 1, 0, 0)).xyz;
  const reflDv = vSurf.sub(edge.y).mul(2).mul(upV.y).div(max(surfV.z.negate(), 0.1).mul(max(crestLight.w, 0.01))).mul(edge.x);
  const refl = texture(reflectMap, vec2(float(1).sub(suv.x), suv.y.add(reflDv)).add(n.xz.sub(wn.xz).mul(look.y)).add(wn.xz.mul(floorRect.w))).rgb.mul(tint.w);
  // the refraction's own depth: the water between the surface and whatever is under it, along the view ray, then
  // taken to a vertical depth. The clip depth (d or 2d - 1) and the row direction are what the host's targets use.
  // texel rows top row first on both backends (three.c's GL targets and the WebGPU copy alike)
  const dSize = refrSize.xy;
  const surfPos = cameraViewMatrix.mul(vec4(positionWorld, 1)).xyz;
  // one point of the refraction: its window depth, where it is in the view, whether anything was drawn there, the
  // water between the surface and it along the view ray, and its world height
  const refrAt = (uvK) => {
    const tc = clamp(floor(uvK.mul(dSize)), vec2(0), dSize.sub(1));
    // (taken to GL's [0, 1] range at once: the WebGPU copy holds 3e38 where nothing was drawn, and any use of that
    // raw value can overflow to inf; inf times a mix weight of 0 is NaN, and a NaN in one channel turns that channel
    // black. Every later use reads dz, never the texel.)
    const dzK = min(textureLoad(refrDepth, ivec2(int(tc.x), int(tc.y))).x, 1);
    // the planar targets' depth is GL's window depth on both backends (three.c remaps WebGPU's clip z), and suv
    // reads them top row first
    // (clamped under 1: the WebGPU copy holds 3e38 where nothing was drawn, and the oblique projection's far plane
    // may send window depth 1 to infinity; a NaN there survives every mix by 0 on some drivers)
    const clipK = vec4(uvK.x.mul(2).sub(1), float(1).sub(uvK.y.mul(2)), min(dzK, 0.999).mul(2).sub(1), 1);
    const vpK = refrInv.mul(clipK);
    const sp = vpK.xyz.div(max(vpK.w, 1e-6)); // (never 0 / 0: a NaN here would reach the picture through every mix by 0)
    const hitK = float(1).sub(step(0.99999, dzK));
    const pwY = dot(refrRowY, clipK).div(max(dot(refrRowW, clipK), 1e-6));
    return { dz: dzK, sp, hit: hitK, t: mix(float(1000), clamp(length(sp).sub(length(surfPos)), 0, 1000), hitK), y: pwY };
  };
  // The water over the undistorted point first: the offset grows with it, from nothing at the contact to whole at
  // contact.x metres (a point's apparent shift under a surface grows with its depth, Snell), so the picture under the
  // surface joins the object above it at the line instead of jumping sideways there.
  const r0 = refrAt(suv);
  const offK = select(contact.x.greaterThan(0.0001), clamp(r0.t.div(max(contact.x, 0.0001)), 0, 1), float(1));
  const uvD = suv.add(offset0.mul(offK));
  const rD = refrAt(uvD);
  // The leak check (a bent read must stay on what the straight one sees): the bent point is above the surface (a part of
  // something out of the water, which the refraction holds up to the crests' height), or it lies on another surface
  // than the straight one (the water along the ray differs by more than contact.y plus a quarter of the shorter, as at
  // an object's edge): then the straight read is used.
  const leakOn = step(0.0001, contact.y);
  const aboveD = rD.hit.mul(step(positionWorld.y.add(0.02), rD.y));
  const jumpD = step(contact.y.add(min(rD.t, r0.t).mul(0.25)), abs(rD.t.sub(r0.t)));
  const leak = max(aboveD, jumpD).mul(leakOn).greaterThan(0.5);
  const uvS = select(leak, suv, uvD);
  const dz = select(leak, r0.dz, rD.dz);
  const scenePos = select(leak, r0.sp, rD.sp);
  // (a point above this surface is not under the water: read as nothing seen, so a dry leg, a rider or a hull beside
  // the surface, which the refraction holds up to the crests' height, never shows through it, also where the half
  // resolution refraction's texel at a silhouette's edge holds it)
  const yS = select(leak, r0.y, rD.y);
  const hit = select(leak, r0.hit, rD.hit).mul(float(1).sub(step(positionWorld.y.add(0.02), yS).mul(leakOn)));
  const ray = clamp(length(scenePos).sub(length(surfPos)), 0, 1000);
  const zDepth = mix(float(100), ray.mul(max(toEye.y, 0.15)), hit);
  const useZ = refrInfo.x.mul(deep.w);
  // What is under the surface and is not the floor (a hull, a rider's legs, a post in deep water): a point the
  // refraction saw that is much shallower than the ground two metres to each side of it on the screen (all four
  // ways: a slope or a floor is shallower on one side, a wide hull has no ground around it). It is seen through the
  // water cloudy (below), and never as a shoal: its own depth must not make shore colour or foam, so those read
  // the ground's depth there.
  const seeT = max(crestLight.y, 0.001);
  const seeOn = step(0.001, crestLight.y).mul(useZ);
  const surfDist = max(length(surfPos), 0.1);
  const aspect = refrSize.x.div(refrSize.y);
  const tap = (uvK) => {
    const tcK = clamp(floor(uvK.mul(refrSize.xy)), vec2(0), refrSize.xy.sub(1));
    const dzK = min(textureLoad(refrDepth, ivec2(int(tcK.x), int(tcK.y))).x, 1);
    // (the depth under the surface straight down: the point's height in the world. Two rows of the refraction's
    // inverse projection and the camera's world matrix, folded on the CPU: a height is two dot products)
    const clipK = vec4(uvK.x.mul(2).sub(1), float(1).sub(uvK.y.mul(2)), min(dzK, 0.999).mul(2).sub(1), 1);
    const pwY = dot(refrRowY, clipK).div(max(dot(refrRowW, clipK), 1e-6));
    // (a point above this surface, which the refraction holds up to the crests' height, is not under the water: read as
    // nothing seen, so the dry part of a leg beside the contact never shows through the surface)
    const hitK = float(1).sub(step(0.99999, dzK)).mul(float(1).sub(step(positionWorld.y.add(0.02), pwY).mul(leakOn)));
    return { hit: hitK, z: mix(bakedDepth, clamp(positionWorld.y.sub(pwY), 0, 1000), hitK) }; // (nothing seen: the floor map's floor, so land is shallow and open water deep)
  };
  // the ground around: three taps two metres out to the sides and up the screen (depths only), the middle one of which
  // is the ground's depth (a slope is shallower on one side, another object may stand beside this one, and neither
  // makes the ground look shallow). Only a pixel that sees something within a few metres of the surface asks; open
  // water, where nothing is drawn or the floor is far, pays for the one depth it reads anyway
  const rFy = float(2).div(surfDist.mul(max(crestLight.w, 0.01)));
  const rF = vec2(rFy.div(aspect), rFy);
  const ringUv = [[-1, 0], [1, 0], [0, 1]].map(([x, y]) => clamp(uvS.add(rF.mul(vec2(x, y))), vec2(0), vec2(1)));
  const median3 = (a, b, c) => max(min(a, b), min(max(a, b), c));
  // (the middle tap is the read already made: the same texel)
  const tapC = { hit, z: mix(bakedDepth, clamp(positionWorld.y.sub(yS), 0, 1000), hit) };
  const near = tapC.hit.mul(step(tapC.z, 3.5)).mul(seeOn).greaterThan(0.5);
  const ground = Fn(() => {
    const o = float(0).toVar();
    If(near, () => { o.assign(median3(tap(ringUv[0]).z, tap(ringUv[1]).z, tap(ringUv[2]).z)); });
    return o;
  })();
  const zRef = ground;
  // this pixel's own part of an object; the rest of the work is for these pixels only
  const objB = near.select(smoothstep(0.5, 1.2, zRef.sub(tapC.z)), float(0));
  const objOf = (t) => t.hit.mul(smoothstep(0.5, 1.2, zRef.sub(t.z))).mul(seeOn);
  const isObj = objB.greaterThan(0.001);
  // The submerged object's picture: five taps (the centre and four around it) over a disc whose radius grows with
  // the water over the object (scattering: about a tenth of the depth, nothing at the contact), each tap counting by
  // its own depth. The light that shows the object crossed the water twice: down to it (the depth z) and back up
  // along the refracted view ray (z / cos of the refracted angle, Snell from this pixel's normal), so it is scattered
  // out by exp(-(z + z / cosT) / (2 see_through)) (straight down that is exp(-z / see_through)) and absorbed per
  // colour by the clarity's coefficients over the same path, red first, as the floor is. The water's tint grows with
  // what was scattered: at the contact the object shows as it is, its colour going to the water's own a little
  // deeper. All of it times the light reaching the water. What does not show is the water's own colour. Colours are
  // capped (a lit float's HDR value, with the red eaten on the way, read as lime patches through a wake on WebGPU).
  // The blur and the picture's work is for the pixels of an object only: a branch, so that open water pays for the
  // depth reads above and nothing else. (contact.z 0: the look before, by the vertical depth, tinted and blurred
  // 4 cm at the contact.)
  const cosE = max(dot(toEye, n), 0.05);
  const cosR = sqrt(max(float(1).sub(float(1).sub(cosE.mul(cosE)).div(1.777)), 0.3));
  const twoWay = float(1).add(float(1).div(cosR)).mul(0.5); // (the path over 2 z)
  const zr = min(tapC.z, 1.5);
  // (the blur is the light scattered on the way: about a tenth of the depth in slightly cloudy water, see_through 0.4 and
  // under, less in clearer water, by see_through / 0.4, to a quarter)
  const rM = mix(float(0.04), float(0), contact.z).add(zr.mul(0.12).mul(mix(float(1), clamp(float(0.4).div(seeT), 0.25, 1), contact.z)));
  const rUvY = rM.div(surfDist.mul(max(crestLight.w, 0.01)));
  // (at least a texel of the refraction: the object's mask then fades over two texels at its outline instead of stepping
  // with them, which a wake's foam standing back over it would show as a hard, striped edge)
  const rUv = max(vec2(rUvY.div(aspect), rUvY), vec2(1).div(refrSize.xy));
  const seeObj = Fn(() => {
    const o = vec4(0).toVar();
    If(isObj, () => {
      let sumV = float(0), sumC = vec3(0);
      for (let k = 0; k < 5; k++) {
        const d = k === 0 ? [0, 0] : [[1, 0], [-1, 0], [0, 1], [0, -1]][k - 1];
        const uvK = clamp(uvS.add(rUv.mul(vec2(d[0] * 0.8, d[1] * 0.8))), vec2(0), vec2(1));
        const t = tap(uvK);
        const vK = objOf(t).mul(exp(t.z.mul(mix(float(1), twoWay, contact.z)).negate().div(seeT)));
        sumV = sumV.add(vK);
        sumC = sumC.add(min(texture(refractMap, uvK).level(0).rgb, vec3(2)).mul(vK));
      }
      // (the red goes first on the way up and down: by the object's own depth)
      const zc = min(tapC.z, 4);
      const scat = float(1).sub(exp(zc.mul(twoWay).negate().div(seeT)));
      const tintN = mix(vec3(1), tint.rgb, scat);
      const absN = exp(vec3(0.45, 0.065, 0.035).mul(shore.z).mul(5).mul(zc.mul(twoWay).mul(2)).negate());
      const tintC = mix(tint.rgb.mul(exp(vec3(-0.9, -0.15, -0.08).mul(zc))), tintN.mul(absN), contact.z);
      // (the light reaching the water scales what shows below the first 10 cm: at the line the object is lit as it is
      // above the surface, by day or by a lamp at night, and its picture already holds that light)
      const lightZ = mix(crestLight.z, mix(float(1), crestLight.z, smoothstep(0, 0.1, zc)), contact.z);
      o.assign(vec4(sumC.div(5).mul(tintC).mul(lightZ), sumV.div(5).mul(lightZ)));
    });
    return o;
  })();
  const objV = seeObj.w;
  const objCol = seeObj.xyz;
  // what the object hides is the floor: the picture of the ring tap whose depth is the ground's, else (nothing there) the water's colour alone
  const seeFloor = Fn(() => {
    const o = vec3(0).toVar();
    If(isObj, () => {
      const pick = select(tap(ringUv[0]).z.equal(zRef), ringUv[0], select(tap(ringUv[1]).z.equal(zRef), ringUv[1], ringUv[2]));
      o.assign(min(texture(refractMap, pick).level(0).rgb, vec3(2)));
    });
    return o;
  })();
  const texC = mix(texture(refractMap, uvS).rgb, seeFloor, objB);
  const depth = mix(mix(bakedDepth, zDepth, useZ), bakedDepth, objB);
  const dOn = max(mapOn, useZ);
  // Beer-Lambert style: the shallow colour turns into the deep one with depth, red first (water eats red light)
  const absorb = mix(float(1), float(1).sub(exp(depth.negate().mul(shore.z))), dOn);
  const ab3 = vec3(pow(absorb, 0.6), pow(absorb, 0.85), absorb);
  // the sun lights the water's body: nothing under a low sun, the sun's own colour (dusk is red) under a high one
  const hasSun = step(0.0001, sunCol.r.add(sunCol.g).add(sunCol.b));
  const sunUp = max(sun.y, 0);
  const lit = mix(vec3(1), max(vec3(0.14).add(sunCol.rgb.mul(sunUp).mul(0.32)), vec3(0.1)), hasSun);
  // a crest holds less water between the eye and the deep than a trough: lighter crests, darker troughs
  const hBody = clamp(vSurf.div(max(sun.w, 0.05)).mul(0.5).add(0.5), 0, 1);
  // crest_light: the thin water at the top of a wave is lighter and greener than the body under it, whatever way the sun lies
  // (over an object near the line it comes in with the water over the object, 2 cm to 40 cm: the crest's light is the
  // thin top of a wave lit through, and a leg or a hull riding the crest has almost none of it in front of it there)
  const nearObj = objB.mul(float(1).sub(smoothstep(0.02, 0.4, tapC.z))).mul(contact.z);
  const crestTop = smoothstep(0.55, 1, hBody).mul(crestLight.x).mul(float(1).sub(nearObj));
  const body = mix(mix(shallow.rgb, deepRgb, ab3), shallow.rgb.mul(1.25), crestTop).mul(lit).mul(mix(0.75, mix(1.3, 1, nearObj), hBody));
  // what is seen through the water lost its light on the way down to it and back up along the view, per colour (pure
  // water eats red ten times faster than green, green twice as fast as blue: clarity 5 m is clear sea water), and the
  // water's own lit colour scattered in over the same path (murk: how much)
  const viewPath = mix(mix(depth.div(max(toEye.y, 0.15)), ray, useZ.mul(hit)), bakedDepth.div(max(toEye.y, 0.15)), objB);
  const path = mix(float(4), depth.add(viewPath), dOn);
  const sig = vec3(0.45, 0.065, 0.035).mul(shore.z).mul(5);
  const trans = exp(sig.mul(path).negate());
  const inscatter = float(1).sub(exp(path.mul(shore.z).mul(dbg.w).mul(0.5).negate()));
  // caustics: the sun's light gathered by the waves into moving lines on what is seen under the water (where the
  // refraction's depth says where that is); two copies of the tile scrolled against each other, the smaller kept
  const toSun0 = normalize(sun.xyz);
  const sceneW = cam.xyz.sub(toEye.mul(length(scenePos)));
  const below = positionWorld.y.sub(sceneW.y);
  const cq = sceneW.xz.add(toSun0.xz.div(max(toSun0.y, 0.3)).mul(below).mul(0.75)).mul(refrInfo.z);
  const ct = shallow.w;
  // (red: the sharp web; green: a soft copy that takes over deeper down, where the lines spread)
  const k1 = texture(causticMap, cq.add(vec2(0.013, 0.007).mul(ct))).rg;
  const k2 = texture(causticMap, cq.mul(1.31).add(vec2(0.41, 0.17)).sub(vec2(0.006, 0.011).mul(ct))).rg;
  const cSoft = max(smoothstep(0.5, 4, below), smoothstep(12, 40, dist));
  const kc = dot(min(k1, k2), vec2(float(1).sub(cSoft), cSoft)).mul(4);
  // (max: a pixel whose refraction depth lies in front of the surface has below < 0, and exp() overflows there: inf times the
  // smoothstep's 0 is NaN, which reaches the picture as black. A wave standing above the eye line hit it)
  const cfade = smoothstep(0.02, 0.5, below).mul(exp(max(below, 0).negate().mul(shore.z).mul(1.6))).mul(smoothstep(0, 0.25, toSun0.y))
    .mul(hit).mul(useZ).mul(hasSun);
  const caus = float(1).add(refrInfo.y.mul(cfade).mul(float(1).sub(objB)).mul(kc.mul(2.2).sub(1)));
  const seenFloor = texC.mul(tint.rgb).mul(trans).mul(caus).add(body.mul(inscatter));
  const seen = seenFloor.mul(float(1).sub(objV)).add(objCol);
  // crest_light: light comes through the thin top of a wave whatever lies under it, so it is mixed in after the refraction
  // (a collar's piled-up bubbly water is thin and lit through as a crest's top is: it glows by its bubbles)
  const collarGlow = smoothstep(0.1, 0.6, fineFoam).mul(edge.z).mul(0.3)
    .mul(float(1).sub(max(objB, objV).mul(exp(max(tapC.z.sub(0.1), 0).div(0.06).negate())).mul(contact.z)));
  const refr = mix(mix(body, seen, deep.w), shallow.rgb.mul(lit).mul(1.5), max(crestTop.mul(0.75), collarGlow).mul(float(1).sub(objV.mul(contact.z))));
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
  const shoreFoam = float(1).sub(smoothstep(0, sw.mul(0.45), depth.add(noise.sub(0.5).mul(sw).mul(0.6)).sub(wob.mul(sw).mul(0.3)))).mul(shoreOn);
  // thinner lines of foam sliding in over the shallows
  const lines = smoothstep(0.62, 0.8, sin(depth.mul(7.0 / 1).sub(shallow.w.mul(1.6)).add(noise.mul(5))).mul(0.5).add(0.5))
    .mul(float(1).sub(smoothstep(sw.mul(0.8), sw.mul(3.2), depth))).mul(shoreOn).mul(0.55);
  const thr = float(0.95).sub(shore.w.mul(0.4));
  const farFade = float(1).sub(smoothstep(200, 700, dist));
  const crestFoam = smoothstep(thr, thr.add(0.12), float(1).sub(vBunch).add(noise.sub(0.5).mul(0.25))).mul(step(0.001, shore.w)).mul(lumpy).mul(farFade);
  const crestTopFoam = smoothstep(0.7, 0.86, hBody.add(noise.sub(0.5).mul(0.14))).mul(crestLight.x).mul(lumpy).mul(float(1).sub(smoothstep(400, 1100, dist)));
  const splashFoam = smoothstep(0.08, 0.5, rfoam.mul(mix(0.55, 1.2, noise2)));
  const foamRaw = max(max(shoreFoam.mul(mix(0.55, 1, lumpy)), lines.mul(noise2)), max(max(crestFoam, crestTopFoam), splashFoam));
  // foam is lace, not cotton: the caustic tile's web of lines, scrolled with the flow, is the bubbles' pattern; thin
  // foam keeps only its brightest knots, thick foam fills the cells (far away, where the lace would alias, it evens out)
  const lace0 = texture(causticMap, rest.mul(grid.w).mul(0.29).add(f.mul(flow.x).mul(0.4))).r.mul(4);
  const lace1 = texture(causticMap, rest.mul(grid.w).mul(0.71).add(vec2(0.37, 0.61)).sub(f.mul(flow.y).mul(0.6))).r.mul(4);
  const lace = mix(clamp(max(lace0, lace1.mul(0.8)).mul(0.4), 0, 1), float(0.55), smoothstep(60, 250, dist));
  const foam1 = clamp(foamRaw.mul(1.45).sub(float(1).sub(lace).mul(refrSize.z)), 0, 1).mul(mix(0.82, 1, noise2));
  // close up: foam by thickness, not by a cut-out. Thick foam is solid and churned, thin foam breaks into soft patches at two
  // scales and goes to nothing, with no edge the eye can read as a mask
  const softLace = clamp(max(texture(causticMap, rest.mul(grid.w).mul(0.29).add(f.mul(flow.x).mul(0.4))).g, texture(causticMap, rest.mul(grid.w).mul(0.71).add(vec2(0.37, 0.61)).sub(f.mul(flow.y).mul(0.6))).g).mul(9), 0, 1);
  const patchy = noise.mul(0.55).add(noise2.mul(0.45));
  const thick = max(max(shoreFoam.mul(mix(0.55, 1, lumpy)), lines.mul(noise2)), max(max(crestFoam, crestTopFoam), rfoam.mul(1.25)));
  const cover = smoothstep(0.04, 0.62, thick.add(patchy.sub(0.5).mul(0.55).mul(float(1).sub(smoothstep(0.5, 1, thick)))));
  const foam2 = cover.mul(mix(softLace.mul(0.75).add(0.25), float(1), smoothstep(0.42, 0.95, thick))).mul(mix(0.8, 1, noise));
  // (foam stands back over an object where it meets the surface: none over its first 10 cm, so at the line a leg or a
  // hull is seen through clear water, never painted over by the crest's foam and lace or a wake's, and it comes back
  // with the depth from there, by half within 4 cm more and nearly all by 27 cm, with no edge. Deeper, foam floats over what lies under it as it does over open water: a stern or
  // a deck a few tens of centimetres down lies under the wake's foam, not under a dark slab cut out of it with the
  // object's outline. The mask is the object's own (objB) and its blurred visibility (objV).
  // The white water of a hull at speed is the game's spray, drawn over the water)
  const foamBack = mix(max(objB, objV).mul(exp(max(tapC.z.sub(0.1), 0).div(0.06).negate())),
    max(objB.mul(float(1).sub(smoothstep(0.1, 0.35, tapC.z))), objV), contact.w).mul(contact.z); // (contact.w 1: v0.87's, the control)
  // (a collar's bubbles are dense where they are: not cut into lace as thin crest foam is)
  const collarFoam = smoothstep(0.1, 0.6, fineFoam).mul(mix(0.6, 1, noise2));
  const foam = max(mix(foam1, clamp(foam2, 0, 1), refrSize.w), collarFoam).mul(float(1).sub(foamBack));
  // (the glow comes from the water between the surface and what lies behind it: in front of an object near the line
  // there is almost none)
  const emitAbove = mix(refr, refl, fres).add(sss.mul(float(1).sub(fres)).mul(float(1).sub(objV.mul(contact.z)))).add(gl);
  // seen from under the water: the world above shows through a window of light (Snell's), bent and dimmed by the
  // Fresnel term from water into air; outside it the surface mirrors the water itself (total internal reflection)
  // (decided on the CPU from the drawn waves at the camera: one answer for the whole surface)
  const under = clamp(floorRect.z, 0, 1);
  const nb = n.negate();
  const cosI = max(dot(toEye, nb), 0);
  const sinT2 = float(1).sub(cosI.mul(cosI)).mul(1.777);
  const cosT = sqrt(max(float(1).sub(sinT2), 0));
  const trU = float(1).sub(pow(float(1).sub(cosT), 5).mul(0.98).add(0.02)).mul(float(1).sub(step(1, sinT2)));
  const skyGuess = mix(vec3(0.5, 0.7, 0.9), sunCol.rgb.mul(0.3).add(vec3(0.1, 0.2, 0.35)), hasSun);
  const above = mix(skyGuess, min(texture(refractMap, suv.add(nb.xz.mul(look.y).mul(3))).rgb, vec3(1e4)), refrInfo.w);
  const bodyU = shallow.rgb.add(deepRgb).mul(0.5).mul(lit).mul(0.6);
  // (select, not mix: whatever the side not shown computes, a NaN or an inf there must not reach the picture)
  const emit = select(under.greaterThan(0.5), mix(bodyU, above, trU), emitAbove);
  const foamV = foam.mul(float(1).sub(under.mul(0.6)));
  // the material colour (black: no diffuse; the C side keeps it so) and emissive (white) stay inputs
  mat.colorNode = vec4(materialColor.rgb.add(vec3(0.9, 0.92, 0.95).mul(foamV)), 1);
  mat.normalNode = normalize(cameraViewMatrix.mul(vec4(mix(n, nb, under), 0)).xyz);
  mat.emissiveNode = emit.mul(materialEmissive).mul(float(1).sub(foamV));
  // debug view HEIGHT: the world height over the range as 16 bits in R and G, pre-decoded so
  // that the sRGB output transfer (tone mapping NONE) lands each on its exact byte
  const hn = clamp(positionWorld.y.sub(dbg.y).div(dbg.z.sub(dbg.y)), 0, 1).mul(65535);
  const hi = floor(hn.div(256)), lo = floor(hn.sub(hi.mul(256)));
  const enc = sRGBTransferEOTF(vec3(hi.div(255), lo.div(255), 1));
  // debug view REFRACTION_DEPTH: R = the vertical depth over 20 m, G = the window depth (1: nothing drawn), B = hit
  const encZ = sRGBTransferEOTF(vec3(clamp(zDepth.div(20), 0, 1), dz, hit));
  // from under the water the surface is drawn as computed here, unlit: the scene's lights and environment shading a
  // surface from its back side are not what the eye sees through it (and the environment's lower half is not water)
  const belowOut = vec4(mix(bodyU, above, trU).mul(float(1).sub(foamV)).add(vec3(0.5, 0.6, 0.6).mul(lit).mul(foamV)), 1);
  // (alpha 1: the post chain reads the scene target's alpha, and the lit output's is not 1 on every backend)
  // (both sides finite: belowOut is built from clamped reads. The lit output cannot go in a select here: r187's
  // shadow pass then stops drawing the caster with its own material)
  const shaded = mix(output, belowOut, under);
  // (select, never mix by a 0 / 1 weight, for the debug views: WGSL leaves inf and NaN arithmetic to the
  // implementation, so a side not shown that overflows would reach the picture as inf * 0 = NaN)
  mat.outputNode = select(dbg.x.greaterThan(0.5), vec4(select(dbg.x.greaterThan(1.5), encZ, enc), 1), shaded);
}
