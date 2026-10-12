/* 3D physics nodes over the Box3D wrapper (cartwheel/physics_nodes.h).
 * Mirrors phys2d_nodes.c step for step; nodes and the world share units
 * (meters, y up). Simulation code: + - * / sqrt and cartwheel/math.h only. */
#include <stdio.h>
#include <stdlib.h>

#include "phys_nodes_internal.h"

enum { K_STATIC, K_KINEMATIC, K_RIGID, K_CHARACTER, K_AREA };
enum { S_BOX, S_SPHERE, S_CAPSULE, S_CONVEX, S_TRIMESH };

typedef struct dead_shape {
  uint64_t shape;
  int32_t step;
} dead_shape;

typedef struct p3w {
  cw_tree *tree;
  cw_world3d world;
  CW_ARRAY(cw_node) objects;
  CW_ARRAY(cw_node) joints, casts;
  cw_map shape_owner, body_owner, joint_owner;
  CW_ARRAY(dead_shape) dead;
  cw__psig_list sigs;
  cw_body3d ground;
  int32_t steps;
  struct p3w *next_live; /* every tree's world, for cw_physics3d_body_node */
} p3w;
static p3w *live_worlds;

static const cw_node_type *co_t, *pb_t, *rigid_t, *char_t, *area_t, *shape_t, *joint_t, *ray_t, *scast_t, *node3d_t;

static void p3w_free(void *p) {
  p3w *W = p;
  for (p3w **l = &live_worlds; *l; l = &(*l)->next_live)
    if (*l == W) {
      *l = W->next_live;
      break;
    }
  if (cw_world3d_valid(W->world)) cw_world3d_destroy(W->world);
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

static p3w *world_of(cw_tree *t) {
  if (!t) return NULL;
  p3w *W = cw_tree_slot(t, "physics3d");
  if (W) return W;
  const cw_physics_nodes_config *c = cw_physics_nodes_get_config();
  W = cw_calloc(1, sizeof *W, CW_MEM_PHYSICS);
  W->tree = t;
  cw_world3d_def wd = cw_world3d_default_def();
  wd.gravity = c->gravity_3d;
  wd.substeps = c->substeps;
  for (int i = 0; i < CW_PHYS_LAYERS; i++) wd.layers[i] = c->layers_3d[i];
  W->world = cw_world3d_create(&wd);
  cw_map_init(&W->shape_owner, CW_MEM_PHYSICS);
  cw_map_init(&W->body_owner, CW_MEM_PHYSICS);
  cw_map_init(&W->joint_owner, CW_MEM_PHYSICS);
  cw_tree_set_slot(t, "physics3d", W, p3w_free);
  W->next_live = live_worlds;
  live_worlds = W;
  cw_tree_add_hook(t, 0, -100, pre_hook, NULL);
  cw_tree_add_hook(t, 0, 101, post_hook, NULL); /* after the 2D world's (100) */
  return W;
}

cw_world3d cw_physics3d_world(cw_tree *t) {
  p3w *W = world_of(t);
  return W ? W->world : (cw_world3d){ 0 };
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
static cw_node owner_of_shape(p3w *W, cw_shape3d s) {
  uint64_t v;
  return cw_map_get(&W->shape_owner, s.id, &v) ? node_by_id(v) : (cw_node){ 0 };
}

/* ── transforms ─────────────────────────────────────────────────────── */

static cw_xform3 node_xform(cw_node n) {
  const t3_mat4 *m = cw_node3d_global_transform(n);
  cw_xform3 x;
  t3_vec3 s;
  t3_mat4_decompose(m, &x.p, &x.q, &s);
  return x;
}

static cw_node parent3d(cw_node n) {
  for (cw_node p = cw_node_parent(n); !CW_HANDLE_IS_NULL(p); p = cw_node_parent(p))
    if (cw_node_data_t(p, node3d_t)) return p;
  return (cw_node){ 0 };
}

/* place the node so its global transform is x (its own scale kept) */
static void set_global(cw_node n, cw_xform3 x) {
  cw_node3d *d = cw_node_data_t(n, node3d_t);
  cw_node p = parent3d(n);
  if (CW_HANDLE_IS_NULL(p)) {
    d->position = x.p;
    cw_node3d_set_quaternion(n, x.q);
    return;
  }
  t3_mat4 inv, w, l;
  t3_mat4_invert(&inv, cw_node3d_global_transform(p));
  t3_mat4_compose(&w, x.p, x.q, (t3_vec3){ 1, 1, 1 });
  t3_mat4_multiply(&l, &inv, &w);
  t3_vec3 pos, s;
  t3_quat q;
  t3_mat4_decompose(&l, &pos, &q, &s);
  d->position = pos;
  cw_node3d_set_quaternion(n, q);
}

/* ── data ───────────────────────────────────────────────────────────── */

typedef struct co3d {
  int collision_layer;
  int32_t collision_mask;
  int kind;
  cw_body3d body;
  CW_ARRAY(uint64_t) shapes;
  CW_ARRAY(uint64_t) meshes; /* cw_mesh3d for trimesh shapes */
  uint32_t cfg_hash;
  bool dirty;
  t3_vec3 synced_pos;
  t3_quat synced_q;
  t3_mat4 synced_g;
  cw_map pairs;
  int32_t overlap_count;
} co3d;

typedef struct pb3d {
  float friction, bounce, rolling_resistance;
  t3_vec3 tangent_velocity;
  int32_t user_material;
  bool contact_monitor, hit_events;
} pb3d;

typedef struct rigid3d {
  float density, mass, gravity_scale, linear_damp, angular_damp;
  bool lock_rotation, bullet, can_sleep, freeze;
  int freeze_mode;
  t3_vec3 init_velocity, init_angular;
} rigid3d;

typedef struct char3d {
  t3_vec3 velocity;
  float floor_max_angle, floor_snap_length, step_height;
  int max_slides;
  float push_mass;
  bool ride_platforms;
  bool on_floor, on_wall, on_ceiling;
  t3_vec3 floor_normal, wall_normal, platform_velocity;
  cw_mover3d mover;
  bool mover_ready;
  uint32_t mover_hash;
  t3_vec3 feet;
} char3d;

typedef struct area3d {
  bool monitoring, monitorable;
} area3d;

typedef struct shape3d {
  int shape;
  t3_vec3 size;
  float radius, height;
  const char *points;
  const char *mesh;
  bool disabled;
} shape3d;

/* ── shapes ─────────────────────────────────────────────────────────── */

static cw_node parent_object(cw_node shape) {
  cw_node p = cw_node_parent(shape);
  return cw_node_data_t(p, co_t) ? p : (cw_node){ 0 };
}
static void mark_dirty(cw_node obj) {
  co3d *c = cw_node_data_t(obj, co_t);
  if (c) c->dirty = true;
}

static uint32_t config_hash(cw_node n, co3d *c) {
  uint32_t h = CW_HASH32_INIT;
  h = cw_hash32(&c->collision_layer, sizeof c->collision_layer, h);
  h = cw_hash32(&c->collision_mask, sizeof c->collision_mask, h);
  pb3d *pb = cw_node_data_t(n, pb_t);
  if (pb) h = cw_hash32(pb, sizeof *pb, h);
  rigid3d *rb = cw_node_data_t(n, rigid_t);
  if (rb) h = cw_hash32(&rb->density, sizeof rb->density, h), h = cw_hash32(&rb->mass, sizeof rb->mass, h);
  area3d *ar = cw_node_data_t(n, area_t);
  if (ar) h = cw_hash32(ar, sizeof *ar, h);
  for (int i = 0; i < cw_node_child_count(n); i++) {
    cw_node ch = cw_node_child(n, i);
    shape3d *s = cw_node_data_t(ch, shape_t);
    if (!s) continue;
    cw_node3d *d = cw_node_data_t(ch, node3d_t);
    h = cw_hash32(s, sizeof *s, h);
    h = cw_hash32(&d->position, sizeof d->position, h);
    h = cw_hash32(&d->quaternion, sizeof d->quaternion, h);
    h = cw_hash32(&d->scale, sizeof d->scale, h);
  }
  return h;
}

static void own_shape(p3w *W, co3d *c, cw_node n, cw_shape3d s) {
  if (CW_HANDLE_IS_NULL(s)) return;
  cw_array_push(&c->shapes, s.id);
  cw_map_set(&W->shape_owner, s.id, n.id);
}

static void destroy_shapes(p3w *W, co3d *c) {
  for (int i = 0; i < c->shapes.count; i++) {
    cw_shape3d s = { c->shapes.data[i] };
    if (cw_shape3d_valid(s)) cw_shape3d_destroy(s);
    dead_shape d = { s.id, W->steps };
    cw_array_push(&W->dead, d);
  }
  for (int i = 0; i < c->meshes.count; i++) cw_mesh3d_destroy((cw_mesh3d){ c->meshes.data[i] });
  cw_array_clear(&c->shapes);
  cw_array_clear(&c->meshes);
}

/* the mesh's vertices in the body's frame (frame arena); triangles too */
static int mesh_points(cw_node body, cw_node shape_node, const char *path, t3_vec3 **out, int32_t **tris, int *ntri) {
  cw_node mn = path && *path ? cw_node_get(shape_node, path) : (cw_node){ 0 };
  t3_mesh *mesh = cw__mesh3d_t3(mn);
  if (!mesh || !mesh->geometry || !mesh->geometry->attributes[T3_ATTR_POSITION]) {
    cw_warn("CollisionShape3D '%s': mesh '%s' is not a MeshInstance3D with geometry", cw_node_name(shape_node), path ? path : "");
    return 0;
  }
  t3_attribute *pa = mesh->geometry->attributes[T3_ATTR_POSITION];
  /* body <- mesh: inv(body global) * mesh node global * the mesh object's own local */
  t3_mat4 inv, mg, ml, tmp, m;
  t3_mat4_invert(&inv, cw_node3d_global_transform(body));
  mg = *cw_node3d_global_transform(mn);
  t3_object *mo = &mesh->base;
  t3_mat4_compose(&ml, mo->position, mo->quaternion, mo->scale);
  t3_mat4_multiply(&tmp, &mg, &ml);
  t3_mat4_multiply(&m, &inv, &tmp);
  int nv = pa->count;
  t3_vec3 *v = cw_frame_alloc(sizeof(t3_vec3) * (size_t)(nv + 1));
  const float *f = t3_attribute_f32(pa);
  for (int i = 0; i < nv; i++) v[i] = t3_vec3_apply_mat4((t3_vec3){ f[3 * i], f[3 * i + 1], f[3 * i + 2] }, &m);
  *out = v;
  if (tris) {
    t3_attribute *ia = mesh->geometry->index;
    int ni = ia ? ia->count : nv;
    int32_t *idx = cw_frame_alloc(sizeof(int32_t) * (size_t)(ni + 1));
    for (int i = 0; i < ni; i++)
      idx[i] = !ia ? i : ia->type == T3_UINT16 ? ((const uint16_t *)ia->array)[i] : ((const int32_t *)ia->array)[i];
    *tris = idx;
    *ntri = ni / 3;
  }
  return nv;
}

static int parse_points3(const char *text, t3_vec3 **out) {
  int cap = 0;
  for (const char *p = text ? text : ""; *p; p++) cap += (*p == ',' || *p == ' ' || *p == ';');
  t3_vec3 *pts = cw_frame_alloc(sizeof(t3_vec3) * (size_t)(cap / 3 + 2));
  int n = 0, k = 0;
  float v[3];
  const char *p = text ? text : "";
  while (*p) {
    char *end;
    double d = strtod(p, &end);
    if (end == p) { p++; continue; }
    v[k++] = (float)d;
    if (k == 3) {
      if (n < cap / 3 + 2) pts[n++] = (t3_vec3){ v[0], v[1], v[2] };
      k = 0;
    }
    p = end;
  }
  *out = pts;
  return n;
}

static void build_shapes(p3w *W, cw_node n, co3d *c) {
  destroy_shapes(W, c);
  cw_shape3d_def sd = cw_shape3d_default_def();
  pb3d *pb = cw_node_data_t(n, pb_t);
  if (pb) {
    sd.material.friction = pb->friction;
    sd.material.restitution = pb->bounce;
    sd.material.rolling_resistance = pb->rolling_resistance;
    sd.material.tangent_velocity[0] = pb->tangent_velocity.x;
    sd.material.tangent_velocity[1] = pb->tangent_velocity.y;
    sd.material.tangent_velocity[2] = pb->tangent_velocity.z;
    sd.material.user_material = (uint64_t)(uint32_t)pb->user_material;
    sd.hit_events = pb->hit_events;
  }
  rigid3d *rb = cw_node_data_t(n, rigid_t);
  if (rb) sd.density = rb->density;
  sd.layer = c->collision_layer & 31;
  sd.mask = (uint32_t)c->collision_mask;
  area3d *ar = cw_node_data_t(n, area_t);
  if (ar) {
    sd.sensor = true;
    sd.sensor_events = ar->monitoring || ar->monitorable;
  }
  for (int i = 0; i < cw_node_child_count(n); i++) {
    cw_node ch = cw_node_child(n, i);
    shape3d *s = cw_node_data_t(ch, shape_t);
    if (!s || s->disabled) continue;
    cw_node3d *d = cw_node_data_t(ch, node3d_t);
    t3_vec3 sc = { fabsf(d->scale.x), fabsf(d->scale.y), fabsf(d->scale.z) };
    t3_mat4 L;
    t3_mat4_compose(&L, d->position, d->quaternion, d->scale);
    switch (s->shape) {
    case S_BOX:
      own_shape(W, c, n, cw_shape3d_box(c->body, &sd, (t3_vec3){ s->size.x * 0.5f * sc.x, s->size.y * 0.5f * sc.y, s->size.z * 0.5f * sc.z },
                                        (cw_xform3){ d->position, d->quaternion }));
      break;
    case S_SPHERE:
      own_shape(W, c, n, cw_shape3d_sphere(c->body, &sd, d->position, s->radius * CW_MAX(sc.x, CW_MAX(sc.y, sc.z))));
      break;
    case S_CAPSULE: {
      float half = CW_MAX(0.0f, s->height * 0.5f - s->radius);
      t3_vec3 a = t3_vec3_apply_mat4((t3_vec3){ 0, -half, 0 }, &L), b = t3_vec3_apply_mat4((t3_vec3){ 0, half, 0 }, &L);
      own_shape(W, c, n, cw_shape3d_capsule(c->body, &sd, a, b, s->radius * CW_MAX(sc.x, sc.z)));
      break;
    }
    case S_CONVEX: case S_TRIMESH: {
      t3_vec3 *pts = NULL;
      int32_t *tris = NULL;
      int ntri = 0, k;
      if (s->points && *s->points) {
        k = parse_points3(s->points, &pts);
        for (int j = 0; j < k; j++) pts[j] = t3_vec3_apply_mat4(pts[j], &L);
      } else {
        k = mesh_points(n, ch, s->mesh, &pts, s->shape == S_TRIMESH ? &tris : NULL, &ntri);
      }
      if (k < 4) break;
      if (s->shape == S_TRIMESH && c->kind == K_STATIC && tris) {
        cw_mesh3d m = cw_mesh3d_create(pts, k, tris, ntri);
        if (!CW_HANDLE_IS_NULL(m)) {
          cw_array_push(&c->meshes, m.id);
          own_shape(W, c, n, cw_shape3d_mesh(c->body, &sd, m, (t3_vec3){ 1, 1, 1 }));
        }
      } else {
        if (s->shape == S_TRIMESH)
          cw_warn("CollisionShape3D '%s': TRIMESH needs a StaticBody3D (and a mesh); using its convex hull", cw_node_name(ch));
        /* weld exact duplicates (mesh seams, poles): the hull builder wants distinct points */
        int u = 0;
        for (int a = 0; a < k; a++) {
          bool dup = false;
          for (int b = 0; b < u && !dup; b++) dup = !memcmp(&pts[a], &pts[b], sizeof(t3_vec3));
          if (!dup) pts[u++] = pts[a];
        }
        own_shape(W, c, n, cw_shape3d_hull(c->body, &sd, pts, u));
      }
      break;
    }
    default: break;
    }
  }
  if (rb && rb->mass > 0 && cw_body3d_valid(c->body)) {
    b3BodyId bid = cw_body3d_raw(c->body);
    b3MassData md = b3Body_GetMassData(bid);
    if (md.mass > 0) {
      float k = rb->mass / md.mass;
      md.mass = rb->mass;
      md.inertia.cx = (b3Vec3){ md.inertia.cx.x * k, md.inertia.cx.y * k, md.inertia.cx.z * k };
      md.inertia.cy = (b3Vec3){ md.inertia.cy.x * k, md.inertia.cy.y * k, md.inertia.cy.z * k };
      md.inertia.cz = (b3Vec3){ md.inertia.cz.x * k, md.inertia.cz.y * k, md.inertia.cz.z * k };
      b3Body_SetMassData(bid, md);
    }
  }
  c->cfg_hash = config_hash(n, c);
  c->dirty = false;
}

/* ── body lifecycle ─────────────────────────────────────────────────── */

static cw_body_type body_type_for(cw_node n, co3d *c) {
  rigid3d *rb = cw_node_data_t(n, rigid_t);
  switch (c->kind) {
  case K_RIGID: return rb && rb->freeze ? (rb->freeze_mode ? CW_BODY_KINEMATIC : CW_BODY_STATIC) : CW_BODY_DYNAMIC;
  case K_STATIC: return CW_BODY_STATIC;
  default: return CW_BODY_KINEMATIC;
  }
}

static void record_synced(cw_node n, co3d *c) {
  cw_node3d *d = cw_node_data_t(n, node3d_t);
  c->synced_pos = d->position;
  c->synced_q = d->quaternion;
  c->synced_g = *cw_node3d_global_transform(n);
}

static void create_body(p3w *W, cw_node n, co3d *c) {
  cw_body3d_def bd = cw_body3d_default_def();
  bd.type = body_type_for(n, c);
  cw_xform3 x = node_xform(n);
  bd.position = x.p;
  bd.rotation = x.q;
  bd.name = cw_node_name(n);
  bd.interpolate = false;
  rigid3d *rb = cw_node_data_t(n, rigid_t);
  if (rb) {
    bd.gravity_scale = rb->gravity_scale;
    bd.linear_damping = rb->linear_damp;
    bd.angular_damping = rb->angular_damp;
    bd.lock_rotation = rb->lock_rotation;
    bd.bullet = rb->bullet;
    bd.sleep = rb->can_sleep;
    bd.linear_velocity = rb->init_velocity;
    bd.angular_velocity = rb->init_angular;
  }
  if (c->kind == K_CHARACTER) bd.lock_rotation = true;
  c->body = cw_body3d_create(W->world, &bd);
  cw_map_set(&W->body_owner, c->body.id, n.id);
  build_shapes(W, n, c);
  record_synced(n, c);
}

static void destroy_body(p3w *W, co3d *c) {
  if (CW_HANDLE_IS_NULL(c->body)) return;
  /* shapes go with the body; the meshes after them */
  for (int i = 0; i < c->shapes.count; i++) {
    dead_shape d = { c->shapes.data[i], W->steps };
    cw_array_push(&W->dead, d);
  }
  cw_array_clear(&c->shapes);
  cw_map_remove(&W->body_owner, c->body.id);
  if (cw_body3d_valid(c->body)) cw_body3d_destroy(c->body);
  for (int i = 0; i < c->meshes.count; i++) cw_mesh3d_destroy((cw_mesh3d){ c->meshes.data[i] });
  cw_array_clear(&c->meshes);
  c->body = (cw_body3d){ 0 };
  cw_map_free(&c->pairs);
  c->overlap_count = 0;
}

static void co_init(cw_node n, void *d) {
  (void)n;
  ((co3d *)d)->collision_mask = (int32_t)CW_PHYS_ALL_LAYERS;
}

static void co_enter(cw_node n, void *d) {
  co3d *c = d;
  p3w *W = world_of(cw_node_tree(n));
  cw_array_push(&W->objects, n);
  const cw_node_type *t = cw_node_get_type(n);
  c->kind = cw_node_type_is(t, "RigidBody3D") ? K_RIGID : cw_node_type_is(t, "CharacterBody3D") ? K_CHARACTER
          : cw_node_type_is(t, "KinematicBody3D") ? K_KINEMATIC : cw_node_type_is(t, "Area3D") ? K_AREA : K_STATIC;
  create_body(W, n, c);
}

static void co_exit(cw_node n, void *d) {
  co3d *c = d;
  p3w *W = cw_tree_slot(cw_node_tree(n), "physics3d");
  if (!W) return;
  list_remove(W->objects.data, &W->objects.count, n);
  destroy_body(W, c);
  char3d *ch = cw_node_data_t(n, char_t);
  if (ch) ch->mover_ready = false;
}

static void co_destroy(cw_node n, void *d) {
  (void)n;
  co3d *c = d;
  cw_array_free(&c->shapes);
  cw_array_free(&c->meshes);
  cw_map_free(&c->pairs);
}

/* ── per-tick sync ──────────────────────────────────────────────────── */

static void ensure_mover(p3w *W, cw_node n, co3d *c, char3d *ch);

static void sync_in(p3w *W, cw_node n, co3d *c, float dt) {
  if (CW_HANDLE_IS_NULL(c->body) || !cw_body3d_valid(c->body)) create_body(W, n, c);
  if (c->dirty || config_hash(n, c) != c->cfg_hash) build_shapes(W, n, c);
  cw_body_type want = body_type_for(n, c);
  if (cw_body3d_type(c->body) != want) cw_body3d_set_type(c->body, want);
  cw_node3d *d = cw_node_data_t(n, node3d_t);
  switch (c->kind) {
  case K_RIGID: case K_CHARACTER:
    if (memcmp(&d->position, &c->synced_pos, sizeof c->synced_pos) || memcmp(&d->quaternion, &c->synced_q, sizeof c->synced_q)) {
      cw_body3d_set_transform(c->body, node_xform(n));
      char3d *ch = cw_node_data_t(n, char_t);
      if (ch && ch->mover_ready) cw_mover3d_teleport(&ch->mover, t3_vec3_add(node_xform(n).p, ch->feet));
      record_synced(n, c);
    }
    if (c->kind == K_RIGID && want == CW_BODY_KINEMATIC) cw_body3d_set_target(c->body, node_xform(n), dt);
    if (c->kind == K_CHARACTER) ensure_mover(W, n, c, cw_node_data_t(n, char_t));
    break;
  case K_KINEMATIC:
    cw_body3d_set_target(c->body, node_xform(n), dt);
    record_synced(n, c);
    break;
  default: {
    const t3_mat4 *g = cw_node3d_global_transform(n);
    if (memcmp(g, &c->synced_g, sizeof c->synced_g)) {
      cw_body3d_set_transform(c->body, node_xform(n));
      record_synced(n, c);
    }
  }
  }
}

static void sync_out(cw_node n, co3d *c) {
  if (c->kind != K_RIGID || cw_body3d_type(c->body) != CW_BODY_DYNAMIC) return;
  set_global(n, cw_body3d_transform(c->body));
  record_synced(n, c);
}

static void pre_hook(cw_tree *t, double dt, void *u) {
  (void)dt;
  (void)u;
  if (cw_tree_paused(t)) return;
  p3w *W = cw_tree_slot(t, "physics3d");
  for (int i = 0; i < W->objects.count; i++) {
    co3d *c = cw_node_data_t(W->objects.data[i], co_t);
    if (c && c->kind != K_STATIC && c->kind != K_AREA) cw_node3d_snapshot(W->objects.data[i]);
  }
}

/* ── characters ─────────────────────────────────────────────────────── */

static void ensure_mover(p3w *W, cw_node n, co3d *c, char3d *ch) {
  float radius = 0.3f, height = 1.8f;
  t3_vec3 feet = { 0, 0, 0 };
  for (int i = 0; i < cw_node_child_count(n); i++) {
    cw_node sn = cw_node_child(n, i);
    shape3d *s = cw_node_data_t(sn, shape_t);
    if (!s || s->disabled) continue;
    cw_node3d *d = cw_node_data_t(sn, node3d_t);
    float sx = fabsf(d->scale.x), sy = fabsf(d->scale.y);
    float r = 0.3f, h = 1.8f;
    if (s->shape == S_CAPSULE) r = s->radius * sx, h = s->height * sy;
    else if (s->shape == S_SPHERE) r = s->radius * sx, h = 2 * r;
    else if (s->shape == S_BOX) r = CW_MIN(s->size.x, s->size.z) * 0.5f * sx, h = s->size.y * sy;
    radius = r;
    height = CW_MAX(h, 2 * r);
    feet = (t3_vec3){ d->position.x, d->position.y - height * 0.5f, d->position.z };
    break;
  }
  cw_mover3d_def md = cw_mover3d_default_def();
  md.radius = radius;
  md.height = height;
  md.max_floor_angle = ch->floor_max_angle;
  md.step_height = ch->step_height;
  md.snap_length = ch->floor_snap_length;
  md.max_iterations = ch->max_slides;
  md.mask = (uint32_t)c->collision_mask;
  md.push_mass = ch->push_mass;
  md.ride_platforms = ch->ride_platforms;
  uint32_t h = cw_hash32(&md, sizeof md, CW_HASH32_INIT);
  h = cw_hash32(&feet, sizeof feet, h);
  if (ch->mover_ready && h == ch->mover_hash) return;
  ch->feet = feet;
  if (ch->mover_ready) {
    t3_vec3 keep = ch->mover.position, v = ch->mover.velocity;
    cw_mover3d_init(&ch->mover, W->world, &md, keep);
    ch->mover.velocity = v;
  } else {
    cw_mover3d_init(&ch->mover, W->world, &md, t3_vec3_add(node_xform(n).p, feet));
  }
  ch->mover_ready = true;
  ch->mover_hash = h;
}

bool cw_character3d_move_and_slide(cw_node n, float dt) {
  co3d *c = cw_node_data_t(n, co_t);
  char3d *ch = cw_node_data_t(n, char_t);
  p3w *W = c && ch ? cw_tree_slot(cw_node_tree(n), "physics3d") : NULL;
  if (!W) return false;
  sync_in(W, n, c, dt);
  cw_mover3d *m = &ch->mover;
  m->velocity = ch->velocity;
  cw_xform3 home = cw_body3d_transform(c->body);
  cw_body3d_set_transform(c->body, (cw_xform3){ { home.p.x, home.p.y - 100000.0f, home.p.z }, home.q });
  cw_mover3d_move(m, dt);
  t3_vec3 origin = t3_vec3_sub(m->position, ch->feet);
  cw_body3d_set_transform(c->body, (cw_xform3){ origin, home.q });
  set_global(n, (cw_xform3){ origin, node_xform(n).q });
  record_synced(n, c);
  ch->velocity = m->velocity;
  ch->on_floor = m->on_floor;
  ch->on_wall = m->on_wall;
  ch->on_ceiling = m->on_ceiling;
  ch->floor_normal = m->floor_normal;
  ch->wall_normal = m->wall_normal;
  ch->platform_velocity = m->platform_velocity;
  return m->on_floor || m->on_wall || m->on_ceiling;
}

/* ── joints ─────────────────────────────────────────────────────────── */

enum { J_PIN, J_HINGE, J_SLIDER, J_DISTANCE, J_SPRING, J_WELD, J_CONE };
static const char *const joint_names[] = { "PIN", "HINGE", "SLIDER", "DISTANCE", "SPRING", "WELD", "CONE", NULL };

typedef struct joint3d {
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
  float cone_angle;
  cw_joint3d joint;
  uint32_t struct_hash, live_hash;
  bool broken;
} joint3d;

static cw_body3d body_at(cw_node from, const char *path) {
  if (!path || !*path) return (cw_body3d){ 0 };
  co3d *c = cw_node_data_t(cw_node_get(from, path), co_t);
  return c ? c->body : (cw_body3d){ 0 };
}

static uint32_t joint_struct_hash(cw_node n, joint3d *j, cw_body3d a, cw_body3d b) {
  uint32_t h = CW_HASH32_INIT;
  h = cw_hash32(&j->joint_type, sizeof j->joint_type, h);
  h = cw_hash32(&a.id, sizeof a.id, h);
  h = cw_hash32(&b.id, sizeof b.id, h);
  h = cw_hash32(&j->collide_connected, sizeof j->collide_connected, h);
  h = cw_hash32(&j->break_force, sizeof(float) * 2, h);
  h = cw_hash32(&j->length, sizeof(float) * 2, h);
  cw_node3d *d = cw_node_data_t(n, node3d_t);
  h = cw_hash32(&d->position, sizeof d->position, h);
  h = cw_hash32(&d->quaternion, sizeof d->quaternion, h);
  return h;
}
static uint32_t joint_live_hash(joint3d *j) {
  uint32_t h = cw_hash32(&j->limit_enabled, sizeof j->limit_enabled, CW_HASH32_INIT);
  h = cw_hash32(&j->lower, sizeof(float) * 2, h);
  h = cw_hash32(&j->motor_enabled, sizeof j->motor_enabled, h);
  h = cw_hash32(&j->motor_speed, sizeof(float) * 4, h);
  return h;
}
static void joint_apply_live(joint3d *j) {
  cw_joint3d_set_limits(j->joint, j->limit_enabled, j->lower, j->upper);
  cw_joint3d_set_motor(j->joint, j->motor_enabled, j->motor_speed, j->max_motor);
  if (j->joint_type == J_SPRING || j->stiffness > 0)
    cw_joint3d_set_spring(j->joint, j->joint_type == J_SPRING || j->stiffness > 0, j->stiffness, j->damping);
  j->live_hash = joint_live_hash(j);
}

static void joint_sync(p3w *W, cw_node n, joint3d *j) {
  if (j->broken) return;
  cw_body3d a = body_at(n, j->node_a), b = body_at(n, j->node_b);
  if (CW_HANDLE_IS_NULL(a) && (!j->node_a || !*j->node_a)) {
    if (CW_HANDLE_IS_NULL(W->ground) || !cw_body3d_valid(W->ground)) {
      cw_body3d_def gd = cw_body3d_default_def();
      gd.type = CW_BODY_STATIC;
      W->ground = cw_body3d_create(W->world, &gd);
    }
    a = W->ground;
  }
  bool valid = cw_joint3d_valid(j->joint);
  if (!cw_body3d_valid(a) || !cw_body3d_valid(b)) {
    if (valid) cw_joint3d_destroy(j->joint);
    j->joint = (cw_joint3d){ 0 };
    return;
  }
  uint32_t sh = joint_struct_hash(n, j, a, b);
  if (valid && sh == j->struct_hash) {
    if (joint_live_hash(j) != j->live_hash) joint_apply_live(j);
    return;
  }
  if (valid) {
    cw_map_remove(&W->joint_owner, j->joint.id);
    cw_joint3d_destroy(j->joint);
  }
  static const cw_joint_type map[] = { CW_JOINT_SPHERICAL, CW_JOINT_REVOLUTE, CW_JOINT_PRISMATIC, CW_JOINT_DISTANCE,
                                       CW_JOINT_DISTANCE, CW_JOINT_WELD, CW_JOINT_SPHERICAL };
  cw_joint3d_def jd = cw_joint3d_default_def(map[j->joint_type]);
  jd.a = a;
  jd.b = b;
  const t3_mat4 *g = cw_node3d_global_transform(n);
  t3_vec3 anchor = { g->e[12], g->e[13], g->e[14] };
  bool x_axis = j->joint_type == J_SLIDER || j->joint_type == J_DISTANCE || j->joint_type == J_SPRING;
  t3_vec3 axis = x_axis ? (t3_vec3){ g->e[0], g->e[1], g->e[2] } : (t3_vec3){ g->e[8], g->e[9], g->e[10] };
  float al = t3_vec3_length(axis);
  axis = al > 0 ? t3_vec3_scale(axis, 1 / al) : (t3_vec3){ 0, 0, 1 };
  cw_joint3d_def_anchor(&jd, anchor, axis);
  if (j->joint_type == J_DISTANCE || j->joint_type == J_SPRING) {
    jd.frame_b.p = (t3_vec3){ 0, 0, 0 };
    t3_vec3 dv = t3_vec3_sub(cw_body3d_transform(b).p, anchor);
    jd.length = j->length > 0 ? j->length : t3_vec3_length(dv);
    jd.spring = j->joint_type == J_SPRING;
    jd.hertz = j->stiffness > 0 ? j->stiffness : 4;
    jd.damping_ratio = j->damping;
  } else if (j->joint_type == J_WELD) {
    jd.hertz = j->stiffness;
    jd.damping_ratio = j->damping;
  }
  jd.limit = j->limit_enabled || j->joint_type == J_CONE;
  jd.lower = j->lower;
  jd.upper = j->upper;
  jd.cone_angle = j->cone_angle;
  jd.motor = j->motor_enabled;
  jd.motor_speed = j->motor_speed;
  jd.max_motor = j->max_motor;
  jd.collide_connected = j->collide_connected;
  jd.break_force = j->break_force;
  jd.break_torque = j->break_torque;
  j->joint = cw_joint3d_create(W->world, &jd);
  if (!CW_HANDLE_IS_NULL(j->joint)) cw_map_set(&W->joint_owner, j->joint.id, n.id);
  j->struct_hash = sh;
  j->live_hash = joint_live_hash(j);
}

static void joint_enter(cw_node n, void *d) {
  (void)d;
  cw_array_push(&world_of(cw_node_tree(n))->joints, n);
}
static void joint_exit(cw_node n, void *d) {
  joint3d *j = d;
  p3w *W = cw_tree_slot(cw_node_tree(n), "physics3d");
  if (!W) return;
  list_remove(W->joints.data, &W->joints.count, n);
  if (cw_joint3d_valid(j->joint)) {
    cw_map_remove(&W->joint_owner, j->joint.id);
    cw_joint3d_destroy(j->joint);
  }
  j->joint = (cw_joint3d){ 0 };
}
static void joint_init(cw_node n, void *d) {
  (void)n;
  joint3d *j = d;
  j->damping = 0.5f;
  j->max_motor = 1000;
  j->cone_angle = CW_PI / 4;
}

/* ── casts ──────────────────────────────────────────────────────────── */

enum { Q_SPHERE, Q_BOX, Q_CAPSULE };

typedef struct cast3d {
  bool enabled;
  t3_vec3 target_position;
  int32_t collision_mask;
  bool collide_with_bodies, collide_with_areas, exclude_parent;
  int shape;
  float radius, height;
  t3_vec3 size;
  int max_results;
  bool colliding;
  int32_t collision_count;
  cw_node collider;
  t3_vec3 collision_point, collision_normal;
  float collision_fraction;
} cast3d;

typedef struct filt_ctx {
  bool bodies, areas;
} filt_ctx;
static bool cast_filter(uint64_t shape_id, void *ctx) {
  filt_ctx *f = ctx;
  return cw_shape3d_is_sensor((cw_shape3d){ shape_id }) ? f->areas : f->bodies;
}

static void cast_update(p3w *W, cw_node n, cast3d *c, bool is_shape) {
  c->colliding = false;
  c->collision_count = 0;
  c->collider = (cw_node){ 0 };
  if (!c->enabled) return;
  const t3_mat4 *g = cw_node3d_global_transform(n);
  t3_vec3 o = { g->e[12], g->e[13], g->e[14] }, e = t3_vec3_apply_mat4(c->target_position, g);
  t3_vec3 tr = t3_vec3_sub(e, o);
  filt_ctx fc = { c->collide_with_bodies, c->collide_with_areas };
  cw_phys_filter f = cw_phys_filter_layers((uint32_t)c->collision_mask);
  f.sensors = c->collide_with_areas;
  f.fn = cast_filter;
  f.ctx = &fc;
  if (c->exclude_parent) {
    co3d *pc = cw_node_data_t(cw_node_parent(n), co_t);
    if (pc) f.ignore_body = pc->body.id;
  }
  cw_ray_hit3d hit;
  if (!is_shape) {
    if (cw_world3d_raycast(W->world, o, tr, &f, &hit)) {
      c->colliding = true;
      c->collision_count = 1;
      c->collider = owner_of_shape(W, hit.shape);
      c->collision_point = hit.point;
      c->collision_normal = hit.normal;
      c->collision_fraction = hit.fraction;
    }
    return;
  }
  cw_xform3 x = node_xform(n);
  cw_qshape3d q;
  if (c->shape == Q_BOX) q = cw_qshape3d_box(o, t3_vec3_scale(c->size, 0.5f), x.q);
  else if (c->shape == Q_CAPSULE) {
    float h = CW_MAX(0.0f, c->height * 0.5f - c->radius);
    q = cw_qshape3d_capsule(t3_vec3_apply_mat4((t3_vec3){ 0, -h, 0 }, g), t3_vec3_apply_mat4((t3_vec3){ 0, h, 0 }, g), c->radius);
  } else q = cw_qshape3d_sphere(o, c->radius);
  int cap = CW_CLAMP(c->max_results, 1, 32);
  if (c->target_position.x == 0 && c->target_position.y == 0 && c->target_position.z == 0) {
    cw_shape3d out[32];
    int k = cw_world3d_overlap(W->world, &q, &f, out, cap);
    if (k) {
      c->colliding = true;
      c->collision_count = k;
      c->collider = owner_of_shape(W, out[0]);
      c->collision_point = o;
      c->collision_normal = (t3_vec3){ 0, 0, 0 };
      c->collision_fraction = 0;
    }
  } else {
    cw_ray_hit3d hits[32];
    int k = cw_world3d_shapecast(W->world, &q, tr, &f, hits, cap);
    if (k) {
      c->colliding = true;
      c->collision_count = k;
      c->collider = owner_of_shape(W, hits[0].shape);
      c->collision_point = hits[0].point;
      c->collision_normal = hits[0].normal;
      c->collision_fraction = hits[0].fraction;
    }
  }
}

static void cast_enter(cw_node n, void *d) {
  (void)d;
  cw_array_push(&world_of(cw_node_tree(n))->casts, n);
}
static void cast_exit(cw_node n, void *d) {
  (void)d;
  p3w *W = cw_tree_slot(cw_node_tree(n), "physics3d");
  if (W) list_remove(W->casts.data, &W->casts.count, n);
}
static void cast_init(cw_node n, void *d) {
  (void)n;
  cast3d *c = d;
  c->enabled = true;
  c->target_position = (t3_vec3){ 0, -1, 0 };
  c->collision_mask = (int32_t)CW_PHYS_ALL_LAYERS;
  c->collide_with_bodies = true;
  c->exclude_parent = true;
  c->radius = 0.25f;
  c->size = (t3_vec3){ 0.5f, 0.5f, 0.5f };
  c->height = 1;
  c->max_results = 8;
}

static cast3d *cast_data(cw_node n, bool *is_shape) {
  cast3d *c = cw_node_data_t(n, ray_t);
  *is_shape = false;
  if (!c) {
    c = cw_node_data_t(n, scast_t);
    *is_shape = c != NULL;
  }
  return c;
}

void cw_raycast3d_force_update(cw_node n) {
  bool sh;
  cast3d *c = cast_data(n, &sh);
  p3w *W = cw_tree_slot(cw_node_tree(n), "physics3d");
  if (c && W) cast_update(W, n, c, sh);
}

/* ── the step ───────────────────────────────────────────────────────── */

static void contact_pair(p3w *W, cw_node a, cw_node b, int delta) {
  pb3d *pa = cw_node_data_t(a, pb_t);
  co3d *ca = cw_node_data_t(a, co_t);
  if (!pa || !ca || !pa->contact_monitor) return;
  int before = 0;
  uint64_t v;
  if (ca->pairs.slot_cap && cw_map_get(&ca->pairs, b.id, &v)) before = (int)v;
  int after = cw__pair_add(&ca->pairs, b.id, delta);
  if (!before && after) cw__psig_push(&W->sigs, a, cw__s_body_entered, b, 0, 1);
  if (before && !after) cw__psig_push(&W->sigs, a, cw__s_body_exited, b, 0, 1);
}

static void sensor_pair(p3w *W, cw_node area, cw_node other, int delta) {
  area3d *ar = cw_node_data_t(area, area_t);
  co3d *ca = cw_node_data_t(area, co_t);
  if (!ar || !ca) return;
  area3d *oa = cw_node_data_t(other, area_t);
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
  p3w *W = cw_tree_slot(t, "physics3d");
  float fdt = (float)dt;
  cw__phys_signals_init();
  for (int i = 0; i < W->objects.count; i++) {
    co3d *c = cw_node_data_t(W->objects.data[i], co_t);
    if (c) sync_in(W, W->objects.data[i], c, fdt);
  }
  for (int i = 0; i < W->joints.count; i++) {
    joint3d *j = cw_node_data_t(W->joints.data[i], joint_t);
    if (j) joint_sync(W, W->joints.data[i], j);
  }
  cw_world3d_step(W->world, fdt);
  W->steps++;
  for (int i = 0; i < W->objects.count; i++) {
    co3d *c = cw_node_data_t(W->objects.data[i], co_t);
    if (c) sync_out(W->objects.data[i], c);
  }
  for (int i = 0; i < W->casts.count; i++) {
    bool sh;
    cast3d *c = cast_data(W->casts.data[i], &sh);
    if (c) cast_update(W, W->casts.data[i], c, sh);
  }
  int ne;
  const cw_phys_event3d *ev = cw_world3d_events(W->world, &ne);
  for (int i = 0; i < ne; i++) {
    const cw_phys_event3d *e = &ev[i];
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
      pb3d *pa = cw_node_data_t(a, pb_t), *pbb = cw_node_data_t(b, pb_t);
      if (pa && pa->hit_events) cw__psig_push(&W->sigs, a, cw__s_body_hit, b, e->speed, 2);
      if (pbb && pbb->hit_events) cw__psig_push(&W->sigs, b, cw__s_body_hit, a, e->speed, 2);
      break;
    }
    case CW_PHYS_JOINT_BREAK: {
      uint64_t v;
      if (cw_map_get(&W->joint_owner, e->joint.id, &v)) {
        cw_node jn = node_by_id(v);
        joint3d *j = cw_node_data_t(jn, joint_t);
        if (j) {
          j->broken = true;
          j->joint = (cw_joint3d){ 0 };
        }
        cw_map_remove(&W->joint_owner, e->joint.id);
        cw__psig_push(&W->sigs, jn, cw__s_broken, (cw_node){ 0 }, 0, 0);
      }
      break;
    }
    default: break;
    }
  }
  int k = 0;
  for (int i = 0; i < W->dead.count; i++) {
    if (W->dead.data[i].step < W->steps - 2) cw_map_remove(&W->shape_owner, W->dead.data[i].shape);
    else W->dead.data[k++] = W->dead.data[i];
  }
  W->dead.count = k;
  cw__psig_deliver(t, &W->sigs);
}

/* ── reflection ─────────────────────────────────────────────────────── */

static const char *const *names3d(void) { return cw_physics_nodes_get_config()->layers_3d; }
static void layer_name_get(cw_node n, void *d, cw_value *o) {
  (void)n;
  const char *nm = names3d()[((co3d *)d)->collision_layer & 31];
  char buf[16];
  snprintf(buf, sizeof buf, "%d", ((co3d *)d)->collision_layer);
  *o = cw_val_string(nm ? nm : buf);
}
static void layer_name_set(cw_node n, void *d, const cw_value *v) {
  (void)n;
  int k = cw__layer_index(names3d(), v->s ? v->s : "");
  if (k >= 0) ((co3d *)d)->collision_layer = k;
}
static void mask_names_get(cw_node n, void *d, cw_value *o) {
  (void)n;
  char buf[1024];
  cw__layer_bits_text(names3d(), (uint32_t)((co3d *)d)->collision_mask, buf, sizeof buf);
  *o = cw_val_string(buf);
}
static void mask_names_set(cw_node n, void *d, const cw_value *v) {
  (void)n;
  ((co3d *)d)->collision_mask = (int32_t)cw__layer_bits(names3d(), v->s);
}
static void body_get(cw_node n, void *d, cw_value *o) {
  (void)n;
  *o = cw_val_int((int64_t)((co3d *)d)->body.id);
}

static const cw_prop co_props[] = {
  CW_PROP_FIELD_RANGE(co3d, collision_layer, CW_T_INT, 0, 31, "The layer its shapes sit on (0..31)."),
  CW_PROP_FIELD(co3d, collision_mask, CW_T_INT, CW_PROP_DEFAULT, "Layers it collides with (bits; -1 = all)."),
  CW_PROP_ACCESSOR("collision_layer_name", CW_T_STRING, CW_PROP_EDITOR, layer_name_get, layer_name_set, "The layer by project name."),
  CW_PROP_ACCESSOR("collision_mask_names", CW_T_STRING, CW_PROP_EDITOR, mask_names_get, mask_names_set, "The mask by project names (\"world|enemy\", \"all\")."),
  CW_PROP_ACCESSOR("body_id", CW_T_INT, CW_PROP_READONLY | CW_PROP_EDITOR, body_get, NULL, "The wrapper body handle (0 outside a tree)."),
  CW_PROP_END,
};
static cw_node_type co_type = { .name = "CollisionObject3D", .parent = "Node3D", .data_size = sizeof(co3d), .props = co_props,
                                .doc = "Base of 3D bodies and areas: collision layer and mask.", .init = co_init,
                                .destroy = co_destroy, .enter_tree = co_enter, .exit_tree = co_exit };

static const char *const body_signals[] = { "body_entered", "body_exited", "body_hit", NULL };
static void pb_init(cw_node n, void *d) {
  (void)n;
  ((pb3d *)d)->friction = 0.6f;
}
static const cw_prop pb_props[] = {
  CW_PROP_FIELD_RANGE(pb3d, friction, CW_T_FLOAT, 0, 10, "Friction (mixed sqrt(a*b))."),
  CW_PROP_FIELD_RANGE(pb3d, bounce, CW_T_FLOAT, 0, 1, "Restitution (mixed max)."),
  CW_PROP_FIELD_RANGE(pb3d, rolling_resistance, CW_T_FLOAT, 0, 1, "Rolling resistance (spheres, capsules)."),
  CW_PROP_FIELD(pb3d, tangent_velocity, CW_T_VEC3, CW_PROP_DEFAULT, "Conveyor surface velocity, shape-local m/s."),
  CW_PROP_FIELD(pb3d, user_material, CW_T_INT, CW_PROP_DEFAULT, "Your material id (hits, queries)."),
  CW_PROP_FIELD(pb3d, contact_monitor, CW_T_BOOL, CW_PROP_DEFAULT, "Emit body_entered/body_exited on contacts."),
  CW_PROP_FIELD(pb3d, hit_events, CW_T_BOOL, CW_PROP_DEFAULT, "Emit body_hit(other, speed) on impacts."),
  CW_PROP_END,
};
static cw_node_type pb_type = { .name = "PhysicsBody3D", .parent = "CollisionObject3D", .data_size = sizeof(pb3d), .props = pb_props,
                                .signals = body_signals, .doc = "Base of 3D bodies: surface material.", .init = pb_init };

static cw_node_type static_type = { .name = "StaticBody3D", .parent = "PhysicsBody3D", .doc = "A body that does not move by itself (moving the node teleports it)." };
static void interp_init(cw_node n, void *d) {
  (void)d;
  cw_node3d *nd = cw_node_data_t(n, node3d_t);
  if (nd) nd->interpolate = true;
}
static cw_node_type kin_type = { .name = "KinematicBody3D", .parent = "PhysicsBody3D", .init = interp_init,
                                 .doc = "Follows its node's transform with velocity (pushes, carries)." };

static const char *const freeze_names[] = { "STATIC", "KINEMATIC", NULL };
static void rigid_init(cw_node n, void *d) {
  interp_init(n, d);
  rigid3d *r = d;
  r->density = 1;
  r->gravity_scale = 1;
  r->can_sleep = true;
}
static void lv_get(cw_node n, void *d, cw_value *o) {
  rigid3d *r = d;
  co3d *c = cw_node_data_t(n, co_t);
  t3_vec3 v = c && cw_body3d_valid(c->body) ? cw_body3d_velocity(c->body) : r->init_velocity;
  *o = cw_val_vec3(v.x, v.y, v.z);
}
static void lv_set(cw_node n, void *d, const cw_value *v) {
  rigid3d *r = d;
  r->init_velocity = (t3_vec3){ v->v[0], v->v[1], v->v[2] };
  co3d *c = cw_node_data_t(n, co_t);
  if (c && cw_body3d_valid(c->body)) cw_body3d_set_velocity(c->body, r->init_velocity);
}
static void av_get(cw_node n, void *d, cw_value *o) {
  rigid3d *r = d;
  co3d *c = cw_node_data_t(n, co_t);
  t3_vec3 v = c && cw_body3d_valid(c->body) ? cw_body3d_angular_velocity(c->body) : r->init_angular;
  *o = cw_val_vec3(v.x, v.y, v.z);
}
static void av_set(cw_node n, void *d, const cw_value *v) {
  rigid3d *r = d;
  r->init_angular = (t3_vec3){ v->v[0], v->v[1], v->v[2] };
  co3d *c = cw_node_data_t(n, co_t);
  if (c && cw_body3d_valid(c->body)) cw_body3d_set_angular_velocity(c->body, r->init_angular);
}
static void sleeping_get(cw_node n, void *d, cw_value *o) {
  (void)d;
  co3d *c = cw_node_data_t(n, co_t);
  *o = cw_val_bool(c && cw_body3d_valid(c->body) && !cw_body3d_awake(c->body));
}
#define RIGID_LIVE(field, apply) \
  static void set_##field(cw_node n, void *d, const cw_value *v) { \
    rigid3d *r = d; \
    r->field = (float)v->f; \
    co3d *c = cw_node_data_t(n, co_t); \
    if (c && cw_body3d_valid(c->body)) apply(cw_body3d_raw(c->body), r->field); \
  } \
  static void get_##field(cw_node n, void *d, cw_value *o) { (void)n; *o = cw_val_float(((rigid3d *)d)->field); }
RIGID_LIVE(gravity_scale, b3Body_SetGravityScale)
RIGID_LIVE(linear_damp, b3Body_SetLinearDamping)
RIGID_LIVE(angular_damp, b3Body_SetAngularDamping)
static void lock_set(cw_node n, void *d, const cw_value *v) {
  rigid3d *r = d;
  r->lock_rotation = v->b;
  co3d *c = cw_node_data_t(n, co_t);
  if (c && cw_body3d_valid(c->body)) {
    b3MotionLocks ml = { 0 };
    ml.angularX = ml.angularY = ml.angularZ = v->b;
    b3Body_SetMotionLocks(cw_body3d_raw(c->body), ml);
  }
}
static void lock_get(cw_node n, void *d, cw_value *o) { (void)n; *o = cw_val_bool(((rigid3d *)d)->lock_rotation); }

static const cw_prop rigid_props[] = {
  CW_PROP_FIELD_RANGE(rigid3d, density, CW_T_FLOAT, 0, 1e6, "kg per cubic meter (mass from shapes)."),
  CW_PROP_FIELD_RANGE(rigid3d, mass, CW_T_FLOAT, 0, 1e9, "kg; 0 = from density."),
  CW_PROP_ACCESSOR("gravity_scale", CW_T_FLOAT, CW_PROP_DEFAULT, get_gravity_scale, set_gravity_scale, "Gravity multiplier."),
  CW_PROP_ACCESSOR("linear_damp", CW_T_FLOAT, CW_PROP_DEFAULT, get_linear_damp, set_linear_damp, "Linear damping."),
  CW_PROP_ACCESSOR("angular_damp", CW_T_FLOAT, CW_PROP_DEFAULT, get_angular_damp, set_angular_damp, "Angular damping."),
  CW_PROP_ACCESSOR("lock_rotation", CW_T_BOOL, CW_PROP_DEFAULT, lock_get, lock_set, "Never rotates."),
  CW_PROP_FIELD(rigid3d, bullet, CW_T_BOOL, CW_PROP_DEFAULT, "Continuous collision against other dynamic bodies."),
  CW_PROP_FIELD(rigid3d, can_sleep, CW_T_BOOL, CW_PROP_DEFAULT, "May fall asleep when resting."),
  CW_PROP_FIELD(rigid3d, freeze, CW_T_BOOL, CW_PROP_DEFAULT, "Frozen: a static or kinematic body (freeze_mode)."),
  { .name = "freeze_mode", .type = CW_T_ENUM, .flags = CW_PROP_DEFAULT, .offset = offsetof(rigid3d, freeze_mode), .size = sizeof(int),
    .enum_names = freeze_names, .doc = "What a frozen body becomes (KINEMATIC follows the node)." },
  CW_PROP_ACCESSOR("linear_velocity", CW_T_VEC3, CW_PROP_DEFAULT, lv_get, lv_set, "m/s (live)."),
  CW_PROP_ACCESSOR("angular_velocity", CW_T_VEC3, CW_PROP_DEFAULT, av_get, av_set, "rad/s (live)."),
  CW_PROP_ACCESSOR("sleeping", CW_T_BOOL, CW_PROP_READONLY | CW_PROP_EDITOR, sleeping_get, NULL, "Asleep."),
  CW_PROP_END,
};
static cw_node_type rigid_type = { .name = "RigidBody3D", .parent = "PhysicsBody3D", .data_size = sizeof(rigid3d),
                                   .props = rigid_props, .doc = "A simulated body; its transform drives the node.", .init = rigid_init };

void cw_rigid_body3d_apply_impulse(cw_node n, t3_vec3 j) {
  co3d *c = cw_node_data_t(n, co_t);
  if (c && c->kind == K_RIGID && cw_body3d_valid(c->body)) cw_body3d_apply_impulse(c->body, j, NULL);
}

static void char_init(cw_node n, void *d) {
  interp_init(n, d);
  char3d *c = d;
  c->floor_max_angle = CW_PI / 4;
  c->floor_snap_length = 0.25f;
  c->step_height = 0.3f;
  c->max_slides = 5;
  c->ride_platforms = true;
}
#define RO(T, field, ty, docs) { .name = #field, .type = ty, .flags = CW_PROP_READONLY | CW_PROP_EDITOR, .offset = (int)offsetof(T, field), .size = (int)sizeof(((T *)0)->field), .doc = docs }
static const cw_prop char_props[] = {
  CW_PROP_FIELD(char3d, velocity, CW_T_VEC3, CW_PROP_DEFAULT, "m/s; move_and_slide moves by it and clips it."),
  CW_PROP_FIELD_RANGE(char3d, floor_max_angle, CW_T_FLOAT, 0, 3.1415927, "Steepest walkable slope, radians."),
  CW_PROP_FIELD_RANGE(char3d, floor_snap_length, CW_T_FLOAT, 0, 1e6, "Snap down to a floor within this, m."),
  CW_PROP_FIELD_RANGE(char3d, step_height, CW_T_FLOAT, 0, 1e6, "Step up ledges this high, m."),
  CW_PROP_FIELD(char3d, max_slides, CW_T_INT, CW_PROP_DEFAULT, "Slide iterations per move."),
  CW_PROP_FIELD_RANGE(char3d, push_mass, CW_T_FLOAT, 0, 1e9, ">0: pushes rigid bodies as this mass."),
  CW_PROP_FIELD(char3d, ride_platforms, CW_T_BOOL, CW_PROP_DEFAULT, "Carried by the floor body's motion."),
  RO(char3d, on_floor, CW_T_BOOL, "Touching a floor after the last move."),
  RO(char3d, on_wall, CW_T_BOOL, "Touching a wall."),
  RO(char3d, on_ceiling, CW_T_BOOL, "Touching a ceiling."),
  RO(char3d, floor_normal, CW_T_VEC3, "Floor normal."),
  RO(char3d, wall_normal, CW_T_VEC3, "Wall normal."),
  RO(char3d, platform_velocity, CW_T_VEC3, "The floor's velocity."),
  CW_PROP_END,
};
static cw_node_type char_type = { .name = "CharacterBody3D", .parent = "PhysicsBody3D", .data_size = sizeof(char3d),
                                  .props = char_props, .doc = "A move-and-slide character (cw_character3d_move_and_slide).",
                                  .init = char_init };

static const char *const area_signals[] = { "body_entered", "body_exited", "area_entered", "area_exited", NULL };
static void area_init(cw_node n, void *d) {
  (void)n;
  area3d *a = d;
  a->monitoring = a->monitorable = true;
}
static void overlap_get(cw_node n, void *d, cw_value *o) {
  (void)d;
  co3d *c = cw_node_data_t(n, co_t);
  *o = cw_val_int(c ? c->overlap_count : 0);
}
static const cw_prop area_props[] = {
  CW_PROP_FIELD(area3d, monitoring, CW_T_BOOL, CW_PROP_DEFAULT, "Report bodies and areas entering/exiting."),
  CW_PROP_FIELD(area3d, monitorable, CW_T_BOOL, CW_PROP_DEFAULT, "Other areas see this one."),
  CW_PROP_ACCESSOR("overlap_count", CW_T_INT, CW_PROP_READONLY | CW_PROP_EDITOR, overlap_get, NULL, "Bodies + areas inside now."),
  CW_PROP_END,
};
static cw_node_type area_type = { .name = "Area3D", .parent = "CollisionObject3D", .data_size = sizeof(area3d), .props = area_props,
                                  .signals = area_signals, .doc = "A sensor region: enter/exit signals.", .init = area_init };

static const char *const shape_names[] = { "BOX", "SPHERE", "CAPSULE", "CONVEX", "TRIMESH", NULL };
static void shape_init(cw_node n, void *d) {
  (void)n;
  shape3d *s = d;
  s->size = (t3_vec3){ 1, 1, 1 };
  s->radius = 0.5f;
  s->height = 2;
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
  { .name = "shape", .type = CW_T_ENUM, .flags = CW_PROP_DEFAULT, .offset = offsetof(shape3d, shape), .size = sizeof(int),
    .enum_names = shape_names, .doc = "Collider kind." },
  CW_PROP_FIELD(shape3d, size, CW_T_VEC3, CW_PROP_DEFAULT, "BOX: full size, m."),
  CW_PROP_FIELD_RANGE(shape3d, radius, CW_T_FLOAT, 0, 1e6, "SPHERE / CAPSULE radius."),
  CW_PROP_FIELD_RANGE(shape3d, height, CW_T_FLOAT, 0, 1e6, "CAPSULE total height (along y)."),
  CW_PROP_FIELD(shape3d, points, CW_T_STRING, CW_PROP_DEFAULT, "CONVEX points \"x y z, ...\" (shape-local)."),
  CW_PROP_FIELD(shape3d, mesh, CW_T_STRING, CW_PROP_DEFAULT, "CONVEX / TRIMESH: path to a MeshInstance3D (relative to this node)."),
  CW_PROP_FIELD(shape3d, disabled, CW_T_BOOL, CW_PROP_DEFAULT, "Not built."),
  CW_PROP_END,
};
static cw_node_type shape_type = { .name = "CollisionShape3D", .parent = "Node3D", .data_size = sizeof(shape3d), .props = shape_props,
                                   .doc = "A collider of its parent body or area.", .init = shape_init, .enter_tree = shape_enter,
                                   .exit_tree = shape_exit };

static const char *const joint_signals[] = { "broken", NULL };
static const cw_prop joint_props[] = {
  { .name = "joint_type", .type = CW_T_ENUM, .flags = CW_PROP_DEFAULT, .offset = offsetof(joint3d, joint_type), .size = sizeof(int),
    .enum_names = joint_names, .doc = "PIN (ball), HINGE (about z), SLIDER (along x), DISTANCE, SPRING, WELD, CONE." },
  CW_PROP_FIELD(joint3d, node_a, CW_T_STRING, CW_PROP_DEFAULT, "Path to body A (relative to the joint; empty = the world)."),
  CW_PROP_FIELD(joint3d, node_b, CW_T_STRING, CW_PROP_DEFAULT, "Path to body B."),
  CW_PROP_FIELD(joint3d, collide_connected, CW_T_BOOL, CW_PROP_DEFAULT, "The two bodies collide with each other."),
  CW_PROP_FIELD_RANGE(joint3d, break_force, CW_T_FLOAT, 0, 1e12, "Breaks above this force, N (0 = never)."),
  CW_PROP_FIELD_RANGE(joint3d, break_torque, CW_T_FLOAT, 0, 1e12, "Breaks above this torque, N*m (0 = never)."),
  CW_PROP_FIELD(joint3d, limit_enabled, CW_T_BOOL, CW_PROP_DEFAULT, "Limit the angle / translation / length."),
  CW_PROP_FIELD(joint3d, lower, CW_T_FLOAT, CW_PROP_DEFAULT, "Lower limit."),
  CW_PROP_FIELD(joint3d, upper, CW_T_FLOAT, CW_PROP_DEFAULT, "Upper limit."),
  CW_PROP_FIELD(joint3d, motor_enabled, CW_T_BOOL, CW_PROP_DEFAULT, "Drive at motor_speed."),
  CW_PROP_FIELD(joint3d, motor_speed, CW_T_FLOAT, CW_PROP_DEFAULT, "rad/s or m/s."),
  CW_PROP_FIELD_RANGE(joint3d, max_motor, CW_T_FLOAT, 0, 1e12, "Max motor torque / force."),
  CW_PROP_FIELD_RANGE(joint3d, stiffness, CW_T_FLOAT, 0, 1000, "Spring frequency, Hz (SPRING, soft WELD)."),
  CW_PROP_FIELD_RANGE(joint3d, damping, CW_T_FLOAT, 0, 100, "Spring damping ratio."),
  CW_PROP_FIELD_RANGE(joint3d, length, CW_T_FLOAT, 0, 1e9, "DISTANCE/SPRING rest length (0 = current)."),
  CW_PROP_FIELD_RANGE(joint3d, cone_angle, CW_T_FLOAT, 0, 3.1415927, "CONE: cone limit, radians."),
  CW_PROP_END,
};
static cw_node_type joint_type = { .name = "Joint3D", .parent = "Node3D", .data_size = sizeof(joint3d), .props = joint_props,
                                   .signals = joint_signals, .doc = "Connects two bodies at the joint's position.",
                                   .init = joint_init, .enter_tree = joint_enter, .exit_tree = joint_exit };

#define CAST_COMMON_PROPS \
  CW_PROP_FIELD(cast3d, enabled, CW_T_BOOL, CW_PROP_DEFAULT, "Query after every step."), \
  CW_PROP_FIELD(cast3d, target_position, CW_T_VEC3, CW_PROP_DEFAULT, "End point, local."), \
  CW_PROP_FIELD(cast3d, collision_mask, CW_T_INT, CW_PROP_DEFAULT, "Layers it hits (bits)."), \
  CW_PROP_FIELD(cast3d, collide_with_bodies, CW_T_BOOL, CW_PROP_DEFAULT, "Hit bodies."), \
  CW_PROP_FIELD(cast3d, collide_with_areas, CW_T_BOOL, CW_PROP_DEFAULT, "Hit areas."), \
  CW_PROP_FIELD(cast3d, exclude_parent, CW_T_BOOL, CW_PROP_DEFAULT, "Skip the parent body."), \
  RO(cast3d, colliding, CW_T_BOOL, "Hit something."), \
  RO(cast3d, collider, CW_T_NODE, "The node hit (closest)."), \
  RO(cast3d, collision_point, CW_T_VEC3, "Global hit point."), \
  RO(cast3d, collision_normal, CW_T_VEC3, "Hit normal."), \
  RO(cast3d, collision_fraction, CW_T_FLOAT, "0..1 along the cast."), \
  RO(cast3d, collision_count, CW_T_INT, "Hits (shape cast: up to max_results).")
static const cw_prop ray_props[] = { CAST_COMMON_PROPS, CW_PROP_END };
static const char *const qshape_names[] = { "SPHERE", "BOX", "CAPSULE", NULL };
static const cw_prop scast_props[] = {
  CAST_COMMON_PROPS,
  { .name = "shape", .type = CW_T_ENUM, .flags = CW_PROP_DEFAULT, .offset = offsetof(cast3d, shape), .size = sizeof(int),
    .enum_names = qshape_names, .doc = "Swept shape." },
  CW_PROP_FIELD_RANGE(cast3d, radius, CW_T_FLOAT, 0, 1e6, "SPHERE / CAPSULE radius."),
  CW_PROP_FIELD(cast3d, size, CW_T_VEC3, CW_PROP_DEFAULT, "BOX size."),
  CW_PROP_FIELD_RANGE(cast3d, height, CW_T_FLOAT, 0, 1e6, "CAPSULE height."),
  CW_PROP_FIELD(cast3d, max_results, CW_T_INT, CW_PROP_DEFAULT, "Hits kept (1..32)."),
  CW_PROP_END,
};
static cw_node_type ray_type = { .name = "RayCast3D", .parent = "Node3D", .data_size = sizeof(cast3d), .props = ray_props,
                                 .doc = "A ray refreshed after every physics step.", .init = cast_init, .enter_tree = cast_enter,
                                 .exit_tree = cast_exit };
static cw_node_type scast_type = { .name = "ShapeCast3D", .parent = "Node3D", .data_size = sizeof(cast3d), .props = scast_props,
                                   .doc = "A shape swept after every physics step (target 0 = overlap).", .init = cast_init,
                                   .enter_tree = cast_enter, .exit_tree = cast_exit };

void cw__register_physics3d_types(void) {
  node3d_t = cw_node_type_find("Node3D");
  cw_node_type_register(&co_type);
  co_t = &co_type;
  cw_node_type_register(&pb_type);
  pb_t = &pb_type;
  cw_node_type_register(&static_type);
  cw_node_type_register(&kin_type);
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

cw_body3d cw_collision_object3d_body(cw_node n) {
  co3d *c = cw_node_data_t(n, co_t);
  return c ? c->body : (cw_body3d){ 0 };
}

cw_node cw_physics3d_body_node(cw_body3d b) {
  /* any tree's world, not only the current tree's: callers ask from
   * contact callbacks and from outside a tree update */
  uint64_t v;
  if (b.id)
    for (p3w *W = live_worlds; W; W = W->next_live)
      if (cw_map_get(&W->body_owner, b.id, &v)) return node_by_id(v);
  return (cw_node){ 0 };
}

/* ── rollback (cartwheel/net.h tree snapshots): see phys2d_nodes.c ───── */
typedef struct char3d_snap {
  uint8_t ready, on_floor, on_wall, on_ceiling, m_floor, m_wall, m_ceiling, m_stepped, m_snapped, pad[7];
  t3_vec3 floor_normal, wall_normal, platform_velocity, m_pos, m_prev, m_vel, m_floor_n, m_wall_n, m_platform_v;
  uint64_t m_floor_body;
} char3d_snap;
int cw__char3d_state_save(cw_node n, void *data, uint8_t *out, int cap) {
  (void)n;
  char3d *ch = data;
  char3d_snap s;
  memset(&s, 0, sizeof s);
  if (cap < (int)sizeof s) return 0;
  s.ready = ch->mover_ready;
  s.on_floor = ch->on_floor;
  s.on_wall = ch->on_wall;
  s.on_ceiling = ch->on_ceiling;
  s.floor_normal = ch->floor_normal;
  s.wall_normal = ch->wall_normal;
  s.platform_velocity = ch->platform_velocity;
  if (ch->mover_ready) {
    const cw_mover3d *m = &ch->mover;
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
void cw__char3d_state_load(cw_node n, void *data, const uint8_t *in, int len) {
  char3d *ch = data;
  char3d_snap s;
  if (len != (int)sizeof s) return;
  memcpy(&s, in, sizeof s);
  ch->on_floor = s.on_floor;
  ch->on_wall = s.on_wall;
  ch->on_ceiling = s.on_ceiling;
  ch->floor_normal = s.floor_normal;
  ch->wall_normal = s.wall_normal;
  ch->platform_velocity = s.platform_velocity;
  if (!s.ready) {
    ch->mover_ready = false;
    return;
  }
  if (!ch->mover_ready) {
    co3d *c = cw_node_data_t(n, co_t);
    p3w *W = cw_tree_slot(cw_node_tree(n), "physics3d");
    if (!c || !W) return;
    ensure_mover(W, n, c, ch);
  }
  cw_mover3d *m = &ch->mover;
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
void cw__phys3d_state_restored(cw_tree *t) {
  p3w *W = cw_tree_slot(t, "physics3d");
  if (!W) return;
  for (int i = 0; i < W->objects.count; i++) {
    cw_node n = W->objects.data[i];
    co3d *c = cw_node_data_t(n, co_t);
    if (!c) continue;
    if (CW_HANDLE_IS_NULL(c->body) || !cw_body3d_valid(c->body)) create_body(W, n, c);
    if (c->dirty || config_hash(n, c) != c->cfg_hash) build_shapes(W, n, c);
    cw_body3d_set_transform(c->body, node_xform(n));
    record_synced(n, c);
  }
}
CW_STATIC_ASSERT(sizeof(char3d_snap) - offsetof(char3d_snap, m_floor_body) == CW__CHAR_STATE_NOHASH, "the handle is the unhashed tail");
