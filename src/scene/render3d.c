/* The tree's 3D scene and the 3D render node types. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cartwheel/debug.h"
#include "cartwheel/pipeline.h"
#include "cartwheel/profile.h"
#include "cartwheel/csg.h"
#include "cartwheel/resource.h"
#include "scene_internal.h"
#include "render3d_internal.h"
#include "render/lighting_internal.h"
#include "render/quality_internal.h"
#include "cartwheel/render2d.h"
#include "cartwheel/render3d.h"
#include "render/bake_internal.h"
#include "cartwheel/gpu.h"
#include "cartwheel/shading.h"
#include "cartwheel/gl.h"
#if CW_WGPU
#include "platform/wgpu.h"
#endif

void cw__programs_register(void); /* src/render/programs.c */
void cw__decals_renderer_init(t3_renderer *r); /* src/fx/decals.c: mesh decals' extension hook */
void cw__skin_shading_renderer_init(t3_renderer *r); /* src/render/skin_shading.c: the skin part's hook */
void cw__skin_shading_frame(void);                   /* follow the quality setting */
void cw__rim_light_renderer_init(t3_renderer *r);   /* src/render/rim_light.c: the rim part's hook */

typedef struct render3d_state {
  t3_scene *scene;
  t3_renderer *renderer;
  cw_node camera; /* explicit; else the first current Camera3D in tree order */
  t3_light *ambient, *hemi;
  int w, h;
  bool shadows_enabled;
  bool shadows_keep; /* cw__render3d_keep_shadows: the next prepare reuses the maps */
  /* the frame's draws over every pipeline render of the scene (splitscreen:
   * each view's), cw__render3d_count; frame_counted: some were */
  unsigned frame_calls, frame_triangles;
  bool frame_counted;
  /* draws three.c could not make (a state the program table lacks, a program
   * refused as incomplete, an extension with no hook), since the tree began;
   * the first of each reason is logged */
  unsigned skipped_total;
  char skipped_said[160];
  float cpu_ms;
  /* the unit primitives (MeshInstance3D) this tree's meshes share: per tree,
   * because three.c keeps one renderer's GL state in a geometry, so two
   * renderers must not share one */
  t3_geometry *prim_cache[5][4];
  /* the materials MeshInstance3D primitives share: one per distinct look
   * (mesh_look), so identical meshes batch into instanced draws */
  struct look_mat *looks;
  int look_count, look_cap;
  /* three.c's light cap: when the scene was last counted, causes warned */
  uint32_t light_check_frame;
  uint8_t light_warned;
} render3d_state;

typedef struct mesh_look {
  int physical;
  t3_color color, emissive, sheen_color;
  float metalness, roughness, ior, specular_intensity, clearcoat, clearcoat_roughness, sheen, sheen_roughness, iridescence,
      iridescence_ior, anisotropy;
} mesh_look;
struct look_mat { mesh_look look; t3_material *mat; };

static void state_free(void *p) {
  render3d_state *s = p;
  for (int k = 0; k < 5; k++)
    for (int l = 0; l < 4; l++)
      if (s->prim_cache[k][l]) t3_release(s->prim_cache[k][l]);
  for (int i = 0; i < s->look_count; i++) t3_release(s->looks[i].mat);
  cw_free(s->looks);
  if (s->renderer) cw_shading_forget(s->renderer), t3_renderer_destroy(s->renderer);
  if (s->scene) t3_release(s->scene);
  cw_free(s);
}

static render3d_state *state(cw_tree *t) {
  render3d_state *s = cw_tree_slot(t, "render3d");
  if (!s) {
    s = cw_calloc(1, sizeof *s, CW_MEM_RENDER);
    s->scene = t3_scene_new();
    cw_tree_set_slot(t, "render3d", s, state_free);
  }
  return s;
}

t3_scene *cw_render3d_scene(cw_tree *t) { return state(t)->scene; }
t3_renderer *cw_render3d_renderer(cw_tree *t) {
  render3d_state *s = cw_tree_slot(t, "render3d");
  return s ? s->renderer : NULL;
}
void cw_render3d_set_camera(cw_tree *t, cw_node c) { state(t)->camera = c; }

t3_renderer *cw_render3d_window_renderer(int w, int h) {
  cw__programs_register();
  t3_renderer *r = t3_renderer_new(w, h);
  t3_renderer_set_sort_bounds(r, !getenv("CW_SORT_ORIGINS"));
  cw__decals_renderer_init(r);
  cw__skin_shading_renderer_init(r);
  cw__rim_light_renderer_init(r);
#if CW_WGPU
  if (r && cw_gpu_backend() == CW_GPU_WEBGPU)
    t3_renderer_use_wgpu(r, cw__wgpu_device(), cw__wgpu_queue(), (int)cw__wgpu_frame_format());
#endif
  return r;
}

void cw_render3d_window_target(t3_renderer *r) {
  cw__skin_shading_frame();
#if CW_WGPU
  if (r && cw_gpu_backend() == CW_GPU_WEBGPU) {
    t3_renderer_wgpu_set_output(r, cw__wgpu_frame_view(), (int)cw__wgpu_frame_format());
    return;
  }
#endif
  if (r) t3_renderer_set_framebuffer(r, cw__gl_window_fbo); /* the window (pixel.c may stand in) */
}

void *cw__r2d_target_three_view(cw_texture h, int *w, int *hh); /* render2d/texture.c */
void cw_render3d_texture_target(t3_renderer *r, cw_texture target) {
  if (!r) return;
  if (CW_HANDLE_IS_NULL(target)) {
    cw_render3d_window_target(r);
    return;
  }
#if CW_WGPU
  if (cw_gpu_backend() == CW_GPU_WEBGPU) {
    void *v = cw__r2d_target_three_view(target, NULL, NULL);
    if (v) t3_renderer_wgpu_set_output(r, v, (int)WGPUTextureFormat_RGBA8Unorm);
    return;
  }
#endif
  t3_renderer_set_framebuffer(r, cw_target_fbo(target));
}

/* ── Camera3D ───────────────────────────────────────────────────────── */

typedef struct camera3d {
  float fov, near, far;
  bool current;
  int32_t cull_layers;      /* the layers the camera's main picture draws (1) */
  int32_t viewmodel_layers; /* layers drawn after it, on top, with viewmodel_fov (0 = none) */
  float viewmodel_fov;
  t3_camera *cam;
  t3_camera *vm;            /* the viewmodel pass's camera: this one's pose, its own lens */
} camera3d;
static const cw_node_type *camera3d_t;

static void camera_init(cw_node n, void *data) {
  camera3d *c = data;
  c->fov = 70;
  c->near = 0.05f;
  c->far = 1000;
  c->current = true;
  c->cull_layers = 1;
  c->viewmodel_fov = 55;
  c->cam = t3_perspective_camera_new(c->fov, 16.0f / 9.0f, c->near, c->far);
  cw_node3d_set_object(n, &c->cam->base);
  t3_release(c->cam); /* the node's Node3D holds it */
}

static void camera_destroy(cw_node n, void *data) {
  (void)n;
  camera3d *c = data;
  if (c->vm) t3_release(c->vm);
  c->vm = NULL;
}

static const cw_prop camera_props[] = {
  CW_PROP_FIELD_RANGE(camera3d, fov, CW_T_FLOAT, 1, 179, "Vertical field of view, degrees."),
  CW_PROP_FIELD_RANGE(camera3d, near, CW_T_FLOAT, 0.001, 1000, "Near clip distance."),
  CW_PROP_FIELD_RANGE(camera3d, far, CW_T_FLOAT, 0.01, 1e7, "Far clip distance."),
  CW_PROP_FIELD(camera3d, current, CW_T_BOOL, CW_PROP_DEFAULT, "The first current camera in tree order renders."),
  CW_PROP_FIELD(camera3d, cull_layers, CW_T_INT, CW_PROP_DEFAULT, "Layers (Node3D layers) the camera's main picture draws; 1 = the default layer."),
  CW_PROP_FIELD(camera3d, viewmodel_layers, CW_T_INT, CW_PROP_DEFAULT, "Layers drawn after the main picture on top of it, depth cleared, with viewmodel_fov (a first person weapon: it neither clips into walls nor stretches at a wide field of view). 0 = none."),
  CW_PROP_FIELD_RANGE(camera3d, viewmodel_fov, CW_T_FLOAT, 1, 179, "Vertical field of view of the viewmodel layers, degrees."),
  CW_PROP_END,
};

static cw_node_type camera3d_type = {
  .name = "Camera3D", .parent = "Node3D", .data_size = sizeof(camera3d), .props = camera_props,
  .doc = "A perspective camera.", .init = camera_init, .destroy = camera_destroy,
};

t3_camera *cw_camera3d_t3(cw_node n) {
  camera3d *c = cw_node_data_t(n, camera3d_t);
  return c ? c->cam : NULL;
}

static bool find_camera(cw_node n, void *u) {
  cw_node *out = u;
  if (!CW_HANDLE_IS_NULL(*out)) return false;
  camera3d *c = cw_node_data_t(n, camera3d_t);
  if (c && c->current) *out = n;
  return true;
}

cw_node cw_render3d_camera(cw_tree *t) {
  render3d_state *s = state(t);
  if (cw_node_valid(s->camera)) return s->camera;
  cw_node found = { 0 };
  cw_node_walk(cw_tree_root(t), find_camera, &found);
  return found;
}

/* ── MeshInstance3D ─────────────────────────────────────────────────── */

typedef enum { PRIM_NONE, PRIM_BOX, PRIM_SPHERE, PRIM_PLANE, PRIM_CYLINDER } prim_kind;
static const char *const prim_names[] = { "NONE", "BOX", "SPHERE", "PLANE", "CYLINDER", NULL };

typedef struct mesh3d {
  int primitive;
  t3_vec3 size;
  t3_color color;
  float metalness, roughness;
  t3_color emissive;
  /* MeshPhysicalMaterial's lobes (three.js r186): any in use makes the mesh's
   * material a MeshPhysicalMaterial, otherwise it is a MeshStandardMaterial */
  float ior, specular_intensity;
  float clearcoat, clearcoat_roughness;
  float sheen, sheen_roughness;
  t3_color sheen_color;
  float iridescence, iridescence_ior;
  float anisotropy;
  bool cast_shadow, receive_shadow;
  uint64_t mesh; /* resource uid (resource.h); 0 = use the primitive */
  cw__mesh_vis vis; /* culling, LOD, impostors (visibility.h) */
  /* built */
  bool procedural; /* caller-owned geometry/materials, retained by node group */
  int built_primitive;
  t3_mesh *t3;
  t3_material *mat;
  int lod; /* level the primitive or resource mesh draws */
  /* the mesh resource drawn instead of the primitive */
  uint64_t built_mesh;
  uint32_t built_version;
  cw_res res;
  t3_object *res_obj;
  /* baked lighting (cartwheel/lightmap.h) */
  int gi_mode;              /* DYNAMIC, STATIC */
  float lightmap_scale;
  mesh_look look;           /* what m->mat draws (a primitive's material, shared by look) */
  t3_material *lm_mat;      /* the mesh's material, cloned to carry its own lightmap */
  bool lightmapped;         /* its geometry is the lightmap's uv2 copy: no LOD switching */
} mesh3d;
static const char *const gi_names[] = { "DYNAMIC", "STATIC", NULL };
static const cw_node_type *mesh3d_t;

/* Primitives are unit shapes shared by every MeshInstance3D (the mesh
 * object's scale carries `size`), so same-shaped meshes batch into instanced
 * draws, and SPHERE / CYLINDER have coarser tessellations as LOD levels. */
#define PRIM_LODS 4
static const int sphere_ws[PRIM_LODS] = { 32, 16, 10, 6 }, sphere_hs[PRIM_LODS] = { 16, 10, 7, 4 };
static const int cyl_rs[PRIM_LODS] = { 24, 12, 8, 5 };
static t3_geometry *prim_cache_orphan[5][PRIM_LODS]; /* for meshes outside any tree */

static int prim_lod_count(int kind) { return kind == PRIM_SPHERE || kind == PRIM_CYLINDER ? PRIM_LODS : 1; }

static t3_geometry *prim_geometry(cw_tree *t, int kind, int lod) {
  lod = CW_CLAMP(lod, 0, prim_lod_count(kind) - 1);
  if (kind <= PRIM_NONE || kind > PRIM_CYLINDER) return NULL;
  t3_geometry **slot = t ? &state(t)->prim_cache[kind][lod] : &prim_cache_orphan[kind][lod];
  if (!*slot) {
    switch (kind) {
    case PRIM_BOX: *slot = t3_box_geometry_new(1, 1, 1, 1, 1, 1); break;
    case PRIM_SPHERE: *slot = t3_sphere_geometry_new(0.5f, sphere_ws[lod], sphere_hs[lod]); break;
    case PRIM_PLANE: *slot = t3_plane_geometry_new(1, 1, 1, 1); break;
    case PRIM_CYLINDER: *slot = t3_cylinder_geometry_new(0.5f, 0.5f, 1, cyl_rs[lod], 1, false); break;
    default: break;
    }
  }
  return *slot;
}

/* how far a level's surface strays from the round shape: the chord error of
 * its segments (unit shapes: radius 0.5) */
static float prim_lod_error(int kind, int lod) {
  if (lod <= 0) return 0;
  int n = kind == PRIM_SPHERE ? CW_MIN(sphere_ws[lod], 2 * sphere_hs[lod]) : kind == PRIM_CYLINDER ? cyl_rs[lod] : 0;
  return n ? 0.5f * (1.0f - cosf(CW_PI / (float)n)) : 0;
}

static void mesh_init(cw_node n, void *data) {
  (void)n;
  mesh3d *m = data;
  m->primitive = PRIM_BOX;
  m->size = (t3_vec3){ 1, 1, 1 };
  m->color = (t3_color){ 0.8f, 0.8f, 0.8f };
  m->roughness = 1;
  m->ior = 1.5f;
  m->specular_intensity = 1;
  m->sheen_roughness = 1;
  m->iridescence_ior = 1.3f;
  m->built_primitive = -1;
  m->vis.lod_bias = 1;
  m->vis.occlusion_cull = true;
  m->vis.impostor_distance = 50;
  m->vis.impostor_fade = 5;
  m->lightmap_scale = 1;
}

/* A mesh resource by UID: a glTF scene (instanced: geometry, materials and
 * textures shared with every other user) or one primitive of one
 * ("file.glb#mesh/0/0", drawn with its glTF material). Rebuilt when the UID
 * changes or the resource reloads. */
static bool mesh_res_rebuild(cw_node n, mesh3d *m) {
  if (!m->mesh) {
    if (m->res_obj) { t3_object_remove(cw_node3d_object(n), m->res_obj); m->res_obj = NULL; }
    cw_res_release(m->res);
    m->res = (cw_res){ 0 };
    m->built_mesh = 0;
    return false;
  }
  if (m->built_mesh == m->mesh && m->built_version == cw_res_version(m->res)) return true;
  if (m->built_mesh != m->mesh) {
    cw_res_release(m->res);
    m->res = cw_res_load_uid(m->mesh, 0);
    m->built_mesh = m->mesh;
  }
  m->built_version = cw_res_version(m->res);
  t3_object *group = cw_node3d_object(n);
  if (m->res_obj) { t3_object_remove(group, m->res_obj); m->res_obj = NULL; }
  if (m->t3) { t3_object_remove(group, m->t3); m->t3 = NULL; m->built_primitive = -1; }
  t3_object *o = NULL;
  const cw_mesh *cm = cw_res_mesh(m->res);
  m->lod = 0;
  if (cm) o = (t3_object *)t3_mesh_new(cm->lod[0], cm->material);
  else if (cw_res_gltf(m->res)) o = cw_gltf_instantiate(m->res);
  else if (cw_res_get_state(m->res) != CW_RES_STATE_LOADING)
    cw_warn("MeshInstance3D: resource %016llx is not a mesh or glTF scene", (unsigned long long)m->mesh);
  if (o) {
    t3_object_add(group, o);
    t3_release(o);
    m->res_obj = o;
  }
  return true;
}

/* the mesh object's own transform: the unit primitive scaled to `size` (a
 * plane lies in XZ facing +Y) */
static void prim_transform(mesh3d *m) {
  t3_mesh *t = m->t3;
  switch (m->primitive) {
  case PRIM_BOX: t->base.scale = m->size; break;
  case PRIM_SPHERE: t->base.scale = (t3_vec3){ m->size.x, m->size.x, m->size.x }; break;
  case PRIM_PLANE: t->base.scale = (t3_vec3){ m->size.x, m->size.z, 1 }; break;
  case PRIM_CYLINDER: t->base.scale = (t3_vec3){ m->size.x, m->size.y, m->size.x }; break;
  default: break;
  }
}

static bool mesh_physical(const mesh3d *m);

/* the material a primitive draws with, from its properties: a
 * MeshPhysicalMaterial as soon as one of the physical lobes is in use */
static mesh_look look_of(const mesh3d *m) {
  mesh_look k;
  memset(&k, 0, sizeof k); /* padding too: looks compare with memcmp */
  k.physical = mesh_physical(m);
  k.color = m->color;
  k.metalness = m->metalness;
  k.roughness = m->roughness;
  k.emissive = m->emissive;
  if (k.physical) {
    k.ior = m->ior;
    k.specular_intensity = m->specular_intensity;
    k.clearcoat = m->clearcoat;
    k.clearcoat_roughness = m->clearcoat_roughness;
    k.sheen = m->sheen;
    k.sheen_roughness = m->sheen_roughness;
    k.sheen_color = m->sheen_color;
    k.iridescence = m->iridescence;
    k.iridescence_ior = m->iridescence_ior;
    k.anisotropy = m->anisotropy;
  }
  return k;
}

static void look_apply(t3_material *t, const mesh_look *k) {
  t->color = k->color;
  t->metalness = k->metalness;
  t->roughness = k->roughness;
  t->emissive = k->emissive;
  if (t->type == T3_MESH_PHYSICAL_MATERIAL) {
    t->ior = k->ior;
    t->specular_intensity = k->specular_intensity;
    t->clearcoat = k->clearcoat;
    t->clearcoat_roughness = k->clearcoat_roughness;
    t->sheen = k->sheen;
    t->sheen_roughness = k->sheen_roughness;
    t->sheen_color = k->sheen_color;
    t->iridescence = k->iridescence;
    t->iridescence_ior = k->iridescence_ior;
    t->anisotropy = k->anisotropy;
  }
}

static t3_material *look_new(const mesh_look *k) {
  t3_material *t = k->physical ? t3_mesh_physical_material_new(0xffffff) : t3_mesh_standard_material_new(0xffffff);
  look_apply(t, k);
  return t;
}

/* A new reference to the tree's material for look k: every primitive in the
 * tree with the same look draws with one material (one batch, one material
 * upload). Materials no mesh uses any more (only the cache holds them) go
 * when a new look is added. Outside a tree, a material of its own. */
static t3_material *look_material(cw_tree *t, const mesh_look *k) {
  if (!t) return look_new(k);
  render3d_state *s = state(t);
  for (int i = 0; i < s->look_count; i++)
    if (!memcmp(&s->looks[i].look, k, sizeof *k)) return t3_retain(s->looks[i].mat);
  for (int i = 0; i < s->look_count;)
    if (s->looks[i].mat->_refs == 1) {
      t3_release(s->looks[i].mat);
      s->looks[i] = s->looks[--s->look_count];
    } else {
      i++;
    }
  if (s->look_count == s->look_cap) {
    s->look_cap = s->look_cap ? s->look_cap * 2 : 16;
    s->looks = cw_realloc(s->looks, (size_t)s->look_cap * sizeof *s->looks, CW_MEM_RENDER);
  }
  struct look_mat *e = &s->looks[s->look_count++];
  e->look = *k;
  e->mat = look_new(k);
  return t3_retain(e->mat);
}

/* point the mesh at the material for its current look (a property changed) */
static void mesh_set_look(cw_node n, mesh3d *m, const mesh_look *k) {
  t3_material *nm = look_material(cw_node_tree(n), k);
  if (m->t3 && m->t3->material_count && m->t3->materials[0] == m->mat) {
    t3_retain(nm);
    t3_release(m->t3->materials[0]);
    m->t3->materials[0] = nm;
  }
  if (m->mat) t3_release(m->mat);
  m->mat = nm;
  m->look = *k;
}

static void mesh_rebuild(cw_node n, mesh3d *m) {
  if (m->procedural) return;
  if (mesh_res_rebuild(n, m)) return;
  if (m->built_primitive != m->primitive) {
    m->built_primitive = m->primitive;
    m->lod = 0;
    t3_geometry *g = prim_geometry(cw_node_tree(n), m->primitive, 0);
    if (!m->mat) {
      mesh_look k = look_of(m);
      mesh_set_look(n, m, &k);
    }
    t3_object *group = cw_node3d_object(n);
    if (m->t3) {
      t3_object_remove(group, m->t3);
      m->t3 = NULL;
    }
    if (!g) return;
    t3_mesh *mesh = t3_mesh_new(g, m->mat);
    if (m->primitive == PRIM_PLANE) t3_object_set_rotation(mesh, -CW_PI / 2, 0, 0); /* lies in XZ, faces +Y */
    /* the mesh is a child of the node's group, so the plane's own rotation and
     * the node's transform compose */
    t3_object_add(group, mesh);
    t3_release(mesh);
    m->t3 = mesh;
  }
  if (m->t3) prim_transform(m);
}

static bool mesh_physical(const mesh3d *m) {
  return m->clearcoat > 0 || m->sheen > 0 || m->iridescence > 0 || m->anisotropy > 0 || m->ior != 1.5f ||
         m->specular_intensity != 1;
}

static void mesh_pre_render(cw_node n, void *data, double alpha) {
  (void)alpha;
  mesh3d *m = data;
  if (m->procedural) return;
  mesh_rebuild(n, m);
  if (m->mat) {
    mesh_look k = look_of(m);
    /* a lightmapped primitive draws with a material of its own (it carries
     * the mesh's lightmap): the properties go straight to it */
    if (m->lm_mat && !m->res_obj && m->t3 && m->t3->material_count && m->t3->materials[0] == m->lm_mat) {
      look_apply(m->lm_mat, &k);
    } else if (memcmp(&k, &m->look, sizeof k)) {
      mesh_set_look(n, m, &k);
    }
  }
  if (m->t3) {
    m->t3->base.cast_shadow = m->cast_shadow;
    m->t3->base.receive_shadow = m->receive_shadow;
  }
}

static void mesh_destroy(cw_node n, void *data) {
  (void)n;
  mesh3d *m = data;
  if (m->mat) t3_release(m->mat);
  m->mat = NULL;
  if (m->lm_mat) t3_release(m->lm_mat);
  m->lm_mat = NULL;
  cw_res_release(m->res);
  m->res = (cw_res){ 0 };
}

static const cw_prop mesh_props[] = {
  { .name = "primitive", .type = CW_T_ENUM, .flags = CW_PROP_DEFAULT, .offset = offsetof(mesh3d, primitive),
    .size = sizeof(int), .enum_names = prim_names, .doc = "Built-in shape when no mesh resource is set." },
  CW_PROP_FIELD(mesh3d, size, CW_T_VEC3, CW_PROP_DEFAULT, "Primitive size (sphere/cylinder: x = diameter)."),
  CW_PROP_FIELD(mesh3d, color, CW_T_COLOR, CW_PROP_DEFAULT, "Albedo (linear)."),
  CW_PROP_FIELD_RANGE(mesh3d, metalness, CW_T_FLOAT, 0, 1, "PBR metalness."),
  CW_PROP_FIELD_RANGE(mesh3d, roughness, CW_T_FLOAT, 0, 1, "PBR roughness."),
  CW_PROP_FIELD_RANGE(mesh3d, clearcoat, CW_T_FLOAT, 0, 1, "Physical: a clear lacquer layer over the surface (0 = none)."),
  CW_PROP_FIELD_RANGE(mesh3d, clearcoat_roughness, CW_T_FLOAT, 0, 1, "Physical: roughness of the clearcoat layer."),
  CW_PROP_FIELD_RANGE(mesh3d, sheen, CW_T_FLOAT, 0, 1, "Physical: a soft velvet / fabric / fur sheen (0 = none)."),
  CW_PROP_FIELD(mesh3d, sheen_color, CW_T_COLOR, CW_PROP_DEFAULT, "Physical: the sheen's colour (linear)."),
  CW_PROP_FIELD_RANGE(mesh3d, sheen_roughness, CW_T_FLOAT, 0, 1, "Physical: roughness of the sheen."),
  CW_PROP_FIELD_RANGE(mesh3d, iridescence, CW_T_FLOAT, 0, 1, "Physical: thin-film iridescence, soap bubble / beetle shell (0 = none)."),
  CW_PROP_FIELD_RANGE(mesh3d, iridescence_ior, CW_T_FLOAT, 1, 2.333, "Physical: index of refraction of the iridescent film."),
  CW_PROP_FIELD_RANGE(mesh3d, ior, CW_T_FLOAT, 1, 2.333, "Physical: index of refraction (1.5 = glass-like dielectric)."),
  CW_PROP_FIELD_RANGE(mesh3d, specular_intensity, CW_T_FLOAT, 0, 1, "Physical: strength of the dielectric specular reflection."),
  CW_PROP_FIELD_RANGE(mesh3d, anisotropy, CW_T_FLOAT, 0, 1, "Physical: stretched highlights, brushed metal (0 = none)."),
  CW_PROP_FIELD(mesh3d, emissive, CW_T_COLOR, CW_PROP_DEFAULT, "Emitted light colour."),
  CW_PROP_FIELD(mesh3d, cast_shadow, CW_T_BOOL, CW_PROP_DEFAULT, "Casts shadows."),
  CW_PROP_FIELD(mesh3d, receive_shadow, CW_T_BOOL, CW_PROP_DEFAULT, "Receives shadows."),
  CW_PROP_FIELD(mesh3d, mesh, CW_T_RES, CW_PROP_DEFAULT, "Mesh resource (overrides the primitive)."),
  { .name = "static", .type = CW_T_BOOL, .flags = CW_PROP_DEFAULT, .offset = offsetof(mesh3d, vis.is_static),
    .size = sizeof(bool), .doc = "Does not move: joins the visibility BVH (visibility.h)." },
  { .name = "lod_bias", .type = CW_T_FLOAT, .flags = CW_PROP_DEFAULT, .offset = offsetof(mesh3d, vis.lod_bias),
    .size = sizeof(float), .min = 0.01, .max = 100, .doc = "Scales the LOD error budget (2 = coarser sooner)." },
  { .name = "visibility_range", .type = CW_T_FLOAT, .flags = CW_PROP_DEFAULT,
    .offset = offsetof(mesh3d, vis.visibility_range), .size = sizeof(float), .min = 0, .max = 1e9,
    .doc = "Hidden beyond this camera distance (0 = no limit)." },
  { .name = "occlusion_cull", .type = CW_T_BOOL, .flags = CW_PROP_DEFAULT, .offset = offsetof(mesh3d, vis.occlusion_cull),
    .size = sizeof(bool), .doc = "May be hidden by GPU occlusion queries." },
  { .name = "impostor", .type = CW_T_BOOL, .flags = CW_PROP_DEFAULT, .offset = offsetof(mesh3d, vis.impostor),
    .size = sizeof(bool), .doc = "Beyond impostor_distance, draw an octahedral impostor instead of the mesh." },
  { .name = "impostor_distance", .type = CW_T_FLOAT, .flags = CW_PROP_DEFAULT,
    .offset = offsetof(mesh3d, vis.impostor_distance), .size = sizeof(float), .min = 0, .max = 1e9,
    .doc = "Camera distance where the impostor is fully in." },
  { .name = "impostor_fade", .type = CW_T_FLOAT, .flags = CW_PROP_DEFAULT, .offset = offsetof(mesh3d, vis.impostor_fade),
    .size = sizeof(float), .min = 0, .max = 1e9, .doc = "Distance over which the impostor fades in over the mesh." },
  { .name = "lod", .type = CW_T_INT, .flags = CW_PROP_EDITOR | CW_PROP_READONLY, .offset = offsetof(mesh3d, vis.lod),
    .size = sizeof(int), .doc = "Level drawn last frame (-1 culled, 100 impostor; needs a VisibilityManager)." },
  { .name = "gi_mode", .type = CW_T_ENUM, .flags = CW_PROP_DEFAULT, .offset = offsetof(mesh3d, gi_mode), .size = sizeof(int),
    .enum_names = gi_names,
    .doc = "STATIC: baked geometry (a lightmap of its own; occludes and bounces light in the bake). DYNAMIC: lit by the "
           "baked probe grid (cartwheel/lightmap.h)." },
  CW_PROP_FIELD_RANGE(mesh3d, lightmap_scale, CW_T_FLOAT, 0.01, 100, "Multiplier of LightmapGI.texel_density for this mesh."),
  CW_PROP_END,
};

static cw_node_type mesh3d_type = {
  .name = "MeshInstance3D", .parent = "Node3D", .data_size = sizeof(mesh3d), .props = mesh_props,
  .doc = "Draws a mesh: a built-in primitive or a mesh resource.",
  .init = mesh_init, .destroy = mesh_destroy, .pre_render = mesh_pre_render,
};

const cw_node_type *cw__mesh3d_type(void) { return mesh3d_t; }

bool cw_mesh3d_set_procedural(cw_node n, t3_mesh *mesh) {
  mesh3d *m = cw_node_data_t(n, mesh3d_t);
  if (!m) return false;
  if (m->procedural && m->t3 == mesh) return true;
  if (mesh && mesh->base.parent) return false;
  t3_object *group = cw_node3d_object(n);
  if (m->t3) { t3_object_remove(group, m->t3); m->t3 = NULL; }
  if (m->res_obj) { t3_object_remove(group, m->res_obj); m->res_obj = NULL; }
  cw_res_release(m->res); m->res = (cw_res){0};
  /* The previous geometry's bake does not belong to its replacement.
   * Release the node's private material reference and restore normal LOD
   * selection when the caller later clears the procedural mesh. */
  if (m->lm_mat) t3_release(m->lm_mat);
  m->lm_mat = NULL;
  m->lightmapped = false;
  m->built_mesh = 0; m->built_version = 0;
  m->built_primitive = -1; m->lod = 0;
  m->procedural = mesh != NULL;
  if (mesh) { t3_object_add(group, mesh); m->t3 = mesh; }
  return true;
}


static uint64_t mix_key(uint64_t h, uint64_t v) {
  h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
  return h * 0xBF58476D1CE4E5B9ull;
}
static uint64_t float_bits(float f) {
  uint32_t u;
  memcpy(&u, &f, 4);
  return u;
}

bool cw__mesh3d_info(cw_node n, cw__mesh_info *out) {
  mesh3d *m = cw_node_data_t(n, mesh3d_t);
  if (!m) return false;
  mesh_rebuild(n, m);
  memset(out, 0, sizeof *out);
  out->vis = &m->vis;
  out->cast_shadow = m->cast_shadow;
  uint64_t k = 1469598103934665603ull;
  if (m->procedural && m->t3) {
    /* Cached mission scenery is parented into raw three.c groups. Do not
     * classify or report a procedural cell whose authored ancestors hide it. */
    for (t3_object *p = m->t3->base.parent; p; p = p->parent) if (!p->visible) return false;
    out->obj = &m->t3->base;
    out->cast_shadow = m->t3->base.cast_shadow;
    out->kind = CW__MESH_PRIMITIVE;
    out->lod_levels = 1;
    out->look_key = m->t3->base.id;
    return true;
  }
  if (m->res_obj) {
    out->obj = m->res_obj;
    const cw_mesh *cm = cw_res_mesh(m->res);
    const cw_gltf *gl = cw_res_gltf(m->res);
    out->kind = cm ? CW__MESH_RESOURCE : CW__MESH_GLTF;
    out->lod_levels = m->lightmapped ? 1 : cm ? CW_MAX(cm->lod_count, 1) : gl ? CW_MAX(gl->lod_levels, 1) : 1;
    k = mix_key(k, m->mesh);
    k = mix_key(k, 7);
  } else if (m->t3) {
    out->obj = &m->t3->base;
    out->kind = CW__MESH_PRIMITIVE;
    out->lod_levels = m->lightmapped ? 1 : prim_lod_count(m->primitive);
    k = mix_key(k, (uint64_t)m->primitive);
    k = mix_key(k, float_bits(m->color.r));
    k = mix_key(k, float_bits(m->color.g));
    k = mix_key(k, float_bits(m->color.b));
    k = mix_key(k, float_bits(m->metalness));
    k = mix_key(k, float_bits(m->roughness));
    k = mix_key(k, float_bits(m->clearcoat));
    k = mix_key(k, float_bits(m->sheen));
    k = mix_key(k, float_bits(m->iridescence));
    k = mix_key(k, float_bits(m->emissive.r));
    k = mix_key(k, float_bits(m->emissive.g));
    k = mix_key(k, float_bits(m->emissive.b));
  }
  out->look_key = k;
  return true;
}

float cw__mesh3d_lod_error(cw_node n, int level) {
  mesh3d *m = cw_node_data_t(n, mesh3d_t);
  if (!m || m->procedural || level <= 0) return 0;
  if (m->res_obj) {
    const cw_mesh *cm = cw_res_mesh(m->res);
    if (!cm || level >= cm->lod_count) return 0;
    return cm->lod_error[level] * cm->extent;
  }
  return prim_lod_error(m->primitive, level);
}

void cw__mesh3d_set_lod(cw_node n, int level) {
  mesh3d *m = cw_node_data_t(n, mesh3d_t);
  if (!m || m->procedural || m->lightmapped) return;
  if (m->res_obj) {
    const cw_mesh *cm = cw_res_mesh(m->res);
    if (cm && m->res_obj->type == T3_MESH) {
      level = CW_CLAMP(level, 0, CW_MAX(cm->lod_count, 1) - 1);
      if (level == m->lod || !cm->lod[level]) return;
      t3_mesh *t = (t3_mesh *)m->res_obj;
      t3_retain(cm->lod[level]);
      t3_release(t->geometry);
      t->geometry = cm->lod[level];
      m->lod = level;
    } else if (cw_res_gltf(m->res)) {
      cw_gltf_instance_set_lod(m->res_obj, level);
      m->lod = level;
    }
    return;
  }
  if (!m->t3) return;
  level = CW_CLAMP(level, 0, prim_lod_count(m->primitive) - 1);
  if (level == m->lod) return;
  t3_geometry *g = prim_geometry(cw_node_tree(n), m->primitive, level);
  t3_retain(g);
  t3_release(m->t3->geometry);
  m->t3->geometry = g;
  m->lod = level;
}

/* ── lights ─────────────────────────────────────────────────────────── */

typedef struct light3d {
  t3_color color;
  float energy, range, decay, angle, penumbra;
  bool shadow;
  int shadow_size;
  float shadow_extent, shadow_far, shadow_bias, shadow_normal_bias;
  t3_light *light;
  int kind; /* 0 directional, 1 omni, 2 spot */
  bool bake;                /* contributes to baked lightmaps (LightmapGI) */
  bool bake_direct;         /* its direct light is baked too (for meshes LightmapGI draws BAKED) */
  bool realtime;            /* lights the scene in real time (false: only the bake has it) */
  int shadow_cascades;      /* directional: 0 = one shadow map (three.c), 1-4 = cascades (r186 CSMShadowNode) */
  float shadow_distance;    /* directional cascades: where the last ends (r186 maxFar; 0 = the camera's far) */
  bool cascade_fade;        /* directional cascades: cross-fade between them (r186 CSMShadowNode.fade) */
  float fog_energy;         /* strength in volumetric fog (src/render/volumetric_fog.c) */
} light3d;
static const cw_node_type *light3d_t;

static void light_common_init(cw_node n, light3d *l, int kind) {
  l->color = (t3_color){ 1, 1, 1 };
  l->energy = 1;
  l->range = 0;
  l->decay = 2;
  l->angle = CW_PI / 4;
  l->penumbra = 0.2f;
  l->shadow_size = 1024;
  l->shadow_extent = 5; /* three.js's DirectionalLightShadow camera: -5..5, near 0.5, far 500 */
  l->shadow_far = 500;
  l->kind = kind;
  l->bake = true;
  l->realtime = true;
  l->cascade_fade = true;
  l->fog_energy = 1;
  l->light = kind == 0 ? t3_directional_light_new(0xffffff, 1)
             : kind == 1 ? t3_point_light_new(0xffffff, 1, 0, 2)
                         : t3_spot_light_new(0xffffff, 1, 0, CW_PI / 4, 0.2f, 2);
  cw_node3d_set_object(n, &l->light->base);
  t3_release(l->light);
}
static void dir_init(cw_node n, void *d) { light_common_init(n, d, 0); }
static void omni_init(cw_node n, void *d) { light_common_init(n, d, 1); }
static void spot_init(cw_node n, void *d) { light_common_init(n, d, 2); }

static void light_enter(cw_node n, void *data) {
  light3d *l = data;
  /* directional and spot lights aim along their -Z: three.js aims at
   * light.target, which sits 1 unit down the light's local -Z here */
  if (l->light->target && l->kind != 1) {
    t3_object *tg = l->light->target;
    if (!tg->parent) t3_object_add(&l->light->base, tg);
    tg->position = (t3_vec3){ 0, 0, -1 };
  }
  cw_tree *t = cw_node_tree(n);
  if (t && l->shadow) state(t)->shadows_enabled = true;
}

static void light_pre_render(cw_node n, void *data, double alpha) {
  (void)alpha;
  light3d *l = data;
  l->light->color = l->color;
  l->light->intensity = l->realtime ? l->energy : 0; /* not real time: dark to three.c and the clusters, the bake still reads energy */
  l->light->distance = l->range;
  l->light->decay = l->decay;
  l->light->angle = l->angle;
  l->light->penumbra = l->penumbra;
  l->light->base.cast_shadow = l->shadow && l->light->base.visible;
  if (l->light->shadow) {
    t3_light_shadow *sh = l->light->shadow;
    sh->map_width = sh->map_height = cw__quality_shadow_size(l->shadow_size); /* cartwheel/quality.h */
    sh->bias = l->shadow_bias;
    sh->normal_bias = l->shadow_normal_bias;
    t3_camera *sc = sh->camera;
    float ext = l->shadow_extent > 0 ? l->shadow_extent : 5, far = l->shadow_far > 0 ? l->shadow_far : 500;
    if (l->kind == 0 && (sc->right != ext || sc->far != far)) {
      sc->left = sc->bottom = -ext;
      sc->right = sc->top = ext;
      sc->far = far;
      t3_camera_update_projection_matrix(sc);
    }
  }
  if (l->shadow && l->light->base.visible) {
    cw_tree *t = cw_node_tree(n);
    if (t) state(t)->shadows_enabled = true;
    /* a cascaded sun (cartwheel/lighting.h): the lighting pass draws its
     * shadows; the Shadows setting keeps 2 cascades on Low, 3 on Medium */
    if (t && l->kind == 0 && l->shadow_cascades > 0) {
      static const int cap[4] = { 2, 3, 4, 4 };
      /* (each cascade at most 2048: the four share one map twice that wide) */
      cw__csm_cfg cfg = { CW_MIN(CW_CLAMP(l->shadow_cascades, 1, 4), cap[cw__quality_shadow_level()]), l->shadow_distance,
                          l->cascade_fade, CW_MIN(cw__quality_shadow_size(l->shadow_size), 2048) };
      cw__lighting_csm_light(t, l->light, &cfg);
    }
  }
  if (l->light->base.visible && l->fog_energy > 0) {
    void cw__vfog_push_light(cw_tree *t, const t3_light *l, int kind, bool shadow, float fog_energy); /* src/render/volumetric_fog.c */
    cw_tree *t = cw_node_tree(n);
    if (t) cw__vfog_push_light(t, l->light, l->kind, l->shadow, l->fog_energy);
  }
}

static const cw_prop light_props[] = {
  CW_PROP_FIELD(light3d, color, CW_T_COLOR, CW_PROP_DEFAULT, "Light colour."),
  CW_PROP_FIELD_RANGE(light3d, energy, CW_T_FLOAT, 0, 1e5, "Intensity, 0 to 100000 (a value outside is clamped, with a warning)."),
  CW_PROP_FIELD(light3d, shadow, CW_T_BOOL, CW_PROP_DEFAULT, "Casts shadows."),
  CW_PROP_FIELD(light3d, shadow_size, CW_T_INT, CW_PROP_DEFAULT, "Shadow map resolution."),
  CW_PROP_FIELD_RANGE(light3d, shadow_extent, CW_T_FLOAT, 0, 1e5,
                      "Directional: half size of the square the shadow map covers, metres around the light's position "
                      "(move the light with the action to keep its shadows sharp); 0 = 5."),
  CW_PROP_FIELD_RANGE(light3d, shadow_far, CW_T_FLOAT, 0, 1e6, "Directional: shadow camera depth along -Z from the light; 0 = 500."),
  CW_PROP_FIELD_RANGE(light3d, shadow_bias, CW_T_FLOAT, -1, 1, "Depth bias of the shadow test (small negative values cure acne)."),
  CW_PROP_FIELD_RANGE(light3d, shadow_normal_bias, CW_T_FLOAT, 0, 10, "Offset along the surface normal for the shadow lookup (metres)."),
  CW_PROP_FIELD_RANGE(light3d, range, CW_T_FLOAT, 0, 1e6, "Omni/spot: distance where light reaches zero (0 = infinite)."),
  CW_PROP_FIELD_RANGE(light3d, decay, CW_T_FLOAT, 0, 10, "Omni/spot: falloff exponent."),
  CW_PROP_FIELD_RANGE(light3d, angle, CW_T_FLOAT, 0, 1.5707963, "Spot: cone half-angle, radians."),
  CW_PROP_FIELD_RANGE(light3d, penumbra, CW_T_FLOAT, 0, 1, "Spot: soft edge fraction."),
  CW_PROP_FIELD(light3d, bake, CW_T_BOOL, CW_PROP_DEFAULT, "Contributes to baked lightmaps and ambient probes (LightmapGI)."),
  CW_PROP_FIELD(light3d, realtime, CW_T_BOOL, CW_PROP_DEFAULT,
                "Lights the scene in real time (default). Off: only the bake has it (with bake_direct, the lightmaps "
                "carry its light; moving objects can take it from cw_lightmap_irradiance_below); it costs nothing a frame."),
  CW_PROP_FIELD(light3d, bake_direct, CW_T_BOOL, CW_PROP_DEFAULT,
                "With bake: its direct light (shadowed by the static geometry) goes into the lightmaps too, for meshes a "
                "LightmapGI with shading BAKED draws unlit; it still lights everything else in real time (cartwheel/lightmap.h)."),
  CW_PROP_FIELD_RANGE(light3d, shadow_cascades, CW_T_INT, 0, 4,
                      "Directional: 0 = one shadow map around the light; 1-4 = cascaded shadow maps that follow the camera "
                      "(r186 CSMShadowNode; shadow_size is each cascade's; cartwheel/lighting.h)."),
  CW_PROP_FIELD_RANGE(light3d, shadow_distance, CW_T_FLOAT, 0, 1e6, "Directional cascades: where the last ends (0 = the camera's far)."),
  CW_PROP_FIELD(light3d, cascade_fade, CW_T_BOOL, CW_PROP_DEFAULT, "Directional cascades: cross-fade between neighbours."),
  CW_PROP_FIELD_RANGE(light3d, fog_energy, CW_T_FLOAT, 0, 100,
                      "Strength of this light in volumetric fog (HeightFog volumetric, FogVolume3D): its shafts and glow; 0 = none."),
  CW_PROP_END,
};

static cw_node_type light3d_base = {
  .name = "Light3D", .parent = "Node3D", .data_size = sizeof(light3d), .props = light_props,
  .doc = "Base of the light types.", .enter_tree = light_enter, .pre_render = light_pre_render,
};

static cw_node_type dir_type = { .name = "DirectionalLight3D", .parent = "Light3D", .init = NULL, .doc = "Sun-like light along -Z." };
static cw_node_type omni_type = { .name = "OmniLight3D", .parent = "Light3D", .doc = "Point light." };
static cw_node_type spot_type = { .name = "SpotLight3D", .parent = "Light3D", .doc = "Cone light along -Z." };

/* The light kind is picked by the leaf type; Light3D's data is shared, so the
 * leaf's init (run after Light3D's) creates the right three.c light. */
/* lights shine on every layer: a camera's layers pick what it draws, not what is lit */
static void light_all_layers(cw_node n) {
  cw_node3d *d3 = cw_node_data(n, "Node3D");
  if (d3) d3->layers = -1;
}
static void dir_leaf_init(cw_node n, void *d) { (void)d; light_all_layers(n); dir_init(n, cw_node_data_t(n, light3d_t)); }
static void omni_leaf_init(cw_node n, void *d) { (void)d; light_all_layers(n); omni_init(n, cw_node_data_t(n, light3d_t)); }
static void spot_leaf_init(cw_node n, void *d) { (void)d; light_all_layers(n); spot_init(n, cw_node_data_t(n, light3d_t)); }

/* ── WorldEnvironment ───────────────────────────────────────────────── */

typedef struct world_env {
  t3_color background;
  bool use_background;
  t3_color ambient_color;
  float ambient_energy;
  t3_color sky_color, ground_color;
  float hemi_energy;
  int fog_mode; /* 0 none, 1 linear, 2 exp2 */
  t3_color fog_color;
  float fog_near, fog_far, fog_density;
} world_env;
static const char *const fog_names[] = { "NONE", "LINEAR", "EXP2", NULL };

static void env_init(cw_node n, void *data) {
  (void)n;
  world_env *e = data;
  e->background = (t3_color){ 0.1f, 0.12f, 0.15f };
  e->use_background = true;
  e->ambient_color = (t3_color){ 1, 1, 1 };
  e->ambient_energy = 0.2f;
  e->sky_color = (t3_color){ 0.6f, 0.7f, 0.9f };
  e->ground_color = (t3_color){ 0.3f, 0.25f, 0.2f };
  e->fog_color = (t3_color){ 0.5f, 0.6f, 0.7f };
  e->fog_near = 10;
  e->fog_far = 100;
  e->fog_density = 0.02f;
}

static void env_pre_render(cw_node n, void *data, double alpha) {
  (void)alpha;
  world_env *e = data;
  cw_tree *t = cw_node_tree(n);
  if (!t) return;
  render3d_state *s = state(t);
  s->scene->has_background = e->use_background;
  s->scene->background = e->background;
  if (!s->ambient) {
    s->ambient = t3_ambient_light_new(0xffffff, 0);
    t3_object_add(&s->scene->base, s->ambient);
    t3_release(s->ambient);
    s->hemi = t3_hemisphere_light_new(0xffffff, 0xffffff, 0);
    t3_object_add(&s->scene->base, s->hemi);
    t3_release(s->hemi);
  }
  s->ambient->color = e->ambient_color;
  s->ambient->intensity = e->ambient_energy;
  s->hemi->color = e->sky_color;
  s->hemi->ground_color = e->ground_color;
  s->hemi->intensity = e->hemi_energy;
  if (e->fog_mode == 1) {
    s->scene->fog.type = T3_FOG_LINEAR;
    s->scene->fog.near = e->fog_near;
    s->scene->fog.far = e->fog_far;
  } else if (e->fog_mode == 2) {
    s->scene->fog.type = T3_FOG_EXP2;
    s->scene->fog.density = e->fog_density;
  } else {
    s->scene->fog.type = T3_FOG_NONE;
  }
  s->scene->fog.color = e->fog_color;
}

static const cw_prop env_props[] = {
  CW_PROP_FIELD(world_env, background, CW_T_COLOR, CW_PROP_DEFAULT, "Clear colour behind the scene: a scene-linear HDR value like every lit pixel (exposure and the tone curve apply to it, so fogged objects fade exactly into it)."),
  CW_PROP_FIELD(world_env, use_background, CW_T_BOOL, CW_PROP_DEFAULT, "Clear to the background colour."),
  CW_PROP_FIELD(world_env, ambient_color, CW_T_COLOR, CW_PROP_DEFAULT, "Ambient light colour."),
  CW_PROP_FIELD_RANGE(world_env, ambient_energy, CW_T_FLOAT, 0, 100, "Ambient light intensity."),
  CW_PROP_FIELD(world_env, sky_color, CW_T_COLOR, CW_PROP_DEFAULT, "Hemisphere light: sky colour."),
  CW_PROP_FIELD(world_env, ground_color, CW_T_COLOR, CW_PROP_DEFAULT, "Hemisphere light: ground colour."),
  CW_PROP_FIELD_RANGE(world_env, hemi_energy, CW_T_FLOAT, 0, 100, "Hemisphere light intensity."),
  { .name = "fog_mode", .type = CW_T_ENUM, .flags = CW_PROP_DEFAULT, .offset = offsetof(world_env, fog_mode),
    .size = sizeof(int), .enum_names = fog_names, .doc = "Fog type." },
  CW_PROP_FIELD(world_env, fog_color, CW_T_COLOR, CW_PROP_DEFAULT, "Fog colour."),
  CW_PROP_FIELD(world_env, fog_near, CW_T_FLOAT, CW_PROP_DEFAULT, "Linear fog start."),
  CW_PROP_FIELD(world_env, fog_far, CW_T_FLOAT, CW_PROP_DEFAULT, "Linear fog end."),
  CW_PROP_FIELD(world_env, fog_density, CW_T_FLOAT, CW_PROP_DEFAULT, "Exp2 fog density."),
  CW_PROP_END,
};

static cw_node_type env_type = {
  .name = "WorldEnvironment", .parent = "Node", .data_size = sizeof(world_env), .props = env_props,
  .doc = "Background, ambient light and fog for the tree's 3D scene.",
  .init = env_init, .pre_render = env_pre_render,
};

/* ── three.c's light cap ────────────────────────────────────────────── */

/* three.c (renderer.c's T3_MAX_LIGHTS) lights a surface with at most this
 * many lights of each kind; the rest, in scene order, are silently dropped */
#define THREE_LIGHT_CAP 8
int cw__lighting_cluster_kinds(cw_tree *t); /* src/render/lighting.c: 1 omni, 2 spot lights are clustered */
enum { LK_DIR, LK_POINT, LK_SPOT, LK_HEMI, LK_COUNT };

/* the lights three.c would collect: visible, under visible parents, on the
 * camera's layers. Per kind: the count, the lowest id of a plain light and
 * the highest id of a shadow caster. */
typedef struct light_count { int n[LK_COUNT]; uint32_t plain_min[LK_COUNT], caster_max[LK_COUNT]; } light_count;
static void count_lights(const t3_object *o, uint32_t layers, int clustered, light_count *c) {
  if (!o->visible) return;
  int k = o->type == T3_DIRECTIONAL_LIGHT ? LK_DIR : o->type == T3_POINT_LIGHT ? LK_POINT : o->type == T3_SPOT_LIGHT ? LK_SPOT
        : o->type == T3_HEMISPHERE_LIGHT ? LK_HEMI : -1;
  /* clustered lights (cartwheel/lighting.h) leave three.c's list: shadowless omni (and spot) lights */
  bool caster = (k == LK_POINT || k == LK_SPOT) && o->cast_shadow && ((const t3_light *)o)->shadow;
  if ((!caster || (clustered & 4)) && ((k == LK_POINT && (clustered & 1)) || (k == LK_SPOT && (clustered & 2)))) k = -1;
  if (k >= 0 && (o->layers & layers)) {
    c->n[k]++;
    if (o->cast_shadow && ((const t3_light *)o)->shadow) {
      if (o->id > c->caster_max[k]) c->caster_max[k] = o->id;
    } else if (o->id < c->plain_min[k]) {
      c->plain_min[k] = o->id;
    }
  }
  for (int i = 0; i < o->child_count; i++) count_lights(o->children[i], layers, clustered, c);
}

/* Lights three.c cannot use fail silently: say so once per cause, the
 * first frame and then once a second (a scene walk) until every cause has
 * been warned about. The causes: more lights of a kind than the cap (the
 * rest light nothing), and a shadow-casting light made after a plain light
 * of its kind (three.c orders a kind's lights by id and needs the casters
 * first: no lit material has a program, so nothing lit is drawn). */
static void check_light_cap(cw_tree *t, render3d_state *s, const t3_camera *cam) {
  if (s->light_warned == (1u << (2 * LK_COUNT)) - 1 || s->light_check_frame++ % 60) return;
  light_count c;
  memset(&c, 0, sizeof c);
  for (int k = 0; k < LK_COUNT; k++) c.plain_min[k] = UINT32_MAX;
  count_lights(&s->scene->base, cam->base.layers, cw__lighting_cluster_kinds(t), &c);
  static const char *const names[LK_COUNT] = { "directional", "point (OmniLight3D)", "spot (SpotLight3D)", "hemisphere" };
  for (int k = 0; k < LK_COUNT; k++) {
    if (c.n[k] > THREE_LIGHT_CAP && !(s->light_warned & (1u << k))) {
      s->light_warned |= (uint8_t)(1u << k);
      cw_warn("render3d: LIGHT CAP EXCEEDED: %d %s lights are in the scene but three.c lights with at most %d of a kind: "
              "the other %d (the last in scene order) light nothing. Keep at most %d in the scene (move a few real lights "
              "to the ones that matter, show the rest as emissive)",
              c.n[k], names[k], THREE_LIGHT_CAP, c.n[k] - THREE_LIGHT_CAP, THREE_LIGHT_CAP);
    }
    if (c.caster_max[k] > c.plain_min[k] && !(s->light_warned & (1u << (LK_COUNT + k)))) {
      s->light_warned |= (uint8_t)(1u << (LK_COUNT + k));
      cw_warn("render3d: LIGHT ORDER: a shadow-casting %s light was made after a plain one: three.c needs a kind's shadow "
              "casters first (in creation order), so NO lit material is drawn. Create the shadowed lights before the plain "
              "ones of their kind",
              names[k]);
    }
  }
}

/* ── drawing ────────────────────────────────────────────────────────── */

/* The camera of the viewmodel pass, posed as the main camera with its own field of view, or NULL when the
 * current camera has no viewmodel layers. Call after cw_render3d_prepare (the main camera's matrices). */
t3_camera *cw__render3d_viewmodel_camera(cw_tree *t) {
  cw_node cam = cw_render3d_camera(t);
  camera3d *c = cw_node_data_t(cam, camera3d_t);
  if (!c || !c->viewmodel_layers) return NULL;
  if (!c->vm) c->vm = t3_perspective_camera_new(c->viewmodel_fov, 1, c->near, c->far);
  t3_camera *m = c->cam, *v = c->vm;
  v->base.matrix_world = m->base.matrix_world;
  v->base.matrix_world_needs_update = false;
  v->matrix_world_inverse = m->matrix_world_inverse;
  memcpy(v->_mwid, m->_mwid, sizeof v->_mwid);
  v->base.position = m->base.position;
  v->base.quaternion = m->base.quaternion;
  v->fov = c->viewmodel_fov;
  v->near = c->near;
  v->far = c->far;
  v->aspect = m->aspect;
  v->base.layers = (uint32_t)c->viewmodel_layers;
  t3_camera_update_projection_matrix(v);
  return v;
}

t3_camera *cw_render3d_prepare(cw_tree *t, int w, int h) {
  render3d_state *s = cw_tree_slot(t, "render3d");
  if (!s) return NULL; /* no Node3D ever entered this tree */
  cw__skin_shading_frame(); /* skin materials follow the quality setting */
  cw_node cam = cw_render3d_camera(t);
  camera3d *c = cw_node_data_t(cam, camera3d_t);
  if (!c) return NULL;
  if (!s->renderer) {
    cw__programs_register();
    s->renderer = t3_renderer_new(w, h);
    t3_renderer_set_sort_bounds(s->renderer, !getenv("CW_SORT_ORIGINS"));
    cw__decals_renderer_init(s->renderer);
    cw__skin_shading_renderer_init(s->renderer);
    cw__rim_light_renderer_init(s->renderer);
#if CW_WGPU
    /* a WebGPU frame: three.c's WebGPU backend, drawing into the pipeline's
     * scene target on its frame encoder (the scene pass) */
    if (cw_gpu_backend() == CW_GPU_WEBGPU)
      t3_renderer_use_wgpu(s->renderer, cw__wgpu_device(), cw__wgpu_queue(), (int)WGPUTextureFormat_RGBA16Float);
#endif
    s->w = w;
    s->h = h;
    /* shadow maps once per view: the passes after the first (the scene after a
     * depth prepass, the visibility fade) reuse them (three.c shadowMap.autoUpdate) */
    t3_renderer_set_shadow_auto_update(s->renderer, false);
  } else if (w != s->w || h != s->h) {
    t3_renderer_set_size(s->renderer, w, h);
    s->w = w;
    s->h = h;
  }
  if (s->shadows_enabled) t3_renderer_set_shadow_map(s->renderer, true, cw__quality_soft_shadows() ? T3_PCF_SOFT_SHADOW_MAP : T3_PCF_SHADOW_MAP);
  /* this view's first render draws them; splitscreen's later views reuse the
   * first view's (a light's shadow camera is its own, not the view's) */
  if (!s->shadows_keep) t3_renderer_shadow_needs_update(s->renderer);
  /* likewise the world matrices: the frame's first view brings the scene's up
   * to date and the later views draw it as it stands (a view hook that moves
   * something updates what it moved) */
  t3_scene *sc = cw_render3d_scene(t);
  if (sc) sc->auto_update = !s->shadows_keep;
  c->cam->fov = c->fov;
  c->cam->near = c->near;
  c->cam->far = c->far;
  c->cam->aspect = h > 0 ? (float)w / (float)h : 1;
  c->cam->base.layers = (uint32_t)c->cull_layers;
  t3_camera_update_projection_matrix(c->cam);
  check_light_cap(t, s, c->cam);
  return c->cam;
}

void cw__render3d_frame_begin(cw_tree *t) {
  render3d_state *s = cw_tree_slot(t, "render3d");
  if (s) s->frame_calls = s->frame_triangles = 0, s->frame_counted = false;
}
/* every tree's skipped draws, the debug field render.skipped: tools/run-cart.mjs
 * fails a run where it is not 0 (every host skipping the same draws would
 * agree with itself) */
static int32_t skipped_all;
static void note_skipped(render3d_state *s, const t3_render_info *i) {
  static bool registered;
  if (!registered) registered = true, cw_debug_i32("render.skipped", &skipped_all);
  if (!i->skipped) return;
  s->skipped_total += i->skipped;
  skipped_all += (int32_t)i->skipped;
  const char *e = t3_renderer_last_error(s->renderer);
  if (!e) e = "";
  if (strncmp(s->skipped_said, e, sizeof s->skipped_said - 1)) {
    snprintf(s->skipped_said, sizeof s->skipped_said, "%s", e);
    cw_warn("render3d: three.c skipped %u draw(s): %s", i->skipped, e);
  }
  /* once per run and kind, at error level (a scene that draws nothing looks like an empty scene), naming the cause
   * three.c gave: only a state missing from the table is fixed by regenerating the table */
  static bool said_table, said_ext, said_other;
  if (strstr(e, "no table state") || strstr(e, "the runtime builder failed")) {
    if (!said_table) {
      said_table = true;
      cw_error("render3d: draws are being skipped because this app's program table lacks a state it needs. Regenerate it with: node tools/programs.mjs <app>  (see docs/guides/rendering-3d.md, program tables)");
    }
  } else if (strstr(e, "left texture") || strstr(e, "no hook for extension")) {
    if (!said_ext) {
      said_ext = true;
      cw_error("render3d: draws are being skipped because a material extension's fill hook left a texture its program samples unset (the table has the state: the node that owns the extension did not bind the texture this frame)");
    }
  } else if (!said_other) {
    said_other = true;
    cw_error("render3d: draws are being skipped: three.c could not draw them (the reason is in the warning above)");
  }
}
void cw__render3d_count(cw_tree *t) {
  render3d_state *s = cw_tree_slot(t, "render3d");
  if (!s || !s->renderer) return;
  const t3_render_info *i = t3_renderer_info(s->renderer);
  s->frame_calls += i->calls;
  s->frame_triangles += i->triangles;
  s->frame_counted = true;
  note_skipped(s, i);
}

bool cw__render3d_shadows_kept(cw_tree *t) {
  render3d_state *s = cw_tree_slot(t, "render3d");
  return s && s->shadows_keep;
}

void cw__render3d_keep_shadows(cw_tree *t, bool keep) {
  render3d_state *s = cw_tree_slot(t, "render3d");
  if (s) s->shadows_keep = keep;
}

/* the camera half of cw_render3d_prepare: matrices only, no t3_renderer
 * (WebGPU runs, where three.c has no backend yet) */
t3_camera *cw__render3d_camera_prepare(cw_tree *t, int w, int h) {
  render3d_state *s = cw_tree_slot(t, "render3d");
  if (!s) return NULL;
  camera3d *c = cw_node_data_t(cw_render3d_camera(t), camera3d_t);
  if (!c) return NULL;
  c->cam->fov = c->fov;
  c->cam->near = c->near;
  c->cam->far = c->far;
  c->cam->aspect = h > 0 ? (float)w / (float)h : 1;
  t3_camera_update_projection_matrix(c->cam);
  return c->cam;
}

bool cw_render3d_draw(cw_tree *t, int w, int h) {
  t3_camera *cam = cw_render3d_prepare(t, w, h);
  if (!cam) return false;
  CW_PROFILE_SCOPE("render3d.draw");
  double t0 = cw__render3d_now_ms();
  render3d_state *s = state(t);
  t3_renderer_render(s->renderer, s->scene, cam);
  s->cpu_ms = (float)(cw__render3d_now_ms() - t0);
  note_skipped(s, t3_renderer_info(s->renderer));
  return true;
}

double cw__render3d_now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec * 1e3 + (double)ts.tv_nsec / 1e6;
}

void cw__render3d_set_cpu_ms(cw_tree *t, float ms) {
  render3d_state *s = cw_tree_slot(t, "render3d");
  if (s) s->cpu_ms = ms;
}

void cw_render3d_get_stats(cw_tree *t, cw_render3d_stats *out) {
  memset(out, 0, sizeof *out);
  render3d_state *s = cw_tree_slot(t, "render3d");
  if (!s || !s->renderer) return;
  const t3_render_info *i = t3_renderer_info(s->renderer);
  /* the pipeline's frame (every view's renders), else the last render's */
  out->draw_calls = (int)(s->frame_counted ? s->frame_calls : i->calls);
  out->triangles = (int)(s->frame_counted ? s->frame_triangles : i->triangles);
  out->programs = (int)i->programs;
  out->cpu_ms = s->cpu_ms;
  out->skipped = (int)s->skipped_total;
}

void cw__register_node3d_types(void) {
  cw_node_type_register(&camera3d_type);
  camera3d_t = &camera3d_type;
  cw_node_type_register(&mesh3d_type);
  mesh3d_t = &mesh3d_type;
  cw_node_type_register(&light3d_base);
  light3d_t = &light3d_base;
  dir_type.init = dir_leaf_init;
  omni_type.init = omni_leaf_init;
  spot_type.init = spot_leaf_init;
  cw_node_type_register(&dir_type);
  cw_node_type_register(&omni_type);
  cw_node_type_register(&spot_type);
  cw_node_type_register(&env_type);
  void cw__register_pipeline_types(void); /* src/render/pipeline_nodes.c */
  cw__register_pipeline_types();
  void cw__register_sky_types(void); /* src/render/sky.c */
  cw__register_sky_types();
  void cw__register_fog_types(void); /* src/render/fog.c */
  cw__register_fog_types();
  void cw__register_fog_volume_types(void); /* src/render/volumetric_fog.c */
  cw__register_fog_volume_types();
  void cw__register_lightmap_types(void); /* src/render/lightmap.c */
  cw__register_lightmap_types();
  void cw__register_lighting_types(void); /* src/render/lighting.c */
  cw__register_lighting_types();
}


bool cw__mesh3d_bake_info(cw_node n, cw__mesh_bake *out) {
  mesh3d *m = cw_node_data_t(n, mesh3d_t);
  if (!m) return false;
  memset(out, 0, sizeof *out);
  out->is_static = m->gi_mode == 1;
  out->mesh = cw__mesh3d_t3(n);
  out->instanced = out->mesh && out->mesh->base.type == T3_INSTANCED_MESH;
  out->scale = m->lightmap_scale;
  t3_material *mat = out->mesh && out->mesh->material_count ? out->mesh->materials[0] : NULL;
  out->albedo = mat ? mat->color : m->color;
  out->emissive = mat ? mat->emissive : m->emissive;
  if (mat) {
    float k = mat->emissive_intensity;
    out->emissive = (t3_color){ out->emissive.r * k, out->emissive.g * k, out->emissive.b * k };
  }
  return out->mesh != NULL;
}

/* what a BAKED lightmapped mesh draws with: an unlit (Basic) copy of its material that keeps the surface (colour,
 * maps, vertex colours, transparency) and takes all its light from the lightmap: Basic with a lightMap is
 * albedo x irradiance / PI, the Lambert diffuse of the baked light, and no real-time light reaches it */
static t3_material *baked_material(const t3_material *src) {
  t3_material *b = t3_mesh_basic_material_new(0xffffff);
  b->color = src->color;
  b->vertex_colors = src->vertex_colors;
  b->opacity = src->opacity;
  b->transparent = src->transparent;
  b->alpha_test = src->alpha_test;
  b->side = src->side;
  b->fog = src->fog;
  b->depth_write = src->depth_write;
  b->depth_test = src->depth_test;
  b->blending = src->blending;
  b->ao_map_intensity = src->ao_map_intensity;
  if (src->map) t3_material_set_texture(b, T3_MAP, src->map);
  if (src->ao_map) t3_material_set_texture(b, T3_AO_MAP, src->ao_map);
  if (src->alpha_map) t3_material_set_texture(b, T3_ALPHA_MAP, src->alpha_map);
  return b;
}

void cw__mesh3d_set_lightmap(cw_node n, t3_geometry *g, t3_texture *lm, float intensity, bool baked) {
  mesh3d *m = cw_node_data_t(n, mesh3d_t);
  t3_mesh *mesh = m ? cw__mesh3d_t3(n) : NULL;
  if (!mesh || !mesh->material_count) return;
  if (g) m->lightmapped = true;
  if (g && mesh->geometry != g) {
    t3_retain(g);
    t3_release(mesh->geometry);
    mesh->geometry = g;
  }
  t3_material *mat = mesh->materials[0];
  if (mat != m->lm_mat) {
    /* a resource's material is shared with its other users, a primitive's
     * with every primitive of the same look: this mesh gets its own */
    if (m->lm_mat) t3_release(m->lm_mat);
    m->lm_mat = baked ? baked_material(mat) : t3_material_clone(mat);
    t3_retain(m->lm_mat);
    t3_release(mesh->materials[0]);
    mesh->materials[0] = m->lm_mat;
    mat = m->lm_mat;
  }
  t3_material_set_texture(mat, T3_LIGHT_MAP, lm);
  mat->light_map_intensity = intensity;
}

bool cw__light3d_bake_info(cw_node n, cw__light *out) {
  light3d *l = cw_node_data_t(n, light3d_t);
  if (!l || !l->bake || l->energy <= 0) return false;
  cw_node3d *nd = cw_node_data(n, "Node3D");
  if (!nd || !nd->visible) return false;
  for (t3_object *o = l->light->base.parent; o; o = o->parent)
    if (!o->visible) return false;
  const t3_mat4 *m = &l->light->base.matrix_world;
  memset(out, 0, sizeof *out);
  out->node = n;
  out->kind = l->kind;
  out->pos = (t3_vec3){ m->e[12], m->e[13], m->e[14] };
  out->dir = t3_vec3_normalize((t3_vec3){ -m->e[8], -m->e[9], -m->e[10] });
  out->color = (t3_color){ l->color.r * l->energy, l->color.g * l->energy, l->color.b * l->energy };
  out->range = l->range;
  out->decay = l->decay;
  out->angle = l->angle;
  out->penumbra = l->penumbra;
  out->direct = l->bake_direct;
  return true;
}

void cw__world_env_bake(cw_tree *t, t3_color *ambient, t3_color *sky, t3_color *ground) {
  render3d_state *s = cw_tree_slot(t, "render3d");
  *ambient = *sky = *ground = (t3_color){ 0, 0, 0 };
  if (!s) return;
  if (s->ambient) *ambient = (t3_color){ s->ambient->color.r * s->ambient->intensity, s->ambient->color.g * s->ambient->intensity,
                                         s->ambient->color.b * s->ambient->intensity };
  if (s->hemi) {
    float k = s->hemi->intensity;
    *sky = (t3_color){ s->hemi->color.r * k, s->hemi->color.g * k, s->hemi->color.b * k };
    *ground = (t3_color){ s->hemi->ground_color.r * k, s->hemi->ground_color.g * k, s->hemi->ground_color.b * k };
  }
}

t3_mesh *cw__mesh3d_t3(cw_node n) {
  mesh3d *m = cw_node_data_t(n, mesh3d_t);
  if (!m) return cw_csg_baked_mesh(n); /* a baked CSG root (cartwheel/csg.h) serves too */
  mesh_rebuild(n, m);
  /* a mesh resource that is a single mesh serves too (a scene of several
   * meshes does not: give each its own MeshInstance3D) */
  if (m->res_obj) return m->res_obj->type == T3_MESH ? (t3_mesh *)m->res_obj : NULL;
  return m->t3;
}
