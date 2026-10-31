/* cartwheel/shell.h: the plumbing around a game's play, so a game writes only
 * its gameplay and art.
 *
 *   loading screen -> title (Play, Settings, Quit) -> join (1-4 players) ->
 *   play <-> pause (Resume, Settings, Quit to title)
 *
 * Settings: graphics (cartwheel/quality.h; left / right step the preset,
 * accept opens the graphics page of cartwheel/quality_menu.h with every
 * setting), music and effects volume, screen shake (Full, Reduced, Off: games
 * scale their camera shake by cw_shell_screen_shake) and the controls (each
 * action's bindings). They are saved, with the game's own save data, in one
 * versioned, checksummed block the shell keeps in cw_app_desc.save.
 *
 *   static cw_app_desc desc = { ... };
 *   desc.save = cw_shell_save_region();          // in cw_main
 *   desc.save_size = cw_shell_save_region_size();
 *
 *   static void init(void) {
 *     cw_shell_init(&(cw_shell_desc){ .title = "My Game", .max_players = 4,
 *                                     .game_save_size = sizeof(my_save), .start = start_match });
 *     my_save *s = cw_shell_game_save();         // zeroed when there was none
 *   }
 *   static void fixed_update(double dt) {
 *     if (!cw_shell_update()) return;            // menus own this tick
 *     ... gameplay for cw_shell_players() players ...
 *   }
 *   static void render(double alpha) {
 *     ... the game's frame ...
 *     cw_shell_draw();                           // menus and overlays on top
 *   }
 *
 * Input: menus read the actions ui_up, ui_down, ui_left, ui_right,
 * ui_accept, ui_back and pause, which the shell defines (keyboard arrows and
 * WASD, Enter / Space, Escape; pad d-pad and left stick, South, East,
 * Start), and pointer taps (mouse or touch) on menu items; Start on any pad
 * also accepts on the title. On the join screen each pad joins with South and
 * leaves with East, the keyboard joins with Enter and leaves with Esc. Start
 * on any pad plays: a pad with no seat takes the first free one first, and
 * with no seat free the seated players play. Enter plays from a keyboard that
 * holds any seat, and when every seat is taken. Players are assigned their pad
 * and devices (cw_input_assign) in join order, so gameplay asks cw_action_*
 * for players 0..n-1.
 *
 * A press to play is answered on its own frame (the select sound and
 * "Starting...", drawn by cw_shell_draw); the start hook runs on the next tick
 * after that frame, so a start that loads for a long time comes after a
 * picture. cw_shell_start skips this: it calls the hook at once.
 *
 * Split screen: cw_shell_split lays one view per player out with the
 * pipeline (cw_pipeline_split: two side by side, three and four in quadrants,
 * three leaving the fourth quadrant for a shared HUD); cw_shell_player_rect
 * and cw_shell_shared_rect give the HUD where to draw.
 *
 * Audio: the shell starts the mixer (cw_audio_init) unless the game did,
 * and then runs its per-frame update from cw_shell_draw. Two buses under the master, "music" and "sfx", follow the volume
 * settings; play the game's music and effects on them. The sound hook plays
 * the menus' sounds (move, select, back, join, pause), the screen hook starts
 * each screen's music.
 *
 * A scene app's tree (cw_scene_app) is paused outside play, so its nodes
 * hold still under the menus.
 *
 * Determinism: menus run in fixed_update from the input snapshot, so a
 * replay drives them like gameplay. A test or an autopilot build skips them
 * with cw_shell_start. Debug fields: shell.screen, shell.players, shell.item,
 * shell.shake, shell.starting (1 a press to play was answered, 2 "Starting..." has been drawn). */
#ifndef CW_SHELL_H
#define CW_SHELL_H

#include "base.h"
#include "audio.h"
#include "render2d.h"
#include "pipeline.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
  CW_SHELL_LOADING,  /* the game is loading (cw_shell_loading / cw_shell_loaded) */
  CW_SHELL_TITLE,
  CW_SHELL_JOIN,     /* players join (max_players > 1) */
  CW_SHELL_PLAY,     /* gameplay: cw_shell_update returns true */
  CW_SHELL_PAUSE,
  CW_SHELL_SETTINGS, /* from the title or the pause menu */
} cw_shell_screen;

typedef enum { CW_SHELL_SND_MOVE, CW_SHELL_SND_SELECT, CW_SHELL_SND_BACK, CW_SHELL_SND_JOIN, CW_SHELL_SND_PAUSE } cw_shell_sound;

typedef struct cw_shell_desc {
  const char *title;      /* the title screen's heading (default "cartwheel") */
  const char *subtitle;   /* a line under it (optional) */
  int max_players;        /* 1..4 (default 1); more than 1 adds the join screen */
  bool shared_screen;     /* every player plays in one shared view (one camera, a scrolling brawler or a co-op
                           * platformer): cw_shell_split draws a single view and cw_shell_player_rect gives each
                           * player the whole canvas */
  bool loading;           /* start on the loading screen until cw_shell_loaded */
  size_t game_save_size;  /* bytes of the game's own save data (cw_shell_game_save) */
  uint16_t game_save_version; /* change it when that layout changes: an older save starts over */
  void (*start)(int players, void *user);   /* Play: the players joined; set the match up */
  void (*quit)(void *user);                 /* Quit to title, from the pause menu */
  void (*sound)(cw_shell_sound s, void *user);
  void (*screen_changed)(cw_shell_screen from, cw_shell_screen to, void *user);
  /* Draws behind the title, join, settings and loading screens and the pause menu (in the shell's own 2D pass,
   * canvas units): the game's backdrop, so these screens are not a dark panel. The shell then dims it lightly
   * and draws the menu over it. NULL: the dark panel. */
  void (*draw_background)(cw_shell_screen screen, cw_vec2 canvas, void *user);
  /* The join screen was opened during play (cw_shell_join_open) and confirmed: the match now has this many
   * players (the highest seat held plus one; see cw_shell_seat_taken for which seats are held). The game adapts
   * its roster; it is not started again. Not called when the screen is cancelled. */
  void (*players_changed)(int players, void *user);
  cw_font font;           /* default: the built-in sans (cw_ui_font_sans) */
  cw_color accent;        /* the selected item's colour (default a warm yellow) */
  void *user;
} cw_shell_desc;

/* The save region for cw_app_desc.save / save_size: the shell's settings and
 * the game's save data. Static, so cw_main can name it before init. */
void *cw_shell_save_region(void);
size_t cw_shell_save_region_size(void); /* its size in bytes */
#define CW_SHELL_GAME_SAVE_MAX 6144

/* Call once in init (the save is loaded by then): checks the save, applies
 * the saved settings, defines the ui actions and the audio buses. */
void cw_shell_init(const cw_shell_desc *d);
/* The game's save data: game_save_size bytes, zeroed when the save was
 * missing or from another version. Write it, then cw_shell_save_commit. */
void *cw_shell_game_save(void);
void cw_shell_save_commit(void); /* write the settings and the game's save data now */

/* fixed_update, first: runs the menus; true when gameplay runs this tick. */
bool cw_shell_update(void);
/* render, last: draws the current screen's menu or overlay (its own 2D pass). */
void cw_shell_draw(void);

cw_shell_screen cw_shell_screen_now(void); /* the screen showing */
int cw_shell_players(void); /* players in the match (1..max_players) */
/* Start a match now with n players (pads 0..n-1, player 0 also on the
 * keyboard, mouse and touch): an autopilot build or a test skips the menus. */
void cw_shell_start(int players);
/* Join and leave during play: opens the join screen over the running match (play pauses, exactly as for the pause
 * menu) with the current players on their seats. A pad presses A to take a free seat, B to leave its own seat;
 * Start or Enter confirms, a pad with no seat taking a free one as it presses Start (at least one player stays) and play resumes with players_changed; B on a pad that
 * has no seat cancels and leaves everything as it was. Seats keep their numbers: a leaver's seat reads no
 * input (cw_shell_seat_taken says which are held), a newcomer takes the first free seat, and the match's
 * player count is the highest seat held plus one. Returns false (and does nothing) outside play or in a one
 * player game. */
bool cw_shell_join_open(void);
/* Is this player seat held by a pad or the keyboard (0 .. cw_shell_players() - 1)? Right after a start every
 * seat below the player count is; after cw_shell_join_open a seat can be empty. */
bool cw_shell_seat_taken(int seat);
/* True on the tick a pad or the keyboard that has no seat presses A (Enter) or Start: the cue for a game to
 * offer cw_shell_join_open, say between rounds. Poll it in fixed_update after cw_shell_update. */
bool cw_shell_join_requested(void);
/* One-press join during play: if a pad that has no seat pressed A this tick, it takes
 * the first free seat at once, with no join screen, players_changed tells the game, and this returns true (the
 * seat number, like every seat, never changes while the match runs). False outside play, in a one player game,
 * when no seat is free, or when nobody without a seat pressed A. Call it in fixed_update after cw_shell_update;
 * cw_shell_join_open stays the way to leave a seat or rearrange. Pads only: the keyboard already drives a player. */
bool cw_shell_join_press(void);
void cw_shell_to_title(void); /* end the match and show the title (every pad drives the menus again) */
/* Loading: progress 0..1 shown on the loading screen; loaded goes on to the title. */
void cw_shell_loading(float progress);
void cw_shell_loaded(void); /* loading done: on to the title */

/* What Reduced screen shake scales a game's shake by (cw_shell_screen_shake). */
#define CW_SHELL_SHAKE_REDUCED_SCALE 0.35f
/* The player's Screen shake setting (Settings > Screen shake), saved with the other settings; Full by default and
 * for a save from before the setting. */
typedef enum { CW_SHELL_SHAKE_FULL, CW_SHELL_SHAKE_REDUCED, CW_SHELL_SHAKE_OFF } cw_shell_shake;
/* The factor a game multiplies every camera shake, kick and bob by: 1 for Full, CW_SHELL_SHAKE_REDUCED_SCALE for
 * Reduced, 0 for Off. Reduced motion (cw_ui_set_reduced_motion) caps it at Reduced. */
float cw_shell_screen_shake(void);
/* The Screen shake level the player chose. */
cw_shell_shake cw_shell_shake_level(void);
/* Set the Screen shake level as the settings menu does: it applies at once and is saved. */
void cw_shell_set_shake_level(cw_shell_shake level);

cw_bus cw_shell_music_bus(void); /* play music here: it follows the Music volume */
cw_bus cw_shell_sfx_bus(void);   /* play effects here: they follow the Effects volume */

/* One view per player (cameras[0..players-1]) when there is more than one
 * player; a single player's camera fills the output. p NULL: a scene app's
 * pipeline (cw_scene_app). Returns the views set. */
int cw_shell_split(cw_pipeline *p, const cw_node *cameras);
/* A player's part of the canvas (2D pass units): the whole canvas for one player. */
cw_rect cw_shell_player_rect(int player);
/* Three players: the fourth quadrant, for a shared HUD (a map, the scores);
 * otherwise an empty rect. */
cw_rect cw_shell_shared_rect(void);

#ifdef __cplusplus
}
#endif
#endif
