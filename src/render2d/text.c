/* render2d text: TTF/OTF through stb_truetype, glyphs baked on first use
 * (single-channel SDF into the linear atlas, or coverage bitmaps into the
 * nearest atlas), layout with kerning, wrapping and alignment; fallback font
 * chains; complex scripts (bidi, OpenType shaping) through kb_text_shape. */
#include <stdlib.h>
#include <string.h>

#include "internal.h"
#include "cartwheel/platform.h"
#include "cartwheel/profile.h"

#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_assert(x) ((void)0)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wsign-compare"
#include "stb_truetype.h"
#pragma GCC diagnostic pop

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wsign-compare"
#include "kb_text_shape.h"
#pragma GCC diagnostic pop

typedef struct r2d_glyph {
  int index;       /* glyph index in its source font */
  int src;         /* slot in the font's effective chain (0 = the font itself) */
  cw_texture tex;  /* null for blank glyphs (space) */
  float xoff, yoff, w, h; /* bitmap placement relative to the pen on the baseline, baked pixels */
  float advance;   /* this font's units (a fallback glyph's advance converted) */
} r2d_glyph;

typedef struct r2d_font {
  stbtt_fontinfo info;
  uint8_t *data;
  size_t len;
  cw_font_mode mode;
  float bake_size, bake_scale; /* em px, and font units -> baked px */
  int pad;
  bool threshold;
  int ascent, descent, line_gap; /* font units */
  cw_map map;                    /* codepoint (or 2^32 * (slot + 1) + glyph id) -> glyph index + 1 */
  /* GPOS pair kerning ('kern' feature lookups, type 2 and type 9 wrapping
   * 2): stb_truetype only reads unwrapped format-4 values, which misses most
   * modern fonts */
  const uint8_t *gpos;
  CW_ARRAY(uint32_t) kern_subtables; /* absolute offsets of PairPos subtables, lookup order */
  CW_ARRAY(uint16_t) kern_lookup_end; /* subtable index where each lookup ends */
  cw_map kern_cache;                  /* (g1 << 16 | g2) -> kern + 0x8000 */
  CW_ARRAY(r2d_glyph) glyphs;
  /* fallbacks: this font's own chain, and the effective chain the glyph
   * cache was built for (this font, its chain, the global chain) */
  CW_ARRAY(uint64_t) chain;
  CW_ARRAY(uint64_t) eff;
  uint32_t epoch;              /* fallback epoch the cache matches */
  kbts_font *kb;               /* this font as kb_text_shape reads it (lazy) */
  kbts_shape_context *kb_ctx;  /* shaping context over the effective chain (lazy) */
  bool grow_warned;            /* an outline/shadow went past the SDF range (warned once) */
} r2d_font;

/* fallback state shared by every font */
static struct {
  CW_ARRAY(uint64_t) global;
  uint32_t epoch;   /* bumped by every chain change: caches rebuild lazily */
  cw_map missing;   /* codepoints no font had (distinct) */
  bool missing_init;
  uint32_t missing_count;
} FB = { .epoch = 1 };

static void shape_cache_clear(void);
static uint32_t shaped_now; /* runs shaped by the complex_layout calls since the caller zeroed it */

void r2d_fonts_init(void) { cw_pool_init(&R.fonts, sizeof(r2d_font), CW_MEM_RENDER2D); }

static r2d_font *font_get(cw_font f) { return R.inited ? cw_pool_get(&R.fonts, f.id) : NULL; }

uint32_t r2d_glyphs_missing(void) { return FB.missing_count; }

/* ── GPOS pair kerning ──────────────────────────────────────────────── */

static inline uint16_t be16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
static inline uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

static int coverage_index(const uint8_t *cov, int g) {
  uint16_t fmt = be16(cov);
  if (fmt == 1) {
    int n = be16(cov + 2), lo = 0, hi = n - 1;
    while (lo <= hi) {
      int mid = (lo + hi) / 2, v = be16(cov + 4 + 2 * mid);
      if (v == g) return mid;
      if (v < g) lo = mid + 1;
      else hi = mid - 1;
    }
  } else if (fmt == 2) {
    int n = be16(cov + 2), lo = 0, hi = n - 1;
    while (lo <= hi) {
      int mid = (lo + hi) / 2;
      const uint8_t *r = cov + 4 + 6 * mid;
      int a = be16(r), b = be16(r + 2);
      if (g < a) hi = mid - 1;
      else if (g > b) lo = mid + 1;
      else return be16(r + 4) + g - a;
    }
  }
  return -1;
}

static int class_of(const uint8_t *cd, int g) {
  uint16_t fmt = be16(cd);
  if (fmt == 1) {
    int start = be16(cd + 2), n = be16(cd + 4);
    return g >= start && g < start + n ? be16(cd + 6 + 2 * (g - start)) : 0;
  }
  if (fmt == 2) {
    int n = be16(cd + 2), lo = 0, hi = n - 1;
    while (lo <= hi) {
      int mid = (lo + hi) / 2;
      const uint8_t *r = cd + 4 + 6 * mid;
      int a = be16(r), b = be16(r + 2);
      if (g < a) hi = mid - 1;
      else if (g > b) lo = mid + 1;
      else return be16(r + 4);
    }
  }
  return 0;
}

static int popcount16(unsigned v) {
  int n = 0;
  for (; v; v &= v - 1) n++;
  return n;
}

/* the x-advance adjustment of one PairPos subtable for (g1, g2); *hit when
 * the subtable covers g1 (OpenType: the first covering subtable of a lookup
 * applies) */
static int pairpos(const uint8_t *t, int g1, int g2, bool *hit) {
  *hit = false;
  int ci = coverage_index(t + be16(t + 2), g1);
  if (ci < 0) return 0;
  uint16_t fmt = be16(t), vf1 = be16(t + 4), vf2 = be16(t + 6);
  int s1 = 2 * popcount16(vf1), s2 = 2 * popcount16(vf2);
  int xadv = (vf1 & 4) ? 2 * popcount16(vf1 & 3) : -1;
  if (fmt == 1) {
    if (ci >= be16(t + 8)) return 0;
    const uint8_t *set = t + be16(t + 10 + 2 * ci);
    int n = be16(set), rec = 2 + s1 + s2, lo = 0, hi = n - 1;
    while (lo <= hi) {
      int mid = (lo + hi) / 2;
      const uint8_t *r = set + 2 + rec * mid;
      int g = be16(r);
      if (g == g2) {
        *hit = true;
        return xadv >= 0 ? (int16_t)be16(r + 2 + xadv) : 0;
      }
      if (g < g2) lo = mid + 1;
      else hi = mid - 1;
    }
    return 0; /* format 1 without the pair: the next subtable may have it */
  }
  if (fmt == 2) {
    int c1 = class_of(t + be16(t + 8), g1), c2 = class_of(t + be16(t + 10), g2);
    int n1 = be16(t + 12), n2 = be16(t + 14);
    *hit = true;
    if (c1 >= n1 || c2 >= n2 || xadv < 0) return 0;
    return (int16_t)be16(t + 16 + (c1 * n2 + c2) * (s1 + s2) + xadv);
  }
  return 0;
}

static void gpos_init(r2d_font *f) {
  /* find the GPOS table in the table directory */
  const uint8_t *d = f->info.data + f->info.fontstart;
  int n = be16(d + 4);
  for (int i = 0; i < n; i++)
    if (!memcmp(d + 12 + 16 * i, "GPOS", 4)) f->gpos = f->info.data + be32(d + 12 + 16 * i + 8);
  if (!f->gpos || be16(f->gpos) != 1) {
    f->gpos = NULL;
    return;
  }
  const uint8_t *fl = f->gpos + be16(f->gpos + 6), *ll = f->gpos + be16(f->gpos + 8);
  int nl = be16(ll);
  bool *want = cw_calloc((size_t)nl + 1, 1, CW_MEM_RENDER2D);
  for (int i = 0, nf = be16(fl); i < nf; i++) {
    if (memcmp(fl + 2 + 6 * i, "kern", 4)) continue;
    const uint8_t *feat = fl + be16(fl + 2 + 6 * i + 4);
    for (int k = 0, nk = be16(feat + 2); k < nk; k++) {
      int li = be16(feat + 4 + 2 * k);
      if (li < nl) want[li] = true;
    }
  }
  for (int li = 0; li < nl; li++) {
    if (!want[li]) continue;
    const uint8_t *lk = ll + be16(ll + 2 + 2 * li);
    int type = be16(lk), ns = be16(lk + 4);
    for (int s = 0; s < ns; s++) {
      const uint8_t *st = lk + be16(lk + 6 + 2 * s);
      int t = type;
      if (t == 9) { /* extension: the real subtable sits behind a 32-bit offset */
        t = be16(st + 2);
        st = st + be32(st + 4);
      }
      if (t == 2) cw_array_push(&f->kern_subtables, (uint32_t)(st - f->info.data));
    }
    if (f->kern_subtables.count && (!f->kern_lookup_end.count || f->kern_lookup_end.data[f->kern_lookup_end.count - 1] != f->kern_subtables.count))
      cw_array_push(&f->kern_lookup_end, (uint16_t)f->kern_subtables.count);
  }
  cw_free(want);
  cw_map_init(&f->kern_cache, CW_MEM_RENDER2D);
}

/* kerning between two glyphs, font units */
static int kern(r2d_font *f, int g1, int g2) {
  if (!f->kern_subtables.count) return stbtt_GetGlyphKernAdvance(&f->info, g1, g2);
  uint64_t key = (uint64_t)(uint32_t)g1 << 16 | (uint32_t)g2, v;
  if (cw_map_get(&f->kern_cache, key, &v)) return (int)v - 0x8000;
  int total = 0, s = 0;
  for (int l = 0; l < f->kern_lookup_end.count; l++) {
    for (; s < f->kern_lookup_end.data[l]; s++) {
      bool hit;
      int k = pairpos(f->info.data + f->kern_subtables.data[s], g1, g2, &hit);
      if (hit) {
        total += k;
        s = f->kern_lookup_end.data[l];
        break;
      }
    }
  }
  cw_map_set(&f->kern_cache, key, (uint64_t)(total + 0x8000));
  return total;
}

/* ── kb_text_shape glue ─────────────────────────────────────────────── */

static void kb_alloc(void *data, kbts_allocator_op *op) {
  (void)data;
  if (op->Kind == KBTS_ALLOCATOR_OP_KIND_ALLOCATE) op->Allocate.Pointer = cw_alloc(op->Allocate.Size ? op->Allocate.Size : 1, CW_MEM_RENDER2D);
  else if (op->Kind == KBTS_ALLOCATOR_OP_KIND_FREE) cw_free(op->Free.Pointer);
}

static kbts_font *kb_font_of(r2d_font *f) {
  if (f->kb) return f->kb;
  kbts_font k = kbts_FontFromMemory(f->data, (int)f->len, 0, kb_alloc, NULL);
  if (!kbts_FontIsValid(&k)) {
    cw_warn("cw_font: kb_text_shape cannot read this font; complex-script text in it will not shape");
    return NULL;
  }
  f->kb = cw_alloc(sizeof *f->kb, CW_MEM_RENDER2D);
  *f->kb = k;
  return f->kb;
}

static void kb_ctx_free(r2d_font *f) {
  if (f->kb_ctx) kbts_DestroyShapeContext(f->kb_ctx);
  f->kb_ctx = NULL;
}

/* ── fallback chains ────────────────────────────────────────────────── */

static void glyphs_flush(r2d_font *f) {
  for (int i = 0; i < f->glyphs.count; i++) cw_texture_destroy(f->glyphs.data[i].tex);
  cw_array_clear(&f->glyphs);
  cw_map_free(&f->map);
  cw_map_init(&f->map, CW_MEM_RENDER2D);
}

static void eff_push(r2d_font *f, uint64_t id) {
  for (int i = 0; i < f->eff.count; i++)
    if (f->eff.data[i] == id) return;
  if (!cw_pool_get(&R.fonts, id)) return;
  cw_array_push(&f->eff, id);
}

/* bring the font's cache in line with the fallback chains */
static void font_sync(r2d_font *f, uint64_t self_id) {
  if (f->epoch == FB.epoch && f->eff.count) return;
  if (f->epoch != FB.epoch && f->glyphs.count) glyphs_flush(f);
  kb_ctx_free(f);
  cw_array_clear(&f->eff);
  cw_array_push(&f->eff, self_id);
  for (int i = 0; i < f->chain.count; i++) eff_push(f, f->chain.data[i]);
  for (int i = 0; i < FB.global.count; i++) eff_push(f, FB.global.data[i]);
  f->epoch = FB.epoch;
}

static r2d_font *eff_font(r2d_font *f, int slot) {
  if (slot <= 0 || slot >= f->eff.count) return f;
  r2d_font *s = cw_pool_get(&R.fonts, f->eff.data[slot]);
  return s ? s : f;
}

/* invisible format/control codepoints never count as missing */
static bool is_invisible(uint32_t cp) {
  return cp < 0x20 || (cp >= 0x7f && cp < 0xa0) || cp == 0xad || (cp >= 0x200b && cp <= 0x200f) ||
         (cp >= 0x2028 && cp <= 0x202e) || (cp >= 0x2060 && cp <= 0x2069) || cp == 0xfeff;
}

static void note_missing(uint32_t cp) {
  if (is_invisible(cp)) return;
  if (!FB.missing_init) {
    cw_map_init(&FB.missing, CW_MEM_RENDER2D);
    FB.missing_init = true;
  }
  uint64_t v;
  if (cw_map_get(&FB.missing, cp, &v)) return;
  cw_map_set(&FB.missing, cp, 1);
  FB.missing_count++;
  cw_warn("text: no font in the chain has U+%04X (add a fallback font that covers it)", cp);
}

void cw_font_set_fallbacks(cw_font h, const cw_font *chain, int n) {
  r2d_font *f = font_get(h);
  if (!f) return;
  cw_array_clear(&f->chain);
  for (int i = 0; i < n; i++)
    if (chain[i].id != h.id && !CW_HANDLE_IS_NULL(chain[i])) cw_array_push(&f->chain, chain[i].id);
  FB.epoch++;
}

int cw_font_fallbacks(cw_font h, cw_font *out, int cap) {
  r2d_font *f = font_get(h);
  if (!f) return 0;
  for (int i = 0; i < f->chain.count && i < cap; i++) out[i] = (cw_font){ f->chain.data[i] };
  return f->chain.count;
}

void cw_font_set_global_fallbacks(const cw_font *chain, int n) {
  cw_array_clear(&FB.global);
  for (int i = 0; i < n; i++)
    if (!CW_HANDLE_IS_NULL(chain[i])) cw_array_push(&FB.global, chain[i].id);
  FB.epoch++;
}

bool cw_font_has_glyph(cw_font h, uint32_t cp) {
  r2d_font *f = font_get(h);
  return f && stbtt_FindGlyphIndex(&f->info, (int)cp) != 0;
}

bool cw_font_covers(cw_font h, uint32_t cp) {
  r2d_font *f = font_get(h);
  if (!f) return false;
  font_sync(f, h.id);
  for (int i = 0; i < f->eff.count; i++)
    if (stbtt_FindGlyphIndex(&eff_font(f, i)->info, (int)cp)) return true;
  return false;
}

/* ── fonts ──────────────────────────────────────────────────────────── */

cw_font cw_font_from_memory(const void *ttf, size_t len, const cw_font_desc *desc) {
  if (!r2d_lazy_init()) return (cw_font){ 0 };
  cw_font_desc d = desc ? *desc : (cw_font_desc){ 0 };
  uint8_t *copy = cw_alloc(len, CW_MEM_RENDER2D);
  memcpy(copy, ttf, len);
  stbtt_fontinfo info;
  int off = stbtt_GetFontOffsetForIndex(copy, 0);
  if (off < 0 || !stbtt_InitFont(&info, copy, off)) {
    cw_error("cw_font: not a TrueType/OpenType font stb_truetype can read");
    cw_free(copy);
    return (cw_font){ 0 };
  }
  uint64_t id;
  r2d_font *f = cw_pool_alloc(&R.fonts, &id);
  f->info = info;
  f->data = copy;
  f->len = len;
  f->mode = d.mode;
  f->bake_size = d.bake_size > 0 ? d.bake_size : (d.mode == CW_FONT_SDF ? 48.0f : 16.0f);
  f->pad = d.sdf_padding > 0 ? d.sdf_padding : 6;
  f->threshold = d.threshold;
  f->bake_scale = stbtt_ScaleForMappingEmToPixels(&f->info, f->bake_size);
  stbtt_GetFontVMetrics(&f->info, &f->ascent, &f->descent, &f->line_gap);
  cw_map_init(&f->map, CW_MEM_RENDER2D);
  gpos_init(f);
  return (cw_font){ id };
}

cw_font cw_font_load(const char *path, const cw_font_desc *desc) {
  size_t len;
  void *data = cw_asset_load(path, &len);
  if (!data) {
    cw_error("cw_font_load: asset '%s' not found", path);
    return (cw_font){ 0 };
  }
  cw_font f = cw_font_from_memory(data, len, desc);
  free(data);
  return f;
}

static void font_free(r2d_font *f) {
  for (int i = 0; i < f->glyphs.count; i++) cw_texture_destroy(f->glyphs.data[i].tex);
  cw_array_free(&f->glyphs);
  cw_map_free(&f->map);
  cw_array_free(&f->kern_subtables);
  cw_array_free(&f->kern_lookup_end);
  if (f->gpos) cw_map_free(&f->kern_cache);
  cw_array_free(&f->chain);
  cw_array_free(&f->eff);
  kb_ctx_free(f);
  if (f->kb) {
    kbts_FreeFont(f->kb);
    cw_free(f->kb);
  }
  cw_free(f->data);
}

void cw_font_destroy(cw_font h) {
  r2d_font *f = font_get(h);
  if (!f) return;
  font_free(f);
  cw_pool_release(&R.fonts, h.id);
  FB.epoch++; /* chains naming it rebuild without it */
}

void r2d_fonts_shutdown(void) {
  for (int i = 0; i < R.fonts.slot_count; i++) {
    r2d_font *f = cw_pool_at(&R.fonts, i);
    if (f) font_free(f);
  }
  cw_pool_free_all(&R.fonts);
  cw_array_free(&FB.global);
  shape_cache_clear();
  if (FB.missing_init) cw_map_free(&FB.missing);
  FB.missing_init = false;
}

/* bake glyph `gi` of source slot `slot` (sized to f's em) into f's cache */
static int bake(r2d_font *f, int slot, int gi, uint64_t key) {
  CW_PROFILE_SCOPE("text.glyph_bake");
  r2d_font *sf = eff_font(f, slot);
  r2d_glyph g = { 0 };
  g.index = gi;
  g.src = slot;
  float scale = sf == f ? f->bake_scale : stbtt_ScaleForMappingEmToPixels(&sf->info, f->bake_size);
  int adv, lsb;
  stbtt_GetGlyphHMetrics(&sf->info, g.index, &adv, &lsb);
  g.advance = sf == f ? (float)adv : (float)adv * scale / f->bake_scale;
  int w = 0, h = 0, xo = 0, yo = 0;
  if (f->mode == CW_FONT_SDF) {
    uint8_t *sdf = stbtt_GetGlyphSDF(&sf->info, scale, g.index, f->pad, 128, 128.0f / (float)f->pad, &w, &h, &xo, &yo);
    if (sdf && w > 0 && h > 0) {
      uint8_t *rgba = cw_alloc((size_t)w * (size_t)h * 4, CW_MEM_RENDER2D);
      for (int i = 0; i < w * h; i++) rgba[4 * i] = rgba[4 * i + 1] = rgba[4 * i + 2] = 255, rgba[4 * i + 3] = sdf[i];
      /* the distance is data, not coverage: never premultiply it */
      g.tex = cw_texture_from_pixels(w, h, rgba, &(cw_texture_desc){ .filter = CW_FILTER_LINEAR, .premultiplied = true });
      cw_free(rgba);
    }
    if (sdf) stbtt_FreeSDF(sdf, NULL);
  } else {
    uint8_t *bm = stbtt_GetGlyphBitmap(&sf->info, scale, scale, g.index, &w, &h, &xo, &yo);
    if (bm && w > 0 && h > 0) {
      uint8_t *rgba = cw_alloc((size_t)w * (size_t)h * 4, CW_MEM_RENDER2D);
      for (int i = 0; i < w * h; i++) {
        uint8_t a = f->threshold ? (bm[i] >= 128 ? 255 : 0) : bm[i];
        rgba[4 * i] = rgba[4 * i + 1] = rgba[4 * i + 2] = 255;
        rgba[4 * i + 3] = a;
      }
      g.tex = cw_texture_from_pixels(w, h, rgba, &(cw_texture_desc){ .filter = CW_FILTER_NEAREST });
      cw_free(rgba);
    }
    if (bm) stbtt_FreeBitmap(bm, NULL);
  }
  g.xoff = (float)xo;
  g.yoff = (float)yo;
  g.w = (float)w;
  g.h = (float)h;
  cw_array_push(&f->glyphs, g);
  cw_map_set(&f->map, key, (uint64_t)f->glyphs.count);
  return f->glyphs.count - 1;
}

/* the glyph for a codepoint (this font, else the first fallback that has
 * it), baked on first use. Returns an index into f->glyphs: pointers into
 * the array move when a later glyph is baked. */
static int glyph_i(r2d_font *f, uint32_t cp) {
  uint64_t v;
  if (cw_map_get(&f->map, cp, &v)) return (int)v - 1;
  int gi = stbtt_FindGlyphIndex(&f->info, (int)cp), slot = 0;
  if (!gi && cp >= 0x80 && is_invisible(cp)) { /* a format mark the font lacks (RLM, ZWJ ...): zero width, nothing drawn */
    r2d_glyph g = { 0 };
    cw_array_push(&f->glyphs, g);
    cw_map_set(&f->map, cp, (uint64_t)f->glyphs.count);
    return f->glyphs.count - 1;
  }
  if (!gi && !is_invisible(cp)) {
    for (int s = 1; s < f->eff.count && !gi; s++) {
      r2d_font *sf = eff_font(f, s);
      if (sf == f) continue;
      int g2 = stbtt_FindGlyphIndex(&sf->info, (int)cp);
      if (g2) gi = g2, slot = s;
    }
    if (!gi) note_missing(cp);
  }
  return bake(f, slot, gi, cp);
}
static const r2d_glyph *glyph(r2d_font *f, uint32_t cp) {
  int i = glyph_i(f, cp); /* first: baking may move the array */
  return &f->glyphs.data[i];
}

/* a shaped glyph (by glyph id in a chain slot) */
static int glyph_id_i(r2d_font *f, int slot, int gid) {
  uint64_t key = ((uint64_t)(slot + 1) << 32) | (uint32_t)gid, v;
  if (cw_map_get(&f->map, key, &v)) return (int)v - 1;
  return bake(f, slot, gid, key);
}

/* UTF-8 decode; invalid bytes read as U+FFFD, one byte each */
static uint32_t utf8_next(const char **ps) {
  const uint8_t *s = (const uint8_t *)*ps;
  uint32_t c = s[0];
  int n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
  if (!n) {
    *ps += 1;
    return 0xfffd;
  }
  if (n == 1) {
    *ps += 1;
    return c;
  }
  c &= 0x7fu >> n;
  for (int i = 1; i < n; i++) {
    if ((s[i] & 0xc0) != 0x80) {
      *ps += 1;
      return 0xfffd;
    }
    c = (c << 6) | (s[i] & 0x3fu);
  }
  *ps += n;
  return c;
}

/* ── line breaking classes ──────────────────────────────────────────── */

/* ideographs, kana, hangul, CJK punctuation and fullwidth forms: a line may
 * break between two of them (and between one and a neighbour) */
static bool is_cjk(uint32_t c) {
  return (c >= 0x2e80 && c <= 0x9fff) || (c >= 0xa960 && c <= 0xa97f) || (c >= 0xac00 && c <= 0xd7ff) ||
         (c >= 0xf900 && c <= 0xfaff) || (c >= 0xfe30 && c <= 0xfe4f) || (c >= 0xff00 && c <= 0xffef) ||
         (c >= 0x20000 && c <= 0x3fffd);
}
/* kinsoku: never at a line start (closing punctuation, small kana, the long
 * vowel mark, iteration marks) */
static bool no_break_before(uint32_t c) {
  static const uint16_t set[] = { 0x3001, 0x3002, 0xff0c, 0xff0e, 0x30fb, 0xff1a, 0xff1b, 0xff1f, 0xff01, 0x30fc, 0x300d, 0x300f,
                                  0xff09, 0x3011, 0x3015, 0x3009, 0x300b, 0x3009, 0x309d, 0x309e, 0x30fd, 0x30fe, 0x3005, 0x3041,
                                  0x3043, 0x3045, 0x3047, 0x3049, 0x3063, 0x3083, 0x3085, 0x3087, 0x308e, 0x30a1, 0x30a3, 0x30a5,
                                  0x30a7, 0x30a9, 0x30c3, 0x30e3, 0x30e5, 0x30e7, 0x30ee, 0x30f5, 0x30f6, 0xff5d, 0x3019, 0x301f };
  if (c == ')' || c == ']' || c == '}' || c == ',' || c == '.' || c == '!' || c == '?' || c == ':' || c == ';') return true;
  for (size_t i = 0; i < CW_ARRAY_LEN(set); i++)
    if (set[i] == c) return true;
  return false;
}
/* kinsoku: never at a line end (opening brackets) */
static bool no_break_after(uint32_t c) {
  return c == 0x300c || c == 0x300e || c == 0xff08 || c == 0x3010 || c == 0x3014 || c == 0x3008 || c == 0x300a ||
         c == 0xff5b || c == 0x3018 || c == 0x301d || c == '(' || c == '[' || c == '{';
}
/* a break opportunity between prev and c that is not a space */
static bool cjk_break(uint32_t prev, uint32_t c) {
  return prev && (is_cjk(c) || is_cjk(prev)) && !no_break_before(c) && !no_break_after(prev) && c != ' ' && prev != ' ';
}

/* right-to-left and complex-shaping codepoints: the text takes the shaper */
static bool is_complex_cp(uint32_t c) {
  return (c >= 0x0300 && c <= 0x036f) || (c >= 0x0590 && c <= 0x08ff) || c == 0x200e || c == 0x200f || c == 0x061c || (c >= 0x0900 && c <= 0x0dff) ||
         (c >= 0x0e00 && c <= 0x0fff) || (c >= 0x1000 && c <= 0x109f) || (c >= 0x1780 && c <= 0x18af) ||
         (c >= 0x1a00 && c <= 0x1aff) || (c >= 0x1b00 && c <= 0x1bff) || (c >= 0x1dc0 && c <= 0x1dff) ||
         (c >= 0x20d0 && c <= 0x20ff) || (c >= 0xa800 && c <= 0xa8ff) || (c >= 0xa980 && c <= 0xa9ff) ||
         (c >= 0xfb1d && c <= 0xfdff) || (c >= 0xfe20 && c <= 0xfe2f) || (c >= 0xfe70 && c <= 0xfeff) ||
         (c >= 0x10800 && c <= 0x10fff) || (c >= 0x1e800 && c <= 0x1efff);
}

bool cw_text_is_complex(const char *text) {
  if (!text) return false;
  for (const char *p = text; *p;) {
    if ((unsigned char)*p < 0x80) {
      p++;
      continue;
    }
    if (is_complex_cp(utf8_next(&p))) return true;
  }
  return false;
}

/* ── layout ────────────────────────────────────────────────────────────── */

typedef struct layout_line { const char *begin, *end; float width; } layout_line;

typedef struct layout {
  float size, em, k; /* px size, font units -> px, baked px -> px */
  float line_h, ascent, descent, spacing;
  CW_ARRAY(layout_line) lines;
} layout;

static float style_size(const r2d_font *f, const cw_text_style *st) {
  return st && st->size > 0 ? st->size : f->bake_size;
}

/* kerning context: the previous glyph's index when it came from this font
 * (fallback glyphs never kern) */
static int kern_prev(const r2d_glyph *g) { return g->src == 0 ? g->index : -1; }

/* advance of a glyph after prev (kerning) in px; prev_index < 0 = none */
static float step(r2d_font *f, const layout *L, int prev_index, const r2d_glyph *g) {
  float x = g->advance * L->em + L->spacing;
  if (prev_index >= 0 && g->src == 0) x += (float)kern(f, prev_index, g->index) * L->em;
  return x;
}

static void layout_metrics(r2d_font *f, const cw_text_style *st, layout *L) {
  memset(L, 0, sizeof *L);
  L->size = style_size(f, st);
  L->em = stbtt_ScaleForMappingEmToPixels(&f->info, L->size);
  L->k = L->size / f->bake_size;
  L->spacing = st ? st->letter_spacing : 0;
  float ls = st && st->line_spacing > 0 ? st->line_spacing : 1;
  L->ascent = (float)f->ascent * L->em;
  L->descent = (float)f->descent * L->em;
  L->line_h = (float)(f->ascent - f->descent + f->line_gap) * L->em * ls;
}

static void do_layout(r2d_font *f, const char *text, const cw_text_style *st, layout *L) {
  layout_metrics(f, st, L);
  float wrap = st ? st->wrap_width : 0;
  const char *p = text, *line_begin = text;
  float x = 0;
  int prev = -1;
  uint32_t prev_cp = 0;
  /* last break opportunity on this line: the line would end at brk_end with
   * width brk_w, and the next start at brk_next */
  const char *brk_end = NULL, *brk_next = NULL;
  float brk_w = 0;
  bool in_space = false;
  while (*p) {
    const char *at = p;
    uint32_t cp = utf8_next(&p);
    if (cp == '\n') {
      cw_array_push(&L->lines, ((layout_line){ line_begin, at, x }));
      line_begin = p;
      x = 0;
      prev = -1;
      prev_cp = 0;
      brk_end = NULL;
      in_space = false;
      continue;
    }
    const r2d_glyph *g = glyph(f, cp);
    if (cp == ' ' || cp == '\t') {
      if (!in_space) { /* a run of spaces breaks before its first one */
        brk_end = at;
        brk_w = x;
      }
      in_space = true;
      x += step(f, L, prev, g);
      prev = kern_prev(g);
      prev_cp = cp;
      brk_next = p;
      continue;
    }
    if (!in_space && at != line_begin && cjk_break(prev_cp, cp)) { /* CJK: break before this character */
      brk_end = at;
      brk_w = x;
      brk_next = at;
    }
    in_space = false;
    float nx = x + step(f, L, prev, g);
    if (wrap > 0 && nx > wrap && at != line_begin) {
      if (brk_end && brk_end > line_begin) {
        /* break at the last opportunity: re-run what follows it */
        cw_array_push(&L->lines, ((layout_line){ line_begin, brk_end, brk_w }));
        line_begin = brk_next;
        p = brk_next;
      } else {
        /* one word wider than the line: break between characters */
        cw_array_push(&L->lines, ((layout_line){ line_begin, at, x }));
        line_begin = at;
        p = at;
      }
      x = 0;
      prev = -1;
      prev_cp = 0;
      brk_end = NULL;
      in_space = false;
      continue;
    }
    x = nx;
    prev = kern_prev(g);
    prev_cp = cp;
  }
  /* trailing spaces never count toward a line's width */
  float w = x;
  if (brk_end && brk_next == p) w = brk_w;
  cw_array_push(&L->lines, ((layout_line){ line_begin, p, w }));
}

static cw_text_metrics metrics_of(const layout *L) {
  cw_text_metrics m = { 0 };
  for (int i = 0; i < L->lines.count; i++) m.width = CW_MAX(m.width, L->lines.data[i].width);
  m.lines = L->lines.count;
  m.height = L->line_h * (float)m.lines;
  m.ascent = L->ascent;
  m.descent = L->descent;
  m.line_height = L->line_h;
  return m;
}

/* ── complex scripts: shaping, bidi, line breaking ──────────────────────
 * One paragraph (text between '\n') is shaped whole by kb_text_shape, which
 * segments it into runs (script, direction, font from the chain) and returns
 * each run's glyphs in visual order. Widths are attributed to the logical
 * codepoint each glyph came from, lines break in logical order (spaces, CJK
 * opportunities, else between characters), and each line's runs are then
 * reordered: an RTL paragraph reverses the run order and restores the order
 * inside each LTR group; an LTR paragraph reverses each group of RTL runs. */

typedef struct sh_glyph {
  int byte;         /* source byte offset in the paragraph (the cluster) */
  int gid, slot, run;
  bool rtl;         /* its run is right-to-left */
  float adv, ox, oy; /* px at the layout size; oy is up */
} sh_glyph;

typedef struct sh_cp { int byte; uint32_t cp; float w; } sh_cp;

typedef struct sh_line { int cp0, cp1; float width; } sh_line; /* logical codepoint range, trailing spaces excluded */

typedef struct sh_para {
  CW_ARRAY(sh_glyph) g;
  CW_ARRAY(sh_cp) cps;
  CW_ARRAY(sh_line) lines;
  bool rtl;
} sh_para;

static bool kb_ctx_ready(r2d_font *f) {
  if (f->kb_ctx) return true;
  f->kb_ctx = kbts_CreateShapeContext2(kb_alloc, NULL, KBTS_SHAPE_CONTEXT_FLAG_FONT_PRIORITY_BOTTOM_TO_TOP);
  if (!f->kb_ctx) return false;
  for (int s = 0; s < f->eff.count; s++) {
    r2d_font *sf = eff_font(f, s);
    kbts_font *k = kb_font_of(sf);
    if (k) kbts_ShapePushFont(f->kb_ctx, k);
  }
  return true;
}

static int slot_of(r2d_font *f, const kbts_font *k) {
  for (int s = 0; s < f->eff.count; s++)
    if (eff_font(f, s)->kb == k) return s;
  return 0;
}

static float em_of(r2d_font *f, int slot, float size) { return stbtt_ScaleForMappingEmToPixels(&eff_font(f, slot)->info, size); }

static int cp_at_byte(const sh_para *P, int byte) {
  int lo = 0, hi = P->cps.count - 1;
  while (lo <= hi) {
    int mid = (lo + hi) / 2;
    if (P->cps.data[mid].byte == byte) return mid;
    if (P->cps.data[mid].byte < byte) lo = mid + 1;
    else hi = mid - 1;
  }
  return hi >= 0 ? hi : 0;
}

/* shape [s, s+len) and break it into lines no wider than wrap (0 = no wrap) */
static void shape_para(r2d_font *f, const char *s, int len, const layout *L, int dir, float wrap, sh_para *P) {
  memset(P, 0, sizeof *P);
  for (const char *p = s; p < s + len;) {
    int b = (int)(p - s);
    uint32_t cp = utf8_next(&p);
    cw_array_push(&P->cps, ((sh_cp){ b, cp, 0 }));
  }
  if (!P->cps.count) return;
  if (kb_ctx_ready(f)) {
    /* An explicit paragraph direction goes in as a leading RLM / LRM (a
     * strong character) with the direction left to kb: given RTL directly,
     * kb resolves neutrals between two RTL runs (" · ") left to right,
     * against UAX #9's N1. The mark's glyphs are dropped below. */
    kbts_ShapeBegin(f->kb_ctx, KBTS_DIRECTION_DONT_KNOW, KBTS_LANGUAGE_DONT_KNOW);
    if (dir == CW_TEXT_DIR_RTL || dir == CW_TEXT_DIR_LTR)
      kbts_ShapeCodepointWithUserId(f->kb_ctx, dir == CW_TEXT_DIR_RTL ? 0x200f : 0x200e, -1);
    kbts_ShapeUtf8WithUserId(f->kb_ctx, s, len, 0, KBTS_USER_ID_GENERATION_MODE_SOURCE_INDEX);
    kbts_ShapeEnd(f->kb_ctx);
    kbts_run run;
    int ri = 0;
    while (kbts_ShapeRun(f->kb_ctx, &run)) {
      int slot = slot_of(f, run.Font);
      float em = em_of(f, slot, L->size);
      bool rtl = run.Direction == KBTS_DIRECTION_RTL;
      P->rtl = run.ParagraphDirection == KBTS_DIRECTION_RTL;
      kbts_glyph *kg;
      while (kbts_GlyphIteratorNext(&run.Glyphs, &kg)) {
        kbts_shape_codepoint sc;
        int byte = kbts_ShapeGetShapeCodepoint(f->kb_ctx, kg->UserIdOrCodepointIndex, &sc) ? sc.UserId : 0;
        if (byte < 0) continue; /* the direction mark */
        if (kg->Id == 0 && is_invisible(kg->Codepoint)) continue; /* format marks no font draws */
        if (kg->Id == 0) note_missing(kg->Codepoint);
        cw_array_push(&P->g, ((sh_glyph){ byte, kg->Id, slot, ri, rtl, (float)kg->AdvanceX * em + L->spacing,
                                          (float)kg->OffsetX * em, (float)kg->OffsetY * em }));
      }
      ri++;
    }
    shaped_now += (uint32_t)ri; /* complex_layout's caller counts them (once per layout) */
    if (kbts_ShapeError(f->kb_ctx) != KBTS_SHAPE_ERROR_NONE) {
      cw_warn("text: shaping failed (%d); drawing the paragraph unshaped", (int)kbts_ShapeError(f->kb_ctx));
      kb_ctx_free(f);
      cw_array_clear(&P->g);
    }
  }
  if (!P->g.count) { /* no shaper: one glyph per codepoint, logical order */
    for (int i = 0; i < P->cps.count; i++) {
      const r2d_glyph *g = glyph(f, P->cps.data[i].cp);
      cw_array_push(&P->g, ((sh_glyph){ P->cps.data[i].byte, g->index, g->src, 0, false, g->advance * L->em + L->spacing, 0, 0 }));
    }
  }
  for (int i = 0; i < P->g.count; i++) P->cps.data[cp_at_byte(P, P->g.data[i].byte)].w += P->g.data[i].adv;
  /* lines */
  int line0 = 0;
  float x = 0, brk_w = 0;
  int brk_end = -1, brk_next = -1;
  bool in_space = false;
  for (int i = 0; i < P->cps.count; i++) {
    uint32_t cp = P->cps.data[i].cp;
    float w = P->cps.data[i].w;
    if (cp == ' ' || cp == '\t') {
      if (!in_space) brk_end = i, brk_w = x;
      in_space = true;
      x += w;
      brk_next = i + 1;
      continue;
    }
    if (!in_space && i > line0 && cjk_break(P->cps.data[i - 1].cp, cp)) brk_end = i, brk_w = x, brk_next = i;
    in_space = false;
    if (wrap > 0 && x + w > wrap && i > line0) {
      if (brk_end > line0) {
        cw_array_push(&P->lines, ((sh_line){ line0, brk_end, brk_w }));
        line0 = i = brk_next;
      } else {
        cw_array_push(&P->lines, ((sh_line){ line0, i, x }));
        line0 = i;
      }
      x = 0;
      brk_end = -1;
      in_space = false;
      i--; /* re-run from the new line start */
      continue;
    }
    x += w;
  }
  int end = P->cps.count;
  float w = x;
  while (end > line0 && (P->cps.data[end - 1].cp == ' ' || P->cps.data[end - 1].cp == '\t')) w -= P->cps.data[--end].w;
  cw_array_push(&P->lines, ((sh_line){ line0, end, w }));
}

static void para_free(sh_para *P) {
  cw_array_free(&P->g);
  cw_array_free(&P->cps);
  cw_array_free(&P->lines);
}

/* a line's glyphs in visual order (indices into P->g), frame arena */
static int line_visual(const sh_para *P, const sh_line *ln, int *out) {
  int b0 = ln->cp0 < P->cps.count ? P->cps.data[ln->cp0].byte : 1 << 30;
  int b1 = ln->cp1 < P->cps.count ? P->cps.data[ln->cp1].byte : 1 << 30;
  /* runs touching the line, logical order */
  int nruns = 0, *runs = cw_frame_alloc(sizeof(int) * (size_t)(P->g.count + 1));
  bool *rtl = cw_frame_alloc(sizeof(bool) * (size_t)(P->g.count + 1));
  for (int i = 0; i < P->g.count; i++) {
    const sh_glyph *g = &P->g.data[i];
    if (g->byte < b0 || g->byte >= b1) continue;
    if (!nruns || runs[nruns - 1] != g->run) runs[nruns] = g->run, rtl[nruns++] = g->rtl;
  }
  /* reorder the runs */
  int *ord = cw_frame_alloc(sizeof(int) * (size_t)(nruns + 1));
  for (int i = 0; i < nruns; i++) ord[i] = i;
  if (P->rtl)
    for (int i = 0; i < nruns / 2; i++) {
      int t = ord[i];
      ord[i] = ord[nruns - 1 - i];
      ord[nruns - 1 - i] = t;
    }
  /* reverse each group of runs against the paragraph direction (as placed now) */
  for (int i = 0; i < nruns;) {
    bool against = rtl[ord[i]] != P->rtl;
    int j = i;
    while (j < nruns && (rtl[ord[j]] != P->rtl) == against) j++;
    if (against)
      for (int a = i, b = j - 1; a < b; a++, b--) {
        int t = ord[a];
        ord[a] = ord[b];
        ord[b] = t;
      }
    i = j;
  }
  int n = 0;
  for (int k = 0; k < nruns; k++)
    for (int i = 0; i < P->g.count; i++) {
      const sh_glyph *g = &P->g.data[i];
      if (g->run == runs[ord[k]] && g->byte >= b0 && g->byte < b1) out[n++] = i;
    }
  return n;
}

/* lay out complex text: paragraphs shaped, lines broken; calls `emit` per line */
typedef void (*line_fn)(r2d_font *f, const layout *L, const sh_para *P, const sh_line *ln, int line_index, void *user);
static int complex_layout(r2d_font *f, const char *text, const cw_text_style *st, layout *L, line_fn emit, void *user) {
  layout_metrics(f, st, L);
  float wrap = st ? st->wrap_width : 0;
  int dir = st ? st->direction : 0;
  int li = 0;
  for (const char *p = text;;) {
    const char *nl = strchr(p, '\n');
    int len = nl ? (int)(nl - p) : (int)strlen(p);
    sh_para P;
    shape_para(f, p, len, L, dir, wrap, &P);
    if (!P.lines.count) cw_array_push(&P.lines, ((sh_line){ 0, 0, 0 }));
    for (int i = 0; i < P.lines.count; i++) {
      cw_array_push(&L->lines, ((layout_line){ p, p + len, P.lines.data[i].width }));
      if (emit) emit(f, L, &P, &P.lines.data[i], li, user);
      li++;
    }
    para_free(&P);
    if (!nl) break;
    p = nl + 1;
  }
  return li;
}

/* ── the complex-layout cache ──────────────────────────────────────────
 * Shaping costs far more than drawing, and UI text is redrawn every frame
 * unchanged: complex layouts (metrics and glyph placements relative to the
 * text's origin) are kept by (font, fallback epoch, style, text) in a small
 * LRU. SDF fonts only (bitmap fonts snap each line to the pixel grid at the
 * draw position). */
typedef struct placed { int gi; float x, y; } placed; /* glyph index into f->glyphs */
#define SHAPE_CACHE 96
typedef struct shape_entry {
  uint64_t key;
  char *text;
  cw_text_metrics m;
  placed *pl;
  int n;
  bool placed_ok; /* placements were computed (a draw), not only metrics */
  uint32_t runs;  /* the shaped runs, counted again on every reuse */
  uint32_t used;
} shape_entry;
static struct { shape_entry e[SHAPE_CACHE]; uint32_t tick, runs; } SC;

/* cw_r2d_stats: a complex layout's runs (shaped now or reused from the
 * cache, so cached text does not read as 0) and whether it was a cache hit */
static void count_layout(uint32_t runs, bool hit) {
  r2d_stats_frame_check();
  R.stats.shaped_runs += runs;
  if (hit) R.stats.shape_cache_hits++;
}

static uint64_t shape_key(uint64_t font_id, const char *text, const cw_text_style *st) {
  float k[6] = { st ? st->size : 0, st ? st->wrap_width : 0, st ? (float)st->align : 0, st ? (float)st->direction : 0,
                 st ? st->letter_spacing : 0, st ? st->line_spacing : 0 };
  uint64_t h = cw_hash_bytes(k, sizeof k, font_id * 0x9e3779b97f4a7c15ull + FB.epoch);
  return cw_hash_bytes(text, strlen(text), h);
}
static shape_entry *shape_find(uint64_t key, const char *text) {
  for (int i = 0; i < SHAPE_CACHE; i++)
    if (SC.e[i].text && SC.e[i].key == key && !strcmp(SC.e[i].text, text)) {
      SC.e[i].used = ++SC.tick;
      return &SC.e[i];
    }
  return NULL;
}
static shape_entry *shape_slot(uint64_t key, const char *text) {
  int lru = 0;
  for (int i = 0; i < SHAPE_CACHE; i++) {
    if (!SC.e[i].text) {
      lru = i;
      break;
    }
    if (SC.e[i].used < SC.e[lru].used) lru = i;
  }
  shape_entry *e = &SC.e[lru];
  cw_free(e->text);
  cw_free(e->pl);
  memset(e, 0, sizeof *e);
  e->key = key;
  e->text = cw_strdup(text, CW_MEM_RENDER2D);
  e->used = ++SC.tick;
  return e;
}
static void shape_cache_clear(void) {
  for (int i = 0; i < SHAPE_CACHE; i++) {
    cw_free(SC.e[i].text);
    cw_free(SC.e[i].pl);
  }
  memset(&SC, 0, sizeof SC);
}

/* ── public layout entry points ─────────────────────────────────────── */

cw_text_metrics cw_font_measure(cw_font h, const char *text, const cw_text_style *st) {
  r2d_font *f = font_get(h);
  if (!f || !text) return (cw_text_metrics){ 0 };
  font_sync(f, h.id);
  layout L;
  bool complex = cw_text_is_complex(text);
  uint64_t key = complex ? shape_key(h.id, text, st) : 0;
  if (complex && f->mode == CW_FONT_SDF) {
    shape_entry *e = shape_find(key, text);
    if (e) {
      count_layout(e->runs, true);
      return e->m;
    }
  }
  shaped_now = 0;
  if (complex) complex_layout(f, text, st, &L, NULL, NULL);
  else do_layout(f, text, st, &L);
  cw_text_metrics m = metrics_of(&L);
  cw_array_free(&L.lines);
  if (complex) count_layout(shaped_now, false);
  if (complex && f->mode == CW_FONT_SDF) {
    shape_entry *e = shape_slot(key, text);
    e->m = m;
    e->runs = shaped_now;
  }
  return m;
}

void cw_font_preload_range(cw_font h, uint32_t first, uint32_t last) {
  r2d_font *f = font_get(h);
  if (!f || first > last || first > 0x10ffff) return;
  if (last > 0x10ffff) last = 0x10ffff;
  font_sync(f, h.id);
  for (uint32_t cp = first; cp <= last; cp++) {
    if (cp >= 0xd800 && cp <= 0xdfff) continue;
    (void)glyph(f, cp);
  }
}

void cw_font_preload_text(cw_font h, const char *utf8) {
  (void)cw_font_measure(h, utf8, NULL);
}

float cw_font_advance(cw_font h, uint32_t cp, float size) {
  r2d_font *f = font_get(h);
  if (!f) return 0;
  font_sync(f, h.id);
  return glyph(f, cp)->advance * stbtt_ScaleForMappingEmToPixels(&f->info, size > 0 ? size : f->bake_size);
}

float cw_font_caret_x(cw_font h, const char *text, int byte, float size) {
  r2d_font *f = font_get(h);
  if (!f || !text) return 0;
  font_sync(f, h.id);
  layout L = { 0 };
  L.size = size > 0 ? size : f->bake_size;
  L.em = stbtt_ScaleForMappingEmToPixels(&f->info, L.size);
  const char *p = text;
  float x = 0;
  int prev = -1;
  while (*p && p - text < byte) {
    uint32_t cp = utf8_next(&p);
    const r2d_glyph *g = glyph(f, cp);
    x += step(f, &L, prev, g);
    prev = kern_prev(g);
  }
  return x;
}

/* ── drawing ────────────────────────────────────────────────────────── */

typedef struct place_ctx {
  placed *pl;
  int n, cap;
  cw_vec2 pos;
  cw_text_align align;
  float box_w;
  bool pixel;
} place_ctx;

static void place_line(r2d_font *f, const layout *L, const sh_para *P, const sh_line *ln, int li, void *user) {
  place_ctx *c = user;
  float x = c->pos.x;
  if (c->align == CW_ALIGN_CENTER) x += (c->box_w - ln->width) * 0.5f;
  else if (c->align == CW_ALIGN_RIGHT) x += c->box_w - ln->width;
  float base = c->pos.y + L->ascent + L->line_h * (float)li;
  if (c->pixel) {
    x = floorf(x + 0.5f);
    base = floorf(base + 0.5f);
  }
  int *vis = cw_frame_alloc(sizeof(int) * (size_t)(P->g.count + 1));
  int nv = line_visual(P, ln, vis);
  for (int k = 0; k < nv; k++) {
    const sh_glyph *g = &P->g.data[vis[k]];
    int gi = glyph_id_i(f, g->slot, g->gid);
    if (!CW_HANDLE_IS_NULL(f->glyphs.data[gi].tex)) {
      if (c->n == c->cap) {
        int nc = c->cap * 2 + 16;
        placed *np = cw_frame_alloc(sizeof(placed) * (size_t)nc);
        if (c->n) memcpy(np, c->pl, sizeof(placed) * (size_t)c->n);
        c->pl = np, c->cap = nc;
      }
      c->pl[c->n++] = (placed){ gi, x + g->ox, base - g->oy };
    }
    x += g->adv;
  }
}

/* test hook (internal.h): every line's glyphs, as source byte offsets in
 * visual (left to right) order */
typedef struct vis_ctx { int *out, cap, n; } vis_ctx;
static void vis_line(r2d_font *f, const layout *L, const sh_para *P, const sh_line *ln, int li, void *user) {
  (void)f, (void)L, (void)li;
  vis_ctx *c = user;
  int *vis = cw_frame_alloc(sizeof(int) * (size_t)(P->g.count + 1));
  int nv = line_visual(P, ln, vis);
  for (int k = 0; k < nv; k++) {
    if (c->n < c->cap) c->out[c->n] = P->g.data[vis[k]].byte;
    c->n++;
  }
}
int r2d_text_visual_bytes(cw_font h, const char *text, const cw_text_style *st, int *out, int cap) {
  r2d_font *f = font_get(h);
  if (!f || !text || !cw_text_is_complex(text)) return 0;
  font_sync(f, h.id);
  layout L;
  vis_ctx c = { out, cap, 0 };
  complex_layout(f, text, st, &L, vis_line, &c);
  cw_array_free(&L.lines);
  return c.n;
}

/* text styles' outline and shadow: one extra copy of the glyph run each */
typedef struct text_copy {
  uint8_t c[4];
  float dx, dy, grow, soft; /* text px */
  uint8_t sdf_grow, sdf_soft; /* SDF: distance units x255 */
  const cw_vec2 *off;       /* bitmap: whole-pixel stamp offsets (frame arena) */
  int noff;
} text_copy;
/* the most an SDF copy may grow plus soften, in distance units: the glyph's
 * baked range ends at 0, 0.5 from the edge; keep room for anti-aliasing */
#define SDF_GROW_MAX 0.44f
/* bitmap fonts: every whole-pixel offset within radius r except the centre */
static const cw_vec2 *disc_offsets(float r, int *n) {
  int R0 = (int)ceilf(r - 0.01f);
  if (R0 < 1) R0 = 1;
  cw_vec2 *o = cw_frame_alloc(sizeof(cw_vec2) * (size_t)((2 * R0 + 1) * (2 * R0 + 1)));
  int k = 0;
  float lim = r * r + r; /* the pixel disc, corners included at radius 1 */
  for (int y = -R0; y <= R0; y++)
    for (int x = -R0; x <= R0; x++)
      if ((x || y) && (float)(x * x + y * y) <= lim) o[k++] = (cw_vec2){ (float)x, (float)y };
  *n = k;
  return o;
}

cw_text_metrics cw_r2d_text(cw_font h, const char *text, cw_vec2 pos, const cw_text_style *st) {
  r2d_font *f = font_get(h);
  if (!f || !text) return (cw_text_metrics){ 0 };
  font_sync(f, h.id);
  layout L;
  cw_text_align align = st ? st->align : CW_ALIGN_LEFT;
  bool pixel = f->mode == CW_FONT_BITMAP;
  place_ctx pc = { .pos = pos, .align = align, .pixel = pixel };
  cw_text_metrics m;
  /* place every glyph (baking any new ones) before touching the stream */
  if (cw_text_is_complex(text)) {
    uint64_t key = shape_key(h.id, text, st);
    shape_entry *e = pixel ? NULL : shape_find(key, text);
    if (e && e->placed_ok) { /* cached: shift the placements to pos */
      count_layout(e->runs, true);
      layout_metrics(f, st, &L);
      m = e->m;
      pc.n = e->n;
      pc.pl = cw_frame_alloc(sizeof(placed) * (size_t)(e->n + 1));
      for (int i = 0; i < e->n; i++) pc.pl[i] = (placed){ e->pl[i].gi, e->pl[i].x + pos.x, e->pl[i].y + pos.y };
    } else {
      /* two passes: the box width (alignment) needs every line first */
      shaped_now = 0;
      complex_layout(f, text, st, &L, NULL, NULL);
      uint32_t runs = shaped_now;
      count_layout(runs, false);
      m = metrics_of(&L);
      cw_array_free(&L.lines);
      pc.box_w = st && st->wrap_width > 0 ? st->wrap_width : m.width;
      if (!pixel) pc.pos = (cw_vec2){ 0, 0 }; /* placed at the origin, cached, then shifted */
      complex_layout(f, text, st, &L, place_line, &pc);
      cw_array_free(&L.lines);
      if (!pixel) {
        e = shape_find(key, text);
        if (!e) e = shape_slot(key, text);
        e->m = m;
        cw_free(e->pl);
        e->pl = cw_alloc(sizeof(placed) * (size_t)(pc.n + 1), CW_MEM_RENDER2D);
        if (pc.n) memcpy(e->pl, pc.pl, sizeof(placed) * (size_t)pc.n);
        e->n = pc.n;
        e->placed_ok = true;
        e->runs = runs;
        for (int i = 0; i < pc.n; i++) pc.pl[i].x += pos.x, pc.pl[i].y += pos.y;
      }
    }
  } else {
    do_layout(f, text, st, &L);
    m = metrics_of(&L);
    float box_w = st && st->wrap_width > 0 ? st->wrap_width : m.width;
    int cap = 0;
    for (const char *p = text; *p;) {
      utf8_next(&p);
      cap++;
    }
    pc.pl = cw_frame_alloc(sizeof(placed) * (size_t)(cap + 1));
    for (int li = 0; li < L.lines.count; li++) {
      const layout_line *ln = &L.lines.data[li];
      float x = pos.x;
      if (align == CW_ALIGN_CENTER) x += (box_w - ln->width) * 0.5f;
      else if (align == CW_ALIGN_RIGHT) x += box_w - ln->width;
      float base = pos.y + L.ascent + L.line_h * (float)li;
      if (pixel) {
        x = floorf(x + 0.5f);
        base = floorf(base + 0.5f);
      }
      int prev = -1;
      for (const char *p = ln->begin; p < ln->end;) {
        uint32_t cp = utf8_next(&p);
        int gi = glyph_i(f, cp);
        const r2d_glyph *g = &f->glyphs.data[gi];
        if (prev >= 0 && g->src == 0) x += (float)kern(f, prev, g->index) * L.em;
        if (!CW_HANDLE_IS_NULL(g->tex)) pc.pl[pc.n++] = (placed){ gi, x, base };
        x += g->advance * L.em + L.spacing;
        prev = kern_prev(g);
      }
    }
    cw_array_free(&L.lines);
  }
  int n = pc.n;
  placed *pl = pc.pl;
  if (!n) return m;
  const r2d_tex *t0 = r2d_tex_get(f->glyphs.data[pl[0].gi].tex);
  uint8_t mode = (uint8_t)(r2d_mode(false) | (f->mode == CW_FONT_SDF ? R2D_MODE_SDF : 0));
  uint8_t layer = (uint8_t)R.cur_layer;
  bool sdf = f->mode == CW_FONT_SDF;
  /* SDF anti-aliasing width: distance change per screen pixel (before the
   * layer camera, which the vertex shader folds in) */
  uint8_t sdfw = 0;
  if (sdf) {
    float ks = L.k;
    if (R.has_xform) ks *= sqrtf(fabsf(R.xform.a * R.xform.d - R.xform.b * R.xform.c));
    float per_px = (128.0f / (float)f->pad) / 255.0f / (ks > 1e-4f ? ks : 1e-4f);
    sdfw = (uint8_t)(cw_clampf(per_px, 0, 1) * 255.0f + 0.5f);
    if (!sdfw) sdfw = 1;
  }
  /* the copies of the glyph run, back to front: shadow, outline, fill. SDF
   * fonts grow (and soften) a copy in the fragment shader, from the one
   * layout and the same glyph quads; bitmap fonts stamp the copy at every
   * whole-pixel offset within the radius. One command, so they batch. */
  text_copy cp[3];
  int ncp = 0;
  float grow_max = 0;
  if (st && st->shadow_color.a > 0) {
    cp[ncp] = (text_copy){ .dx = st->shadow_offset.x, .dy = st->shadow_offset.y, .grow = CW_MAX(st->shadow_spread, 0),
                           .soft = CW_MAX(st->shadow_softness, 0) };
    r2d_pack_color(cp[ncp].c, st->shadow_color);
    grow_max = CW_MAX(grow_max, cp[ncp].grow + cp[ncp].soft);
    ncp++;
  }
  if (st && st->outline_width > 0) {
    cw_color oc = st->outline_color;
    if (oc.r == 0 && oc.g == 0 && oc.b == 0 && oc.a == 0) oc = (cw_color){ 0, 0, 0, 1 };
    cp[ncp] = (text_copy){ .grow = st->outline_width };
    r2d_pack_color(cp[ncp].c, oc);
    grow_max = CW_MAX(grow_max, cp[ncp].grow);
    ncp++;
  }
  cp[ncp] = (text_copy){ 0 };
  r2d_pack_color(cp[ncp].c, r2d_default_white(st ? st->color : (cw_color){ 0 }));
  ncp++;
  /* SDF: px at this size -> distance units (0..1 over the baked range) */
  float per_text_px = sdf ? (128.0f / (float)f->pad) / 255.0f / (L.k > 1e-4f ? L.k : 1e-4f) : 0;
  if (sdf && grow_max * per_text_px > SDF_GROW_MAX && !f->grow_warned) {
    f->grow_warned = true;
    float bake_px = grow_max / (L.k > 1e-4f ? L.k : 1e-4f);
    cw_warn("cw_r2d_text: an outline/shadow of %.1f px at size %.1f is %.1f baked px, past this font's SDF range "
            "(sdf_padding %d at bake_size %g): clamped. Load the font with sdf_padding >= %d",
            (double)grow_max, (double)L.size, (double)bake_px, f->pad, (double)f->bake_size,
            (int)ceilf(bake_px * 0.5f / SDF_GROW_MAX) + 1);
  }
  int nquads = 0;
  for (int k = 0; k < ncp; k++) {
    text_copy *c = &cp[k];
    if (sdf) {
      float g = c->grow * per_text_px, so = c->soft * per_text_px;
      if (g + so > SDF_GROW_MAX) { /* keep the copy inside the glyph's distance range */
        float sc = SDF_GROW_MAX / (g + so);
        g *= sc, so *= sc;
      }
      c->sdf_grow = (uint8_t)(g * 255.0f + 0.5f);
      c->sdf_soft = (uint8_t)(so * 255.0f + 0.5f);
      c->noff = 1;
      c->off = NULL;
    } else {
      c->off = c->grow > 0 ? disc_offsets(c->grow, &c->noff) : NULL;
      if (!c->off) c->noff = 1;
    }
    nquads += c->noff * n;
  }
  uint32_t *idx, basev;
  r2d_vertex *v = r2d_alloc(4 * nquads, 6 * nquads, &idx, &basev, t0->array, pos.y, 0);
  int q = 0;
  for (int k = 0; k < ncp; k++) {
    const text_copy *c = &cp[k];
    for (int o = 0; o < c->noff; o++) {
      float ox = c->dx + (c->off ? c->off[o].x : 0), oy = c->dy + (c->off ? c->off[o].y : 0);
      for (int i = 0; i < n; i++, q++) {
        const r2d_glyph *g = &f->glyphs.data[pl[i].gi];
        const r2d_tex *t = r2d_tex_get(g->tex);
        float u0, v0, u1, v1;
        r2d_tex_uv(t, (cw_rect){ 0, 0, (float)t->w, (float)t->h }, &u0, &v0, &u1, &v1);
        float x0 = pl[i].x + ox + g->xoff * L.k, y0 = pl[i].y + oy + g->yoff * L.k;
        float x1 = x0 + g->w * L.k, y1 = y0 + g->h * L.k;
        float xs[4] = { x0, x1, x1, x0 }, ys[4] = { y0, y0, y1, y1 }, us[4] = { u0, u1, u1, u0 }, vs[4] = { v0, v0, v1, v1 };
        for (int j = 0; j < 4; j++) {
          r2d_vertex *out = &v[4 * q + j];
          float x = xs[j], y = ys[j];
          if (R.has_xform) {
            float tx = R.xform.a * x + R.xform.c * y + R.xform.tx;
            y = R.xform.b * x + R.xform.d * y + R.xform.ty;
            x = tx;
          }
          *out = (r2d_vertex){ x, y, us[j], vs[j], { c->c[0], c->c[1], c->c[2], c->c[3] }, { (uint8_t)t->page, mode, layer, sdfw },
                               { c->sdf_grow, c->sdf_soft, 0, 0 } };
        }
        uint32_t b = basev + 4u * (uint32_t)q;
        uint32_t qi[6] = { b, b + 1, b + 2, b, b + 2, b + 3 };
        memcpy(idx + 6 * q, qi, sizeof qi);
      }
    }
  }
  return m;
}
