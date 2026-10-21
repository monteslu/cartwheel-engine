/* Effects and visibility unit suite: the visibility BVH against brute force
 * (random boxes and frusta, before and after a refit), the box / frustum
 * test, curve parsing, the particle emission function's alive counts (3D and
 * 2D, ticked without drawing) and the MeshInstance3D LOD hooks. */
#include <math.h>
#include <string.h>

#include "cartwheel/particles.h"
#include "cartwheel/pipeline.h"
#include "cartwheel/render3d.h"
#include "scene/render3d_internal.h"
#include "test.h"
#include "visibility/vis_internal.h"

bool cw__fx_curve_bake(const char *text, int comps, float def, int n, float *out, const char *what);

static void random_frustum(cw_rng *r, vis_frustum *f) {
  t3_camera *c = t3_perspective_camera_new(cw_rng_range(r, 30, 100), cw_rng_range(r, 0.5f, 2.5f), 0.1f,
                                           cw_rng_range(r, 50, 400));
  c->base.position = (t3_vec3){ cw_rng_range(r, -60, 60), cw_rng_range(r, 0, 40), cw_rng_range(r, -60, 60) };
  t3_object_look_at(c, cw_rng_range(r, -60, 60), cw_rng_range(r, 0, 10), cw_rng_range(r, -60, 60));
  t3_object_update_world_matrix(c, false, false);
  t3_camera_update_projection_matrix(c);
  t3_mat4 view, vp;
  t3_mat4_invert(&view, &c->base.matrix_world);
  t3_mat4_multiply(&vp, &c->projection_matrix, &view);
  cw__vis_frustum_from(f, &vp);
  t3_release(c);
}

static void test_bvh(void) {
  enum { N = 3000 };
  cw_rng r;
  cw_rng_seed(&r, 99);
  float *mn = cw_alloc(sizeof(float) * 3 * N, CW_MEM_GAME), *mx = cw_alloc(sizeof(float) * 3 * N, CW_MEM_GAME);
  int32_t *ids = cw_alloc(sizeof(int32_t) * N, CW_MEM_GAME);
  for (int i = 0; i < N; i++) {
    for (int k = 0; k < 3; k++) {
      float c = cw_rng_range(&r, -100, 100), e = cw_rng_range(&r, 0.1f, 4);
      mn[3 * i + k] = c - e;
      mx[3 * i + k] = c + e;
    }
    ids[i] = i;
  }
  vis_bvh b = { .root = -1 };
  cw__vis_bvh_build(&b, N, mn, mx, ids);
  CHECK(b.root >= 0);
  CHECK_EQ_INT(b.prims.count, N);
  uint8_t *out = cw_alloc(N, CW_MEM_GAME);
  int32_t missed = 0, extra = 0, visible = 0;
  for (int pass = 0; pass < 2; pass++) {
    if (pass == 1) { /* move every box and refit */
      for (int i = 0; i < 3 * N; i++) {
        float d = cw_rng_range(&r, -3, 3);
        mn[i] += d;
        mx[i] += d;
      }
      cw__vis_bvh_refit(&b, mn, mx, 3);
    }
    for (int t = 0; t < 40; t++) {
      vis_frustum f;
      random_frustum(&r, &f);
      memset(out, 0, N);
      cw__vis_bvh_cull(&b, &f, NULL, out);
      for (int i = 0; i < N; i++) {
        int exact = cw__vis_box_test(&f, mn + 3 * i, mx + 3 * i, NULL);
        visible += exact != 0;
        missed += exact && !out[i]; /* a false negative would hide a visible mesh */
        extra += !exact && out[i];  /* conservative leaves: allowed */
      }
    }
  }
  CHECK_EQ_INT(missed, 0);
  CHECK(visible > 1000);
  CHECK(extra < visible); /* the BVH rejects most of what is outside */
  /* (visible and extra are not folded: the random frusta come through libm
   * tan, which differs between C libraries in the last bits) */
  FOLD(missed);
  cw__vis_bvh_free(&b);
  cw_free(out);
  cw_free(ids);
  cw_free(mx);
  cw_free(mn);
}

static void test_box_test(void) {
  vis_frustum f = { .n = 1 };
  f.nx[0] = 1; /* x >= 0 inside */
  float a0[3] = { 1, 0, 0 }, a1[3] = { 2, 1, 1 };
  float b0[3] = { -2, 0, 0 }, b1[3] = { -1, 1, 1 };
  float c0[3] = { -1, 0, 0 }, c1[3] = { 1, 1, 1 };
  CHECK_EQ_INT(cw__vis_box_test(&f, a0, a1, NULL), 2);
  CHECK_EQ_INT(cw__vis_box_test(&f, b0, b1, NULL), 0);
  CHECK_EQ_INT(cw__vis_box_test(&f, c0, c1, NULL), 1);
  /* swept toward the inside by 3 units, the outside box reaches in */
  float sw[3] = { 3, 0, 0 };
  CHECK_EQ_INT(cw__vis_box_test(&f, b0, b1, sw), 1);
  float sw2[3] = { -3, 0, 0 };
  CHECK_EQ_INT(cw__vis_box_test(&f, b0, b1, sw2), 0);
}

static void test_curves(void) {
  float v[5];
  CHECK(cw__fx_curve_bake("0 0, 1 2", 1, 1, 5, v, "test"));
  CHECK(fabsf(v[0]) < 1e-6f && fabsf(v[2] - 1) < 1e-6f && fabsf(v[4] - 2) < 1e-6f);
  CHECK(cw__fx_curve_bake("", 1, 7, 5, v, "test"));
  CHECK(v[3] == 7);
  CHECK(cw__fx_curve_bake("0.5 3", 1, 1, 5, v, "test"));
  CHECK(v[0] == 3 && v[4] == 3);
  float c[3 * 4];
  CHECK(cw__fx_curve_bake("0 1 0 0 1, 1 0 0 1 0", 4, 1, 3, c, "test"));
  CHECK(fabsf(c[4] - 0.5f) < 1e-6f && fabsf(c[6] - 0.5f) < 1e-6f && fabsf(c[7] - 0.5f) < 1e-6f);
  CHECK(!cw__fx_curve_bake("0 1, oops", 1, 1, 5, v, "test (an expected error)"));
  CHECK(v[0] == 1 && v[4] == 1);
}

static void test_emission(void) {
  cw_scene_register_builtin_types();
  cw_tree *t = cw_tree_new();
  cw_node p = cw_node_new("GPUParticles3D", "P");
  cw_node_set(p, "amount", cw_val_int(1000));
  cw_node_set(p, "lifetime", cw_val_float(1));
  cw_node_add_child(cw_tree_root(t), p);
  cw_node q = cw_node_new("GPUParticles2D", "Q");
  cw_node_set(q, "amount", cw_val_int(1000));
  cw_node_set(q, "lifetime", cw_val_float(1));
  cw_node_add_child(cw_tree_root(t), q);
  for (int i = 0; i < 45; i++) cw_tree_fixed_update(t, 1.0 / 60);
  /* 0.75 s in: particles 0..~750 have spawned, none died */
  int a3 = cw_particles_alive(p), a2 = cw_particles_alive(q);
  CHECK(a3 >= 749 && a3 <= 751);
  CHECK_EQ_INT(a2, a3);
  FOLD(a3);
  for (int i = 0; i < 60; i++) cw_tree_fixed_update(t, 1.0 / 60);
  /* steady state: every slot alive (lifetime_random 0) */
  CHECK_EQ_INT(cw_particles_alive(p), 1000);
  /* stop: the last spawns live out their second */
  cw_node_set(p, "emitting", cw_val_bool(false));
  for (int i = 0; i < 61; i++) cw_tree_fixed_update(t, 1.0 / 60);
  CHECK_EQ_INT(cw_particles_alive(p), 0);
  /* one shot with explosiveness 1: all at once, gone after one lifetime */
  cw_node_set(p, "explosiveness", cw_val_float(1));
  cw_node_set(p, "one_shot", cw_val_bool(true));
  cw_node_set(p, "emitting", cw_val_bool(true));
  cw_tree_fixed_update(t, 1.0 / 60);
  CHECK_EQ_INT(cw_particles_alive(p), 1000);
  for (int i = 0; i < 70; i++) cw_tree_fixed_update(t, 1.0 / 60);
  CHECK_EQ_INT(cw_particles_alive(p), 0);
  cw_tree_free(t);
}

static void test_prewarmed_emission(void) {
  cw_scene_register_builtin_types();
  cw_tree *t = cw_tree_new();
  cw_node p = cw_node_new("GPUParticles3D", "Prewarmed");
  cw_node_set(p, "amount", cw_val_int(120));
  cw_node_set(p, "lifetime", cw_val_float(1));
  cw_node_set(p, "preprocess", cw_val_float(3));
  cw_node_set(p, "emitting", cw_val_bool(false));
  cw_node_add_child(cw_tree_root(t), p);
  cw_tree_fixed_update(t, 1.0 / 60);
  CHECK_EQ_INT(cw_particles_alive(p), 0);
  cw_node_set(p, "emitting", cw_val_bool(true));
  cw_tree_fixed_update(t, 1.0 / 60);
  CHECK_EQ_INT(cw_particles_alive(p), 120);
  cw_node_set(p, "emitting", cw_val_bool(false));
  for (int i = 0; i < 30; i++) cw_tree_fixed_update(t, 1.0 / 60);
  int draining = cw_particles_alive(p);
  CHECK(draining >= 59 && draining <= 61);
  for (int i = 0; i < 100; i++) cw_tree_fixed_update(t, 1.0 / 60);
  CHECK_EQ_INT(cw_particles_alive(p), 0);
  cw_particles_restart(p);
  cw_tree_fixed_update(t, 1.0 / 60);
  CHECK_EQ_INT(cw_particles_alive(p), 120);
  cw_tree_free(t);
}

static void test_lod_hooks(void) {
  cw_scene_register_builtin_types();
  cw_tree *t = cw_tree_new();
  cw_node m = cw_node_new("MeshInstance3D", "M");
  cw_node_set(m, "primitive", cw_val_enum("SPHERE"));
  cw_node_set(m, "size", cw_val_vec3(2, 2, 2));
  cw_node_add_child(cw_tree_root(t), m);
  cw__mesh_info info;
  CHECK(cw__mesh3d_info(m, &info));
  CHECK_EQ_INT(info.lod_levels, 4);
  CHECK(info.obj != NULL);
  float e1 = cw__mesh3d_lod_error(m, 1), e2 = cw__mesh3d_lod_error(m, 2), e3 = cw__mesh3d_lod_error(m, 3);
  CHECK(cw__mesh3d_lod_error(m, 0) == 0 && e1 > 0 && e2 > e1 && e3 > e2);
  int v0 = ((t3_mesh *)info.obj)->geometry->attributes[T3_ATTR_POSITION]->count;
  cw__mesh3d_set_lod(m, 3);
  int v3 = ((t3_mesh *)info.obj)->geometry->attributes[T3_ATTR_POSITION]->count;
  CHECK(v3 < v0 / 4);
  /* the mesh object carries the size: a coarser level keeps the scale */
  CHECK(fabsf(info.obj->scale.x - 2) < 1e-6f);
  cw_node b = cw_node_new("MeshInstance3D", "B");
  cw_node_add_child(cw_tree_root(t), b);
  CHECK(cw__mesh3d_info(b, &info));
  CHECK_EQ_INT(info.lod_levels, 1);
  /* two boxes share one unit geometry (instancing) */
  cw_node b2 = cw_node_new("MeshInstance3D", "B2");
  cw_node_set(b2, "size", cw_val_vec3(3, 1, 5));
  cw_node_add_child(cw_tree_root(t), b2);
  cw__mesh_info i2;
  CHECK(cw__mesh3d_info(b2, &i2));
  CHECK(((t3_mesh *)info.obj)->geometry == ((t3_mesh *)i2.obj)->geometry);
  cw_tree_free(t);
}

bool cw__vis_test_restore_active(cw_pass_ctx *ctx);
void cw__vis_test_exec(cw_pass_ctx *ctx,int phase);
static void test_split_visibility_restore(void) {
  cw_scene_register_builtin_types();
  cw_tree *t=cw_tree_new();
  cw_node mgr=cw_node_new("VisibilityManager","Visibility");
  cw_node_set(mgr,"occlusion_cull",cw_val_bool(false));
  cw_node_set(mgr,"impostors",cw_val_bool(false));
  cw_node_add_child(cw_tree_root(t),mgr);
  cw_node nodes[2];t3_mesh *meshes[2];
  for(int i=0;i<2;i++) {
    nodes[i]=cw_node_new("MeshInstance3D",NULL);
    cw_node_set(nodes[i],"position",cw_val_vec3(i*100,0,-10));
    cw_node_set(nodes[i],"static",cw_val_bool(true));
    cw_node_add_child(cw_tree_root(t),nodes[i]);
  }
  cw_tree_pre_render(t,0);
  for(int i=0;i<2;i++){cw__mesh_info info;cw__mesh3d_info(nodes[i],&info);meshes[i]=(t3_mesh *)info.obj;}
  t3_camera *cam=t3_perspective_camera_new(60,1,.1f,50);
  cw_pass_ctx ctx={.tree=t,.scene3d=cw_render3d_scene(t),.camera=cam,.width=64,.height=64};
  for(int frame=0;frame<3;frame++)for(int view=0;view<2;view++) {
    cam->base.position.x=view*100;
    /* Pipeline activity is evaluated before visibility executes, including
     * the first frame when no mesh has been hidden yet. */
    bool restore = cw__vis_test_restore_active(&ctx);
    CHECK(restore);
    cw__vis_test_exec(&ctx,0);
    CHECK(meshes[view]->base.visible);
    CHECK(!meshes[1-view]->base.visible);
    if (frame & 1) cw__visibility_end_view(t);
    else if (restore) cw__vis_test_exec(&ctx,2);
    CHECK(meshes[0]->base.visible && meshes[1]->base.visible);
  }
  t3_release(cam);cw_tree_free(t);
}

/* The manager must use every placement's fixed bound, even when the
 * currently active LOD subset is empty or far outside the view. */
static void test_fixed_instance_bounds(void) {
  cw_scene_register_builtin_types();
  cw_tree *t = cw_tree_new();
  cw_node mgr = cw_node_new("VisibilityManager", NULL);
  cw_node_set(mgr, "occlusion_cull", cw_val_bool(false));
  cw_node_set(mgr, "impostors", cw_val_bool(false));
  cw_node_add_child(cw_tree_root(t), mgr);
  cw_node n = cw_node_new("MeshInstance3D", NULL);
  cw_node_set(n, "static", cw_val_bool(true));
  cw_node_add_child(cw_tree_root(t), n);
  t3_geometry *g = t3_box_geometry_new(1,1,1,1,1,1);
  t3_material *mat = t3_mesh_standard_material_new(0xffffff);
  t3_mesh *root = t3_mesh_new(g,mat);
  t3_instanced_mesh *im = t3_instanced_mesh_new(g,mat,1);
  im->count = 0;
  t3_sphere bound = { .center = {100,0,-10}, .radius = 3 };
  t3_instanced_mesh_set_bounding_sphere(im,&bound);
  t3_object_add(&root->base,&im->mesh.base);
  CHECK(cw_mesh3d_set_procedural(n,root));
  cw_tree_pre_render(t,0);
  t3_camera *cam = t3_perspective_camera_new(60,1,.1f,50);
  cw_pass_ctx ctx = {.tree=t,.scene3d=cw_render3d_scene(t),.camera=cam,.width=64,.height=64};
  for(int frame=0;frame<2;frame++) {
    cam->base.position.x=100;
    cw__vis_test_exec(&ctx,0);
    CHECK(root->base.visible);
    cw__visibility_end_view(t);
    cam->base.position.x=300;
    cw__vis_test_exec(&ctx,0);
    CHECK(!root->base.visible);
    cw__visibility_end_view(t);
    /* Changing the active subset does not shrink the cached sphere. */
    im->count=1;
  }
  t3_release(cam); cw_tree_free(t);
  t3_release(im);t3_release(root);t3_release(mat);t3_release(g);
}

CW_TEST_SUITE("fx", { "fixed_instance_bounds", test_fixed_instance_bounds }, { "bvh", test_bvh }, { "box_test", test_box_test }, { "curves", test_curves },
              { "emission", test_emission }, { "prewarmed_emission", test_prewarmed_emission }, { "lod_hooks", test_lod_hooks }, { "split_visibility_restore", test_split_visibility_restore })
