/* wasi-sdk clang 22, -O3 -msimd128: the loop vectorizer drops the third
 * iteration of rings() and it returns 0 instead of 0x1.239a8p-4 (found when
 * a game's texture hash diverged from native). Cart builds pass
 * -fno-vectorize (CMakeLists.txt). Re-check on a toolchain update:
 *   deps/wasi-sdk/bin/clang --target=wasm32-wasip1 -O3 -msimd128 -ffp-contract=off \
 *     tests/repro/wasm-loop-vectorize.c -o out/r.wasm -lm   (then run it with node: WASI) */
#include <stdio.h>
#include <stdint.h>
#include <math.h>
#include <stdlib.h>
static uint32_t hash2(int x, int y, uint32_t seed) {
  uint32_t h = (uint32_t)x * 374761393u + (uint32_t)y * 668265263u + seed * 2246822519u;
  h = (h ^ (h >> 13)) * 1274126177u;
  return h ^ (h >> 16);
}
static float h01(int x, int y, uint32_t seed) { return (float)(hash2(x, y, seed) & 0xffffff) / 16777216.0f; }
static float smooth(float t) { return t * t * (3 - 2 * t); }
static int wrapi(int a, int p) { return ((a % p) + p) % p; }
static float clamp01(float x) { return x < 0 ? 0 : x > 1 ? 1 : x; }
static float vnoise(float x, float y, int p, uint32_t seed) {
  int x0 = (int)floorf(x), y0 = (int)floorf(y);
  float fx = smooth(x - x0), fy = smooth(y - y0);
  float a = h01(wrapi(x0, p), wrapi(y0, p), seed), b = h01(wrapi(x0 + 1, p), wrapi(y0, p), seed);
  float c = h01(wrapi(x0, p), wrapi(y0 + 1, p), seed), d = h01(wrapi(x0 + 1, p), wrapi(y0 + 1, p), seed);
  return a + (b - a) * fx + (c - a) * fy + (a - b - c + d) * fx * fy;
}
static float fbm(float u, float v, int p, int oct, uint32_t seed) {
  float s = 0, amp = 0.5f, norm = 0;
  for (int o = 0; o < oct; o++) { s += amp * vnoise(u * p, v * p, p, seed + (uint32_t)o * 101u); norm += amp; amp *= 0.5f; p *= 2; }
  return s / norm;
}
static uint8_t b8(float x) { return (uint8_t)(clamp01(x) * 255.0f + 0.5f); }
/* the crack texture's rings: a 3-trip fmaxf reduction over inlined value noise */
__attribute__((noinline)) float rings(float u, float v, float r) {
  float line = 0;
  for (int k = 1; k <= 3; k++) {
    float rr = 0.035f * k + 0.01f * fbm(u, v, 6, 2, 510 + (uint32_t)k);
    line = fmaxf(line, clamp01(1 - fabsf(r - rr) * 260) * (r < 0.2f ? 1 : 0));
  }
  return line;
}
int main(void) {
  volatile float x = 174, y = 130;
  float u = (x + 0.5f) / 256, v = (y + 0.5f) / 256, dx = u - 0.7f, dy = v - 0.62f, r = sqrtf(dx * dx + dy * dy);
  printf("rings = %a (scalar answer 0x1.239a8p-4: ring k=3 hits)\n", rings(u, v, r));
}
