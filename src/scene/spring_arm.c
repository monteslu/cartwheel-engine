/* SpringArm3D (cartwheel/gameplay.h): a camera boom that shortens when something is in the way. */
#include <math.h>
#include <string.h>

#include "cartwheel/gameplay.h"
#include "cartwheel/physics_nodes.h"

typedef struct spring_arm {
  float spring_length, margin, radius, extend_speed;
  int32_t collision_mask;
  bool exclude_parent;
  float hit_length;
  bool started;
} spring_arm;

static cw_node_type arm_type;

float cw_spring_arm_length(cw_node n) {
  spring_arm *a = cw_node_data_t(n, &arm_type);
  return a ? a->hit_length : 0;
}

static void place_children(cw_node n, float length, bool snap) {
  for (int i = 0; i < cw_node_child_count(n); i++) {
    cw_node c = cw_node_child(n, i);
    cw_node3d *d = cw_node_data(c, "Node3D");
    if (!d) continue;
    d->interpolate = true;
    if (!snap) cw_node3d_snapshot(c); /* drawn smoothly from where it was */
    d->position = (t3_vec3){ 0, 0, length };
    if (snap) cw_node3d_snapshot(c);
  }
}

static float cast(cw_node n, const spring_arm *a) {
  cw_tree *t = cw_node_tree(n);
  const t3_mat4 *m = cw_node3d_global_transform(n);
  if (!t || !m || !(a->spring_length > 0)) return a->spring_length > 0 ? a->spring_length : 0;
  cw_world3d w = cw_physics3d_world(t);
  t3_vec3 o = { m->e[12], m->e[13], m->e[14] };
  t3_vec3 z = { m->e[8], m->e[9], m->e[10] }; /* the arm runs along the node's +Z */
  float zl = sqrtf(z.x * z.x + z.y * z.y + z.z * z.z);
  if (!(zl > 1e-9f)) return a->spring_length;
  t3_vec3 tr = { z.x / zl * a->spring_length, z.y / zl * a->spring_length, z.z / zl * a->spring_length };
  cw_phys_filter f = cw_phys_filter_layers((uint32_t)a->collision_mask);
  if (a->exclude_parent)
    for (cw_node p = cw_node_parent(n); cw_node_valid(p); p = cw_node_parent(p)) {
      cw_body3d b = cw_collision_object3d_body(p);
      if (!CW_HANDLE_IS_NULL(b)) {
        f.ignore_body = b.id;
        break;
      }
    }
  cw_ray_hit3d hit;
  bool got;
  if (a->radius > 0) {
    cw_qshape3d q = cw_qshape3d_sphere(o, a->radius);
    got = cw_world3d_shapecast(w, &q, tr, &f, &hit, 1) > 0;
  } else got = cw_world3d_raycast(w, o, tr, &f, &hit);
  if (!got) return a->spring_length;
  float len = hit.fraction * a->spring_length - a->margin;
  return len < 0 ? 0 : len;
}

static void arm_fixed(cw_node n, void *d, double dt) {
  spring_arm *a = d;
  float want = cast(n, a);
  bool snap = !a->started;
  /* shorten at once (never show the inside of a wall); lengthen at extend_speed when it is set */
  if (a->started && a->extend_speed > 0 && want > a->hit_length) {
    float step = a->hit_length + a->extend_speed * (float)dt;
    want = step < want ? step : want;
  }
  a->started = true;
  a->hit_length = want;
  place_children(n, want, snap);
}
static void arm_init(cw_node n, void *d) {
  (void)n;
  spring_arm *a = d;
  a->spring_length = 4;
  a->margin = 0.05f;
  a->radius = 0.2f;
  a->collision_mask = (int32_t)CW_PHYS_ALL_LAYERS;
  a->exclude_parent = true;
  a->hit_length = 4;
}
/* Children are placed by the fixed tick only (the first one snaps them to the end of the arm): loading or
 * editing a scene never moves them, so a scene saves back as it was written. */
static void arm_enter(cw_node n, void *d) {
  (void)n;
  spring_arm *a = d;
  a->started = false;
  a->hit_length = a->spring_length;
}
#define AF(field, lo, hi, docs) CW_PROP_FIELD_RANGE(spring_arm, field, CW_T_FLOAT, lo, hi, docs)
static const cw_prop arm_props[] = {
  AF(spring_length, 0, 1e4, "The arm's full length, along the node's +Z."),
  AF(margin, 0, 10, "How far short of what it hit the arm stops."),
  AF(radius, 0, 10, "Radius of the sphere swept along the arm (0 = a ray): keep it near the camera's near plane."),
  AF(extend_speed, 0, 1e4, "Meters a second the arm grows back at once the way is clear (0 = at once). It always shortens at once."),
  CW_PROP_FIELD(spring_arm, collision_mask, CW_T_INT, CW_PROP_DEFAULT, "Layers that shorten the arm (bits)."),
  CW_PROP_FIELD(spring_arm, exclude_parent, CW_T_BOOL, CW_PROP_DEFAULT, "Ignore the nearest body above the arm in the tree (the character it follows)."),
  CW_PROP_FIELD(spring_arm, hit_length, CW_T_FLOAT, CW_PROP_EDITOR | CW_PROP_READONLY, "The arm's length now."),
  CW_PROP_END,
};
static cw_node_type arm_type = { .name = "SpringArm3D", .parent = "Node3D", .data_size = sizeof(spring_arm), .props = arm_props,
                                 .doc = "A camera boom: its Node3D children sit at the end of an arm along +Z that shortens when the 3D physics world is in the way (cartwheel/gameplay.h).",
                                 .init = arm_init, .enter_tree = arm_enter, .fixed_update = arm_fixed };

void cw__register_spring_arm_type(void);
void cw__register_spring_arm_type(void) { cw_node_type_register(&arm_type); }
