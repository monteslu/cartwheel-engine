/* cartwheel/shell.h: the plumbing around a game's play, so a game writes only
 * its gameplay and art.
 *
 *   loading screen -> title (Play, Settings, Quit) -> join (1-4 players) ->
 *   play <-> pause (Resume, the game's own items if any, Settings, Quit to title)
 *
 * Settings: graphics (cartwheel/quality.h; left / right step the preset,
 * accept opens the graphics page of cartwheel/quality_menu.h with every
 * setting), music and effects volume, text size and background, and the
 * controls (each action's bindings), then the game's own rows (cw_shell_desc.settings_items), then Back. There is no screen shake setting: no camera
 * shakes (a hit or a blast is felt in the pads, cw_input_rumble). They are saved, with the game's own save data, in one
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
 *     if (cw_shell_hud_shown()) draw_hud();      // not under the menus
 *     cw_shell_draw();                           // menus and overlays on top
 *   }
 *
 * Input: menus read the actions ui_up, ui_down, ui_left, ui_right,
 * ui_accept, ui_back and pause, which the shell defines (keyboard arrows and
 * WASD, Enter / Space, Escape; pad d-pad and left stick, South, East,
 * Start), and pointer taps on menu items: a mouse click or any finger (pointer slots 1-9, with or without
 * cw_app_desc.touch_pointer); a tap on the join screen seats the keyboard's player (who holds the mouse and touch)
 * and plays. Start on any pad also accepts on the title. On the join screen each pad joins with South and
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
 * with cw_shell_start. Debug fields: shell.screen, shell.hud (cw_shell_hud_shown), shell.players, shell.item,
 * shell.shake, shell.settings (the game's Settings rows shown), shell.starting (1 a press to play was answered, 2 "Starting..." has been drawn). */
#ifndef CW_SHELL_H
#define CW_SHELL_H

#include "base.h"
#include "audio.h"
#include "render2d.h"
#include "pipeline.h"
#include "save.h"

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

/* A row of the game's own on the Settings screen: a named choice, such as "Visual style: All / Neon / Paper". It draws
 * after the shell's rows (after Controls, before Back) like them: the label, then the chosen choice. Left / right
 * step the choice and stop at the ends, accept (South, Enter, a tap or a click on the row) steps it forward and goes
 * round, as on the shell's Music, Text size and Text background rows. */
typedef struct cw_shell_setting {
  const char *label;           /* the row's name */
  /* The name the choice is saved under, in the shell's save block (see cw_shell_desc.settings_items): 1 ..
   * CW_SHELL_SETTING_KEY_MAX bytes, unique among the game's rows. NULL: the row is not saved. */
  const char *key;
  int *value;                  /* the game's own int: the chosen index into choices. Its value at cw_shell_init is the default */
  const char *const *choices;  /* count labels, what the row shows for each value */
  int count;                   /* how many choices (at least 1) */
} cw_shell_setting;
#define CW_SHELL_SETTINGS_MAX 8     /* rows a game can add */
#define CW_SHELL_SETTING_KEY_MAX 56 /* bytes in a row's key */
/* Bytes at the end of the game's save area (CW_SHELL_GAME_SAVE_MAX) that hold the game's rows when the game keeps its
 * save as a struct (game_save_size) and has a saved row: that struct can be at most CW_SHELL_GAME_SAVE_MAX minus this. */
#define CW_SHELL_SETTINGS_SAVE_BYTES 1024

typedef enum { CW_SHELL_SND_MOVE, CW_SHELL_SND_SELECT, CW_SHELL_SND_BACK, CW_SHELL_SND_JOIN, CW_SHELL_SND_PAUSE } cw_shell_sound;

/* How the menus show the selected row: the title, pause and Settings lists, the Graphics page, and the join screen's
 * taken seats. A game whose picture has only two colours (a one-bit look) shows the selection by shape: MARKER,
 * UNDERLINE or INVERSE with an accent of its light colour. */
typedef enum {
  CW_SHELL_SELECT_COLOUR,    /* the row's text in the accent (the default; as before the styles) */
  CW_SHELL_SELECT_MARKER,    /* the accent text and a caret in the accent before the row */
  CW_SHELL_SELECT_UNDERLINE, /* the accent text and a bar in the accent under it */
  CW_SHELL_SELECT_INVERSE,   /* the row on a plate of the accent, its text in selection_text */
  CW_SHELL_SELECT_COUNT
} cw_shell_selection;

typedef struct cw_shell_desc {
  const char *title;      /* the title screen's heading (default "cartwheel") */
  const char *subtitle;   /* a line under it (optional) */
  int max_players;        /* 1..4 (default 1); more than 1 adds the join screen */
  bool shared_screen;     /* every player plays in one shared view (one camera, a scrolling brawler or a co-op
                           * platformer): cw_shell_split draws a single view and cw_shell_player_rect gives each
                           * player the whole canvas */
  bool loading;           /* start on the loading screen until cw_shell_loaded */
  size_t game_save_size;  /* bytes of the game's own save data (cw_shell_game_save). A save from a build where it
                           * was another size (same version) keeps the bytes both have, zeros after them: a
                           * struct that only grows at its end loses nothing. */
  uint16_t game_save_version; /* change it when the layout changes in a way that cannot follow (a field removed or
                               * moved): game_save_migrate then converts an older save, or it starts over */
  /* Convert the game's save data from an older game_save_version: old/old_size are the stored bytes, out (zeroed,
   * out_size = game_save_size) the data to fill. Return false to start over. NULL: an older version starts over.
   * The settings (volumes, graphics, text size) are kept either way. */
  bool (*game_save_migrate)(uint16_t old_version, const void *old, size_t old_size, void *out, size_t out_size, void *user);
  /* The game keeps its save data as named, versioned slots (cartwheel/save.h) instead of one struct: use
   * cw_shell_save_store, and leave game_save_size / game_save_version / game_save_migrate unset. The first run
   * over a save from before the game used slots calls game_save_import with the old struct (its version and
   * bytes) and the empty store to write slots into. */
  bool game_save_slots;
  void (*game_save_import)(cw_save_store *store, uint16_t old_version, const void *old, size_t old_size, void *user);
  void (*start)(int players, void *user);   /* Play: the players joined; set the match up */
  void (*quit)(void *user);                 /* Quit to title, from the pause menu */
  /* Up to two items of the game's own in the pause menu, after Resume and before Settings, drawn like the others
   * ("Restart level", "Change song"). NULL (or empty) leaves a slot out; with none the menu is Resume, Settings,
   * Quit to title. The shell reads them when the pause menu opens, so cw_shell_set_pause_items, or new text in a
   * buffer a label points at, shows from the next pause; a label's text must stay valid while the menu is open. */
  const char *pause_items[2];
  /* An item of pause_items was chosen: the shell is back in play (as Resume leaves it: the Back sound, the press
   * kept from the game's first tick), then this runs with the item's index in pause_items (0 or 1). The game
   * switches its own screen (restart the level, open its song list). */
  void (*pause_item)(int index, void *user);
  /* Rows of the game's own on the Settings screen (cw_shell_setting), up to CW_SHELL_SETTINGS_MAX, from the title and
   * from the pause menu. The shell copies the array at init (the strings and the ints it points at must stay valid;
   * cw_shell_set_settings_items replaces it) and puts each row's saved choice into *value before the game's first
   * frame. Saved: each row with a key is a slot "shell:<key>" of the save store (cartwheel/save.h), committed with the
   * shell's own settings (cw_shell_save_commit, which also saves a *value the game changed itself). With
   * game_save_slots the slot is in the game's store (cw_shell_save_store); with a game struct the shell keeps a store
   * of its own in the last CW_SHELL_SETTINGS_SAVE_BYTES of the save area. A slot holds the choice's index and its
   * label: loading takes the choice with that label, else the index, else (the index out of range) the default. So a
   * new row starts at its default, a removed row's slot stays unread, and a choice added, removed or moved keeps the
   * player's pick by its label. */
  const cw_shell_setting *settings_items;
  int settings_item_count;
  /* A player changed a row: index is the row's index in settings_items, value its new *value (already set). Not
   * called when a step at an end leaves the value as it was. */
  void (*settings_changed)(int index, int value, void *user);
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
  cw_shell_selection selection; /* how the selected row shows (COLOUR); cw_shell_set_selection changes it */
  cw_color selection_text; /* INVERSE: the selected row's text (alpha 0: the menus' dark background, 0.02 0.02 0.05) */
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
 * missing or from another version with no migration (see cw_shell_desc). Write it, then cw_shell_save_commit. */
void *cw_shell_game_save(void);
void cw_shell_save_commit(void); /* write the settings and the game's save data now */
/* The game's save data as a slot store (cw_shell_desc.game_save_slots): CW_SHELL_GAME_SAVE_MAX bytes of named,
 * versioned slots. NULL (logged) when the game did not ask for slots. cw_save_write, then cw_shell_save_commit. */
cw_save_store *cw_shell_save_store(void);

/* fixed_update, first: runs the menus; true when gameplay runs this tick. */
bool cw_shell_update(void);
/* render, last: draws the current screen's menu or overlay (its own 2D pass). */
void cw_shell_draw(void);

cw_shell_screen cw_shell_screen_now(void); /* the screen showing */
/* Should the game draw its HUD now? True only in play: false under the pause menu, the settings opened from it,
 * the join screen opened during play, and on the title and loading screens. The shell draws its menus over the
 * game's frame, so a HUD left on screen sits under the menu's lines (at a large text scale they overlap); draw the
 * HUD only when this is true. The world, the views and the game's draw_background stay: only the HUD goes. */
bool cw_shell_hud_shown(void);
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
/* The game's pause menu items (as cw_shell_desc.pause_items, NULL items or NULL for none), from the next time the
 * pause menu opens. The pointers are kept: the strings must outlive the menu showing them. */
void cw_shell_set_pause_items(const char *const items[2]);
/* Replace the game's Settings rows (as cw_shell_desc.settings_items; NULL or 0 for none), say to rename a row or its
 * choices in another language, or to offer a row only in some modes. Shown from the next frame the Settings screen
 * draws. A row whose key was not in the rows before loads its saved choice into *value now; the others keep *value. */
void cw_shell_set_settings_items(const cw_shell_setting *items, int count);
/* The menus' selection style from the next frame they draw (as cw_shell_desc.selection; out of range: COLOUR), say
 * when the game switches to a one-bit look. */
void cw_shell_set_selection(cw_shell_selection style);
cw_shell_selection cw_shell_selection_style(void); /* the style in use */
void cw_shell_to_title(void); /* end the match and show the title (every pad drives the menus again) */
/* Loading: progress 0..1 shown on the loading screen; loaded goes on to the title. */
void cw_shell_loading(float progress);
void cw_shell_loaded(void); /* loading done: on to the title */

/* Deprecated: screen shake is gone. No camera shakes, so the shell has no Screen shake setting any more; a hit or a
 * blast is felt in the pads (cw_input_rumble). These stay so older games build and older saves load. */
#define CW_SHELL_SHAKE_REDUCED_SCALE 0.35f /* deprecated: unused */
typedef enum { CW_SHELL_SHAKE_FULL, CW_SHELL_SHAKE_REDUCED, CW_SHELL_SHAKE_OFF } cw_shell_shake;
/* Deprecated: always 0, at any stored level (a game that multiplies its shake by it shakes nothing). */
float cw_shell_screen_shake(void);
/* Deprecated: the level an older save stored (a save keeps its layout; migrations may read it). Nothing applies it. */
cw_shell_shake cw_shell_shake_level(void);
/* Deprecated: stores a level in the save; nothing applies it. */
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
/* A seat's box on the join screen (2D pass units), seat 0 .. max_players - 1: a quarter of the canvas wide
 * whatever the seat count, the row centred under the "Join" heading, so a draw_background can put its own art
 * where the seats are. An empty rect for a seat the game does not have (or before cw_shell_init). */
cw_rect cw_shell_join_seat_rect(int seat);

#ifdef __cplusplus
}
#endif
#endif
