/* Font subsetting for packs (font.subset): keep the glyphs a game's text can
 * reach and empty the rest, without renumbering glyphs, so GSUB/GPOS/GDEF
 * stay valid as they are.
 *
 * Kept: .notdef, the glyphs the cmap maps the wanted codepoints to, the
 * components of kept composite glyphs, and the closure of GSUB single,
 * multiple, alternate and ligature substitutions (through extension
 * lookups) over the kept set: an over-approximation that keeps every form a
 * shaper can produce (Arabic joining forms, ligatures, localized forms).
 * Contextual lookups only point at those, so they need no closure of their
 * own. Outlines of every other glyph are emptied (loca rewritten, long
 * format), the cmap is rebuilt (format 4 + format 12) with only the wanted
 * codepoints, and variation tables are dropped (the default instance is
 * what stb_truetype and kb_text_shape draw). TrueType outlines only: a CFF
 * font (OTTO) is refused with a message. */
#include <stdlib.h>
#include <string.h>

#include "cwpack.h"

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static void wr16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 8), p[1] = (uint8_t)v; }
static void wr32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24), p[1] = (uint8_t)(v >> 16), p[2] = (uint8_t)(v >> 8), p[3] = (uint8_t)v; }

typedef struct { uint32_t tag; const uint8_t *data; uint32_t len; } table;
typedef struct {
  const uint8_t *src;
  size_t len;
  table t[64];
  int nt;
  int nglyphs;
} font;

static uint32_t tagv(const char *s) { return (uint32_t)s[0] << 24 | (uint32_t)s[1] << 16 | (uint32_t)s[2] << 8 | (uint32_t)s[3]; }

static const table *tbl(const font *f, const char *tag) {
  uint32_t t = tagv(tag);
  for (int i = 0; i < f->nt; i++)
    if (f->t[i].tag == t) return &f->t[i];
  return NULL;
}

/* ── cmap ───────────────────────────────────────────────────────────── */

static int cmap_lookup(const uint8_t *sub, uint32_t len, uint32_t cp) {
  uint16_t fmt = rd16(sub);
  if (fmt == 4) {
    if (cp > 0xffff) return 0;
    int segs = rd16(sub + 6) / 2;
    const uint8_t *ends = sub + 14, *starts = ends + 2 * segs + 2, *deltas = starts + 2 * segs, *ranges = deltas + 2 * segs;
    for (int s = 0; s < segs; s++) {
      if (cp > rd16(ends + 2 * s)) continue;
      uint16_t st = rd16(starts + 2 * s);
      if (cp < st) return 0;
      uint16_t ro = rd16(ranges + 2 * s);
      if (!ro) return (int)((cp + rd16(deltas + 2 * s)) & 0xffff);
      const uint8_t *g = ranges + 2 * s + ro + 2 * (cp - st);
      if (g + 2 > sub + len) return 0;
      uint16_t gid = rd16(g);
      return gid ? (int)((gid + rd16(deltas + 2 * s)) & 0xffff) : 0;
    }
    return 0;
  }
  if (fmt == 12) {
    uint32_t n = rd32(sub + 12);
    for (uint32_t i = 0; i < n; i++) {
      const uint8_t *g = sub + 16 + 12 * i;
      if (cp >= rd32(g) && cp <= rd32(g + 4)) return (int)(rd32(g + 8) + (cp - rd32(g)));
    }
    return 0;
  }
  return 0;
}

static const uint8_t *cmap_best(const font *f, uint32_t *sublen) {
  const table *c = tbl(f, "cmap");
  if (!c) return NULL;
  int n = rd16(c->data + 2);
  const uint8_t *best = NULL;
  int score = 0;
  for (int i = 0; i < n; i++) {
    const uint8_t *r = c->data + 4 + 8 * i;
    uint16_t pid = rd16(r), eid = rd16(r + 2);
    const uint8_t *sub = c->data + rd32(r + 4);
    uint16_t fmt = rd16(sub);
    int s = (pid == 3 && eid == 10 && fmt == 12) ? 4 : (pid == 0 && fmt == 12) ? 3 : (pid == 3 && eid == 1 && fmt == 4) ? 2 : (pid == 0 && fmt == 4) ? 1 : 0;
    if (s > score) score = s, best = sub;
  }
  if (best) *sublen = (uint32_t)(c->data + c->len - best);
  return best;
}

/* ── coverage tables ────────────────────────────────────────────────── */

/* calls fn(gid, coverage index) for every covered glyph */
typedef void (*cov_fn)(int gid, int index, void *u);
static void coverage_each(const uint8_t *cov, cov_fn fn, void *u) {
  uint16_t fmt = rd16(cov), n = rd16(cov + 2);
  if (fmt == 1)
    for (int i = 0; i < n; i++) fn(rd16(cov + 4 + 2 * i), i, u);
  else if (fmt == 2)
    for (int r = 0; r < n; r++) {
      const uint8_t *rr = cov + 4 + 6 * r;
      int a = rd16(rr), b = rd16(rr + 2), si = rd16(rr + 4);
      for (int g = a; g <= b; g++) fn(g, si + g - a, u);
    }
}

typedef struct { uint8_t *keep; int nglyphs; const uint8_t *st; int type; bool changed; } closure;

static void add_glyph(closure *c, int g) {
  if (g >= 0 && g < c->nglyphs && !c->keep[g]) c->keep[g] = 1, c->changed = true;
}

static void subst_glyph(int gid, int ci, void *u) {
  closure *c = u;
  if (gid >= c->nglyphs || !c->keep[gid]) return;
  const uint8_t *st = c->st;
  uint16_t fmt = rd16(st);
  switch (c->type) {
  case 1:
    if (fmt == 1) add_glyph(c, (gid + (int16_t)rd16(st + 4)) & 0xffff);
    else if (fmt == 2 && ci < rd16(st + 4)) add_glyph(c, rd16(st + 6 + 2 * ci));
    break;
  case 2: case 3: { /* sequences / alternate sets: arrays of glyphs */
    if (ci >= rd16(st + 4)) break;
    const uint8_t *set = st + rd16(st + 6 + 2 * ci);
    for (int k = 0, n = rd16(set); k < n; k++) add_glyph(c, rd16(set + 2 + 2 * k));
    break;
  }
  case 4: {
    if (ci >= rd16(st + 4)) break;
    const uint8_t *set = st + rd16(st + 6 + 2 * ci);
    for (int k = 0, n = rd16(set); k < n; k++) {
      const uint8_t *lig = set + rd16(set + 2 + 2 * k);
      int comps = rd16(lig + 2);
      bool all = true;
      for (int q = 0; q + 1 < comps; q++) {
        int g = rd16(lig + 4 + 2 * q);
        all &= g < c->nglyphs && c->keep[g];
      }
      if (all) add_glyph(c, rd16(lig));
    }
    break;
  }
  default: break;
  }
}

static void gsub_closure(const font *f, uint8_t *keep) {
  const table *g = tbl(f, "GSUB");
  if (!g || g->len < 10) return;
  const uint8_t *ll = g->data + rd16(g->data + 8);
  int nl = rd16(ll);
  closure c = { keep, f->nglyphs, NULL, 0, true };
  for (int pass = 0; pass < 16 && c.changed; pass++) {
    c.changed = false;
    for (int l = 0; l < nl; l++) {
      const uint8_t *lk = ll + rd16(ll + 2 + 2 * l);
      int type = rd16(lk), ns = rd16(lk + 4);
      for (int s = 0; s < ns; s++) {
        const uint8_t *st = lk + rd16(lk + 6 + 2 * s);
        int t = type;
        if (t == 7) {
          t = rd16(st + 2);
          st = st + rd32(st + 4);
        }
        if (t < 1 || t > 4) continue;
        c.st = st;
        c.type = t;
        coverage_each(st + rd16(st + 2), subst_glyph, &c);
      }
    }
  }
}

/* ── glyf ───────────────────────────────────────────────────────────── */

static bool glyph_range(const font *f, int g, uint32_t *off, uint32_t *len) {
  const table *loca = tbl(f, "loca"), *head = tbl(f, "head");
  int fmt = (int16_t)rd16(head->data + 50);
  uint32_t a, b;
  if (fmt == 0) a = 2u * rd16(loca->data + 2 * g), b = 2u * rd16(loca->data + 2 * g + 2);
  else a = rd32(loca->data + 4 * g), b = rd32(loca->data + 4 * g + 4);
  *off = a;
  *len = b > a ? b - a : 0;
  return true;
}

static void composite_closure(const font *f, uint8_t *keep) {
  const table *glyf = tbl(f, "glyf");
  bool changed = true;
  for (int pass = 0; pass < 8 && changed; pass++) {
    changed = false;
    for (int g = 0; g < f->nglyphs; g++) {
      if (!keep[g]) continue;
      uint32_t off, len;
      glyph_range(f, g, &off, &len);
      if (len < 10 || off + len > glyf->len) continue;
      const uint8_t *p = glyf->data + off;
      if ((int16_t)rd16(p) >= 0) continue; /* simple glyph */
      p += 10;
      for (;;) {
        uint16_t flags = rd16(p), gi = rd16(p + 2);
        if (gi < f->nglyphs && !keep[gi]) keep[gi] = 1, changed = true;
        p += 4 + ((flags & 1) ? 4 : 2);
        if (flags & 8) p += 2;
        else if (flags & 0x40) p += 4;
        else if (flags & 0x80) p += 8;
        if (!(flags & 0x20)) break;
      }
    }
  }
}

/* ── cmap writer ────────────────────────────────────────────────────── */

typedef struct { uint32_t cp; uint16_t gid; } cmapping;
static int cmp_map(const void *a, const void *b) {
  uint32_t x = ((const cmapping *)a)->cp, y = ((const cmapping *)b)->cp;
  return x < y ? -1 : x > y;
}

static uint8_t *build_cmap(const cmapping *m, int n, uint32_t *out_len) {
  /* format 4: one segment per run of consecutive codepoints AND glyph ids */
  int segs = 0;
  uint32_t *s_start = cw_alloc(sizeof(uint32_t) * (size_t)(n + 2), CW_MEM_GENERAL), *s_end = cw_alloc(sizeof(uint32_t) * (size_t)(n + 2), CW_MEM_GENERAL);
  int *s_delta = cw_alloc(sizeof(int) * (size_t)(n + 2), CW_MEM_GENERAL);
  for (int i = 0; i < n && m[i].cp <= 0xfffe; i++) {
    if (segs && m[i].cp == s_end[segs - 1] + 1 && (int)m[i].gid - (int)m[i].cp == s_delta[segs - 1]) s_end[segs - 1] = m[i].cp;
    else s_start[segs] = s_end[segs] = m[i].cp, s_delta[segs++] = (int)m[i].gid - (int)m[i].cp;
  }
  s_start[segs] = s_end[segs] = 0xffff, s_delta[segs++] = 1;
  uint32_t f4 = 16 + 8 * (uint32_t)segs;
  /* format 12: groups of consecutive codepoints and glyph ids */
  int groups = 0;
  for (int i = 0; i < n; i++)
    if (!i || m[i].cp != m[i - 1].cp + 1 || m[i].gid != m[i - 1].gid + 1) groups++;
  uint32_t f12 = 16 + 12 * (uint32_t)groups;
  uint32_t total = 4 + 8 * 2 + f4 + f12;
  uint8_t *o = cw_calloc(1, total, CW_MEM_GENERAL);
  wr16(o, 0), wr16(o + 2, 2);
  wr16(o + 4, 3), wr16(o + 6, 1), wr32(o + 8, 20);
  wr16(o + 12, 3), wr16(o + 14, 10), wr32(o + 16, 20 + f4);
  uint8_t *p = o + 20;
  wr16(p, 4), wr16(p + 2, f4), wr16(p + 4, 0), wr16(p + 6, (uint32_t)segs * 2);
  int es = 0;
  while ((2 << es) <= segs) es++;
  wr16(p + 8, (uint32_t)(1 << es) * 2), wr16(p + 10, (uint32_t)es), wr16(p + 12, (uint32_t)segs * 2 - (uint32_t)(1 << es) * 2);
  uint8_t *ends = p + 14, *starts = ends + 2 * segs + 2, *deltas = starts + 2 * segs, *ranges = deltas + 2 * segs;
  for (int s = 0; s < segs; s++) {
    wr16(ends + 2 * s, s_end[s]);
    wr16(starts + 2 * s, s_start[s]);
    wr16(deltas + 2 * s, (uint32_t)s_delta[s] & 0xffff);
    wr16(ranges + 2 * s, 0);
  }
  p = o + 20 + f4;
  wr16(p, 12), wr16(p + 2, 0), wr32(p + 4, f12), wr32(p + 8, 0), wr32(p + 12, (uint32_t)groups);
  uint8_t *g = p + 16;
  for (int i = 0; i < n; i++) {
    if (!i || m[i].cp != m[i - 1].cp + 1 || m[i].gid != m[i - 1].gid + 1) {
      wr32(g, m[i].cp), wr32(g + 4, m[i].cp), wr32(g + 8, m[i].gid);
      g += 12;
    } else wr32(g - 12 + 4, m[i].cp);
  }
  cw_free(s_start);
  cw_free(s_end);
  cw_free(s_delta);
  *out_len = total;
  return o;
}

/* ── the subset ─────────────────────────────────────────────────────── */

static uint32_t checksum(const uint8_t *d, uint32_t len) {
  uint32_t s = 0;
  for (uint32_t i = 0; i < len; i += 4) {
    uint8_t b[4] = { 0 };
    memcpy(b, d + i, len - i < 4 ? len - i : 4);
    s += rd32(b);
  }
  return s;
}

static int cmp_tag(const void *a, const void *b) {
  uint32_t x = ((const table *)a)->tag, y = ((const table *)b)->tag;
  return x < y ? -1 : x > y;
}

uint8_t *cwp_subset_font(const char *name, const uint8_t *src, size_t len, const uint32_t *cps, int ncp, size_t *out_len,
                         char *note, size_t note_cap) {
  font f = { src, len, { { 0 } }, 0, 0 };
  if (len < 12) goto bad;
  uint32_t ver = rd32(src);
  if (ver == tagv("OTTO")) {
    fprintf(stderr, "cwpack: res://%s: font.subset needs TrueType outlines (glyf); this is a CFF font: "
                    "subset it elsewhere or drop font.subset\n", name);
    return NULL;
  }
  if (ver != 0x00010000 && ver != tagv("true")) goto bad;
  f.nt = rd16(src + 4);
  if (f.nt > 64 || 12 + 16u * (uint32_t)f.nt > len) goto bad;
  for (int i = 0; i < f.nt; i++) {
    const uint8_t *r = src + 12 + 16 * i;
    uint32_t off = rd32(r + 8), l = rd32(r + 12);
    if ((uint64_t)off + l > len) goto bad;
    f.t[i] = (table){ rd32(r), src + off, l };
  }
  const table *maxp = tbl(&f, "maxp"), *head = tbl(&f, "head");
  if (!maxp || !head || !tbl(&f, "glyf") || !tbl(&f, "loca") || !tbl(&f, "cmap")) goto bad;
  f.nglyphs = rd16(maxp->data + 4);
  uint8_t *keep = cw_calloc((size_t)f.nglyphs + 1, 1, CW_MEM_GENERAL);
  keep[0] = 1;
  uint32_t sublen = 0;
  const uint8_t *cm = cmap_best(&f, &sublen);
  if (!cm) {
    cw_free(keep);
    goto bad;
  }
  cmapping *map = cw_alloc(sizeof *map * (size_t)(ncp + 1), CW_MEM_GENERAL);
  int nm = 0;
  for (int i = 0; i < ncp; i++) {
    int gid = cmap_lookup(cm, sublen, cps[i]);
    if (gid > 0 && gid < f.nglyphs) {
      keep[gid] = 1;
      map[nm++] = (cmapping){ cps[i], (uint16_t)gid };
    }
  }
  qsort(map, (size_t)nm, sizeof *map, cmp_map);
  int nu = 0; /* one mapping per codepoint */
  for (int i = 0; i < nm; i++)
    if (!nu || map[nu - 1].cp != map[i].cp) map[nu++] = map[i];
  nm = nu;
  gsub_closure(&f, keep);
  composite_closure(&f, keep);
  int kept = 0;
  for (int g = 0; g < f.nglyphs; g++) kept += keep[g];
  /* glyf + loca (long) */
  const table *glyf = tbl(&f, "glyf");
  uint32_t glen = 0;
  for (int g = 0; g < f.nglyphs; g++) {
    uint32_t off, l;
    glyph_range(&f, g, &off, &l);
    if (keep[g]) glen += (l + 3) & ~3u;
  }
  uint8_t *nglyf = cw_calloc(1, glen + 4, CW_MEM_GENERAL), *nloca = cw_alloc(4 * ((size_t)f.nglyphs + 1), CW_MEM_GENERAL);
  uint32_t at = 0;
  for (int g = 0; g < f.nglyphs; g++) {
    wr32(nloca + 4 * g, at);
    uint32_t off, l;
    glyph_range(&f, g, &off, &l);
    if (!keep[g] || off + l > glyf->len) continue;
    memcpy(nglyf + at, glyf->data + off, l);
    at += (l + 3) & ~3u;
  }
  wr32(nloca + 4 * f.nglyphs, at);
  uint32_t cmap_len;
  uint8_t *ncmap = build_cmap(map, nm, &cmap_len);
  uint8_t *nhead = cw_alloc(head->len, CW_MEM_GENERAL);
  memcpy(nhead, head->data, head->len);
  wr16(nhead + 50, 1);      /* indexToLocFormat: long */
  wr32(nhead + 8, 0);       /* checkSumAdjustment, set below */
  /* the table set */
  static const char *const drop[] = { "fvar", "gvar", "avar", "cvar", "HVAR", "VVAR", "MVAR", "STAT", "DSIG", "hdmx",
                                      "LTSH", "VDMX" };
  table out[64];
  int no = 0;
  for (int i = 0; i < f.nt; i++) {
    bool dropped = false;
    for (size_t k = 0; k < CW_ARRAY_LEN(drop); k++) dropped |= f.t[i].tag == tagv(drop[k]);
    if (dropped) continue;
    table t = f.t[i];
    if (t.tag == tagv("glyf")) t.data = nglyf, t.len = at;
    else if (t.tag == tagv("loca")) t.data = nloca, t.len = 4 * ((uint32_t)f.nglyphs + 1);
    else if (t.tag == tagv("cmap")) t.data = ncmap, t.len = cmap_len;
    else if (t.tag == tagv("head")) t.data = nhead;
    out[no++] = t;
  }
  qsort(out, (size_t)no, sizeof *out, cmp_tag);
  size_t total = 12 + 16 * (size_t)no;
  for (int i = 0; i < no; i++) total += ((size_t)out[i].len + 3) & ~(size_t)3;
  uint8_t *o = cw_calloc(1, total, CW_MEM_GENERAL);
  wr32(o, 0x00010000);
  wr16(o + 4, (uint32_t)no);
  int es = 0;
  while ((2 << es) <= no) es++;
  wr16(o + 6, (uint32_t)(16 << es)), wr16(o + 8, (uint32_t)es), wr16(o + 10, (uint32_t)(no * 16 - (16 << es)));
  size_t pos = 12 + 16 * (size_t)no, head_at = 0;
  for (int i = 0; i < no; i++) {
    uint8_t *r = o + 12 + 16 * i;
    memcpy(o + pos, out[i].data, out[i].len);
    wr32(r, out[i].tag);
    wr32(r + 4, checksum(o + pos, out[i].len));
    wr32(r + 8, (uint32_t)pos);
    wr32(r + 12, out[i].len);
    if (out[i].tag == tagv("head")) head_at = pos;
    pos += ((size_t)out[i].len + 3) & ~(size_t)3;
  }
  if (head_at) wr32(o + head_at + 8, 0xB1B0AFBAu - checksum(o, (uint32_t)total));
  if (note) snprintf(note, note_cap, "subset: %d codepoints, %d of %d glyphs kept (GSUB closure, composites)", nm, kept, f.nglyphs);
  cw_free(keep);
  cw_free(map);
  cw_free(nglyf);
  cw_free(nloca);
  cw_free(ncmap);
  cw_free(nhead);
  *out_len = total;
  return o;
bad:
  fprintf(stderr, "cwpack: res://%s: not a TrueType font font.subset can read\n", name);
  return NULL;
}
