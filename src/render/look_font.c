/* The ASCII look's glyphs (looks.c): a TrueType font rasterized into character cells with stb_truetype (its own static
 * copy: render2d's is private to text.c). */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "cartwheel/core.h"
#include "look_internal.h"

#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_assert(x) ((void)0)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wsign-compare"
#include "stb_truetype.h"
#pragma GCC diagnostic pop

struct cw__look_font {
  stbtt_fontinfo info;
  const unsigned char *ttf;
  void *owned; /* a font read from an asset (malloc'd by cw_vfs_read) */
  float aspect;
};

cw__look_font *cw__look_font_new(const unsigned char *ttf, size_t len, void *owned) {
  if (!ttf || len < 12) return NULL;
  cw__look_font *f = cw_calloc(1, sizeof *f, CW_MEM_RENDER);
  int off = stbtt_GetFontOffsetForIndex(ttf, 0);
  if (off < 0 || !stbtt_InitFont(&f->info, ttf, off)) {
    cw_free(f);
    return NULL;
  }
  f->ttf = ttf;
  f->owned = owned;
  int asc, desc, gap, adv, lsb;
  stbtt_GetFontVMetrics(&f->info, &asc, &desc, &gap);
  stbtt_GetCodepointHMetrics(&f->info, 'M', &adv, &lsb);
  f->aspect = asc - desc > 0 ? (float)adv / (float)(asc - desc) : 0.6f;
  return f;
}

void cw__look_font_free(cw__look_font *f) {
  if (!f) return;
  free(f->owned);
  cw_free(f);
}

float cw__look_font_aspect(const cw__look_font *f) { return f ? f->aspect : 0.6f; }

/* codepoint cp into a cw x ch cell (row 0 the TOP), anti-aliased coverage 0..255, the advance box centred */
void cw__look_font_raster(const cw__look_font *f, int cp, int cw, int ch, unsigned char *out) {
  memset(out, 0, (size_t)cw * (size_t)ch);
  if (!f || cp <= ' ') return;
  int asc, desc, gap;
  stbtt_GetFontVMetrics(&f->info, &asc, &desc, &gap);
  float s = (float)ch / (float)(asc - desc);
  int adv, lsb;
  stbtt_GetCodepointHMetrics(&f->info, cp, &adv, &lsb);
  int x0, y0, x1, y1;
  stbtt_GetCodepointBitmapBox(&f->info, cp, s, s, &x0, &y0, &x1, &y1);
  int gw = x1 - x0, gh = y1 - y0;
  if (gw <= 0 || gh <= 0) return;
  unsigned char *g = cw_calloc((size_t)gw * (size_t)gh, 1, CW_MEM_RENDER);
  stbtt_MakeCodepointBitmap(&f->info, g, gw, gh, gw, s, s, cp);
  float base = (float)asc * s;
  int oy = (int)lrintf(base) + y0, ox = (int)lrintf(((float)cw - (float)adv * s) * 0.5f) + x0;
  for (int y = 0; y < gh; y++)
    for (int x = 0; x < gw; x++) {
      int cx = ox + x, cy = oy + y;
      if (cx < 0 || cy < 0 || cx >= cw || cy >= ch) continue;
      int v = out[cy * cw + cx] + g[y * gw + x];
      out[cy * cw + cx] = (unsigned char)(v > 255 ? 255 : v);
    }
  cw_free(g);
}
