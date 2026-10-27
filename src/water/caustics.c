/* The caustic pattern the water lays on what lies under it: light from the
 * sun bent by a rippled surface and gathered into bright lines on the floor.
 *
 * Made once, on the CPU, by tracing it: a tileable height field (a sum of
 * waves with whole wave numbers across the tile), one photon from each of
 * 3 x 3 points per texel moved by the surface's slope (the refraction, to
 * first order) and splatted where it lands, then a small tent blur and a
 * scale to a mean of 1. The shaders scroll two copies of it against each
 * other and keep the smaller, which makes the moving web.
 *
 * The sines are cw_sin / cw_cos and the order of the sums is fixed, so the
 * texture is the same bytes on every host. */
#include <math.h>
#include <string.h>

#include "cartwheel/math.h"
#include "cartwheel/platform.h"
#include "water_internal.h"

#define SUB 3
#define MAXW 64

static uint32_t hash(uint32_t v) {
  v = v * 747796405u + 2891336453u;
  uint32_t w = ((v >> ((v >> 28u) + 4u)) ^ v) * 277803737u;
  return (w >> 22u) ^ w;
}
static float unit(uint32_t h) { return (float)(h >> 8) * (1.0f / 16777216.0f); }

static uint8_t g_tex[CW_WATER_CAUSTIC_N * CW_WATER_CAUSTIC_N];
static bool g_done;

const uint8_t *cw__water_caustics(void) {
  if (g_done) return g_tex;
  const int n = CW_WATER_CAUSTIC_N, m = n * SUB;
  const float two_pi = 6.28318530718f;
  /* the waves: wave numbers 3 .. 8.5 across the tile, amplitude falling as 1 / k^2 */
  float kx[MAXW], ky[MAXW], amp[MAXW], ph[MAXW], lap2 = 0;
  int nw = 0;
  for (uint32_t i = 0; i < 40 && nw < MAXW; i++) {
    uint32_t h = hash(i * 7919u + 13u);
    int ix = (int)(unit(h) * 19.0f) - 9, iy = (int)(unit(hash(h ^ 0x9e3779b9u)) * 19.0f) - 9;
    float kk = sqrtf((float)(ix * ix + iy * iy));
    if (kk < 3.0f || kk > 8.5f) continue;
    kx[nw] = (float)ix, ky[nw] = (float)iy;
    amp[nw] = 1.0f / (kk * kk);
    ph[nw] = unit(hash(h + 77u)) * two_pi;
    float w = two_pi * kk / (float)n;
    lap2 += (amp[nw] * w * w) * (amp[nw] * w * w) * 0.5f;
    nw++;
  }
  /* how far a photon moves per unit of slope: the focus a little past the first folds */
  float dk = 1.3f / sqrtf(lap2);
  /* sin(tx + ty + ph) = sin(tx) cos(ty + ph) + cos(tx) sin(ty + ph): per-wave tables along x and y */
  float *sx = cw_alloc((size_t)nw * (size_t)m * 4 * sizeof(float), CW_MEM_RENDER);
  float *cx = sx + (size_t)nw * (size_t)m, *sy = cx + (size_t)nw * (size_t)m, *cy = sy + (size_t)nw * (size_t)m;
  for (int w = 0; w < nw; w++)
    for (int i = 0; i < m; i++) {
      float p = ((float)i + 0.5f) / (float)SUB;
      float tx = two_pi * kx[w] * p / (float)n, ty = two_pi * ky[w] * p / (float)n + ph[w];
      sx[w * m + i] = cw_sin(tx), cx[w * m + i] = cw_cos(tx);
      sy[w * m + i] = cw_sin(ty), cy[w * m + i] = cw_cos(ty);
    }
  float *img = cw_calloc((size_t)n * (size_t)n * 2, sizeof(float), CW_MEM_RENDER);
  float *blur = img + (size_t)n * (size_t)n;
  for (int j = 0; j < m; j++)
    for (int i = 0; i < m; i++) {
      float gx = 0, gy = 0;
      for (int w = 0; w < nw; w++) {
        float s = sx[w * m + i] * cy[w * m + j] + cx[w * m + i] * sy[w * m + j];
        float g = -amp[w] * s * two_pi / (float)n;
        gx += g * kx[w], gy += g * ky[w];
      }
      float x = ((float)i + 0.5f) / (float)SUB - dk * gx - 0.5f, y = ((float)j + 0.5f) / (float)SUB - dk * gy - 0.5f;
      float fx0 = floorf(x), fy0 = floorf(y);
      int x0 = (int)fx0, y0 = (int)fy0;
      float fx = x - fx0, fy = y - fy0;
      int xa = ((x0 % n) + n) % n, ya = ((y0 % n) + n) % n, xb = (xa + 1) % n, yb = (ya + 1) % n;
      img[ya * n + xa] += (1 - fx) * (1 - fy);
      img[ya * n + xb] += fx * (1 - fy);
      img[yb * n + xa] += (1 - fx) * fy;
      img[yb * n + xb] += fx * fy;
    }
  double sum = 0;
  for (int j = 0; j < n; j++)
    for (int i = 0; i < n; i++) {
      float s = 0;
      for (int dj = -1; dj <= 1; dj++)
        for (int di = -1; di <= 1; di++)
          s += img[((j + dj + n) % n) * n + (i + di + n) % n] * (float)((2 - (di < 0 ? -di : di)) * (2 - (dj < 0 ? -dj : dj)));
      blur[j * n + i] = s / 16.0f;
      sum += blur[j * n + i];
    }
  float mean = (float)(sum / (double)(n * n));
  /* stored as intensity / 4 (a mean of 1 is 64) */
  for (int k = 0; k < n * n; k++) {
    float v = blur[k] / mean * (255.0f / 4.0f) + 0.5f;
    g_tex[k] = (uint8_t)(v > 255.0f ? 255.0f : v);
  }
  cw_free(sx);
  cw_free(img);
  g_done = true;
  return g_tex;
}
