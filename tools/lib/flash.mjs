// Flash analysis: does a run of frames strobe? The rules are the photosensitive-seizure thresholds of WCAG 2.2
// success criterion 2.3.1 (three flashes or below) and the Harding test they come from, applied to every frame of a
// run at its frame rate (60 a second for a cartwheel test run):
//
//   relative luminance  Y = 0.2126 R + 0.7152 G + 0.0722 B on linear (decoded sRGB) channels, 0..1
//   transition          a change of Y of 0.1 or more from the last extreme the other way, where the darker side is
//                       below 0.8 (changes the same way only stretch the extreme)
//   flash               a pair of opposing transitions
//   red flash           the same on saturated red: per pixel, (R - G - B) x 320 (linear, floored at 0) where
//                       R / (R + G + B) >= 0.8, else 0; a transition is a change of 20 or more
//   area                a flash counts when it covers more than a quarter of a 10-degree field of view. WCAG's
//                       estimate of that field is a 341 x 256 rectangle of a 1024 x 768 screen: a third of the width
//                       by a third of the height. A quarter of it is a block a sixth by a sixth of the screen (2.8%)
//   bound               no more than 3 flashes, and no more than 3 red flashes, in any one-second window
//
// Measures (each over the whole run, every one-second window):
//   regions  the screen in a 12 x 12 grid; every block of 2 x 2 cells (a sixth by a sixth: the area above) at every
//            cell offset is one signal, its mean Y (and its mean red): the most flashes any block has in a second
//   fields   the 10-degree fields (a third by a third, at every twelfth): a field transitions when more than a
//            quarter of its pixels made the same transition within the last 3 frames, net of those that went the
//            other way (scattered small lights switching together count, a moving picture, whose pixels go up and
//            down in equal numbers, does not)
//   flicker  regular flicker (WCAG asks to avoid it, the Harding test looks for it above 3 Hz): each cell of the
//            grid's in-place change (the cell's mean change, weighted by how much its pixels change together, so a
//            picture moving through the cell is not a light flickering in it) over two-second windows (a Hann
//            window, every half second, half-hertz bins); flicker is a spectral peak from 3.5 to 30 Hz (60 frames a
//            second sees up to 30; three flashes a second is the bound itself, so a 3 Hz pulse is not flicker) holding
//            most of the window's varying power, with an amplitude of `flickerAmp` or more, at the same rate in two
//            windows in a row
//   rise     the largest rise of a block's mean Y over two frames (how hard the hardest onset is; a swell rises over a
//            tenth of a second or more). Reported; bounded only when `maxRise` is set
//   motion   the mean change of a pixel between frames: a run whose frames never change proves nothing
//
// Use (a gate):
//   import { flashGate, readFrameDump } from '<engine>/tools/lib/flash.mjs';
//   const { w, h, frames } = readFrameDump('out/run.cwfd');    // CW_FRAME_DUMP (src/platform/native.c)
//   const g = flashGate(w, h, frames);    // the run, and the CONTROL: the same frames with an 8 Hz full-screen strobe
//   console.log(g.lines.join('\n')); if (!g.ok) process.exit(1);
// tools/flash-check.mjs captures and checks one app's run from the command line.

export const FLASH_DEFAULTS = {
  fps: 60,
  grid: 12, // cells across and down
  block: 2, // cells per side of a region block (a sixth of the screen with grid 12)
  field: 4, // cells per side of a 10-degree field (a third)
  step: 0.1, dark: 0.8, // a general transition
  redStep: 20, // a red transition
  maxFlashes: 3, // per one-second window, general and red
  fieldShare: 0.25, // the share of a field that must transition together
  syncFrames: 3, // how close together (frames) its pixels' transitions must be
  flickerAmp: 0.02, // the smallest regular flicker (sine amplitude of a cell's mean Y) that fails
  flickerShare: 0.6, // the share of a window's varying power the peak (3 bins) must hold
  flickerLo: 3.5, flickerHi: 30, // Hz: flicker counts above 3 Hz (three flashes a second is the bound itself), up to what 60 frames show
  flickerWindow: 2, // seconds of each spectrum (half-hertz bins)
  maxRise: null, // optional bound on rise
  minMotion: 0.0005, // a run must change at least this much (mean |delta Y| per pixel per frame) or it proves nothing
};

const LIN = Float32Array.from({ length: 256 }, (_, i) => { const c = i / 255; return c <= 0.04045 ? c / 12.92 : ((c + 0.055) / 1.055) ** 2.4; });

/** a transition tracker over n signals: opposing changes of `step` or more, the darker side under `dark`.
 * update(vals) returns nothing; onT(i, dir) is called on each transition (+1 up, -1 down) */
function tracker(n, step, dark) {
  const dir = new Int8Array(n), ext = new Float32Array(n), lo = new Float32Array(n), hi = new Float32Array(n);
  let first = true;
  return (vals, onT) => {
    if (first) { ext.set(vals), lo.set(vals), hi.set(vals), first = false; return; }
    for (let i = 0; i < n; i++) {
      const v = vals[i];
      if (dir[i] === 0) {
        if (v < lo[i]) lo[i] = v;
        if (v > hi[i]) hi[i] = v;
        if (v - lo[i] >= step && lo[i] < dark) dir[i] = 1, ext[i] = v, onT(i, 1);
        else if (hi[i] - v >= step && v < dark) dir[i] = -1, ext[i] = v, onT(i, -1);
      } else if (dir[i] > 0) {
        if (v > ext[i]) ext[i] = v;
        else if (ext[i] - v >= step && v < dark) dir[i] = -1, ext[i] = v, onT(i, -1);
      } else {
        if (v < ext[i]) ext[i] = v;
        else if (v - ext[i] >= step && ext[i] < dark) dir[i] = 1, ext[i] = v, onT(i, 1);
      }
    }
  };
}

/** most flashes (floor(transitions / 2)) of one transition time list in any window of `win` frames: [count, start] */
function mostInWindow(times, win) {
  let best = 0, at = -1;
  for (let a = 0, b = 0; b < times.length; b++) {
    while (times[b] - times[a] >= win) a++;
    const f = (b - a + 1) >> 1;
    if (f > best) best = f, at = times[a];
  }
  return [best, at];
}

/**
 * A streaming analyser for frames of w x h RGB (sRGB bytes, top row first; RGBA when `channels` is 4).
 * @param {number} w @param {number} h @param {Partial<typeof FLASH_DEFAULTS> & {channels?: number}} [opts]
 */
export function createFlashAnalyser(w, h, opts = {}) {
  const o = { ...FLASH_DEFAULTS, ...opts };
  const ch = opts.channels || 3, G = o.grid, n = w * h;
  if (w < G || h < G) throw new Error(`flash: a ${w}x${h} frame is smaller than the ${G}x${G} grid`);
  const cellOf = new Int32Array(n);
  for (let y = 0; y < h; y++) for (let x = 0; x < w; x++) cellOf[y * w + x] = Math.min(G - 1, Math.floor(y * G / h)) * G + Math.min(G - 1, Math.floor(x * G / w));
  const cellN = new Float64Array(G * G);
  for (let i = 0; i < n; i++) cellN[cellOf[i]]++;
  // region blocks (B x B cells at every cell offset) and fields (F x F)
  const blocks = [], fields = [];
  for (let by = 0; by + o.block <= G; by++) for (let bx = 0; bx + o.block <= G; bx++) blocks.push([bx, by]);
  for (let fy = 0; fy + o.field <= G; fy++) for (let fx = 0; fx + o.field <= G; fx++) fields.push([fx, fy]);
  const NB = blocks.length, NF = fields.length;
  const bY = new Float32Array(NB), bR = new Float32Array(NB);
  const blockTrack = tracker(NB, o.step, o.dark), blockRedTrack = tracker(NB, o.redStep, Infinity);
  const blockT = Array.from({ length: NB }, () => []), blockRT = Array.from({ length: NB }, () => []);
  // per-pixel trackers for the fields measure: the last frame each pixel went up / down
  const Y = new Float32Array(n), RD = new Float32Array(n), prevY = new Float32Array(n);
  const pixTrack = tracker(n, o.step, o.dark), pixRedTrack = tracker(n, o.redStep, Infinity);
  const lastUp = new Int32Array(n).fill(-1e9), lastDown = new Int32Array(n).fill(-1e9);
  const lastRUp = new Int32Array(n).fill(-1e9), lastRDown = new Int32Array(n).fill(-1e9);
  const fieldDir = new Int8Array(NF), fieldRDir = new Int8Array(NF);
  const fieldT = Array.from({ length: NF }, () => []), fieldRT = Array.from({ length: NF }, () => []);
  // integral images over the grid's pixel rectangles: fields are F x F cells, measured with per-cell counts
  const cellUp = new Float64Array(G * G), cellDown = new Float64Array(G * G), cellRUp = new Float64Array(G * G), cellRDown = new Float64Array(G * G);
  const cellY = new Float64Array(G * G), cellR = new Float64Array(G * G);
  // flicker reads each cell's in-place change: a frame's mean change of the cell times the square of its coherence (|sum of
  // the pixels' changes| / sum of |changes|: 1 when the whole cell brightens or darkens together, as a light does, near 0
  // when a picture moves through it), summed into a signal
  const history = []; // per frame: Float32Array of the cells' in-place signal
  const cellD = new Float64Array(G * G), cellA = new Float64Array(G * G), coh = new Float32Array(G * G);
  let t = 0, motion = 0, rise = 0, riseAt = -1, riseWhere = -1;
  const bHist = [new Float32Array(NB), new Float32Array(NB)];

  function push(px) {
    if (px.length < n * ch) throw new Error(`flash: frame ${t} has ${px.length} bytes, want ${n * ch}`);
    cellY.fill(0), cellR.fill(0), cellD.fill(0), cellA.fill(0);
    let dsum = 0;
    for (let i = 0, j = 0; i < n; i++, j += ch) {
      const r = LIN[px[j]], g = LIN[px[j + 1]], b = LIN[px[j + 2]];
      const y = 0.2126 * r + 0.7152 * g + 0.0722 * b, s = r + g + b;
      const rd = s > 0 && r / s >= 0.8 ? Math.max(0, (r - g - b) * 320) : 0;
      if (t) { const d = y - prevY[i]; dsum += Math.abs(d), cellD[cellOf[i]] += d, cellA[cellOf[i]] += Math.abs(d); }
      prevY[i] = Y[i] = y, RD[i] = rd;
      cellY[cellOf[i]] += y, cellR[cellOf[i]] += rd;
    }
    if (t) motion += dsum / n;
    for (let c = 0; c < G * G; c++) cellY[c] /= cellN[c], cellR[c] /= cellN[c];
    for (let c = 0; c < G * G; c++) coh[c] = (t ? coh[c] : 0) + (cellA[c] > 0 ? (cellD[c] / cellN[c]) * (cellD[c] / cellA[c]) ** 2 : 0);
    history.push(Float32Array.from(coh));
    // regions
    for (let k = 0; k < NB; k++) {
      const [bx, by] = blocks[k];
      let sy = 0, sr = 0, sn = 0;
      for (let y = by; y < by + o.block; y++) for (let x = bx; x < bx + o.block; x++) { const c = y * G + x; sy += cellY[c] * cellN[c], sr += cellR[c] * cellN[c], sn += cellN[c]; }
      bY[k] = sy / sn, bR[k] = sr / sn;
    }
    if (t >= 2) for (let k = 0; k < NB; k++) { const d = bY[k] - bHist[t % 2][k]; if (d > rise) rise = d, riseAt = t, riseWhere = k; }
    bHist[t % 2].set(bY);
    blockTrack(bY, (k) => blockT[k].push(t));
    blockRedTrack(bR, (k) => blockRT[k].push(t));
    // fields: pixel transitions, counted per cell over the last syncFrames frames
    pixTrack(Y, (i, d) => { if (d > 0) lastUp[i] = t; else lastDown[i] = t; });
    pixRedTrack(RD, (i, d) => { if (d > 0) lastRUp[i] = t; else lastRDown[i] = t; });
    cellUp.fill(0), cellDown.fill(0), cellRUp.fill(0), cellRDown.fill(0);
    const since = t - o.syncFrames;
    for (let i = 0; i < n; i++) {
      const c = cellOf[i];
      if (lastUp[i] > since) cellUp[c]++;
      if (lastDown[i] > since) cellDown[c]++;
      if (lastRUp[i] > since) cellRUp[c]++;
      if (lastRDown[i] > since) cellRDown[c]++;
    }
    for (let k = 0; k < NF; k++) {
      const [fx, fy] = fields[k];
      let up = 0, down = 0, rup = 0, rdown = 0, sn = 0;
      for (let y = fy; y < fy + o.field; y++) for (let x = fx; x < fx + o.field; x++) { const c = y * G + x; up += cellUp[c], down += cellDown[c], rup += cellRUp[c], rdown += cellRDown[c], sn += cellN[c]; }
      const step = (dirs, list, u, d) => {
        // the net share: a moving picture has as many pixels going down as up, a flash moves them one way
        if ((u - d) / sn > o.fieldShare && dirs[k] !== 1) dirs[k] = 1, list[k].push(t);
        else if ((d - u) / sn > o.fieldShare && dirs[k] !== -1) dirs[k] = -1, list[k].push(t);
      };
      step(fieldDir, fieldT, up, down);
      step(fieldRDir, fieldRT, rup, rdown);
    }
    t++;
  }

  function flicker() {
    const N = Math.round(o.fps * o.flickerWindow), hop = Math.max(1, o.fps >> 1), C = G * G;
    const hann = Float64Array.from({ length: N }, (_, i) => 0.5 - 0.5 * Math.cos((2 * Math.PI * i) / (N - 1)));
    const wsum = hann.reduce((a, b) => a + b, 0);
    const kLo = Math.max(1, Math.ceil(o.flickerLo * N / o.fps)), kHi = Math.min(N >> 1, Math.round(o.flickerHi * N / o.fps));
    const cosT = [], sinT = [];
    for (let k = 0; k <= N >> 1; k++) { cosT.push(Float64Array.from({ length: N }, (_, i) => Math.cos((2 * Math.PI * k * i) / N))); sinT.push(Float64Array.from({ length: N }, (_, i) => Math.sin((2 * Math.PI * k * i) / N))); }
    let worst = { amp: 0, hz: 0, cell: -1, at: -1, held: false };
    const prevPeak = new Int32Array(C).fill(-99);
    const sig = new Float64Array(N);
    for (let a = 0; a + N <= history.length; a += hop) {
      for (let c = 0; c < C; c++) {
        let mean = 0;
        for (let i = 0; i < N; i++) mean += history[a + i][c];
        mean /= N;
        let range = 0;
        for (let i = 0; i < N; i++) { const v = history[a + i][c] - mean; sig[i] = v * hann[i]; range = Math.max(range, Math.abs(v)); }
        if (range < o.flickerAmp) { prevPeak[c] = -99; continue; }
        const pw = new Float64Array((N >> 1) + 1);
        let total = 0;
        for (let k = 1; k <= N >> 1; k++) {
          let re = 0, im = 0;
          const ck = cosT[k], sk = sinT[k];
          for (let i = 0; i < N; i++) re += sig[i] * ck[i], im -= sig[i] * sk[i];
          pw[k] = re * re + im * im, total += pw[k];
        }
        let pk = -1, pv = 0;
        for (let k = kLo; k <= kHi; k++) if (pw[k] > pv) pv = pw[k], pk = k;
        // a peak of the whole spectrum (the edge of a slower pulse's fundamental is not one)
        if (pk < 0 || total <= 0 || pw[pk - 1] > pw[pk] || (pk < N >> 1 && pw[pk + 1] > pw[pk])) { prevPeak[c] = -99; continue; }
        const near = pw[pk] + (pw[pk - 1] || 0) + (pk + 1 <= N >> 1 ? pw[pk + 1] : 0);
        const amp = (2 * Math.sqrt(near)) / wsum; // the sine amplitude, the peak's power spread over its neighbours put back
        const regular = near / total >= o.flickerShare && amp >= o.flickerAmp;
        if (regular) {
          const held = Math.abs(prevPeak[c] - pk) <= 2;
          if (held && amp > worst.amp) worst = { amp, hz: (pk * o.fps) / N, cell: c, at: a, held };
          prevPeak[c] = pk;
        } else prevPeak[c] = -99;
      }
    }
    return worst;
  }

  function result() {
    const most = (lists, items) => {
      let best = 0, at = -1, where = -1;
      lists.forEach((l, k) => { const [f, a] = mostInWindow(l, o.fps); if (f > best) best = f, at = a, where = k; });
      const it = where >= 0 ? items[where] : null;
      return { flashes: best, at, where: it ? { x: it[0] / G, y: it[1] / G } : null };
    };
    const blockXY = blocks[riseWhere];
    const fl = flicker();
    return {
      frames: t, width: w, height: h, opts: o,
      regions: most(blockT, blocks), regionsRed: most(blockRT, blocks),
      fields: most(fieldT, fields), fieldsRed: most(fieldRT, fields),
      flicker: { amp: fl.amp, hz: fl.hz, at: fl.at, where: fl.cell >= 0 ? { x: (fl.cell % G) / G, y: Math.floor(fl.cell / G) / G } : null },
      rise: { amount: rise, at: riseAt, where: blockXY ? { x: blockXY[0] / G, y: blockXY[1] / G } : null },
      motion: t > 1 ? motion / (t - 1) : 0,
    };
  }
  return { push, result };
}

const pct = (v) => `${Math.round(v * 100)}%`;
const at = (m, fps) => (m.at >= 0 ? ` from frame ${m.at} (${(m.at / fps).toFixed(2)} s)` : '') + (m.where ? ` at ${pct(m.where.x)},${pct(m.where.y)} of the screen` : '');

/** the reasons a result fails its bounds ([] = it passes) */
export function judgeFlash(m) {
  const o = m.opts, why = [];
  const f = (name, r) => { if (r.flashes > o.maxFlashes) why.push(`${name}: ${r.flashes} flashes in one second (at most ${o.maxFlashes})${at(r, o.fps)}`); };
  f('a sixth of the screen', m.regions);
  f('a sixth of the screen, red', m.regionsRed);
  f('a quarter of a 10-degree field together', m.fields);
  f('a quarter of a 10-degree field together, red', m.fieldsRed);
  if (m.flicker.amp >= o.flickerAmp) why.push(`regular flicker at ${m.flicker.hz.toFixed(1)} Hz, amplitude ${m.flicker.amp.toFixed(3)} (under ${o.flickerAmp})${at(m.flicker, o.fps)}`);
  if (o.maxRise != null && m.rise.amount > o.maxRise) why.push(`a hard onset: a sixth of the screen rose by ${m.rise.amount.toFixed(3)} in two frames (at most ${o.maxRise})${at(m.rise, o.fps)}`);
  if (m.motion < o.minMotion) why.push(`the frames hardly change (mean change ${m.motion.toFixed(5)} a frame, at least ${o.minMotion}): this run proves nothing`);
  return why;
}

/** one line of a result's numbers */
export function describeFlash(m) {
  return `${m.frames} frames ${m.width}x${m.height}: flashes in a second ${m.regions.flashes} (red ${m.regionsRed.flashes}), fields ${m.fields.flashes} (red ${m.fieldsRed.flashes}); ` +
    `flicker ${m.flicker.amp >= 0.001 ? `${m.flicker.amp.toFixed(3)} at ${m.flicker.hz.toFixed(1)} Hz` : 'none'}; hardest two-frame rise ${m.rise.amount.toFixed(3)}; motion ${m.motion.toFixed(4)}`;
}

/** analyse a list of frames (each Uint8Array RGB) */
export function analyseFrames(w, h, frames, opts) {
  const a = createFlashAnalyser(w, h, opts);
  for (const f of frames) a.push(f);
  return a.result();
}

/**
 * The CONTROL a flash check must fail: the same frames with a full-screen strobe laid over them, white mixed in at
 * `mix` on alternate halves of each cycle (8 Hz by default: on 4 frames, off 3 or 4).
 * @returns {Uint8Array[]}
 */
export function withStrobe(frames, { hz = 8, mix = 0.7, fps = 60, color = [255, 255, 255], area = 1 } = {}) {
  return frames.map((f, i) => {
    const on = Math.floor((i * hz * 2) / fps) % 2 === 1;
    if (!on) return f;
    const out = Uint8Array.from(f);
    const len = Math.floor((out.length / 3) * area) * 3;
    for (let j = 0; j < len; j += 3) for (let c = 0; c < 3; c++) out[j + c] = Math.round(out[j + c] * (1 - mix) + color[c] * mix);
    return out;
  });
}

/**
 * The gate: the run must pass and the control (the run with an 8 Hz full-screen strobe) must fail.
 * @returns {{ok: boolean, result: object, control: object, problems: string[], lines: string[]}}
 */
export function flashGate(w, h, frames, opts = {}, { label = 'run', control = {} } = {}) {
  const result = analyseFrames(w, h, frames, opts);
  const problems = judgeFlash(result);
  const ctl = analyseFrames(w, h, withStrobe(frames, { fps: opts.fps || FLASH_DEFAULTS.fps, ...control }), opts);
  const ctlProblems = judgeFlash(ctl).filter((p) => !/proves nothing/.test(p));
  const lines = [];
  lines.push(`${problems.length ? 'FAIL' : 'ok  '} ${label}: ${describeFlash(result)}`);
  for (const p of problems) lines.push(`     ${p}`);
  const ctlFails = ctlProblems.length > 0;
  lines.push(ctlFails ? `ok   control fails as it must (the run with an ${control.hz || 8} Hz full-screen strobe): ${ctlProblems[0]}`
    : `CONTROL PASSED (a failure): an ${control.hz || 8} Hz full-screen strobe over the run went unseen: ${describeFlash(ctl)}`);
  return { ok: !problems.length && ctlFails, result, control: ctl, problems, lines };
}

/**
 * Read a CW_FRAME_DUMP file (src/platform/native.c): "CWFD", u32 width, u32 height, u32 0, then RGB frames.
 * @param {string} path @returns {{w: number, h: number, frames: Uint8Array[]}}
 */
export async function readFrameDump(path) {
  const { readFileSync } = await import('node:fs');
  const buf = readFileSync(path);
  if (buf.length < 16 || buf.readUInt32LE(0) !== 0x44465743) throw new Error(`${path}: not a CW_FRAME_DUMP file`);
  const w = buf.readUInt32LE(4), h = buf.readUInt32LE(8), size = w * h * 3;
  const count = Math.floor((buf.length - 16) / size);
  if ((buf.length - 16) % size) throw new Error(`${path}: ${buf.length - 16} bytes of frames is not a whole number of ${w}x${h} frames`);
  const frames = [];
  for (let i = 0; i < count; i++) frames.push(new Uint8Array(buf.buffer, buf.byteOffset + 16 + i * size, size));
  return { w, h, frames };
}

/** frames from a directory of PNGs (sorted by the number in their names), as RGB */
export async function readPngFrames(dir) {
  const { readdirSync } = await import('node:fs');
  const { join } = await import('node:path');
  const { readImage } = await import('./png.mjs');
  const names = readdirSync(dir).filter((f) => /\.(png|ppm)$/i.test(f)).sort((a, b) => (+(a.match(/\d+/) || [0])[0]) - (+(b.match(/\d+/) || [0])[0]) || a.localeCompare(b));
  let w = 0, h = 0;
  const frames = names.map((f) => {
    const img = readImage(join(dir, f));
    w = img.w, h = img.h;
    const rgb = new Uint8Array(img.w * img.h * 3);
    for (let i = 0; i < img.w * img.h; i++) rgb[i * 3] = img.rgba[i * 4], rgb[i * 3 + 1] = img.rgba[i * 4 + 1], rgb[i * 3 + 2] = img.rgba[i * 4 + 2];
    return rgb;
  });
  return { w, h, frames };
}
