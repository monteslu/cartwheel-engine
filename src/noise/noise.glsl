// cartwheel noise for GLSL ES 3.00: FastNoiseLite 1.1.1 (MIT, Jordan Peck,
// src/third_party/fastnoiselite) ported line for line from its C version, so a
// sample matches cw_noise_2d / cw_noise_3d to float rounding. Integer hashing
// is done in uint (wraps by definition); FastNoiseLite's own floor, round and
// square root are kept (they are not GLSL's floor, round and sqrt at every
// input). The constants are the C code's float results, printed exactly.
// tools/gen-noise-shaders.mjs embeds this file and the tables in noise_shaders.c.
struct CwNoise {
  int seed; float frequency; int type; int rotation; int fractal; int octaves;
  float lacunarity; float gain; float weighted_strength; float ping_pong_strength;
  int cellular_distance; int cellular_return; float cellular_jitter;
};
@TABLES@
const uint CWN_PX = 501125321u;
const uint CWN_PY = 1136930381u;
const uint CWN_PZ = 1720413743u;

int cwn_floor(float f) { return f >= 0.0 ? int(f) : int(f) - 1; }
int cwn_round(float f) { return f >= 0.0 ? int(f + 0.5) : int(f - 0.5); }
float cwn_lerp(float a, float b, float t) { return a + t * (b - a); }
float cwn_hermite(float t) { return t * t * (3.0 - 2.0 * t); }
float cwn_quintic(float t) { return t * t * t * (t * (t * 6.0 - 15.0) + 10.0); }
float cwn_cubic(float a, float b, float c, float d, float t) {
  float p = (d - c) - (a - b);
  return t * t * t * p + t * t * ((a - b) - p) + t * (c - a) + b;
}
float cwn_pingpong(float t) {
  t -= float(int(t * 0.5) * 2);
  return t < 1.0 ? t : 2.0 - t;
}
float cwn_fsqrt(float a) {
  float xhalf = 0.5 * a;
  float r = intBitsToFloat(0x5f3759df - (floatBitsToInt(a) >> 1));
  r = r * (1.5 - xhalf * r * r);
  return a * r;
}
uint cwn_hash2(uint s, uint x, uint y) { return (s ^ x ^ y) * 0x27d4eb2du; }
uint cwn_hash3(uint s, uint x, uint y, uint z) { return (s ^ x ^ y ^ z) * 0x27d4eb2du; }
float cwn_val2(uint s, uint x, uint y) {
  uint h = cwn_hash2(s, x, y);
  h *= h;
  h ^= h << 19;
  return float(int(h)) * 4.65661287e-10;
}
float cwn_val3(uint s, uint x, uint y, uint z) {
  uint h = cwn_hash3(s, x, y, z);
  h *= h;
  h ^= h << 19;
  return float(int(h)) * 4.65661287e-10;
}
float cwn_grad2(uint s, uint x, uint y, float xd, float yd) {
  uint h = cwn_hash2(s, x, y);
  h ^= uint(int(h) >> 15);
  int i = int(h & 254u);
  return xd * CWN_G2D[i] + yd * CWN_G2D[i | 1];
}
float cwn_grad3(uint s, uint x, uint y, uint z, float xd, float yd, float zd) {
  uint h = cwn_hash3(s, x, y, z);
  h ^= uint(int(h) >> 15);
  int i = int(h & 252u);
  return xd * CWN_G3D[i] + yd * CWN_G3D[i | 1] + zd * CWN_G3D[i | 2];
}

// ── OpenSimplex2 ──
float cwn_os2_2(uint s, float x, float y) {
  const float G2 = 0.211324871;
  int i = cwn_floor(x), j = cwn_floor(y);
  float xi = x - float(i), yi = y - float(j);
  float t = (xi + yi) * G2;
  float x0 = xi - t, y0 = yi - t;
  uint pi = uint(i) * CWN_PX, pj = uint(j) * CWN_PY;
  float n0 = 0.0, n1 = 0.0, n2 = 0.0;
  float a = 0.5 - x0 * x0 - y0 * y0;
  if (a > 0.0) n0 = (a * a) * (a * a) * cwn_grad2(s, pi, pj, x0, y0);
  float c = 3.15470052 * t + (-0.666666627 + a);
  if (c > 0.0) {
    float x2 = x0 + -0.577350259, y2 = y0 + -0.577350259;
    n2 = (c * c) * (c * c) * cwn_grad2(s, pi + CWN_PX, pj + CWN_PY, x2, y2);
  }
  if (y0 > x0) {
    float x1 = x0 + G2, y1 = y0 + -0.788675129;
    float b = 0.5 - x1 * x1 - y1 * y1;
    if (b > 0.0) n1 = (b * b) * (b * b) * cwn_grad2(s, pi, pj + CWN_PY, x1, y1);
  } else {
    float x1 = x0 + -0.788675129, y1 = y0 + G2;
    float b = 0.5 - x1 * x1 - y1 * y1;
    if (b > 0.0) n1 = (b * b) * (b * b) * cwn_grad2(s, pi + CWN_PX, pj, x1, y1);
  }
  return (n0 + n1 + n2) * 99.83685446303647;
}

float cwn_os2_3(uint s, float x, float y, float z) {
  int i = cwn_round(x), j = cwn_round(y), k = cwn_round(z);
  float x0 = x - float(i), y0 = y - float(j), z0 = z - float(k);
  int xs = int(-1.0 - x0) | 1, ys = int(-1.0 - y0) | 1, zs = int(-1.0 - z0) | 1;
  float ax0 = float(xs) * -x0, ay0 = float(ys) * -y0, az0 = float(zs) * -z0;
  uint pi = uint(i) * CWN_PX, pj = uint(j) * CWN_PY, pk = uint(k) * CWN_PZ;
  float value = 0.0;
  float a = (0.6 - x0 * x0) - (y0 * y0 + z0 * z0);
  for (int l = 0; l < 2; l++) {
    if (a > 0.0) value += (a * a) * (a * a) * cwn_grad3(s, pi, pj, pk, x0, y0, z0);
    float b = a + 1.0;
    uint i1 = pi, j1 = pj, k1 = pk;
    float x1 = x0, y1 = y0, z1 = z0;
    if (ax0 >= ay0 && ax0 >= az0) {
      x1 += float(xs);
      b -= float(xs * 2) * x1;
      i1 -= uint(xs) * CWN_PX;
    } else if (ay0 > ax0 && ay0 >= az0) {
      y1 += float(ys);
      b -= float(ys * 2) * y1;
      j1 -= uint(ys) * CWN_PY;
    } else {
      z1 += float(zs);
      b -= float(zs * 2) * z1;
      k1 -= uint(zs) * CWN_PZ;
    }
    if (b > 0.0) value += (b * b) * (b * b) * cwn_grad3(s, i1, j1, k1, x1, y1, z1);
    if (l == 1) break;
    ax0 = 0.5 - ax0;
    ay0 = 0.5 - ay0;
    az0 = 0.5 - az0;
    x0 = float(xs) * ax0;
    y0 = float(ys) * ay0;
    z0 = float(zs) * az0;
    a += (0.75 - ax0) - (ay0 + az0);
    pi += uint(xs >> 1) & CWN_PX;
    pj += uint(ys >> 1) & CWN_PY;
    pk += uint(zs >> 1) & CWN_PZ;
    xs = -xs;
    ys = -ys;
    zs = -zs;
    s = ~s;
  }
  return value * 32.69428253173828125;
}

// ── OpenSimplex2S ──
float cwn_os2s_2(uint s, float x, float y) {
  const float G2 = 0.211324871;
  int i0 = cwn_floor(x), j0 = cwn_floor(y);
  float xi = x - float(i0), yi = y - float(j0);
  uint i = uint(i0) * CWN_PX, j = uint(j0) * CWN_PY;
  uint i1 = i + CWN_PX, j1 = j + CWN_PY;
  float t = (xi + yi) * G2;
  float x0 = xi - t, y0 = yi - t;
  float a0 = 0.666666687 - x0 * x0 - y0 * y0;
  float value = (a0 * a0) * (a0 * a0) * cwn_grad2(s, i, j, x0, y0);
  float a1 = 3.15470052 * t + (-0.666666627 + a0);
  float x1 = x0 - 0.577350259, y1 = y0 - 0.577350259;
  value += (a1 * a1) * (a1 * a1) * cwn_grad2(s, i1, j1, x1, y1);
  float xmyi = xi - yi;
  if (t > G2) {
    if (xi + xmyi > 1.0) {
      float x2 = x0 + -1.36602545, y2 = y0 + -0.366025388;
      float a2 = 0.666666687 - x2 * x2 - y2 * y2;
      if (a2 > 0.0) value += (a2 * a2) * (a2 * a2) * cwn_grad2(s, i + (CWN_PX << 1), j + CWN_PY, x2, y2);
    } else {
      float x2 = x0 + G2, y2 = y0 + -0.788675129;
      float a2 = 0.666666687 - x2 * x2 - y2 * y2;
      if (a2 > 0.0) value += (a2 * a2) * (a2 * a2) * cwn_grad2(s, i, j + CWN_PY, x2, y2);
    }
    if (yi - xmyi > 1.0) {
      float x3 = x0 + -0.366025388, y3 = y0 + -1.36602545;
      float a3 = 0.666666687 - x3 * x3 - y3 * y3;
      if (a3 > 0.0) value += (a3 * a3) * (a3 * a3) * cwn_grad2(s, i + CWN_PX, j + (CWN_PY << 1), x3, y3);
    } else {
      float x3 = x0 + -0.788675129, y3 = y0 + G2;
      float a3 = 0.666666687 - x3 * x3 - y3 * y3;
      if (a3 > 0.0) value += (a3 * a3) * (a3 * a3) * cwn_grad2(s, i + CWN_PX, j, x3, y3);
    }
  } else {
    if (xi + xmyi < 0.0) {
      float x2 = x0 + 0.788675129, y2 = y0 - G2;
      float a2 = 0.666666687 - x2 * x2 - y2 * y2;
      if (a2 > 0.0) value += (a2 * a2) * (a2 * a2) * cwn_grad2(s, i - CWN_PX, j, x2, y2);
    } else {
      float x2 = x0 + -0.788675129, y2 = y0 + G2;
      float a2 = 0.666666687 - x2 * x2 - y2 * y2;
      if (a2 > 0.0) value += (a2 * a2) * (a2 * a2) * cwn_grad2(s, i + CWN_PX, j, x2, y2);
    }
    if (yi < xmyi) {
      float x2 = x0 - G2, y2 = y0 - -0.788675129;
      float a2 = 0.666666687 - x2 * x2 - y2 * y2;
      if (a2 > 0.0) value += (a2 * a2) * (a2 * a2) * cwn_grad2(s, i, j - CWN_PY, x2, y2);
    } else {
      float x2 = x0 + G2, y2 = y0 + -0.788675129;
      float a2 = 0.666666687 - x2 * x2 - y2 * y2;
      if (a2 > 0.0) value += (a2 * a2) * (a2 * a2) * cwn_grad2(s, i, j + CWN_PY, x2, y2);
    }
  }
  return value * 18.24196194486065;
}

float cwn_os2s_3(uint s, float x, float y, float z) {
  int i0 = cwn_floor(x), j0 = cwn_floor(y), k0 = cwn_floor(z);
  float xi = x - float(i0), yi = y - float(j0), zi = z - float(k0);
  uint i = uint(i0) * CWN_PX, j = uint(j0) * CWN_PY, k = uint(k0) * CWN_PZ;
  uint s2 = s + 1293373u;
  int xm = int(-0.5 - xi), ym = int(-0.5 - yi), zm = int(-0.5 - zi);
  uint uxm = uint(xm), uym = uint(ym), uzm = uint(zm);
  float x0 = xi + float(xm), y0 = yi + float(ym), z0 = zi + float(zm);
  float a0 = 0.75 - x0 * x0 - y0 * y0 - z0 * z0;
  float value = (a0 * a0) * (a0 * a0) * cwn_grad3(s, i + (uxm & CWN_PX), j + (uym & CWN_PY), k + (uzm & CWN_PZ), x0, y0, z0);
  float x1 = xi - 0.5, y1 = yi - 0.5, z1 = zi - 0.5;
  float a1 = 0.75 - x1 * x1 - y1 * y1 - z1 * z1;
  value += (a1 * a1) * (a1 * a1) * cwn_grad3(s2, i + CWN_PX, j + CWN_PY, k + CWN_PZ, x1, y1, z1);
  float xf0 = float((xm | 1) << 1) * x1, yf0 = float((ym | 1) << 1) * y1, zf0 = float((zm | 1) << 1) * z1;
  float xf1 = float(-2 - (xm << 2)) * x1 - 1.0, yf1 = float(-2 - (ym << 2)) * y1 - 1.0, zf1 = float(-2 - (zm << 2)) * z1 - 1.0;
  float sx = float(xm | 1), sy = float(ym | 1), sz = float(zm | 1);
  bool skip5 = false;
  float a2 = xf0 + a0;
  if (a2 > 0.0) {
    value += (a2 * a2) * (a2 * a2) * cwn_grad3(s, i + (~uxm & CWN_PX), j + (uym & CWN_PY), k + (uzm & CWN_PZ), x0 - sx, y0, z0);
  } else {
    float a3 = yf0 + zf0 + a0;
    if (a3 > 0.0) value += (a3 * a3) * (a3 * a3) * cwn_grad3(s, i + (uxm & CWN_PX), j + (~uym & CWN_PY), k + (~uzm & CWN_PZ), x0, y0 - sy, z0 - sz);
    float a4 = xf1 + a1;
    if (a4 > 0.0) {
      value += (a4 * a4) * (a4 * a4) * cwn_grad3(s2, i + (uxm & (CWN_PX * 2u)), j + CWN_PY, k + CWN_PZ, sx + x1, y1, z1);
      skip5 = true;
    }
  }
  bool skip9 = false;
  float a6 = yf0 + a0;
  if (a6 > 0.0) {
    value += (a6 * a6) * (a6 * a6) * cwn_grad3(s, i + (uxm & CWN_PX), j + (~uym & CWN_PY), k + (uzm & CWN_PZ), x0, y0 - sy, z0);
  } else {
    float a7 = xf0 + zf0 + a0;
    if (a7 > 0.0) value += (a7 * a7) * (a7 * a7) * cwn_grad3(s, i + (~uxm & CWN_PX), j + (uym & CWN_PY), k + (~uzm & CWN_PZ), x0 - sx, y0, z0 - sz);
    float a8 = yf1 + a1;
    if (a8 > 0.0) {
      value += (a8 * a8) * (a8 * a8) * cwn_grad3(s2, i + CWN_PX, j + (uym & (CWN_PY << 1)), k + CWN_PZ, x1, sy + y1, z1);
      skip9 = true;
    }
  }
  bool skipD = false;
  float aA = zf0 + a0;
  if (aA > 0.0) {
    value += (aA * aA) * (aA * aA) * cwn_grad3(s, i + (uxm & CWN_PX), j + (uym & CWN_PY), k + (~uzm & CWN_PZ), x0, y0, z0 - sz);
  } else {
    float aB = xf0 + yf0 + a0;
    if (aB > 0.0) value += (aB * aB) * (aB * aB) * cwn_grad3(s, i + (~uxm & CWN_PX), j + (~uym & CWN_PY), k + (uzm & CWN_PZ), x0 - sx, y0 - sy, z0);
    float aC = zf1 + a1;
    if (aC > 0.0) {
      value += (aC * aC) * (aC * aC) * cwn_grad3(s2, i + CWN_PX, j + CWN_PY, k + (uzm & (CWN_PZ << 1)), x1, y1, sz + z1);
      skipD = true;
    }
  }
  if (!skip5) {
    float a5 = yf1 + zf1 + a1;
    if (a5 > 0.0) value += (a5 * a5) * (a5 * a5) * cwn_grad3(s2, i + CWN_PX, j + (uym & (CWN_PY << 1)), k + (uzm & (CWN_PZ << 1)), x1, sy + y1, sz + z1);
  }
  if (!skip9) {
    float a9 = xf1 + zf1 + a1;
    if (a9 > 0.0) value += (a9 * a9) * (a9 * a9) * cwn_grad3(s2, i + (uxm & (CWN_PX * 2u)), j + CWN_PY, k + (uzm & (CWN_PZ << 1)), sx + x1, y1, sz + z1);
  }
  if (!skipD) {
    float aD = xf1 + yf1 + a1;
    if (aD > 0.0) value += (aD * aD) * (aD * aD) * cwn_grad3(s2, i + (uxm & (CWN_PX << 1)), j + (uym & (CWN_PY << 1)), k + CWN_PZ, sx + x1, sy + y1, z1);
  }
  return value * 9.046026385208288;
}

// ── Cellular ──
float cwn_cell_ret(CwNoise n, float d0, float d1, uint closest) {
  if (n.cellular_distance == 0 && n.cellular_return >= 1) {
    d0 = cwn_fsqrt(d0);
    if (n.cellular_return >= 2) d1 = cwn_fsqrt(d1);
  }
  switch (n.cellular_return) {
  case 0: return float(int(closest)) * 4.65661287e-10;
  case 1: return d0 - 1.0;
  case 2: return d1 - 1.0;
  case 3: return (d1 + d0) * 0.5 - 1.0;
  case 4: return d1 - d0 - 1.0;
  case 5: return d1 * d0 * 0.5 - 1.0;
  case 6: return d0 / d1 - 1.0;
  }
  return 0.0;
}

float cwn_cell_2(CwNoise n, uint s, float x, float y) {
  int xr = cwn_round(x), yr = cwn_round(y);
  float d0 = 3.40282347e38, d1 = 3.40282347e38;
  uint closest = 0u;
  float jit = 0.43701595 * n.cellular_jitter;
  uint xp = uint(xr - 1) * CWN_PX, ypb = uint(yr - 1) * CWN_PY;
  for (int xi = xr - 1; xi <= xr + 1; xi++) {
    uint yp = ypb;
    for (int yi = yr - 1; yi <= yr + 1; yi++) {
      uint h = cwn_hash2(s, xp, yp);
      int idx = int(h & 510u);
      float vx = (float(xi) - x) + CWN_R2D[idx] * jit;
      float vy = (float(yi) - y) + CWN_R2D[idx | 1] * jit;
      float nd;
      if (n.cellular_distance == 2) nd = abs(vx) + abs(vy);
      else if (n.cellular_distance == 3) nd = (abs(vx) + abs(vy)) + (vx * vx + vy * vy);
      else nd = vx * vx + vy * vy;
      d1 = max(min(d1, nd), d0);
      if (nd < d0) { d0 = nd; closest = h; }
      yp += CWN_PY;
    }
    xp += CWN_PX;
  }
  return cwn_cell_ret(n, d0, d1, closest);
}

float cwn_cell_3(CwNoise n, uint s, float x, float y, float z) {
  int xr = cwn_round(x), yr = cwn_round(y), zr = cwn_round(z);
  float d0 = 3.40282347e38, d1 = 3.40282347e38;
  uint closest = 0u;
  float jit = 0.39614353 * n.cellular_jitter;
  uint xp = uint(xr - 1) * CWN_PX, ypb = uint(yr - 1) * CWN_PY, zpb = uint(zr - 1) * CWN_PZ;
  for (int xi = xr - 1; xi <= xr + 1; xi++) {
    uint yp = ypb;
    for (int yi = yr - 1; yi <= yr + 1; yi++) {
      uint zp = zpb;
      for (int zi = zr - 1; zi <= zr + 1; zi++) {
        uint h = cwn_hash3(s, xp, yp, zp);
        int idx = int(h & 1020u);
        float vx = (float(xi) - x) + CWN_R3D[idx] * jit;
        float vy = (float(yi) - y) + CWN_R3D[idx | 1] * jit;
        float vz = (float(zi) - z) + CWN_R3D[idx | 2] * jit;
        float nd;
        if (n.cellular_distance == 2) nd = abs(vx) + abs(vy) + abs(vz);
        else if (n.cellular_distance == 3) nd = (abs(vx) + abs(vy) + abs(vz)) + (vx * vx + vy * vy + vz * vz);
        else nd = vx * vx + vy * vy + vz * vz;
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
float cwn_perlin_2(uint s, float x, float y) {
  int x0i = cwn_floor(x), y0i = cwn_floor(y);
  float xd0 = x - float(x0i), yd0 = y - float(y0i);
  float xd1 = xd0 - 1.0, yd1 = yd0 - 1.0;
  float xs = cwn_quintic(xd0), ys = cwn_quintic(yd0);
  uint x0 = uint(x0i) * CWN_PX, y0 = uint(y0i) * CWN_PY;
  uint x1 = x0 + CWN_PX, y1 = y0 + CWN_PY;
  float xf0 = cwn_lerp(cwn_grad2(s, x0, y0, xd0, yd0), cwn_grad2(s, x1, y0, xd1, yd0), xs);
  float xf1 = cwn_lerp(cwn_grad2(s, x0, y1, xd0, yd1), cwn_grad2(s, x1, y1, xd1, yd1), xs);
  return cwn_lerp(xf0, xf1, ys) * 1.4247691104677813;
}

float cwn_perlin_3(uint s, float x, float y, float z) {
  int x0i = cwn_floor(x), y0i = cwn_floor(y), z0i = cwn_floor(z);
  float xd0 = x - float(x0i), yd0 = y - float(y0i), zd0 = z - float(z0i);
  float xd1 = xd0 - 1.0, yd1 = yd0 - 1.0, zd1 = zd0 - 1.0;
  float xs = cwn_quintic(xd0), ys = cwn_quintic(yd0), zs = cwn_quintic(zd0);
  uint x0 = uint(x0i) * CWN_PX, y0 = uint(y0i) * CWN_PY, z0 = uint(z0i) * CWN_PZ;
  uint x1 = x0 + CWN_PX, y1 = y0 + CWN_PY, z1 = z0 + CWN_PZ;
  float xf00 = cwn_lerp(cwn_grad3(s, x0, y0, z0, xd0, yd0, zd0), cwn_grad3(s, x1, y0, z0, xd1, yd0, zd0), xs);
  float xf10 = cwn_lerp(cwn_grad3(s, x0, y1, z0, xd0, yd1, zd0), cwn_grad3(s, x1, y1, z0, xd1, yd1, zd0), xs);
  float xf01 = cwn_lerp(cwn_grad3(s, x0, y0, z1, xd0, yd0, zd1), cwn_grad3(s, x1, y0, z1, xd1, yd0, zd1), xs);
  float xf11 = cwn_lerp(cwn_grad3(s, x0, y1, z1, xd0, yd1, zd1), cwn_grad3(s, x1, y1, z1, xd1, yd1, zd1), xs);
  float yf0 = cwn_lerp(xf00, xf10, ys), yf1 = cwn_lerp(xf01, xf11, ys);
  return cwn_lerp(yf0, yf1, zs) * 0.964921414852142333984375;
}

float cwn_vcubic_2(uint s, float x, float y) {
  int x1i = cwn_floor(x), y1i = cwn_floor(y);
  float xs = x - float(x1i), ys = y - float(y1i);
  uint x1 = uint(x1i) * CWN_PX, y1 = uint(y1i) * CWN_PY;
  uint x0 = x1 - CWN_PX, y0 = y1 - CWN_PY, x2 = x1 + CWN_PX, y2 = y1 + CWN_PY;
  uint x3 = x1 + (CWN_PX << 1), y3 = y1 + (CWN_PY << 1);
  return cwn_cubic(
    cwn_cubic(cwn_val2(s, x0, y0), cwn_val2(s, x1, y0), cwn_val2(s, x2, y0), cwn_val2(s, x3, y0), xs),
    cwn_cubic(cwn_val2(s, x0, y1), cwn_val2(s, x1, y1), cwn_val2(s, x2, y1), cwn_val2(s, x3, y1), xs),
    cwn_cubic(cwn_val2(s, x0, y2), cwn_val2(s, x1, y2), cwn_val2(s, x2, y2), cwn_val2(s, x3, y2), xs),
    cwn_cubic(cwn_val2(s, x0, y3), cwn_val2(s, x1, y3), cwn_val2(s, x2, y3), cwn_val2(s, x3, y3), xs),
    ys) * 0.444444448;
}

float cwn_vcubic_3(uint s, float x, float y, float z) {
  int x1i = cwn_floor(x), y1i = cwn_floor(y), z1i = cwn_floor(z);
  float xs = x - float(x1i), ys = y - float(y1i), zs = z - float(z1i);
  uint xb = uint(x1i) * CWN_PX, yb = uint(y1i) * CWN_PY, zb = uint(z1i) * CWN_PZ;
  uint X[4] = uint[4](xb - CWN_PX, xb, xb + CWN_PX, xb + (CWN_PX << 1));
  uint Y[4] = uint[4](yb - CWN_PY, yb, yb + CWN_PY, yb + (CWN_PY << 1));
  uint Z[4] = uint[4](zb - CWN_PZ, zb, zb + CWN_PZ, zb + (CWN_PZ << 1));
  float zr[4];
  for (int c = 0; c < 4; c++) {
    float yr[4];
    for (int b = 0; b < 4; b++)
      yr[b] = cwn_cubic(cwn_val3(s, X[0], Y[b], Z[c]), cwn_val3(s, X[1], Y[b], Z[c]), cwn_val3(s, X[2], Y[b], Z[c]), cwn_val3(s, X[3], Y[b], Z[c]), xs);
    zr[c] = cwn_cubic(yr[0], yr[1], yr[2], yr[3], ys);
  }
  return cwn_cubic(zr[0], zr[1], zr[2], zr[3], zs) * 0.296296299;
}

float cwn_value_2(uint s, float x, float y) {
  int x0i = cwn_floor(x), y0i = cwn_floor(y);
  float xs = cwn_hermite(x - float(x0i)), ys = cwn_hermite(y - float(y0i));
  uint x0 = uint(x0i) * CWN_PX, y0 = uint(y0i) * CWN_PY;
  uint x1 = x0 + CWN_PX, y1 = y0 + CWN_PY;
  float xf0 = cwn_lerp(cwn_val2(s, x0, y0), cwn_val2(s, x1, y0), xs);
  float xf1 = cwn_lerp(cwn_val2(s, x0, y1), cwn_val2(s, x1, y1), xs);
  return cwn_lerp(xf0, xf1, ys);
}

float cwn_value_3(uint s, float x, float y, float z) {
  int x0i = cwn_floor(x), y0i = cwn_floor(y), z0i = cwn_floor(z);
  float xs = cwn_hermite(x - float(x0i)), ys = cwn_hermite(y - float(y0i)), zs = cwn_hermite(z - float(z0i));
  uint x0 = uint(x0i) * CWN_PX, y0 = uint(y0i) * CWN_PY, z0 = uint(z0i) * CWN_PZ;
  uint x1 = x0 + CWN_PX, y1 = y0 + CWN_PY, z1 = z0 + CWN_PZ;
  float xf00 = cwn_lerp(cwn_val3(s, x0, y0, z0), cwn_val3(s, x1, y0, z0), xs);
  float xf10 = cwn_lerp(cwn_val3(s, x0, y1, z0), cwn_val3(s, x1, y1, z0), xs);
  float xf01 = cwn_lerp(cwn_val3(s, x0, y0, z1), cwn_val3(s, x1, y0, z1), xs);
  float xf11 = cwn_lerp(cwn_val3(s, x0, y1, z1), cwn_val3(s, x1, y1, z1), xs);
  float yf0 = cwn_lerp(xf00, xf10, ys), yf1 = cwn_lerp(xf01, xf11, ys);
  return cwn_lerp(yf0, yf1, zs);
}

// ── dispatch, fractals ──
float cwn_single2(CwNoise n, int seed, float x, float y) {
  uint s = uint(seed);
  switch (n.type) {
  case 0: return cwn_os2_2(s, x, y);
  case 1: return cwn_os2s_2(s, x, y);
  case 2: return cwn_cell_2(n, s, x, y);
  case 3: return cwn_perlin_2(s, x, y);
  case 4: return cwn_vcubic_2(s, x, y);
  case 5: return cwn_value_2(s, x, y);
  }
  return 0.0;
}

float cwn_single3(CwNoise n, int seed, float x, float y, float z) {
  uint s = uint(seed);
  switch (n.type) {
  case 0: return cwn_os2_3(s, x, y, z);
  case 1: return cwn_os2s_3(s, x, y, z);
  case 2: return cwn_cell_3(n, s, x, y, z);
  case 3: return cwn_perlin_3(s, x, y, z);
  case 4: return cwn_vcubic_3(s, x, y, z);
  case 5: return cwn_value_3(s, x, y, z);
  }
  return 0.0;
}

float cwn_bounding(CwNoise n) {
  float gain = abs(n.gain), amp = gain, af = 1.0;
  for (int i = 1; i < n.octaves; i++) { af += amp; amp *= gain; }
  return 1.0 / af;
}

float cw_noise2(vec2 p, CwNoise n) {
  float x = p.x * n.frequency, y = p.y * n.frequency;
  if (n.type == 0 || n.type == 1) {
    float t = (x + y) * 0.366025388;
    x += t;
    y += t;
  }
  if (n.fractal < 1 || n.fractal > 3) return cwn_single2(n, n.seed, x, y);
  int seed = n.seed;
  float sum = 0.0, amp = cwn_bounding(n);
  for (int i = 0; i < n.octaves; i++) {
    float v = cwn_single2(n, seed++, x, y);
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

float cw_noise3(vec3 p, CwNoise n) {
  float x = p.x * n.frequency, y = p.y * n.frequency, z = p.z * n.frequency;
  if (n.rotation == 1) {
    float xy = x + y, s2 = xy * -0.211324871;
    z *= 0.577350259;
    x += s2 - z;
    y = y + s2 - z;
    z += xy * 0.577350259;
  } else if (n.rotation == 2) {
    float xz = x + z, s2 = xz * -0.211324871;
    y *= 0.577350259;
    x += s2 - y;
    z += s2 - y;
    y += xz * 0.577350259;
  } else if (n.type == 0 || n.type == 1) {
    float r = (x + y + z) * 0.666666687;
    x = r - x;
    y = r - y;
    z = r - z;
  }
  if (n.fractal < 1 || n.fractal > 3) return cwn_single3(n, n.seed, x, y, z);
  int seed = n.seed;
  float sum = 0.0, amp = cwn_bounding(n);
  for (int i = 0; i < n.octaves; i++) {
    float v = cwn_single3(n, seed++, x, y, z);
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

CwNoise cw_noise_make(int seed, float frequency, int type, int fractal, int octaves) {
  return CwNoise(seed, frequency, type, 0, fractal, octaves, 2.0, 0.5, 0.0, 2.0, 1, 1, 1.0);
}

// the 16 floats of cw_noise_gpu_params as four vec4 uniforms
CwNoise cw_noise_unpack(vec4 a, vec4 b, vec4 c, vec4 d) {
  return CwNoise(int(a.x), a.y, int(a.z), int(a.w), int(b.x), int(b.y), b.z, b.w, c.x, c.y, int(c.z), int(c.w), d.x);
}
