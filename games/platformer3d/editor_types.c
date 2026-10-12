/* platformer3d's node types for the editor: the editor app built with
 * them (target platformer3d_editor) opens, edits and saves the game's scenes
 * (LevelInfo, FollowCamera, Hero, Pickup, Hazard, Checkpoint, Goal).
 *
 * The editor never runs a scene (its documents are never processed), so the
 * game's runtime hooks that nodes.c calls are inert here; the game itself
 * links main.c instead of this file. */
#include "game.h"

game G;

void game_hurt(void) {}
void game_sfx(sfx_id id, const t3_vec3 *at) { (void)id, (void)at; }
void game_free_later(cw_node n, int ticks) { (void)n, (void)ticks; }

void platformer3d_editor_types(void);
void platformer3d_editor_types(void) { game_register_types(); }
