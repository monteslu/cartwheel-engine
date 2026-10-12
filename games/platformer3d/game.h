/* platformer3d: shared game state between the node types (nodes.c) and the
 * app (main.c). Everything here that feeds the simulation is advanced only
 * in fixed_update, from the tick action snapshot (cartwheel/input.h). */
#ifndef PLATFORMER3D_GAME_H
#define PLATFORMER3D_GAME_H

#include "cartwheel/anim.h"
#include "cartwheel/audio.h"
#include "cartwheel/input.h"
#include "cartwheel/physics_nodes.h"
#include "cartwheel/scene_file.h"

typedef enum {
  GS_TITLE,    /* title screen, waiting for start */
  GS_PLAYING,  /* a level runs */
  GS_DYING,    /* the hero was hurt: respawn after a beat */
  GS_CLEAR,    /* the goal was reached: banner, then the next scene */
  GS_GAMEOVER, /* no lives left: back to the title */
  GS_END,      /* the end screen */
} game_state;

typedef enum { SFX_JUMP, SFX_DOUBLE_JUMP, SFX_LAND, SFX_DASH, SFX_COIN, SFX_GEM, SFX_HURT, SFX_CHECKPOINT, SFX_GOAL, SFX_BUMP, SFX_COUNT } sfx_id;

/* LevelInfo: the root node type of every scene the game loads */
typedef enum { LEVEL_TITLE, LEVEL_PLAY, LEVEL_END } level_kind;
typedef struct level_info {
  const char *title;      /* shown in the banner and the HUD */
  int kind;               /* level_kind */
  const char *next_scene; /* loaded after the goal / from the title */
  const char *music;      /* streamed music asset */
  float kill_y;           /* falling below this costs a life */
  int index;              /* 1-based level number in the HUD */
} level_info;

typedef struct game {
  /* debug fields (exposed by name in main.c) */
  int32_t state, level, score, coins, gems, lives, deaths, checkpoint, timer, level_ticks;
  int32_t jumps, double_jumps, dashes, anim_state, floor, pickups_left, ticks;
  float player_pos[3], player_vel[3], cam_pos[3];
  uint32_t state_hash, props_hash, phys_hash;
  /* flow */
  int32_t state_ticks;     /* ticks in the current state */
  int32_t banner_ticks;    /* level-name banner countdown */
  int32_t toast_ticks;     /* "checkpoint" toast countdown */
  bool cam_snap;           /* the camera jumps to its target this tick */
  const char *pending_scene;
  t3_vec3 respawn;
  /* the loaded scene */
  cw_node scene, hero, camera;
  level_info *info;
  float cam_yaw;
  /* input actions */
  cw_action a_left, a_right, a_forward, a_back, a_jump, a_dash, a_start;
} game;
extern game G;

/* nodes.c */
void game_register_types(void);
cw_signal_fn game_resolve_method(cw_node target, const char *method, void **user);
void hero_respawn(cw_node hero, t3_vec3 at);
void hero_set_cheer(cw_node hero, bool on);

/* main.c */
void game_hurt(void);
void game_sfx(sfx_id id, const t3_vec3 *at); /* at NULL = 2D */
void game_free_later(cw_node n, int ticks);   /* free a node after a tween played out */

#endif
