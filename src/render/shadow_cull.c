/* Shadow caster culling (cartwheel/quality.h, the Shadows setting): for one
 * view's render, a mesh whose bounding sphere is small on screen (radius over
 * its distance to the camera under the setting's share) casts no shadow. Its
 * cast_shadow is cleared for that render and put back after it, so every
 * shadow map of the frame (three.c's own, the sun's cascades, the clustered
 * lights' atlas) skips it and nothing else changes. Render only. */
#include <math.h>
#include <stdlib.h>

#include "cartwheel/core.h"
#include "cartwheel/debug.h"
#include "three.h"
#include "quality_internal.h"
#include "shadow_cull.h"

static struct {
  t3_object **o;
  int n, cap;
  float k;
  t3_vec3 eye;
  int32_t culled, kept; /* the last view's (debug fields shadow.culled, shadow.casters) */
  bool fields;
  /* CW_SHADOW_CONTACTS=1 (native, tests/quality-grounding.mjs): each caster's base on screen, the last view's */
  int record; /* -1 not read yet */
  t3_mat4 view, proj;
  struct contact { float x, y, cx, cy, size; bool culled, front, cfront, around, along; } contacts[512];
  int n_contacts;
} C = { .record = -1 };

/* a point of the world on screen, 0..1 from the top left; false behind the camera */
static bool on_screen(t3_vec3 p, float *x, float *y) {
  t3_vec3 v = t3_vec3_apply_mat4(p, &C.view);
  t3_vec3 n = t3_vec3_apply_mat4(v, &C.proj);
  *x = n.x * 0.5f + 0.5f, *y = 0.5f - n.y * 0.5f;
  return v.z < 0;
}
/* the caster's base on screen: the lowest point under the middle of its world box. Two kinds of caster have no base
 * there (the middle of the box is where nothing of it stands):
 *   around: its box stands around the camera (the camera above its footprint: a ring of walls, a floor, a merged
 *     set of stands); it has no single base and the grounding gate leaves it out;
 *   along: a long caster (its footprint more than 6 times as long as it is wide and as it is tall: a wall, a fence,
 *     a row of stands) stands on a line; its base is the point of that line nearest the camera.
 * cx, cy keep the box's middle (the old rule) for the gate's control. */
static void contact(t3_object *o, t3_geometry *g, float size, bool culled) {
  if (C.n_contacts >= (int)CW_ARRAY_LEN(C.contacts)) return;
  if (!g->has_bounding_box) t3_geometry_compute_bounding_box(g);
  t3_vec3 lo = { 1e30f, 1e30f, 1e30f }, hi = { -1e30f, -1e30f, -1e30f };
  for (int k = 0; k < 8; k++) {
    t3_vec3 c = { k & 1 ? g->bounding_box.max.x : g->bounding_box.min.x, k & 2 ? g->bounding_box.max.y : g->bounding_box.min.y,
                  k & 4 ? g->bounding_box.max.z : g->bounding_box.min.z };
    c = t3_vec3_apply_mat4(c, &o->matrix_world);
    lo.x = fminf(lo.x, c.x), lo.y = fminf(lo.y, c.y), lo.z = fminf(lo.z, c.z);
    hi.x = fmaxf(hi.x, c.x), hi.y = fmaxf(hi.y, c.y), hi.z = fmaxf(hi.z, c.z);
  }
  struct contact *c = &C.contacts[C.n_contacts++];
  c->size = size, c->culled = culled;
  c->around = C.eye.x >= lo.x && C.eye.x <= hi.x && C.eye.z >= lo.z && C.eye.z <= hi.z;
  float dx = hi.x - lo.x, dz = hi.z - lo.z;
  c->along = !c->around && fmaxf(dx, dz) > 6 * fmaxf(fmaxf(fminf(dx, dz), hi.y - lo.y), 0.01f);
  t3_vec3 mid = { (lo.x + hi.x) * 0.5f, lo.y, (lo.z + hi.z) * 0.5f };
  c->cfront = on_screen(mid, &c->cx, &c->cy);
  t3_vec3 base = c->along ? (t3_vec3){ CW_CLAMP(C.eye.x, lo.x, hi.x), lo.y, CW_CLAMP(C.eye.z, lo.z, hi.z) } : mid;
  c->front = on_screen(base, &c->x, &c->y);
}

static void cmd_contacts(int argc, const char **argv, cw_reply *r) {
  (void)argc, (void)argv;
  cw_reply_printf(r, "{\"shadow_contacts\":[");
  for (int i = 0; i < C.n_contacts; i++) {
    const struct contact *c = &C.contacts[i];
    cw_reply_printf(r, "%s{\"x\":%.4f,\"y\":%.4f,\"size\":%.4f,\"culled\":%s,\"front\":%s,\"around\":%s,\"along\":%s,\"cx\":%.4f,\"cy\":%.4f,\"cfront\":%s}",
                    i ? "," : "", (double)c->x, (double)c->y, (double)c->size, c->culled ? "true" : "false", c->front ? "true" : "false",
                    c->around ? "true" : "false", c->along ? "true" : "false", (double)c->cx, (double)c->cy, c->cfront ? "true" : "false");
  }
  cw_reply_printf(r, "]}");
}

static float max_scale(const t3_mat4 *m) {
  float s = 0;
  for (int k = 0; k < 3; k++) {
    const float *c = &m->e[k * 4];
    float l = c[0] * c[0] + c[1] * c[1] + c[2] * c[2];
    if (l > s) s = l;
  }
  return sqrtf(s);
}

static void walk(t3_object *o) {
  if (!o->visible) return;
  if ((o->type == T3_MESH || o->type == T3_SKINNED_MESH) && o->cast_shadow) {
    t3_geometry *g = ((t3_mesh *)o)->geometry;
    if (g && g->attributes[T3_ATTR_POSITION]) {
      if (!g->has_bounding_sphere) t3_geometry_compute_bounding_sphere(g);
      t3_vec3 c = t3_vec3_apply_mat4(g->bounding_sphere.center, &o->matrix_world);
      float r = g->bounding_sphere.radius * max_scale(&o->matrix_world);
      float d = t3_vec3_distance(c, C.eye);
      if (C.record > 0) contact(o, g, d > 0 ? r / d : 1, r < C.k * d);
      if (r < C.k * d) {
        if (C.n == C.cap) {
          C.cap = C.cap ? C.cap * 2 : 256;
          C.o = realloc(C.o, (size_t)C.cap * sizeof *C.o);
          if (!C.o) abort();
        }
        C.o[C.n++] = o;
        o->cast_shadow = false;
      } else C.kept++;
    }
  } else if (o->type == T3_INSTANCED_MESH && o->cast_shadow) C.kept++; /* a whole set of instances: kept */
  for (int i = 0; i < o->child_count; i++) walk(o->children[i]);
}

void cw__shadow_cull_begin(t3_scene *scene, const t3_camera *cam) {
  if (!C.fields) {
    C.fields = true;
    cw_debug_i32("shadow.culled", &C.culled);
    cw_debug_i32("shadow.casters", &C.kept);
    cw_debug_command("shadow_contacts", "shadow_contacts: each shadow caster's base on screen in the last view (CW_SHADOW_CONTACTS=1), its size (radius / distance), whether the Shadows setting culled it and whether it stands around the camera (no single base) or along a line (its base the line's point nearest the camera), and the old base under its box's middle (cx, cy)", cmd_contacts);
  }
  if (C.record < 0) {
#if !CW_CART
    const char *e = getenv("CW_SHADOW_CONTACTS");
    C.record = e && atoi(e) > 0;
#else
    C.record = 0;
#endif
  }
  C.n_contacts = 0;
  C.n = 0;
  C.kept = 0;
  C.culled = 0;
  C.k = cw__quality_caster_min();
  if (!scene || !cam || (C.k <= 0 && C.record <= 0)) return;
  C.eye = t3_mat4_get_position(&cam->base.matrix_world);
  C.view = cam->matrix_world_inverse;
  C.proj = cam->projection_matrix;
  walk(&scene->base);
  C.culled = C.n;
}

void cw__shadow_cull_end(void) {
  for (int i = 0; i < C.n; i++) C.o[i]->cast_shadow = true;
  C.n = 0;
}
