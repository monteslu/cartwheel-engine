/* render2d water (cw_water2d, cw_r2d_water): a spring surface simulated on
 * the CPU, and a body drawn from the screen texture: the world refracted
 * through it, mirrored on it, tinted with depth, with caustics and a lit
 * surface line. Everything is ordinary submissions (quads, triangles,
 * trails), so it batches, sorts and draws the same on GL and WebGPU. */
#include <math.h>
#include <string.h>

#include "internal.h"

/* ── the surface ───────────────────────────────────────────────────────── */

struct cw_water2d {
  int n;
  float width, spacing;
  float *h, *v, *tmp; /* height above rest (+ up), its speed, the spread scratch */
  cw_water2d_desc d;
};

static float def(float v, float d) { return v != 0 ? v : d; }

cw_water2d *cw_water2d_new(const cw_water2d_desc *desc) {
  cw_water2d *w = cw_calloc(1, sizeof *w, CW_MEM_RENDER2D);
  w->d = *desc;
  w->n = CW_CLAMP(desc->columns > 0 ? desc->columns : 64, 2, 4096);
  w->width = desc->width > 0 ? desc->width : 256;
  w->spacing = w->width / (float)(w->n - 1);
  w->h = cw_calloc((size_t)w->n * 3, sizeof(float), CW_MEM_RENDER2D);
  w->v = w->h + w->n;
  w->tmp = w->v + w->n;
  return w;
}

void cw_water2d_free(cw_water2d *w) {
  if (!w) return;
  cw_free(w->h);
  cw_free(w);
}

void cw_water2d_step(cw_water2d *w, float dt) {
  if (!w || dt <= 0) return;
  float k = def(w->d.tension, 40.0f), damp = def(w->d.damping, 2.5f), spread = def(w->d.spread, 0.25f);
  for (int i = 0; i < w->n; i++) {
    w->v[i] += (-k * w->h[i] - damp * w->v[i]) * dt;
    w->h[i] += w->v[i] * dt;
  }
  /* neighbours pull each other: the wave travels along the surface */
  for (int pass = 0; pass < 4; pass++) {
    for (int i = 0; i < w->n; i++) {
      float l = i > 0 ? w->h[i - 1] - w->h[i] : 0, r = i < w->n - 1 ? w->h[i + 1] - w->h[i] : 0;
      w->tmp[i] = spread * (l + r);
    }
    for (int i = 0; i < w->n; i++) {
      w->v[i] += w->tmp[i] * 60.0f * dt;
      w->h[i] += w->tmp[i] * 0.25f;
    }
  }
}

void cw_water2d_splash(cw_water2d *w, float x, float speed) {
  if (!w) return;
  int i = (int)lroundf(x / w->spacing);
  for (int k = -1; k <= 1; k++) {
    int j = i + k;
    if (j >= 0 && j < w->n) w->v[j] += speed * (k == 0 ? 1.0f : 0.5f);
  }
}

float cw_water2d_height(const cw_water2d *w, float x) {
  if (!w) return 0;
  float f = CW_CLAMP(x / w->spacing, 0.0f, (float)(w->n - 1));
  int i = CW_MIN((int)f, w->n - 2);
  float t = f - (float)i;
  return w->h[i] * (1 - t) + w->h[i + 1] * t;
}

int cw_water2d_columns(const cw_water2d *w, const float **heights) {
  if (heights) *heights = w ? w->h : NULL;
  return w ? w->n : 0;
}

/* ── caustics: a tileable pattern of bright filaments ──────────────────── */

static cw_texture caustic_tex;

cw_texture cw_r2d_caustics_texture(void) {
  if (!CW_HANDLE_IS_NULL(caustic_tex) && r2d_tex_get(caustic_tex)) return caustic_tex;
  enum { S = 256, C = 8 };
  /* cell points of a jittered 8x8 grid; the filaments are where the two
   * nearest points are about as near (the cells' borders), warped */
  float px[C * C], py[C * C];
  for (int j = 0; j < C; j++)
    for (int i = 0; i < C; i++) {
      uint32_t h = (uint32_t)(j * C + i) * 2654435761u;
      h ^= h >> 15;
      h *= 0x2c1b3c6du;
      h ^= h >> 12;
      px[j * C + i] = ((float)i + 0.15f + 0.7f * (float)(h & 0xffff) / 65535.0f) / C;
      py[j * C + i] = ((float)j + 0.15f + 0.7f * (float)(h >> 16) / 65535.0f) / C;
    }
  uint8_t *pix = cw_alloc(S * S * 4, CW_MEM_RENDER2D);
  for (int y = 0; y < S; y++)
    for (int x = 0; x < S; x++) {
      float u = ((float)x + 0.5f) / S, v = ((float)y + 0.5f) / S;
      u += 0.02f * sinf(v * 6.2831853f * 2), v += 0.02f * sinf(u * 6.2831853f * 3);
      float d1 = 9, d2 = 9;
      for (int k = 0; k < C * C; k++)
        for (int oy = -1; oy <= 1; oy++)
          for (int ox = -1; ox <= 1; ox++) {
            float dx = px[k] + (float)ox - u, dy = py[k] + (float)oy - v, d = dx * dx + dy * dy;
            if (d < d1) d2 = d1, d1 = d;
            else if (d < d2) d2 = d;
          }
      float e = sqrtf(d2) - sqrtf(d1); /* 0 on a border */
      float b = expf(-e * 60.0f);
      uint8_t c = (uint8_t)(CW_CLAMP(b, 0.0f, 1.0f) * 255.0f + 0.5f);
      uint8_t *p = pix + 4 * (y * S + x);
      p[0] = p[1] = p[2] = c, p[3] = c;
    }
  caustic_tex = cw_texture_from_pixels(S, S, pix, &(cw_texture_desc){ .repeat = true, .premultiplied = true });
  cw_free(pix);
  return caustic_tex;
}

/* ── the body ──────────────────────────────────────────────────────────── */

void cw_r2d_water(const cw_r2d_water_desc *d) {
  cw_rect a = d->area;
  if (a.w <= 0 || a.h <= 0) return;
  const float *hs = NULL;
  int n = d->surface ? cw_water2d_columns(d->surface, &hs) : 0;
  int cols = n >= 2 ? n : CW_CLAMP((int)(a.w / 12.0f), 8, 256);
  float t = d->time, amp = d->wave_height;
  cw_vec2 cs = cw_r2d_canvas_size();
  cw_texture scr = cw_r2d_screen_texture();
  /* the surface line: the sim's heights (+ up) and gentle waves */
  float *sy = cw_frame_alloc(sizeof(float) * (size_t)cols);
  for (int i = 0; i < cols; i++) {
    float x = a.x + a.w * (float)i / (float)(cols - 1);
    float h = hs ? hs[i] : 0;
    h += amp * (0.6f * sinf(x * 0.035f + t * 1.7f) + 0.4f * sinf(x * 0.071f - t * 2.3f));
    sy[i] = a.y - h;
  }
  cw_color tint = d->tint.a > 0 ? d->tint : cw_rgba(0.1f, 0.3f, 0.45f, 0.55f);
  float refr = d->refraction > 0 ? d->refraction : 0;
  float refl = d->reflection > 0 ? CW_MIN(d->reflection, 1.0f) : 0;
  /* columns of quads from the surface down: refraction (the world below,
   * wobbling), then the reflection (the world above, mirrored about the
   * surface, fading with depth), the tint and the caustics */
  int nv = cols * 2;
  cw_vertex2d *body = cw_frame_alloc(sizeof(cw_vertex2d) * (size_t)nv);
  cw_vertex2d *mir = cw_frame_alloc(sizeof(cw_vertex2d) * (size_t)nv);
  cw_vertex2d *tv = cw_frame_alloc(sizeof(cw_vertex2d) * (size_t)nv);
  uint32_t *ix = cw_frame_alloc(sizeof(uint32_t) * 6 * (size_t)(cols - 1));
  float bottom = a.y + a.h;
  for (int i = 0; i < cols; i++) {
    float x = a.x + a.w * (float)i / (float)(cols - 1), top = sy[i];
    /* the screen texture is in canvas pixels; a layer transform or camera
     * would need its inverse here, so water is drawn in a layer without one */
    float wob = refr * sinf(x * 0.05f + t * 2.0f);
    body[2 * i] = (cw_vertex2d){ { x, top }, { (x + wob) / cs.x, top / cs.y }, CW_WHITE };
    body[2 * i + 1] = (cw_vertex2d){ { x, bottom }, { (x - wob) / cs.x, bottom / cs.y }, CW_WHITE };
    /* mirrored about the rest surface: a point d below it shows the world d above it */
    float m0 = a.y - (top - a.y), m1 = a.y - (bottom - a.y);
    mir[2 * i] = (cw_vertex2d){ { x, top }, { (x + wob * 0.5f) / cs.x, m0 / cs.y }, cw_rgba(1, 1, 1, refl) };
    mir[2 * i + 1] = (cw_vertex2d){ { x, bottom }, { x / cs.x, m1 / cs.y }, cw_rgba(1, 1, 1, 0) };
    tv[2 * i] = (cw_vertex2d){ { x, top }, { 0, 0 }, (cw_color){ tint.r, tint.g, tint.b, tint.a * 0.6f } };
    tv[2 * i + 1] = (cw_vertex2d){ { x, bottom }, { 0, 0 }, tint };
    if (i < cols - 1) {
      uint32_t k = 2u * (uint32_t)i;
      uint32_t q[6] = { k, k + 2, k + 3, k, k + 3, k + 1 };
      memcpy(ix + 6 * i, q, sizeof q);
    }
  }
  if (!CW_HANDLE_IS_NULL(scr)) {
    cw_r2d_triangles(scr, body, nv, ix, 6 * (cols - 1));
    if (refl > 0) cw_r2d_triangles(scr, mir, nv, ix, 6 * (cols - 1));
  }
  cw_r2d_triangles((cw_texture){ 0 }, tv, nv, ix, 6 * (cols - 1));
  if (d->caustics > 0) { /* two scrolling layers of filaments, added */
    cw_texture ct = cw_r2d_caustics_texture();
    cw_blend prev = R.blend;
    cw_r2d_set_blend(CW_BLEND_ADD);
    for (int L = 0; L < 2; L++) {
      float sc = L ? 1.0f / 230.0f : 1.0f / 170.0f, ox = t * (L ? -0.021f : 0.017f), oy = t * (L ? 0.013f : -0.009f);
      cw_vertex2d *cv = cw_frame_alloc(sizeof(cw_vertex2d) * (size_t)nv);
      for (int i = 0; i < nv; i++) {
        cw_vec2 p = body[i].pos;
        float depth = CW_CLAMP((p.y - a.y) / a.h, 0.0f, 1.0f);
        cv[i] = (cw_vertex2d){ p, { p.x * sc + ox, p.y * sc + oy }, cw_rgba(0.6f, 0.85f, 1.0f, d->caustics * 0.5f * (1 - 0.7f * depth)) };
      }
      cw_r2d_triangles(ct, cv, nv, ix, 6 * (cols - 1));
    }
    cw_r2d_set_blend(prev);
  }
  if (d->surface_glow > 0) { /* the surface line, lit */
    cw_vec2 *line = cw_frame_alloc(sizeof(cw_vec2) * (size_t)cols);
    for (int i = 0; i < cols; i++) line[i] = (cw_vec2){ a.x + a.w * (float)i / (float)(cols - 1), sy[i] };
    cw_blend prev = R.blend;
    cw_r2d_set_blend(CW_BLEND_ADD);
    cw_r2d_trail(&(cw_r2d_trail_desc){ .points = line, .count = cols, .head_width = 2.5f, .tail_width = 2.5f,
                                       .head_color = cw_rgba(0.7f, 0.9f, 1.0f, d->surface_glow), .tail_color = cw_rgba(0.7f, 0.9f, 1.0f, d->surface_glow) });
    cw_r2d_set_blend(prev);
  }
}
