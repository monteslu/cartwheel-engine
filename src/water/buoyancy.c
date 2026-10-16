/* Buoyancy3D (cartwheel/water.h): floats its parent RigidBody3D on a
 * Water3D. Each fixed tick, before the physics step, every probe point is
 * compared with the water surface over it (cw_water_sample_at at the
 * waves' time, deterministic): a wet probe gets its share of the
 * displaced water's weight, upward, at the probe, and a drag against its
 * velocity relative to the water's own. All of it is + - * / and cw_ math,
 * so the floating is the same on every host. */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "cartwheel/math.h"
#include "cartwheel/physics3d.h"
#include "cartwheel/physics_nodes.h"
#include "water_internal.h"

#define MAX_PROBES 32

typedef struct buoy3d {
  cw_node water;
  const char *probes;
  float volume, probe_depth, fluid_density, linear_drag, angular_drag;
  float submerged; /* read-only */
  /* runtime: the probes as parsed / derived */
  const char *parsed;
  int count;
  t3_vec3 p[MAX_PROBES];
  float vol, depth;
  bool derived;
} buoy3d;

static const cw_node_type *buoy_t;

static void buoy_init(cw_node n, void *data) {
  (void)n;
  buoy3d *b = data;
  b->fluid_density = 1000;
  b->linear_drag = 60;
  b->angular_drag = 40;
}

/* the body's first CollisionShape3D: its bounding box in body space */
static bool shape_box(cw_node body, t3_vec3 *lo, t3_vec3 *hi) {
  for (int i = 0; i < cw_node_child_count(body); i++) {
    cw_node c = cw_node_child(body, i);
    if (!cw_node_is(c, "CollisionShape3D")) continue;
    cw_value shape, v;
    if (!cw_node_get_prop(c, "shape", &shape)) continue;
    t3_vec3 half = { 0.5f, 0.5f, 0.5f };
    const char *nm = NULL;
    const cw_prop *pr = cw_node_find_prop(c, "shape", NULL);
    if (pr && pr->enum_names && shape.i >= 0) nm = pr->enum_names[shape.i];
    if (nm && !strcmp(nm, "BOX") && cw_node_get_prop(c, "size", &v)) half = (t3_vec3){ v.v[0] * 0.5f, v.v[1] * 0.5f, v.v[2] * 0.5f };
    else if (nm && (!strcmp(nm, "SPHERE") || !strcmp(nm, "CAPSULE")) && cw_node_get_prop(c, "radius", &v)) {
      float r = (float)v.f, hh = r;
      cw_value h;
      if (!strcmp(nm, "CAPSULE") && cw_node_get_prop(c, "height", &h)) hh = (float)h.f * 0.5f;
      half = (t3_vec3){ r, hh > r ? hh : r, r };
    }
    t3_vec3 o = cw_node3d_position(c);
    *lo = (t3_vec3){ o.x - half.x, o.y - half.y, o.z - half.z };
    *hi = (t3_vec3){ o.x + half.x, o.y + half.y, o.z + half.z };
    return true;
  }
  return false;
}

static void derive(cw_node n, buoy3d *b) {
  if (b->derived && b->parsed == b->probes) return;
  b->derived = true;
  b->parsed = b->probes;
  b->count = 0;
  cw_node body = cw_node_parent(n);
  t3_vec3 lo = { -0.5f, -0.5f, -0.5f }, hi = { 0.5f, 0.5f, 0.5f };
  bool have = shape_box(body, &lo, &hi);
  if (b->probes && *b->probes) {
    const char *s = b->probes;
    while (*s && b->count < MAX_PROBES) {
      char *e;
      float x = strtof(s, &e);
      if (e == s) break;
      float y = strtof(e, &e), z = strtof(e, &e);
      b->p[b->count++] = (t3_vec3){ x, y, z };
      s = e;
      while (*s == ',' || *s == ' ') s++;
    }
  } else {
    /* the box's eight corners, pulled in to 90% */
    t3_vec3 c = { (lo.x + hi.x) * 0.5f, (lo.y + hi.y) * 0.5f, (lo.z + hi.z) * 0.5f };
    for (int k = 0; k < 8; k++) {
      float x = k & 1 ? hi.x : lo.x, y = k & 2 ? hi.y : lo.y, z = k & 4 ? hi.z : lo.z;
      b->p[b->count++] = (t3_vec3){ c.x + (x - c.x) * 0.9f, c.y + (y - c.y) * 0.9f, c.z + (z - c.z) * 0.9f };
    }
  }
  float bv = (hi.x - lo.x) * (hi.y - lo.y) * (hi.z - lo.z);
  b->vol = b->volume > 0 ? b->volume : have ? bv : 1;
  b->depth = b->probe_depth > 0 ? b->probe_depth : (hi.y - lo.y) * 0.25f;
  if (b->depth < 0.01f) b->depth = 0.01f;
}

static t3_vec3 qrot(t3_quat q, t3_vec3 v) {
  /* v + 2 w (u x v) + 2 u x (u x v): + - * only */
  t3_vec3 u = { q.x, q.y, q.z };
  t3_vec3 t = { 2 * (u.y * v.z - u.z * v.y), 2 * (u.z * v.x - u.x * v.z), 2 * (u.x * v.y - u.y * v.x) };
  return (t3_vec3){ v.x + q.w * t.x + (u.y * t.z - u.z * t.y), v.y + q.w * t.y + (u.z * t.x - u.x * t.z),
                    v.z + q.w * t.z + (u.x * t.y - u.y * t.x) };
}

static void buoy_fixed(cw_node n, void *data, double dt) {
  (void)dt;
  buoy3d *b = data;
  b->submerged = 0;
  cw_node body_node = cw_node_parent(n);
  cw_body3d body = cw_collision_object3d_body(body_node);
  if (!cw_body3d_valid(body) || cw_body3d_type(body) != CW_BODY_DYNAMIC) return;
  cw_tree *t = cw_node_tree(n);
  cw_node water = cw_node_valid(b->water) ? b->water : cw_water_find(t);
  if (!cw_node_valid(water)) return;
  derive(n, b);
  if (!b->count) return;
  double time = cw_water_time(water);
  cw_xform3 x = cw_body3d_transform(body);
  t3_vec3 v = cw_body3d_velocity(body), w = cw_body3d_angular_velocity(body);
  t3_vec3 g = cw_physics_nodes_get_config()->gravity_3d;
  float gm = sqrtf(g.x * g.x + g.y * g.y + g.z * g.z);
  float share = b->fluid_density * gm * b->vol / (float)b->count;
  float wet_sum = 0;
  for (int i = 0; i < b->count; i++) {
    t3_vec3 r = qrot(x.q, b->p[i]);
    t3_vec3 p = { x.p.x + r.x, x.p.y + r.y, x.p.z + r.z };
    cw_water_sample s;
    if (!cw_water_sample_at(water, p.x, p.z, time, &s)) continue;
    float wet = (s.height - p.y) / b->depth;
    if (wet <= 0) continue;
    if (wet > 1) wet = 1;
    wet_sum += wet;
    /* the probe point's velocity relative to the water's */
    t3_vec3 pv = { v.x + (w.y * r.z - w.z * r.y) - s.velocity.x, v.y + (w.z * r.x - w.x * r.z) - s.velocity.y,
                   v.z + (w.x * r.y - w.y * r.x) - s.velocity.z };
    float k = b->linear_drag * wet;
    t3_vec3 f = { -pv.x * k, share * wet - pv.y * k, -pv.z * k };
    cw_body3d_apply_force(body, f, &p);
  }
  b->submerged = wet_sum / (float)b->count;
  if (b->submerged > 0) {
    float k = b->angular_drag * b->submerged;
    cw_body3d_apply_torque(body, (t3_vec3){ -w.x * k, -w.y * k, -w.z * k });
  }
}

static void buoy_prop_changed(cw_node n, void *data, const cw_prop *p) {
  (void)n, (void)p;
  ((buoy3d *)data)->derived = false;
}

static const cw_prop buoy_props[] = {
  CW_PROP_FIELD(buoy3d, water, CW_T_NODE, CW_PROP_DEFAULT, "The Water3D (empty: the tree's first)."),
  CW_PROP_FIELD(buoy3d, probes, CW_T_STRING, CW_PROP_DEFAULT, "\"x y z, ...\" in the body's space (empty: its box's corners)."),
  CW_PROP_FIELD_RANGE(buoy3d, volume, CW_T_FLOAT, 0, 1e9, "m^3 displaced when fully under (0: the box's volume)."),
  CW_PROP_FIELD_RANGE(buoy3d, probe_depth, CW_T_FLOAT, 0, 1e6, "Metres from dry to fully under, per probe (0: a quarter of the box's height)."),
  CW_PROP_FIELD_RANGE(buoy3d, fluid_density, CW_T_FLOAT, 0, 1e6, "kg/m^3 (1000: water)."),
  CW_PROP_FIELD_RANGE(buoy3d, linear_drag, CW_T_FLOAT, 0, 1e9, "N per m/s per fully wet probe, against the motion through the water."),
  CW_PROP_FIELD_RANGE(buoy3d, angular_drag, CW_T_FLOAT, 0, 1e9, "N*m per rad/s, times the wet share."),
  { .name = "submerged", .type = CW_T_FLOAT, .flags = CW_PROP_EDITOR | CW_PROP_READONLY, .offset = offsetof(buoy3d, submerged),
    .size = sizeof(float), .doc = "The probes' wet share last tick, 0..1." },
  CW_PROP_END,
};

static cw_node_type buoy_type = {
  .name = "Buoyancy3D", .parent = "Node3D", .data_size = sizeof(buoy3d), .props = buoy_props,
  .doc = "Floats its parent RigidBody3D on a Water3D (water.h).",
  .init = buoy_init, .fixed_update = buoy_fixed, .prop_changed = buoy_prop_changed,
};

void cw__register_buoyancy_type(void) {
  cw_node_type_register(&buoy_type);
  buoy_t = &buoy_type;
}

static bool count_wet(cw_node n, void *u) {
  buoy3d *b = cw_node_data_t(n, buoy_t);
  if (b && b->submerged > 0) (*(int *)u)++;
  return true;
}

int cw__buoyancy_count(cw_tree *t) {
  int c = 0;
  if (t) cw_node_walk(cw_tree_root(t), count_wet, &c);
  return c;
}
