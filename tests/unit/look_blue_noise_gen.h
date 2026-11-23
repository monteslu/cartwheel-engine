/* The looks' 64 x 64 blue noise tile (src/render/look_blue_noise.h), made the way the engine made it at run time up to
 * v0.66.0: void and cluster (Ulichney 1993; a Gaussian of sigma 1.5 on the torus), as ranks scaled to 0..255, from an
 * integer seed with float energies summed in one order, so the same bytes on every host. The engine now ships the
 * table (it cost 46 ms of CPU on the first frame a dither look drew); unit_looks makes it again here and holds the
 * table to it, on every host. Regenerate the table with tools/gen-look-blue-noise.c. */
#ifndef LOOK_BLUE_NOISE_GEN_H
#define LOOK_BLUE_NOISE_GEN_H

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define LBN 64
static void lbn_add(float *e, const float *g, int pos, float sgn) {
  int px = pos % LBN, py = pos / LBN;
  for (int y = 0; y < LBN; y++)
    for (int x = 0; x < LBN; x++) e[y * LBN + x] += sgn * g[((y - py + LBN) % LBN) * LBN + ((x - px + LBN) % LBN)];
}

static void look_blue_noise_gen(uint8_t out[LBN * LBN]) {
  enum { N = LBN * LBN };
  float *g = malloc(sizeof(float) * N), *e = calloc(N, sizeof(float)), *e0 = malloc(sizeof(float) * N);
  uint8_t *on = calloc(N, 1), *proto = calloc(N, 1);
  int *rank = malloc(sizeof(int) * N);
  for (int y = 0; y < LBN; y++)
    for (int x = 0; x < LBN; x++) {
      int dx = x > LBN / 2 ? x - LBN : x, dy = y > LBN / 2 ? y - LBN : y;
      g[y * LBN + x] = expf(-(float)(dx * dx + dy * dy) / (2.0f * 1.5f * 1.5f));
    }
  /* the prototype: a tenth of the cells on, from a fixed LCG, then the tightest cluster moved into the largest void
   * until that is the same cell */
  uint32_t r = 12345u;
  int ones = 0;
  while (ones < N / 10) {
    r = r * 1664525u + 1013904223u;
    int pos = (int)((r >> 8) % N);
    if (!on[pos]) on[pos] = 1, ones++, lbn_add(e, g, pos, 1.0f);
  }
  for (int guard = 0; guard < 4 * N; guard++) {
    int tc = -1, lv = -1;
    for (int i = 0; i < N; i++)
      if (on[i] && (tc < 0 || e[i] > e[tc])) tc = i;
    on[tc] = 0, lbn_add(e, g, tc, -1.0f);
    for (int i = 0; i < N; i++)
      if (!on[i] && (lv < 0 || e[i] < e[lv])) lv = i;
    on[lv] = 1, lbn_add(e, g, lv, 1.0f);
    if (lv == tc) break;
  }
  memcpy(proto, on, N);
  memcpy(e0, e, sizeof(float) * N);
  /* ranks below the prototype's count: its tightest clusters taken out one by one */
  for (int k = ones - 1; k >= 0; k--) {
    int tc = -1;
    for (int i = 0; i < N; i++)
      if (on[i] && (tc < 0 || e[i] > e[tc])) tc = i;
    on[tc] = 0, lbn_add(e, g, tc, -1.0f), rank[tc] = k;
  }
  /* the rest: the largest void filled, one by one */
  memcpy(on, proto, N);
  memcpy(e, e0, sizeof(float) * N);
  for (int k = ones; k < N; k++) {
    int lv = -1;
    for (int i = 0; i < N; i++)
      if (!on[i] && (lv < 0 || e[i] < e[lv])) lv = i;
    on[lv] = 1, lbn_add(e, g, lv, 1.0f), rank[lv] = k;
  }
  for (int i = 0; i < N; i++) out[i] = (uint8_t)(rank[i] * 256 / N);
  free(g), free(e), free(e0), free(on), free(proto), free(rank);
}

#endif
