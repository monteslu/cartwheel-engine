// Generates action2d's pixel art (CC0: drawn here, from code; no external art).
//   node games/action2d/make-art.mjs
// Every sprite is built from flat-coloured parts, then given a one-pixel
// bevel per part (light from the top left) and a dark outline, so the whole
// set shares one look. Writes assets/images/*.png.
import { mkdirSync } from 'node:fs';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { writePng } from '../../tools/lib/png.mjs';

const DIR = join(dirname(fileURLToPath(import.meta.url)), 'assets', 'images');
mkdirSync(DIR, { recursive: true });
// A stateless position hash (no cycle); it loses low bits in doubles, but changing it
// int-math-ok: changing it would change the committed art. New code: Math.imul or tools/lib/rng.mjs.
const hash = (x, y, s = 0) => { let h = (x * 374761393 + y * 668265263 + s * 2147483647) >>> 0; h = ((h ^ (h >>> 13)) * 1274126177) >>> 0; return ((h ^ (h >>> 16)) >>> 0) / 4294967296; };
const hex = (h) => [parseInt(h.slice(1, 3), 16), parseInt(h.slice(3, 5), 16), parseInt(h.slice(5, 7), 16)];
const mix = (a, b, t) => a.map((v, i) => Math.round(v + (b[i] - v) * t));
const OUTLINE = hex('#140f1e');

// ── a sheet: cells of w x h; parts drawn per cell, then finished ─────────
function sheet(cols, rows, cw, ch) {
  const W = cols * cw, H = rows * ch;
  const px = new Uint8Array(W * H * 4);
  const set = (x, y, c, a = 255) => {
    if (x < 0 || y < 0 || x >= W || y >= H) return;
    const i = 4 * (y * W + x); px[i] = c[0]; px[i + 1] = c[1]; px[i + 2] = c[2]; px[i + 3] = a;
  };
  const get = (x, y) => { const i = 4 * (y * W + x); return [px[i], px[i + 1], px[i + 2], px[i + 3]]; };
  return { W, H, px, set, get, cw, ch, cols, rows };
}

// A sprite cell under construction: a part id + colour per pixel.
function cell(w, h) {
  const id = new Int16Array(w * h).fill(-1), col = new Array(w * h).fill(null), flat = new Uint8Array(w * h);
  let next = 0;
  const c = {
    w, h,
    part(color, opts = {}) { return { id: next++, color: hex(color), flat: !!opts.flat }; },
    plot(p, x, y) { x = Math.round(x); y = Math.round(y); if (x < 0 || y < 0 || x >= w || y >= h) return; id[y * w + x] = p.id; col[y * w + x] = p.color; flat[y * w + x] = p.flat ? 1 : 0; },
    rect(p, x, y, rw, rh) { for (let j = 0; j < rh; j++) for (let i = 0; i < rw; i++) c.plot(p, x + i, y + j); },
    ellipse(p, cx, cy, rx, ry) {
      for (let y = Math.floor(cy - ry); y <= Math.ceil(cy + ry); y++) for (let x = Math.floor(cx - rx); x <= Math.ceil(cx + rx); x++) {
        const dx = (x - cx) / rx, dy = (y - cy) / ry;
        if (dx * dx + dy * dy <= 1.0) c.plot(p, x, y);
      }
    },
    poly(p, pts) { // filled polygon, even-odd at pixel centres
      const ys = pts.map((q) => q[1]);
      for (let y = Math.floor(Math.min(...ys)); y <= Math.ceil(Math.max(...ys)); y++) {
        const xs = [];
        for (let i = 0; i < pts.length; i++) {
          const [x0, y0] = pts[i], [x1, y1] = pts[(i + 1) % pts.length];
          const yc = y + 0.5;
          if ((y0 <= yc && y1 > yc) || (y1 <= yc && y0 > yc)) xs.push(x0 + (yc - y0) / (y1 - y0) * (x1 - x0));
        }
        xs.sort((a, b) => a - b);
        for (let k = 0; k + 1 < xs.length; k += 2) for (let x = Math.ceil(xs[k] - 0.5); x <= Math.floor(xs[k + 1] - 0.5); x++) c.plot(p, x, y);
      }
    },
    line(p, x0, y0, x1, y1) { const n = Math.max(Math.abs(x1 - x0), Math.abs(y1 - y0), 1); for (let i = 0; i <= n; i++) c.plot(p, x0 + (x1 - x0) * i / n, y0 + (y1 - y0) * i / n); },
    // finish into the sheet at (ox, oy): bevel + outline; white: hurt flash silhouette
    blit(S, ox, oy, { outline = true, white = false, alpha = 255 } = {}) {
      const at = (x, y) => (x < 0 || y < 0 || x >= w || y >= h ? -1 : id[y * w + x]);
      for (let y = 0; y < h; y++) for (let x = 0; x < w; x++) {
        const k = id[y * w + x];
        if (k < 0) {
          if (outline && (at(x - 1, y) >= 0 || at(x + 1, y) >= 0 || at(x, y - 1) >= 0 || at(x, y + 1) >= 0))
            S.set(ox + x, oy + y, white ? [255, 255, 255] : OUTLINE, alpha);
          continue;
        }
        let color = col[y * w + x];
        if (!flat[y * w + x]) {
          if (at(x - 1, y) !== k || at(x, y - 1) !== k) color = mix(color, [255, 255, 240], 0.22);
          else if (at(x + 1, y) !== k || at(x, y + 1) !== k) color = mix(color, [10, 5, 30], 0.28);
        }
        S.set(ox + x, oy + y, white ? [255, 255, 255] : color, alpha);
      }
    },
  };
  return c;
}

const save = (S, name) => writePng(join(DIR, name), S.W, S.H, S.px);

// ── the hero: 16x16, frames: [down, up, side] x [idle, walk1, walk2, attack], hurt, dead ──
{
  const S = sheet(14, 1, 16, 16);
  const draw = (dir, pose, ox, opts = {}) => {
    const c = cell(16, 16);
    const tunic = c.part('#3b6fd8'), tunic2 = c.part('#2a4a9a'), skin = c.part('#f2c29b'), steel = c.part('#c9d2e3');
    const plume = c.part('#e0453a'), boot = c.part('#5b3a24'), belt = c.part('#7a4b25', { flat: true }), gold = c.part('#ffd04a', { flat: true });
    const eye = c.part('#1b1730', { flat: true }), shield = c.part('#8a5a2e'), rim = c.part('#ffd04a', { flat: true });
    const lunge = pose === 3 ? 1 : 0;
    const bob = pose === 1 || pose === 2 ? 1 : 0;
    if (dir === 0 || dir === 1) { // down / up
      const dy = dir === 0 ? lunge : -lunge;
      // legs
      const l = pose === 1 ? -1 : 0, r = pose === 2 ? -1 : 0;
      c.rect(boot, 5, 12 + l + dy, 2, 3);
      c.rect(boot, 9, 12 + r + dy, 2, 3);
      // body
      c.rect(tunic, 4, 8 + dy - bob * 0, 8, 5);
      c.rect(tunic2, 4, 12 + dy, 8, 1);
      c.rect(belt, 4, 11 + dy, 8, 1);
      if (dir === 0) c.plot(gold, 8, 11 + dy);
      // arms
      c.rect(tunic, 3, 9 + dy, 1, 2); c.plot(skin, 3, 11 + dy);
      c.rect(tunic, 12, 9 + dy, 1, 2); c.plot(skin, 12, 11 + dy);
      // head
      c.ellipse(skin, 7.5, 5 + dy, 3.6, 3.4);
      c.ellipse(steel, 7.5, 3.8 + dy, 4.0, 2.6);
      if (dir === 0) {
        c.rect(steel, 4, 4 + dy, 8, 1);
        c.plot(eye, 6, 6 + dy); c.plot(eye, 9, 6 + dy);
        // shield on the left arm
        c.rect(shield, 1, 8 + dy, 3, 5); c.rect(rim, 1, 8 + dy, 3, 1); c.plot(rim, 2, 10 + dy);
      } else {
        c.ellipse(steel, 7.5, 5 + dy, 3.6, 3.0); // the back of the helmet
        c.rect(shield, 12, 8 + dy, 3, 5); c.rect(rim, 12, 8 + dy, 3, 1);
      }
      c.rect(plume, 7, 0 + dy, 2, 2);
    } else { // side, facing +x
      const dx = lunge;
      const sw = pose === 1 ? 1 : pose === 2 ? -1 : 0;
      c.rect(boot, 6 + sw + dx, 12, 2, 3);
      c.rect(boot, 9 - sw + dx, 12, 2, 3);
      c.rect(tunic, 5 + dx, 8, 7, 5);
      c.rect(tunic2, 5 + dx, 12, 7, 1);
      c.rect(belt, 5 + dx, 11, 7, 1);
      c.ellipse(skin, 8.5 + dx, 5, 3.4, 3.4);
      c.ellipse(steel, 8 + dx, 3.8, 3.8, 2.6);
      c.rect(steel, 5 + dx, 4, 4, 2);
      c.plot(eye, 10 + dx, 6);
      c.rect(plume, 5 + dx, 1, 2, 2);
      // shield on the back, arm forward when attacking
      c.rect(shield, 3 + dx, 8, 2, 5); c.rect(rim, 3 + dx, 8, 2, 1);
      if (pose === 3) { c.rect(tunic, 12 + dx, 9, 2, 1); c.plot(skin, 14, 9); }
      else { c.rect(tunic, 9 + dx, 9, 1, 2); c.plot(skin, 9 + dx, 11); }
    }
    c.blit(S, ox, 0, opts);
  };
  for (let d = 0; d < 3; d++) for (let p = 0; p < 4; p++) draw(d, p, (d * 4 + p) * 16);
  draw(0, 0, 12 * 16, { white: true }); // hurt flash
  { // dead: lying flat
    const c = cell(16, 16);
    const tunic = c.part('#3b6fd8'), steel = c.part('#c9d2e3'), boot = c.part('#5b3a24'), skin = c.part('#f2c29b');
    c.rect(tunic, 4, 10, 7, 4); c.rect(boot, 11, 11, 3, 2); c.ellipse(skin, 3.5, 11.5, 2.4, 2.2); c.ellipse(steel, 3, 10.5, 2.6, 1.8);
    c.blit(S, 13 * 16, 0);
  }
  save(S, 'hero.png');
}

// ── sword slash: 3 frames of 24x24, pointing +x ─────────────────────────
{
  const S = sheet(3, 1, 24, 24);
  for (let f = 0; f < 3; f++) {
    const a0 = -1.3 + f * 0.55, a1 = a0 + 1.2 + f * 0.4;
    for (let y = 0; y < 24; y++) for (let x = 0; x < 24; x++) {
      const dx = x - 6, dy = y - 11.5, r = Math.hypot(dx, dy), a = Math.atan2(dy, dx);
      if (a < a0 || a > a1) continue;
      const t = (a - a0) / (a1 - a0); // 0 = tail, 1 = blade tip
      const outer = 11.5, inner = 11.5 - (2 + 4 * t);
      if (r > outer || r < inner) continue;
      const edge = r > outer - 1.2;
      const fade = f === 2 ? 0.55 : 1;
      S.set(f * 24 + x, y, edge ? [255, 255, 255] : mix(hex('#7fd8ff'), [255, 255, 255], t * 0.6), Math.round((120 + 135 * t) * fade));
    }
  }
  save(S, 'slash.png');
}

// ── slime: 4 bounce frames + hurt, 16x16 ────────────────────────────────
{
  const S = sheet(5, 1, 16, 16);
  const shapes = [[6, 4.6, 10.5], [7, 3.8, 11.3], [5.4, 5.6, 9.5], [6.6, 4.2, 10.9]];
  shapes.forEach(([rx, ry, cy], f) => {
    for (const white of f === 0 ? [false, true] : [false]) {
      const c = cell(16, 16);
      const body = c.part('#4fc451'), shade = c.part('#2f8a3d'), shine = c.part('#e8ffe0', { flat: true }), eye = c.part('#10202a', { flat: true });
      c.ellipse(body, 7.5, cy, rx, ry);
      c.rect(shade, Math.round(7.5 - rx + 1), Math.round(cy + ry - 1), Math.round(rx * 2 - 1), 1);
      c.plot(shine, 5, Math.round(cy - ry + 2)); c.plot(shine, 6, Math.round(cy - ry + 1));
      c.rect(eye, 6, Math.round(cy - 1), 1, 2); c.rect(eye, 9, Math.round(cy - 1), 1, 2);
      c.blit(S, (white ? 4 : f) * 16, 0, { white });
    }
  });
  save(S, 'slime.png');
}

// ── bat: 3 flap frames + hurt, 16x16 ────────────────────────────────────
{
  const S = sheet(4, 1, 16, 16);
  const flap = [-4, 0, 3];
  for (let f = 0; f < 4; f++) {
    const c = cell(16, 16);
    const body = c.part('#7a45b8'), wing = c.part('#4b2a7a'), eye = c.part('#ff4a4a', { flat: true }), ear = c.part('#7a45b8');
    const w = flap[f % 3];
    c.poly(wing, [[7, 7], [1, 7 + w], [2, 10 + w * 0.5], [4, 9], [7, 10]]);
    c.poly(wing, [[8, 7], [14, 7 + w], [13, 10 + w * 0.5], [11, 9], [8, 10]]);
    c.ellipse(body, 7.5, 8.5, 2.6, 3);
    c.plot(ear, 6, 5); c.plot(ear, 9, 5);
    c.plot(eye, 6, 8); c.plot(eye, 9, 8);
    c.blit(S, f * 16, 0, { white: f === 3 });
  }
  save(S, 'bat.png');
}

// ── spitter: a skull totem: idle x2, charge, hurt, 16x16 ────────────────
{
  const S = sheet(4, 1, 16, 16);
  for (let f = 0; f < 4; f++) {
    const c = cell(16, 16);
    const stone = c.part('#6a6f86'), bone = c.part('#e9dfc0'), socket = c.part(f === 2 ? '#ff9a2a' : '#2a1d2e', { flat: true });
    const glow = c.part('#fff1a0', { flat: true }), jaw = c.part('#cfc3a0');
    c.rect(stone, 3, 12, 10, 3);
    c.rect(stone, 4, 11, 8, 1);
    const up = f === 1 ? 1 : 0;
    c.ellipse(bone, 7.5, 6 - up, 4.6, 4.2);
    c.rect(jaw, 5, 9 - up, 6, f === 2 ? 3 : 2);
    c.rect(socket, 5, 5 - up, 2, 2); c.rect(socket, 9, 5 - up, 2, 2);
    if (f === 2) { c.plot(glow, 5, 5); c.plot(glow, 9, 5); }
    c.plot(socket, 7, 8 - up); c.plot(socket, 8, 8 - up);
    c.blit(S, f * 16, 0, { white: f === 3 });
  }
  save(S, 'spitter.png');
}

// ── the boss: 32x32: idle x2, charge, hurt, dead ─────────────────────────
{
  const S = sheet(5, 1, 32, 32);
  for (let f = 0; f < 5; f++) {
    const c = cell(32, 32);
    const armor = c.part('#4a3560'), armor2 = c.part('#2c2040'), horn = c.part('#e8dcc0'), cape = c.part('#8a1e2e');
    const eye = c.part(f === 2 ? '#fff3b0' : '#ff7a2a', { flat: true }), mouth = c.part(f === 2 ? '#ffb040' : '#1a1020', { flat: true });
    const gold = c.part('#ffcf4a'), skull = c.part('#d9cfb2');
    const breathe = f === 1 ? 1 : 0, sink = f === 4 ? 8 : 0;
    // cape behind
    c.poly(cape, [[7, 12 + sink], [25, 12 + sink], [28, 30], [4, 30]]);
    // body
    c.ellipse(armor, 16, 20 + breathe + sink / 2, 9, 8 - sink / 4);
    c.rect(armor2, 9, 25 + sink / 2, 15, 3);
    c.rect(gold, 14, 17 + breathe + sink / 2, 4, 2);
    // arms / fists
    c.ellipse(armor, 6, 21 + breathe + sink / 2, 3, 4); c.ellipse(armor, 26, 21 + breathe + sink / 2, 3, 4);
    c.ellipse(skull, 6, 25 + breathe + sink / 2, 2.5, 2); c.ellipse(skull, 26, 25 + breathe + sink / 2, 2.5, 2);
    // head: a horned skull helm
    const hy = 9 + breathe + sink;
    c.ellipse(skull, 16, hy, 6, 5.5);
    c.poly(horn, [[11, hy - 3], [5, hy - 9], [4, hy - 4], [9, hy]]);
    c.poly(horn, [[21, hy - 3], [27, hy - 9], [28, hy - 4], [23, hy]]);
    c.rect(eye, 12, hy, 3, 2); c.rect(eye, 18, hy, 3, 2);
    c.rect(mouth, 14, hy + 3, 5, f === 2 ? 2 : 1);
    c.blit(S, f * 32, 0, { white: f === 3 });
  }
  save(S, 'boss.png');
}

// ── orbs: enemy (orange) x2, boss (violet) x2; 8x8 ─────────────────────
{
  const S = sheet(4, 1, 8, 8);
  const cols = [['#ff7a2a', '#ffe7a0'], ['#b04aff', '#f6d0ff']];
  for (let k = 0; k < 2; k++) for (let f = 0; f < 2; f++) {
    for (let y = 0; y < 8; y++) for (let x = 0; x < 8; x++) {
      const r = Math.hypot(x - 3.5, y - 3.5), R = f ? 3.2 : 3.7;
      if (r > R) continue;
      const t = r / R;
      S.set((k * 2 + f) * 8 + x, y, mix(hex(cols[k][1]), hex(cols[k][0]), Math.min(1, t * 1.4)), t > 0.85 ? 200 : 255);
    }
  }
  save(S, 'orb.png');
}

// ── pickups: heart x2 (12x12), gem x2 (12x12) ───────────────────────────
{
  const S = sheet(4, 1, 12, 12);
  const heartAt = (c, p, ox, oy, s) => {
    for (let y = 0; y < 12; y++) for (let x = 0; x < 12; x++) {
      const u = (x - 5.5) / s, v = (y - 5) / s;
      const h = (u * u + v * v - 1) ** 3 - u * u * v * v * v;
      if (h <= 0) c.plot(p, x + ox, -y + 10 + oy);
    }
  };
  for (let f = 0; f < 2; f++) {
    const c = cell(12, 12);
    const red = c.part('#e8384a'), shine = c.part('#ffd8d8', { flat: true });
    heartAt(c, red, 0, 0, 4.6);
    c.plot(shine, 3, 3); if (f) { c.plot(shine, 4, 3); c.plot(shine, 3, 4); }
    c.blit(S, f * 12, 0);
  }
  for (let f = 0; f < 2; f++) {
    const c = cell(12, 12);
    const gem = c.part('#38d8e8'), dark = c.part('#1f8aa8'), shine = c.part('#f0ffff', { flat: true });
    c.poly(gem, [[6, 1], [11, 5], [6, 11], [1, 5]]);
    c.poly(dark, [[6, 6], [11, 5], [6, 11]]);
    c.plot(shine, 4 + f, 4); if (f) c.plot(shine, 6, 2);
    c.blit(S, (2 + f) * 12, 0);
  }
  save(S, 'pickups.png');
}

// ── HUD heart: full, half, empty (10x9) ──────────────────────────────────
{
  const S = sheet(3, 1, 10, 9);
  for (let f = 0; f < 3; f++) for (let y = 0; y < 9; y++) for (let x = 0; x < 10; x++) {
    const u = (x - 4.5) / 4.3, v = (4.6 - y) / 4.3;
    const h = (u * u + v * v - 1) ** 3 - u * u * v * v * v;
    const u2 = (x - 4.5) / 3.3, v2 = (4.4 - y) / 3.3;
    const inner = (u2 * u2 + v2 * v2 - 1) ** 3 - u2 * u2 * v2 * v2 * v2 <= 0;
    if (h > 0) continue;
    let c = OUTLINE;
    if (inner) {
      const filled = f === 0 || (f === 1 && x < 5);
      c = filled ? (x <= 3 && y <= 3 ? hex('#ffb0b8') : hex('#e8384a')) : hex('#3a2a40');
    }
    S.set(f * 10 + x, y, c);
  }
  save(S, 'hud_heart.png');
}

// ── puff: 4 frames 16x16 ─────────────────────────────────────────────────
{
  const S = sheet(4, 1, 16, 16);
  for (let f = 0; f < 4; f++) {
    const R = 3 + f * 1.6;
    for (let k = 0; k < 5; k++) {
      const a = k * 1.256 + f * 0.3, cx = 7.5 + Math.cos(a) * f * 1.5, cy = 7.5 + Math.sin(a) * f * 1.5;
      for (let y = 0; y < 16; y++) for (let x = 0; x < 16; x++) {
        const r = Math.hypot(x - cx, y - cy);
        if (r <= R * 0.6) S.set(f * 16 + x, y, mix([255, 255, 255], [160, 150, 190], f / 3), 255 - f * 50);
      }
    }
  }
  save(S, 'puff.png');
}

// ── tiles: 16x16, 8 columns x 4 rows ────────────────────────────────────
// 0-3 floors (A, B, cracked, mossy), 4 rug, 5 rug edge, 6 grate, 7 boss floor
// 8 wall top, 9 wall face, 10/11 wall face + torch sconce (2 frames), 12 pillar
// 13 boss rune A, 14 boss rune B (glow), 15 floor shadow under a wall
// 16 skull, 17 bones, 18 rubble, 19 cobweb, 20 door closed (left), 21 door closed (right),
// 22 door open (left), 23 door open (right), 24 wall face cracked, 25 banner, 26 slab in shadow
{
  const S = sheet(8, 4, 16, 16);
  const tile = (id, fn) => {
    const ox = (id % 8) * 16, oy = Math.floor(id / 8) * 16;
    for (let y = 0; y < 16; y++) for (let x = 0; x < 16; x++) { const c = fn(x, y); if (c) S.set(ox + x, oy + y, c, c[3] ?? 255); }
  };
  const flagstone = (x, y, s, base) => {
    const bx = (y >> 3) & 1 ? (x + 4) & 7 : x & 7; // offset rows of 8x8 stones
    const mortar = (y & 7) === 7 || bx === 7;
    const n = hash(x >> 3, y >> 3, s) * 18 + hash(x, y, s + 1) * 10;
    if (mortar) return mix(base, [0, 0, 0], 0.45);
    if ((y & 7) === 0 || bx === 0) return mix(base, [255, 255, 255], 0.1);
    return base.map((v) => Math.min(255, Math.round(v + n - 12)));
  };
  const F = hex('#4a4e66');
  tile(0, (x, y) => flagstone(x, y, 1, F));
  tile(1, (x, y) => flagstone(x, y, 7, mix(F, hex('#55506a'), 0.6)));
  tile(2, (x, y) => { const d = Math.abs(x - 8 - (y - 8) * 0.6) < 0.8 && y > 2 && y < 13; return d ? hex('#22243a') : flagstone(x, y, 3, F); });
  tile(3, (x, y) => (hash(x, y, 9) < 0.35 + (y > 9 ? 0.3 : 0) ? mix(hex('#3f6a3a'), hex('#5f8a4a'), hash(x, y, 4)) : flagstone(x, y, 5, F)));
  tile(4, (x, y) => { const d = (x + y) % 6 === 0 || (x - y + 16) % 6 === 0; return d ? hex('#c0484a') : hex('#8a2a36'); });
  tile(5, (x, y) => (y < 3 ? hex('#e0b040') : y < 4 ? hex('#5a1a20') : (x + y) % 6 === 0 ? hex('#c0484a') : hex('#8a2a36')));
  tile(6, (x, y) => (x % 4 === 0 || y % 4 === 0 ? hex('#6a6e80') : hex('#14121e')));
  const slab = (x, y) => { // a big cut slab (boss hall): one stone per tile, bevelled
    const B = hex('#3b3050');
    if (x === 15 || y === 15) return mix(B, [0, 0, 0], 0.55);
    if (x === 0 || y === 0) return mix(B, [255, 255, 255], 0.12);
    return B.map((v) => Math.round(v + hash(x >> 1, y >> 1, 13) * 10 - 5));
  };
  tile(7, slab);
  tile(26, (x, y) => mix(slab(x, y), [0, 0, 0], y < 6 ? 0.45 - y * 0.06 : 0.1)); // slab in a wall's shadow
  const brick = (x, y, s, base) => {
    const row = y >> 2, bx = (x + (row & 1) * 4) & 7;
    if ((y & 3) === 3 || bx === 7) return mix(base, [0, 0, 0], 0.5);
    const n = hash(x >> 3, row, s) * 20;
    return base.map((v) => Math.round(v + n - 8 + ((y & 3) === 0 ? 14 : 0)));
  };
  const WALL = hex('#6e5f6a');
  tile(8, (x, y) => { const e = Math.min(x, y, 15 - x, 15 - y); return e === 0 ? hex('#2a2230') : mix(hex('#2e2636'), hex('#3a3044'), hash(x >> 2, y >> 2, 2)); });
  tile(9, (x, y) => (y < 2 ? hex('#8a7a86') : y > 13 ? mix(WALL, [0, 0, 0], 0.55) : brick(x, y, 3, WALL)));
  for (let f = 0; f < 2; f++) tile(10 + f, (x, y) => {
    if (x >= 6 && x <= 9 && y >= 8 && y <= 11) return hex('#3a2a20');      // sconce
    if (x >= 7 && x <= 8 && y >= 11 && y <= 13) return hex('#5a3a24');
    const fx = x - 7.5, fy = y - (f ? 4.5 : 5);
    const flame = fy < 3.5 && fy > -4 && Math.abs(fx) < 2.6 - (fy + 4) * 0.08 - (f ? 0.3 : 0) + Math.sin(y + f * 2) * 0.4;
    if (flame) return fy > 1 ? hex('#ff7a20') : fy > -1.5 ? hex('#ffc040') : hex('#fff0a0');
    return y < 2 ? hex('#8a7a86') : y > 13 ? mix(WALL, [0, 0, 0], 0.55) : brick(x, y, 3, WALL);
  });
  tile(12, (x, y) => { // pillar seen from the top front
    if (y < 5) return Math.min(x, 15 - x) === 0 || y === 0 ? hex('#2a2230') : hex('#9a8f9e');
    if (x < 2 || x > 13) return null;
    const shade = x < 5 ? 0.12 : x > 10 ? -0.35 : 0;
    const base = brick(x, y + 1, 9, hex('#7a6c78'));
    return shade > 0 ? mix(base, [255, 255, 255], shade) : mix(base, [0, 0, 0], -shade);
  });
  for (let f = 0; f < 2; f++) tile(13 + f, (x, y) => {
    const r = Math.hypot(x - 7.5, y - 7.5);
    const ring = Math.abs(r - 5.5) < 0.8 || (r < 4 && (Math.abs(x - 7.5) < 0.6 || Math.abs(y - 7.5) < 0.6));
    if (ring) return f ? hex('#d070ff') : hex('#7a3ab0');
    return flagstone(x, y, 11, hex('#3a2e4a'));
  });
  tile(15, (x, y) => mix(flagstone(x, y, 1, F), [0, 0, 0], y < 6 ? 0.45 - y * 0.06 : 0.1));
  tile(16, (x, y) => { const r = Math.hypot(x - 7.5, (y - 7) * 1.1); if (r < 4.2) return (y === 7 && (x === 6 || x === 9)) || (y === 10 && x > 5 && x < 10 && x % 2) ? hex('#2a2030') : hex('#e0d6bc'); return null; });
  tile(17, (x, y) => ((Math.abs(y - 9 - (x - 8) * 0.4) < 0.9 && x > 2 && x < 13) || (Math.abs(y - 6 + (x - 8) * 0.6) < 0.9 && x > 5 && x < 11) ? hex('#d8ceb4') : null));
  tile(18, (x, y) => { const k = hash(x >> 2, y >> 2, 5); return k < 0.35 && y > 6 ? mix(hex('#5a5468'), hex('#8a849a'), hash(x, y, 1)) : null; });
  tile(19, (x, y) => { const a = Math.atan2(y, x), r = Math.hypot(x, y); return (r < 14 && (Math.abs(a % 0.35) < 0.04 || Math.abs(r % 4) < 0.4)) ? [220, 220, 230, 150] : null; });
  const door = (open, half) => (x, y) => {
    const gx = half ? x : x - 16; // the arch spans two tiles, centred on their seam
    const ax = gx + 0.5, r = Math.hypot(ax, y - 8);
    const inside = (y >= 8 && Math.abs(ax) <= 12) || (y < 8 && r <= 12);
    const frame = !inside || (y >= 8 && Math.abs(ax) > 10.5) || (y < 8 && r > 10.5);
    if (!inside && y < 8 && r > 12) return y < 2 ? hex('#8a7a86') : brick(x, y, 3, WALL);
    if (frame) return mix(hex('#9a8a7a'), [0, 0, 0], hash(x, y, 3) * 0.3);
    if (open) return mix(hex('#06040c'), hex('#1a1228'), (16 - y) / 16 * 0.5);
    if (Math.round(ax + 12) % 4 === 0) return hex('#3a3442');       // bars
    if (y % 6 === 2) return hex('#4a4452');
    return hex('#120e1c');
  };
  tile(20, door(false, false)); tile(21, door(false, true));
  tile(22, door(true, false)); tile(23, door(true, true));
  tile(24, (x, y) => { const c = Math.abs(x - 6 - Math.sin(y * 0.8) * 1.5) < 0.7 && y > 2 && y < 14; return c ? hex('#2a2030') : y < 2 ? hex('#8a7a86') : y > 13 ? mix(WALL, [0, 0, 0], 0.55) : brick(x, y, 3, WALL); });
  tile(25, (x, y) => { // a hanging banner on a wall face
    if (x >= 4 && x <= 11 && y >= 1 && y <= 13 - (x % 2 ? 0 : 1)) {
      if (y === 1) return hex('#c0a040');
      const emb = Math.hypot(x - 7.5, y - 7) < 2.2;
      return emb ? hex('#ffd04a') : x === 4 || x === 11 ? hex('#5a1424') : hex('#8a1e30');
    }
    return y < 2 ? hex('#8a7a86') : y > 13 ? mix(WALL, [0, 0, 0], 0.55) : brick(x, y, 3, WALL);
  });
  save(S, 'tiles.png');
}

console.log('wrote', DIR);
