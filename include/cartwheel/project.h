/* Project settings for games: project.cwproj, read through the project layer
 * (cartwheel/vfs.h), so a game sees the same file natively (its project
 * directory or packed assets) and in a cart (its bundled assets, or the
 * in-memory layer an editor or agent wrote).
 *
 * The file is `key = value` lines ('#' or ';' start a comment line; values
 * may be quoted). Keys the engine and editor use:
 *
 *   name = My Game                      the project's display name
 *   app = my_game                       the project's cw_add_app target
 *   main_scene = main.cwscene
 *   layer_names_2d / layer_names_3d / gravity_2d / gravity_3d /
 *   pixels_per_meter / substeps         physics (cartwheel/physics_nodes.h)
 *   input.<action> = key:space, pad:south   the input map: one action per key,
 *                                       the cw_input_load_bindings line format
 *   render.width / render.height        logical resolution (a cart's declared size)
 *   render.msaa = 4                     default framebuffer samples (native)
 *   render.vsync = true                 native only
 *   render.fixed_hz = 60                simulation ticks per second
 *   export.<preset>.platform / .output / ...   export presets (tools/cwexport)
 *   net.domains = play.example.com      hosts the cart may dial (wc_peer_open's
 *                                       manifest grant; read by cw_add_app at build)
 * Editor and tool keys (the engine itself never reads them):
 *   scenes = levels/a.cwscene, ...     scenes the editor lists besides main_scene
 *   sources = src, game.c               C files and directories (the editor's
 *                                       code view and agent tools)
 *   build / game_module                 the editor's build command and hot module
 *   game_exe / game_cart                written by tools/cwproject.mjs after a
 *                                       build: what the editor's Play launches
 *
 * Every function here may be called from cw_main (before init): the file is
 * read on first use and cached. */
#ifndef CW_PROJECT_H
#define CW_PROJECT_H

#include "platform.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The value text of a key (quotes stripped, surrounding spaces trimmed);
 * NULL when the file or the key is missing. Valid until cw_project_reload. */
const char *cw_project_get(const char *key);
/* Typed reads with a fallback. */
int cw_project_get_int(const char *key, int def);
bool cw_project_get_bool(const char *key, bool def); /* true/false/1/0/on/off */
/* Number of keys, and key i (file order). */
int cw_project_count(void);
const char *cw_project_key(int i); /* interned; NULL when i is out of range */
/* Re-read project.cwproj (after the editor or an agent changed it). */
void cw_project_reload(void);
/* Editing (the editor, tools): set a key to a value (written as is: quote
 * it yourself when it needs quotes), or remove the key (value NULL), in
 * project.cwproj through the project layer (natively the project
 * directory, in a cart the in-memory layer), keeping every other line
 * (comments, order) as it was; a new key is appended. The cache is
 * reloaded. false when the key or value is invalid (empty, '=' in the key,
 * a newline) or the write failed. */
bool cw_project_set(const char *key, const char *value);
/* The same edit on text (any project.cwproj, e.g. one in another
 * directory): a malloc'd new text, or NULL for an invalid key/value. */
char *cw_project_text_set(const char *text, const char *key, const char *value);

/* Load every `input.<action>` line into the input module (creating the
 * actions) and commit them as the defaults. Returns the number of actions
 * loaded; bad lines are logged with their key. */
int cw_project_load_input(void);

/* Apply the render.* settings and the name (as the title when the desc has
 * none) to an app description; call from cw_main. Unset keys keep the
 * desc's values. */
void cw_project_apply_app(cw_app_desc *desc);

#ifdef __cplusplus
}
#endif
#endif
