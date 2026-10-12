/* action2d ("Crypt of Embers"): shared game state between the app (main.c:
 * states, menus, HUD, audio, rooms) and the node types that act in the world
 * (actors.c: the hero, enemies, projectiles, pickups, doors, tile walls). */
#ifndef ACTION2D_GAME_H
#define ACTION2D_GAME_H

#include "cartwheel/anim.h"
#include "cartwheel/audio.h"
#include "cartwheel/debug.h"
#include "cartwheel/input.h"
#include "cartwheel/physics_nodes.h"
#include "cartwheel/platform.h"
#include "cartwheel/scene2d.h"
#include "cartwheel/scene_file.h"
#include "cartwheel/ui.h"

typedef enum { ST_TITLE, ST_OPTIONS, ST_PLAY, ST_PAUSE, ST_TRANSITION, ST_GAMEOVER, ST_VICTORY } game_state;
typedef enum { SFX_SWING, SFX_HIT, SFX_HURT, SFX_PICKUP, SFX_GEM, SFX_DIE, SFX_SHOOT, SFX_DOOR, SFX_MOVE, SFX_SELECT,
               SFX_ROAR, SFX_BOOM, SFX_VICTORY, SFX_GAMEOVER, SFX_COUNT } sfx_id;

#define HERO_MAX_HP 6
#define ROOM_COUNT 3

typedef struct game {
  int32_t state, options_from;
  int32_t score, hp, room, enemies, kills, hits_taken, pickups, attacks, rebinds, boss_hp, pauses, deaths, doors;
  int32_t play_ticks, victory_ticks, gameover_ticks, boss_dead;
  float player[2];
  uint32_t hash;
  int32_t music_vol, sfx_vol;
  int32_t focus_moves, menu_presses;
  /* actions */
  cw_action a_left, a_right, a_up, a_down, a_attack, a_pause;
  /* nodes */
  cw_node root, world, room_node, hero;
} game;

extern game G;

/* main.c services for the actors */
void game_sfx(sfx_id id);
void game_shake(bool big);
void game_flash(const char *anim); /* "flash" | "hurt" on the screen overlay */
void game_score(int add);
void game_hero_hurt(int dmg);
void game_hero_heal(int amount);
void game_room_exit(int next_room);
void game_boss_defeated(void);
/* a fresh instance of an actor scene placed in the current room at pos */
cw_node game_spawn(const char *scene, cw_vec2 pos);

/* actors.c */
void actors_register(void);
cw_signal_fn actors_resolve(const char *method);
bool actors_wall_at(cw_vec2 world_pos);     /* the current room's solid tiles */
int actors_enemies_alive(void);
uint32_t actors_hash(uint32_t h);           /* fold every enemy's position and hp */
void actors_open_doors(void);
/* for the native authoring bot (main.c) */
bool actors_cell_solid(int x, int y);
void actors_room_size(int *w, int *h);
bool actors_door(cw_vec2 *pos);              /* the room's door position; true when open */
int actors_hero_cooldown(void);              /* ticks until the hero can swing again */
int actors_enemy_state(cw_node n, int *timer);
bool actors_enemy_alive(cw_node n, float *radius);
void bot_install(const char *out_path);      /* bot.c: native only */

#endif
