/* LightProbeGrid3D (cartwheel/gi.h): three.js r186's LightProbeGrid on
 * three.c. Each probe renders the scene into six small faces, an extension
 * material ("giproj", src/gi/extensions/giproj.mjs) projects them onto L2
 * spherical harmonics on the GPU and writes the seven packed groups into the
 * grid's atlas, and the "gi" shading part reads the atlas per pixel. */
#include <math.h>
#include <string.h>

#include "cartwheel/gpu.h"
#include "cartwheel/pipeline.h"
#include "cartwheel/quality.h"
#include "cartwheel/shading.h"
#include "gi_internal.h"

static const char *const grid_update_names[] = { "BAKED", "INCREMENTAL", NULL };

static const cw_node_type *grid_t;

/* the faces' cameras: giproj.mjs's FACES (look along f, up u) */
static const float FACE_F[6][3] = { { 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 }, { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 } };
static const float FACE_U[6][3] = { { 0, 1, 0 }, { 0, 1, 0 }, { 0, 0, 1 }, { 0, 0, -1 }, { 0, 1, 0 }, { 0, 1, 0 } };

/* ── quality ──────────────────────────────────────────────────────────── */

static int effects(void) { return cw_gfx_get(CW_GFX_EFFECTS); }
/* probes refreshed per frame once baked (INCREMENTAL) */
static int refresh_budget(const probe_grid *p) {
  int e = effects(), n = p->probes_per_frame;
  if (e == 0) return 0; /* Low: the bake only */
  if (e == 1) return (n + 1) / 2;
  if (e == 3) return n * 2;
  return n;
}
static int face_size(const probe_grid *p) {
  int s = p->cube_size;
  if (effects() == 0 && s > 8) s = 8;
  return s < 4 ? 4 : s > 32 ? 32 : s;
}

/* ── geometry of the grid ────────────────────────────────────────────── */

void cw__grid_bounds(cw_node n, const probe_grid *p, float lo[3], float hi[3]) {
  const t3_mat4 *m = cw_node3d_global_transform(n);
  float pos[3] = { 0, 0, 0 }, s[3] = { 1, 1, 1 };
  if (m) {
    for (int k = 0; k < 3; k++) {
      const float *col = &m->e[k * 4];
      s[k] = sqrtf(col[0] * col[0] + col[1] * col[1] + col[2] * col[2]);
    }
    pos[0] = m->e[12], pos[1] = m->e[13], pos[2] = m->e[14];
  }
  for (int k = 0; k < 3; k++) {
    float h = 0.5f * fabsf((&p->size.x)[k]) * s[k];
    lo[k] = pos[k] - h, hi[k] = pos[k] + h;
  }
}

static void grid_res(const probe_grid *p, int r[3]) {
  for (int k = 0; k < 3; k++) {
    int v = (int)(&p->probes.x)[k];
    if (v <= 0) v = (int)lroundf(fabsf((&p->size.x)[k])) + 1; /* r186: one a metre, plus one */
    r[k] = v < 2 ? 2 : v > 32 ? 32 : v;
  }
}

/* r186's order: x fastest, then z, then y */
static void probe_pos(cw_node n, const probe_grid *p, const int r[3], int i, int *ix, int *iy, int *iz, float out[3]) {
  float lo[3], hi[3];
  cw__grid_bounds(n, p, lo, hi);
  *ix = i % r[0];
  *iy = i / (r[0] * r[2]);
  *iz = (i / r[0]) % r[2];
  int id[3] = { *ix, *iy, *iz };
  for (int k = 0; k < 3; k++) out[k] = lo[k] + (hi[k] - lo[k]) * (float)id[k] / (float)(r[k] - 1);
}

/* ── GPU resources ───────────────────────────────────────────────────── */

static t3_render_target *half_rt(int w, int h, bool depth, t3_filter f) {
  t3_render_target_options o = t3_render_target_options_default();
  o.type = T3_HALF_FLOAT_TYPE;
  o.depth_buffer = depth;
  o.mag_filter = o.min_filter = f;
  o.generate_mipmaps = false;
  return t3_render_target_new(w, h, &o);
}

static void grid_free_gpu(probe_grid *p) {
  if (p->atlas) t3_release(p->atlas), p->atlas = NULL;
  p->atlas_w = p->atlas_h = 0;
}

static void shared_free(gi_tree *g) {
  for (int k = 0; k < 6; k++)
    if (g->face[k]) t3_release(g->face[k]), g->face[k] = NULL;
  if (g->face_cam) t3_release(g->face_cam), g->face_cam = NULL;
  if (g->proj_scene) t3_release(g->proj_scene), g->proj_scene = NULL;
  if (g->proj_mat) t3_release(g->proj_mat), g->proj_mat = NULL;
  if (g->proj_cam) t3_release(g->proj_cam), g->proj_cam = NULL;
  g->face_size = 0;
}

void cw__gi_grid_tree_free(gi_tree *g) { shared_free(g); }

static bool shared_ready(gi_tree *g, int fs) {
  if (g->face_size != fs) {
    for (int k = 0; k < 6; k++) {
      if (g->face[k]) t3_release(g->face[k]);
      g->face[k] = half_rt(fs, fs, true, T3_NEAREST);
    }
    g->face_size = fs;
  }
  if (!g->face_cam) g->face_cam = t3_perspective_camera_new(90, 1, 0.05f, 100);
  if (!g->proj_scene) {
    g->proj_scene = t3_scene_new();
    g->proj_mat = t3_mesh_basic_material_new(0xffffff);
    t3_material_set_extension(g->proj_mat, "giproj");
    g->proj_mat->depth_test = false;
    g->proj_mat->depth_write = false;
    t3_geometry *geo = t3_plane_geometry_new(2, 2, 1, 1);
    t3_mesh *quad = t3_mesh_new(geo, g->proj_mat);
    t3_release(geo);
    T3_OBJ(quad)->frustum_culled = false;
    t3_object_add(&g->proj_scene->base, quad);
    t3_release(quad);
    g->proj_cam = t3_orthographic_camera_new(-1, 1, 1, -1, 0, 2);
    t3_object_set_position(g->proj_cam, 0, 0, 1);
  }
  return true;
}

/* ── the projection's fill hook ──────────────────────────────────────── */

static void proj_fill(t3_extension_ctx *c, void *user) {
  gi_tree *g = user;
  float v[4] = { (float)g->face_size, (float)g->proj_group, 0, 0 };
  t3_extension_set_uniform(c, "giProj", v, 4);
  char nm[8] = "giF0";
  for (int k = 0; k < 6; k++) {
    nm[3] = (char)('0' + k);
    t3_extension_set_texture(c, nm, g->face[k]->texture);
  }
}

/* ── capture one probe ───────────────────────────────────────────────── */

static void capture_probe(gi_tree *g, t3_renderer *r, t3_scene *scene, cw_node n, probe_grid *p, const int res[3], int i) {
  int ix, iy, iz;
  float pos[3];
  probe_pos(n, p, res, i, &ix, &iy, &iz, pos);
  t3_camera *cam = g->face_cam;
  cam->near = p->near;
  cam->far = p->far;
  t3_camera_update_projection_matrix(cam);
  bool clear = t3_renderer_get_auto_clear(r);
  t3_renderer_set_auto_clear(r, true);
  for (int k = 0; k < 6; k++) {
    T3_OBJ(cam)->up = (t3_vec3){ FACE_U[k][0], FACE_U[k][1], FACE_U[k][2] };
    t3_object_set_position(cam, pos[0], pos[1], pos[2]);
    t3_object_look_at(cam, pos[0] + FACE_F[k][0], pos[1] + FACE_F[k][1], pos[2] + FACE_F[k][2]);
    t3_object_update_matrix_world(T3_OBJ(cam), true);
    t3_renderer_set_render_target(r, g->face[k]);
    t3_renderer_render(r, scene, cam);
  }
  /* the seven groups: one texel each */
  t3_renderer_set_auto_clear(r, false);
  for (int grp = 0; grp < 7; grp++) {
    g->proj_group = grp;
    int x = iz * res[0] + ix, y = grp * res[1] + iy;
    p->atlas->viewport[0] = x, p->atlas->viewport[1] = y, p->atlas->viewport[2] = 1, p->atlas->viewport[3] = 1;
    p->atlas->scissor[0] = x, p->atlas->scissor[1] = y, p->atlas->scissor[2] = 1, p->atlas->scissor[3] = 1;
    p->atlas->scissor_test = true;
    t3_renderer_set_render_target(r, p->atlas);
    t3_renderer_render(r, g->proj_scene, g->proj_cam);
  }
  t3_renderer_set_auto_clear(r, clear);
  t3_renderer_set_render_target(r, NULL);
  p->updates++;
}

/* ── per frame ───────────────────────────────────────────────────────── */

void cw__gi_grids_exec(gi_tree *g, cw_pass_ctx *ctx) {
  g->stats.grid_updates = 0;
  if (!g->grids.count) return;
  t3_renderer *r = ctx->renderer;
  cw_node n = g->grids.data[0]; /* one grid shades a tree (gi.h) */
  probe_grid *p = cw_node_data_t(n, grid_t);
  if (!p) return;
  int res[3];
  grid_res(p, res);
  int total = res[0] * res[1] * res[2];
  int aw = res[0] * res[2], ah = res[1] * 7;
  if (!p->atlas || p->atlas_w != aw || p->atlas_h != ah || p->res_now[0] != res[0] || p->res_now[1] != res[1] || p->res_now[2] != res[2]) {
    grid_free_gpu(p);
    p->atlas = half_rt(aw, ah, false, T3_LINEAR);
    p->atlas_w = aw, p->atlas_h = ah;
    memcpy(p->res_now, res, sizeof res);
    p->next = 0;
    p->sweeps = 0;
    p->baked = false;
    p->clear_atlas = true;
  }
  int budget;
  if (!p->baked) budget = p->bake_per_frame > 0 ? p->bake_per_frame : total;
  else if (p->update_mode == 1 || p->requested) budget = p->requested ? total : refresh_budget(p);
  else budget = 0;
  if (budget <= 0) return;
  bool wgpu = cw_gpu_backend() == CW_GPU_WEBGPU;
  if (wgpu) t3_renderer_set_external_encoder(r, 0);
  else t3_renderer_reset_bindings(r);
  shared_ready(g, face_size(p));
  if (g->proj_hook != r) {
    cw_shading_part(r, "giproj", proj_fill, g);
    g->proj_hook = r;
  }
  if (p->clear_atlas) {
    /* the atlas starts at zero (no light): the first sweep's surfaces see no bounce */
    t3_renderer_set_render_target(r, p->atlas);
    t3_renderer_set_clear_color(r, 0x000000, 0);
    t3_renderer_clear(r, true, false, false);
    t3_renderer_set_render_target(r, NULL);
    p->clear_atlas = false;
  }
  for (int k = 0; k < budget; k++) {
    capture_probe(g, r, ctx->scene3d, n, p, res, p->next);
    g->stats.grid_updates++;
    if (++p->next >= total) {
      p->next = 0;
      p->sweeps++;
      if (!p->baked && p->sweeps > (p->bounces < 0 ? 0 : p->bounces)) {
        p->baked = true;
        break;
      }
      if (p->requested) {
        p->requested = false;
        break;
      }
    }
  }
  if (!wgpu) t3_renderer_reset_bindings(r);
}

/* a new sky: a baked grid sweeps again (it holds the old sky's light) */
void cw__gi_grids_sky_changed(gi_tree *g) {
  for (int i = 0; i < g->grids.count; i++) {
    probe_grid *p = cw_node_data_t(g->grids.data[i], grid_t);
    if (p && p->baked) p->requested = true;
  }
}

void cw__gi_fill_grid(t3_extension_ctx *c, gi_tree *g) {
  float v[4] = { 0, 0, 0, 0 };
  probe_grid *p = g->grids.count ? cw_node_data_t(g->grids.data[0], grid_t) : NULL;
  if (!p || !p->atlas || (!p->baked && p->sweeps == 0)) {
    t3_extension_set_uniform(c, "giGridMin", v, 4);
    t3_extension_set_uniform(c, "giGridMax", v, 4);
    t3_extension_set_uniform(c, "giGridRes", v, 4);
    t3_extension_set_texture(c, "giGrid", cw__gi_dummy(g));
    return;
  }
  float lo[3], hi[3];
  cw__grid_bounds(g->grids.data[0], p, lo, hi);
  v[0] = lo[0], v[1] = lo[1], v[2] = lo[2], v[3] = p->intensity;
  t3_extension_set_uniform(c, "giGridMin", v, 4);
  v[0] = hi[0], v[1] = hi[1], v[2] = hi[2], v[3] = p->normal_offset;
  t3_extension_set_uniform(c, "giGridMax", v, 4);
  v[0] = (float)p->res_now[0], v[1] = (float)p->res_now[1], v[2] = (float)p->res_now[2], v[3] = 0;
  t3_extension_set_uniform(c, "giGridRes", v, 4);
  t3_extension_set_texture(c, "giGrid", p->atlas->texture);
}

/* ── the node ───────────────────────────────────────────────────────── */

void cw_light_probe_grid_rebake(cw_node grid) {
  probe_grid *p = cw_node_data_t(grid, grid_t);
  if (p) p->requested = true;
}

static bool m_rebake(cw_node self, const cw_value *args, int argc, cw_value *ret, void *user) {
  (void)args, (void)argc, (void)ret, (void)user;
  cw_light_probe_grid_rebake(self);
  return true;
}

static void grid_init(cw_node n, void *data) {
  (void)n;
  probe_grid *p = data;
  p->size = (t3_vec3){ 8, 4, 8 };
  p->cube_size = 8;
  p->bounces = 1;
  p->probes_per_frame = 8;
  p->intensity = 1;
  p->normal_offset = 0.5f;
  p->near = 0.05f;
  p->far = 100;
}

static void grid_enter(cw_node n, void *data) {
  (void)data;
  cw_tree *t = cw_node_tree(n);
  gi_tree *g = cw__gi_tree(t);
  cw_array_push(&g->grids, n);
  cw__gi_ensure_pass(t, g);
}

static void grid_exit(cw_node n, void *data) {
  probe_grid *p = data;
  cw_tree *t = cw_node_tree(n);
  gi_tree *g = t ? cw_tree_slot(t, "gi") : NULL;
  if (g)
    for (int i = 0; i < g->grids.count; i++)
      if (g->grids.data[i].id == n.id) {
        memmove(&g->grids.data[i], &g->grids.data[i + 1], sizeof g->grids.data[0] * (size_t)(g->grids.count - i - 1));
        g->grids.count--;
        break;
      }
  grid_free_gpu(p);
}

static void grid_destroy(cw_node n, void *data) {
  (void)n;
  grid_free_gpu(data);
}

static void ro_updates(cw_node n, void *d, cw_value *o) { (void)n; *o = cw_val_int(((probe_grid *)d)->updates); }
static void ro_baked(cw_node n, void *d, cw_value *o) { (void)n; *o = cw_val_bool(((probe_grid *)d)->baked); }
static void ro_count(cw_node n, void *d, cw_value *o) {
  (void)n;
  int r[3];
  grid_res(d, r);
  *o = cw_val_int(r[0] * r[1] * r[2]);
}

static const cw_prop grid_props[] = {
  CW_PROP_FIELD(probe_grid, size, CW_T_VEC3, CW_PROP_DEFAULT, "The grid's box (world units, axis aligned; the node's scale applies)."),
  CW_PROP_FIELD(probe_grid, probes, CW_T_VEC3, CW_PROP_DEFAULT,
                "Probes along x, y, z (2 .. 32 each; 0 = one a metre plus one, as r186)."),
  CW_PROP_FIELD_RANGE(probe_grid, cube_size, CW_T_INT, 4, 32, "Each probe's capture face size in pixels (r186: 8)."),
  CW_PROP_FIELD_RANGE(probe_grid, bounces, CW_T_INT, 0, 8, "Extra sweeps of the bake, each adding a bounce."),
  { .name = "update_mode", .type = CW_T_ENUM, .flags = CW_PROP_DEFAULT, .offset = offsetof(probe_grid, update_mode), .size = sizeof(int),
    .enum_names = grid_update_names, .doc = "BAKED (once, at load) or INCREMENTAL (then probes_per_frame probes every frame, for moving lights)." },
  CW_PROP_FIELD_RANGE(probe_grid, probes_per_frame, CW_T_INT, 1, 4096, "INCREMENTAL: probes refreshed a frame (the quality preset scales it)."),
  CW_PROP_FIELD_RANGE(probe_grid, bake_per_frame, CW_T_INT, 0, 1 << 20, "Probes captured a frame while baking (0 = the whole grid in one frame)."),
  CW_PROP_FIELD_RANGE(probe_grid, intensity, CW_T_FLOAT, 0, 100, "Multiplier of the grid's light."),
  CW_PROP_FIELD_RANGE(probe_grid, normal_offset, CW_T_FLOAT, 0, 4, "Sample offset along the surface normal, in probe spacings (r186: 0.5)."),
  CW_PROP_FIELD_RANGE(probe_grid, near, CW_T_FLOAT, 0.001, 1000, "Capture near clip."),
  CW_PROP_FIELD_RANGE(probe_grid, far, CW_T_FLOAT, 0.01, 100000, "Capture far clip."),
  CW_PROP_ACCESSOR("probe_count", CW_T_INT, CW_PROP_EDITOR | CW_PROP_READONLY, ro_count, NULL, "Probes in the grid."),
  CW_PROP_ACCESSOR("updates", CW_T_INT, CW_PROP_EDITOR | CW_PROP_READONLY, ro_updates, NULL, "Probe captures so far."),
  CW_PROP_ACCESSOR("baked", CW_T_BOOL, CW_PROP_EDITOR | CW_PROP_READONLY, ro_baked, NULL, "The bake's sweeps are done."),
  CW_PROP_END,
};

static cw_node_type grid_type = {
  .name = "LightProbeGrid3D", .parent = "Node3D", .data_size = sizeof(probe_grid), .props = grid_props,
  .doc = "Bounce light: a grid of L2 spherical-harmonic probes captured from the scene (three.js r186 LightProbeGrid; gi.h).",
  .init = grid_init, .enter_tree = grid_enter, .exit_tree = grid_exit, .destroy = grid_destroy,
};

void cw__register_probe_grid(void) {
  cw_node_type_register(&grid_type);
  grid_t = &grid_type;
  cw_node_type_add_method("LightProbeGrid3D", "rebake", NULL, m_rebake, NULL, "Capture every probe again (one sweep).");
}
