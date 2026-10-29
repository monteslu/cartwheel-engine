/* Blob shadows and light splats (cartwheel/splats.h). */
#include <math.h>
#include <string.h>

#include "cartwheel/core.h"
#include "cartwheel/math.h"
#include "cartwheel/render3d.h"
#include "cartwheel/splats.h"

struct cw_splats {
  cw_splats_kind kind;
  cw_node node;
  t3_instanced_mesh *im;
  int cap, n;
};

enum { TEX = 128 };

/* the falloff across a splat, 1 at its centre and 0 at its rim (r = 1) */
static float falloff(cw_splats_kind k, float r) {
  if (r >= 1) return 0;
  if (k == CW_SPLATS_SHADOW) {
    float s = cw_smoothstep(0.0f, 1.0f, r);
    return cw_pow(1 - s, 1.5f);
  }
  /* a point light at height h over the ground: irradiance h^3 / (h^2 + r^2)^1.5 (1 under it), windowed to 0 at the rim */
  const float h = 0.35f;
  float e = h * h * h / cw_pow(h * h + r * r, 1.5f);
  float w = 1 - r * r * r * r;
  return e * w * w;
}

static t3_texture *falloff_texture(cw_splats_kind k) {
  static uint8_t px[TEX * TEX * 4];
  for (int y = 0; y < TEX; y++)
    for (int x = 0; x < TEX; x++) {
      float u = ((float)x + 0.5f) / TEX * 2 - 1, v = ((float)y + 0.5f) / TEX * 2 - 1;
      float f = falloff(k, sqrtf(u * u + v * v));
      uint8_t c = (uint8_t)CW_CLAMP((int)(f * 255.0f + 0.5f), 0, 255);
      uint8_t *p = px + (y * TEX + x) * 4;
      p[0] = p[1] = p[2] = c;
      p[3] = 255;
    }
  t3_texture *t = t3_texture_new(TEX, TEX, px);
  t->wrap_s = t->wrap_t = T3_CLAMP_TO_EDGE;
  t->flip_y = false;
  t->generate_mipmaps = true;
  t->min_filter = T3_LINEAR_MIPMAP_LINEAR;
  t->mag_filter = T3_LINEAR;
  t->color_space = T3_NO_COLOR_SPACE;
  return t;
}

cw_splats *cw_splats_new(cw_node parent, const char *name, cw_splats_kind kind, int capacity) {
  if (capacity < 1) return NULL;
  cw_splats *s = cw_calloc(1, sizeof *s, CW_MEM_RENDER);
  s->kind = kind;
  s->cap = capacity;
  /* a 16-sided disc, not a quad: the falloff is round, and the corners a quad would add are a fifth of its pixels */
  t3_geometry *g = t3_circle_geometry_new(0.5f, 16);
  t3_geometry_apply_matrix4(g, &(t3_mat4){ { 1, 0, 0, 0, 0, 0, -1, 0, 0, 1, 0, 0, 0, 0, 0, 1 } }); /* face +Y */
  t3_material *m = t3_mesh_basic_material_new(0xffffff);
  t3_texture *t = falloff_texture(kind);
  t3_material_set_texture(m, T3_MAP, t);
  t3_release(t);
  m->transparent = true;
  m->depth_write = false;
  m->fog = false;
  if (kind == CW_SPLATS_SHADOW) {
    m->blending = T3_CUSTOM_BLENDING; /* dst x (1 - src): the light below loses strength x falloff */
    m->blend_src = T3_ZERO_FACTOR;
    m->blend_dst = T3_ONE_MINUS_SRC_COLOR_FACTOR;
    m->blend_equation = T3_ADD_EQUATION;
  } else m->blending = T3_ADDITIVE_BLENDING;
  s->im = t3_instanced_mesh_new(g, m, capacity);
  t3_release(g);
  t3_release(m);
  s->im->mesh.base.cast_shadow = false;
  s->im->mesh.base.receive_shadow = false;
  s->im->mesh.base.render_order = kind == CW_SPLATS_SHADOW ? -2 : -1; /* before the other transparent things */
  static const t3_mat4 zero;
  for (int i = 0; i < capacity; i++) {
    t3_instanced_mesh_set_matrix_at(s->im, i, &zero);
    t3_instanced_mesh_set_color_at(s->im, i, (t3_color){ 0, 0, 0 });
  }
  s->im->instance_matrix->usage = T3_DYNAMIC_DRAW;
  s->im->instance_color->usage = T3_DYNAMIC_DRAW;
  t3_sphere sph = { { 0, 0, 0 }, 1e5f }; /* placed anywhere each frame: never culled as a whole */
  t3_instanced_mesh_set_bounding_sphere(s->im, &sph);
  s->im->count = 0;
  s->node = cw_node_new("Node3D", name ? name : "Splats");
  cw_node_add_child(parent, s->node);
  t3_object *wrap = t3_group_new();
  t3_object_add(wrap, &s->im->mesh.base);
  cw_node3d_set_object(s->node, wrap);
  t3_release(wrap);
  return s;
}

void cw_splats_free(cw_splats *s) {
  if (!s) return;
  if (cw_node_valid(s->node)) cw_node_free(s->node);
  t3_release(s->im);
  cw_free(s);
}

void cw_splats_begin(cw_splats *s) {
  if (s) s->n = 0;
}

bool cw_splats_add(cw_splats *s, t3_vec3 at, float length, float width, float yaw, float offset, t3_color color,
                   float strength) {
  if (!s || s->n >= s->cap) return false;
  float sy, cy;
  cw_sincos(yaw, &sy, &cy);
  float along = offset * length * 0.5f;
  t3_mat4 m;
  t3_quat q = { 0, cw_sin(yaw * 0.5f), 0, cw_cos(yaw * 0.5f) };
  t3_mat4_compose(&m, (t3_vec3){ at.x + sy * along, at.y + 0.02f, at.z + cy * along }, q, (t3_vec3){ width, 1, length });
  t3_instanced_mesh_set_matrix_at(s->im, s->n, &m);
  float k = s->kind == CW_SPLATS_SHADOW ? CW_CLAMP(strength, 0.0f, 1.0f) : strength;
  t3_color c = s->kind == CW_SPLATS_SHADOW ? (t3_color){ k, k, k } : (t3_color){ color.r * k, color.g * k, color.b * k };
  t3_instanced_mesh_set_color_at(s->im, s->n, c);
  s->n++;
  return true;
}

void cw_splats_end(cw_splats *s) {
  if (!s) return;
  s->im->count = s->n;
  s->im->instance_matrix->version++;
  s->im->instance_color->version++;
}

int cw_splats_count(const cw_splats *s) { return s ? s->n : 0; }
t3_instanced_mesh *cw_splats_mesh(cw_splats *s) { return s ? s->im : NULL; }

/* every lit, visible OmniLight3D under root that is not baked (bake_direct off): a light splat under it */
typedef struct lights_ctx { cw_splats *s; float ground, scale; int added; } lights_ctx;
static bool light_fn(cw_node n, void *user) {
  lights_ctx *c = user;
  if (!cw_node_is(n, "OmniLight3D")) return true;
  cw_value v;
  if (cw_node_get_prop(n, "visible", &v) && v.type == CW_T_BOOL && !v.b) return true;
  if (cw_node_get_prop(n, "bake_direct", &v) && v.b) return true;
  float e = cw_node_get_prop(n, "energy", &v) ? (float)v.f : 0;
  if (e <= 0) return true;
  t3_vec3 p = cw_node3d_global_position(n);
  float h = p.y - c->ground;
  if (h <= 0.05f) return true;
  float range = cw_node_get_prop(n, "range", &v) ? (float)v.f : 0;
  /* the pool's reach: where the light's irradiance on the ground falls to a twentieth (h / r = 0.37 for an
   * inverse square light above the ground), at most its range */
  float r = h * 2.7f;
  if (range > 0) r = CW_MIN(r, range);
  t3_color col = { 1, 1, 1 };
  if (cw_node_get_prop(n, "color", &v)) col = (t3_color){ v.v[0], v.v[1], v.v[2] };
  /* irradiance straight below: energy / h^2 (three.js's physical point light, decay 2) */
  if (cw_splats_add(c->s, (t3_vec3){ p.x, c->ground, p.z }, 2 * r, 2 * r, 0, 0, col, e / (h * h) * c->scale)) c->added++;
  return true;
}

int cw_splats_add_lights(cw_splats *s, cw_node root, float ground_y, float scale) {
  if (!s || s->kind != CW_SPLATS_LIGHT) return 0;
  lights_ctx c = { s, ground_y, scale, 0 };
  cw_node_walk(root, light_fn, &c);
  return c.added;
}
