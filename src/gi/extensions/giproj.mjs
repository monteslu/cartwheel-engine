// three.c generator extension "giproj" (state basic+xgiproj, in the engine's
// own table): the light probe grid's spherical-harmonic projection, drawn
// into one texel of the grid's atlas per draw (src/gi/probe_grid.c).
//
// It is r187's LightProbeGenerator.fromCubeRenderTarget run on the GPU: every
// texel of the probe's six captured faces adds its radiance times the L2 SH
// basis of its direction, weighted by the texel's solid angle, and the sum is
// normalized to 4 pi. The output is one of the seven RGBA groups r187's
// LightProbeGrid packs the nine RGB coefficients into (LightProbeGrid.js
// repackNode): group 0 = c0.rgb, c1.r; 1 = c1.gb, c2.rg; 2 = c2.b, c3.rgb;
// 3 = c4.rgb, c5.r; 4 = c5.gb, c6.rg; 5 = c6.b, c7.rgb; 6 = c8.rgb, 0.
//
// The faces are what probe_grid.c renders, with these cameras: face k looks
// along FACES[k].f with up FACES[k].u (three.js lookAt: right = f x u), so a
// face texel (x, y), row 0 at the bottom, looks along f + s r + t u with s, t
// the texel centre in -1..1.
//
// Values the C side sets: giProj vec4 (face size in texels, group 0..6, 0, 0);
// textures giF0 .. giF5 (the faces: half float RGBA).
export const FACES = [
  { f: [1, 0, 0], u: [0, 1, 0] }, { f: [-1, 0, 0], u: [0, 1, 0] },
  { f: [0, 1, 0], u: [0, 0, 1] }, { f: [0, -1, 0], u: [0, 0, -1] },
  { f: [0, 0, 1], u: [0, 1, 0] }, { f: [0, 0, -1], u: [0, 1, 0] },
];
const cross = (a, b) => [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]];

export const name = 'giproj';
export async function apply({ TSL, THREE, mat, props, sentinel }) {
  const { Fn, Loop, If, Break, uniform, textureLoad, vec3, vec4, ivec2, float, int, sqrt } = TSL;
  const s = [sentinel(), sentinel(), sentinel(), sentinel()];
  const proj = uniform(new THREE.Vector4(...s)).setName('giProj');
  props['ext.giProj'] = { kind: 'vec4', value: s };
  const face = (k) => {
    const t = new THREE.DataTexture(new Uint16Array(4 * 4 * 4), 4, 4, THREE.RGBAFormat, THREE.HalfFloatType);
    t.name = 'ext.giF' + k;
    t.magFilter = t.minFilter = THREE.NearestFilter;
    t.generateMipmaps = false;
    t.wrapS = t.wrapT = THREE.ClampToEdgeWrapping;
    t.needsUpdate = true;
    return t;
  };
  const faces = FACES.map((_, k) => face(k));

  mat.outputNode = Fn(() => {
    const n = int(proj.x).toVar();
    const nf = proj.x;
    const c = Array.from({ length: 9 }, () => vec3(0).toVar());
    const wsum = float(0).toVar();
    FACES.forEach((F, k) => {
      const r = cross(F.f, F.u);
      Loop({ start: int(0), end: int(32), name: 'y' }, ({ y }) => {
        If(y.greaterThanEqual(n), () => { Break(); });
        Loop({ start: int(0), end: int(32), name: 'x' }, ({ x }) => {
          If(x.greaterThanEqual(n), () => { Break(); });
          const su = float(x).add(0.5).div(nf).mul(2).sub(1);
          const tv = float(y).add(0.5).div(nf).mul(2).sub(1);
          const d = vec3(su.mul(r[0]).add(tv.mul(F.u[0])).add(F.f[0]),
            su.mul(r[1]).add(tv.mul(F.u[1])).add(F.f[1]),
            su.mul(r[2]).add(tv.mul(F.u[2])).add(F.f[2])).toVar();
          const l2 = d.dot(d);
          const w = float(4).div(sqrt(l2).mul(l2)).toVar();
          const dir = d.div(sqrt(l2)).toVar();
          const rad = textureLoad(faces[k], ivec2(x, y)).rgb.mul(w).toVar();
          const X = dir.x, Y = dir.y, Z = dir.z;
          // the L2 basis (r187 SphericalHarmonics3.getBasisAt)
          c[0].addAssign(rad.mul(0.282095));
          c[1].addAssign(rad.mul(Y.mul(0.488603)));
          c[2].addAssign(rad.mul(Z.mul(0.488603)));
          c[3].addAssign(rad.mul(X.mul(0.488603)));
          c[4].addAssign(rad.mul(X.mul(Y).mul(1.092548)));
          c[5].addAssign(rad.mul(Y.mul(Z).mul(1.092548)));
          c[6].addAssign(rad.mul(Z.mul(Z).mul(3).sub(1).mul(0.315392)));
          c[7].addAssign(rad.mul(X.mul(Z).mul(1.092548)));
          c[8].addAssign(rad.mul(X.mul(X).sub(Y.mul(Y)).mul(0.546274)));
          wsum.addAssign(w);
        });
      });
    });
    // (vars, evaluated here: a node first built inside one branch of the group chain below is declared only there, and
    // the other branches read it unset: NaN on llvmpipe, zero elsewhere, for groups 2 to 6)
    const norm = float(4 * Math.PI).div(wsum.max(1e-6)).toVar();
    const C = c.map((v) => v.mul(norm).toVar());
    const g = proj.y;
    const P = [
      vec4(C[0], C[1].x), vec4(C[1].yz, C[2].xy), vec4(C[2].z, C[3]), vec4(C[4], C[5].x),
      vec4(C[5].yz, C[6].xy), vec4(C[6].z, C[7]), vec4(C[8], 0),
    ];
    const out = vec4(0).toVar();
    let chain = If(g.lessThan(0.5), () => { out.assign(P[0]); });
    for (let i = 1; i < 6; i++) chain = chain.ElseIf(g.lessThan(i + 0.5), () => { out.assign(P[i]); });
    chain.Else(() => { out.assign(P[6]); });
    return out;
  })();
}
