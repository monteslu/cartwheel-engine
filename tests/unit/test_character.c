/* Character rendering pieces: glTF material names and KHR_materials_*
 * values, skin shading from the file and from code (and its quality switch),
 * morph targets by name on instances of one skinned glTF, and morph weight
 * tracks in AnimationPlayer clips. Uses samples/character_skin's generated
 * character.glb. */
#include <math.h>
#include <string.h>

#include "cartwheel/anim.h"
#include "cartwheel/morph.h"
#include "cartwheel/quality.h"
#include "cartwheel/resource.h"
#include "cartwheel/scene.h"
#include "cartwheel/shading.h"
#include "cartwheel/skin_shading.h"
#include "test.h"
#include "three.h"

void cw__skin_shading_frame(void); /* src/render/skin_shading.c: follows the quality setting */

#define NEAR(a, b, e) CHECK(fabsf((float)(a) - (float)(b)) <= (e))

typedef struct { const char *name; t3_material *m; int meshes; } find_ctx;
static void find_mat(t3_object *o, void *u) {
  find_ctx *c = u;
  if (o->type != T3_MESH && o->type != T3_SKINNED_MESH) return;
  t3_mesh *m = (t3_mesh *)o;
  for (int i = 0; i < m->material_count; i++)
    if (m->materials[i]->name && !strcmp(m->materials[i]->name, c->name)) { c->m = m->materials[i]; c->meshes++; }
}
static find_ctx material(t3_object *root, const char *name) {
  find_ctx c = { name, NULL, 0 };
  t3_object_traverse(root, find_mat, &c);
  return c;
}

static void test_materials(void) {
  cw_res r = cw_res_load("character.glb", CW_RES_GLTF);
  const cw_gltf *g = cw_res_gltf(r);
  CHECK(g != NULL);
  if (!g) return;
  t3_object *o = g->gltf->scene;
  find_ctx skin = material(o, "Skin"), knit = material(o, "Knit"), eye = material(o, "Eye"), leather = material(o, "Leather"),
           hair = material(o, "Hair"), mouth = material(o, "MouthInside");
  CHECK(skin.m && knit.m && eye.m && leather.m && hair.m && mouth.m);
  if (!skin.m || !knit.m || !eye.m || !leather.m || !hair.m || !mouth.m) { cw_res_release(r); return; }
  CHECK_EQ_INT(skin.meshes, 2); /* face and hands share it */
  /* KHR_materials_sheen / clearcoat / specular / ior: physical materials */
  CHECK_EQ_INT(knit.m->type, T3_MESH_PHYSICAL_MATERIAL);
  NEAR(knit.m->sheen, 1, 0);
  NEAR(knit.m->sheen_color.b, 0.8f, 1e-6f);
  NEAR(knit.m->sheen_roughness, 0.8f, 1e-6f);
  NEAR(eye.m->clearcoat, 1, 0);
  NEAR(eye.m->clearcoat_roughness, 0.03f, 1e-6f);
  NEAR(leather.m->ior, 1.45f, 1e-6f);
  NEAR(skin.m->specular_intensity, 0.5f, 1e-6f);
  CHECK_EQ_INT(mouth.m->type, T3_MESH_STANDARD_MATERIAL); /* no extension: standard */
  /* skin shading from the file's extras, on the material the meshes draw */
  cw_skin_shading_desc d;
  CHECK(cw_skin_shading_get(skin.m, &d));
  NEAR(d.translucency, 0.45f, 1e-6f);
  NEAR(d.scatter.g, 0.35f, 1e-6f);
  CHECK(!cw_skin_shading_get(knit.m, NULL));
  CHECK(cw_shading_has(skin.m, "skinshade"));
  /* the quality switch: Low draws plain PBR, the others skin */
  cw_quality_set(CW_QUALITY_LOW, false);
  cw__skin_shading_frame();
  CHECK(!cw_skin_shading_enabled());
  CHECK(!cw_shading_has(skin.m, "skinshade"));
  CHECK(cw_skin_shading_get(skin.m, NULL)); /* still skin: back on at the next preset */
  cw_quality_set(CW_QUALITY_MEDIUM, false);
  cw__skin_shading_frame();
  CHECK(cw_shading_has(skin.m, "skinshade"));
  cw_quality_set(CW_QUALITY_HIGH, false);
  cw__skin_shading_frame();
  /* from code, by name: the knit becomes skin (a test, not a look) and back */
  int before = cw_skin_shading_count();
  CHECK_EQ_INT(cw_skin_shading_apply_named(o, "Knit", NULL), 1);
  CHECK_EQ_INT(cw_skin_shading_count(), before + 1);
  cw_skin_shading_remove(knit.m);
  CHECK(!cw_shading_has(knit.m, "skinshade"));
  CHECK_EQ_INT(cw_skin_shading_count(), before);
  /* not a lit PBR material: refused */
  t3_material *basic = t3_mesh_basic_material_new(0xffffff);
  CHECK(!cw_skin_shading_apply(basic, NULL));
  t3_release(basic);
  int32_t n = cw_skin_shading_count();
  FOLD(n);
  cw_res_release(r);
}

static void test_morph_instances(void) {
  cw_res r = cw_res_load("character.glb", CW_RES_GLTF);
  if (!cw_res_gltf(r)) { CHECK(!"character.glb"); return; }
  t3_object *a = cw_gltf_instantiate(r), *b = cw_gltf_instantiate(r);
  CHECK(a && b);
  if (!a || !b) return;
  const char *names[8];
  CHECK_EQ_INT(cw_morph_names(a, names, 8), 4);
  CHECK(!strcmp(names[0], "smile") && !strcmp(names[1], "jawOpen") && !strcmp(names[2], "browUp") && !strcmp(names[3], "blink"));
  /* smile is on the face and the mouth: one call moves both */
  CHECK_EQ_INT(cw_morph_set(a, "smile", 0.75f), 2);
  CHECK_EQ_INT(cw_morph_set(a, "blink", 1), 1);
  CHECK_EQ_INT(cw_morph_set(a, "frown", 1), 0);
  NEAR(cw_morph_get(a, "smile"), 0.75f, 0);
  NEAR(cw_morph_get(b, "smile"), 0, 0); /* the other instance keeps its own weights */
  /* instances share geometry (the morph data once) and materials */
  t3_mesh *fa = (t3_mesh *)t3_object_get_by_name(a, "Face"), *fb = (t3_mesh *)t3_object_get_by_name(b, "Face");
  CHECK(fa && fb && fa != fb);
  if (fa && fb) {
    CHECK(fa->geometry == fb->geometry && fa->materials[0] == fb->materials[0]);
    CHECK(fa->morph_influences != fb->morph_influences);
    CHECK_EQ_INT(fa->geometry->morph_count, 3);
    CHECK_EQ_INT(fa->base.type, T3_SKINNED_MESH);
  }
  CHECK_EQ_INT(cw_morph_reset(a), 3); /* face, mouth, eyes */
  NEAR(cw_morph_get(a, "smile"), 0, 0);
  t3_release(a);
  t3_release(b);
  cw_res_release(r);
}

static void test_morph_tracks(void) {
  cw_res r = cw_res_load("character.glb", CW_RES_GLTF);
  const cw_gltf *g = cw_res_gltf(r);
  if (!g) { CHECK(!"character.glb"); return; }
  cw_scene_register_builtin_types();
  cw_tree *tree = cw_tree_new();
  cw_node root = cw_node_new("Node3D", "Root");
  cw_node model = cw_node_new("Node3D", "Model");
  cw_node anim = cw_node_new("AnimationPlayer", "Anim");
  cw_node_add_child(root, model);
  cw_node_add_child(root, anim);
  cw_node_add_child(cw_tree_root(tree), root);
  t3_object *inst = cw_gltf_instantiate(r);
  cw_node3d_set_object(model, inst);
  t3_release(inst);
  const t3_animation_clip *talk = NULL;
  for (int i = 0; i < g->gltf->animation_count; i++)
    if (!strcmp(g->gltf->animations[i]->name, "talk")) talk = g->gltf->animations[i];
  CHECK(talk != NULL);
  if (!talk) { cw_tree_free(tree); cw_res_release(r); return; }
  cw_anim a = cw_anim_from_t3_clip(talk, "Model");
  /* the weights channels: a float track per target, for the face and the mouth */
  int jaw = cw_anim_track_find(a, "Model@Face:morphTargetInfluences[1]");
  CHECK(jaw >= 0);
  CHECK(cw_anim_track_find(a, "Model@Mouth:morphTargetInfluences[2]") >= 0);
  /* a hand-made track names the target */
  cw_anim brow = cw_anim_new("brow", 1, CW_LOOP_NONE);
  int bt = cw_anim_add_value_track(brow, "Model@Face:morphTargetInfluences[browUp]", CW_INTERP_LINEAR);
  cw_anim_add_key(brow, bt, 0, cw_val_float(0));
  cw_anim_add_key(brow, bt, 1, cw_val_float(1));
  cw_anim_player_add(anim, "talk", a);
  cw_anim_player_add(anim, "brow", brow);
  cw_node_set(anim, "process", cw_val_int(CW_ANIM_PROCESS_MANUAL));
  cw_anim_player_play(anim, "talk", 0);
  cw_anim_player_advance(anim, 0.125);
  cw_value v;
  CHECK(cw_anim_sample(a, jaw, 0.125f, &v));
  t3_mesh *face = (t3_mesh *)t3_object_get_by_name(inst, "Face");
  CHECK(face != NULL);
  if (face) {
    CHECK(v.f > 0.1f);
    NEAR(face->morph_influences[1], (float)v.f, 0); /* the player writes what it samples */
    float w = face->morph_influences[1];
    FOLD(w);
  }
  cw_anim_player_play(anim, "brow", 0);
  cw_anim_player_advance(anim, 0.25);
  NEAR(cw_morph_get(inst, "browUp"), 0.25f, 1e-6f);
  cw_anim_release(a);
  cw_anim_release(brow);
  cw_tree_free(tree);
  cw_res_release(r);
}

CW_TEST_SUITE("character", { "materials", test_materials }, { "morph_instances", test_morph_instances },
              { "morph_tracks", test_morph_tracks })
