/* ReflectionProbe3D (cartwheel/gi.h): live captures of the scene into
 * PMREMs, and the "gi" shading part's fill hook that binds up to two of them
 * per draw (src/gi/extensions/gi_shading.mjs). */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "cartwheel/gi.h"
#include "cartwheel/gpu.h"
#include "cartwheel/pipeline.h"
#include "cartwheel/quality.h"
#include "cartwheel/render3d.h"
#include "cartwheel/shading.h"
#include "gi_internal.h"
#include "render/pipeline_internal.h"

static const char *const update_names[] = { "ONCE", "ON_DEMAND", "EVERY_N_FRAMES", NULL };

static const cw_node_type *probe_t;

/* ── per tree ─────────────────────────────────────────────────────────── */

static void gtree_free(void *p) {
  gi_tree *g = p;
  for (int i = 0; i < g->mats.count; i++) {
    cw_shading_remove(g->mats.data[i], "gi");
    t3_release(g->mats.data[i]);
  }
  cw_array_free(&g->mats);
  cw_array_free(&g->probes);
  cw_array_free(&g->grids);
  cw__gi_grid_tree_free(g);
  cw__rt_destroy(&g->gbuf);
  cw__rt_destroy(&g->albedo);
  if (g->dummy) t3_release(g->dummy);
  if (g->pmrem) t3_pmrem_generator_destroy(g->pmrem);
  cw_free(g);
}

gi_tree *cw__gi_tree(cw_tree *t) {
  gi_tree *g = cw_tree_slot(t, "gi");
  if (!g) {
    g = cw_calloc(1, sizeof *g, CW_MEM_RENDER);
    cw_tree_set_slot(t, "gi", g, gtree_free);
  }
  return g;
}

void cw_gi_get_stats(cw_tree *t, cw_gi_stats *out) {
  gi_tree *g = t ? cw_tree_slot(t, "gi") : NULL;
  memset(out, 0, sizeof *out);
  if (!g) return;
  *out = g->stats;
  out->reflection_probes = g->probes.count;
  out->materials = g->mats.count;
}

/* ── quality ──────────────────────────────────────────────────────────── */

static int effects(void) { return cw_gfx_get(CW_GFX_EFFECTS); }

static int probe_size(const refl_probe *p) {
  int r = p->resolution, e = effects();
  if (e <= 1) r /= 2;
  else if (e >= 3) r *= 2;
  int lo = e <= 1 ? 32 : 16, hi = e >= 3 ? 512 : 1024;
  if (r < lo) r = lo;
  if (r > hi) r = hi;
  int p2 = 16;
  while (p2 * 2 <= r) p2 *= 2;
  return p2;
}
static int every_scale(void) { int e = effects(); return e == 0 ? 4 : e == 1 ? 2 : 1; }
static int capture_budget(void) { int e = effects(); return e <= 1 ? 1 : e == 2 ? 2 : 4; }

/* ── the probe's box ─────────────────────────────────────────────────── */

static void probe_box(cw_node n, const refl_probe *p, float c[3], float h[3], float o[3]) {
  const t3_mat4 *m = cw_node3d_global_transform(n);
  float s[3] = { 1, 1, 1 }, pos[3] = { 0, 0, 0 };
  if (m) {
    for (int k = 0; k < 3; k++) {
      const float *col = &m->e[k * 4];
      s[k] = sqrtf(col[0] * col[0] + col[1] * col[1] + col[2] * col[2]);
    }
    pos[0] = m->e[12], pos[1] = m->e[13], pos[2] = m->e[14];
  }
  float off[3] = { p->origin_offset.x, p->origin_offset.y, p->origin_offset.z };
  for (int k = 0; k < 3; k++) {
    c[k] = pos[k];
    h[k] = 0.5f * fabsf((&p->size.x)[k]) * s[k];
    o[k] = pos[k] + off[k] * s[k];
  }
}

/* ── capture ─────────────────────────────────────────────────────────── */

/* a probe's cull_mask for one capture: what it leaves out is hidden, and what
 * it keeps is put on layer 1 too (the capture's camera draws layer 1, as
 * three.js's CubeCamera does), so a stand-in on a layer the view camera skips
 * is captured. Restored after. */
typedef struct masked { t3_object *o; uint32_t layers; bool visible; } masked;
typedef CW_ARRAY(masked) mask_list;
static void mask_walk(t3_object *o, uint32_t mask, mask_list *changed) {
  bool drawable = o->type == T3_MESH || o->type == T3_SKINNED_MESH || o->type == T3_INSTANCED_MESH || o->type == T3_SPRITE ||
                  o->type == T3_POINTS || o->type == T3_LINE || o->type == T3_LINE_SEGMENTS || o->type == T3_LINE_LOOP;
  if (drawable && o->visible) {
    if ((o->layers & mask) == 0) {
      cw_array_push(changed, (masked){ o, o->layers, true });
      o->visible = false;
      return;
    }
    if (!(o->layers & 1u)) {
      cw_array_push(changed, (masked){ o, o->layers, true });
      o->layers |= 1u;
    }
  }
  for (int i = 0; i < o->child_count; i++) mask_walk(o->children[i], mask, changed);
}

static bool capture(gi_tree *g, cw_pass_ctx *ctx, cw_node n, refl_probe *p) {
  t3_renderer *r = ctx->renderer;
  float c[3], h[3], o[3];
  probe_box(n, p, c, h, o);
  bool wgpu = cw_gpu_backend() == CW_GPU_WEBGPU;
  /* three.c draws the capture on command buffers of its own (WebGPU) or with
   * its own GL state: forget the cached state either way */
  if (wgpu) t3_renderer_set_external_encoder(r, 0);
  else t3_renderer_reset_bindings(r);
  if (!g->pmrem) g->pmrem = t3_pmrem_generator_new(r);
  mask_list changed = { 0 };
  if (p->cull_mask != -1) mask_walk(&ctx->scene3d->base, (uint32_t)p->cull_mask, &changed);
  int size = probe_size(p);
  int back = p->front ^ 1;
  /* drawn into the back target while the scene samples the front one */
  t3_render_target *next = t3_pmrem_from_scene_ex(g->pmrem, ctx->scene3d, 0, p->near, p->far,
                                                  &(t3_pmrem_scene_options){ .size = size, .position = { o[0], o[1], o[2] },
                                                                             .render_target = p->rt[back] });
  for (int i = 0; i < changed.count; i++) changed.data[i].o->layers = changed.data[i].layers, changed.data[i].o->visible = changed.data[i].visible;
  cw_array_free(&changed);
  if (!wgpu) t3_renderer_reset_bindings(r);
  if (!next) return false;
  if (p->rt[back]) t3_release(p->rt[back]);
  p->rt[back] = next;
  p->front = back;
  p->size_now = size;
  p->captures++;
  return true;
}

typedef struct due { cw_node n; refl_probe *p; int overdue; float dist2; } due;

static int due_cmp(const void *a, const void *b) {
  const due *x = a, *y = b;
  if (x->overdue != y->overdue) return y->overdue - x->overdue;
  if (x->dist2 != y->dist2) return x->dist2 < y->dist2 ? -1 : 1;
  return x->n.id < y->n.id ? -1 : x->n.id > y->n.id;
}

/* every Standard / Physical material of the scene carries the gi part */
void cw__gi_tag(gi_tree *g, t3_object *o) {
  if (o->type == T3_MESH || o->type == T3_SKINNED_MESH || o->type == T3_INSTANCED_MESH) {
    t3_mesh *m = (t3_mesh *)o;
    for (int i = 0; i < m->material_count; i++) {
      t3_material *mat = m->materials[i];
      if (!mat || (mat->type != T3_MESH_STANDARD_MATERIAL && mat->type != T3_MESH_PHYSICAL_MATERIAL)) continue;
      if (cw_shading_has(mat, "gi")) continue;
      /* a surface extension of its own (terrain) stays alone: only bare
       * materials and the lighting part compose with gi */
      const char *ext = t3_material_get_extension(mat);
      if (ext && !cw_shading_has(mat, "cluster")) continue;
      if (!cw_shading_add(mat, "gi")) continue;
      t3_retain(mat);
      cw_array_push(&g->mats, mat);
    }
  }
  for (int i = 0; i < o->child_count; i++) cw__gi_tag(g, o->children[i]);
}

static void captures(gi_tree *g, cw_pass_ctx *ctx);

static void probes_exec(cw_pass_ctx *ctx, void *user) {
  (void)user;
  gi_tree *g = cw__gi_tree(ctx->tree);
  g->stats.captures = 0;
  g->stats.probes_bound_max = g->bound_max;
  g->bound_max = 0;
  if (!ctx->renderer || !ctx->scene3d) return;
  if (g->hook_renderer != ctx->renderer) {
    cw_shading_part(ctx->renderer, "gi", cw__gi_fill, g);
    g->hook_renderer = ctx->renderer;
  }
  /* the sky's environment changed (a new sky render): what was captured once
   * holds the old sky, so ONCE probes capture again */
  if (ctx->scene3d->environment != g->env_seen) {
    g->env_seen = ctx->scene3d->environment;
    for (int i = 0; i < g->probes.count; i++) {
      refl_probe *p = cw_node_data_t(g->probes.data[i], probe_t);
      if (p && p->update_mode == CW_PROBE_UPDATE_ONCE && p->captures) p->requested = true;
    }
    cw__gi_grids_sky_changed(g);
  }
  /* materials appear as nodes enter: tag what is new (cheap: a walk of the
   * scene's objects) */
  cw__gi_tag(g, &ctx->scene3d->base);
  /* the grid first: probes captured after it see its bounce light */
  cw__gi_grids_exec(g, ctx);
  captures(g, ctx);
  /* the screen-space passes' G-buffer, after this frame's captures (ssr.c) */
  int hw = (ctx->width + 1) / 2, hh = (ctx->height + 1) / 2;
  bool want = ctx->settings->ssr || ctx->settings->ssgi;
  g->gbuf_ok = want && cw__gi_gbuffer(ctx, g, hw, hh, 1);
  g->albedo_ok = ctx->settings->ssgi && g->gbuf_ok && cw__gi_gbuffer(ctx, g, hw, hh, 2);
}

static void captures(gi_tree *g, cw_pass_ctx *ctx) {
  if (!g->probes.count) return;
  t3_vec3 cam = ctx->camera ? t3_mat4_get_position(&ctx->camera->base.matrix_world) : (t3_vec3){ 0, 0, 0 };
  due list[64];
  int nd = 0;
  for (int i = 0; i < g->probes.count && nd < 64; i++) {
    cw_node n = g->probes.data[i];
    refl_probe *p = cw_node_data_t(n, probe_t);
    if (!p) continue;
    p->age++;
    int over = 0;
    if (!p->rt[p->front] || p->size_now != probe_size(p)) over = 1 << 20;
    else if (p->requested) over = 1 << 19;
    else if (p->update_mode == CW_PROBE_UPDATE_EVERY_N_FRAMES) {
      int every = (p->update_every < 1 ? 1 : p->update_every) * every_scale();
      if (p->age >= every) over = p->age - every + 1;
    }
    if (!over) continue;
    float c[3], h[3], o[3];
    probe_box(n, p, c, h, o);
    float dx = o[0] - cam.x, dy = o[1] - cam.y, dz = o[2] - cam.z;
    list[nd++] = (due){ n, p, over, dx * dx + dy * dy + dz * dz };
  }
  if (!nd) return;
  qsort(list, (size_t)nd, sizeof list[0], due_cmp);
  int budget = capture_budget();
  for (int i = 0; i < nd && i < budget; i++) {
    if (!capture(g, ctx, list[i].n, list[i].p)) continue;
    list[i].p->age = 0;
    list[i].p->requested = false;
    g->stats.captures++;
    g->stats.captures_total++;
  }
}

/* ── the fill hook: two probes per draw ──────────────────────────────── */

t3_texture *cw__gi_dummy(gi_tree *g) {
  if (!g->dummy) {
    uint16_t px[4 * 4 * 4] = { 0 };
    g->dummy = t3_data_texture_new_typed(4, 4, px, T3_HALF_FLOAT_TYPE);
    g->dummy->mag_filter = g->dummy->min_filter = T3_LINEAR;
    g->dummy->generate_mipmaps = false;
    g->dummy->wrap_s = g->dummy->wrap_t = T3_CLAMP_TO_EDGE;
  }
  return g->dummy;
}

/* the object's world bounding sphere (geometry bounds through its matrix) */
static void world_sphere(const t3_object *o, float c[3], float *rad) {
  const t3_mat4 *m = &o->matrix_world;
  float lc[3] = { 0, 0, 0 }, lr = 0;
  if (o->type == T3_MESH || o->type == T3_SKINNED_MESH || o->type == T3_INSTANCED_MESH) {
    t3_geometry *geo = ((const t3_mesh *)o)->geometry;
    if (geo) {
      if (!geo->has_bounding_sphere) t3_geometry_compute_bounding_sphere(geo);
      lc[0] = geo->bounding_sphere.center.x, lc[1] = geo->bounding_sphere.center.y, lc[2] = geo->bounding_sphere.center.z;
      lr = geo->bounding_sphere.radius;
    }
  }
  float s = 0;
  for (int k = 0; k < 3; k++) {
    c[k] = m->e[k] * lc[0] + m->e[4 + k] * lc[1] + m->e[8 + k] * lc[2] + m->e[12 + k];
    const float *col = &m->e[k * 4];
    float l = col[0] * col[0] + col[1] * col[1] + col[2] * col[2];
    if (l > s) s = l;
  }
  *rad = lr * sqrtf(s);
}

typedef struct cand { refl_probe *p; float c[3], h[3], o[3]; int inside; float vol; uint64_t id; } cand;

static int cand_cmp(const cand *x, const cand *y) {
  if (x->inside != y->inside) return y->inside - x->inside;
  if (x->vol != y->vol) return x->vol < y->vol ? -1 : 1;
  return x->id < y->id ? -1 : x->id > y->id;
}

static void set_slot(t3_extension_ctx *c, gi_tree *g, char k, const cand *q) {
  char nm[8] = "giA0";
  nm[2] = k;
  float v[4] = { 0, 0, 0, 0 };
  t3_texture *tex = cw__gi_dummy(g);
  float sz[4] = { 0, 0, 4, 0 };
  if (q) {
    const refl_probe *p = q->p;
    v[0] = q->c[0], v[1] = q->c[1], v[2] = q->c[2], v[3] = p->blend_distance;
    t3_extension_set_uniform(c, nm, v, 4);
    nm[3] = '1';
    v[0] = q->h[0], v[1] = q->h[1], v[2] = q->h[2], v[3] = p->intensity;
    t3_extension_set_uniform(c, nm, v, 4);
    nm[3] = '2';
    v[0] = q->o[0], v[1] = q->o[1], v[2] = q->o[2], v[3] = (float)((p->box_projection ? 1 : 0) | (p->ambient ? 2 : 0));
    t3_extension_set_uniform(c, nm, v, 4);
    t3_render_target *rt = p->rt[p->front];
    tex = rt->texture;
    float hgt = (float)rt->height;
    /* r186 PMREMNode's size uniforms from the target's height */
    float mm = log2f(hgt) - 2;
    float face = exp2f(mm) > 112 ? exp2f(mm) : 112;
    sz[0] = 1.0f / (3 * face), sz[1] = 1.0f / hgt, sz[2] = mm;
  } else {
    t3_extension_set_uniform(c, nm, v, 4);
    nm[3] = '1';
    t3_extension_set_uniform(c, nm, v, 4); /* intensity 0: empty */
    nm[3] = '2';
    t3_extension_set_uniform(c, nm, v, 4);
  }
  nm[3] = '3';
  t3_extension_set_uniform(c, nm, sz, 4);
  char tn[16] = "giProbeA";
  tn[7] = k;
  t3_extension_set_texture(c, tn, tex);
}

void cw__gi_fill(t3_extension_ctx *c, void *user) {
  gi_tree *g = user;
  const t3_object *o = t3_extension_object(c);
  cand best[2];
  int nb = 0;
  if (o) {
    float oc[3], orad;
    world_sphere(o, oc, &orad);
    for (int i = 0; i < g->probes.count; i++) {
      cw_node n = g->probes.data[i];
      refl_probe *p = cw_node_data_t(n, probe_t);
      if (!p || !p->rt[p->front] || p->intensity <= 0) continue;
      cand q = { .p = p, .id = n.id };
      probe_box(n, p, q.c, q.h, q.o);
      /* the sphere reaches the box */
      float d2 = 0;
      int inside = 1;
      for (int k = 0; k < 3; k++) {
        float d = fabsf(oc[k] - q.c[k]) - q.h[k];
        if (d > 0) d2 += d * d, inside = 0;
      }
      if (d2 > orad * orad) continue;
      q.inside = inside;
      q.vol = q.h[0] * q.h[1] * q.h[2];
      if (nb < 2) best[nb++] = q;
      else if (cand_cmp(&q, &best[1]) < 0) best[1] = q;
      if (nb == 2 && cand_cmp(&best[1], &best[0]) < 0) {
        cand t = best[0];
        best[0] = best[1], best[1] = t;
      }
    }
  }
  if (nb > g->bound_max) g->bound_max = nb;
  set_slot(c, g, 'A', nb > 0 ? &best[0] : NULL);
  set_slot(c, g, 'B', nb > 1 ? &best[1] : NULL);
  cw__gi_fill_grid(c, g);
  float m[4] = { (float)g->mode, 0, 0, 0 };
  t3_extension_set_uniform(c, "giMode", m, 4);
}

/* ── the node ───────────────────────────────────────────────────────── */

void cw_reflection_probe_update(cw_node probe) {
  refl_probe *p = cw_node_data_t(probe, probe_t);
  if (p) p->requested = true;
}

static bool m_update(cw_node self, const cw_value *args, int argc, cw_value *ret, void *user) {
  (void)args, (void)argc, (void)ret, (void)user;
  cw_reflection_probe_update(self);
  return true;
}

static void probe_init(cw_node n, void *data) {
  (void)n;
  refl_probe *p = data;
  p->size = (t3_vec3){ 10, 10, 10 };
  p->blend_distance = 1;
  p->intensity = 1;
  p->ambient = true;
  p->resolution = 128;
  p->update_every = 1;
  p->near = 0.1f;
  p->far = 100;
  p->cull_mask = -1;
}

static void probe_free_targets(refl_probe *p) {
  for (int k = 0; k < 2; k++)
    if (p->rt[k]) t3_release(p->rt[k]), p->rt[k] = NULL;
}

static void probe_enter(cw_node n, void *data) {
  (void)data;
  cw_tree *t = cw_node_tree(n);
  gi_tree *g = cw__gi_tree(t);
  cw_array_push(&g->probes, n);
  (void)t;
}

static void probe_exit(cw_node n, void *data) {
  refl_probe *p = data;
  cw_tree *t = cw_node_tree(n);
  gi_tree *g = t ? cw_tree_slot(t, "gi") : NULL;
  if (g)
    for (int i = 0; i < g->probes.count; i++)
      if (g->probes.data[i].id == n.id) {
        memmove(&g->probes.data[i], &g->probes.data[i + 1], sizeof g->probes.data[0] * (size_t)(g->probes.count - i - 1));
        g->probes.count--;
        break;
      }
  probe_free_targets(p);
  p->size_now = 0;
}

static void probe_destroy(cw_node n, void *data) {
  (void)n;
  probe_free_targets(data);
}

static void ro_captures(cw_node n, void *d, cw_value *o) { (void)n; *o = cw_val_int(((refl_probe *)d)->captures); }

static const cw_prop probe_props[] = {
  CW_PROP_FIELD(refl_probe, size, CW_T_VEC3, CW_PROP_DEFAULT, "The box the probe affects (world units, axis aligned; the node's scale applies)."),
  CW_PROP_FIELD(refl_probe, origin_offset, CW_T_VEC3, CW_PROP_DEFAULT, "Where the capture is taken, from the box centre."),
  CW_PROP_FIELD(refl_probe, box_projection, CW_T_BOOL, CW_PROP_DEFAULT,
                "Trace reflections to the box's faces (rooms, corridors) instead of at infinity (outdoors)."),
  CW_PROP_FIELD_RANGE(refl_probe, blend_distance, CW_T_FLOAT, 0, 1000, "Distance inside the box over which the probe fades out at its faces."),
  CW_PROP_FIELD_RANGE(refl_probe, intensity, CW_T_FLOAT, 0, 100, "Multiplier of what the probe reflects."),
  CW_PROP_FIELD(refl_probe, ambient, CW_T_BOOL, CW_PROP_DEFAULT, "The probe also gives the diffuse ambient light, in place of the sky's."),
  CW_PROP_FIELD_RANGE(refl_probe, resolution, CW_T_INT, 16, 1024, "Capture face size in pixels (a power of two; the quality preset scales it)."),
  { .name = "update_mode", .type = CW_T_ENUM, .flags = CW_PROP_DEFAULT, .offset = offsetof(refl_probe, update_mode), .size = sizeof(int),
    .enum_names = update_names, .doc = "ONCE (first frame), ON_DEMAND (the update method) or EVERY_N_FRAMES (update_every)." },
  CW_PROP_FIELD_RANGE(refl_probe, update_every, CW_T_INT, 1, 100000, "EVERY_N_FRAMES: frames between captures (the quality preset scales it)."),
  CW_PROP_FIELD_RANGE(refl_probe, near, CW_T_FLOAT, 0.001, 1000, "Capture near clip."),
  CW_PROP_FIELD_RANGE(refl_probe, far, CW_T_FLOAT, 0.01, 100000, "Capture far clip."),
  CW_PROP_FIELD(refl_probe, cull_mask, CW_T_INT, CW_PROP_DEFAULT, "Layers the capture sees (three.c object layers; -1 = all)."),
  CW_PROP_ACCESSOR("captures", CW_T_INT, CW_PROP_EDITOR | CW_PROP_READONLY, ro_captures, NULL, "Captures made so far."),
  CW_PROP_END,
};

static cw_node_type probe_type = {
  .name = "ReflectionProbe3D", .parent = "Node3D", .data_size = sizeof(refl_probe), .props = probe_props,
  .doc = "Reflections of the real surroundings inside a box: a live PMREM capture, box projected and blended (gi.h).",
  .init = probe_init, .enter_tree = probe_enter, .exit_tree = probe_exit, .destroy = probe_destroy,
};

void cw__register_reflection_probe(void) {
  cw_node_type_register(&probe_type);
  probe_t = &probe_type;
  cw_node_type_add_method("ReflectionProbe3D", "update", NULL, m_update, NULL, "Capture again at the next opportunity.");
}

/* the "gi" pass (every pipeline has it; it runs for a tree with a probe, a
 * grid or SSR) */
static bool gi_active(const cw_pass_ctx *ctx, void *user) {
  (void)user;
  if (ctx->settings->ssr || ctx->settings->ssgi) return true;
  const gi_tree *g = cw_tree_slot(ctx->tree, "gi");
  return g && (g->probes.count || g->grids.count);
}

void cw__gi_add_shadow_pass(cw_pipeline *p) {
  cw_pipeline_add_pass(p, &(cw_pass_desc){ .name = "gi", .phase = CW_PHASE_SHADOW, .order = 150,
                                           .writes = CW_RES_SHADOW, .active = gi_active, .execute = probes_exec, .webgpu = true });
}

void cw__register_gi_types(void);
void cw__register_gi_types(void) {
  cw__register_reflection_probe();
  cw__register_probe_grid();
}
