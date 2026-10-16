/* Mesh-mode Decal3D geometry: the triangles of a source mesh clipped to the
 * decal's box on the CPU (Sutherland-Hodgman against the box's six planes),
 * each output vertex kept as barycentric weights of its source triangle so
 * the bind-pose position and the skin joints / weights can be rebuilt. Plain
 * float arithmetic (+ - * / and sqrtf, all correctly rounded), no libm, so
 * native and wasm hosts produce the same bits. See cartwheel/decals.h. */
#include <math.h>
#include <string.h>

#include "cartwheel/math.h"
#include "fx_internal.h"

/* a polygon vertex while clipping: box-local position and the barycentric
 * weights of the source triangle's three corners */
typedef struct cv { float p[3], b[3]; } cv;

/* keep the part of poly where sign * p[axis] <= 0.5 */
static int clip_plane(const cv *in, int n, cv *out, int axis, float sign) {
  int m = 0;
  for (int i = 0; i < n; i++) {
    const cv *a = &in[i], *b = &in[(i + 1) % n];
    float da = 0.5f - sign * a->p[axis], db = 0.5f - sign * b->p[axis];
    if (da >= 0) out[m++] = *a;
    if ((da >= 0) != (db >= 0)) {
      float t = da / (da - db);
      cv *o = &out[m++];
      for (int k = 0; k < 3; k++) {
        o->p[k] = a->p[k] + (b->p[k] - a->p[k]) * t;
        o->b[k] = a->b[k] + (b->b[k] - a->b[k]) * t;
      }
      o->p[axis] = 0.5f * sign; /* exactly on the plane */
    }
  }
  return m;
}

static inline void xform(const float *m, const float *v, float *o) {
  o[0] = m[0] * v[0] + m[4] * v[1] + m[8] * v[2] + m[12];
  o[1] = m[1] * v[0] + m[5] * v[1] + m[9] * v[2] + m[13];
  o[2] = m[2] * v[0] + m[6] * v[1] + m[10] * v[2] + m[14];
}

static inline uint32_t fnv(uint32_t h, const void *p, size_t n) {
  const uint8_t *b = p;
  for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * 16777619u;
  return h;
}

static uint32_t idx_at(const cw__decal_src *s, int i) {
  if (!s->index) return (uint32_t)i;
  return s->index16 ? ((const uint16_t *)s->index)[i] : ((const uint32_t *)s->index)[i];
}

/* the four strongest joints of three corners' skins weighted by b, renormalised;
 * ties keep the lower joint index (the same choice on every host) */
static void blend_skin(const cw__decal_src *s, const uint32_t v[3], const float b[3], float *jo, float *wo) {
  float j[12], w[12];
  int n = 0;
  for (int c = 0; c < 3; c++)
    for (int k = 0; k < 4; k++) {
      float jj = s->joints[4 * v[c] + k], ww = s->weights[4 * v[c] + k] * b[c];
      if (ww <= 0) continue;
      int f = 0;
      while (f < n && j[f] != jj) f++;
      if (f == n) { j[n] = jj; w[n] = 0; n++; }
      w[f] += ww;
    }
  float sum = 0;
  for (int k = 0; k < 4; k++) {
    int best = -1;
    for (int i = 0; i < n; i++) {
      if (w[i] < 0) continue;
      if (best < 0 || w[i] > w[best] || (w[i] == w[best] && j[i] < j[best])) best = i;
    }
    if (best < 0) { jo[k] = 0; wo[k] = 0; continue; }
    jo[k] = j[best];
    wo[k] = w[best];
    sum += w[best];
    w[best] = -1;
  }
  if (sum > 0) for (int k = 0; k < 4; k++) wo[k] = wo[k] / sum;
  else { jo[0] = s->joints[4 * v[0]]; wo[0] = 1; }
}

int cw__decal_clip(const cw__decal_src *s, const cw__decal_clip_params *p, cw__decal_out *o) {
  int tris_in = s->index ? s->index_count / 3 : s->vertex_count / 3;
  if (tris_in <= 0) return 0;
  /* every vertex's posed position in box space, and its outcode */
  float *lp = cw_alloc(sizeof(float) * 3 * (size_t)s->vertex_count, CW_MEM_RENDER);
  uint8_t *code = cw_alloc((size_t)s->vertex_count, CW_MEM_RENDER);
  for (int i = 0; i < s->vertex_count; i++) {
    float *l = lp + 3 * i;
    xform(p->world_to_box, s->posed + 3 * i, l);
    uint8_t c = 0;
    for (int k = 0; k < 3; k++) {
      if (l[k] > 0.5f) c |= (uint8_t)(1u << (2 * k));
      if (l[k] < -0.5f) c |= (uint8_t)(2u << (2 * k));
    }
    code[i] = c;
  }
  const bool skin = s->joints && s->weights;
  int added = 0;
  for (int t = 0; t < tris_in; t++) {
    uint32_t v[3] = { idx_at(s, 3 * t), idx_at(s, 3 * t + 1), idx_at(s, 3 * t + 2) };
    if ((int)v[0] >= s->vertex_count || (int)v[1] >= s->vertex_count || (int)v[2] >= s->vertex_count) continue;
    if (code[v[0]] & code[v[1]] & code[v[2]]) continue; /* all outside one plane */
    /* the posed face against the projection axis: back faces never receive */
    const float *w0 = s->posed + 3 * v[0], *w1 = s->posed + 3 * v[1], *w2 = s->posed + 3 * v[2];
    float e1[3] = { w1[0] - w0[0], w1[1] - w0[1], w1[2] - w0[2] }, e2[3] = { w2[0] - w0[0], w2[1] - w0[1], w2[2] - w0[2] };
    float fn[3] = { e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0] };
    float fl = sqrtf(fn[0] * fn[0] + fn[1] * fn[1] + fn[2] * fn[2]);
    if (!(fl > 0)) continue;
    if ((fn[0] * p->axis[0] + fn[1] * p->axis[1] + fn[2] * p->axis[2]) / fl <= p->min_facing) continue;
    cv a[9], b[9];
    for (int c = 0; c < 3; c++) {
      memcpy(a[c].p, lp + 3 * v[c], sizeof a[c].p);
      a[c].b[0] = c == 0;
      a[c].b[1] = c == 1;
      a[c].b[2] = c == 2;
    }
    int n = 3;
    n = clip_plane(a, n, b, 0, 1);
    if (n >= 3) n = clip_plane(b, n, a, 0, -1);
    if (n >= 3) n = clip_plane(a, n, b, 1, 1);
    if (n >= 3) n = clip_plane(b, n, a, 1, -1);
    if (n >= 3) n = clip_plane(a, n, b, 2, 1);
    if (n >= 3) n = clip_plane(b, n, a, 2, -1);
    if (n < 3) continue;
    if (o->tris + n - 2 > p->max_tris) {
      o->truncated = true;
      break;
    }
    /* the geometry-space face normal (no normals: the offset direction) */
    const float *g0 = s->pos + 3 * v[0], *g1 = s->pos + 3 * v[1], *g2 = s->pos + 3 * v[2];
    float ge1[3] = { g1[0] - g0[0], g1[1] - g0[1], g1[2] - g0[2] }, ge2[3] = { g2[0] - g0[0], g2[1] - g0[1], g2[2] - g0[2] };
    float gfn[3] = { ge1[1] * ge2[2] - ge1[2] * ge2[1], ge1[2] * ge2[0] - ge1[0] * ge2[2], ge1[0] * ge2[1] - ge1[1] * ge2[0] };
    /* the polygon's vertices, then a fan */
    int base = o->pos.count / 3;
    for (int i = 0; i < n; i++) {
      const cv *c = &a[i];
      float gp[3] = { 0, 0, 0 }, gn[3] = { 0, 0, 0 }, wn[3] = { 0, 0, 0 };
      for (int k = 0; k < 3; k++) {
        const float *q = s->pos + 3 * v[k];
        gp[0] += q[0] * c->b[k];
        gp[1] += q[1] * c->b[k];
        gp[2] += q[2] * c->b[k];
        if (s->nrm) {
          const float *nn = s->nrm + 3 * v[k];
          gn[0] += nn[0] * c->b[k];
          gn[1] += nn[1] * c->b[k];
          gn[2] += nn[2] * c->b[k];
        }
        if (s->posed_nrm) {
          const float *nn = s->posed_nrm + 3 * v[k];
          wn[0] += nn[0] * c->b[k];
          wn[1] += nn[1] * c->b[k];
          wn[2] += nn[2] * c->b[k];
        }
      }
      if (!s->nrm) memcpy(gn, gfn, sizeof gn);
      if (!s->posed_nrm) memcpy(wn, fn, sizeof wn);
      float gl = sqrtf(gn[0] * gn[0] + gn[1] * gn[1] + gn[2] * gn[2]);
      float un[3] = { 0, 0, 1 };
      if (gl > 0) {
        for (int k = 0; k < 3; k++) un[k] = gn[k] / gl;
        /* offset metres in world: the normal's length through the mesh's scale */
        const float *m = p->geo_to_world;
        float wx = m[0] * un[0] + m[3] * un[1] + m[6] * un[2], wy = m[1] * un[0] + m[4] * un[1] + m[7] * un[2],
              wz = m[2] * un[0] + m[5] * un[1] + m[8] * un[2];
        float wl2 = sqrtf(wx * wx + wy * wy + wz * wz);
        float k = wl2 > 0 ? p->offset / wl2 : 0;
        for (int q = 0; q < 3; q++) gp[q] += un[q] * k;
      }
      /* fades: the surface turning away from the axis, the box's near and far ends */
      float wl = sqrtf(wn[0] * wn[0] + wn[1] * wn[1] + wn[2] * wn[2]);
      float facing = wl > 0 ? (wn[0] * p->axis[0] + wn[1] * p->axis[1] + wn[2] * p->axis[2]) / wl : 1;
      float nf = p->normal_fade, fade;
      if (facing <= 0) fade = 0;
      else fade = cw_clampf((facing - nf) / (1 - nf > 1e-4f ? 1 - nf : 1e-4f), 0, 1);
      if (p->edge_fade > 0) fade *= cw_clampf((0.5f - fabsf(c->p[2])) / p->edge_fade, 0, 1);
      float u = p->uv_rect[0] + (c->p[0] + 0.5f) * (p->uv_rect[2] - p->uv_rect[0]);
      float vv = p->uv_rect[1] + (c->p[1] + 0.5f) * (p->uv_rect[3] - p->uv_rect[1]);
      for (int k = 0; k < 3; k++) cw_array_push(&o->pos, gp[k]);
      for (int k = 0; k < 3; k++) cw_array_push(&o->nrm, un[k]);
      cw_array_push(&o->uv, u);
      cw_array_push(&o->uv, vv);
      cw_array_push(&o->fade, fade);
      if (skin) {
        float jo[4], wo[4];
        blend_skin(s, v, c->b, jo, wo);
        for (int k = 0; k < 4; k++) {
          cw_array_push(&o->joints, jo[k]);
          cw_array_push(&o->weights, wo[k]);
        }
      }
    }
    for (int i = 1; i + 1 < n; i++) {
      cw_array_push(&o->index, (uint32_t)base);
      cw_array_push(&o->index, (uint32_t)(base + i));
      cw_array_push(&o->index, (uint32_t)(base + i + 1));
    }
    o->tris += n - 2;
    added += n - 2;
  }
  cw_free(lp);
  cw_free(code);
  return added;
}

uint32_t cw__decal_out_hash(const cw__decal_out *o, uint32_t h) {
  if (!h) h = 2166136261u;
  h = fnv(h, &o->tris, sizeof o->tris);
  h = fnv(h, o->pos.data, sizeof(float) * (size_t)o->pos.count);
  h = fnv(h, o->nrm.data, sizeof(float) * (size_t)o->nrm.count);
  h = fnv(h, o->uv.data, sizeof(float) * (size_t)o->uv.count);
  h = fnv(h, o->fade.data, sizeof(float) * (size_t)o->fade.count);
  h = fnv(h, o->joints.data, sizeof(float) * (size_t)o->joints.count);
  h = fnv(h, o->weights.data, sizeof(float) * (size_t)o->weights.count);
  h = fnv(h, o->index.data, sizeof(uint32_t) * (size_t)o->index.count);
  return h;
}

void cw__decal_out_free(cw__decal_out *o) {
  cw_array_free(&o->pos);
  cw_array_free(&o->nrm);
  cw_array_free(&o->uv);
  cw_array_free(&o->fade);
  cw_array_free(&o->joints);
  cw_array_free(&o->weights);
  cw_array_free(&o->index);
  o->tris = 0;
  o->truncated = false;
}
