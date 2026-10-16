/* The Cloth3D and Rope3D node types (cartwheel/cloth.h).
 *
 * Both build their simulation on the first fixed tick after they enter a
 * tree (so every body they attach to exists) and again when a property that
 * shapes them changes. They step in a tree hook right after the 3D world's
 * step (phase 0, order 102), in the order they entered the tree, and draw in
 * pre_render: the cloth's geometry (positions blended by the render alpha,
 * normals rebuilt) lives in the tree's 3D scene, in world space.
 *
 * Pins follow a frame: the node named by attach_to (or the node itself). A
 * physics body node (RigidBody3D, KinematicBody3D, StaticBody3D) is attached
 * to with cw_cloth_attach_body, so a dynamic one feels the pull; any other
 * Node3D's global transform moves the pins each tick (a cape on a character,
 * a flag on a moving pole). Skin limits (Cloth3D.skin_distance) use the same
 * frame. */
#include <math.h>
#include <string.h>

#include "cartwheel/cloth.h"
#include "cartwheel/physics_nodes.h"
#include "cartwheel/render3d.h"
#include "cartwheel/scene.h"

typedef t3_vec3 V;

static V mat_point(const t3_mat4 *m, V p) {
  const float *e = m->e;
  return (V){ e[0] * p.x + e[4] * p.y + e[8] * p.z + e[12], e[1] * p.x + e[5] * p.y + e[9] * p.z + e[13],
              e[2] * p.x + e[6] * p.y + e[10] * p.z + e[14] };
}
static V vsub(V a, V b) { return (V){ a.x - b.x, a.y - b.y, a.z - b.z }; }
static V qrot(t3_quat q, V v) {
  V u = { q.x, q.y, q.z };
  V t = { 2 * (u.y * v.z - u.z * v.y), 2 * (u.z * v.x - u.x * v.z), 2 * (u.x * v.y - u.y * v.x) };
  V c = { u.y * t.z - u.z * t.y, u.z * t.x - u.x * t.z, u.x * t.y - u.y * t.x };
  return (V){ v.x + q.w * t.x + c.x, v.y + q.w * t.y + c.y, v.z + q.w * t.z + c.z };
}
/* world point -> a body's local frame */
static V body_local(cw_body3d b, V world) {
  cw_xform3 x = cw_body3d_transform(b);
  return qrot((t3_quat){ -x.q.x, -x.q.y, -x.q.z, x.q.w }, vsub(world, x.p));
}

/* the tree's cloth and rope nodes, in the order they entered */
typedef struct cloth_tree {
  CW_ARRAY(cw_node) nodes;
} cloth_tree;

static const cw_node_type *cloth_t, *rope_t;
static void cloth_tick(cw_node n, void *d, float dt);
static void rope_tick(cw_node n, void *d, float dt);

static void ctree_free(void *p) {
  cloth_tree *ct = p;
  cw_array_free(&ct->nodes);
  cw_free(ct);
}

static void hook(cw_tree *t, double dt, void *u) {
  (void)u;
  if (cw_tree_paused(t)) return;
  cloth_tree *ct = cw_tree_slot(t, "cloth");
  if (!ct) return;
  for (int i = 0; i < ct->nodes.count; i++) {
    cw_node n = ct->nodes.data[i];
    void *d;
    if ((d = cw_node_data_t(n, cloth_t))) cloth_tick(n, d, (float)dt);
    else if ((d = cw_node_data_t(n, rope_t))) rope_tick(n, d, (float)dt);
  }
}

static void tree_add(cw_node n) {
  cw_tree *t = cw_node_tree(n);
  cloth_tree *ct = cw_tree_slot(t, "cloth");
  if (!ct) {
    ct = cw_calloc(1, sizeof *ct, CW_MEM_PHYSICS);
    cw_tree_set_slot(t, "cloth", ct, ctree_free);
    cw_physics3d_world(t); /* the world steps first: its hook is added before ours */
    cw_tree_add_hook(t, 0, 102, hook, NULL);
  }
  cw_array_push(&ct->nodes, n);
}

static void tree_remove(cw_node n) {
  cw_tree *t = cw_node_tree(n);
  cloth_tree *ct = t ? cw_tree_slot(t, "cloth") : NULL;
  if (!ct) return;
  for (int i = 0; i < ct->nodes.count; i++)
    if (ct->nodes.data[i].id == n.id) {
      memmove(&ct->nodes.data[i], &ct->nodes.data[i + 1], sizeof(cw_node) * (size_t)(ct->nodes.count - i - 1));
      ct->nodes.count--;
      return;
    }
}

/* the frame pins follow: a node's body, or its global transform */
typedef struct frame {
  cw_node node;
  cw_body3d body;
} frame;

static frame frame_of(cw_node self, const char *path) {
  frame f = { self, { 0 } };
  if (path && *path) {
    cw_node o = cw_node_get(self, path);
    if (CW_HANDLE_IS_NULL(o)) {
      cw_warn("%s: attach path '%s' names no node; following the node itself", cw_node_name(self), path);
    } else {
      f.node = o;
      f.body = cw_collision_object3d_body(o);
    }
  }
  return f;
}

static t3_geometry *release_geo(t3_geometry *g) {
  if (g) t3_release(g);
  return NULL;
}

/* ── Cloth3D ──────────────────────────────────────────────────────────── */

enum { PIN_NONE, PIN_TOP, PIN_LEFT, PIN_TOP_CORNERS, PIN_LEFT_CORNERS };
static const char *const pin_names[] = { "NONE", "TOP", "LEFT", "TOP_CORNERS", "LEFT_CORNERS", NULL };
enum { PAT_NONE, PAT_STRIPES, PAT_CHECKER, PAT_CROSS };
static const char *const pattern_names[] = { "NONE", "STRIPES", "CHECKER", "CROSS", NULL };

typedef struct cloth3d {
  float width, height;
  int columns, rows;
  int pin;
  const char *attach_to;
  float skin_distance;
  float mass, stretch_compliance, bend_compliance, tether_slack, damping, thickness, friction;
  int substeps;
  int collision_mask;
  bool two_way, self_collision, quality_scaled;
  float wind[3];
  float wind_drag, wind_lift, turbulence;
  float color[4], color2[4];
  int pattern;
  float roughness, sheen;
  bool cast_shadow;
  /* runtime */
  int32_t hash;
  bool dirty;
  cw_cloth cloth;
  frame fr;
  V *rest_local;           /* rest positions in the follow frame (node frames only) */
  V *targets;
  uint8_t *pinned;
  t3_mesh *mesh;
  t3_geometry *geo;
  t3_material *mat;
} cloth3d;

static void cloth_init(cw_node n, void *data) {
  (void)n;
  cloth3d *d = data;
  cw_cloth_def def = cw_cloth_default_def();
  d->width = 1, d->height = 1.4f;
  d->columns = 16, d->rows = 22;
  d->pin = PIN_TOP;
  d->attach_to = cw_intern("");
  d->mass = 0.5f;
  d->stretch_compliance = def.stretch_compliance, d->bend_compliance = def.bend_compliance;
  d->tether_slack = def.tether_slack, d->damping = def.damping, d->thickness = def.thickness;
  d->friction = def.friction, d->substeps = def.substeps;
  d->collision_mask = -1;
  d->wind_drag = 1, d->wind_lift = 0.3f;
  d->color[0] = 0.6f, d->color[1] = 0.08f, d->color[2] = 0.06f, d->color[3] = 1;
  d->color2[0] = 0.9f, d->color2[1] = 0.85f, d->color2[2] = 0.75f, d->color2[3] = 1;
  d->roughness = 0.85f;
  d->sheen = 0.5f;
  d->cast_shadow = true;
  d->dirty = true;
}

static void cloth_teardown(cloth3d *d) {
  if (cw_cloth_valid(d->cloth)) cw_cloth_destroy(d->cloth);
  d->cloth = (cw_cloth){ 0 };
  if (d->mesh) {
    if (d->mesh->base.parent) t3_object_remove(d->mesh->base.parent, d->mesh);
    t3_release(d->mesh);
  }
  d->mesh = NULL;
  d->geo = release_geo(d->geo);
  if (d->mat) t3_release(d->mat);
  d->mat = NULL;
  cw_free(d->rest_local), cw_free(d->targets), cw_free(d->pinned);
  d->rest_local = d->targets = NULL;
  d->pinned = NULL;
}

static bool is_pinned(int pin, int i, int j, int nu, int nv) {
  switch (pin) {
  case PIN_TOP: return j == 0;
  case PIN_LEFT: return i == 0;
  case PIN_TOP_CORNERS: return j == 0 && (i == 0 || i == nu - 1);
  case PIN_LEFT_CORNERS: return i == 0 && (j == 0 || j == nv - 1);
  default: return false;
  }
}

static void pattern_colours(cloth3d *d, t3_geometry *g, int nu, int nv) {
  if (d->pattern == PAT_NONE) return;
  float *col = cw_alloc(sizeof(float) * 3 * (size_t)(nu * nv), CW_MEM_RENDER);
  for (int j = 0; j < nv; j++)
    for (int i = 0; i < nu; i++) {
      float s = (float)i / (float)(nu - 1), t = (float)j / (float)(nv - 1);
      bool two = false;
      if (d->pattern == PAT_STRIPES) two = ((int)(t * 3.0f - 1e-4f)) == 1;
      else if (d->pattern == PAT_CHECKER) two = (((int)(s * 6)) + ((int)(t * 6))) & 1;
      else two = fabsf(t - 0.5f) < 0.09f || fabsf(s - 0.38f) < 0.07f;
      const float *c = two ? d->color2 : d->color;
      float *o = &col[3 * (i + j * nu)];
      o[0] = c[0], o[1] = c[1], o[2] = c[2];
    }
  t3_geometry_set_attribute(g, T3_ATTR_COLOR, t3_attribute_new(T3_FLOAT32, col, nu * nv, 3));
  cw_free(col);
}

static void cloth_build(cw_node n, cloth3d *d) {
  cloth_teardown(d);
  d->dirty = false;
  cw_tree *t = cw_node_tree(n);
  if (!t) return;
  int nu = d->columns + 1, nv = d->rows + 1;
  const t3_mat4 *m = cw_node3d_global_transform(n);
  /* the grid hangs in the node's XY plane: x along +x, rows down -y */
  V o = mat_point(m, (V){ 0, 0, 0 });
  V u = vsub(mat_point(m, (V){ d->width, 0, 0 }), o), v = vsub(mat_point(m, (V){ 0, -d->height, 0 }), o);
  float *inv = cw_alloc(sizeof(float) * (size_t)(nu * nv), CW_MEM_PHYSICS);
  float *maxd = d->skin_distance > 0 ? cw_alloc(sizeof(float) * (size_t)(nu * nv), CW_MEM_PHYSICS) : NULL;
  d->pinned = cw_calloc((size_t)(nu * nv), 1, CW_MEM_PHYSICS);
  for (int j = 0; j < nv; j++)
    for (int i = 0; i < nu; i++) {
      int k = i + j * nu;
      d->pinned[k] = is_pinned(d->pin, i, j, nu, nv);
      inv[k] = d->pinned[k] ? 0 : 1;
      if (maxd) { /* looser away from the pinned side */
        float s = d->pin == PIN_LEFT || d->pin == PIN_LEFT_CORNERS ? (float)i / (float)(nu - 1) : (float)j / (float)(nv - 1);
        maxd[k] = d->skin_distance * s;
      }
    }
  cw_cloth_def def = cw_cloth_default_def();
  def.inv_mass = inv;
  def.max_distance = maxd;
  def.mass = d->mass, def.stretch_compliance = d->stretch_compliance, def.bend_compliance = d->bend_compliance;
  def.tether_slack = d->tether_slack, def.damping = d->damping, def.thickness = d->thickness;
  def.friction = d->friction, def.substeps = d->substeps, def.collide_mask = (uint32_t)d->collision_mask;
  def.two_way = d->two_way, def.self_collision = d->self_collision, def.quality_scaled = d->quality_scaled;
  d->cloth = cw_cloth_create_grid(cw_physics3d_world(t), &def, nu, nv, o, u, v);
  cw_free(inv), cw_free(maxd);
  if (!cw_cloth_valid(d->cloth)) return;
  cw_cloth_set_wind(d->cloth, (V){ d->wind[0], d->wind[1], d->wind[2] }, d->wind_drag, d->wind_lift, d->turbulence);
  /* pins: on the body, or following the frame node */
  d->fr = frame_of(n, d->attach_to);
  int cnt;
  const V *x = cw_cloth_positions(d->cloth, &cnt);
  t3_mat4 inv_m;
  t3_mat4_invert(&inv_m, cw_node3d_global_transform(d->fr.node));
  d->rest_local = cw_alloc(sizeof(V) * (size_t)cnt, CW_MEM_PHYSICS);
  d->targets = cw_alloc(sizeof(V) * (size_t)cnt, CW_MEM_PHYSICS);
  for (int k = 0; k < cnt; k++) d->rest_local[k] = mat_point(&inv_m, x[k]);
  if (cw_body3d_valid(d->fr.body))
    for (int k = 0; k < cnt; k++)
      if (d->pinned[k]) cw_cloth_attach_body(d->cloth, k, d->fr.body, body_local(d->fr.body, x[k]));
  /* drawing */
  d->geo = cw_cloth_geometry_new(d->cloth);
  pattern_colours(d, d->geo, nu, nv);
  t3_material *mat = d->sheen > 0 ? t3_mesh_physical_material_new(0xffffff) : t3_mesh_standard_material_new(0xffffff);
  mat->side = T3_DOUBLE_SIDE;
  mat->roughness = d->roughness;
  mat->metalness = 0;
  if (d->pattern != PAT_NONE) mat->vertex_colors = true;
  else mat->color = (t3_color){ d->color[0], d->color[1], d->color[2] };
  if (d->sheen > 0) {
    mat->sheen = d->sheen;
    mat->sheen_roughness = 0.8f;
    mat->sheen_color = (t3_color){ 1, 1, 1 };
  }
  d->mat = mat;
  d->mesh = t3_mesh_new(d->geo, d->mat);
  d->mesh->base.cast_shadow = d->mesh->base.receive_shadow = d->cast_shadow;
  d->mesh->base.receive_shadow = true;
  t3_object_add(&cw_render3d_scene(t)->base, d->mesh);
}

static void cloth_tick(cw_node n, void *data, float dt) {
  cloth3d *d = data;
  if (d->dirty || !cw_cloth_valid(d->cloth)) cloth_build(n, d);
  if (!cw_cloth_valid(d->cloth)) return;
  int cnt = cw_cloth_vertex_count(d->cloth);
  /* the frame node's transform: a body node's is its body's, written after the world step */
  bool follow = !cw_body3d_valid(d->fr.body);
  if ((follow || d->skin_distance > 0) && cw_node_in_tree(d->fr.node)) {
    const t3_mat4 *m = cw_node3d_global_transform(d->fr.node);
    for (int k = 0; k < cnt; k++) {
      d->targets[k] = mat_point(m, d->rest_local[k]);
      if (follow && d->pinned[k]) cw_cloth_set_pin_target(d->cloth, k, d->targets[k]);
    }
    if (d->skin_distance > 0) cw_cloth_set_skin_targets(d->cloth, d->targets);
  }
  cw_cloth_step(d->cloth, dt);
  d->hash = (int32_t)cw_cloth_hash(d->cloth);
}

static void cloth_pre_render(cw_node n, void *data, double alpha) {
  (void)n;
  cloth3d *d = data;
  if (d->geo && cw_cloth_valid(d->cloth)) cw_cloth_update_geometry(d->cloth, d->geo, (float)alpha);
}

static void cloth_enter(cw_node n, void *data) {
  ((cloth3d *)data)->dirty = true;
  tree_add(n);
}
static void cloth_exit(cw_node n, void *data) {
  tree_remove(n);
  cloth_teardown(data);
  ((cloth3d *)data)->dirty = true;
}
static void cloth_destroy(cw_node n, void *data) {
  (void)n;
  cloth_teardown(data);
}
static void cloth_changed(cw_node n, void *data, const cw_prop *p) {
  (void)n;
  cloth3d *d = data;
  if (cw_cloth_valid(d->cloth) && (!strcmp(p->name, "wind") || !strncmp(p->name, "wind_", 5) || !strcmp(p->name, "turbulence"))) {
    cw_cloth_set_wind(d->cloth, (V){ d->wind[0], d->wind[1], d->wind[2] }, d->wind_drag, d->wind_lift, d->turbulence);
    return;
  }
  d->dirty = true;
}

static const cw_prop cloth_props[] = {
  CW_PROP_FIELD_RANGE(cloth3d, width, CW_T_FLOAT, 0.01, 1000, "Metres along the node's +x."),
  CW_PROP_FIELD_RANGE(cloth3d, height, CW_T_FLOAT, 0.01, 1000, "Metres down the node's -y."),
  CW_PROP_FIELD_RANGE(cloth3d, columns, CW_T_INT, 1, 255, "Quads across (particles: columns + 1)."),
  CW_PROP_FIELD_RANGE(cloth3d, rows, CW_T_INT, 1, 255, "Quads down (particles: rows + 1)."),
  { .name = "pin", .type = CW_T_ENUM, .flags = CW_PROP_DEFAULT, .offset = offsetof(cloth3d, pin), .size = sizeof(int),
    .enum_names = pin_names, .doc = "Which particles hold: the top edge (banner, cape), the left edge (flag on a pole), corners or none." },
  CW_PROP_FIELD(cloth3d, attach_to, CW_T_STRING, CW_PROP_DEFAULT,
                "Path of the node the pins follow (a body node pulls on the body); empty = this node."),
  CW_PROP_FIELD_RANGE(cloth3d, skin_distance, CW_T_FLOAT, 0, 100,
                      "> 0: particles stay within this many metres (at the far edge) of the rest shape riding the pins' frame."),
  CW_PROP_FIELD_RANGE(cloth3d, mass, CW_T_FLOAT, 0.001, 10000, "Kilograms of the whole cloth."),
  CW_PROP_FIELD_RANGE(cloth3d, stretch_compliance, CW_T_FLOAT, 0, 1, "m/N of the edges (0 = as stiff as the substeps allow)."),
  CW_PROP_FIELD_RANGE(cloth3d, bend_compliance, CW_T_FLOAT, -1, 100, "m/N across neighbouring triangles (< 0 = no bending)."),
  CW_PROP_FIELD_RANGE(cloth3d, tether_slack, CW_T_FLOAT, 0, 2, "Tethers hold particles within rest distance x this of the pins (0 = off)."),
  CW_PROP_FIELD_RANGE(cloth3d, damping, CW_T_FLOAT, 0, 100, "Share of velocity lost per second."),
  CW_PROP_FIELD_RANGE(cloth3d, thickness, CW_T_FLOAT, 0, 1, "Metres kept from shapes (and itself)."),
  CW_PROP_FIELD_RANGE(cloth3d, friction, CW_T_FLOAT, 0, 1, "Against shapes."),
  CW_PROP_FIELD_RANGE(cloth3d, substeps, CW_T_INT, 1, 64, "Per tick, one iteration each."),
  CW_PROP_FIELD(cloth3d, collision_mask, CW_T_INT, CW_PROP_DEFAULT, "Layer bits it collides with (-1 = all, 0 = none)."),
  CW_PROP_FIELD(cloth3d, two_way, CW_T_BOOL, CW_PROP_DEFAULT, "Collisions push dynamic bodies back."),
  CW_PROP_FIELD(cloth3d, self_collision, CW_T_BOOL, CW_PROP_DEFAULT, "Particles keep thickness apart (costly)."),
  CW_PROP_FIELD(cloth3d, quality_scaled, CW_T_BOOL, CW_PROP_DEFAULT, "Cosmetic: Low graphics settings halve the substeps."),
  { .name = "wind", .type = CW_T_VEC3, .flags = CW_PROP_DEFAULT, .offset = offsetof(cloth3d, wind), .size = sizeof(float) * 3,
    .doc = "Air velocity, m/s (world)." },
  CW_PROP_FIELD_RANGE(cloth3d, wind_drag, CW_T_FLOAT, 0, 100, "Drag coefficient (1 looks like fabric)."),
  CW_PROP_FIELD_RANGE(cloth3d, wind_lift, CW_T_FLOAT, 0, 100, "Lift coefficient."),
  CW_PROP_FIELD_RANGE(cloth3d, turbulence, CW_T_FLOAT, 0, 1, "Gusts over the cloth and over time."),
  CW_PROP_FIELD(cloth3d, color, CW_T_COLOR, CW_PROP_DEFAULT, "Albedo (linear)."),
  CW_PROP_FIELD(cloth3d, color2, CW_T_COLOR, CW_PROP_DEFAULT, "The pattern's second colour."),
  { .name = "pattern", .type = CW_T_ENUM, .flags = CW_PROP_DEFAULT, .offset = offsetof(cloth3d, pattern), .size = sizeof(int),
    .enum_names = pattern_names, .doc = "Vertex-colour pattern of color and color2." },
  CW_PROP_FIELD_RANGE(cloth3d, roughness, CW_T_FLOAT, 0, 1, "PBR roughness."),
  CW_PROP_FIELD_RANGE(cloth3d, sheen, CW_T_FLOAT, 0, 1, "Fabric sheen (MeshPhysicalMaterial; 0 = standard material)."),
  CW_PROP_FIELD(cloth3d, cast_shadow, CW_T_BOOL, CW_PROP_DEFAULT, "Casts shadows."),
  { .name = "hash", .type = CW_T_INT, .flags = CW_PROP_EDITOR | CW_PROP_READONLY, .offset = offsetof(cloth3d, hash),
    .size = sizeof(int32_t), .doc = "The simulation's running hash (cw_cloth_hash)." },
  CW_PROP_END,
};

static cw_node_type cloth_type = {
  .name = "Cloth3D", .parent = "Node3D", .data_size = sizeof(cloth3d), .props = cloth_props,
  .doc = "A rectangle of cloth (cloth.h): flags, banners, capes, curtains. Pinned to this node or attach_to.",
  .init = cloth_init, .destroy = cloth_destroy, .enter_tree = cloth_enter, .exit_tree = cloth_exit,
  .pre_render = cloth_pre_render, .prop_changed = cloth_changed,
};

cw_cloth cw_cloth3d_get(cw_node n) {
  cloth3d *d = cloth_t ? cw_node_data_t(n, cloth_t) : NULL;
  return d ? d->cloth : (cw_cloth){ 0 };
}

/* ── Rope3D ───────────────────────────────────────────────────────────── */

enum { MODE_XPBD, MODE_LINKS };
static const char *const mode_names[] = { "XPBD", "LINKS", NULL };

typedef struct rope3d {
  float end[3];
  float length;
  int segments;
  int mode;
  const char *start_attach, *end_attach;
  bool pin_start, pin_end;
  float mass, stretch_compliance, bend_compliance, damping, thickness, friction;
  int substeps;
  int collision_mask;
  bool two_way;
  float radius;
  int sides;
  float color[4];
  float roughness, metalness;
  bool cast_shadow;
  /* runtime */
  int32_t hash;
  float tension;
  bool dirty;
  cw_cloth rope;
  frame fs, fe;
  V start_local, end_local; /* the ends in their frames' nodes (node frames) */
  t3_mesh *mesh;
  t3_geometry *geo;
  t3_material *mat;
  int nlinks;
  cw_body3d *links;
  cw_joint3d *joints;
  t3_mesh **link_meshes;
} rope3d;

static void rope_init(cw_node n, void *data) {
  (void)n;
  rope3d *d = data;
  cw_rope_def def = cw_rope_default_def();
  d->end[1] = -2;
  d->segments = 16;
  d->start_attach = d->end_attach = cw_intern("");
  d->pin_start = true;
  d->mass = def.mass, d->stretch_compliance = def.stretch_compliance, d->bend_compliance = def.bend_compliance;
  d->damping = def.damping, d->thickness = def.thickness, d->friction = def.friction, d->substeps = def.substeps;
  d->collision_mask = -1;
  d->radius = 0.025f;
  d->sides = 8;
  d->color[0] = 0.45f, d->color[1] = 0.33f, d->color[2] = 0.2f, d->color[3] = 1;
  d->roughness = 0.9f;
  d->cast_shadow = true;
  d->dirty = true;
}

static void rope_teardown(rope3d *d) {
  if (cw_cloth_valid(d->rope)) cw_cloth_destroy(d->rope);
  d->rope = (cw_cloth){ 0 };
  if (d->mesh) {
    if (d->mesh->base.parent) t3_object_remove(d->mesh->base.parent, d->mesh);
    t3_release(d->mesh);
  }
  d->mesh = NULL;
  d->geo = release_geo(d->geo);
  for (int i = 0; i < d->nlinks; i++) {
    if (d->link_meshes && d->link_meshes[i]) {
      if (d->link_meshes[i]->base.parent) t3_object_remove(d->link_meshes[i]->base.parent, d->link_meshes[i]);
      t3_release(d->link_meshes[i]);
    }
    if (d->links && cw_body3d_valid(d->links[i])) cw_body3d_destroy(d->links[i]);
  }
  cw_free(d->links), cw_free(d->joints), cw_free(d->link_meshes);
  d->links = NULL, d->joints = NULL, d->link_meshes = NULL;
  d->nlinks = 0;
  if (d->mat) t3_release(d->mat);
  d->mat = NULL;
}

/* ends: attach to a body, follow a node, or (no attach) pin to this node when asked */
static void rope_end(rope3d *d, int vertex, frame f, bool attached, bool pin, V world, V *local) {
  if (attached && cw_body3d_valid(f.body)) {
    cw_cloth_attach_body(d->rope, vertex, f.body, body_local(f.body, world));
    return;
  }
  if (!attached && !pin) return;
  t3_mat4 inv;
  t3_mat4_invert(&inv, cw_node3d_global_transform(f.node));
  *local = mat_point(&inv, world);
  cw_cloth_pin(d->rope, vertex, true, world);
}

static void rope_build(cw_node n, rope3d *d) {
  rope_teardown(d);
  d->dirty = false;
  cw_tree *t = cw_node_tree(n);
  if (!t) return;
  cw_world3d w = cw_physics3d_world(t);
  const t3_mat4 *m = cw_node3d_global_transform(n);
  V a = mat_point(m, (V){ 0, 0, 0 }), b = mat_point(m, (V){ d->end[0], d->end[1], d->end[2] });
  d->fs = frame_of(n, d->start_attach);
  d->fe = frame_of(n, d->end_attach);
  bool sa = d->start_attach && *d->start_attach, ea = d->end_attach && *d->end_attach;
  t3_material *mat = t3_mesh_standard_material_new(0xffffff);
  mat->color = (t3_color){ d->color[0], d->color[1], d->color[2] };
  mat->roughness = d->roughness;
  mat->metalness = d->metalness;
  d->mat = mat;
  if (d->mode == MODE_LINKS) {
    cw_chain3d_def cd = cw_chain3d_default_def();
    cd.start = a, cd.end = b;
    cd.links = d->segments;
    cd.radius = d->radius;
    cd.attach_start = sa ? d->fs.body : (cw_body3d){ 0 };
    cd.attach_end = ea ? d->fe.body : (cw_body3d){ 0 };
    cd.fix_start = d->pin_start;
    cd.mask = (uint32_t)d->collision_mask;
    d->nlinks = d->segments;
    d->links = cw_calloc((size_t)d->nlinks, sizeof(cw_body3d), CW_MEM_PHYSICS);
    d->joints = cw_calloc((size_t)d->nlinks + 1, sizeof(cw_joint3d), CW_MEM_PHYSICS);
    d->link_meshes = cw_calloc((size_t)d->nlinks, sizeof(t3_mesh *), CW_MEM_PHYSICS);
    cw_chain3d_create(w, &cd, d->links, d->joints);
    float seg = sqrtf((b.x - a.x) * (b.x - a.x) + (b.y - a.y) * (b.y - a.y) + (b.z - a.z) * (b.z - a.z)) / (float)d->nlinks;
    t3_geometry *g = t3_cylinder_geometry_new(d->radius, d->radius, seg, d->sides, 1, false);
    for (int i = 0; i < d->nlinks; i++) {
      d->link_meshes[i] = t3_mesh_new(g, d->mat);
      d->link_meshes[i]->base.cast_shadow = d->cast_shadow;
      d->link_meshes[i]->base.receive_shadow = true;
      t3_object_add(&cw_render3d_scene(t)->base, d->link_meshes[i]);
    }
    t3_release(g);
    return;
  }
  cw_rope_def rd = cw_rope_default_def();
  rd.start = a, rd.end = b;
  rd.segments = d->segments, rd.length = d->length, rd.mass = d->mass;
  rd.stretch_compliance = d->stretch_compliance, rd.bend_compliance = d->bend_compliance;
  rd.damping = d->damping, rd.thickness = d->thickness, rd.friction = d->friction, rd.substeps = d->substeps;
  rd.pin_start = rd.pin_end = false;
  rd.collide_mask = (uint32_t)d->collision_mask, rd.two_way = d->two_way;
  d->rope = cw_rope_create(w, &rd);
  if (!cw_cloth_valid(d->rope)) return;
  int cnt;
  const V *x = cw_cloth_positions(d->rope, &cnt);
  V x0 = x[0], x1 = x[cnt - 1];
  rope_end(d, 0, d->fs, sa, d->pin_start, x0, &d->start_local);
  rope_end(d, cnt - 1, d->fe, ea, d->pin_end, x1, &d->end_local);
  d->geo = cw_rope_geometry_new(d->rope, d->radius, d->sides);
  d->mesh = t3_mesh_new(d->geo, d->mat);
  d->mesh->base.cast_shadow = d->cast_shadow;
  d->mesh->base.receive_shadow = true;
  t3_object_add(&cw_render3d_scene(t)->base, d->mesh);
}

static void rope_tick(cw_node n, void *data, float dt) {
  rope3d *d = data;
  bool built = d->mode == MODE_LINKS ? d->nlinks > 0 : cw_cloth_valid(d->rope);
  if (d->dirty || !built) rope_build(n, d);
  if (d->mode == MODE_LINKS) {
    uint32_t h = CW_HASH32_INIT;
    for (int i = 0; i < d->nlinks; i++) {
      cw_xform3 x = cw_body3d_transform(d->links[i]);
      h = cw_hash32(&x, sizeof x, h);
    }
    d->hash = (int32_t)h;
    return;
  }
  if (!cw_cloth_valid(d->rope)) return;
  int cnt = cw_cloth_vertex_count(d->rope);
  bool sa = d->start_attach && *d->start_attach, ea = d->end_attach && *d->end_attach;
  if (!cw_body3d_valid(d->fs.body) && (sa || d->pin_start))
    cw_cloth_set_pin_target(d->rope, 0, mat_point(cw_node3d_global_transform(d->fs.node), d->start_local));
  if (!cw_body3d_valid(d->fe.body) && (ea || d->pin_end))
    cw_cloth_set_pin_target(d->rope, cnt - 1, mat_point(cw_node3d_global_transform(d->fe.node), d->end_local));
  cw_cloth_step(d->rope, dt);
  d->hash = (int32_t)cw_cloth_hash(d->rope);
  d->tension = cw_cloth_attachment_force(d->rope, cnt - 1);
}

static void rope_pre_render(cw_node n, void *data, double alpha) {
  (void)n;
  rope3d *d = data;
  if (d->mode == MODE_LINKS) {
    for (int i = 0; i < d->nlinks; i++) {
      if (!d->link_meshes[i] || !cw_body3d_valid(d->links[i])) continue;
      cw_xform3 x = cw_body3d_interpolated(d->links[i], (float)alpha);
      t3_object *o = &d->link_meshes[i]->base;
      t3_object_set_position(o, x.p.x, x.p.y, x.p.z);
      t3_object_set_quaternion(o, x.q);
    }
    return;
  }
  if (d->geo && cw_cloth_valid(d->rope)) cw_cloth_update_geometry(d->rope, d->geo, (float)alpha);
}

static void rope_enter(cw_node n, void *data) {
  ((rope3d *)data)->dirty = true;
  tree_add(n);
}
static void rope_exit(cw_node n, void *data) {
  tree_remove(n);
  rope_teardown(data);
  ((rope3d *)data)->dirty = true;
}
static void rope_destroy(cw_node n, void *data) {
  (void)n;
  rope_teardown(data);
}
static void rope_changed(cw_node n, void *data, const cw_prop *p) {
  (void)n;
  (void)p;
  ((rope3d *)data)->dirty = true;
}

static const cw_prop rope_props[] = {
  { .name = "end", .type = CW_T_VEC3, .flags = CW_PROP_DEFAULT, .offset = offsetof(rope3d, end), .size = sizeof(float) * 3,
    .doc = "The far end at creation, in the node's space (the near end is the node)." },
  CW_PROP_FIELD_RANGE(rope3d, length, CW_T_FLOAT, 0, 10000, "Rest length, m (0 = the distance to end; longer sags)."),
  CW_PROP_FIELD_RANGE(rope3d, segments, CW_T_INT, 1, 1000, "Segments (LINKS: rigid links)."),
  { .name = "mode", .type = CW_T_ENUM, .flags = CW_PROP_DEFAULT, .offset = offsetof(rope3d, mode), .size = sizeof(int),
    .enum_names = mode_names, .doc = "XPBD: a particle rope; LINKS: a chain of Box3D capsule bodies on spherical joints." },
  CW_PROP_FIELD(rope3d, start_attach, CW_T_STRING, CW_PROP_DEFAULT, "Path of a node the near end hangs from (a body node feels the pull)."),
  CW_PROP_FIELD(rope3d, end_attach, CW_T_STRING, CW_PROP_DEFAULT, "Path of a node the far end holds (a body node feels the pull)."),
  CW_PROP_FIELD(rope3d, pin_start, CW_T_BOOL, CW_PROP_DEFAULT, "Without start_attach: the near end follows this node."),
  CW_PROP_FIELD(rope3d, pin_end, CW_T_BOOL, CW_PROP_DEFAULT, "Without end_attach: the far end follows this node."),
  CW_PROP_FIELD_RANGE(rope3d, mass, CW_T_FLOAT, 0.001, 10000, "Kilograms (XPBD)."),
  CW_PROP_FIELD_RANGE(rope3d, stretch_compliance, CW_T_FLOAT, 0, 1, "m/N (XPBD)."),
  CW_PROP_FIELD_RANGE(rope3d, bend_compliance, CW_T_FLOAT, -1, 100, "m/N (XPBD; < 0 = no bending)."),
  CW_PROP_FIELD_RANGE(rope3d, damping, CW_T_FLOAT, 0, 100, "Share of velocity lost per second (XPBD)."),
  CW_PROP_FIELD_RANGE(rope3d, thickness, CW_T_FLOAT, 0, 1, "Collision radius, m (XPBD)."),
  CW_PROP_FIELD_RANGE(rope3d, friction, CW_T_FLOAT, 0, 1, "Against shapes (XPBD)."),
  CW_PROP_FIELD_RANGE(rope3d, substeps, CW_T_INT, 1, 64, "Per tick (XPBD)."),
  CW_PROP_FIELD(rope3d, collision_mask, CW_T_INT, CW_PROP_DEFAULT, "Layer bits it collides with (-1 = all, 0 = none)."),
  CW_PROP_FIELD(rope3d, two_way, CW_T_BOOL, CW_PROP_DEFAULT, "Collisions push dynamic bodies back (XPBD)."),
  CW_PROP_FIELD_RANGE(rope3d, radius, CW_T_FLOAT, 0.001, 10, "Drawn radius (LINKS: the links' radius)."),
  CW_PROP_FIELD_RANGE(rope3d, sides, CW_T_INT, 3, 64, "Sides of the drawn tube."),
  CW_PROP_FIELD(rope3d, color, CW_T_COLOR, CW_PROP_DEFAULT, "Albedo (linear)."),
  CW_PROP_FIELD_RANGE(rope3d, roughness, CW_T_FLOAT, 0, 1, "PBR roughness."),
  CW_PROP_FIELD_RANGE(rope3d, metalness, CW_T_FLOAT, 0, 1, "PBR metalness (a chain: 1)."),
  CW_PROP_FIELD(rope3d, cast_shadow, CW_T_BOOL, CW_PROP_DEFAULT, "Casts shadows."),
  { .name = "hash", .type = CW_T_INT, .flags = CW_PROP_EDITOR | CW_PROP_READONLY, .offset = offsetof(rope3d, hash),
    .size = sizeof(int32_t), .doc = "The simulation's running hash." },
  { .name = "tension", .type = CW_T_FLOAT, .flags = CW_PROP_EDITOR | CW_PROP_READONLY, .offset = offsetof(rope3d, tension),
    .size = sizeof(float), .doc = "N: the pull on a dynamic body at the far end, last tick (XPBD)." },
  CW_PROP_END,
};

static cw_node_type rope_type = {
  .name = "Rope3D", .parent = "Node3D", .data_size = sizeof(rope3d), .props = rope_props,
  .doc = "A rope or chain from this node to `end` (cloth.h): hangs, swings, carries and tows bodies.",
  .init = rope_init, .destroy = rope_destroy, .enter_tree = rope_enter, .exit_tree = rope_exit,
  .pre_render = rope_pre_render, .prop_changed = rope_changed,
};

cw_cloth cw_rope3d_get(cw_node n) {
  rope3d *d = rope_t ? cw_node_data_t(n, rope_t) : NULL;
  return d ? d->rope : (cw_cloth){ 0 };
}

void cw__register_cloth_types(void) {
  cw_node_type_register(&cloth_type);
  cloth_t = &cloth_type;
  cw_node_type_register(&rope_type);
  rope_t = &rope_type;
}
