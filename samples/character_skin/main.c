/* character_skin: characters that look like people, not plastic.
 *
 * One generated glTF (make-character.mjs: a head with ears, eyes, hair, a
 * knit top, a leather belt) is loaded once through the resource system and
 * instanced CHARACTER_COUNT times (default 6). Every instance shares the
 * file's geometry, materials and textures and has its own skeleton, its own
 * AnimationPlayer and its own morph target weights:
 *
 *   - skin shading (cartwheel/skin_shading.h): the file's "Skin" material
 *     asks for it (extras skin_shading), so the face, ears and hands get the
 *     soft warm terminator and the back light glowing through the ears;
 *   - glTF materials: names kept; KHR_materials_sheen (the knit, the hair),
 *     clearcoat (the eyes, the belt), specular and ior read from the file;
 *   - morph targets on skinned meshes: the "talk" clip opens the jaw through
 *     the glTF's weights channel; code drives smile, browUp and blink by
 *     name (cartwheel/morph.h), each character on its own phase.
 *
 * Environment:
 *   CHARACTER_COUNT=n        characters (1 to 64)
 *   CHARACTER_CLOSEUP=1      the camera on the first character's face
 *   CHARACTER_QUALITY=low|medium|high|ultra  the quality preset (Low draws skin as plain PBR)
 *   CHARACTER_NO_SKIN=1      the skin shading removed (the gate's control: state and picture differ)
 *   CHARACTER_NO_MORPH=1     no weights driven (all faces at rest)
 * Debug fields: skin.* (materials shaded, enabled), mat.* (what the loader
 * read from the file's extensions), morph.* (weights of the first character
 * and how many meshes the last cw_morph_set reached), chars, instances. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cartwheel/anim.h"
#include "cartwheel/debug.h"
#include "cartwheel/morph.h"
#include "cartwheel/quality.h"
#include "cartwheel/resource.h"
#include "cartwheel/scene_file.h"
#include "cartwheel/skin_shading.h"

#define MAX_CHARS 64

static cw_res model;
static int count = 6;
static bool no_morph;
static cw_node chars[MAX_CHARS];
static t3_object *inst[MAX_CHARS];
static int32_t skin_materials, skin_enabled, instances, char_count, morph_meshes, material_names, morph_names;
static float mat_vals[8], weights[4];
static uint64_t ticks;

static float env_f(const char *k, float def) {
  const char *v = getenv(k);
  return v && *v ? (float)atof(v) : def;
}

/* what the loader read from the file, through material names */
typedef struct { const char *name; t3_material *m; } find_ctx;
static void find_mat(t3_object *o, void *u) {
  find_ctx *c = u;
  if (c->m || (o->type != T3_MESH && o->type != T3_SKINNED_MESH)) return;
  t3_mesh *m = (t3_mesh *)o;
  for (int i = 0; i < m->material_count; i++)
    if (m->materials[i]->name && !strcmp(m->materials[i]->name, c->name)) c->m = m->materials[i];
}
static t3_material *material(t3_object *root, const char *name) {
  find_ctx c = { name, NULL };
  t3_object_traverse(root, find_mat, &c);
  return c.m;
}
static void shadows_on(t3_object *o, void *u) {
  (void)u;
  o->cast_shadow = o->receive_shadow = true;
}
static void count_named(t3_object *o, void *u) {
  if (o->type != T3_MESH && o->type != T3_SKINNED_MESH) return;
  t3_mesh *m = (t3_mesh *)o;
  for (int i = 0; i < m->material_count; i++) *(int32_t *)u += m->materials[i]->name != NULL;
}

static void init(void *u) {
  (void)u;
  count = (int)env_f("CHARACTER_COUNT", 6);
  if (count < 1) count = 1;
  if (count > MAX_CHARS) count = MAX_CHARS;
  no_morph = env_f("CHARACTER_NO_MORPH", 0) != 0;
  const char *q = getenv("CHARACTER_QUALITY");
  if (q && *q) {
    cw_quality p = !strcmp(q, "low") ? CW_QUALITY_LOW : !strcmp(q, "medium") ? CW_QUALITY_MEDIUM : !strcmp(q, "ultra") ? CW_QUALITY_ULTRA : CW_QUALITY_HIGH;
    cw_quality_set(p, false);
  }
  cw_node level = cw_node_get(cw_tree_root(cw_scene_app_tree()), "Level");
  cw_node cast = cw_node_get(level, "Cast");
  /* one parse: every character below is an instance of this resource */
  model = cw_res_load("character.glb", CW_RES_GLTF);
  const cw_gltf *g = cw_res_gltf(model);
  if (!g) { cw_error("character_skin: character.glb did not load"); return; }
  int cols = count < 4 ? count : count <= 6 ? 3 : count <= 12 ? 4 : 8;
  for (int i = 0; i < count; i++) {
    int row = i / cols, col = i % cols;
    int in_row = count - row * cols < cols ? count - row * cols : cols;
    char nm[32];
    snprintf(nm, sizeof nm, "Char%d", i);
    cw_node n = cw_node_new("Node3D", nm);
    cw_node_add_child(cast, n);
    cw_node3d_set_position(n, (t3_vec3){ ((float)col - (float)(in_row - 1) * 0.5f) * 0.85f, 0, -1.1f * (float)row });
    cw_node3d_set_rotation(n, (t3_vec3){ 0, -0.25f * ((float)col - (float)(in_row - 1) * 0.5f), 0 });
    t3_object *o = cw_gltf_instantiate(model);
    t3_object_traverse(o, shadows_on, NULL); /* casts on the floor and on itself (hair on the face) */
    cw_node3d_set_object(n, o);
    t3_release(o); /* the node holds it */
    inst[i] = o;
    chars[i] = n;
    /* its own AnimationPlayer: the glTF clips sampled by cartwheel, morph weights included */
    cw_node anim = cw_node_new("AnimationPlayer", "Anim");
    cw_node_add_child(n, anim);
    for (int k = 0; k < g->gltf->animation_count; k++) {
      cw_anim a = cw_anim_from_t3_clip(g->gltf->animations[k], "");
      cw_anim_player_add(anim, g->gltf->animations[k]->name, a);
      cw_anim_release(a);
    }
    cw_anim_player_play_ex(anim, "talk", 0, 0.8f + 0.07f * (float)(i % 5), false);
    cw_anim_player_seek(anim, 0.31f * (float)i, true);
  }
  if (env_f("CHARACTER_NO_SKIN", 0) != 0) {
    t3_material *s = material(inst[0], "Skin");
    if (s) cw_skin_shading_remove(s);
  }
  if (env_f("CHARACTER_CLOSEUP", 0) != 0) {
    cw_node cam = cw_node_get(level, "Camera");
    t3_vec3 p = cw_node3d_position(chars[0]);
    /* three quarters: the key light's terminator across the cheek, the rim light behind the ear */
    cw_node3d_set_position(cam, (t3_vec3){ p.x + 0.42f, 1.71f, p.z + 0.5f });
    cw_node3d_set_rotation(cam, (t3_vec3){ -0.04f, 0.68f, 0 });
  }
  /* what the loader read from the extensions (names kept, KHR_materials_*) */
  t3_material *knit = material(inst[0], "Knit"), *eye = material(inst[0], "Eye"), *leather = material(inst[0], "Leather"),
              *skin = material(inst[0], "Skin");
  mat_vals[0] = knit ? knit->sheen : -1;
  mat_vals[1] = knit ? knit->sheen_color.b : -1;
  mat_vals[2] = eye ? eye->clearcoat : -1;
  mat_vals[3] = leather ? leather->clearcoat : -1;
  mat_vals[4] = leather ? leather->ior : -1;
  mat_vals[5] = skin ? skin->specular_intensity : -1;
  mat_vals[6] = knit ? (float)knit->type : -1;
  material_names = 0;
  t3_object_traverse(inst[0], count_named, &material_names);
  morph_names = cw_morph_names(inst[0], NULL, 0);

  cw_debug_i32("skin.materials", &skin_materials);
  cw_debug_i32("skin.enabled", &skin_enabled);
  cw_debug_i32("chars", &char_count);
  cw_debug_i32("instances", &instances);
  cw_debug_i32("mat.named", &material_names);
  cw_debug_f32_array("mat.vals", mat_vals, 7);
  cw_debug_i32("morph.names", &morph_names);
  cw_debug_i32("morph.meshes", &morph_meshes);
  cw_debug_f32_array("morph.weights", weights, 4);
}

static void fixed(void *u, double dt) {
  (void)u;
  (void)dt;
  ticks++;
  if (!cw_res_gltf(model)) return;
  float t = (float)ticks / 60.0f;
  if (!no_morph) {
    morph_meshes = 0;
    for (int i = 0; i < count; i++) {
      float ph = t * 0.9f + 0.7f * (float)i;
      morph_meshes += cw_morph_set(inst[i], "smile", 0.5f + 0.5f * cw_sin(ph));
      cw_morph_set(inst[i], "browUp", 0.5f + 0.5f * cw_sin(ph * 1.7f + 1));
      /* a blink every ~3 s, 0.15 s long */
      float c = t + 0.37f * (float)i;
      c -= 3.1f * (float)(int)(c / 3.1f);
      cw_morph_set(inst[i], "blink", c < 0.15f ? cw_sin(c / 0.15f * CW_PI) : 0);
    }
  } else {
    for (int i = 0; i < count; i++) cw_morph_reset(inst[i]);
  }
  weights[0] = cw_morph_get(inst[0], "smile");
  weights[1] = cw_morph_get(inst[0], "jawOpen");
  weights[2] = cw_morph_get(inst[0], "browUp");
  weights[3] = cw_morph_get(inst[0], "blink");
  skin_materials = cw_skin_shading_count();
  skin_enabled = cw_skin_shading_enabled();
  char_count = count;
  instances = cw_gltf_instance_count(model);
}

static void shutdown(void *u) {
  (void)u;
  cw_res_release(model);
}

cw_app_desc cw_main(int argc, char **argv) {
  (void)argc;
  (void)argv;
  static cw_app_desc base = { .title = "character_skin", .width = 960, .height = 540, .debug = true, .init = init,
                              .fixed_update = fixed, .shutdown = shutdown };
  return cw_scene_app("main.cwscene", &base);
}
