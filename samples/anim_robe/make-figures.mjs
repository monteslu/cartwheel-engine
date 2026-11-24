// Writes samples/anim_robe/project/robed.glb and coated.glb: the robe sample's two figures, generated (no source art).
// Two 1.74 m people (plain bone names, bones unrotated at rest) whose garments hang on bone chains for SpringBone3D
// in skirt mode (cartwheel/ik.h):
//   robed   a long robe from the waist to the ankles: eight chains of four bones round the waist, front first and
//           on round the left (RobeF, RobeFL, RobeL, RobeBL, RobeB, RobeBR, RobeR, RobeFR; RobeF0 .. RobeF3, RobeF_end)
//   coated  a long coat open at the front, from the waist to below the knees: five chains of three bones from the
//           left front round the back to the right front (CoatL, CoatBL, CoatB, CoatBR, CoatR; CoatL0 .. CoatL2, CoatL_end)
// Each garment is one surface skinned to its chains: across, between the two nearest chains; down, along the bones.
// Clips (keyed from joint angles; the garment bones are not keyed): "Run" (a 0.72 s cycle, in place, for 5.4 m/s)
// and "Idle" (arms down, breathing).
//
//   node samples/anim_robe/make-figures.mjs [out dir]
import { writeFileSync, mkdirSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const OUT = process.argv[2] || join(dirname(fileURLToPath(import.meta.url)), 'project');

// ── small math ───────────────────────────────────────────────────────────
const add = (a, b) => [a[0] + b[0], a[1] + b[1], a[2] + b[2]];
const sub = (a, b) => [a[0] - b[0], a[1] - b[1], a[2] - b[2]];
const mul = (a, s) => [a[0] * s, a[1] * s, a[2] * s];
const dot = (a, b) => a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
const cross = (a, b) => [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]];
const len = (a) => Math.hypot(a[0], a[1], a[2]);
const norm = (a) => { const l = len(a) || 1; return [a[0] / l, a[1] / l, a[2] / l]; };
const lerp = (a, b, t) => a + (b - a) * t;
const lerp3 = (a, b, t) => [lerp(a[0], b[0], t), lerp(a[1], b[1], t), lerp(a[2], b[2], t)];
const smooth = (a, b, x) => { const t = Math.min(1, Math.max(0, (x - a) / (b - a))); return t * t * (3 - 2 * t); };
const qmul = (a, b) => [a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1], a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0],
  a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3], a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2]];
const qconj = (q) => [-q[0], -q[1], -q[2], q[3]];
const qrot = (q, v) => { const t = mul(cross(q, v), 2); return add(add(v, mul(t, q[3])), cross(q, t)); };
const qFromTo = (a, b) => { // shortest rotation from unit a to unit b
  const r = dot(a, b) + 1;
  let q = r < 1e-6 ? (Math.abs(a[0]) > Math.abs(a[2]) ? [-a[1], a[0], 0, 0] : [0, -a[2], a[1], 0]) : [...cross(a, b), r];
  const l = Math.hypot(...q); return q.map((x) => x / l);
};
const rotZ = (p, c, ang) => { const d = sub(p, c), s = Math.sin(ang), k = Math.cos(ang); return add(c, [d[0] * k - d[1] * s, d[0] * s + d[1] * k, d[2]]); };

// ── the character ─────────────────────────────────────────────────
const NAMES = {
  plain: { hips: 'Hips', spine: 'Spine', chest: 'Chest', upperChest: 'UpperChest', neck: 'Neck', head: 'Head',
    shoulder: 'Shoulder.#', upperArm: 'UpperArm.#', lowerArm: 'LowerArm.#', hand: 'Grip.#',
    upperLeg: 'UpperLeg.#', lowerLeg: 'LowerLeg.#', foot: 'Foot.#', toes: 'Toes.#', side: ['L', 'R'] },
};
const GARMENTS = {
  // levels from the waist down: height, half width (x) and half depth (z) of the garment there
  robe: { prefix: 'Robe', chains: ['F', 'FL', 'L', 'BL', 'B', 'BR', 'R', 'FR'], angles: [0, 45, 90, 135, 180, 225, 270, 315], closed: true,
    y: [0.99, 0.76, 0.535, 0.315, 0.1], rx: [0.17, 0.225, 0.27, 0.31, 0.345], rz: [0.14, 0.192, 0.24, 0.282, 0.318] },
  coat: { prefix: 'Coat', chains: ['L', 'BL', 'B', 'BR', 'R'], angles: [48, 114, 180, 246, 312], closed: false, arc: [34, 326],
    y: [0.99, 0.775, 0.56, 0.345], rx: [0.175, 0.228, 0.27, 0.305], rz: [0.145, 0.196, 0.242, 0.278] },
};
const CHARACTERS = [
  { file: 'robed', garment: 'robe', names: 'plain', axis: null, clips: true, H: 1.74, hip: 0.515, knee: 0.51, shoulderW: 0.235, hipW: 0.105, build: 1.0, armDown: 0,
    skin: [0.40, 0.23, 0.16], shirt: [0.16, 0.12, 0.07], shorts: [0.05, 0.04, 0.035], shoes: [0.09, 0.05, 0.03], hair: [0.05, 0.035, 0.025], coat: [0.09, 0.14, 0.2] },
  { file: 'coated', garment: 'coat', names: 'plain', axis: null, clips: true, H: 1.74, hip: 0.515, knee: 0.51, shoulderW: 0.24, hipW: 0.105, build: 1.05, armDown: 0,
    skin: [0.62, 0.42, 0.3], shirt: [0.2, 0.19, 0.17], shorts: [0.06, 0.06, 0.07], shoes: [0.05, 0.04, 0.035], hair: [0.3, 0.24, 0.16], coat: [0.25, 0.12, 0.06] },
];
const _unused = [
  { file: 'walker', names: 'plain', axis: null, clips: true, H: 1.74, hip: 0.515, knee: 0.51, shoulderW: 0.235, hipW: 0.105, build: 1.0, armDown: 0,
    skin: [0.40, 0.23, 0.16], shirt: [0.05, 0.075, 0.11], shorts: [0.035, 0.03, 0.028], shoes: [0.09, 0.05, 0.03], hair: [0.12, 0.05, 0.018], coat: [0.05, 0.075, 0.11] },
];

class Mesh {
  constructor() { this.p = []; this.n = []; this.j = []; this.w = []; this.i = []; }
  vert(p, n, joints, weights) {
    this.p.push(...p); this.n.push(...norm(n));
    // the four largest influences
    const pairs = joints.map((x, k) => [x, weights[k]]).filter((x) => x[1] > 1e-4).sort((a, b) => b[1] - a[1]).slice(0, 4);
    const s = pairs.reduce((a, b) => a + b[1], 0);
    const j = [0, 0, 0, 0], w = [0, 0, 0, 0];
    pairs.forEach((x, k) => { j[k] = x[0]; w[k] = x[1] / s; });
    this.j.push(...j); this.w.push(...w);
    return this.p.length / 3 - 1;
  }
  count() { return this.p.length / 3; }
}
// rings swept along a path: pts[k] = { p, rx, rz, j, w }; rx along `side` x tangent, rz along `front`
function sweep(m, pts, segs, front) {
  const base = m.count();
  pts.forEach((pt, k) => {
    const tan = norm(sub(pts[Math.min(k + 1, pts.length - 1)].p, pts[Math.max(k - 1, 0)].p));
    let f = norm(sub(front, mul(tan, dot(front, tan))));
    const side = norm(cross(f, tan));
    for (let s = 0; s <= segs; s++) {
      const a = (s / segs) * Math.PI * 2, c = Math.cos(a), sn = Math.sin(a);
      const p = add(pt.p, add(mul(side, c * pt.rx), mul(f, sn * pt.rz)));
      const n = add(mul(side, c / Math.max(pt.rx, 1e-4)), mul(f, sn / Math.max(pt.rz, 1e-4)));
      m.vert(p, pt.cap ? mul(tan, pt.cap) : n, pt.j, pt.w);
    }
  });
  for (let r = 0; r < pts.length - 1; r++) for (let s = 0; s < segs; s++) {
    const a = base + r * (segs + 1) + s, b = a + segs + 1;
    m.i.push(a, a + 1, b, a + 1, b + 1, b);
  }
}
function ellipsoid(m, c, rad, rows, cols, jw, shape) {
  const base = m.count();
  for (let r = 0; r <= rows; r++) for (let k = 0; k <= cols; k++) {
    const th = (r / rows) * Math.PI, ph = (k / cols) * Math.PI * 2;
    let s = [Math.sin(th) * Math.sin(ph), Math.cos(th), Math.sin(th) * Math.cos(ph)];
    const g = shape ? shape(s) : 1;
    const p = [c[0] + s[0] * rad[0] * g, c[1] + s[1] * rad[1] * g, c[2] + s[2] * rad[2] * g];
    const { j, w } = jw(p);
    m.vert(p, [s[0] / rad[0], s[1] / rad[1], s[2] / rad[2]], j, w);
  }
  for (let r = 0; r < rows; r++) for (let k = 0; k < cols; k++) {
    const a = base + r * (cols + 1) + k, b = a + cols + 1;
    m.i.push(a, b, a + 1, a + 1, b, b + 1);
  }
}

function build(ch) {
  const H = ch.H, N = NAMES[ch.names], B = ch.build;
  // joints in the rest pose (model space: +Y up, +Z forward, +X the character's left)
  const hipY = ch.hip * H, shY = 0.815 * H;
  const J = {};
  const bones = []; // [key, parentKey, name]
  const bone = (key, parent, name, p) => { J[key] = p; bones.push([key, parent, name]); };
  bone('hips', null, N.hips, [0, hipY + 0.045 * H, 0]);
  const torso = shY - J.hips[1];
  bone('spine', 'hips', N.spine, [0, J.hips[1] + torso * 0.2, 0.004 * H]);
  bone('chest', 'spine', N.chest, [0, J.hips[1] + torso * 0.48, 0.006 * H]);
  bone('upperChest', 'chest', N.upperChest, [0, J.hips[1] + torso * 0.74, 0.004 * H]);
  bone('neck', 'upperChest', N.neck, [0, shY + 0.03 * H, 0]);
  bone('head', 'neck', N.head, [0, shY + 0.075 * H, 0.004 * H]);
  for (const [si, sx] of [[0, 1], [1, -1]]) {
    const n = (t) => t.replace('#', N.side[si]), k = (t) => t + 'LR'[si];
    const sh = [sx * ch.shoulderW * H * 0.5, shY - 0.02 * H, 0];
    const el0 = add(sh, [sx * 0.158 * H, 0, 0]), wr0 = add(el0, [sx * 0.142 * H, 0, 0]), tip0 = add(wr0, [sx * 0.05 * H, 0, 0]);
    const ang = -sx * ch.armDown;
    bone(k('shoulder'), 'upperChest', n(N.shoulder), [sx * 0.025 * H, shY - 0.012 * H, 0]);
    bone(k('upperArm'), k('shoulder'), n(N.upperArm), sh);
    bone(k('lowerArm'), k('upperArm'), n(N.lowerArm), rotZ(el0, sh, ang));
    bone(k('hand'), k('lowerArm'), n(N.hand), rotZ(wr0, sh, ang));
    J[k('handTip')] = rotZ(tip0, sh, ang);
    const hx = sx * ch.hipW * H * 0.5 + sx * 0.008 * H; // hip joints under the pelvis: an athletic stance, not a wide one
    const ankleY = 0.048 * H, kneeY = ankleY + (hipY - ankleY) * ch.knee;
    bone(k('upperLeg'), 'hips', n(N.upperLeg), [hx, hipY, 0]);
    bone(k('lowerLeg'), k('upperLeg'), n(N.lowerLeg), [hx, kneeY, 0.006 * H]);
    bone(k('foot'), k('lowerLeg'), n(N.foot), [hx, ankleY, -0.004 * H]);
    bone(k('toes'), k('foot'), n(N.toes), [hx, 0.016 * H, 0.085 * H]);
  }
  // the garment's chains: from the waist down, each joint on the garment's level at the chain's angle
  const G = GARMENTS[ch.garment], NL = G.y.length;
  const gpt = (lv, deg) => { const a = (deg * Math.PI) / 180; return [G.rx[lv] * Math.sin(a), G.y[lv], G.rz[lv] * Math.cos(a)]; };
  G.chains.forEach((c, k) => {
    for (let lv = 0; lv < NL; lv++) {
      const end = lv === NL - 1;
      bone('g' + k + '_' + lv, lv ? 'g' + k + '_' + (lv - 1) : 'hips', G.prefix + c + (end ? '_end' : lv), gpt(lv, G.angles[k]));
    }
  });
  const idx = Object.fromEntries(bones.map((b, i) => [b[0], i]));
  // bone orientations: unrotated, or one local axis along the bone toward its (first) child
  const childOf = {};
  bones.forEach(([key, parent]) => { if (parent && !(parent in childOf)) childOf[parent] = key; });
  const tipOf = { handL: 'handTipL', handR: 'handTipR' };
  const wrot = bones.map(([key]) => {
    if (!ch.axis) return [0, 0, 0, 1];
    const to = childOf[key] ? J[childOf[key]] : tipOf[key] ? J[tipOf[key]] : null;
    if (!to) return [0, 0, 0, 1];
    return qFromTo(ch.axis, norm(sub(to, J[key])));
  });
  // leaf bones without a direction take their parent's orientation
  bones.forEach(([key, parent], i) => { if (ch.axis && !childOf[key] && !tipOf[key] && parent) wrot[i] = wrot[idx[parent]]; });

  // ── meshes, one per material ────────────────────────────────────────────
  const M = { skin: new Mesh(), shirt: new Mesh(), shorts: new Mesh(), shoes: new Mesh(), hair: new Mesh(), eyes: new Mesh(), coat: new Mesh() };
  const FRONT = [0, 0, 1];
  const jw = (pairs) => ({ j: pairs.map((x) => idx[x[0]]), w: pairs.map((x) => x[1]) });
  // torso: pelvis (shorts) then waist to shoulders (shirt)
  const y0 = hipY - 0.03 * H, yWaist = J.hips[1] + torso * 0.14;
  const torsoRing = (y) => {
    const t = (y - y0) / (shY + 0.012 * H - y0);
    // widths: hips, waist, chest, shoulders
    const hipsRx = (ch.hipW * 0.5 + 0.04 * Math.sqrt(B)) * H, waistRx = 0.08 * H * Math.pow(B, 0.3), chestRx = ch.shoulderW * H * 0.5 * 0.86, topRx = ch.shoulderW * H * 0.5 * 0.7;
    const rx = t < 0.2 ? lerp(hipsRx * 0.9, hipsRx, smooth(0, 0.2, t)) : t < 0.42 ? lerp(hipsRx, waistRx, smooth(0.2, 0.42, t))
      : t < 0.8 ? lerp(waistRx, chestRx, smooth(0.42, 0.8, t)) : lerp(chestRx, topRx, smooth(0.8, 1, t));
    // front to back: a flat waist under a deep chest, whatever the build (a heavy build reads as muscle, not belly)
    const rz = (t < 0.42 ? lerp(0.06, 0.05, smooth(0.1, 0.42, t)) * Math.pow(B, 0.2) : t < 0.8 ? lerp(0.05 * Math.pow(B, 0.2), 0.068 * Math.pow(B, 0.8), smooth(0.42, 0.8, t)) : lerp(0.068, 0.05, smooth(0.8, 1, t)) * Math.pow(B, 0.8)) * H;
    const ys = [J.hips[1], J.spine[1], J.chest[1], J.upperChest[1]], keys = ['hips', 'spine', 'chest', 'upperChest'];
    const w = keys.map((_, k) => {
      const lo = k === 0 ? -1e9 : (ys[k - 1] + ys[k]) / 2, hi = k === 3 ? 1e9 : (ys[k] + ys[k + 1]) / 2, band = 0.035 * H;
      return smooth(lo - band, lo + band, y) * (1 - smooth(hi - band, hi + band, y));
    });
    return { p: [0, y, 0.004 * H], rx, rz, ...jw(keys.map((k2, i2) => [k2, w[i2]])) };
  };
  const ringsBetween = (a, b, n) => Array.from({ length: n + 1 }, (_, k) => torsoRing(lerp(a, b, k / n)));
  const crotchCap = { ...torsoRing(y0), rx: 0.001, rz: 0.001, cap: -1 };
  crotchCap.p = [0, y0 - 0.012 * H, 0.004 * H];
  sweep(M.shorts, [crotchCap, ...ringsBetween(y0, yWaist, 6)], 28, FRONT);
  const top = torsoRing(shY + 0.012 * H);
  sweep(M.shirt, [...ringsBetween(yWaist, shY + 0.012 * H, 16), { ...top, p: [0, shY + 0.03 * H, 0], rx: 0.036 * H, rz: 0.034 * H }], 28, FRONT);
  // neck and head
  const nr = 0.029 * H * Math.pow(B, 0.5);
  sweep(M.skin, [0, 0.5, 1].map((t) => ({ p: lerp3([0, shY + 0.022 * H, 0], [0, J.head[1] + 0.02 * H, 0.004 * H], t), rx: nr, rz: nr * 0.95,
    ...jw([['upperChest', 1 - smooth(0, 0.5, t)], ['neck', smooth(0, 0.5, t) * (1 - smooth(0.5, 1, t)) + 0.001], ['head', smooth(0.5, 1, t)]]) })), 16, FRONT);
  const headC = [0, H - 0.064 * H, 0.012 * H], headR = [0.046 * H, 0.066 * H, 0.056 * H];
  const headJ = () => jw([['head', 1]]);
  ellipsoid(M.skin, headC, headR, 18, 24, headJ, (s) => 1 - 0.13 * smooth(-0.1, -0.9, s[1]) * (1 - 0.4 * Math.max(0, s[2]))); // a narrower jaw
  ellipsoid(M.skin, add(headC, [0, -0.008 * H, headR[2] * 0.98]), [0.007 * H, 0.012 * H, 0.008 * H], 6, 8, headJ); // nose
  ellipsoid(M.hair, add(headC, [0, 0.008 * H, -0.005 * H]), [headR[0] * 1.07, headR[1] * 1.04, headR[2] * 1.06], 16, 24, headJ,
    (s) => (s[1] > lerp(0.05, 0.5, Math.max(0, s[2])) - 0.35 * Math.max(0, -s[2]) ? 1 : 0.9));
  for (const sx of [1, -1]) ellipsoid(M.eyes, add(headC, [sx * 0.017 * H, 0.006 * H, headR[2] * 0.9]), [0.0055 * H, 0.0045 * H, 0.004 * H], 6, 8, headJ);
  // limbs
  for (const S of ['L', 'R']) {
    const sx = S === 'L' ? 1 : -1;
    // arm: a path from the shoulder joint through the elbow to the wrist
    const sh = J['upperArm' + S], el = J['lowerArm' + S], wr = J['hand' + S], tip = J['handTip' + S];
    const armPt = (t) => { // t 0..1 shoulder..elbow, 1..2 elbow..wrist
      const p = t <= 1 ? lerp3(sh, el, t) : lerp3(el, wr, t - 1);
      const r = t <= 1 ? lerp(0.039, 0.029, t) + 0.006 * Math.sin(Math.PI * Math.min(1, t * 1.15)) : lerp(0.029, 0.02, t - 1) + 0.005 * Math.sin(Math.PI * Math.min(1, (t - 1) * 1.6));
      const we = smooth(0.82, 1.18, t);
      return { p, rx: r * H * B, rz: r * H * B, ...jw([['upperArm' + S, 1 - we], ['lowerArm' + S, we * (1 - smooth(1.9, 2.05, t))], ['hand' + S, smooth(1.9, 2.05, t) + 0.0001]]) };
    };
    const armFront = FRONT;
    const steps = (a, b, n) => Array.from({ length: n + 1 }, (_, k) => armPt(lerp(a, b, k / n)));
    const cap0 = armPt(0);
    // the shoulder: a rounded cap that joins the sleeve to the chest
    ellipsoid(M.shirt, add(sh, [-sx * 0.004 * H, 0.002 * H, 0]), [0.041 * H * B, 0.039 * H * B, 0.04 * H * B], 10, 14,
      () => jw([['upperChest', 0.45], ['shoulder' + S, 0.25], ['upperArm' + S, 0.3]]));
    sweep(M.shirt, [{ ...cap0, rx: cap0.rx * 0.9, rz: cap0.rz * 0.9 }, ...steps(0.02, 0.42, 5)], 14, armFront);
    sweep(M.skin, steps(0.42, 2, 20), 14, armFront);
    const hd = norm(sub(tip, wr));
    const hc = add(wr, mul(hd, 0.036 * H));
    // the hand: a flattened mitten along the arm, thin front to back
    const handQ = qFromTo([1, 0, 0], hd);
    const hm = M.skin, hb = hm.count();
    ellipsoid(hm, [0, 0, 0], [0.042 * H, 0.021 * H * Math.sqrt(B), 0.0125 * H * Math.sqrt(B)], 8, 12, () => jw([['hand' + S, 1]]));
    for (let v = hb; v < hm.count(); v++) {
      const p = add(hc, qrot(handQ, hm.p.slice(v * 3, v * 3 + 3))), n = qrot(handQ, hm.n.slice(v * 3, v * 3 + 3));
      hm.p.splice(v * 3, 3, ...p); hm.n.splice(v * 3, 3, ...n);
    }
    // leg
    const hp = J['upperLeg' + S], kn = J['lowerLeg' + S], an = J['foot' + S];
    const legPt = (t) => {
      const p = t <= 1 ? lerp3(hp, kn, t) : lerp3(kn, an, t - 1);
      const r = t <= 1 ? lerp(0.047, 0.035, Math.pow(t, 0.9)) + 0.005 * Math.sin(Math.PI * t) : lerp(0.037, 0.024, t - 1) + 0.011 * Math.sin(Math.PI * Math.min(1, (t - 1) * 1.45)) * (1 - (t - 1) * 0.3);
      const wk = smooth(0.84, 1.16, t), wa = smooth(1.88, 2.04, t);
      const wh = 1 - smooth(-0.05, 0.22, t); // the top of the thigh follows the pelvis a little
      // legs lean: a heavy build is broad shoulders and arms (muscle), its legs only a little thicker
      const LB = Math.sqrt(B);
      return { p, rx: r * H * LB * (t < 0.15 ? 0.96 : 1), rz: r * H * LB * 1.04,
        ...jw([['hips', wh * 0.45], ['upperLeg' + S, (1 - wh * 0.45) * (1 - wk)], ['lowerLeg' + S, wk * (1 - wa)], ['foot' + S, wa + 0.0001]]) };
    };
    const lsteps = (a, b, n) => Array.from({ length: n + 1 }, (_, k) => legPt(lerp(a, b, k / n)));
    const topCap = legPt(0);
    // the shorts' legs start inside the pelvis (no cap to poke out at the hip) and sit just over the thigh
    sweep(M.shorts, [{ ...topCap, p: add(topCap.p, [-sx * 0.012 * H, 0.035 * H, 0]), rx: topCap.rx * 0.8, rz: topCap.rz * 0.85 }, ...lsteps(0.06, 0.5, 7)].map((x) => ({ ...x, rx: x.rx * 1.035, rz: x.rz * 1.035 })), 16, FRONT);
    sweep(M.skin, lsteps(0.5, 1.94, 22), 16, FRONT);
    // shoe: an ellipsoid from the heel to the toes, flat underneath
    const toe = J['toes' + S];
    ellipsoid(M.shoes, [an[0], 0.03 * H, lerp(an[2], toe[2], 0.42)], [0.03 * H * Math.sqrt(B), 0.03 * H, 0.078 * H], 10, 14,
      (p) => { const wt = smooth(toe[2] - 0.02 * H, toe[2] + 0.02 * H, p[2]); return jw([['foot' + S, 1 - wt], ['toes' + S, wt + 0.0001]]); },
      (s) => (s[1] < -0.55 ? 0.62 / Math.max(0.62, -s[1]) * 1.0 : 1) * (1 + 0.1 * Math.max(0, s[2])));
    sweep(M.shoes, [1.9, 2.0].map((t) => ({ ...legPt(t), rx: legPt(t).rx * 1.12, rz: legPt(t).rz * 1.12 })), 14, FRONT); // the ankle collar
  }

  // the garment: one surface, rings from the waist to the hem; across, the two nearest chains share each vertex by
  // angle; down, the bone the vertex lies on, blended across each joint. Its two sides are drawn (double sided).
  {
    const ang = G.angles, nc = ang.length, arc = G.closed ? [0, 360] : G.arc;
    const segs = G.closed ? 64 : 48, rows = 12 * (NL - 1);
    const lvAt = (t) => { const k = Math.min(NL - 2, Math.floor(t)), f = t - k; return { k, f }; };
    const lerpL = (arr, t) => { const { k, f } = lvAt(t); return lerp(arr[k], arr[k + 1], f); };
    const gm = M.coat, base = gm.count();
    for (let r = 0; r <= rows; r++) {
      const t = (r / rows) * (NL - 1), y = lerpL(G.y, t), rx = lerpL(G.rx, t), rz = lerpL(G.rz, t);
      for (let sI = 0; sI <= segs; sI++) {
        const deg = arc[0] + ((arc[1] - arc[0]) * sI) / segs, a = (deg * Math.PI) / 180;
        const p = [rx * Math.sin(a), y, rz * Math.cos(a) * (1 + 0.025 * Math.sin(a * 6) * t / (NL - 1))];
        // the two chains either side by angle
        let k0 = nc - 1, k1 = 0, u = 0;
        for (let k = 0; k < nc; k++) {
          const a0 = ang[k], a1 = k + 1 < nc ? ang[k + 1] : ang[0] + 360;
          let dd = deg; if (dd < a0) dd += 360;
          if (dd >= a0 && dd <= a1) { k0 = k; k1 = (k + 1) % nc; u = (dd - a0) / (a1 - a0); break; }
        }
        if (!G.closed && deg < ang[0]) { k0 = k1 = 0; u = 0; }
        if (!G.closed && deg > ang[nc - 1]) { k0 = k1 = nc - 1; u = 0; }
        const bl = smooth(0, 1, u);
        // down: the bone of the level the vertex lies in, blended over a quarter level either side of each joint
        const { k, f } = lvAt(Math.min(t, NL - 1.0001));
        const pairsDown = [[k, 1]];
        if (f < 0.25 && k > 0) { const b = smooth(-0.25, 0.25, f); pairsDown[0][1] = b; pairsDown.push([k - 1, 1 - b]); }
        if (f > 0.75 && k < NL - 2) { const b = smooth(0.75, 1.25, f); pairsDown[0][1] = 1 - b; pairsDown.push([k + 1, b]); }
        const top = 1 - smooth(0, 0.15, t); // the waistband stays on the hips
        const pairs = [['hips', top]];
        for (const [lv, w] of pairsDown) {
          pairs.push(['g' + k0 + '_' + lv, (1 - top) * w * (1 - bl)]);
          if (k1 !== k0) pairs.push(['g' + k1 + '_' + lv, (1 - top) * w * bl]);
        }
        const n = [Math.sin(a) / rx, 0.15, Math.cos(a) / rz];
        gm.vert(p, n, ...Object.values(jw(pairs)));
      }
    }
    for (let r = 0; r < rows; r++) for (let sI = 0; sI < segs; sI++) {
      const a = base + r * (segs + 1) + sI, b = a + segs + 1;
      gm.i.push(a, b, a + 1, a + 1, b, b + 1);
    }
  }

  // ── glTF ────────────────────────────────────────────────────────────────
  const bin = [], views = [], accessors = [];
  let binLen = 0;
  const view = (bytes, target) => {
    const pad4 = (4 - (binLen % 4)) % 4;
    if (pad4) { bin.push(new Uint8Array(pad4)); binLen += pad4; }
    views.push({ buffer: 0, byteOffset: binLen, byteLength: bytes.byteLength, ...(target ? { target } : {}) });
    bin.push(new Uint8Array(bytes.buffer, bytes.byteOffset, bytes.byteLength)); binLen += bytes.byteLength;
    return views.length - 1;
  };
  const acc = (arr, type, ct, target, minmax) => {
    const n = { SCALAR: 1, VEC3: 3, VEC4: 4, MAT4: 16 }[type];
    const T = ct === 5126 ? Float32Array : ct === 5123 ? Uint16Array : Uint8Array;
    const a = { bufferView: view(new T(arr), target), componentType: ct, count: arr.length / n, type };
    if (minmax) {
      a.min = []; a.max = [];
      for (let k = 0; k < n; k++) { let lo = Infinity, hi = -Infinity; for (let i = k; i < arr.length; i += n) { lo = Math.min(lo, arr[i]); hi = Math.max(hi, arr[i]); } a.min.push(Math.fround(lo)); a.max.push(Math.fround(hi)); }
    }
    accessors.push(a); return accessors.length - 1;
  };
  const mat = (name, c, rough, extra = {}) => ({ name, pbrMetallicRoughness: { baseColorFactor: [...c, 1], metallicFactor: 0, roughnessFactor: rough }, ...extra });
  const materials = [
    mat('Skin', ch.skin, 0.62),
    mat('Shirt', ch.shirt, 0.97),  // matte cotton
    mat('Shorts', ch.shorts, 0.96),
    mat('Shoes', ch.shoes, 0.7), mat('Hair', ch.hair, 0.8), mat('Eyes', [0.03, 0.03, 0.04], 0.35),
    mat('Coat', ch.coat, 0.95, { doubleSided: true }), // matte wool, both sides
  ];
  const order = ['skin', 'shirt', 'shorts', 'shoes', 'hair', 'eyes', 'coat'];
  const prims = order.map((k, mi) => {
    const m = M[k];
    return { attributes: { POSITION: acc(m.p, 'VEC3', 5126, 34962, true), NORMAL: acc(m.n, 'VEC3', 5126, 34962),
      JOINTS_0: acc(m.j, 'VEC4', 5121, 34962), WEIGHTS_0: acc(m.w, 'VEC4', 5126, 34962) }, indices: acc(m.i, 'SCALAR', 5123, 34963), material: mi };
  });
  const nodes = bones.map(([key, parent, name], i) => {
    const pq = parent ? wrot[idx[parent]] : [0, 0, 0, 1], pp = parent ? J[parent] : [0, 0, 0];
    const t = qrot(qconj(pq), sub(J[key], pp)), r = qmul(qconj(pq), wrot[i]);
    return { name, translation: t.map(Math.fround), rotation: r.map(Math.fround), children: [] };
  });
  bones.forEach(([, parent], i) => { if (parent) nodes[idx[parent]].children.push(i); });
  nodes.forEach((n) => { if (!n.children.length) delete n.children; });
  const ibm = [];
  bones.forEach(([key], i) => { // inverse of (rotation, position): R^T and -R^T p, column major
    const q = qconj(wrot[i]), c0 = qrot(q, [1, 0, 0]), c1 = qrot(q, [0, 1, 0]), c2 = qrot(q, [0, 0, 1]), t = mul(qrot(q, J[key]), -1);
    ibm.push(c0[0], c0[1], c0[2], 0, c1[0], c1[1], c1[2], 0, c2[0], c2[1], c2[2], 0, t[0], t[1], t[2], 1);
  });
  nodes.push({ name: 'Body', mesh: 0, skin: 0 });
  const animations = ch.clips ? makeClips(ch, J, idx, acc) : [];
  const gltf = {
    asset: { version: '2.0', generator: 'cartwheel samples/anim_robe/make-figures.mjs' },
    scene: 0, scenes: [{ name: ch.file, nodes: [0, nodes.length - 1] }],
    nodes, meshes: [{ name: 'Body', primitives: prims }], materials,
    skins: [{ name: 'Rig', joints: bones.map((_, i) => i), skeleton: 0, inverseBindMatrices: acc(ibm, 'MAT4', 5126) }],
    buffers: [{ byteLength: 0 }], bufferViews: views, accessors,
  };
  if (animations.length) gltf.animations = animations;
  let binBuf = Buffer.concat(bin.map((b) => Buffer.from(b)));
  binBuf = Buffer.concat([binBuf, Buffer.alloc((4 - (binBuf.length % 4)) % 4)]);
  gltf.buffers[0].byteLength = binBuf.length;
  let json = Buffer.from(JSON.stringify(gltf));
  json = Buffer.concat([json, Buffer.alloc((4 - (json.length % 4)) % 4, 0x20)]);
  const header = Buffer.alloc(12), jh = Buffer.alloc(8), bh = Buffer.alloc(8);
  header.writeUInt32LE(0x46546c67, 0); header.writeUInt32LE(2, 4); header.writeUInt32LE(12 + 8 + json.length + 8 + binBuf.length, 8);
  jh.writeUInt32LE(json.length, 0); jh.writeUInt32LE(0x4e4f534a, 4);
  bh.writeUInt32LE(binBuf.length, 0); bh.writeUInt32LE(0x004e4942, 4);
  mkdirSync(OUT, { recursive: true });
  const out = join(OUT, `${ch.file}.glb`);
  writeFileSync(out, Buffer.concat([header, jh, json, bh, binBuf]));
  const legLen = len(sub(J.upperLegL, J.lowerLegL)) + len(sub(J.lowerLegL, J.footL));
  console.log(`${out}: ${order.reduce((s, k) => s + M[k].count(), 0)} vertices, ${bones.length} bones (${ch.names} names), ` +
    `height ${H} m (${(H / (headR[1] * 2)).toFixed(2)} heads), leg ${legLen.toFixed(3)} m, thigh:shin ${(len(sub(J.upperLegL, J.lowerLegL)) / len(sub(J.lowerLegL, J.footL))).toFixed(2)}`);
}
// ── the clips: joint angles over time (bones unrotated at rest: +X the left, +Y up, +Z forward) ──
function makeClips(ch, J, idx, acc) {
  const qx = (a) => [Math.sin(a / 2), 0, 0, Math.cos(a / 2)], qy = (a) => [0, Math.sin(a / 2), 0, Math.cos(a / 2)], qz = (a) => [0, 0, Math.sin(a / 2), Math.cos(a / 2)];
  const mulq = (...qs) => qs.reduce((a, b) => qmul(a, b));
  const L1 = len(sub(J.lowerLegL, J.upperLegL)), L2 = len(sub(J.footL, J.lowerLegL)), hipDrop = J.hips[1] - J.upperLegL[1], ankle = J.footL[1];
  // a pose: rotations by bone key; the hips' height puts the lower ankle at its rest height
  const grounded = (pose, legs) => { // legs: [thighForward, kneeFlex] per side, angles in the sagittal plane
    const reach = legs.map(([f, k]) => L1 * Math.cos(f) + L2 * Math.cos(f - k));
    return { ...pose, hipsY: Math.max(...reach) + hipDrop + ankle };
  };
  const walk = (t) => {
    const ph = 2 * Math.PI * t, legs = [], pose = {};
    for (const [S, o, sx] of [['L', 0, 1], ['R', Math.PI, -1]]) {
      const p = ph + o;
      const f = 0.44 * Math.sin(p);                                         // the thigh forward of vertical
      const k = 0.1 + 0.95 * Math.max(0, Math.cos(p - 0.35)) ** 2;          // the knee flexes through the swing
      const toe = 0.3 * Math.max(0, Math.cos(p + 2.5)) ** 2;                // heel lifts at push-off
      legs.push([f, k]);
      pose['upperLeg' + S] = qx(-f);
      pose['lowerLeg' + S] = qx(k);
      pose['foot' + S] = qx(f - k + toe);                                   // the sole stays level but for the push-off
      // the arm hangs, and swings against the leg on its side; the elbow bends more on the forward swing
      const a = -0.4 * Math.sin(p);
      pose['upperArm' + S] = mulq(qx(-a), qz(-sx * 1.36));
      pose['lowerArm' + S] = qy(-sx * (0.3 + 0.3 * Math.max(0, a / 0.4)));
    }
    pose.hips = mulq(qy(0.09 * Math.sin(ph)), qz(0.025 * Math.cos(ph)));
    pose.spine = qy(-0.07 * Math.sin(ph));
    pose.chest = mulq(qy(-0.09 * Math.sin(ph)), qx(0.03));
    pose.head = qy(0.05 * Math.sin(ph));
    return grounded(pose, legs);
  };
  void walk;
  const clip = (name, length, fn) => {
    const n = Math.round(length * 30), times = Array.from({ length: n + 1 }, (_, i) => (length * i) / n);
    const input = acc(times.map(Math.fround), 'SCALAR', 5126, undefined, true);
    const poses = times.map(fn), samplers = [], channels = [];
    const keys = [...new Set(poses.flatMap((p) => Object.keys(p)))].filter((k) => k !== 'hipsY');
    for (const k of keys) {
      samplers.push({ input, output: acc(poses.flatMap((p) => (p[k] || [0, 0, 0, 1]).map(Math.fround)), 'VEC4', 5126), interpolation: 'LINEAR' });
      channels.push({ sampler: samplers.length - 1, target: { node: idx[k], path: 'rotation' } });
    }
    samplers.push({ input, output: acc(poses.flatMap((p) => [J.hips[0], p.hipsY, J.hips[2]].map(Math.fround)), 'VEC3', 5126), interpolation: 'LINEAR' });
    channels.push({ sampler: samplers.length - 1, target: { node: idx.hips, path: 'translation' } });
    return { name, samplers, channels };
  };
  const idle = (t) => {
    const ph = 2 * Math.PI * t, pose = {};
    for (const [S, sx] of [['L', 1], ['R', -1]]) {
      pose['upperArm' + S] = mulq(qx(0.05), qz(-sx * 1.36));
      pose['lowerArm' + S] = qy(-sx * 0.25);
      pose['upperLeg' + S] = qx(0); pose['lowerLeg' + S] = qx(0.04); pose['foot' + S] = qx(-0.04);
    }
    pose.chest = qx(0.02 * Math.sin(ph));
    pose.head = qx(-0.015 * Math.sin(ph));
    return grounded(pose, [[0, 0.04], [0, 0.04]]);
  };
  // a run: 0.72 s a cycle (two strides of 1.94 m at 5.4 m/s), thighs swing well forward, the knee folds high
  // through the swing, the trunk leans in, the arms pump with bent elbows
  const run = (t) => {
    const ph = 2 * Math.PI * t, legs = [], pose = {};
    for (const [S, o, sx] of [['L', 0, 1], ['R', Math.PI, -1]]) {
      const p = ph + o;
      const f = 0.12 + 0.62 * Math.sin(p);
      const k = 0.25 + 1.45 * Math.max(0, Math.cos(p - 0.45)) ** 1.5;
      legs.push([f, k]);
      pose['upperLeg' + S] = qx(-f);
      pose['lowerLeg' + S] = qx(k);
      pose['foot' + S] = qx(f - k + 0.2);
      const a = -0.6 * Math.sin(p);
      pose['upperArm' + S] = mulq(qx(-a), qz(-sx * 1.3));
      pose['lowerArm' + S] = qy(-sx * 1.35);
    }
    pose.hips = mulq(qy(0.12 * Math.sin(ph)), qx(0.06));
    pose.spine = mulq(qy(-0.1 * Math.sin(ph)), qx(0.08));
    pose.chest = mulq(qy(-0.1 * Math.sin(ph)), qx(0.04));
    pose.head = mulq(qy(0.08 * Math.sin(ph)), qx(-0.14));
    return grounded(pose, legs);
  };
  return [clip('Run', 0.72, run), clip('Idle', 3, idle)];
}

for (const ch of CHARACTERS) build(ch);
void _unused;
