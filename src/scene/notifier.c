/* VisibleOnScreenNotifier3D / 2D (cartwheel/gameplay.h): a box (or rectangle) tested against the camera's view
 * once per fixed tick; screen_entered / screen_exited when the answer changes. */
#include <math.h>
#include <string.h>

#include "cartwheel/gameplay.h"
#include "cartwheel/math.h"
#include "cartwheel/platform.h"
#include "cartwheel/render3d.h"
#include "cartwheel/scene2d.h"

typedef struct notifier3d {
  t3_vec3 center, size;
  bool on_screen;
} notifier3d;
typedef struct notifier2d {
  cw_vec2 center, size;
  bool on_screen;
} notifier2d;

static cw_node_type n3_type, n2_type;
static const cw_node_type *node3d_t, *node2d_t;
static const char *s_entered, *s_exited;

/* A camera's view volume, ready to test boxes against: its world matrix and the sine and cosine of half its
 * fov. The side planes come from the fov and the picture's aspect through cw_sincos, never from the projection
 * matrix (which the renderer builds with libm), so every host answers alike. */
typedef struct view3d {
  bool valid;
  float cam[16], sn, cs, aspect, znear, zfar;
} view3d;

static void view3d_from(cw_node camera, view3d *v) {
  v->valid = false;
  const t3_mat4 *cm = cw_node_valid(camera) ? cw_node3d_global_transform(camera) : NULL;
  if (!cm) return;
  cw_value p;
  float fov = 70;
  v->znear = 0.05f, v->zfar = 1000;
  if (cw_node_get_prop(camera, "fov", &p)) fov = (float)p.f;
  if (cw_node_get_prop(camera, "near", &p)) v->znear = (float)p.f;
  if (cw_node_get_prop(camera, "far", &p)) v->zfar = (float)p.f;
  int w = 0, h = 0;
  cw_render_size(&w, &h);
  v->aspect = h > 0 ? (float)w / (float)h : 1;
  cw_sincos(fov * 0.5f * (CW_PI / 180.0f), &v->sn, &v->cs);
  memcpy(v->cam, cm->e, sizeof v->cam);
  v->valid = true;
}

/* The box against the view volume, in camera space: outside when all eight corners are beyond one of the six
 * planes (the usual conservative test: a box near a corner of the view may count as on screen). */
static bool view3d_sees(const view3d *v, const t3_mat4 *box_world, t3_vec3 center, t3_vec3 size) {
  if (!v->valid || !box_world) return false;
  /* camera axes and position (the world matrix's columns); a camera is not scaled */
  const float *c = v->cam, *b = box_world->e;
  int out[6] = { 0 };
  for (int i = 0; i < 8; i++) {
    float lx = center.x + (i & 1 ? 0.5f : -0.5f) * size.x, ly = center.y + (i & 2 ? 0.5f : -0.5f) * size.y,
          lz = center.z + (i & 4 ? 0.5f : -0.5f) * size.z;
    float wx = b[0] * lx + b[4] * ly + b[8] * lz + b[12] - c[12], wy = b[1] * lx + b[5] * ly + b[9] * lz + b[13] - c[13],
          wz = b[2] * lx + b[6] * ly + b[10] * lz + b[14] - c[14];
    float px = wx * c[0] + wy * c[1] + wz * c[2], py = wx * c[4] + wy * c[5] + wz * c[6];
    float pz = -(wx * c[8] + wy * c[9] + wz * c[10]); /* depth in front of the camera */
    float lim_y = pz * v->sn, lim_x = lim_y * v->aspect; /* |y| cos <= depth sin; |x| cos <= depth sin aspect */
    if (pz < v->znear) out[0]++;
    if (pz > v->zfar) out[1]++;
    if (py * v->cs > lim_y) out[2]++;
    if (py * v->cs < -lim_y) out[3]++;
    if (px * v->cs > lim_x) out[4]++;
    if (px * v->cs < -lim_x) out[5]++;
  }
  for (int k = 0; k < 6; k++)
    if (out[k] == 8) return false;
  return true;
}

bool cw_view3d_sees_box(cw_node camera, const t3_mat4 *box_world, t3_vec3 center, t3_vec3 size) {
  view3d v;
  view3d_from(camera, &v);
  return view3d_sees(&v, box_world, center, size);
}

/* The tree's current camera is found and measured once per fixed tick, however many notifiers ask: a hook
 * before the nodes' fixed_update marks the tree's view stale, the first notifier of the tick rebuilds it. */
typedef struct tree_view {
  bool fresh;
  view3d view;
} tree_view;
static void stale_hook(cw_tree *t, double dt, void *user) {
  (void)dt, (void)user;
  tree_view *tv = cw_tree_slot(t, "notifier_view");
  if (tv) tv->fresh = false;
}
static const view3d *tree_view3d(cw_tree *t) {
  tree_view *tv = cw_tree_slot(t, "notifier_view");
  if (!tv) {
    tv = cw_calloc(1, sizeof *tv, CW_MEM_SCENE);
    cw_tree_set_slot(t, "notifier_view", tv, cw_free);
    cw_tree_add_hook(t, 0, -50, stale_hook, NULL);
  }
  if (!tv->fresh) {
    view3d_from(cw_render3d_camera(t), &tv->view);
    tv->fresh = true;
  }
  return &tv->view;
}

bool cw_view2d_sees_rect(cw_tree *t, const float *xform, cw_vec2 center, cw_vec2 size) {
  cw_rect vp;
  if (!t || !xform || !cw_canvas_viewport(t, &vp)) return false;
  float lo_x = 0, lo_y = 0, hi_x = 0, hi_y = 0;
  for (int i = 0; i < 4; i++) {
    float lx = center.x + (i & 1 ? 0.5f : -0.5f) * size.x, ly = center.y + (i & 2 ? 0.5f : -0.5f) * size.y;
    cw_vec2 s = cw_canvas_world_to_screen(t, cw_v2(xform[0] * lx + xform[2] * ly + xform[4], xform[1] * lx + xform[3] * ly + xform[5]));
    if (!i || s.x < lo_x) lo_x = s.x;
    if (!i || s.x > hi_x) hi_x = s.x;
    if (!i || s.y < lo_y) lo_y = s.y;
    if (!i || s.y > hi_y) hi_y = s.y;
  }
  return hi_x >= vp.x && lo_x <= vp.x + vp.w && hi_y >= vp.y && lo_y <= vp.y + vp.h;
}

bool cw_notifier_is_on_screen(cw_node n) {
  notifier3d *a = cw_node_data_t(n, &n3_type);
  if (a) return a->on_screen;
  notifier2d *b = cw_node_data_t(n, &n2_type);
  return b && b->on_screen;
}

static void change(cw_node n, bool *state, bool now) {
  if (*state == now) return;
  *state = now;
  cw_emit(n, now ? s_entered : s_exited, NULL, 0);
}
static void n3_fixed(cw_node n, void *d, double dt) {
  (void)dt;
  notifier3d *a = d;
  cw_tree *t = cw_node_tree(n);
  /* hidden by itself or an ancestor: off screen (cw_node_visible_in_tree) */
  change(n, &a->on_screen, t && cw_node_visible_in_tree(n) && view3d_sees(tree_view3d(t), cw_node3d_global_transform(n), a->center, a->size));
}
static void n2_fixed(cw_node n, void *d, double dt) {
  (void)dt;
  notifier2d *a = d;
  change(n, &a->on_screen, cw_node_visible_in_tree(n) && cw_view2d_sees_rect(cw_node_tree(n), cw_node2d_global_transform(n), a->center, a->size));
}
static void n3_exit(cw_node n, void *d) {
  change(n, &((notifier3d *)d)->on_screen, false); /* leaving the tree leaves the screen */
}
static void n2_exit(cw_node n, void *d) { change(n, &((notifier2d *)d)->on_screen, false); }
static void n3_init(cw_node n, void *d) {
  (void)n;
  ((notifier3d *)d)->size = (t3_vec3){ 2, 2, 2 };
}
static void n2_init(cw_node n, void *d) {
  (void)n;
  ((notifier2d *)d)->size = cw_v2(20, 20);
}

static const char *const notifier_signals[] = { "screen_entered", "screen_exited", NULL };
static const cw_prop n3_props[] = {
  CW_PROP_FIELD(notifier3d, center, CW_T_VEC3, CW_PROP_DEFAULT, "The box's centre in the node's space."),
  CW_PROP_FIELD(notifier3d, size, CW_T_VEC3, CW_PROP_DEFAULT, "The box's full size in the node's space."),
  CW_PROP_FIELD(notifier3d, on_screen, CW_T_BOOL, CW_PROP_EDITOR | CW_PROP_READONLY, "Inside the current camera's view as of the last fixed tick."),
  CW_PROP_END,
};
static const cw_prop n2_props[] = {
  CW_PROP_FIELD(notifier2d, center, CW_T_VEC2, CW_PROP_DEFAULT, "The rectangle's centre in the node's space."),
  CW_PROP_FIELD(notifier2d, size, CW_T_VEC2, CW_PROP_DEFAULT, "The rectangle's full size in the node's space."),
  CW_PROP_FIELD(notifier2d, on_screen, CW_T_BOOL, CW_PROP_EDITOR | CW_PROP_READONLY, "Inside the view the last frame drew, as of the last fixed tick."),
  CW_PROP_END,
};
static cw_node_type n3_type = { .name = "VisibleOnScreenNotifier3D", .parent = "Node3D", .data_size = sizeof(notifier3d), .props = n3_props,
                                .signals = notifier_signals,
                                .doc = "Emits screen_entered / screen_exited as its box enters and leaves the current camera's view (cartwheel/gameplay.h).",
                                .init = n3_init, .exit_tree = n3_exit, .fixed_update = n3_fixed };
static cw_node_type n2_type = { .name = "VisibleOnScreenNotifier2D", .parent = "Node2D", .data_size = sizeof(notifier2d), .props = n2_props,
                                .signals = notifier_signals,
                                .doc = "Emits screen_entered / screen_exited as its rectangle enters and leaves the 2D view (cartwheel/gameplay.h).",
                                .init = n2_init, .exit_tree = n2_exit, .fixed_update = n2_fixed };

void cw__register_notifier_types(void);
void cw__register_notifier_types(void) {
  s_entered = cw_intern("screen_entered");
  s_exited = cw_intern("screen_exited");
  node3d_t = cw_node_type_find("Node3D");
  node2d_t = cw_node_type_find("Node2D");
  cw_node_type_register(&n3_type);
  cw_node_type_register(&n2_type);
}
