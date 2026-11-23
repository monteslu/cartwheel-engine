/* foliage_field: grass and leaf cards that look like plants (cartwheel/foliage.h, cartwheel/terrain.h): the built-in
 * GRASS tuft over a gently rolling terrain of two layers, seen from a play camera, and the scenes the gates measure.
 * Everything is set from test switches (cw_test_env, read from frame 2), so one cart runs every case:
 *
 *   FOL_SCENE   field (default): the GRASS layer from a play camera 2.2 m up
 *               cards: a layer of crossed alpha-tested leaf cards (made here: cw_terrain_scatter_add) instead
 *               wind: a sparse layer of tall tufts on flat ground before a magenta background, the camera 0.3 m up
 *                     looking along the ground: the tips stand against the background, each tracked from the picture
 *               twosided: two upright double-sided cards with straight-up normals, one showing the camera its
 *                     front face and one its back
 *               root: a disc of dirt in the grass seen from 12 m straight above
 *               (<scene>_all: every option below on, for the program survey)
 *   FOL_CAM     play (default), pan (the play camera turning 2 degrees a second), dolly (looking level, moving
 *               forward 1 m a second through the LOD distance)
 *   FOL_WIND=<m> wind strength (FOL_FREQ, FOL_GUST)      FOL_ALPHA=smooth      FOL_FADE=dither (FOL_BLEND=<m>)
 *   FOL_ROOT=1 root_from_terrain     FOL_TWOSIDED=1     FOL_JITTER=<0..1>     FOL_NOGRASS=1 (no scatter drawn)
 *   FOL_MSAA=<n> PostProcess msaa (kept on every anti-aliasing value)     FOL_TAA=1 PostProcess taa
 *   FOL_FROM=<frame> where the measurements start (default 30)
 *
 * Measurements, from the picture (a window read every frame, cartwheel/gpu.h), as debug fields:
 *   fol.frames        pictures measured
 *   fol.flip_mean, fol.flip_max   share of the measured pixels whose brightness moved by more than 0.12 from the last
 *                     picture (mean and the largest single frame); in the dolly, of the rows where the ground is
 *                     8 to 20 m away (the LOD switch at 14 m)
 *   fol.block_mean, fol.block_max   the same over 8 x 8 pixel blocks (a block's mean brightness moved by more than
 *                     0.02): what a mesh changing shape at once moves, and a sub-pixel shift of the picture hardly
 *   fol.tips, fol.tip_amp, fol.tip_corr   wind: tufts tracked, the median of their tips' travel (metres, the
 *                     tip band's centroid, max - min), the mean correlation of neighbouring tufts' tracks
 *   fol.front, fol.back   twosided: mean brightness of the card showing its front and of the one showing its back
 *   fol.disc_rg, fol.field_rg   root: the dirt disc's and the grass field's mean red over green
 *   fol.color_var, fol.root_err   the layer's instance colours in the cell under the camera: their brightness
 *                     variance, and the largest log ratio of an instance's colour to the terrain's under it
 *   fol.shimmer       the pan: share of the middle 640 x 400 pixels whose brightness differs by more than 0.1 from the
 *                     picture 11 frames before moved by the pan (6 pixels): what flickers rather than moves
 *   fol.cover         the dolly (over dirt, FOL_LODREF=1: LOD 0 to the end): per frame, the share of the rows 11 to 17 m
 *                     away that is grass; a run less its reference is what the LOD switch changes, frame by frame
 *   fol.band          the dolly: those rows as 96 x 8 blocks of mean brightness per frame (bytes), for the same
 *                     difference block by block
 *   fol.gpu_ms        the pipeline's GPU time, averaged over the measured frames */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cartwheel/debug.h"
#include "cartwheel/foliage.h"
#include "cartwheel/gpu.h"
#include "cartwheel/pipeline.h"
#include "cartwheel/platform.h"
#include "cartwheel/quality.h"
#include "cartwheel/render3d.h"
#include "cartwheel/scene_file.h"
#include "cartwheel/terrain.h"

#define W 1920
#define H 1080
#define FOV 60.0f

static cw_tree *tree;
static cw_node cam, terrain_node, grass_node;
static int32_t tick, frames, tips_n, instances;
static float flip_mean, flip_max, tip_amp, tip_corr, front_l, back_l, disc_rg, field_rg, color_var, root_err, gpu_ms;
static double flip_sum, gpu_sum, block_sum;
static float block_mean, block_max;
/* the dolly: the share of the LOD band's pixels that are grass, per frame (a frame's own index) */
#define SERIES 1200
static float cover[SERIES];
/* the same rows as 96 x 8 blocks of mean brightness (0..255) per frame, for a run less its reference block by block */
#define BX 96
#define BY 8
static uint8_t band[SERIES * BX * BY];
/* the pan: shimmer against the picture 11 frames before, moved by the pan's 6 pixels */
#define LAG 11
#define SH_W 640
#define SH_H 400
static uint8_t *hist[LAG + 1];
static int hist_n;
static float shimmer;
static double shimmer_sum;
static int shimmer_n;
static char scene[16] = "field", camm[16] = "play";
static bool applied;
static int from = 30;
static float cam_h = 2.2f;
static int card_layer = -1, wind_layer = -1;
static cw_node card_nodes[2];

static bool all_on; /* FOL_SCENE <scene>_all: every option on (the program survey) */
static const char *env(const char *n) {
  const char *v = cw_test_env(n);
  if (v || !all_on) return v;
  if (!strcmp(n, "FOL_WIND")) return "0.08";
  if (!strcmp(n, "FOL_ALPHA")) return "smooth";
  if (!strcmp(n, "FOL_FADE")) return "dither";
  if (!strcmp(n, "FOL_ROOT") || !strcmp(n, "FOL_TWOSIDED")) return "1";
  return NULL;
}
static float envf(const char *n, float d) { const char *v = env(n); return v ? (float)atof(v) : d; }

/* ── the cards: two crossed quads of grass blades cut out of a texture ── */

static t3_texture *card_texture(void) {
  enum { S = 128 };
  static uint8_t px[S * S * 4];
  memset(px, 0, sizeof px);
  for (int b = 0; b < 9; b++) {
    uint32_t h = (uint32_t)b * 2654435761u;
    float x0 = 10 + (float)(h % 108), lean = ((float)((h >> 8) % 41) - 20) * 0.6f, top = 20 + (float)((h >> 16) % 50);
    float w = 3.0f + (float)((h >> 4) % 3);
    for (int y = (int)top; y < S; y++) {
      float t = (float)(S - y) / (float)(S - top); /* 0 at the root, 1 at the tip */
      float cx = x0 + lean * t * t, hw = w * (1 - t) + 0.4f;
      for (int x = (int)(cx - hw - 1); x <= (int)(cx + hw + 1); x++) {
        if (x < 0 || x >= S) continue;
        float cov = cw_clampf(hw - fabsf((float)x + 0.5f - cx), 0, 1);
        uint8_t *p = &px[4 * (y * S + x)];
        if (cov * 255 > p[3]) {
          p[0] = (uint8_t)(50 + 70 * t), p[1] = (uint8_t)(80 + 80 * t), p[2] = (uint8_t)(25 + 20 * t);
          p[3] = (uint8_t)(cov * 255);
        }
      }
    }
  }
  t3_texture *tx = t3_texture_new(S, S, px);
  tx->color_space = T3_SRGB_COLOR_SPACE;
  return tx;
}

static t3_geometry *card_geometry_n(int quads);
static t3_geometry *card_geometry(void) { return card_geometry_n(2); }
/* quads: 2 crossed (LOD 0), or 1 (LOD 1: a coarser stand-in, so the switch shows) */
static t3_geometry *card_geometry_n(int quads) {
  /* two quads 0.7 wide and 0.5 tall crossed at right angles, normals straight up (a common foliage trick) */
  float pos[8 * 3], nrm[8 * 3], uv[8 * 2];
  uint16_t idx[12];
  for (int q = 0; q < quads; q++)
    for (int v = 0; v < 4; v++) {
      int i = q * 4 + v;
      float sx = (v & 1) ? 0.35f : -0.35f, y = (v & 2) ? 0.5f : 0;
      pos[3 * i] = q ? 0 : sx, pos[3 * i + 1] = y, pos[3 * i + 2] = q ? sx : 0;
      nrm[3 * i] = 0, nrm[3 * i + 1] = 1, nrm[3 * i + 2] = 0;
      uv[2 * i] = (v & 1) ? 1.0f : 0.0f, uv[2 * i + 1] = (v & 2) ? 1.0f : 0.0f;
    }
  for (int q = 0; q < quads; q++) {
    uint16_t b = (uint16_t)(q * 4), t[6] = { b, (uint16_t)(b + 1), (uint16_t)(b + 3), b, (uint16_t)(b + 3), (uint16_t)(b + 2) };
    memcpy(&idx[q * 6], t, sizeof t);
  }
  t3_geometry *g = t3_geometry_new();
  t3_attribute *a = t3_attribute_new(T3_FLOAT32, pos, 4 * quads, 3);
  t3_geometry_set_attribute(g, T3_ATTR_POSITION, a), t3_release(a);
  a = t3_attribute_new(T3_FLOAT32, nrm, 4 * quads, 3);
  t3_geometry_set_attribute(g, T3_ATTR_NORMAL, a), t3_release(a);
  a = t3_attribute_new(T3_FLOAT32, uv, 4 * quads, 2);
  t3_geometry_set_attribute(g, T3_ATTR_UV, a), t3_release(a);
  a = t3_attribute_new(T3_UINT16, idx, 6 * quads, 1);
  t3_geometry_set_index(g, a), t3_release(a);
  return g;
}

/* the options every layer here takes from the switches */
static void layer_options(cw_scatter_def *d) {
  d->wind.strength = envf("FOL_WIND", 0);
  d->wind.frequency = envf("FOL_FREQ", 0.5f);
  d->wind.gust = envf("FOL_GUST", 0.5f);
  d->smooth_alpha = env("FOL_ALPHA") && !strcmp(env("FOL_ALPHA"), "smooth");
  d->two_sided_lighting = env("FOL_TWOSIDED") != NULL;
  d->fade_mode = env("FOL_FADE") && !strcmp(env("FOL_FADE"), "dither") ? CW_SCATTER_FADE_DITHER : CW_SCATTER_FADE_SHRINK;
  d->lod_blend = envf("FOL_BLEND", 6);
  d->root_from_terrain = env("FOL_ROOT") != NULL;
}

static void add_cards(cw_terrain *t) {
  cw_scatter_def d = cw_scatter_default_def();
  d.geometry[0][0] = card_geometry();
  t3_material *m = t3_mesh_standard_material_new(0xffffff);
  t3_texture *tx = card_texture();
  t3_material_set_map(m, tx);
  t3_release(tx);
  m->alpha_test = 0.5f;
  m->side = T3_DOUBLE_SIDE;
  m->roughness = 0.85f;
  d.material[0][0] = m;
  d.geometry[1][0] = card_geometry_n(1);
  d.material[1][0] = t3_retain(m);
  d.lod_distance[0] = env("FOL_LODREF") ? 1000 : 14; /* FOL_LODREF=1: LOD 0 to the end (the dolly's reference) */
  d.density = 4;
  d.visible_range = 40;
  d.fade = 8;
  d.min_slope_cos = 0.7f;
  d.seed = 9;
  layer_options(&d);
  card_layer = cw_terrain_scatter_add(t, &d);
  cw_scatter_def_release(&d);
}

/* one blade: a stem 1 m tall and 2 cm wide (both ways round), so each instance's tip is one point to track */
static t3_geometry *stem_geometry(void) {
  enum { SEG = 8 };
  float pos[(SEG + 1) * 2 * 3], nrm[(SEG + 1) * 2 * 3];
  uint16_t idx[SEG * 12];
  for (int i = 0; i <= SEG; i++)
    for (int k = 0; k < 2; k++) {
      int v = 2 * i + k;
      pos[3 * v] = k ? 0.01f : -0.01f, pos[3 * v + 1] = (float)i / SEG, pos[3 * v + 2] = 0;
      nrm[3 * v] = 0, nrm[3 * v + 1] = 0.8f, nrm[3 * v + 2] = 0.6f;
    }
  int n = 0;
  for (int i = 0; i < SEG; i++) {
    uint16_t a = (uint16_t)(2 * i), b = (uint16_t)(a + 1), c = (uint16_t)(a + 2), e = (uint16_t)(a + 3);
    uint16_t q[12] = { a, b, e, a, e, c, a, e, b, a, c, e };
    memcpy(&idx[n], q, sizeof q), n += 12;
  }
  t3_geometry *g = t3_geometry_new();
  t3_attribute *at = t3_attribute_new(T3_FLOAT32, pos, (SEG + 1) * 2, 3);
  t3_geometry_set_attribute(g, T3_ATTR_POSITION, at), t3_release(at);
  at = t3_attribute_new(T3_FLOAT32, nrm, (SEG + 1) * 2, 3);
  t3_geometry_set_attribute(g, T3_ATTR_NORMAL, at), t3_release(at);
  at = t3_attribute_new(T3_UINT16, idx, n, 1);
  t3_geometry_set_index(g, at), t3_release(at);
  return g;
}

static void add_wind_layer(cw_terrain *t) {
  cw_scatter_def d = cw_scatter_default_def();
  d.geometry[0][0] = stem_geometry();
  t3_material *m = t3_mesh_standard_material_new(0x3a6a20);
  m->roughness = 0.9f;
  d.material[0][0] = m;
  d.align_to_normal = false;
  d.density = 0.25f;
  d.layers = 2; /* the dirt band (apply): nothing in front of it */
  d.scale_min = 0.9f, d.scale_max = 1.1f;
  d.visible_range = 30;
  d.fade = 5;
  d.lod_distance[0] = 25;
  d.seed = 5;
  layer_options(&d);
  wind_layer = cw_terrain_scatter_add(t, &d);
  cw_scatter_def_release(&d);
}

/* two upright cards with normals straight up: (0, 1, z) the left one's front faces the camera, the right one shows
 * its back */
static void add_twosided(cw_node world) {
  t3_material *m = t3_mesh_standard_material_new(0xb0c8a0);
  m->side = T3_DOUBLE_SIDE;
  m->roughness = 0.9f;
  cw_foliage_desc fd = cw_foliage_default();
  fd.two_sided_lighting = env("FOL_TWOSIDED") != NULL;
  if (fd.two_sided_lighting) cw_foliage_apply(m, &fd);
  for (int k = 0; k < 2; k++) {
    t3_geometry *g = t3_plane_geometry_new(1, 1, 1, 1);
    t3_attribute *n = g->attributes[T3_ATTR_NORMAL];
    for (int i = 0; i < n->count; i++) {
      float *v = (float *)n->array + 3 * i;
      v[0] = 0, v[1] = 1, v[2] = 0;
    }
    cw_node nd = cw_node_new("Node3D", k ? "Back" : "Front");
    cw_node_add_child(world, nd);
    t3_mesh *mesh = t3_mesh_new(g, m);
    t3_object_add(cw_node3d_object(nd), mesh);
    t3_release(mesh);
    t3_release(g);
    cw_node3d_set_position(nd, (t3_vec3){ k ? -0.7f : 0.7f, 1, 3 });
    /* the plane faces +z; turned to face the camera (-z) the front shows, else the back */
    float s, c;
    cw_sincos(k ? 0 : 3.14159265f * 0.5f, &s, &c);
    cw_node3d_set_quaternion(nd, (t3_quat){ 0, s, 0, c });
    card_nodes[k] = nd;
  }
  t3_release(m);
}

static void paint_layers(cw_terrain *t) {
  enum { S = 64 };
  static uint8_t a[S * S * 4], n[S * S * 4];
  static const uint8_t col[2][3] = { { 92, 118, 52 }, { 118, 88, 58 } }; /* grass green, dirt brown (sRGB) */
  for (int l = 0; l < 2; l++) {
    for (int i = 0; i < S * S; i++) {
      uint32_t h = (uint32_t)i * 2654435761u ^ (uint32_t)l * 0x9E3779B1u;
      int v = (int)((h >> 24) % 21) - 10;
      for (int k = 0; k < 3; k++) a[4 * i + k] = (uint8_t)CW_CLAMP(col[l][k] + v, 0, 255);
      a[4 * i + 3] = 128;
      n[4 * i] = 128, n[4 * i + 1] = 128, n[4 * i + 2] = 255, n[4 * i + 3] = 230;
    }
    cw_terrain_set_layer_pixels(t, l, a, n, S, S, 0.3f);
  }
  /* dirt in a disc of 3.5 m round (4, 0) and along a path at x = -12, grass elsewhere */
  const cw_terrain_def *d = cw_terrain_get_def(t);
  int nx = d->size_x + 1, nz = d->size_z + 1;
  uint32_t *ctl = malloc(sizeof(uint32_t) * (size_t)nx * nz);
  for (int z = 0; z < nz; z++)
    for (int x = 0; x < nx; x++) {
      float wx = d->origin.x + (float)x * d->vertex_spacing, wz = d->origin.z + (float)z * d->vertex_spacing;
      float r = sqrtf((wx - 4) * (wx - 4) + wz * wz);
      bool dirt = r < 3.5f || fabsf(wx + 12) < 1.2f;
      ctl[z * nx + x] = cw_terrain_control(dirt ? 1 : 0, 0, 0, 0, 0, false);
    }
  cw_terrain_set_control(t, 0, 0, nx, nz, ctl);
  free(ctl);
  cw_terrain_scatter_invalidate(t);
}

static void apply(void) {
  applied = true;
  const char *s = env("FOL_SCENE"), *c = env("FOL_CAM");
  if (s) snprintf(scene, sizeof scene, "%s", s);
  char *all = strstr(scene, "_all");
  if (all) *all = 0, all_on = true;
  if (c) snprintf(camm, sizeof camm, "%s", c);
  from = (int)envf("FOL_FROM", 30);
  cw_node root = cw_tree_root(tree), world = cw_node_get(root, "World");
  cw_node post = cw_node_get(root, "World/Post");
  if (env("FOL_MSAA")) {
    cw_quality_keep_msaa(true);
    cw_node_set(post, "msaa", cw_val_int((int)envf("FOL_MSAA", 4)));
  }
  if (env("FOL_TAA")) cw_node_set(post, "taa", cw_val_bool(true));
  /* the GRASS layer's options, through the node's properties */
  cw_scatter_def o = cw_scatter_default_def();
  layer_options(&o);
  cw_node_set(grass_node, "wind_strength", cw_val_float(o.wind.strength));
  cw_node_set(grass_node, "wind_frequency", cw_val_float(o.wind.frequency));
  cw_node_set(grass_node, "wind_gust", cw_val_float(o.wind.gust));
  cw_node_set(grass_node, "smooth_alpha", cw_val_bool(o.smooth_alpha));
  cw_node_set(grass_node, "two_sided_lighting", cw_val_bool(o.two_sided_lighting));
  cw_node_set(grass_node, "fade_mode", cw_val_enum(o.fade_mode ? "DITHER" : "SHRINK"));
  cw_node_set(grass_node, "lod_blend", cw_val_float(o.lod_blend));
  cw_node_set(grass_node, "root_from_terrain", cw_val_bool(o.root_from_terrain));
  if (env("FOL_JITTER")) cw_node_set(grass_node, "tint_jitter", cw_val_float(envf("FOL_JITTER", 0.25f)));
  cw_terrain *t = cw_terrain_of(terrain_node);
  bool magenta = false;
  if (!strcmp(scene, "cards")) {
    cw_node_free(grass_node);
    if (t) add_cards(t);
  } else if (!strcmp(scene, "wind")) {
    cw_node_free(grass_node);
    magenta = true;
    cam_h = 0.3f;
    if (t) {
      /* flat ground: the tips stand against the background */
      static uint16_t zero[257 * 257];
      cw_terrain_set_heights(t, 0, 0, 257, 257, zero);
      /* one band of ground 3.5 to 6.5 m ahead for the tufts: nothing stands in front of them or close behind */
      static uint32_t ctl[257 * 257];
      const cw_terrain_def *td = cw_terrain_get_def(t);
      for (int z = 0; z < 257; z++)
        for (int x = 0; x < 257; x++) {
          float wz = td->origin.z + (float)z * td->vertex_spacing;
          ctl[z * 257 + x] = cw_terrain_control(wz > 3.5f && wz < 6.5f ? 1 : 0, 0, 0, 0, 0, false);
        }
      cw_terrain_set_control(t, 0, 0, 257, 257, ctl);
      cw_terrain_scatter_invalidate(t);
      add_wind_layer(t);
    }
  } else if (!strcmp(scene, "twosided")) {
    cw_node_free(grass_node);
    magenta = true;
    cam_h = 1;
    add_twosided(world);
  }
  if (!strcmp(camm, "dolly") && t) {
    /* (the cards scene freed the GRASS node above) */
    /* dirt everywhere: grass is the green over it; FOL_LODREF=1 keeps LOD 0 to the end (the reference run) */
    const cw_terrain_def *td = cw_terrain_get_def(t);
    int nx = td->size_x + 1, nz = td->size_z + 1;
    uint32_t *ctl = malloc(sizeof(uint32_t) * (size_t)nx * nz);
    for (int i = 0; i < nx * nz; i++) ctl[i] = cw_terrain_control(1, 0, 0, 0, 0, false);
    cw_terrain_set_control(t, 0, 0, nx, nz, ctl);
    free(ctl);
    cw_terrain_scatter_invalidate(t);
    if (env("FOL_LODREF") && !strcmp(scene, "field")) cw_node_set(grass_node, "lod_distance", cw_val_float(1000));
    if (env("FOL_SPARSE") && !strcmp(scene, "field")) {
      cw_node_set(grass_node, "density", cw_val_float(0.6f));
      cw_node_set(grass_node, "scale_min", cw_val_float(1.8f));
      cw_node_set(grass_node, "scale_max", cw_val_float(2.2f));
    }
  }
  if (env("FOL_NOGRASS") && !strcmp(scene, "field")) cw_node_free(grass_node);
  if (env("FOL_NOGRASS") && !strcmp(scene, "root")) cw_node_free(grass_node);
  if (magenta) {
    cw_node e = cw_node_get(root, "World/Env");
    cw_node_free(cw_node_get(root, "World/Sky"));
    cw_node_set(e, "background", cw_val_color(1, 0, 1, 1));
    cw_node_set(e, "use_background", cw_val_bool(true));
  }
}

/* ── the camera ─────────────────────────────────────────────────────── */

static t3_vec3 cam_pos, cam_fwd;
static void place_camera(float tt) {
  float yaw = 0, pitch = -0.2f;
  t3_vec3 p = { 0, cam_h, -4 };
  if (!strcmp(camm, "pan")) yaw = tt * (2.0f * 3.14159265f / 180.0f);
  if (!strcmp(camm, "dolly")) pitch = 0, p.z = -4 + tt * 1.0f;
  if (!strcmp(scene, "wind") || !strcmp(scene, "twosided")) pitch = 0, p = (t3_vec3){ 0, cam_h, 0 };
  if (!strcmp(scene, "root")) {
    p = (t3_vec3){ 0, 12, 0 };
    cw_terrain *t = cw_terrain_of(terrain_node);
    if (t) p.y += cw_terrain_height_at(t, 0, 0);
    cw_node3d_set_position(cam, p);
    cw_node3d_look_at(cam, (t3_vec3){ 0, p.y - 12, 0.001f }, (t3_vec3){ 0, 0, 1 });
    cam_pos = p;
    return;
  }
  cw_terrain *t = cw_terrain_of(terrain_node);
  if (t) p.y += cw_terrain_height_at(t, p.x, p.z);
  float sy, cy, sp, cp;
  cw_sincos(yaw, &sy, &cy);
  cw_sincos(pitch, &sp, &cp);
  t3_vec3 f = { sy * cp, sp, cy * cp };
  cw_node3d_set_position(cam, p);
  cw_node3d_look_at(cam, (t3_vec3){ p.x + f.x, p.y + f.y, p.z + f.z }, (t3_vec3){ 0, 1, 0 });
  cam_pos = p, cam_fwd = f;
}

/* world point to window pixel for the level camera looking along +z (the wind and twosided scenes) */
static bool project_level(t3_vec3 q, float *sx, float *sy) {
  float dz = q.z - cam_pos.z;
  if (dz < 0.1f) return false;
  float th = tanf(FOV * 0.5f * 3.14159265f / 180.0f), asp = (float)W / H;
  *sx = W * 0.5f - (q.x - cam_pos.x) / dz / (th * asp) * W * 0.5f;
  *sy = H * 0.5f - (q.y - cam_pos.y) / dz / th * H * 0.5f;
  return true;
}

/* ── measuring the picture ──────────────────────────────────────────── */

#define RX 0
#define RY 270
#define RW 1920
#define RH 540
static uint32_t tickets[8];
static int ticket_frame[8], nt;
static uint8_t *cur, *prev;
static bool have_prev;
static float luma(const uint8_t *p) { return (0.2126f * p[0] + 0.7152f * p[1] + 0.0722f * p[2]) / 255.0f; }

/* wind: the tracked tufts */
#define MAXT 16
#define MAXS 900
static struct { float x, z, h, ppm; float wx0, wx1, wy0, wy1; float s[MAXS]; } tip[MAXT];
static int nser;

static void pick_tips(cw_terrain *t) {
  static float buf[5 * 4096];
  const cw_terrain_def *d = cw_terrain_get_def(t);
  int cx = (int)((0 - d->origin.x) / 32), cz = (int)((5 - d->origin.z) / 32);
  int n = 0;
  for (int ox = -1; ox <= 0 && n < 4096; ox++) n += cw_terrain_scatter_cell(t, wind_layer, cx + ox, cz, buf + 5 * n, 4096 - n);
  float th = tanf(FOV * 0.5f * 3.14159265f / 180.0f);
  int dbg[2] = { 0, 0 };
  for (int i = 0; i < n && tips_n < MAXT; i++) {
    float x = buf[5 * i], y = buf[5 * i + 1], z = buf[5 * i + 2], s = buf[5 * i + 3];
    if (z < 3 || z > 6) continue;
    dbg[0]++;
    float hh = 1.0f * s, ppm = H * 0.5f / ((z - cam_pos.z) * th);
    float sx, sy;
    if (!project_level((t3_vec3){ x, y + hh, z }, &sx, &sy)) continue;
    float hw = 0.3f * ppm, x0 = sx - hw, x1 = sx + hw, y0 = sy - 0.06f * ppm, y1 = sy + 0.25f * hh * ppm;
    if (x0 < RX + 4 || x1 > RX + RW - 4 || y0 < RY + 4 || y1 > RY + RH - 4) continue;
    dbg[1]++;
    /* a window no other nearby tuft's tip reaches into */
    bool clear = true;
    for (int k = 0; k < n && clear; k++) {
      if (k == i) continue;
      float kx = buf[5 * k], ky = buf[5 * k + 1], kz = buf[5 * k + 2], ks = buf[5 * k + 3], qx, qy;
      if (kz < 0.5f || kz > 25) continue;
      if (!project_level((t3_vec3){ kx, ky + 1.0f * ks, kz }, &qx, &qy)) continue;
      /* a stem leans up to its wind strength either way of its root, from its root up to its tip */
      float kw = 0.3f * (H * 0.5f / ((kz - cam_pos.z) * th)), rx, ry;
      project_level((t3_vec3){ kx, ky, kz }, &rx, &ry);
      if (qx + kw > x0 && qx - kw < x1 && qy < y1 && ry > y0) clear = false;
    }
    if (!clear) continue;
    tip[tips_n].x = x, tip[tips_n].z = z, tip[tips_n].h = hh, tip[tips_n].ppm = ppm;
    tip[tips_n].wx0 = x0, tip[tips_n].wx1 = x1, tip[tips_n].wy0 = y0, tip[tips_n].wy1 = y1;
    tips_n++;
  }
  cw_info("foliage_field: tips: %d instances, %d at 3-6 m, %d in the window, %d clear; cam %.2f %.2f %.2f", n, dbg[0], dbg[1], tips_n, cam_pos.x, cam_pos.y, cam_pos.z);
}

static bool background(const uint8_t *p) { return p[0] > 170 && p[2] > 170 && p[1] + 40 < p[0]; }

static void wind_stats(void) {
  if (nser < 4 || !tips_n) return;
  float amps[MAXT];
  for (int k = 0; k < tips_n; k++) {
    float lo = 1e9f, hi = -1e9f;
    for (int i = 0; i < nser; i++) lo = CW_MIN(lo, tip[k].s[i]), hi = CW_MAX(hi, tip[k].s[i]);
    amps[k] = (hi - lo) / tip[k].ppm;
  }
  for (int a = 0; a < tips_n; a++)
    for (int b = a + 1; b < tips_n; b++)
      if (amps[b] < amps[a]) { float x = amps[a]; amps[a] = amps[b]; amps[b] = x; }
  tip_amp = amps[tips_n / 2];
  /* neighbours: each tuft with the nearest other in x */
  double csum = 0;
  int cn = 0;
  for (int a = 0; a < tips_n; a++) {
    int bb = -1;
    float bd = 1e9f;
    for (int b = 0; b < tips_n; b++)
      if (b != a && fabsf(tip[b].x - tip[a].x) < bd) bd = fabsf(tip[b].x - tip[a].x), bb = b;
    if (bb < 0) continue;
    double ma = 0, mb = 0, sab = 0, saa = 0, sbb = 0;
    for (int i = 0; i < nser; i++) ma += tip[a].s[i], mb += tip[bb].s[i];
    ma /= nser, mb /= nser;
    for (int i = 0; i < nser; i++) {
      double u = tip[a].s[i] - ma, v = tip[bb].s[i] - mb;
      sab += u * v, saa += u * u, sbb += v * v;
    }
    if (saa > 0 && sbb > 0) csum += sab / sqrt(saa * sbb), cn++;
  }
  tip_corr = cn ? (float)(csum / cn) : 0;
}

static float region_mean(const uint8_t *img, float cx, float cy, float r, int ch) {
  double s = 0;
  int n = 0;
  for (int y = (int)(cy - r); y <= (int)(cy + r); y++)
    for (int x = (int)(cx - r); x <= (int)(cx + r); x++) {
      if (x < RX || y < RY || x >= RX + RW || y >= RY + RH) continue;
      if ((x - cx) * (x - cx) + (y - cy) * (y - cy) > r * r) continue;
      const uint8_t *p = &img[4 * ((y - RY) * RW + (x - RX))];
      s += ch < 0 ? luma(p) : p[ch] / 255.0;
      n++;
    }
  return n ? (float)(s / n) : 0;
}

static void measure(int frame) {
  frames++;
  /* frame-to-frame flips */
  int y0 = 0, y1 = RH;
  if (!strcmp(camm, "dolly")) {
    float th = tanf(FOV * 0.5f * 3.14159265f / 180.0f);
    y0 = (int)(H * 0.5f + cam_h / 20.0f / th * H * 0.5f) - RY, y1 = (int)(H * 0.5f + cam_h / 8.0f / th * H * 0.5f) - RY;
    y0 = CW_CLAMP(y0, 0, RH), y1 = CW_CLAMP(y1, 0, RH);
  }
  if (have_prev && y1 > y0) {
    int n = 0;
    for (int y = y0; y < y1; y++)
      for (int x = 0; x < RW; x++) {
        const uint8_t *a = &cur[4 * (y * RW + x)], *b = &prev[4 * (y * RW + x)];
        if (fabsf(luma(a) - luma(b)) > 0.12f) n++;
      }
    float f = (float)n / (float)((y1 - y0) * RW);
    flip_sum += f;
    flip_mean = (float)(flip_sum / (frames - 1));
    if (f > flip_max) flip_max = f;
    /* the same over 8 x 8 blocks: a block whose mean brightness moved by more than 0.02 (a sub-pixel shift of
     * the picture moves a block little; a mesh that changes shape at once moves it) */
    int nb = 0, bn = 0;
    for (int by = y0; by + 8 <= y1; by += 8)
      for (int bx = 0; bx + 8 <= RW; bx += 8) {
        float sa = 0, sb = 0;
        for (int y = by; y < by + 8; y++)
          for (int x = bx; x < bx + 8; x++) sa += luma(&cur[4 * (y * RW + x)]), sb += luma(&prev[4 * (y * RW + x)]);
        nb++;
        if (fabsf(sa - sb) / 64 > 0.02f) bn++;
      }
    float bf = nb ? (float)bn / (float)nb : 0;
    block_sum += bf;
    block_mean = (float)(block_sum / (frames - 1));
    if (bf > block_max) block_max = bf;
  }
  if (!strcmp(camm, "dolly") && frame >= 0 && frame < SERIES) {
    /* rows where the ground is 11 to 17 m away (the switch at 14 m) */
    float th = tanf(FOV * 0.5f * 3.14159265f / 180.0f);
    int a = (int)(H * 0.5f + cam_h / 17.0f / th * H * 0.5f) - RY, b = (int)(H * 0.5f + cam_h / 11.0f / th * H * 0.5f) - RY;
    a = CW_CLAMP(a, 0, RH), b = CW_CLAMP(b, 0, RH);
    int g = 0;
    for (int y = a; y < b; y++)
      for (int x = 0; x < RW; x++) {
        const uint8_t *p = &cur[4 * (y * RW + x)];
        if (p[1] > p[0] * 1.08f && p[1] > p[2] * 1.3f) g++; /* green grass over brown dirt */
      }
    cover[frame] = b > a ? (float)g / (float)((b - a) * RW) : 0;
    int bh = (b - a) / BY, bw = RW / BX;
    for (int j = 0; j < BY && bh > 0; j++)
      for (int i = 0; i < BX; i++) {
        float sum = 0;
        for (int y = a + j * bh; y < a + (j + 1) * bh; y++)
          for (int x = i * bw; x < (i + 1) * bw; x++) sum += luma(&cur[4 * (y * RW + x)]);
        band[(frame * BY + j) * BX + i] = (uint8_t)(sum / (float)(bh * bw) * 255 + 0.5f);
      }
  }
  if (!strcmp(camm, "pan")) {
    /* the middle 640 x 400 as brightness, kept for LAG frames */
    uint8_t *l = hist[hist_n % (LAG + 1)];
    for (int y = 0; y < SH_H; y++)
      for (int x = 0; x < SH_W; x++) l[y * SH_W + x] = (uint8_t)(luma(&cur[4 * ((y + (RH - SH_H) / 2) * RW + x + (RW - SH_W) / 2)]) * 255);
    if (hist_n >= LAG) {
      const uint8_t *o = hist[(hist_n - LAG) % (LAG + 1)];
      int best = SH_W * SH_H;
      for (int dir = -1; dir <= 1; dir += 2) {
        int n = 0, sh = 6 * dir;
        for (int y = 0; y < SH_H; y++)
          for (int x = 8; x < SH_W - 8; x++)
            if (abs((int)l[y * SH_W + x] - (int)o[y * SH_W + x - sh]) > 25) n++;
        if (n < best) best = n;
      }
      shimmer_sum += (double)best / ((SH_W - 16) * SH_H);
      shimmer_n++;
      shimmer = (float)(shimmer_sum / shimmer_n);
    }
    hist_n++;
  }
  if (!strcmp(scene, "wind") && tips_n && nser < MAXS) {
    for (int k = 0; k < tips_n; k++) {
      double sx = 0;
      int n = 0;
      for (int y = (int)tip[k].wy0; y < (int)tip[k].wy1; y++)
        for (int x = (int)tip[k].wx0; x < (int)tip[k].wx1; x++) {
          const uint8_t *p = &cur[4 * ((y - RY) * RW + (x - RX))];
          if (!background(p)) sx += x, n++;
        }
      tip[k].s[nser] = n ? (float)(sx / n) : (tip[k].wx0 + tip[k].wx1) * 0.5f;
    }
    nser++;
    wind_stats();
  }
  if (!strcmp(scene, "twosided")) {
    for (int k = 0; k < 2; k++) {
      float sx, sy;
      project_level((t3_vec3){ k ? -0.7f : 0.7f, 1, 3 }, &sx, &sy);
      float l = region_mean(cur, sx, sy, 0.25f * H * 0.5f / (3 * tanf(FOV * 0.5f * 3.14159265f / 180.0f)), -1);
      if (k) back_l = l;
      else front_l = l;
    }
  }
  if (!strcmp(scene, "root")) {
    /* straight down from 12 m: (4, 0) is left of the centre (the camera's right is -x looking down with +z up) */
    float ppm = H * 0.5f / (12 * tanf(FOV * 0.5f * 3.14159265f / 180.0f));
    float dx = W * 0.5f - 4 * ppm, r = 2.2f * ppm;
    disc_rg = region_mean(cur, dx, H * 0.5f, r, 0) / CW_MAX(region_mean(cur, dx, H * 0.5f, r, 1), 1e-3f);
    float gx = W * 0.5f + 3.5f * ppm;
    field_rg = region_mean(cur, gx, H * 0.5f, r, 0) / CW_MAX(region_mean(cur, gx, H * 0.5f, r, 1), 1e-3f);
  }
  gpu_sum += cw_pipeline_gpu_ms(cw_pipeline_of(tree));
  gpu_ms = (float)(gpu_sum / frames);
  (void)frame;
  uint8_t *t = prev;
  prev = cur, cur = t;
  have_prev = true;
}

static void colour_stats(void) {
  /* the instance colours of the GRASS layer's cell under the camera (the first scatter layer) */
  cw_terrain *t = cw_terrain_of(terrain_node);
  if (!t) return;
  enum { CAP = 4096 };
  static float xyz[5 * CAP], rgb[3 * CAP];
  const cw_terrain_def *d = cw_terrain_get_def(t);
  int cx = (int)((cam_pos.x - d->origin.x) / 32), cz = (int)((cam_pos.z - d->origin.z) / 32);
  int n = cw_terrain_scatter_cell(t, 0, cx, cz, xyz, CAP);
  int m = cw_terrain_scatter_cell_colors(t, 0, cx, cz, rgb, CAP);
  instances = n;
  if (n != m || !n) return;
  double s = 0, s2 = 0;
  float worst = 0;
  for (int i = 0; i < n; i++) {
    float l = 0.2126f * rgb[3 * i] + 0.7152f * rgb[3 * i + 1] + 0.0722f * rgb[3 * i + 2];
    s += l, s2 += (double)l * l;
    t3_color a = cw_terrain_albedo_at(t, xyz[5 * i], xyz[5 * i + 2]);
    float ac[3] = { a.r, a.g, a.b };
    for (int k = 0; k < 3; k++) worst = CW_MAX(worst, fabsf(logf(CW_MAX(rgb[3 * i + k], 1e-4f) / CW_MAX(ac[k], 1e-4f))));
  }
  color_var = (float)(s2 / n - (s / n) * (s / n));
  root_err = worst;
}

static void init(void *u) {
  (void)u;
  tree = cw_scene_app_tree();
  cw_node root = cw_tree_root(tree);
  cam = cw_node_get(root, "World/Camera");
  terrain_node = cw_node_get(root, "World/Terrain");
  grass_node = cw_node_get(root, "World/Terrain/Grass");
  cw_terrain *t = cw_terrain_of(terrain_node);
  if (t) paint_layers(t);
  cur = malloc((size_t)RW * RH * 4), prev = malloc((size_t)RW * RH * 4);
  for (int i = 0; i <= LAG; i++) hist[i] = malloc((size_t)SH_W * SH_H);
  cw_debug_i32("fol.tick", &tick);
  cw_debug_i32("fol.frames", &frames);
  cw_debug_f32("fol.flip_mean", &flip_mean);
  cw_debug_f32("fol.flip_max", &flip_max);
  cw_debug_f32("fol.block_mean", &block_mean);
  cw_debug_f32("fol.block_max", &block_max);
  cw_debug_f32("fol.shimmer", &shimmer);
  cw_debug_f32_array("fol.cover", cover, SERIES);
  cw_debug_field("fol.band", band, CW_DBG_U8, SERIES * BX * BY);
  cw_debug_i32("fol.tips", &tips_n);
  cw_debug_f32("fol.tip_amp", &tip_amp);
  cw_debug_f32("fol.tip_corr", &tip_corr);
  cw_debug_f32("fol.front", &front_l);
  cw_debug_f32("fol.back", &back_l);
  cw_debug_f32("fol.disc_rg", &disc_rg);
  cw_debug_f32("fol.field_rg", &field_rg);
  cw_debug_i32("fol.instances", &instances);
  cw_debug_f32("fol.color_var", &color_var);
  cw_debug_f32("fol.root_err", &root_err);
  cw_debug_f32("fol.gpu_ms", &gpu_ms);
  place_camera(0);
}

static void fixed_update(void *u, double dt) {
  (void)u;
  (void)dt;
  tick++;
  if (!applied && tick >= 2) apply();
}

static void render(void *u, double alpha) {
  (void)u;
  int f = (int)cw_frame_index();
  place_camera((float)(tick - 1 + alpha) / 60.0f);
  if (!applied) return;
  cw_terrain *t = cw_terrain_of(terrain_node);
  if (!strcmp(scene, "wind") && t && !tips_n && f == from - 2) pick_tips(t);
  if (f == from) colour_stats();
  /* the pictures that have arrived, in order */
  while (nt) {
    int r = cw_gpu_read_result(tickets[0], cur);
    if (r == 0) break;
    if (r == 1) measure(ticket_frame[0]);
    memmove(tickets, tickets + 1, sizeof tickets[0] * (size_t)(nt - 1));
    memmove(ticket_frame, ticket_frame + 1, sizeof ticket_frame[0] * (size_t)(nt - 1));
    nt--;
  }
  if (f >= from && nt < 8) {
    uint32_t k = cw_gpu_read_window(RX, RY, RW, RH);
    if (k) tickets[nt] = k, ticket_frame[nt++] = f;
  }
}

cw_app_desc cw_main(int argc, char **argv) {
  (void)argc;
  (void)argv;
  static cw_app_desc base = { .title = "foliage_field", .width = W, .height = H, .debug = true, .init = init,
                              .fixed_update = fixed_update, .render = render };
  return cw_scene_app("main.cwscene", &base);
}
