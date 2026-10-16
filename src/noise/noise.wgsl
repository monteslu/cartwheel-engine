// cartwheel noise for WGSL: FastNoiseLite 1.1.1 (MIT, Jordan Peck,
// src/third_party/fastnoiselite) ported line for line from its C version, as
// noise.glsl is. Integer hashing is in u32 (wraps by definition); i32(u32) and
// u32(i32) reinterpret bits. tools/gen-noise-shaders.mjs embeds this file and
// the tables in noise_shaders.c.
struct CwNoise {
  seed: i32, frequency: f32, ntype: i32, rotation: i32, fractal: i32, octaves: i32,
  lacunarity: f32, gain: f32, weighted_strength: f32, ping_pong_strength: f32,
  cellular_distance: i32, cellular_return: i32, cellular_jitter: f32,
};
@TABLES@
const CWN_PX: u32 = 501125321u;
const CWN_PY: u32 = 1136930381u;
const CWN_PZ: u32 = 1720413743u;

fn cwn_floor(f: f32) -> i32 { return select(i32(f) - 1, i32(f), f >= 0.0); }
fn cwn_round(f: f32) -> i32 { return select(i32(f - 0.5), i32(f + 0.5), f >= 0.0); }
fn cwn_lerp(a: f32, b: f32, t: f32) -> f32 { return a + t * (b - a); }
fn cwn_hermite(t: f32) -> f32 { return t * t * (3.0 - 2.0 * t); }
fn cwn_quintic(t: f32) -> f32 { return t * t * t * (t * (t * 6.0 - 15.0) + 10.0); }
fn cwn_cubic(a: f32, b: f32, c: f32, d: f32, t: f32) -> f32 {
  let p = (d - c) - (a - b);
  return t * t * t * p + t * t * ((a - b) - p) + t * (c - a) + b;
}
fn cwn_pingpong(t0: f32) -> f32 {
  let t = t0 - f32(i32(t0 * 0.5) * 2);
  return select(2.0 - t, t, t < 1.0);
}
fn cwn_fsqrt(a: f32) -> f32 {
  let xhalf = 0.5 * a;
  var r = bitcast<f32>(0x5f3759df - (bitcast<i32>(a) >> 1u));
  r = r * (1.5 - xhalf * r * r);
  return a * r;
}
fn cwn_hash2(s: u32, x: u32, y: u32) -> u32 { return (s ^ x ^ y) * 0x27d4eb2du; }
fn cwn_hash3(s: u32, x: u32, y: u32, z: u32) -> u32 { return (s ^ x ^ y ^ z) * 0x27d4eb2du; }
fn cwn_val2(s: u32, x: u32, y: u32) -> f32 {
  var h = cwn_hash2(s, x, y);
  h *= h;
  h ^= h << 19u;
  return f32(i32(h)) * 4.65661287e-10;
}
fn cwn_val3(s: u32, x: u32, y: u32, z: u32) -> f32 {
  var h = cwn_hash3(s, x, y, z);
  h *= h;
  h ^= h << 19u;
  return f32(i32(h)) * 4.65661287e-10;
}
fn cwn_grad2(s: u32, x: u32, y: u32, xd: f32, yd: f32) -> f32 {
  var h = cwn_hash2(s, x, y);
  h ^= u32(i32(h) >> 15u);
  let i = h & 254u;
  return xd * CWN_G2D[i] + yd * CWN_G2D[i | 1u];
}
fn cwn_grad3(s: u32, x: u32, y: u32, z: u32, xd: f32, yd: f32, zd: f32) -> f32 {
  var h = cwn_hash3(s, x, y, z);
  h ^= u32(i32(h) >> 15u);
  let i = h & 252u;
  return xd * CWN_G3D[i] + yd * CWN_G3D[i | 1u] + zd * CWN_G3D[i | 2u];
}

// ── OpenSimplex2 ──
fn cwn_os2_2(s: u32, x: f32, y: f32) -> f32 {
  const G2 = 0.211324871;
  let i = cwn_floor(x);
  let j = cwn_floor(y);
  let xi = x - f32(i);
  let yi = y - f32(j);
  let t = (xi + yi) * G2;
  let x0 = xi - t;
  let y0 = yi - t;
  let pi = u32(i) * CWN_PX;
  let pj = u32(j) * CWN_PY;
  var n0 = 0.0;
  var n1 = 0.0;
  var n2 = 0.0;
  let a = 0.5 - x0 * x0 - y0 * y0;
  if (a > 0.0) { n0 = (a * a) * (a * a) * cwn_grad2(s, pi, pj, x0, y0); }
  let c = 3.15470052 * t + (-0.666666627 + a);
  if (c > 0.0) {
    let x2 = x0 + -0.577350259;
    let y2 = y0 + -0.577350259;
    n2 = (c * c) * (c * c) * cwn_grad2(s, pi + CWN_PX, pj + CWN_PY, x2, y2);
  }
  if (y0 > x0) {
    let x1 = x0 + G2;
    let y1 = y0 + -0.788675129;
    let b = 0.5 - x1 * x1 - y1 * y1;
    if (b > 0.0) { n1 = (b * b) * (b * b) * cwn_grad2(s, pi, pj + CWN_PY, x1, y1); }
  } else {
    let x1 = x0 + -0.788675129;
    let y1 = y0 + G2;
    let b = 0.5 - x1 * x1 - y1 * y1;
    if (b > 0.0) { n1 = (b * b) * (b * b) * cwn_grad2(s, pi + CWN_PX, pj, x1, y1); }
  }
  return (n0 + n1 + n2) * 99.83685446303647;
}

fn cwn_os2_3(s0: u32, x: f32, y: f32, z: f32) -> f32 {
  var s = s0;
  let i = cwn_round(x);
  let j = cwn_round(y);
  let k = cwn_round(z);
  var x0 = x - f32(i);
  var y0 = y - f32(j);
  var z0 = z - f32(k);
  var xs = i32(-1.0 - x0) | 1;
  var ys = i32(-1.0 - y0) | 1;
  var zs = i32(-1.0 - z0) | 1;
  var ax0 = f32(xs) * -x0;
  var ay0 = f32(ys) * -y0;
  var az0 = f32(zs) * -z0;
  var pi = u32(i) * CWN_PX;
  var pj = u32(j) * CWN_PY;
  var pk = u32(k) * CWN_PZ;
  var value = 0.0;
  var a = (0.6 - x0 * x0) - (y0 * y0 + z0 * z0);
  for (var l = 0; l < 2; l++) {
    if (a > 0.0) { value += (a * a) * (a * a) * cwn_grad3(s, pi, pj, pk, x0, y0, z0); }
    var b = a + 1.0;
    var i1 = pi;
    var j1 = pj;
    var k1 = pk;
    var x1 = x0;
    var y1 = y0;
    var z1 = z0;
    if (ax0 >= ay0 && ax0 >= az0) {
      x1 += f32(xs);
      b -= f32(xs * 2) * x1;
      i1 -= u32(xs) * CWN_PX;
    } else if (ay0 > ax0 && ay0 >= az0) {
      y1 += f32(ys);
      b -= f32(ys * 2) * y1;
      j1 -= u32(ys) * CWN_PY;
    } else {
      z1 += f32(zs);
      b -= f32(zs * 2) * z1;
      k1 -= u32(zs) * CWN_PZ;
    }
    if (b > 0.0) { value += (b * b) * (b * b) * cwn_grad3(s, i1, j1, k1, x1, y1, z1); }
    if (l == 1) { break; }
    ax0 = 0.5 - ax0;
    ay0 = 0.5 - ay0;
    az0 = 0.5 - az0;
    x0 = f32(xs) * ax0;
    y0 = f32(ys) * ay0;
    z0 = f32(zs) * az0;
    a += (0.75 - ax0) - (ay0 + az0);
    pi += u32(xs >> 1u) & CWN_PX;
    pj += u32(ys >> 1u) & CWN_PY;
    pk += u32(zs >> 1u) & CWN_PZ;
    xs = -xs;
    ys = -ys;
    zs = -zs;
    s = ~s;
  }
  return value * 32.69428253173828125;
}

// ── OpenSimplex2S ──
fn cwn_os2s_2(s: u32, x: f32, y: f32) -> f32 {
  const G2 = 0.211324871;
  let i0 = cwn_floor(x);
  let j0 = cwn_floor(y);
  let xi = x - f32(i0);
  let yi = y - f32(j0);
  let i = u32(i0) * CWN_PX;
  let j = u32(j0) * CWN_PY;
  let i1 = i + CWN_PX;
  let j1 = j + CWN_PY;
  let t = (xi + yi) * G2;
  let x0 = xi - t;
  let y0 = yi - t;
  let a0 = 0.666666687 - x0 * x0 - y0 * y0;
  var value = (a0 * a0) * (a0 * a0) * cwn_grad2(s, i, j, x0, y0);
  let a1 = 3.15470052 * t + (-0.666666627 + a0);
  let x1 = x0 - 0.577350259;
  let y1 = y0 - 0.577350259;
  value += (a1 * a1) * (a1 * a1) * cwn_grad2(s, i1, j1, x1, y1);
  let xmyi = xi - yi;
  if (t > G2) {
    if (xi + xmyi > 1.0) {
      let x2 = x0 + -1.36602545;
      let y2 = y0 + -0.366025388;
      let a2 = 0.666666687 - x2 * x2 - y2 * y2;
      if (a2 > 0.0) { value += (a2 * a2) * (a2 * a2) * cwn_grad2(s, i + (CWN_PX << 1u), j + CWN_PY, x2, y2); }
    } else {
      let x2 = x0 + G2;
      let y2 = y0 + -0.788675129;
      let a2 = 0.666666687 - x2 * x2 - y2 * y2;
      if (a2 > 0.0) { value += (a2 * a2) * (a2 * a2) * cwn_grad2(s, i, j + CWN_PY, x2, y2); }
    }
    if (yi - xmyi > 1.0) {
      let x3 = x0 + -0.366025388;
      let y3 = y0 + -1.36602545;
      let a3 = 0.666666687 - x3 * x3 - y3 * y3;
      if (a3 > 0.0) { value += (a3 * a3) * (a3 * a3) * cwn_grad2(s, i + CWN_PX, j + (CWN_PY << 1u), x3, y3); }
    } else {
      let x3 = x0 + -0.788675129;
      let y3 = y0 + G2;
      let a3 = 0.666666687 - x3 * x3 - y3 * y3;
      if (a3 > 0.0) { value += (a3 * a3) * (a3 * a3) * cwn_grad2(s, i + CWN_PX, j, x3, y3); }
    }
  } else {
    if (xi + xmyi < 0.0) {
      let x2 = x0 + 0.788675129;
      let y2 = y0 - G2;
      let a2 = 0.666666687 - x2 * x2 - y2 * y2;
      if (a2 > 0.0) { value += (a2 * a2) * (a2 * a2) * cwn_grad2(s, i - CWN_PX, j, x2, y2); }
    } else {
      let x2 = x0 + -0.788675129;
      let y2 = y0 + G2;
      let a2 = 0.666666687 - x2 * x2 - y2 * y2;
      if (a2 > 0.0) { value += (a2 * a2) * (a2 * a2) * cwn_grad2(s, i + CWN_PX, j, x2, y2); }
    }
    if (yi < xmyi) {
      let x2 = x0 - G2;
      let y2 = y0 - -0.788675129;
      let a2 = 0.666666687 - x2 * x2 - y2 * y2;
      if (a2 > 0.0) { value += (a2 * a2) * (a2 * a2) * cwn_grad2(s, i, j - CWN_PY, x2, y2); }
    } else {
      let x2 = x0 + G2;
      let y2 = y0 + -0.788675129;
      let a2 = 0.666666687 - x2 * x2 - y2 * y2;
      if (a2 > 0.0) { value += (a2 * a2) * (a2 * a2) * cwn_grad2(s, i, j + CWN_PY, x2, y2); }
    }
  }
  return value * 18.24196194486065;
}

fn cwn_os2s_3(s: u32, x: f32, y: f32, z: f32) -> f32 {
  let i0 = cwn_floor(x);
  let j0 = cwn_floor(y);
  let k0 = cwn_floor(z);
  let xi = x - f32(i0);
  let yi = y - f32(j0);
  let zi = z - f32(k0);
  let i = u32(i0) * CWN_PX;
  let j = u32(j0) * CWN_PY;
  let k = u32(k0) * CWN_PZ;
  let s2 = s + 1293373u;
  let xm = i32(-0.5 - xi);
  let ym = i32(-0.5 - yi);
  let zm = i32(-0.5 - zi);
  let uxm = u32(xm);
  let uym = u32(ym);
  let uzm = u32(zm);
  let x0 = xi + f32(xm);
  let y0 = yi + f32(ym);
  let z0 = zi + f32(zm);
  let a0 = 0.75 - x0 * x0 - y0 * y0 - z0 * z0;
  var value = (a0 * a0) * (a0 * a0) * cwn_grad3(s, i + (uxm & CWN_PX), j + (uym & CWN_PY), k + (uzm & CWN_PZ), x0, y0, z0);
  let x1 = xi - 0.5;
  let y1 = yi - 0.5;
  let z1 = zi - 0.5;
  let a1 = 0.75 - x1 * x1 - y1 * y1 - z1 * z1;
  value += (a1 * a1) * (a1 * a1) * cwn_grad3(s2, i + CWN_PX, j + CWN_PY, k + CWN_PZ, x1, y1, z1);
  let xf0 = f32((xm | 1) << 1u) * x1;
  let yf0 = f32((ym | 1) << 1u) * y1;
  let zf0 = f32((zm | 1) << 1u) * z1;
  let xf1 = f32(-2 - (xm << 2u)) * x1 - 1.0;
  let yf1 = f32(-2 - (ym << 2u)) * y1 - 1.0;
  let zf1 = f32(-2 - (zm << 2u)) * z1 - 1.0;
  let sx = f32(xm | 1);
  let sy = f32(ym | 1);
  let sz = f32(zm | 1);
  var skip5 = false;
  let a2 = xf0 + a0;
  if (a2 > 0.0) {
    value += (a2 * a2) * (a2 * a2) * cwn_grad3(s, i + (~uxm & CWN_PX), j + (uym & CWN_PY), k + (uzm & CWN_PZ), x0 - sx, y0, z0);
  } else {
    let a3 = yf0 + zf0 + a0;
    if (a3 > 0.0) { value += (a3 * a3) * (a3 * a3) * cwn_grad3(s, i + (uxm & CWN_PX), j + (~uym & CWN_PY), k + (~uzm & CWN_PZ), x0, y0 - sy, z0 - sz); }
    let a4 = xf1 + a1;
    if (a4 > 0.0) {
      value += (a4 * a4) * (a4 * a4) * cwn_grad3(s2, i + (uxm & (CWN_PX * 2u)), j + CWN_PY, k + CWN_PZ, sx + x1, y1, z1);
      skip5 = true;
    }
  }
  var skip9 = false;
  let a6 = yf0 + a0;
  if (a6 > 0.0) {
    value += (a6 * a6) * (a6 * a6) * cwn_grad3(s, i + (uxm & CWN_PX), j + (~uym & CWN_PY), k + (uzm & CWN_PZ), x0, y0 - sy, z0);
  } else {
    let a7 = xf0 + zf0 + a0;
    if (a7 > 0.0) { value += (a7 * a7) * (a7 * a7) * cwn_grad3(s, i + (~uxm & CWN_PX), j + (uym & CWN_PY), k + (~uzm & CWN_PZ), x0 - sx, y0, z0 - sz); }
    let a8 = yf1 + a1;
    if (a8 > 0.0) {
      value += (a8 * a8) * (a8 * a8) * cwn_grad3(s2, i + CWN_PX, j + (uym & (CWN_PY << 1u)), k + CWN_PZ, x1, sy + y1, z1);
      skip9 = true;
    }
  }
  var skipD = false;
  let aA = zf0 + a0;
  if (aA > 0.0) {
    value += (aA * aA) * (aA * aA) * cwn_grad3(s, i + (uxm & CWN_PX), j + (uym & CWN_PY), k + (~uzm & CWN_PZ), x0, y0, z0 - sz);
  } else {
    let aB = xf0 + yf0 + a0;
    if (aB > 0.0) { value += (aB * aB) * (aB * aB) * cwn_grad3(s, i + (~uxm & CWN_PX), j + (~uym & CWN_PY), k + (uzm & CWN_PZ), x0 - sx, y0 - sy, z0); }
    let aC = zf1 + a1;
    if (aC > 0.0) {
      value += (aC * aC) * (aC * aC) * cwn_grad3(s2, i + CWN_PX, j + CWN_PY, k + (uzm & (CWN_PZ << 1u)), x1, y1, sz + z1);
      skipD = true;
    }
  }
  if (!skip5) {
    let a5 = yf1 + zf1 + a1;
    if (a5 > 0.0) { value += (a5 * a5) * (a5 * a5) * cwn_grad3(s2, i + CWN_PX, j + (uym & (CWN_PY << 1u)), k + (uzm & (CWN_PZ << 1u)), x1, sy + y1, sz + z1); }
  }
  if (!skip9) {
    let a9 = xf1 + zf1 + a1;
    if (a9 > 0.0) { value += (a9 * a9) * (a9 * a9) * cwn_grad3(s2, i + (uxm & (CWN_PX * 2u)), j + CWN_PY, k + (uzm & (CWN_PZ << 1u)), sx + x1, y1, sz + z1); }
  }
  if (!skipD) {
    let aD = xf1 + yf1 + a1;
    if (aD > 0.0) { value += (aD * aD) * (aD * aD) * cwn_grad3(s2, i + (uxm & (CWN_PX << 1u)), j + (uym & (CWN_PY << 1u)), k + CWN_PZ, sx + x1, sy + y1, z1); }
  }
  return value * 9.046026385208288;
}

// ── Cellular ──
fn cwn_cell_ret(n: CwNoise, d0i: f32, d1i: f32, closest: u32) -> f32 {
  var d0 = d0i;
  var d1 = d1i;
  if (n.cellular_distance == 0 && n.cellular_return >= 1) {
    d0 = cwn_fsqrt(d0);
    if (n.cellular_return >= 2) { d1 = cwn_fsqrt(d1); }
  }
  switch (n.cellular_return) {
    case 0: { return f32(i32(closest)) * 4.65661287e-10; }
    case 1: { return d0 - 1.0; }
    case 2: { return d1 - 1.0; }
    case 3: { return (d1 + d0) * 0.5 - 1.0; }
    case 4: { return d1 - d0 - 1.0; }
    case 5: { return d1 * d0 * 0.5 - 1.0; }
    case 6: { return d0 / d1 - 1.0; }
    default: { return 0.0; }
  }
}

fn cwn_cell_2(n: CwNoise, s: u32, x: f32, y: f32) -> f32 {
  let xr = cwn_round(x);
  let yr = cwn_round(y);
  var d0 = bitcast<f32>(0x7f7fffffu); // FLT_MAX
  var d1 = bitcast<f32>(0x7f7fffffu); // FLT_MAX
  var closest = 0u;
  let jit = 0.43701595 * n.cellular_jitter;
  var xp = u32(xr - 1) * CWN_PX;
  let ypb = u32(yr - 1) * CWN_PY;
  for (var xi = xr - 1; xi <= xr + 1; xi++) {
    var yp = ypb;
    for (var yi = yr - 1; yi <= yr + 1; yi++) {
      let h = cwn_hash2(s, xp, yp);
      let idx = h & 510u;
      let vx = (f32(xi) - x) + CWN_R2D[idx] * jit;
      let vy = (f32(yi) - y) + CWN_R2D[idx | 1u] * jit;
      var nd: f32;
      if (n.cellular_distance == 2) { nd = abs(vx) + abs(vy); }
      else if (n.cellular_distance == 3) { nd = (abs(vx) + abs(vy)) + (vx * vx + vy * vy); }
      else { nd = vx * vx + vy * vy; }
      d1 = max(min(d1, nd), d0);
      if (nd < d0) { d0 = nd; closest = h; }
      yp += CWN_PY;
    }
    xp += CWN_PX;
  }
  return cwn_cell_ret(n, d0, d1, closest);
}

fn cwn_cell_3(n: CwNoise, s: u32, x: f32, y: f32, z: f32) -> f32 {
  let xr = cwn_round(x);
  let yr = cwn_round(y);
  let zr = cwn_round(z);
  var d0 = bitcast<f32>(0x7f7fffffu); // FLT_MAX
  var d1 = bitcast<f32>(0x7f7fffffu); // FLT_MAX
  var closest = 0u;
  let jit = 0.39614353 * n.cellular_jitter;
  var xp = u32(xr - 1) * CWN_PX;
  let ypb = u32(yr - 1) * CWN_PY;
  let zpb = u32(zr - 1) * CWN_PZ;
  for (var xi = xr - 1; xi <= xr + 1; xi++) {
    var yp = ypb;
    for (var yi = yr - 1; yi <= yr + 1; yi++) {
      var zp = zpb;
      for (var zi = zr - 1; zi <= zr + 1; zi++) {
        let h = cwn_hash3(s, xp, yp, zp);
        let idx = h & 1020u;
        let vx = (f32(xi) - x) + CWN_R3D[idx] * jit;
        let vy = (f32(yi) - y) + CWN_R3D[idx | 1u] * jit;
        let vz = (f32(zi) - z) + CWN_R3D[idx | 2u] * jit;
        var nd: f32;
        if (n.cellular_distance == 2) { nd = abs(vx) + abs(vy) + abs(vz); }
        else if (n.cellular_distance == 3) { nd = (abs(vx) + abs(vy) + abs(vz)) + (vx * vx + vy * vy + vz * vz); }
        else { nd = vx * vx + vy * vy + vz * vz; }
        d1 = max(min(d1, nd), d0);
        if (nd < d0) { d0 = nd; closest = h; }
        zp += CWN_PZ;
      }
      yp += CWN_PY;
    }
    xp += CWN_PX;
  }
  return cwn_cell_ret(n, d0, d1, closest);
}

// ── Perlin, value ──
fn cwn_perlin_2(s: u32, x: f32, y: f32) -> f32 {
  let x0i = cwn_floor(x);
  let y0i = cwn_floor(y);
  let xd0 = x - f32(x0i);
  let yd0 = y - f32(y0i);
  let xd1 = xd0 - 1.0;
  let yd1 = yd0 - 1.0;
  let xs = cwn_quintic(xd0);
  let ys = cwn_quintic(yd0);
  let x0 = u32(x0i) * CWN_PX;
  let y0 = u32(y0i) * CWN_PY;
  let x1 = x0 + CWN_PX;
  let y1 = y0 + CWN_PY;
  let xf0 = cwn_lerp(cwn_grad2(s, x0, y0, xd0, yd0), cwn_grad2(s, x1, y0, xd1, yd0), xs);
  let xf1 = cwn_lerp(cwn_grad2(s, x0, y1, xd0, yd1), cwn_grad2(s, x1, y1, xd1, yd1), xs);
  return cwn_lerp(xf0, xf1, ys) * 1.4247691104677813;
}

fn cwn_perlin_3(s: u32, x: f32, y: f32, z: f32) -> f32 {
  let x0i = cwn_floor(x);
  let y0i = cwn_floor(y);
  let z0i = cwn_floor(z);
  let xd0 = x - f32(x0i);
  let yd0 = y - f32(y0i);
  let zd0 = z - f32(z0i);
  let xd1 = xd0 - 1.0;
  let yd1 = yd0 - 1.0;
  let zd1 = zd0 - 1.0;
  let xs = cwn_quintic(xd0);
  let ys = cwn_quintic(yd0);
  let zs = cwn_quintic(zd0);
  let x0 = u32(x0i) * CWN_PX;
  let y0 = u32(y0i) * CWN_PY;
  let z0 = u32(z0i) * CWN_PZ;
  let x1 = x0 + CWN_PX;
  let y1 = y0 + CWN_PY;
  let z1 = z0 + CWN_PZ;
  let xf00 = cwn_lerp(cwn_grad3(s, x0, y0, z0, xd0, yd0, zd0), cwn_grad3(s, x1, y0, z0, xd1, yd0, zd0), xs);
  let xf10 = cwn_lerp(cwn_grad3(s, x0, y1, z0, xd0, yd1, zd0), cwn_grad3(s, x1, y1, z0, xd1, yd1, zd0), xs);
  let xf01 = cwn_lerp(cwn_grad3(s, x0, y0, z1, xd0, yd0, zd1), cwn_grad3(s, x1, y0, z1, xd1, yd0, zd1), xs);
  let xf11 = cwn_lerp(cwn_grad3(s, x0, y1, z1, xd0, yd1, zd1), cwn_grad3(s, x1, y1, z1, xd1, yd1, zd1), xs);
  let yf0 = cwn_lerp(xf00, xf10, ys);
  let yf1 = cwn_lerp(xf01, xf11, ys);
  return cwn_lerp(yf0, yf1, zs) * 0.964921414852142333984375;
}

fn cwn_vcubic_2(s: u32, x: f32, y: f32) -> f32 {
  let x1i = cwn_floor(x);
  let y1i = cwn_floor(y);
  let xs = x - f32(x1i);
  let ys = y - f32(y1i);
  let x1 = u32(x1i) * CWN_PX;
  let y1 = u32(y1i) * CWN_PY;
  let x0 = x1 - CWN_PX;
  let y0 = y1 - CWN_PY;
  let x2 = x1 + CWN_PX;
  let y2 = y1 + CWN_PY;
  let x3 = x1 + (CWN_PX << 1u);
  let y3 = y1 + (CWN_PY << 1u);
  return cwn_cubic(
    cwn_cubic(cwn_val2(s, x0, y0), cwn_val2(s, x1, y0), cwn_val2(s, x2, y0), cwn_val2(s, x3, y0), xs),
    cwn_cubic(cwn_val2(s, x0, y1), cwn_val2(s, x1, y1), cwn_val2(s, x2, y1), cwn_val2(s, x3, y1), xs),
    cwn_cubic(cwn_val2(s, x0, y2), cwn_val2(s, x1, y2), cwn_val2(s, x2, y2), cwn_val2(s, x3, y2), xs),
    cwn_cubic(cwn_val2(s, x0, y3), cwn_val2(s, x1, y3), cwn_val2(s, x2, y3), cwn_val2(s, x3, y3), xs),
    ys) * 0.444444448;
}

fn cwn_vcubic_3(s: u32, x: f32, y: f32, z: f32) -> f32 {
  let x1i = cwn_floor(x);
  let y1i = cwn_floor(y);
  let z1i = cwn_floor(z);
  let xs = x - f32(x1i);
  let ys = y - f32(y1i);
  let zs = z - f32(z1i);
  let xb = u32(x1i) * CWN_PX;
  let yb = u32(y1i) * CWN_PY;
  let zb = u32(z1i) * CWN_PZ;
  let X = array<u32, 4>(xb - CWN_PX, xb, xb + CWN_PX, xb + (CWN_PX << 1u));
  let Y = array<u32, 4>(yb - CWN_PY, yb, yb + CWN_PY, yb + (CWN_PY << 1u));
  let Z = array<u32, 4>(zb - CWN_PZ, zb, zb + CWN_PZ, zb + (CWN_PZ << 1u));
  var zr: array<f32, 4>;
  for (var c = 0; c < 4; c++) {
    var yr: array<f32, 4>;
    for (var b = 0; b < 4; b++) {
      yr[b] = cwn_cubic(cwn_val3(s, X[0], Y[b], Z[c]), cwn_val3(s, X[1], Y[b], Z[c]), cwn_val3(s, X[2], Y[b], Z[c]), cwn_val3(s, X[3], Y[b], Z[c]), xs);
    }
    zr[c] = cwn_cubic(yr[0], yr[1], yr[2], yr[3], ys);
  }
  return cwn_cubic(zr[0], zr[1], zr[2], zr[3], zs) * 0.296296299;
}

fn cwn_value_2(s: u32, x: f32, y: f32) -> f32 {
  let x0i = cwn_floor(x);
  let y0i = cwn_floor(y);
  let xs = cwn_hermite(x - f32(x0i));
  let ys = cwn_hermite(y - f32(y0i));
  let x0 = u32(x0i) * CWN_PX;
  let y0 = u32(y0i) * CWN_PY;
  let x1 = x0 + CWN_PX;
  let y1 = y0 + CWN_PY;
  let xf0 = cwn_lerp(cwn_val2(s, x0, y0), cwn_val2(s, x1, y0), xs);
  let xf1 = cwn_lerp(cwn_val2(s, x0, y1), cwn_val2(s, x1, y1), xs);
  return cwn_lerp(xf0, xf1, ys);
}

fn cwn_value_3(s: u32, x: f32, y: f32, z: f32) -> f32 {
  let x0i = cwn_floor(x);
  let y0i = cwn_floor(y);
  let z0i = cwn_floor(z);
  let xs = cwn_hermite(x - f32(x0i));
  let ys = cwn_hermite(y - f32(y0i));
  let zs = cwn_hermite(z - f32(z0i));
  let x0 = u32(x0i) * CWN_PX;
  let y0 = u32(y0i) * CWN_PY;
  let z0 = u32(z0i) * CWN_PZ;
  let x1 = x0 + CWN_PX;
  let y1 = y0 + CWN_PY;
  let z1 = z0 + CWN_PZ;
  let xf00 = cwn_lerp(cwn_val3(s, x0, y0, z0), cwn_val3(s, x1, y0, z0), xs);
  let xf10 = cwn_lerp(cwn_val3(s, x0, y1, z0), cwn_val3(s, x1, y1, z0), xs);
  let xf01 = cwn_lerp(cwn_val3(s, x0, y0, z1), cwn_val3(s, x1, y0, z1), xs);
  let xf11 = cwn_lerp(cwn_val3(s, x0, y1, z1), cwn_val3(s, x1, y1, z1), xs);
  let yf0 = cwn_lerp(xf00, xf10, ys);
  let yf1 = cwn_lerp(xf01, xf11, ys);
  return cwn_lerp(yf0, yf1, zs);
}

// ── dispatch, fractals ──
fn cwn_single2(n: CwNoise, seed: i32, x: f32, y: f32) -> f32 {
  let s = u32(seed);
  switch (n.ntype) {
    case 0: { return cwn_os2_2(s, x, y); }
    case 1: { return cwn_os2s_2(s, x, y); }
    case 2: { return cwn_cell_2(n, s, x, y); }
    case 3: { return cwn_perlin_2(s, x, y); }
    case 4: { return cwn_vcubic_2(s, x, y); }
    case 5: { return cwn_value_2(s, x, y); }
    default: { return 0.0; }
  }
}

fn cwn_single3(n: CwNoise, seed: i32, x: f32, y: f32, z: f32) -> f32 {
  let s = u32(seed);
  switch (n.ntype) {
    case 0: { return cwn_os2_3(s, x, y, z); }
    case 1: { return cwn_os2s_3(s, x, y, z); }
    case 2: { return cwn_cell_3(n, s, x, y, z); }
    case 3: { return cwn_perlin_3(s, x, y, z); }
    case 4: { return cwn_vcubic_3(s, x, y, z); }
    case 5: { return cwn_value_3(s, x, y, z); }
    default: { return 0.0; }
  }
}

fn cwn_bounding(n: CwNoise) -> f32 {
  let gain = abs(n.gain);
  var amp = gain;
  var af = 1.0;
  for (var i = 1; i < n.octaves; i++) {
    af += amp;
    amp *= gain;
  }
  return 1.0 / af;
}

fn cw_noise2(p: vec2f, n: CwNoise) -> f32 {
  var x = p.x * n.frequency;
  var y = p.y * n.frequency;
  if (n.ntype == 0 || n.ntype == 1) {
    let t = (x + y) * 0.366025388;
    x += t;
    y += t;
  }
  if (n.fractal < 1 || n.fractal > 3) { return cwn_single2(n, n.seed, x, y); }
  var seed = n.seed;
  var sum = 0.0;
  var amp = cwn_bounding(n);
  for (var i = 0; i < n.octaves; i++) {
    var v = cwn_single2(n, seed, x, y);
    seed++;
    if (n.fractal == 1) {
      sum += v * amp;
      amp *= cwn_lerp(1.0, min(v + 1.0, 2.0) * 0.5, n.weighted_strength);
    } else if (n.fractal == 2) {
      v = abs(v);
      sum += (v * -2.0 + 1.0) * amp;
      amp *= cwn_lerp(1.0, 1.0 - v, n.weighted_strength);
    } else {
      v = cwn_pingpong((v + 1.0) * n.ping_pong_strength);
      sum += (v - 0.5) * 2.0 * amp;
      amp *= cwn_lerp(1.0, v, n.weighted_strength);
    }
    x *= n.lacunarity;
    y *= n.lacunarity;
    amp *= n.gain;
  }
  return sum;
}

fn cw_noise3(p: vec3f, n: CwNoise) -> f32 {
  var x = p.x * n.frequency;
  var y = p.y * n.frequency;
  var z = p.z * n.frequency;
  if (n.rotation == 1) {
    let xy = x + y;
    let s2 = xy * -0.211324871;
    z *= 0.577350259;
    x += s2 - z;
    y = y + s2 - z;
    z += xy * 0.577350259;
  } else if (n.rotation == 2) {
    let xz = x + z;
    let s2 = xz * -0.211324871;
    y *= 0.577350259;
    x += s2 - y;
    z += s2 - y;
    y += xz * 0.577350259;
  } else if (n.ntype == 0 || n.ntype == 1) {
    let r = (x + y + z) * 0.666666687;
    x = r - x;
    y = r - y;
    z = r - z;
  }
  if (n.fractal < 1 || n.fractal > 3) { return cwn_single3(n, n.seed, x, y, z); }
  var seed = n.seed;
  var sum = 0.0;
  var amp = cwn_bounding(n);
  for (var i = 0; i < n.octaves; i++) {
    var v = cwn_single3(n, seed, x, y, z);
    seed++;
    if (n.fractal == 1) {
      sum += v * amp;
      amp *= cwn_lerp(1.0, (v + 1.0) * 0.5, n.weighted_strength);
    } else if (n.fractal == 2) {
      v = abs(v);
      sum += (v * -2.0 + 1.0) * amp;
      amp *= cwn_lerp(1.0, 1.0 - v, n.weighted_strength);
    } else {
      v = cwn_pingpong((v + 1.0) * n.ping_pong_strength);
      sum += (v - 0.5) * 2.0 * amp;
      amp *= cwn_lerp(1.0, v, n.weighted_strength);
    }
    x *= n.lacunarity;
    y *= n.lacunarity;
    z *= n.lacunarity;
    amp *= n.gain;
  }
  return sum;
}

fn cw_noise_make(seed: i32, frequency: f32, ntype: i32, fractal: i32, octaves: i32) -> CwNoise {
  return CwNoise(seed, frequency, ntype, 0, fractal, octaves, 2.0, 0.5, 0.0, 2.0, 1, 1, 1.0);
}

// the 16 floats of cw_noise_gpu_params as four vec4 uniforms
fn cw_noise_unpack(a: vec4f, b: vec4f, c: vec4f, d: vec4f) -> CwNoise {
  return CwNoise(i32(a.x), a.y, i32(a.z), i32(a.w), i32(b.x), i32(b.y), b.z, b.w, c.x, c.y, i32(c.z), i32(c.w), d.x);
}
