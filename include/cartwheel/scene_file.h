/* Scene files (.cwscene): a node subtree as text that diffs and merges in git.
 *
 *   [scene format=1]
 *   [ext_scene id=1 path="player/player.cwscene"]
 *
 *   [node name="Level" type="Node3D"]
 *   position = vec3(0, 0, 0)
 *
 *   [node name="Player" parent="." instance=1 groups="players"]
 *   position = vec3(0, 1.5, 4)
 *
 *   [node name="Sun" parent="." type="DirectionalLight3D"]
 *   energy = 3
 *
 *   [node name="Mesh" parent="Player"]      <- no type, no instance: an OVERRIDE
 *   color = color(1, 0, 0, 1)               of the instanced scene's node
 *
 *   [connection signal="body_entered" from="Goal" to="." method="on_goal"]
 *
 * Rules: one property per line (`name = value`, values as cw_value_format
 * writes them); parent paths are relative to the scene root ("." = root);
 * only properties that differ from the type's defaults (or, inside an
 * instance, from the instanced scene) are written; nodes in tree order.
 * Node-valued properties are node("path") from the scene root ("." = the
 * root), resolved after the whole instance is built.
 * Lines starting with `;` or `#` are comments: the loader skips them and
 * cw_scene_save does not keep them (it writes the tree, not the old text),
 * so an editor save drops them. Saving the result of a load reproduces a
 * file in that canonical form byte for byte. Docs listings select scene
 * sections by node (docs/AUTHORING.md), never by comment markers. */
#ifndef CW_SCENE_FILE_H
#define CW_SCENE_FILE_H

#include "platform.h"
#include "scene.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct cw_packed_scene cw_packed_scene;

/* Parse scene text. path names the file for errors and resolves ext_scene
 * paths (relative to the asset root). NULL on a parse error (logged with
 * the line number). Packed scenes are cached by path. */
cw_packed_scene *cw_scene_parse(const char *text, const char *path);
/* Load + parse a scene (cached by path). Reads through the project layer
 * (cartwheel/vfs.h: written-but-unexported files and a native project
 * directory first), then the app's assets. */
cw_packed_scene *cw_scene_load(const char *asset_path);
/* Drop a path from the load cache (NULL = every path), so the next load
 * reads the file again: call it after rewriting a scene (an editor saving a
 * sub-scene). Packed scenes already loaded stay valid for the nodes made
 * from them. */
void cw_scene_cache_forget(const char *asset_path);
/* Build a new node subtree from a packed scene (not in a tree yet). */
cw_node cw_scene_instance(cw_packed_scene *ps);
/* cw_scene_load + cw_scene_instance; null handle (logged) when the scene is missing */
cw_node cw_scene_instance_path(const char *asset_path);
/* Instance a scene AS AN INSTANCE: the new root remembers its source, so
 * saving the tree it is added to writes `instance=` plus only the
 * properties that differ (what an editor's "instance a scene" does);
 * cw_scene_instance_path makes a plain copy whose nodes save in full. */
cw_node cw_scene_instance_linked(const char *asset_path);
/* The scene path n was instanced from (n is the root of a linked or
 * scene-file instance), else NULL. */
const char *cw_scene_instance_source(cw_node n);
/* Free a packed scene from cw_scene_parse once nothing will instance it
 * again (nodes already made from it do not need it). Cached scenes
 * (cw_scene_load) are refused. */
void cw_packed_scene_free(cw_packed_scene *ps);
/* Write a subtree as scene text; returns a cw_alloc'd string. */
char *cw_scene_save(cw_node root);
/* interned asset path, "(memory)" for unnamed text; ps must not be NULL */
const char *cw_packed_scene_path(cw_packed_scene *ps);

/* ── extending the format ─────────────────────────────────────────────
 * Modules store data that is not node properties (animations, ...) in
 * sections of their own. A section starts at its `[name ...]` header and
 * owns every following line, and the sub-headers it declares (`[track ...]`),
 * until the next section header. Extension sections are written after the
 * nodes and before the connections, extensions in registration order. */
typedef struct cw_scene_section {
  const char *name;               /* "animation" */
  const char *const *subsections; /* NULL-terminated sub-header names it owns while open */
  /* a new section's header line; returns its parsed state, NULL on error (logged) */
  void *(*begin)(const char *header, const char *path, int lineno);
  /* a body line or an owned sub-header (leading whitespace trimmed); false on error */
  bool (*line)(void *state, const char *line, const char *path, int lineno);
  /* the scene was instanced: root is the new subtree (not in a tree yet) */
  void (*instance)(void *state, cw_node root, const char *path);
  void (*free)(void *state);
  /* append this extension's sections for the subtree under root */
  void (*save)(cw_node root, void (*put)(void *ctx, const char *text), void *ctx);
} cw_scene_section;
/* The descriptor must outlive the program (static). Idempotent per name. */
void cw_scene_register_section(const cw_scene_section *s);
/* `key=value` or `key="value"` of a section header (interned; NULL when absent) */
const char *cw_scene_attr(const char *header, const char *key);
/* true when n (or an ancestor below root) is the root of an instanced
 * sub-scene: its data belongs to that scene's file */
bool cw_scene_node_in_instance(cw_node n, cw_node root);

/* ── the scene runner ──────────────────────────────────────────────────
 * An app that runs a tree: instance `main_scene` under the root, fixed
 * ticks / updates / pre_render through the tree, then draw its 3D scene
 * (and later 2D layers and UI). `base` may supply title/size/debug/workers
 * and extra callbacks (called after the tree's own): base.render runs after
 * the 3D draw, so a HUD drawn there with render2d lands on top (the runner
 * resets three.c's GL state cache afterwards). main_scene may be NULL: build
 * the tree in base.init through cw_scene_app_tree(). Physics worlds made with
 * physics3d.h directly are stepped by the app, in base.fixed_update. */
cw_app_desc cw_scene_app(const char *main_scene, const cw_app_desc *base);
cw_tree *cw_scene_app_tree(void); /* the runner's tree; NULL before the app starts */
/* Replace the main scene (or, with none, everything under the root) with a
 * new instance of path. The project layer's copy (cartwheel/vfs.h) wins over
 * the asset; reread parses the text again instead of using the cache (the
 * debug channel's reload_scene). false (logged) when it does not load. */
bool cw_scene_app_change_scene(const char *path, bool reread);
const char *cw_scene_app_current_scene(void); /* NULL when the app has none */

#ifdef __cplusplus
}
#endif
#endif
