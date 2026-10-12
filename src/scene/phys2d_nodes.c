/* 2D physics nodes over the Box2D wrapper (cartwheel/physics_nodes.h).
 * Mirrored step for step by phys3d_nodes.c.
 *
 * Nodes are canvas pixels, y down; the world is meters, y up:
 *   world = (x / ppm, -y / ppm), rotation (cos a, -sin a) for a node angle a.
 * Simulation code: + - * / sqrt and cartwheel/math.h only. */
#include <stdio.h>
#include <stdlib.h>

#include "phys_nodes_internal.h"
#include "cartwheel/debug.h"
#include "cartwheel/scene2d.h"

enum { K_STATIC, K_KINEMATIC, K_RIGID, K_CHARACTER, K_AREA };
enum { S_RECT, S_CIRCLE, S_CAPSULE, S_POLYGON, S_SEGMENT, S_CHAIN };

/* ── per-tree world ─────────────────────────────────────────────────── */

typedef struct dead_shape {
  uint64_t shape;
  int32_t step;
} dead_shape;

typedef struct p2w {
  cw_tree *tree;
  cw_world2d world;
  float ppm;
  CW_ARRAY(cw_node) objects; /* collision objects, enter order */
  CW_ARRAY(cw_node) joints, casts;
  cw_map shape_owner;        /* shape id -> owner node id (kept 2 steps after the shape dies) */
  cw_map body_owner;         /* body id -> node id */
  cw_map joint_owner;        /* joint id -> node id */
  CW_ARRAY(dead_shape) dead;
  cw__psig_list sigs;
  cw_body2d ground;          /* static body for joints with no node_a */
  int32_t steps;
  struct p2w *next_live; /* every tree's world, for cw_physics2d_body_node */
} p2w;
static p2w *live_worlds;

static const cw_node_type *co_t, *pb_t, *static_t, *kin_t, *rigid_t, *char_t, *area_t, *shape_t, *joint_t, *ray_t,
  *scast_t, *node2d_t;

static void p2w_free(void *p) {
  p2w *W = p;
  for (p2w **l = &live_worlds; *l; l = &(*l)->next_live)
    if (*l == W) {
      *l = W->next_live;
      break;
    }
  if (cw_world2d_valid(W->world)) cw_world2d_destroy(W->world);
  cw_array_free(&W->objects);
  cw_array_free(&W->joints);
  cw_array_free(&W->casts);
  cw_map_free(&W->shape_owner);
  cw_map_free(&W->body_owner);
  cw_map_free(&W->joint_owner);
  cw_array_free(&W->dead);
  cw_array_free(&W->sigs);
  cw_free(W);
}

static void pre_hook(cw_tree *t, double dt, void *u);
static void post_hook(cw_tree *t, double dt, void *u);

static p2w *world_of(cw_tree *t) {
  if (!t) return NULL;
  p2w *W = cw_tree_slot(t, "physics2d");
  if (W) return W;
  const cw_physics_nodes_config *c = cw_physics_nodes_get_config();
  W = cw_calloc(1, sizeof *W, CW_MEM_PHYSICS);
  W->tree = t;
  W->ppm = c->pixels_per_meter;
  cw_world2d_def wd = cw_world2d_default_def();
  wd.gravity = cw_v2(c->gravity_2d.x / W->ppm, -c->gravity_2d.y / W->ppm);
  wd.substeps = c->substeps;
  for (int i = 0; i < CW_PHYS_LAYERS; i++) wd.layers[i] = c->layers_2d[i];
  W->world = cw_world2d_create(&wd);
  cw_map_init(&W->shape_owner, CW_MEM_PHYSICS);
  cw_map_init(&W->body_owner, CW_MEM_PHYSICS);
  cw_map_init(&W->joint_owner, CW_MEM_PHYSICS);
  cw_tree_set_slot(t, "physics2d", W, p2w_free);
  W->next_live = live_worlds;
  live_worlds = W;
  cw_tree_add_hook(t, 0, -100, pre_hook, NULL);
  cw_tree_add_hook(t, 0, 100, post_hook, NULL);
  return W;
}

cw_world2d cw_physics2d_world(cw_tree *t) {
  p2w *W = world_of(t);
  return W ? W->world : (cw_world2d){ 0 };
}
float cw_physics2d_ppm(cw_tree *t) {
  /* no world yet: the value it will be created with (asking creates nothing) */
  p2w *W = t ? cw_tree_slot(t, "physics2d") : NULL;
  return W ? W->ppm : cw_physics_nodes_get_config()->pixels_per_meter;
}

static void list_remove(cw_node *data, int *count, cw_node n) {
  for (int i = 0; i < *count; i++)
    if (data[i].id == n.id) {
      memmove(data + i, data + i + 1, sizeof(cw_node) * (size_t)(*count - i - 1));
      (*count)--;
      return;
    }
}

static cw_node node_by_id(uint64_t id) { return (cw_node){ id }; }
static cw_node owner_of_shape(p2w *W, cw_shape2d s) {
  uint64_t v;
  return cw_map_get(&W->shape_owner, s.id, &v) ? node_by_id(v) : (cw_node){ 0 };
}

/* ── conversions ────────────────────────────────────────────────────── */

static cw_vec2 to_m(const p2w *W, cw_vec2 px) { return cw_v2(px.x / W->ppm, -px.y / W->ppm); }
static cw_vec2 to_px(const p2w *W, cw_vec2 m) { return cw_v2(m.x * W->ppm, -m.y * W->ppm); }

/* a node's global transform as a physics transform (scale dropped) */
static cw_xform2 node_xform(const p2w *W, cw_node n) {
  const float *m = cw_node2d_global_transform(n);
  float len = sqrtf(m[0] * m[0] + m[1] * m[1]);
  cw_xform2 x;
  x.p = to_m(W, cw_v2(m[4], m[5]));
  x.q = len > 0 ? (cw_rot2){ m[0] / len, -m[1] / len } : (cw_rot2){ 1, 0 };
  return x;
}

/* ── data ───────────────────────────────────────────────────────────── */

typedef struct co2d {
  int collision_layer;
  int32_t collision_mask;
  /* runtime */
  int kind;
  cw_body2d body;
  CW_ARRAY(uint64_t) shapes;
  CW_ARRAY(uint64_t) chains;
  uint32_t cfg_hash;
  bool dirty;
  cw_vec2 synced_pos; /* node LOCAL position/rotation last written or pushed (rigid, character) */
  float synced_rot;
  float synced_g[6];  /* node GLOBAL transform last pushed (static, kinematic, area) */
  cw_map pairs;       /* other node id -> overlapping shape pairs */
  int32_t overlap_count;
} co2d;

typedef struct pb2d {
  float friction, bounce, rolling_resistance, tangent_speed;
  int32_t user_material;
  bool contact_monitor, hit_events;
} pb2d;

typedef struct rigid2d {
  float density, mass, gravity_scale, linear_damp, angular_damp;
  bool lock_rotation, bullet, can_sleep, freeze;
  int freeze_mode;
  cw_vec2 init_velocity;
  float init_angular;
} rigid2d;

typedef struct char2d {
  cw_vec2 velocity;
  float floor_max_angle, floor_snap_length, step_height;
  int max_slides;
  float push_mass;
  bool ride_platforms;
  /* outputs */
  bool on_floor, on_wall, on_ceiling;
  cw_vec2 floor_normal, wall_normal, platform_velocity;
  /* runtime */
  cw_mover2d mover;
  bool mover_ready;
  uint32_t mover_hash;
  cw_vec2 feet; /* feet offset from the node origin, pixels (body-local) */
} char2d;

typedef struct area2d {
  bool monitoring, monitorable;
} area2d;

typedef struct shape2d {
  int shape;
  cw_vec2 size;
  float radius, height;
  const char *points;
  bool loop;
  bool disabled;
} shape2d;

/* ── shapes ─────────────────────────────────────────────────────────── */

static cw_node parent_object(cw_node shape) {
  cw_node p = cw_node_parent(shape);
  return cw_node_data_t(p, co_t) ? p : (cw_node){ 0 };
}

static void mark_dirty(cw_node obj) {
  co2d *c = cw_node_data_t(obj, co_t);
  if (c) c->dirty = true;
}

/* hash of everything that shapes the body's colliders */
static uint32_t config_hash(cw_node n, co2d *c) {
  uint32_t h = CW_HASH32_INIT;
  h = cw_hash32(&c->collision_layer, sizeof c->collision_layer, h);
  h = cw_hash32(&c->collision_mask, sizeof c->collision_mask, h);
  pb2d *pb = cw_node_data_t(n, pb_t);
  if (pb) h = cw_hash32(pb, sizeof *pb, h);
  rigid2d *rb = cw_node_data_t(n, rigid_t);
  if (rb) h = cw_hash32(&rb->density, sizeof rb->density, h), h = cw_hash32(&rb->mass, sizeof rb->mass, h);
  area2d *ar = cw_node_data_t(n, area_t);
  if (ar) h = cw_hash32(ar, sizeof *ar, h);
  for (int i = 0; i < cw_node_child_count(n); i++) {
    cw_node ch = cw_node_child(n, i);
    shape2d *s = cw_node_data_t(ch, shape_t);
    if (!s) continue;
    cw_node2d *d = cw_node_data_t(ch, node2d_t);
    h = cw_hash32(s, sizeof *s, h);
    h = cw_hash32(&d->position, sizeof d->position, h);
    h = cw_hash32(&d->rotation, sizeof d->rotation, h);
    h = cw_hash32(&d->scale, sizeof d->scale, h);
  }
  return h;
}

static void own_shape(p2w *W, co2d *c, cw_node n, cw_shape2d s) {
  if (CW_HANDLE_IS_NULL(s)) return;
  cw_array_push(&c->shapes, s.id);
  cw_map_set(&W->shape_owner, s.id, n.id);
}

static void destroy_shapes(p2w *W, co2d *c) {
  for (int i = 0; i < c->shapes.count; i++) {
    cw_shape2d s = { c->shapes.data[i] };
    if (cw_shape2d_valid(s)) cw_shape2d_destroy(s);
    dead_shape d = { s.id, W->steps };
    cw_array_push(&W->dead, d);
  }
  for (int i = 0; i < c->chains.count; i++) cw_chain2d_destroy((cw_chain2d){ c->chains.data[i] });
  cw_array_clear(&c->shapes);
  cw_array_clear(&c->chains);
}

/* the shape's local transform relative to its body, applied to a point (px) */
static cw_vec2 shape_point(const cw_xform2d *L, cw_vec2 p) { return cw_xform2d_apply(L, p); }

static cw_xform2d shape_local(cw_node2d *d) {
  float s, cs;
  cw_sincos(d->rotation, &s, &cs);
  return (cw_xform2d){ cs * d->scale.x, s * d->scale.x, -s * d->scale.y, cs * d->scale.y, d->position.x, d->position.y };
}

static float signed_area(const cw_vec2 *p, int n) {
  float a = 0;
  for (int i = 0; i < n; i++) a += cw_v2_cross(p[i], p[(i + 1) % n]);
  return a * 0.5f;
}

static void build_shapes(p2w *W, cw_node n, co2d *c) {
  destroy_shapes(W, c);
  cw_shape2d_def sd = cw_shape2d_default_def();
  pb2d *pb = cw_node_data_t(n, pb_t);
  if (pb) {
    sd.material.friction = pb->friction;
    sd.material.restitution = pb->bounce;
    sd.material.rolling_resistance = pb->rolling_resistance;
    sd.material.tangent_speed = pb->tangent_speed / W->ppm;
    sd.material.user_material = (uint64_t)(uint32_t)pb->user_material;
    sd.hit_events = pb->hit_events;
  }
  rigid2d *rb = cw_node_data_t(n, rigid_t);
  if (rb) sd.density = rb->density;
  sd.layer = c->collision_layer & 31;
  sd.mask = (uint32_t)c->collision_mask;
  area2d *ar = cw_node_data_t(n, area_t);
  if (ar) {
    sd.sensor = true;
    sd.sensor_events = ar->monitoring || ar->monitorable;
  }
  for (int i = 0; i < cw_node_child_count(n); i++) {
    cw_node ch = cw_node_child(n, i);
    shape2d *s = cw_node_data_t(ch, shape_t);
    if (!s || s->disabled) continue;
    cw_node2d *d = cw_node_data_t(ch, node2d_t);
    cw_xform2d L = shape_local(d);
    float sx = sqrtf(L.a * L.a + L.b * L.b), sy = sqrtf(L.c * L.c + L.d * L.d);
    cw_vec2 center = to_m(W, d->position);
    cw_rot2 rot = cw_rot2_from_angle(-d->rotation);
    switch (s->shape) {
    case S_RECT: {
      cw_vec2 half = cw_v2(s->size.x * 0.5f * sx / W->ppm, s->size.y * 0.5f * sy / W->ppm);
      own_shape(W, c, n, cw_shape2d_box(c->body, &sd, half, (cw_xform2){ center, rot }));
      break;
    }
    case S_CIRCLE:
      own_shape(W, c, n, cw_shape2d_circle(c->body, &sd, center, s->radius * CW_MAX(sx, sy) / W->ppm));
      break;
    case S_CAPSULE: {
      float half = CW_MAX(0.0f, s->height * 0.5f - s->radius);
      cw_vec2 a = to_m(W, shape_point(&L, cw_v2(0, -half))), b = to_m(W, shape_point(&L, cw_v2(0, half)));
      own_shape(W, c, n, cw_shape2d_capsule(c->body, &sd, a, b, s->radius * sx / W->ppm));
      break;
    }
    case S_POLYGON: case S_SEGMENT: case S_CHAIN: {
      cw_vec2 *pts;
      int k = cw_points_parse(s->points, &pts);
      for (int j = 0; j < k; j++) pts[j] = to_m(W, shape_point(&L, pts[j]));
      if (s->shape == S_SEGMENT) {
        if (k >= 2) own_shape(W, c, n, cw_shape2d_segment(c->body, &sd, pts[0], pts[1]));
      } else if (s->shape == S_CHAIN) {
        if (s->loop && k >= 3) {
          /* solid inside: counter-clockwise in the y-up world */
          if (signed_area(pts, k) < 0)
            for (int j = 0; j < k / 2; j++) { cw_vec2 t = pts[j]; pts[j] = pts[k - 1 - j]; pts[k - 1 - j] = t; }
        }
        if (!s->loop && k >= 2) {
          /* Box2D treats an open chain's end points as ghost vertices (no
           * collision): extend both ends so every authored segment collides */
          cw_vec2 *ext = cw_frame_alloc(sizeof(cw_vec2) * (size_t)(k + 2));
          ext[0] = cw_v2_add(pts[0], cw_v2_sub(pts[0], pts[1]));
          memcpy(ext + 1, pts, sizeof(cw_vec2) * (size_t)k);
          ext[k + 1] = cw_v2_add(pts[k - 1], cw_v2_sub(pts[k - 1], pts[k - 2]));
          pts = ext;
          k += 2;
        }
        if (k >= (s->loop ? 3 : 4)) {
          cw_chain2d ch2 = cw_chain2d_create(c->body, &sd, pts, k, s->loop);
          if (!CW_HANDLE_IS_NULL(ch2)) {
            cw_array_push(&c->chains, ch2.id);
            /* the chain's segments: owned for event lookups */
            b2ChainId cid = cw_chain2d_raw(ch2);
            int sc = b2Chain_GetSegmentCount(cid);
            b2ShapeId *segs = cw_frame_alloc(sizeof(b2ShapeId) * (size_t)(sc + 1));
            sc = b2Chain_GetSegments(cid, segs, sc);
            for (int q = 0; q < sc; q++) cw_map_set(&W->shape_owner, b2StoreShapeId(segs[q]), n.id);
          }
        } else {
          cw_warn("CollisionShape2D '%s': a chain needs %d+ points", cw_node_name(ch), s->loop ? 3 : 2);
        }
      } else if (k >= 3) {
        if (k <= 8 && signed_area(pts, k) != 0) {
          /* convex check: every turn the same way */
          bool convex = true;
          float sgn = 0;
          for (int j = 0; j < k && convex; j++) {
            float cr = cw_v2_cross(cw_v2_sub(pts[(j + 1) % k], pts[j]), cw_v2_sub(pts[(j + 2) % k], pts[(j + 1) % k]));
            if (cr != 0) {
              if (sgn == 0) sgn = cr;
              else if ((cr > 0) != (sgn > 0)) convex = false;
            }
          }
          if (convex) {
            own_shape(W, c, n, cw_shape2d_polygon(c->body, &sd, pts, k, 0));
            break;
          }
        }
        uint32_t *idx = cw_frame_alloc(sizeof(uint32_t) * (size_t)(3 * k));
        int ni = cw_triangulate(pts, k, idx);
        for (int j = 0; j + 2 < ni; j += 3) {
          cw_vec2 tri[3] = { pts[idx[j]], pts[idx[j + 1]], pts[idx[j + 2]] };
          if (signed_area(tri, 3) != 0) own_shape(W, c, n, cw_shape2d_polygon(c->body, &sd, tri, 3, 0));
        }
      }
      break;
    }
    default: break;
    }
  }
  if (rb && rb->mass > 0 && cw_body2d_valid(c->body)) {
    b2BodyId bid = cw_body2d_raw(c->body);
    b2MassData md = b2Body_GetMassData(bid);
    if (md.mass > 0) {
      float k = rb->mass / md.mass;
      md.mass = rb->mass;
      md.rotationalInertia *= k;
      b2Body_SetMassData(bid, md);
    }
  }
  c->cfg_hash = config_hash(n, c);
  c->dirty = false;
}

/* ── body lifecycle ─────────────────────────────────────────────────── */

static cw_body_type body_type_for(cw_node n, co2d *c) {
  rigid2d *rb = cw_node_data_t(n, rigid_t);
  switch (c->kind) {
  case K_RIGID: return rb && rb->freeze ? (rb->freeze_mode ? CW_BODY_KINEMATIC : CW_BODY_STATIC) : CW_BODY_DYNAMIC;
  case K_STATIC: return CW_BODY_STATIC;
  default: return CW_BODY_KINEMATIC;
  }
}

static void record_synced(cw_node n, co2d *c) {
  cw_node2d *d = cw_node_data_t(n, node2d_t);
  c->synced_pos = d->position;
  c->synced_rot = d->rotation;
  memcpy(c->synced_g, cw_node2d_global_transform(n), sizeof c->synced_g);
}

static void create_body(p2w *W, cw_node n, co2d *c) {
  cw_body2d_def bd = cw_body2d_default_def();
  bd.type = body_type_for(n, c);
  cw_xform2 x = node_xform(W, n);
  bd.position = x.p;
  bd.rotation = x.q;
  bd.name = cw_node_name(n);
  bd.interpolate = false; /* the node interpolates */
  rigid2d *rb = cw_node_data_t(n, rigid_t);
  if (rb) {
    bd.gravity_scale = rb->gravity_scale;
    bd.linear_damping = rb->linear_damp;
    bd.angular_damping = rb->angular_damp;
    bd.lock_rotation = rb->lock_rotation;
    bd.bullet = rb->bullet;
    bd.sleep = rb->can_sleep;
    bd.linear_velocity = cw_v2(rb->init_velocity.x / W->ppm, -rb->init_velocity.y / W->ppm);
    bd.angular_velocity = -rb->init_angular;
  }
  if (c->kind == K_CHARACTER) bd.lock_rotation = true;
  c->body = cw_body2d_create(W->world, &bd);
  cw_map_set(&W->body_owner, c->body.id, n.id);
  build_shapes(W, n, c);
  record_synced(n, c);
}

static void destroy_body(p2w *W, co2d *c) {
  if (CW_HANDLE_IS_NULL(c->body)) return;
  destroy_shapes(W, c);
  cw_map_remove(&W->body_owner, c->body.id);
  if (cw_body2d_valid(c->body)) cw_body2d_destroy(c->body);
  c->body = (cw_body2d){ 0 };
  cw_map_free(&c->pairs);
  c->overlap_count = 0;
}

static void co_init(cw_node n, void *d) {
  (void)n;
  co2d *c = d;
  c->collision_mask = (int32_t)CW_PHYS_ALL_LAYERS;
}

static void co_enter(cw_node n, void *d) {
  co2d *c = d;
  p2w *W = world_of(cw_node_tree(n));
  cw_array_push(&W->objects, n);
  const cw_node_type *t = cw_node_get_type(n);
  c->kind = cw_node_type_is(t, "RigidBody2D") ? K_RIGID : cw_node_type_is(t, "CharacterBody2D") ? K_CHARACTER
          : cw_node_type_is(t, "KinematicBody2D") ? K_KINEMATIC : cw_node_type_is(t, "Area2D") ? K_AREA : K_STATIC;
  create_body(W, n, c);
}

static void co_exit(cw_node n, void *d) {
  co2d *c = d;
  p2w *W = cw_tree_slot(cw_node_tree(n), "physics2d");
  if (!W) return;
  list_remove(W->objects.data, &W->objects.count, n);
  destroy_body(W, c);
  char2d *ch = cw_node_data_t(n, char_t);
  if (ch) ch->mover_ready = false;
}

static void co_destroy(cw_node n, void *d) {
  (void)n;
  co2d *c = d;
  cw_array_free(&c->shapes);
  cw_array_free(&c->chains);
  cw_map_free(&c->pairs);
}

/* ── per-tick sync ──────────────────────────────────────────────────── */

static bool g_changed(const float a[6], const float b[6]) { return memcmp(a, b, sizeof(float) * 6) != 0; }

static void ensure_mover(p2w *W, cw_node n, co2d *c, char2d *ch);

static void sync_in(p2w *W, cw_node n, co2d *c, float dt) {
  if (CW_HANDLE_IS_NULL(c->body) || !cw_body2d_valid(c->body)) create_body(W, n, c);
  if (c->dirty || config_hash(n, c) != c->cfg_hash) build_shapes(W, n, c);
  cw_body_type want = body_type_for(n, c);
  if (cw_body2d_type(c->body) != want) cw_body2d_set_type(c->body, want);
  cw_node2d *d = cw_node_data_t(n, node2d_t);
  switch (c->kind) {
  case K_RIGID: case K_CHARACTER:
    if (d->position.x != c->synced_pos.x || d->position.y != c->synced_pos.y || d->rotation != c->synced_rot) {
      /* moved by code: teleport */
      cw_body2d_set_transform(c->body, node_xform(W, n));
      char2d *ch = cw_node_data_t(n, char_t);
      if (ch && ch->mover_ready) {
        cw_xform2 x = node_xform(W, n);
        cw_mover2d_teleport(&ch->mover, cw_v2_add(x.p, to_m(W, ch->feet)));
      }
      record_synced(n, c);
    }
    if (c->kind == K_RIGID && want == CW_BODY_KINEMATIC) cw_body2d_set_target(c->body, node_xform(W, n), dt);
    if (c->kind == K_CHARACTER) ensure_mover(W, n, c, cw_node_data_t(n, char_t));
    break;
  case K_KINEMATIC:
    cw_body2d_set_target(c->body, node_xform(W, n), dt);
    record_synced(n, c);
    break;
  default: { /* static, area: teleport when the node moved */
    const float *g = cw_node2d_global_transform(n);
    if (g_changed(g, c->synced_g)) {
      cw_body2d_set_transform(c->body, node_xform(W, n));
      record_synced(n, c);
    }
  }
  }
}

static void sync_out(p2w *W, cw_node n, co2d *c) {
  if (c->kind != K_RIGID || cw_body2d_type(c->body) != CW_BODY_DYNAMIC) return;
  cw_xform2 x = cw_body2d_transform(c->body);
  cw_vec2 p = to_px(W, x.p);
  float rot = cw_atan2(-x.q.s, x.q.c);
  cw_node2d_set_global(n, p, rot);
  record_synced(n, c);
}

static void pre_hook(cw_tree *t, double dt, void *u) {
  (void)dt;
  (void)u;
  if (cw_tree_paused(t)) return;
  p2w *W = cw_tree_slot(t, "physics2d");
  for (int i = 0; i < W->objects.count; i++) {
    co2d *c = cw_node_data_t(W->objects.data[i], co_t);
    if (c && c->kind != K_STATIC && c->kind != K_AREA) cw_node2d_snapshot(W->objects.data[i]);
  }
}

/* ── characters ─────────────────────────────────────────────────────── */

static void ensure_mover(p2w *W, cw_node n, co2d *c, char2d *ch) {
  /* size from the first shape */
  float radius = 0.3f, height = 1.8f;
  cw_vec2 feet = cw_v2(0, 0);
  for (int i = 0; i < cw_node_child_count(n); i++) {
    cw_node sn = cw_node_child(n, i);
    shape2d *s = cw_node_data_t(sn, shape_t);
    if (!s || s->disabled) continue;
    cw_node2d *d = cw_node_data_t(sn, node2d_t);
    float sx = fabsf(d->scale.x), sy = fabsf(d->scale.y);
    float r = 8, h = 16;
    if (s->shape == S_CAPSULE) r = s->radius * sx, h = s->height * sy;
    else if (s->shape == S_CIRCLE) r = s->radius * CW_MAX(sx, sy), h = 2 * r;
    else if (s->shape == S_RECT) r = s->size.x * 0.5f * sx, h = s->size.y * sy;
    radius = r / W->ppm;
    height = CW_MAX(h, 2 * r) / W->ppm;
    feet = cw_v2(d->position.x, d->position.y + CW_MAX(h, 2 * r) * 0.5f);
    break;
  }
  cw_mover2d_def md = cw_mover2d_default_def();
  md.radius = radius;
  md.height = height;
  md.max_floor_angle = ch->floor_max_angle;
  md.step_height = ch->step_height / W->ppm;
  md.snap_length = ch->floor_snap_length / W->ppm;
  md.max_iterations = ch->max_slides;
  md.mask = (uint32_t)c->collision_mask;
  md.push_mass = ch->push_mass;
  md.ride_platforms = ch->ride_platforms;
  uint32_t h = cw_hash32(&md, sizeof md, CW_HASH32_INIT);
  h = cw_hash32(&feet, sizeof feet, h);
  if (ch->mover_ready && h == ch->mover_hash) return;
  ch->feet = feet;
  cw_xform2 x = node_xform(W, n);
  cw_vec2 at = cw_v2_add(x.p, to_m(W, feet));
  if (ch->mover_ready) {
    cw_vec2 keep = ch->mover.position, v = ch->mover.velocity;
    cw_mover2d_init(&ch->mover, W->world, &md, keep);
    ch->mover.velocity = v;
  } else {
    cw_mover2d_init(&ch->mover, W->world, &md, at);
  }
  ch->mover_ready = true;
  ch->mover_hash = h;
}

bool cw_character2d_move_and_slide(cw_node n, float dt) {
  co2d *c = cw_node_data_t(n, co_t);
  char2d *ch = cw_node_data_t(n, char_t);
  p2w *W = c && ch ? cw_tree_slot(cw_node_tree(n), "physics2d") : NULL;
  if (!W) return false;
  sync_in(W, n, c, dt); /* shapes, teleports, the mover */
  cw_mover2d *m = &ch->mover;
  m->velocity = cw_v2(ch->velocity.x / W->ppm, -ch->velocity.y / W->ppm);
  /* the proxy body must not block its own mover: park it out of the way */
  cw_xform2 home = cw_body2d_transform(c->body);
  cw_body2d_set_transform(c->body, (cw_xform2){ cw_v2(home.p.x, home.p.y - 100000.0f), home.q });
  cw_mover2d_move(m, dt);
  cw_vec2 origin = cw_v2_sub(m->position, to_m(W, ch->feet));
  cw_body2d_set_transform(c->body, (cw_xform2){ origin, home.q });
  cw_node2d *d = cw_node_data_t(n, node2d_t);
  cw_node2d_set_global(n, to_px(W, origin), d->rotation);
  /* keep the node's own rotation: characters do not rotate */
  record_synced(n, c);
  ch->velocity = cw_v2(m->velocity.x * W->ppm, -m->velocity.y * W->ppm);
  ch->on_floor = m->on_floor;
  ch->on_wall = m->on_wall;
  ch->on_ceiling = m->on_ceiling;
  ch->floor_normal = cw_v2(m->floor_normal.x, -m->floor_normal.y);
  ch->wall_normal = cw_v2(m->wall_normal.x, -m->wall_normal.y);
  ch->platform_velocity = cw_v2(m->platform_velocity.x * W->ppm, -m->platform_velocity.y * W->ppm);
  return m->on_floor || m->on_wall || m->on_ceiling;
}

/* ── joints ─────────────────────────────────────────────────────────── */

enum { J_PIN, J_HINGE, J_SLIDER, J_DISTANCE, J_SPRING, J_WELD, J_WHEEL, J_MOTOR };
static const char *const joint_names[] = { "PIN", "HINGE", "SLIDER", "DISTANCE", "SPRING", "WELD", "WHEEL", "MOTOR", NULL };

typedef struct joint2d {
  int joint_type;
  const char *node_a, *node_b;
  bool collide_connected;
  float break_force, break_torque;
  bool limit_enabled;
  float lower, upper;
  bool motor_enabled;
  float motor_speed, max_motor;
  float stiffness, damping;
  float length;
  /* runtime */
  cw_joint2d joint;
  uint32_t struct_hash, live_hash;
  bool broken;
  cw_body2d built_a, built_b;
} joint2d;

static cw_body2d body_at(cw_node from, const char *path) {
  if (!path || !*path) return (cw_body2d){ 0 };
  cw_node t = cw_node_get(from, path);
  co2d *c = cw_node_data_t(t, co_t);
  return c ? c->body : (cw_body2d){ 0 };
}

static uint32_t joint_struct_hash(cw_node n, joint2d *j, cw_body2d a, cw_body2d b) {
  uint32_t h = CW_HASH32_INIT;
  h = cw_hash32(&j->joint_type, sizeof j->joint_type, h);
  h = cw_hash32(&a.id, sizeof a.id, h);
  h = cw_hash32(&b.id, sizeof b.id, h);
  h = cw_hash32(&j->collide_connected, sizeof j->collide_connected, h);
  h = cw_hash32(&j->break_force, sizeof(float) * 2, h);
  h = cw_hash32(&j->length, sizeof j->length, h);
  cw_node2d *d = cw_node_data_t(n, node2d_t);
  h = cw_hash32(&d->position, sizeof d->position, h);
  h = cw_hash32(&d->rotation, sizeof d->rotation, h);
  return h;
}
static uint32_t joint_live_hash(joint2d *j) {
  uint32_t h = cw_hash32(&j->limit_enabled, sizeof j->limit_enabled, CW_HASH32_INIT);
  h = cw_hash32(&j->lower, sizeof(float) * 2, h);
  h = cw_hash32(&j->motor_enabled, sizeof j->motor_enabled, h);
  h = cw_hash32(&j->motor_speed, sizeof(float) * 4, h);
  return h;
}

static void joint_apply_live(p2w *W, joint2d *j) {
  bool lin = j->joint_type == J_SLIDER || j->joint_type == J_WHEEL || j->joint_type == J_DISTANCE || j->joint_type == J_SPRING;
  float k = lin ? 1.0f / W->ppm : 1;
  cw_joint2d_set_limits(j->joint, j->limit_enabled, lin ? j->lower * k : -j->upper, lin ? j->upper * k : -j->lower);
  bool lin_motor = lin && j->joint_type != J_WHEEL; /* a wheel's motor spins it: rad/s */
  cw_joint2d_set_motor(j->joint, j->motor_enabled, lin_motor ? j->motor_speed * k : -j->motor_speed, j->max_motor);
  if (j->joint_type == J_SPRING || j->joint_type == J_WHEEL || j->stiffness > 0)
    cw_joint2d_set_spring(j->joint, j->joint_type == J_SPRING || j->stiffness > 0, j->stiffness, j->damping);
  j->live_hash = joint_live_hash(j);
}

static void joint_sync(p2w *W, cw_node n, joint2d *j) {
  if (j->broken) return;
  cw_body2d a = body_at(n, j->node_a), b = body_at(n, j->node_b);
  if (CW_HANDLE_IS_NULL(a) && (!j->node_a || !*j->node_a)) {
    if (CW_HANDLE_IS_NULL(W->ground) || !cw_body2d_valid(W->ground)) {
      cw_body2d_def gd = cw_body2d_default_def();
      gd.type = CW_BODY_STATIC;
      W->ground = cw_body2d_create(W->world, &gd);
    }
    a = W->ground;
  }
  bool valid = cw_joint2d_valid(j->joint);
  if (!cw_body2d_valid(a) || !cw_body2d_valid(b)) {
    if (valid) cw_joint2d_destroy(j->joint);
    j->joint = (cw_joint2d){ 0 };
    return;
  }
  uint32_t sh = joint_struct_hash(n, j, a, b);
  if (valid && sh == j->struct_hash) {
    if (joint_live_hash(j) != j->live_hash) joint_apply_live(W, j);
    return;
  }
  if (valid) {
    cw_map_remove(&W->joint_owner, j->joint.id);
    cw_joint2d_destroy(j->joint);
  }
  static const cw_joint_type map[] = { CW_JOINT_REVOLUTE, CW_JOINT_REVOLUTE, CW_JOINT_PRISMATIC, CW_JOINT_DISTANCE,
                                       CW_JOINT_DISTANCE, CW_JOINT_WELD, CW_JOINT_WHEEL, CW_JOINT_MOTOR };
  cw_joint2d_def jd = cw_joint2d_default_def(map[j->joint_type]);
  jd.a = a;
  jd.b = b;
  const float *g = cw_node2d_global_transform(n);
  cw_vec2 anchor = to_m(W, cw_v2(g[4], g[5]));
  float len = sqrtf(g[0] * g[0] + g[1] * g[1]);
  cw_vec2 axis = len > 0 ? cw_v2(g[0] / len, -g[1] / len) : cw_v2(1, 0);
  cw_joint2d_def_anchor(&jd, anchor, axis);
  if (j->joint_type == J_DISTANCE || j->joint_type == J_SPRING) {
    jd.frame_b.p = cw_v2(0, 0); /* anchor on B: its origin */
    cw_xform2 xb = cw_body2d_transform(b);
    cw_vec2 dv = cw_v2_sub(xb.p, anchor);
    jd.length = j->length > 0 ? j->length / W->ppm : cw_v2_length(dv);
    jd.spring = j->joint_type == J_SPRING;
    jd.hertz = j->stiffness > 0 ? j->stiffness : 4;
    jd.damping_ratio = j->damping;
    jd.limit = j->limit_enabled;
    jd.lower = j->lower / W->ppm;
    jd.upper = j->upper / W->ppm;
  } else if (j->joint_type == J_SLIDER || j->joint_type == J_WHEEL) {
    jd.limit = j->limit_enabled;
    jd.lower = j->lower / W->ppm;
    jd.upper = j->upper / W->ppm;
    jd.motor_speed = j->joint_type == J_WHEEL ? -j->motor_speed : j->motor_speed / W->ppm;
    if (j->joint_type == J_WHEEL) { jd.spring = true; jd.hertz = j->stiffness > 0 ? j->stiffness : 4; jd.damping_ratio = j->damping; }
  } else if (j->joint_type == J_PIN || j->joint_type == J_HINGE) {
    /* node angles are clockwise on screen: the world's are counter-clockwise */
    jd.limit = j->limit_enabled;
    jd.lower = -j->upper;
    jd.upper = -j->lower;
    jd.motor_speed = -j->motor_speed;
  } else if (j->joint_type == J_WELD) {
    jd.hertz = j->stiffness;
    jd.damping_ratio = j->damping;
  }
  jd.motor = j->motor_enabled;
  jd.max_motor = j->max_motor;
  jd.collide_connected = j->collide_connected;
  jd.break_force = j->break_force;
  jd.break_torque = j->break_torque;
  j->joint = cw_joint2d_create(W->world, &jd);
  if (!CW_HANDLE_IS_NULL(j->joint)) cw_map_set(&W->joint_owner, j->joint.id, n.id);
  j->struct_hash = sh;
  j->live_hash = joint_live_hash(j);
}

static void joint_enter(cw_node n, void *d) {
  (void)d;
  p2w *W = world_of(cw_node_tree(n));
  cw_array_push(&W->joints, n);
}
static void joint_exit(cw_node n, void *d) {
  joint2d *j = d;
  p2w *W = cw_tree_slot(cw_node_tree(n), "physics2d");
  if (!W) return;
  list_remove(W->joints.data, &W->joints.count, n);
  if (cw_joint2d_valid(j->joint)) {
    cw_map_remove(&W->joint_owner, j->joint.id);
    cw_joint2d_destroy(j->joint);
  }
  j->joint = (cw_joint2d){ 0 };
}
static void joint_init(cw_node n, void *d) {
  (void)n;
  joint2d *j = d;
  j->damping = 0.5f;
  j->max_motor = 1000;
}

/* ── casts ──────────────────────────────────────────────────────────── */

enum { Q_CIRCLE, Q_RECT, Q_CAPSULE };

typedef struct cast2d {
  bool enabled;
  cw_vec2 target_position;
  int32_t collision_mask;
  bool collide_with_bodies, collide_with_areas, exclude_parent;
  /* shape cast */
  int shape;
  float radius, height;
  cw_vec2 size;
  int max_results;
  /* results */
  bool colliding;
  int32_t collision_count;
  cw_node collider;
  cw_vec2 collision_point, collision_normal;
  float collision_fraction;
} cast2d;

typedef struct filt_ctx {
  bool bodies, areas;
} filt_ctx;
static bool cast_filter(uint64_t shape_id, void *ctx) {
  filt_ctx *f = ctx;
  bool sensor = cw_shape2d_is_sensor((cw_shape2d){ shape_id });
  return sensor ? f->areas : f->bodies;
}

static void cast_update(p2w *W, cw_node n, cast2d *c, bool is_shape) {
  c->colliding = false;
  c->collision_count = 0;
  c->collider = (cw_node){ 0 };
  if (!c->enabled) return;
  const float *g = cw_node2d_global_transform(n);
  cw_xform2d gx = { g[0], g[1], g[2], g[3], g[4], g[5] };
  cw_vec2 o_px = cw_v2(g[4], g[5]), e_px = cw_xform2d_apply(&gx, c->target_position);
  cw_vec2 o = to_m(W, o_px), tr = cw_v2_sub(to_m(W, e_px), o);
  filt_ctx fc = { c->collide_with_bodies, c->collide_with_areas };
  cw_phys_filter f = cw_phys_filter_layers((uint32_t)c->collision_mask);
  f.sensors = c->collide_with_areas;
  f.fn = cast_filter;
  f.ctx = &fc;
  if (c->exclude_parent) {
    co2d *pc = cw_node_data_t(cw_node_parent(n), co_t);
    if (pc) f.ignore_body = pc->body.id;
  }
  cw_ray_hit2d hit;
  if (!is_shape) {
    if (cw_world2d_raycast(W->world, o, tr, &f, &hit)) {
      c->colliding = true;
      c->collision_count = 1;
      c->collider = owner_of_shape(W, hit.shape);
      c->collision_point = to_px(W, hit.point);
      c->collision_normal = cw_v2(hit.normal.x, -hit.normal.y);
      c->collision_fraction = hit.fraction;
    }
    return;
  }
  float sx = sqrtf(g[0] * g[0] + g[1] * g[1]);
  cw_qshape2d q;
  float r = c->radius * sx / W->ppm;
  if (c->shape == Q_RECT) q = cw_qshape2d_box(o, cw_v2(c->size.x * 0.5f * sx / W->ppm, c->size.y * 0.5f * sx / W->ppm),
                                              (cw_rot2){ g[0] / sx, -g[1] / sx });
  else if (c->shape == Q_CAPSULE) {
    float h = CW_MAX(0.0f, c->height * 0.5f - c->radius) * sx / W->ppm;
    q = cw_qshape2d_capsule(cw_v2(o.x, o.y - h), cw_v2(o.x, o.y + h), r);
  } else q = cw_qshape2d_circle(o, r);
  int cap = CW_CLAMP(c->max_results, 1, 32);
  if (c->target_position.x == 0 && c->target_position.y == 0) {
    cw_shape2d out[32];
    int k = cw_world2d_overlap(W->world, &q, &f, out, cap);
    if (k) {
      c->colliding = true;
      c->collision_count = k;
      c->collider = owner_of_shape(W, out[0]);
      c->collision_point = o_px;
      c->collision_normal = cw_v2(0, 0);
      c->collision_fraction = 0;
    }
  } else {
    cw_ray_hit2d hits[32];
    int k = cw_world2d_shapecast(W->world, &q, tr, &f, hits, cap);
    if (k) {
      c->colliding = true;
      c->collision_count = k;
      c->collider = owner_of_shape(W, hits[0].shape);
      c->collision_point = to_px(W, hits[0].point);
      c->collision_normal = cw_v2(hits[0].normal.x, -hits[0].normal.y);
      c->collision_fraction = hits[0].fraction;
    }
  }
}

static void cast_enter(cw_node n, void *d) {
  (void)d;
  p2w *W = world_of(cw_node_tree(n));
  cw_array_push(&W->casts, n);
}
static void cast_exit(cw_node n, void *d) {
  (void)d;
  p2w *W = cw_tree_slot(cw_node_tree(n), "physics2d");
  if (W) list_remove(W->casts.data, &W->casts.count, n);
}
static void cast_init(cw_node n, void *d) {
  (void)n;
  cast2d *c = d;
  c->enabled = true;
  c->target_position = cw_v2(0, 50);
  c->collision_mask = (int32_t)CW_PHYS_ALL_LAYERS;
  c->collide_with_bodies = true;
  c->exclude_parent = true;
  c->radius = 8;
  c->size = cw_v2(16, 16);
  c->height = 32;
  c->max_results = 8;
}

void cw_raycast2d_force_update(cw_node n) {
  cast2d *c = cw_node_data_t(n, cw_node_data_t(n, ray_t) ? ray_t : scast_t);
  p2w *W = cw_tree_slot(cw_node_tree(n), "physics2d");
  if (c && W) cast_update(W, n, c, cw_node_data_t(n, scast_t) != NULL);
}

/* ── the step ───────────────────────────────────────────────────────── */

static void contact_pair(p2w *W, cw_node a, cw_node b, int delta) {
  pb2d *pa = cw_node_data_t(a, pb_t);
  co2d *ca = cw_node_data_t(a, co_t);
  if (!pa || !ca || !pa->contact_monitor) return;
  int before = 0;
  uint64_t v;
  if (ca->pairs.slot_cap && cw_map_get(&ca->pairs, b.id, &v)) before = (int)v;
  int after = cw__pair_add(&ca->pairs, b.id, delta);
  if (!before && after) cw__psig_push(&W->sigs, a, cw__s_body_entered, b, 0, 1);
  if (before && !after) cw__psig_push(&W->sigs, a, cw__s_body_exited, b, 0, 1);
}

static void sensor_pair(p2w *W, cw_node area, cw_node other, int delta) {
  area2d *ar = cw_node_data_t(area, area_t);
  co2d *ca = cw_node_data_t(area, co_t);
  if (!ar || !ca) return;
  area2d *oa = cw_node_data_t(other, area_t);
  if (oa && !oa->monitorable) return;
  int before = 0;
  uint64_t v;
  if (ca->pairs.slot_cap && cw_map_get(&ca->pairs, other.id, &v)) before = (int)v;
  int after = cw__pair_add(&ca->pairs, other.id, delta);
  ca->overlap_count = (int32_t)cw_map_size(&ca->pairs);
  if (!ar->monitoring) return;
  if (!before && after) cw__psig_push(&W->sigs, area, oa ? cw__s_area_entered : cw__s_body_entered, other, 0, 1);
  if (before && !after) cw__psig_push(&W->sigs, area, oa ? cw__s_area_exited : cw__s_body_exited, other, 0, 1);
}

static void post_hook(cw_tree *t, double dt, void *u) {
  (void)u;
  if (cw_tree_paused(t)) return;
  p2w *W = cw_tree_slot(t, "physics2d");
  float fdt = (float)dt;
  cw__phys_signals_init();
  for (int i = 0; i < W->objects.count; i++) {
    cw_node n = W->objects.data[i];
    co2d *c = cw_node_data_t(n, co_t);
    if (c) sync_in(W, n, c, fdt);
  }
  for (int i = 0; i < W->joints.count; i++) {
    joint2d *j = cw_node_data_t(W->joints.data[i], joint_t);
    if (j) joint_sync(W, W->joints.data[i], j);
  }
  cw_world2d_step(W->world, fdt);
  W->steps++;
  for (int i = 0; i < W->objects.count; i++) {
    cw_node n = W->objects.data[i];
    co2d *c = cw_node_data_t(n, co_t);
    if (c) sync_out(W, n, c);
  }
  for (int i = 0; i < W->casts.count; i++) {
    cw_node n = W->casts.data[i];
    cast2d *c = cw_node_data_t(n, cw_node_data_t(n, ray_t) ? ray_t : scast_t);
    if (c) cast_update(W, n, c, cw_node_data_t(n, scast_t) != NULL);
  }
  int ne;
  const cw_phys_event2d *ev = cw_world2d_events(W->world, &ne);
  for (int i = 0; i < ne; i++) {
    const cw_phys_event2d *e = &ev[i];
    switch (e->type) {
    case CW_PHYS_CONTACT_BEGIN: case CW_PHYS_CONTACT_END: {
      cw_node a = owner_of_shape(W, e->shape_a), b = owner_of_shape(W, e->shape_b);
      if (CW_HANDLE_IS_NULL(a) || CW_HANDLE_IS_NULL(b)) break;
      int dlt = e->type == CW_PHYS_CONTACT_BEGIN ? 1 : -1;
      contact_pair(W, a, b, dlt);
      contact_pair(W, b, a, dlt);
      break;
    }
    case CW_PHYS_SENSOR_ENTER: case CW_PHYS_SENSOR_EXIT: {
      cw_node a = owner_of_shape(W, e->shape_a), b = owner_of_shape(W, e->shape_b);
      if (CW_HANDLE_IS_NULL(a) || CW_HANDLE_IS_NULL(b)) break;
      sensor_pair(W, a, b, e->type == CW_PHYS_SENSOR_ENTER ? 1 : -1);
      break;
    }
    case CW_PHYS_HIT: {
      cw_node a = owner_of_shape(W, e->shape_a), b = owner_of_shape(W, e->shape_b);
      pb2d *pa = cw_node_data_t(a, pb_t), *pbb = cw_node_data_t(b, pb_t);
      if (pa && pa->hit_events) cw__psig_push(&W->sigs, a, cw__s_body_hit, b, e->speed * W->ppm, 2);
      if (pbb && pbb->hit_events) cw__psig_push(&W->sigs, b, cw__s_body_hit, a, e->speed * W->ppm, 2);
      break;
    }
    case CW_PHYS_JOINT_BREAK: {
      uint64_t v;
      if (cw_map_get(&W->joint_owner, e->joint.id, &v)) {
        cw_node jn = node_by_id(v);
        joint2d *j = cw_node_data_t(jn, joint_t);
        if (j) {
          j->broken = true;
          j->joint = (cw_joint2d){ 0 };
        }
        cw_map_remove(&W->joint_owner, e->joint.id);
        cw__psig_push(&W->sigs, jn, cw__s_broken, (cw_node){ 0 }, 0, 0);
      }
      break;
    }
    default: break;
    }
  }
  /* shapes destroyed two steps ago can no longer appear in events */
  int k = 0;
  for (int i = 0; i < W->dead.count; i++) {
    if (W->dead.data[i].step < W->steps - 2) cw_map_remove(&W->shape_owner, W->dead.data[i].shape);
    else W->dead.data[k++] = W->dead.data[i];
  }
  W->dead.count = k;
  cw__psig_deliver(t, &W->sigs);
}

/* ── reflection ─────────────────────────────────────────────────────── */

static const char *const *names2d(void) { return cw_physics_nodes_get_config()->layers_2d; }
static void layer_name_get(cw_node n, void *d, cw_value *o) {
  (void)n;
  const char *nm = names2d()[((co2d *)d)->collision_layer & 31];
  char buf[16];
  snprintf(buf, sizeof buf, "%d", ((co2d *)d)->collision_layer);
  *o = cw_val_string(nm ? nm : buf);
}
static void layer_name_set(cw_node n, void *d, const cw_value *v) {
  (void)n;
  int k = cw__layer_index(names2d(), v->s ? v->s : "");
  if (k >= 0) ((co2d *)d)->collision_layer = k;
}
static void mask_names_get(cw_node n, void *d, cw_value *o) {
  (void)n;
  char buf[1024];
  cw__layer_bits_text(names2d(), (uint32_t)((co2d *)d)->collision_mask, buf, sizeof buf);
  *o = cw_val_string(buf);
}
static void mask_names_set(cw_node n, void *d, const cw_value *v) {
  (void)n;
  ((co2d *)d)->collision_mask = (int32_t)cw__layer_bits(names2d(), v->s);
}
static void body_get(cw_node n, void *d, cw_value *o) {
  (void)n;
  *o = cw_val_int((int64_t)((co2d *)d)->body.id);
}

static const cw_prop co_props[] = {
  CW_PROP_FIELD_RANGE(co2d, collision_layer, CW_T_INT, 0, 31, "The layer its shapes sit on (0..31)."),
  CW_PROP_FIELD(co2d, collision_mask, CW_T_INT, CW_PROP_DEFAULT, "Layers it collides with (bits; -1 = all)."),
  CW_PROP_ACCESSOR("collision_layer_name", CW_T_STRING, CW_PROP_EDITOR, layer_name_get, layer_name_set, "The layer by project name."),
  CW_PROP_ACCESSOR("collision_mask_names", CW_T_STRING, CW_PROP_EDITOR, mask_names_get, mask_names_set, "The mask by project names (\"world|enemy\", \"all\")."),
  CW_PROP_ACCESSOR("body_id", CW_T_INT, CW_PROP_READONLY | CW_PROP_EDITOR, body_get, NULL, "The wrapper body handle (0 outside a tree)."),
  CW_PROP_END,
};
static cw_node_type co_type = { .name = "CollisionObject2D", .parent = "Node2D", .data_size = sizeof(co2d), .props = co_props,
                                .doc = "Base of 2D bodies and areas: collision layer and mask.", .init = co_init,
                                .destroy = co_destroy, .enter_tree = co_enter, .exit_tree = co_exit };

static const char *const body_signals[] = { "body_entered", "body_exited", "body_hit", NULL };
static void pb_init(cw_node n, void *d) {
  (void)n;
  pb2d *p = d;
  p->friction = 0.6f;
}
static const cw_prop pb_props[] = {
  CW_PROP_FIELD_RANGE(pb2d, friction, CW_T_FLOAT, 0, 10, "Friction (mixed sqrt(a*b))."),
  CW_PROP_FIELD_RANGE(pb2d, bounce, CW_T_FLOAT, 0, 1, "Restitution (mixed max)."),
  CW_PROP_FIELD_RANGE(pb2d, rolling_resistance, CW_T_FLOAT, 0, 1, "Rolling resistance (circles, capsules)."),
  CW_PROP_FIELD(pb2d, tangent_speed, CW_T_FLOAT, CW_PROP_DEFAULT, "Conveyor surface speed, px/s."),
  CW_PROP_FIELD(pb2d, user_material, CW_T_INT, CW_PROP_DEFAULT, "Your material id (hits, queries)."),
  CW_PROP_FIELD(pb2d, contact_monitor, CW_T_BOOL, CW_PROP_DEFAULT, "Emit body_entered/body_exited on contacts."),
  CW_PROP_FIELD(pb2d, hit_events, CW_T_BOOL, CW_PROP_DEFAULT, "Emit body_hit(other, speed px/s) on impacts."),
  CW_PROP_END,
};
static cw_node_type pb_type = { .name = "PhysicsBody2D", .parent = "CollisionObject2D", .data_size = sizeof(pb2d), .props = pb_props,
                                .signals = body_signals, .doc = "Base of 2D bodies: surface material.", .init = pb_init };

static cw_node_type static_type = { .name = "StaticBody2D", .parent = "PhysicsBody2D", .doc = "A body that does not move by itself (moving the node teleports it)." };
static void interp_init(cw_node n, void *d) {
  (void)d;
  cw_node2d *nd = cw_node_data_t(n, node2d_t);
  if (nd) nd->interpolate = true;
}
static cw_node_type kin_type = { .name = "KinematicBody2D", .parent = "PhysicsBody2D", .init = interp_init,
                                 .doc = "Follows its node's transform with velocity (pushes, carries)." };

/* rigid */
static const char *const freeze_names[] = { "STATIC", "KINEMATIC", NULL };
static void rigid_init(cw_node n, void *d) {
  interp_init(n, d);
  rigid2d *r = d;
  r->density = 1;
  r->gravity_scale = 1;
  r->can_sleep = true;
}
static void lv_get(cw_node n, void *d, cw_value *o) {
  rigid2d *r = d;
  co2d *c = cw_node_data_t(n, co_t);
  p2w *W = cw_tree_slot(cw_node_tree(n), "physics2d");
  if (W && c && cw_body2d_valid(c->body)) {
    cw_vec2 v = cw_body2d_velocity(c->body);
    *o = cw_val_vec2(v.x * W->ppm, -v.y * W->ppm);
  } else *o = cw_val_vec2(r->init_velocity.x, r->init_velocity.y);
}
static void lv_set(cw_node n, void *d, const cw_value *v) {
  rigid2d *r = d;
  r->init_velocity = cw_v2(v->v[0], v->v[1]);
  co2d *c = cw_node_data_t(n, co_t);
  p2w *W = cw_tree_slot(cw_node_tree(n), "physics2d");
  if (W && c && cw_body2d_valid(c->body)) cw_body2d_set_velocity(c->body, cw_v2(v->v[0] / W->ppm, -v->v[1] / W->ppm));
}
static void av_get(cw_node n, void *d, cw_value *o) {
  rigid2d *r = d;
  co2d *c = cw_node_data_t(n, co_t);
  *o = cw_val_float(c && cw_body2d_valid(c->body) ? -cw_body2d_angular_velocity(c->body) : r->init_angular);
}
static void av_set(cw_node n, void *d, const cw_value *v) {
  rigid2d *r = d;
  r->init_angular = (float)v->f;
  co2d *c = cw_node_data_t(n, co_t);
  if (c && cw_body2d_valid(c->body)) cw_body2d_set_angular_velocity(c->body, -(float)v->f);
}
static void sleeping_get(cw_node n, void *d, cw_value *o) {
  (void)d;
  co2d *c = cw_node_data_t(n, co_t);
  *o = cw_val_bool(c && cw_body2d_valid(c->body) && !cw_body2d_awake(c->body));
}
/* body settings that need the body updated in place */
#define RIGID_LIVE(field, apply) \
  static void set_##field(cw_node n, void *d, const cw_value *v) { \
    rigid2d *r = d; \
    r->field = (float)v->f; \
    co2d *c = cw_node_data_t(n, co_t); \
    if (c && cw_body2d_valid(c->body)) apply(cw_body2d_raw(c->body), r->field); \
  } \
  static void get_##field(cw_node n, void *d, cw_value *o) { (void)n; *o = cw_val_float(((rigid2d *)d)->field); }
RIGID_LIVE(gravity_scale, b2Body_SetGravityScale)
RIGID_LIVE(linear_damp, b2Body_SetLinearDamping)
RIGID_LIVE(angular_damp, b2Body_SetAngularDamping)
static void lock_set(cw_node n, void *d, const cw_value *v) {
  rigid2d *r = d;
  r->lock_rotation = v->b;
  co2d *c = cw_node_data_t(n, co_t);
  if (c && cw_body2d_valid(c->body)) {
    b2MotionLocks ml = { 0 };
    ml.angularZ = v->b;
    b2Body_SetMotionLocks(cw_body2d_raw(c->body), ml);
  }
}
static void lock_get(cw_node n, void *d, cw_value *o) { (void)n; *o = cw_val_bool(((rigid2d *)d)->lock_rotation); }

static const cw_prop rigid_props[] = {
  CW_PROP_FIELD_RANGE(rigid2d, density, CW_T_FLOAT, 0, 1e6, "kg per square meter (mass from shapes)."),
  CW_PROP_FIELD_RANGE(rigid2d, mass, CW_T_FLOAT, 0, 1e9, "kg; 0 = from density."),
  CW_PROP_ACCESSOR("gravity_scale", CW_T_FLOAT, CW_PROP_DEFAULT, get_gravity_scale, set_gravity_scale, "Gravity multiplier."),
  CW_PROP_ACCESSOR("linear_damp", CW_T_FLOAT, CW_PROP_DEFAULT, get_linear_damp, set_linear_damp, "Linear damping."),
  CW_PROP_ACCESSOR("angular_damp", CW_T_FLOAT, CW_PROP_DEFAULT, get_angular_damp, set_angular_damp, "Angular damping."),
  CW_PROP_ACCESSOR("lock_rotation", CW_T_BOOL, CW_PROP_DEFAULT, lock_get, lock_set, "Never rotates."),
  CW_PROP_FIELD(rigid2d, bullet, CW_T_BOOL, CW_PROP_DEFAULT, "Continuous collision against other dynamic bodies."),
  CW_PROP_FIELD(rigid2d, can_sleep, CW_T_BOOL, CW_PROP_DEFAULT, "May fall asleep when resting."),
  CW_PROP_FIELD(rigid2d, freeze, CW_T_BOOL, CW_PROP_DEFAULT, "Frozen: a static or kinematic body (freeze_mode)."),
  { .name = "freeze_mode", .type = CW_T_ENUM, .flags = CW_PROP_DEFAULT, .offset = offsetof(rigid2d, freeze_mode), .size = sizeof(int),
    .enum_names = freeze_names, .doc = "What a frozen body becomes (KINEMATIC follows the node)." },
  CW_PROP_ACCESSOR("linear_velocity", CW_T_VEC2, CW_PROP_DEFAULT, lv_get, lv_set, "px/s (live)."),
  CW_PROP_ACCESSOR("angular_velocity", CW_T_FLOAT, CW_PROP_DEFAULT, av_get, av_set, "rad/s clockwise (live)."),
  CW_PROP_ACCESSOR("sleeping", CW_T_BOOL, CW_PROP_READONLY | CW_PROP_EDITOR, sleeping_get, NULL, "Asleep."),
  CW_PROP_END,
};
static cw_node_type rigid_type = { .name = "RigidBody2D", .parent = "PhysicsBody2D", .data_size = sizeof(rigid2d),
                                   .props = rigid_props, .doc = "A simulated body; its transform drives the node.", .init = rigid_init };

void cw_rigid_body2d_apply_impulse(cw_node n, cw_vec2 j) {
  co2d *c = cw_node_data_t(n, co_t);
  p2w *W = cw_tree_slot(cw_node_tree(n), "physics2d");
  if (c && W && c->kind == K_RIGID && cw_body2d_valid(c->body))
    cw_body2d_apply_impulse(c->body, cw_v2(j.x / W->ppm, -j.y / W->ppm), NULL);
}

/* character */
static void char_init(cw_node n, void *d) {
  interp_init(n, d);
  char2d *c = d;
  c->floor_max_angle = CW_PI / 4;
  c->floor_snap_length = 4;
  c->step_height = 4;
  c->max_slides = 5;
  c->ride_platforms = true;
}
#define RO(T, field, ty, docs) { .name = #field, .type = ty, .flags = CW_PROP_READONLY | CW_PROP_EDITOR, .offset = (int)offsetof(T, field), .size = (int)sizeof(((T *)0)->field), .doc = docs }
static const cw_prop char_props[] = {
  CW_PROP_FIELD(char2d, velocity, CW_T_VEC2, CW_PROP_DEFAULT, "px/s, y down; move_and_slide moves by it and clips it."),
  CW_PROP_FIELD_RANGE(char2d, floor_max_angle, CW_T_FLOAT, 0, 3.1415927, "Steepest walkable slope, radians."),
  CW_PROP_FIELD_RANGE(char2d, floor_snap_length, CW_T_FLOAT, 0, 1e6, "Snap down to a floor within this, pixels."),
  CW_PROP_FIELD_RANGE(char2d, step_height, CW_T_FLOAT, 0, 1e6, "Step up ledges this high, pixels."),
  CW_PROP_FIELD(char2d, max_slides, CW_T_INT, CW_PROP_DEFAULT, "Slide iterations per move."),
  CW_PROP_FIELD_RANGE(char2d, push_mass, CW_T_FLOAT, 0, 1e9, ">0: pushes rigid bodies as this mass."),
  CW_PROP_FIELD(char2d, ride_platforms, CW_T_BOOL, CW_PROP_DEFAULT, "Carried by the floor body's motion."),
  RO(char2d, on_floor, CW_T_BOOL, "Touching a floor after the last move."),
  RO(char2d, on_wall, CW_T_BOOL, "Touching a wall."),
  RO(char2d, on_ceiling, CW_T_BOOL, "Touching a ceiling."),
  RO(char2d, floor_normal, CW_T_VEC2, "Floor normal (y down)."),
  RO(char2d, wall_normal, CW_T_VEC2, "Wall normal (y down)."),
  RO(char2d, platform_velocity, CW_T_VEC2, "The floor's velocity, px/s."),
  CW_PROP_END,
};
static cw_node_type char_type = { .name = "CharacterBody2D", .parent = "PhysicsBody2D", .data_size = sizeof(char2d),
                                  .props = char_props, .doc = "A move-and-slide character (cw_character2d_move_and_slide).",
                                  .init = char_init };

/* area */
static const char *const area_signals[] = { "body_entered", "body_exited", "area_entered", "area_exited", NULL };
static void area_init(cw_node n, void *d) {
  (void)n;
  area2d *a = d;
  a->monitoring = a->monitorable = true;
}
static void overlap_get(cw_node n, void *d, cw_value *o) {
  (void)d;
  co2d *c = cw_node_data_t(n, co_t);
  *o = cw_val_int(c ? c->overlap_count : 0);
}
static const cw_prop area_props[] = {
  CW_PROP_FIELD(area2d, monitoring, CW_T_BOOL, CW_PROP_DEFAULT, "Report bodies and areas entering/exiting."),
  CW_PROP_FIELD(area2d, monitorable, CW_T_BOOL, CW_PROP_DEFAULT, "Other areas see this one."),
  CW_PROP_ACCESSOR("overlap_count", CW_T_INT, CW_PROP_READONLY | CW_PROP_EDITOR, overlap_get, NULL, "Bodies + areas inside now."),
  CW_PROP_END,
};
static cw_node_type area_type = { .name = "Area2D", .parent = "CollisionObject2D", .data_size = sizeof(area2d), .props = area_props,
                                  .signals = area_signals, .doc = "A sensor region: enter/exit signals.", .init = area_init };

/* shape */
static const char *const shape_names[] = { "RECT", "CIRCLE", "CAPSULE", "POLYGON", "SEGMENT", "CHAIN", NULL };
static void shape_init(cw_node n, void *d) {
  (void)n;
  shape2d *s = d;
  s->size = cw_v2(16, 16);
  s->radius = 8;
  s->height = 32;
}
static void shape_enter(cw_node n, void *d) {
  (void)d;
  mark_dirty(parent_object(n));
}
static void shape_exit(cw_node n, void *d) {
  (void)d;
  if (!cw__exiting_with_ancestor(n, co_t)) mark_dirty(parent_object(n));
}
static const cw_prop shape_props[] = {
  { .name = "shape", .type = CW_T_ENUM, .flags = CW_PROP_DEFAULT, .offset = offsetof(shape2d, shape), .size = sizeof(int),
    .enum_names = shape_names, .doc = "Collider kind." },
  CW_PROP_FIELD(shape2d, size, CW_T_VEC2, CW_PROP_DEFAULT, "RECT: full size, pixels."),
  CW_PROP_FIELD_RANGE(shape2d, radius, CW_T_FLOAT, 0, 1e6, "CIRCLE / CAPSULE radius, pixels."),
  CW_PROP_FIELD_RANGE(shape2d, height, CW_T_FLOAT, 0, 1e6, "CAPSULE: total height (vertical), pixels."),
  CW_PROP_FIELD(shape2d, points, CW_T_STRING, CW_PROP_DEFAULT, "POLYGON / SEGMENT / CHAIN points \"x y, ...\"."),
  CW_PROP_FIELD(shape2d, loop, CW_T_BOOL, CW_PROP_DEFAULT, "CHAIN: closed loop (solid inside)."),
  CW_PROP_FIELD(shape2d, disabled, CW_T_BOOL, CW_PROP_DEFAULT, "Not built."),
  CW_PROP_END,
};
static cw_node_type shape_type = { .name = "CollisionShape2D", .parent = "Node2D", .data_size = sizeof(shape2d), .props = shape_props,
                                   .doc = "A collider of its parent body or area.", .init = shape_init, .enter_tree = shape_enter,
                                   .exit_tree = shape_exit };

/* joint */
static const char *const joint_signals[] = { "broken", NULL };
static const cw_prop joint_props[] = {
  { .name = "joint_type", .type = CW_T_ENUM, .flags = CW_PROP_DEFAULT, .offset = offsetof(joint2d, joint_type), .size = sizeof(int),
    .enum_names = joint_names, .doc = "PIN/HINGE (revolute), SLIDER (prismatic), DISTANCE, SPRING, WELD, WHEEL, MOTOR." },
  CW_PROP_FIELD(joint2d, node_a, CW_T_STRING, CW_PROP_DEFAULT, "Path to body A (relative to the joint; empty = the world)."),
  CW_PROP_FIELD(joint2d, node_b, CW_T_STRING, CW_PROP_DEFAULT, "Path to body B."),
  CW_PROP_FIELD(joint2d, collide_connected, CW_T_BOOL, CW_PROP_DEFAULT, "The two bodies collide with each other."),
  CW_PROP_FIELD_RANGE(joint2d, break_force, CW_T_FLOAT, 0, 1e12, "Breaks above this force, N (0 = never)."),
  CW_PROP_FIELD_RANGE(joint2d, break_torque, CW_T_FLOAT, 0, 1e12, "Breaks above this torque, N*m (0 = never)."),
  CW_PROP_FIELD(joint2d, limit_enabled, CW_T_BOOL, CW_PROP_DEFAULT, "Limit the angle (rad) or translation (px)."),
  CW_PROP_FIELD(joint2d, lower, CW_T_FLOAT, CW_PROP_DEFAULT, "Lower limit (rad, clockwise / px)."),
  CW_PROP_FIELD(joint2d, upper, CW_T_FLOAT, CW_PROP_DEFAULT, "Upper limit."),
  CW_PROP_FIELD(joint2d, motor_enabled, CW_T_BOOL, CW_PROP_DEFAULT, "Drive at motor_speed."),
  CW_PROP_FIELD(joint2d, motor_speed, CW_T_FLOAT, CW_PROP_DEFAULT, "rad/s clockwise or px/s."),
  CW_PROP_FIELD_RANGE(joint2d, max_motor, CW_T_FLOAT, 0, 1e12, "Max motor torque / force."),
  CW_PROP_FIELD_RANGE(joint2d, stiffness, CW_T_FLOAT, 0, 1000, "Spring frequency, Hz (SPRING, WHEEL, soft WELD)."),
  CW_PROP_FIELD_RANGE(joint2d, damping, CW_T_FLOAT, 0, 100, "Spring damping ratio."),
  CW_PROP_FIELD_RANGE(joint2d, length, CW_T_FLOAT, 0, 1e9, "DISTANCE/SPRING rest length, px (0 = current)."),
  CW_PROP_END,
};
static cw_node_type joint_type = { .name = "Joint2D", .parent = "Node2D", .data_size = sizeof(joint2d), .props = joint_props,
                                   .signals = joint_signals, .doc = "Connects two bodies at the joint's position.",
                                   .init = joint_init, .enter_tree = joint_enter, .exit_tree = joint_exit };

/* casts */
#define CAST_COMMON_PROPS \
  CW_PROP_FIELD(cast2d, enabled, CW_T_BOOL, CW_PROP_DEFAULT, "Query after every step."), \
  CW_PROP_FIELD(cast2d, target_position, CW_T_VEC2, CW_PROP_DEFAULT, "End point, local pixels."), \
  CW_PROP_FIELD(cast2d, collision_mask, CW_T_INT, CW_PROP_DEFAULT, "Layers it hits (bits)."), \
  CW_PROP_FIELD(cast2d, collide_with_bodies, CW_T_BOOL, CW_PROP_DEFAULT, "Hit bodies."), \
  CW_PROP_FIELD(cast2d, collide_with_areas, CW_T_BOOL, CW_PROP_DEFAULT, "Hit areas."), \
  CW_PROP_FIELD(cast2d, exclude_parent, CW_T_BOOL, CW_PROP_DEFAULT, "Skip the parent body."), \
  RO(cast2d, colliding, CW_T_BOOL, "Hit something."), \
  RO(cast2d, collider, CW_T_NODE, "The node hit (closest)."), \
  RO(cast2d, collision_point, CW_T_VEC2, "Global hit point, pixels."), \
  RO(cast2d, collision_normal, CW_T_VEC2, "Hit normal (y down)."), \
  RO(cast2d, collision_fraction, CW_T_FLOAT, "0..1 along the cast."), \
  RO(cast2d, collision_count, CW_T_INT, "Hits (shape cast: up to max_results).")
static const cw_prop ray_props[] = { CAST_COMMON_PROPS, CW_PROP_END };
static const char *const qshape_names[] = { "CIRCLE", "RECT", "CAPSULE", NULL };
static const cw_prop scast_props[] = {
  CAST_COMMON_PROPS,
  { .name = "shape", .type = CW_T_ENUM, .flags = CW_PROP_DEFAULT, .offset = offsetof(cast2d, shape), .size = sizeof(int),
    .enum_names = qshape_names, .doc = "Swept shape." },
  CW_PROP_FIELD_RANGE(cast2d, radius, CW_T_FLOAT, 0, 1e6, "CIRCLE / CAPSULE radius."),
  CW_PROP_FIELD(cast2d, size, CW_T_VEC2, CW_PROP_DEFAULT, "RECT size."),
  CW_PROP_FIELD_RANGE(cast2d, height, CW_T_FLOAT, 0, 1e6, "CAPSULE height."),
  CW_PROP_FIELD(cast2d, max_results, CW_T_INT, CW_PROP_DEFAULT, "Hits kept (1..32)."),
  CW_PROP_END,
};
static cw_node_type ray_type = { .name = "RayCast2D", .parent = "Node2D", .data_size = sizeof(cast2d), .props = ray_props,
                                 .doc = "A ray refreshed after every physics step.", .init = cast_init, .enter_tree = cast_enter,
                                 .exit_tree = cast_exit };
static cw_node_type scast_type = { .name = "ShapeCast2D", .parent = "Node2D", .data_size = sizeof(cast2d), .props = scast_props,
                                   .doc = "A shape swept after every physics step (target 0 = overlap).", .init = cast_init,
                                   .enter_tree = cast_enter, .exit_tree = cast_exit };

void cw__register_physics2d_types(void) {
  node2d_t = cw_node_type_find("Node2D");
  cw_node_type_register(&co_type);
  co_t = &co_type;
  cw_node_type_register(&pb_type);
  pb_t = &pb_type;
  cw_node_type_register(&static_type);
  static_t = &static_type;
  cw_node_type_register(&kin_type);
  kin_t = &kin_type;
  cw_node_type_register(&rigid_type);
  rigid_t = &rigid_type;
  cw_node_type_register(&char_type);
  char_t = &char_type;
  cw_node_type_register(&area_type);
  area_t = &area_type;
  cw_node_type_register(&shape_type);
  shape_t = &shape_type;
  cw_node_type_register(&joint_type);
  joint_t = &joint_type;
  cw_node_type_register(&ray_type);
  ray_t = &ray_type;
  cw_node_type_register(&scast_type);
  scast_t = &scast_type;
}

cw_body2d cw_collision_object2d_body(cw_node n) {
  co2d *c = cw_node_data_t(n, co_t);
  return c ? c->body : (cw_body2d){ 0 };
}

cw_node cw_physics2d_body_node(cw_body2d b) {
  /* any tree's world, not only the current tree's: callers ask from
   * contact callbacks and from outside a tree update */
  uint64_t v;
  if (b.id)
    for (p2w *W = live_worlds; W; W = W->next_live)
      if (cw_map_get(&W->body_owner, b.id, &v)) return node_by_id(v);
  return (cw_node){ 0 };
}

/* ── rollback (cartwheel/net.h tree snapshots) ─────────────────────────
 * A snapshot restores node properties; what the physics layer keeps beside
 * them is restored here: a character's mover state and read-only outputs
 * (on_floor... feed the next move), and every body's place in the Box2D
 * world (a restored tree must not query bodies at their future poses). */
typedef struct char2d_snap {
  uint8_t ready, on_floor, on_wall, on_ceiling, m_floor, m_wall, m_ceiling, m_stepped, m_snapped, pad[7];
  cw_vec2 floor_normal, wall_normal, platform_velocity, m_pos, m_prev, m_vel, m_floor_n, m_wall_n, m_platform_v;
  uint64_t m_floor_body;
} char2d_snap;
int cw__char2d_state_save(cw_node n, void *data, uint8_t *out, int cap) {
  (void)n;
  char2d *ch = data;
  char2d_snap s;
  memset(&s, 0, sizeof s); /* padding hashes as zeros */
  if (cap < (int)sizeof s) return 0;
  s.ready = ch->mover_ready;
  s.on_floor = ch->on_floor;
  s.on_wall = ch->on_wall;
  s.on_ceiling = ch->on_ceiling;
  s.floor_normal = ch->floor_normal;
  s.wall_normal = ch->wall_normal;
  s.platform_velocity = ch->platform_velocity;
  if (ch->mover_ready) {
    const cw_mover2d *m = &ch->mover;
    s.m_floor = m->on_floor;
    s.m_wall = m->on_wall;
    s.m_ceiling = m->on_ceiling;
    s.m_stepped = m->stepped;
    s.m_snapped = m->snapped;
    s.m_pos = m->position;
    s.m_prev = m->prev_position;
    s.m_vel = m->velocity;
    s.m_floor_n = m->floor_normal;
    s.m_wall_n = m->wall_normal;
    s.m_platform_v = m->platform_velocity;
    s.m_floor_body = m->floor_body.id;
  }
  memcpy(out, &s, sizeof s);
  return (int)sizeof s;
}
void cw__char2d_state_load(cw_node n, void *data, const uint8_t *in, int len) {
  char2d *ch = data;
  char2d_snap s;
  if (len != (int)sizeof s) return;
  memcpy(&s, in, sizeof s);
  ch->on_floor = s.on_floor;
  ch->on_wall = s.on_wall;
  ch->on_ceiling = s.on_ceiling;
  ch->floor_normal = s.floor_normal;
  ch->wall_normal = s.wall_normal;
  ch->platform_velocity = s.platform_velocity;
  if (!s.ready) {
    ch->mover_ready = false; /* rebuilt from the node at the next move */
    return;
  }
  if (!ch->mover_ready) {
    co2d *c = cw_node_data_t(n, co_t);
    p2w *W = cw_tree_slot(cw_node_tree(n), "physics2d");
    if (!c || !W) return;
    ensure_mover(W, n, c, ch);
  }
  cw_mover2d *m = &ch->mover;
  m->on_floor = s.m_floor;
  m->on_wall = s.m_wall;
  m->on_ceiling = s.m_ceiling;
  m->stepped = s.m_stepped;
  m->snapped = s.m_snapped;
  m->position = s.m_pos;
  m->prev_position = s.m_prev;
  m->velocity = s.m_vel;
  m->floor_normal = s.m_floor_n;
  m->wall_normal = s.m_wall_n;
  m->platform_velocity = s.m_platform_v;
  m->floor_body.id = s.m_floor_body;
}
void cw__phys2d_state_restored(cw_tree *t) {
  p2w *W = cw_tree_slot(t, "physics2d");
  if (!W) return;
  for (int i = 0; i < W->objects.count; i++) {
    cw_node n = W->objects.data[i];
    co2d *c = cw_node_data_t(n, co_t);
    if (!c) continue;
    if (CW_HANDLE_IS_NULL(c->body) || !cw_body2d_valid(c->body)) create_body(W, n, c);
    if (c->dirty || config_hash(n, c) != c->cfg_hash) build_shapes(W, n, c);
    /* every body at its node's pose now (the mover state came with the snapshot) */
    cw_body2d_set_transform(c->body, node_xform(W, n));
    record_synced(n, c);
  }
}
CW_STATIC_ASSERT(sizeof(char2d_snap) - offsetof(char2d_snap, m_floor_body) == CW__CHAR_STATE_NOHASH, "the handle is the unhashed tail");
