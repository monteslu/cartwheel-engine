/* The game shell (cartwheel/shell.h): loading, title, join, pause and
 * settings screens around a game's play, its saved settings and save data,
 * split screen views. */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "cartwheel/shell.h"
#include "cartwheel/core.h"
#include "cartwheel/debug.h"
#include "cartwheel/input.h"
#include "cartwheel/platform.h"
#include "cartwheel/prompt.h"
#include "cartwheel/quality.h"
#include "cartwheel/quality_menu.h"
#include "cartwheel/scene.h"
#include "cartwheel/scene_file.h"
#include "cartwheel/ui.h"
#include "../input/input_internal.h"

/* ── the save block ────────────────────────────────────────────────── */

#define SHELL_MAGIC 0x4c485357u /* "WSHL" */
#define SHELL_VERSION 1
typedef struct shell_save {
  uint32_t magic;
  uint16_t version, game_version;
  uint32_t game_size;
  uint32_t checksum; /* FNV-1a of everything after this field */
  uint8_t quality;   /* an older engine's preset byte (1..4), read when gfx is 0; written 0 */
  uint8_t music, sfx; /* volume steps 0..10 */
  uint8_t crisp;     /* cw_crisp_ui_saved */
  uint8_t gfx[4];    /* cw_quality_saved, little-endian (0: none, or a save from before it) */
  uint8_t shake;     /* cw_shell_shake (0 Full: also what a save from before the setting holds) */
  uint8_t text;      /* cw_text_scale_saved: Text size and Text background (0: none chosen, also a save from before them) */
  uint8_t reserved[6];
  uint8_t game[CW_SHELL_GAME_SAVE_MAX];
} shell_save;
static shell_save SAVE;

void *cw_shell_save_region(void) { return &SAVE; }
size_t cw_shell_save_region_size(void) { return sizeof SAVE; }

static uint32_t save_sum(void) {
  const uint8_t *b = (const uint8_t *)&SAVE + offsetof(shell_save, quality);
  uint32_t h = 2166136261u;
  for (size_t i = 0; i < sizeof SAVE - offsetof(shell_save, quality); i++) h = (h ^ b[i]) * 16777619u;
  return h;
}

/* ── state ──────────────────────────────────────────────────────────── */

enum { A_UP, A_DOWN, A_LEFT, A_RIGHT, A_ACCEPT, A_BACK, A_PAUSE, A_START, A_COUNT };
static const char *const ACTION_NAMES[A_COUNT] = { "ui_up", "ui_down", "ui_left", "ui_right", "ui_accept", "ui_back", "pause", "ui_start" };
/* ui_start is what the join screen plays with: Start on a pad, Enter on the keyboard (its prompt) */
static const char *const ACTION_BINDS[A_COUNT] = {
  "key:up, key:w, pad:up, axis:ly-@0.5", "key:down, key:s, pad:down, axis:ly+@0.5",
  "key:left, key:a, pad:left, axis:lx-@0.5", "key:right, key:d, pad:right, axis:lx+@0.5",
  "key:return, key:space, pad:south", "key:escape, key:backspace, pad:east", "key:escape, key:p, pad:start",
  "key:return, pad:start",
};

static struct {
  cw_shell_desc d;
  cw_shell_screen screen, settings_from;
  int item;            /* the highlighted menu item */
  int players;
  int joined_pad[4];   /* join screen: each slot's pad (-1 none, CW_MAX_PADS = the keyboard) */
  int seat_pad[4];     /* the pads the players hold in a running match, in player order (-1 none, CW_MAX_PADS = the keyboard) */
  bool mid_join;       /* the join screen was opened during play (cw_shell_join_open) */
  int starting;        /* a Play press was answered: this many players, the game's start waits for that answer to be on screen */
  bool starting_shown; /* cw_shell_draw drew the answer */
  int starting_ticks;
  bool seat_seen[4];   /* the seat's pad has been seen connected (an unplugged pad frees its seat; one never seen is not "gone") */
  float progress;
  cw_action act[A_COUNT];
  cw_bus music, sfx;
  int dbg_screen, dbg_hud, dbg_players, dbg_item, dbg_shake, dbg_starting;
  bool inited, own_audio, hooked, just_resumed;
} S;

static void sound(cw_shell_sound s) {
  if (S.d.sound) S.d.sound(s, S.d.user);
}
static void go(cw_shell_screen to) {
  cw_shell_screen from = S.screen;
  S.screen = to;
  S.item = 0;
  if (to != CW_SHELL_JOIN) S.mid_join = false;
  if (to != CW_SHELL_SETTINGS) cw_quality_menu_close();
  S.dbg_screen = (int)to;
  S.dbg_hud = to == CW_SHELL_PLAY;
  /* a scene app's tree (cw_scene_app) holds still outside play */
  cw_tree *t = cw_scene_app_tree();
  if (t) cw_tree_set_paused(t, to != CW_SHELL_PLAY);
  if (to == CW_SHELL_PLAY && from != CW_SHELL_PLAY && S.hooked) S.just_resumed = true;
  if (from != to && S.d.screen_changed) S.d.screen_changed(from, to, S.d.user);
}
static void apply_volumes(void) {
  cw_bus_set_volume(S.music, SAVE.music / 10.0f);
  cw_bus_set_volume(S.sfx, SAVE.sfx / 10.0f);
}

static void save_reset(void) {
  memset(&SAVE, 0, sizeof SAVE);
  SAVE.magic = SHELL_MAGIC;
  SAVE.version = SHELL_VERSION;
  SAVE.game_version = S.d.game_save_version;
  SAVE.game_size = (uint32_t)S.d.game_save_size;
  SAVE.music = 8;
  SAVE.sfx = 8;
  SAVE.checksum = save_sum();
}

void cw_shell_save_commit(void) {
  uint32_t q = cw_quality_saved();
  SAVE.quality = 0; /* the old preset byte: an older engine reading this save sees no choice (the word is in gfx) */
  for (int i = 0; i < 4; i++) SAVE.gfx[i] = (uint8_t)(q >> (8 * i));
  SAVE.crisp = cw_crisp_ui_saved();
  SAVE.text = cw_text_scale_saved();
  SAVE.checksum = save_sum();
  cw_save_commit();
}

void *cw_shell_game_save(void) { return SAVE.game; }

cw_shell_shake cw_shell_shake_level(void) { return SAVE.shake <= CW_SHELL_SHAKE_OFF ? (cw_shell_shake)SAVE.shake : CW_SHELL_SHAKE_FULL; }
float cw_shell_screen_shake(void) {
  cw_shell_shake l = cw_shell_shake_level();
  float s = l == CW_SHELL_SHAKE_OFF ? 0.0f : l == CW_SHELL_SHAKE_REDUCED ? CW_SHELL_SHAKE_REDUCED_SCALE : 1.0f;
  return cw_ui_reduced_motion() && s > CW_SHELL_SHAKE_REDUCED_SCALE ? CW_SHELL_SHAKE_REDUCED_SCALE : s;
}
void cw_shell_set_shake_level(cw_shell_shake level) {
  SAVE.shake = (uint8_t)CW_CLAMP((int)level, 0, 2);
  S.dbg_shake = SAVE.shake;
  cw_shell_save_commit();
}

static void tree_hook(cw_tree *t, double dt, void *user);

void cw_shell_init(const cw_shell_desc *d) {
  memset(&S, 0, sizeof S);
  S.d = *d;
  if (!S.d.title) S.d.title = "cartwheel";
  if (S.d.max_players < 1) S.d.max_players = 1;
  if (S.d.max_players > 4) S.d.max_players = 4;
  if (!S.d.font.id) S.d.font = cw_ui_font_sans();
  if (S.d.accent.a == 0) S.d.accent = cw_rgba(1.0f, 0.78f, 0.25f, 1);
  if (S.d.game_save_size > CW_SHELL_GAME_SAVE_MAX)
    cw_error("shell: game_save_size %zu is over CW_SHELL_GAME_SAVE_MAX (%d): the save keeps the first %d bytes",
             S.d.game_save_size, CW_SHELL_GAME_SAVE_MAX, CW_SHELL_GAME_SAVE_MAX);
  if (SAVE.magic != SHELL_MAGIC || SAVE.version != SHELL_VERSION || SAVE.game_version != S.d.game_save_version ||
      SAVE.game_size != (uint32_t)S.d.game_save_size || SAVE.checksum != save_sum()) {
    if (SAVE.magic) cw_warn("shell: the save is from another version or damaged: starting with defaults");
    save_reset();
  }
  uint32_t gfx = (uint32_t)SAVE.gfx[0] | (uint32_t)SAVE.gfx[1] << 8 | (uint32_t)SAVE.gfx[2] << 16 | (uint32_t)SAVE.gfx[3] << 24;
  cw_quality_restore(gfx ? gfx : SAVE.quality);
  cw_crisp_ui_restore(SAVE.crisp);
  cw_text_scale_restore(SAVE.text);
  for (int i = 0; i < A_COUNT; i++) {
    S.act[i] = cw_action_add(ACTION_NAMES[i], 0.5f);
    cw_binding b[CW_INPUT_MAX_BINDINGS];
    if (cw_action_bindings(S.act[i], b, CW_INPUT_MAX_BINDINGS) == 0) cw_action_bind_text(S.act[i], ACTION_BINDS[i]);
  }
  /* the mixer, unless the game started it already (the shell then also runs
   * its per-frame update, in cw_shell_draw) */
  if (!cw_audio_sample_rate()) S.own_audio = cw_audio_init(NULL);
  S.music = cw_bus_find("music");
  if (S.music < 0) S.music = cw_bus_create("music", 0);
  S.sfx = cw_bus_find("sfx");
  if (S.sfx < 0) S.sfx = cw_bus_create("sfx", 0);
  apply_volumes();
  S.players = 1;
  for (int i = 0; i < 4; i++) S.joined_pad[i] = -1;
  cw_debug_field("shell.screen", &S.dbg_screen, CW_DBG_I32, 1);
  cw_debug_field("shell.players", &S.dbg_players, CW_DBG_I32, 1);
  cw_debug_field("shell.item", &S.dbg_item, CW_DBG_I32, 1);
  S.dbg_shake = cw_shell_shake_level();
  cw_debug_field("shell.shake", &S.dbg_shake, CW_DBG_I32, 1);
  cw_debug_field("shell.starting", &S.dbg_starting, CW_DBG_I32, 1);
  cw_debug_field("shell.hud", &S.dbg_hud, CW_DBG_I32, 1);
  S.inited = true;
  S.screen = S.d.loading ? CW_SHELL_LOADING : CW_SHELL_TITLE;
  S.dbg_screen = (int)S.screen;
  S.dbg_hud = S.screen == CW_SHELL_PLAY;
  cw_tree *t = cw_scene_app_tree();
  if (t) {
    cw_tree_set_paused(t, true);
    cw_tree_add_hook(t, 0, 1000, tree_hook, NULL);
    S.hooked = true;
  }
  S.dbg_players = S.players;
  if (S.d.screen_changed) S.d.screen_changed(S.screen, S.screen, S.d.user);
}

cw_shell_screen cw_shell_screen_now(void) { return S.screen; }
bool cw_shell_hud_shown(void) { return S.screen == CW_SHELL_PLAY; }
int cw_shell_players(void) { return S.players; }
cw_bus cw_shell_music_bus(void) { return S.music; }
cw_bus cw_shell_sfx_bus(void) { return S.sfx; }
void cw_shell_loading(float progress) { S.progress = CW_CLAMP(progress, 0.0f, 1.0f); }
void cw_shell_loaded(void) {
  if (S.screen == CW_SHELL_LOADING) go(CW_SHELL_TITLE);
}

/* players: pads 0..n-1 in order, player 0 also on the keyboard, mouse and touch */
static void assign_default(int n) {
  for (int p = 0; p < 4; p++) {
    cw_input_assign(p, p < n ? p : -1, p == 0 ? CW_DEVICE_KEYBOARD | CW_DEVICE_MOUSE | CW_DEVICE_TOUCH : 0);
    S.seat_pad[p] = p < n ? p : -1;
    S.seat_seen[p] = false;
  }
}

static void begin_match(int n) {
  S.starting = S.dbg_starting = 0;
  S.players = CW_CLAMP(n, 1, S.d.max_players);
  S.dbg_players = S.players;
  go(CW_SHELL_PLAY);
  if (S.d.start) S.d.start(S.players, S.d.user);
}

/* A player's press to play is answered now: the sound at once, "Starting..." on this frame, and the game's start
 * (which can load for a long time, with no frame presented) on the next tick after that picture is drawn. A game that
 * never draws the shell starts anyway. */
enum { STARTING_MAX_TICKS = 16 };
static void begin_starting(int n) {
  sound(CW_SHELL_SND_SELECT);
  S.starting = CW_CLAMP(n, 1, S.d.max_players);
  S.dbg_starting = 1;
  S.starting_shown = false;
  S.starting_ticks = 0;
}

void cw_shell_start(int players) {
  assign_default(players);
  begin_match(players);
}

bool cw_shell_join_open(void) {
  if (!S.inited || S.screen != CW_SHELL_PLAY || S.d.max_players < 2) return false;
  for (int i = 0; i < 4; i++) S.joined_pad[i] = i < S.players ? S.seat_pad[i] : -1;
  S.mid_join = true;
  go(CW_SHELL_JOIN);
  return true;
}

bool cw_shell_seat_taken(int seat) {
  return S.inited && seat >= 0 && seat < S.players && S.seat_pad[seat] >= 0;
}

bool cw_shell_join_requested(void) {
  if (!S.inited || S.screen != CW_SHELL_PLAY || S.d.max_players < 2) return false;
  for (int pad = 0; pad <= CW_MAX_PADS; pad++) {
    bool seated = false;
    for (int p = 0; p < S.players; p++) seated |= S.seat_pad[p] == pad;
    bool kb = pad == CW_MAX_PADS;
    if (seated) continue;
    if (kb ? (cw__input_key_pressed(40) || cw__input_key_pressed(44))
           : (cw__input_pad_pressed(pad) & ((1u << CW_PAD_SOUTH) | (1u << CW_PAD_START)))) return true;
  }
  return false;
}

bool cw_shell_join_press(void) {
  if (!S.inited || S.screen != CW_SHELL_PLAY || S.d.max_players < 2) return false;
  /* pads only: the keyboard usually drives a seated player already (Enter there is that player's own key) */
  for (int pad = 0; pad < CW_MAX_PADS; pad++) {
    bool seated = false;
    for (int p = 0; p < S.players; p++) seated |= S.seat_pad[p] == pad;
    if (seated || !(cw__input_pad_pressed(pad) & (1u << CW_PAD_SOUTH))) continue;
    int seat = -1;
    for (int s = 0; s < S.d.max_players && s < 4; s++)
      if (s >= S.players || S.seat_pad[s] < 0) { seat = s; break; }
    if (seat < 0) return false;
    S.seat_pad[seat] = pad;
    S.seat_seen[seat] = false;
    cw_input_assign(seat, pad, 0);
    if (seat + 1 > S.players) S.players = seat + 1;
    S.dbg_players = S.players;
    sound(CW_SHELL_SND_JOIN);
    if (S.d.players_changed) S.d.players_changed(S.players, S.d.user);
    return true;
  }
  return false;
}

void cw_shell_to_title(void) {
  assign_default(4); /* menus answer every pad again */
  for (int i = 0; i < 4; i++) S.joined_pad[i] = -1;
  S.starting = S.dbg_starting = 0;
  go(CW_SHELL_TITLE);
}

/* ── menus ──────────────────────────────────────────────────────────── */

/* any player's action this tick (before a match every pad is a player) */
static bool any_pressed(int a) {
  for (int p = 0; p < 4; p++)
    if (cw_action_just_pressed(p, S.act[a])) return true;
  return false;
}
static bool any_pad_start(void) {
  for (int pad = 0; pad < CW_MAX_PADS; pad++)
    if (cw__input_pad_pressed(pad) & (1u << CW_PAD_START)) return true;
  return false;
}

typedef struct { const char *label; char value[48]; } item;
enum { MAX_ITEMS = 12 };
static item items[MAX_ITEMS];
static int n_items;
static cw_rect item_rects[MAX_ITEMS];

static void title_items(void) {
  n_items = 0;
  items[n_items++] = (item){ "Play", "" };
  items[n_items++] = (item){ "Settings", "" };
#if !defined(__wasi__) && !defined(__ANDROID__)
  items[n_items++] = (item){ "Quit", "" };
#endif
}
static void pause_items(void) {
  n_items = 0;
  items[n_items++] = (item){ "Resume", "" };
  items[n_items++] = (item){ "Settings", "" };
  items[n_items++] = (item){ "Quit to title", "" };
}
enum { SET_GRAPHICS, SET_MUSIC, SET_SFX, SET_SHAKE, SET_TEXT, SET_TEXT_BG, SET_CONTROLS, SET_BACK };
static const char *const SHAKE_LABELS[3] = { "Full", "Reduced", "Off" };
static void settings_items(void) {
  n_items = 0;
  items[n_items] = (item){ "Graphics", "" };
  snprintf(items[n_items++].value, sizeof items[0].value, "%s  (%s)  ...", cw_quality_label(), cw_render_res_label());
  items[n_items] = (item){ "Music", "" };
  snprintf(items[n_items++].value, sizeof items[0].value, "%d%%", SAVE.music * 10);
  items[n_items] = (item){ "Effects", "" };
  snprintf(items[n_items++].value, sizeof items[0].value, "%d%%", SAVE.sfx * 10);
  items[n_items] = (item){ "Screen shake", "" };
  snprintf(items[n_items++].value, sizeof items[0].value, "%s", SHAKE_LABELS[cw_shell_shake_level()]);
  items[n_items] = (item){ "Text size", "" };
  snprintf(items[n_items++].value, sizeof items[0].value, "%s", cw_text_scale_label());
  items[n_items] = (item){ "Text background", "" };
  snprintf(items[n_items++].value, sizeof items[0].value, "%s", cw_text_backdrop_label());
  items[n_items++] = (item){ "Controls", "" };
  items[n_items++] = (item){ "Back", "" };
}
static void build_items(void) {
  switch (S.screen) {
  case CW_SHELL_TITLE: title_items(); break;
  case CW_SHELL_PAUSE: pause_items(); break;
  case CW_SHELL_SETTINGS: settings_items(); break;
  default: n_items = 0; break;
  }
}

/* a pointer tap (mouse or touch, canvas units) on an item: that item, chosen */
static int tapped_item(void) {
  const cw_raw_input *in = cw_input_raw();
  if (!in || !(cw__input_mouse_pressed() & 1)) return -1;
  cw_vec2 p = cw_r2d_window_to_canvas(cw_v2(in->mouse_x, in->mouse_y));
  for (int i = 0; i < n_items; i++) {
    cw_rect r = item_rects[i];
    if (p.x >= r.x && p.x < r.x + r.w && p.y >= r.y && p.y < r.y + r.h) return i;
  }
  return -1;
}

/* a volume step 0..10: left / right move it, accept goes round */
static uint8_t step(uint8_t v, int dir) {
  if (!dir) return (uint8_t)((v + 1) % 11);
  return (uint8_t)CW_CLAMP((int)v + dir, 0, 10);
}
static void settings_change(int dir) { /* dir -1 / +1: left / right; 0: accept */
  switch (S.item) {
  case SET_GRAPHICS:
    if (!dir) { /* accept: the graphics page, every setting */
      cw_quality_menu_open();
      sound(CW_SHELL_SND_SELECT);
      return;
    }
    if (dir < 0) cw_quality_cycle_back();
    else cw_quality_cycle();
    break;
  case SET_MUSIC: SAVE.music = step(SAVE.music, dir); break;
  case SET_SFX: SAVE.sfx = step(SAVE.sfx, dir); break;
  case SET_SHAKE: /* left / right step it (stopping at the ends), accept goes round */
    SAVE.shake = (uint8_t)(!dir ? ((int)cw_shell_shake_level() + 1) % 3 : CW_CLAMP((int)cw_shell_shake_level() + dir, 0, 2));
    break;
  case SET_TEXT: /* 100, 125, 150, 200%: left / right step it (stopping at the ends), accept goes round */
    if (dir < 0 && cw_text_scale() > 1.01f) cw_text_scale_cycle_back();
    else if ((dir > 0 && cw_text_scale() < 1.99f) || !dir) cw_text_scale_cycle();
    break;
  case SET_TEXT_BG: { /* Off, Outline, Panel: the same */
    int b = (int)cw_text_backdrop_get();
    cw_text_backdrop_set((cw_text_backdrop)(!dir ? (b + 1) % 3 : CW_CLAMP(b + dir, 0, 2)));
    break;
  }
  default: return;
  }
  apply_volumes();
  S.dbg_shake = SAVE.shake;
  cw_shell_save_commit();
  sound(CW_SHELL_SND_MOVE);
}

static void accept(int i) {
  S.item = i;
  switch (S.screen) {
  case CW_SHELL_TITLE:
    sound(CW_SHELL_SND_SELECT);
    if (i == 0) {
      if (S.d.max_players > 1) {
        for (int k = 0; k < 4; k++) S.joined_pad[k] = -1;
        go(CW_SHELL_JOIN);
      } else {
        assign_default(1);
        begin_starting(1);
      }
    } else if (i == 1) {
      S.settings_from = CW_SHELL_TITLE;
      go(CW_SHELL_SETTINGS);
    } else cw_quit();
    break;
  case CW_SHELL_PAUSE:
    sound(i == 0 ? CW_SHELL_SND_BACK : CW_SHELL_SND_SELECT);
    if (i == 0) go(CW_SHELL_PLAY);
    else if (i == 1) {
      S.settings_from = CW_SHELL_PAUSE;
      go(CW_SHELL_SETTINGS);
    } else {
      if (S.d.quit) S.d.quit(S.d.user);
      cw_shell_to_title();
    }
    break;
  case CW_SHELL_SETTINGS:
    if (i == SET_BACK) {
      sound(CW_SHELL_SND_BACK);
      cw_shell_screen back = S.settings_from;
      go(back);
      S.item = 1; /* Settings, in the title and pause menus */
    } else if (i == SET_CONTROLS) {
      sound(CW_SHELL_SND_SELECT); /* the list shows beside the menu while Controls is highlighted */
    } else settings_change(0);
    break;
  default: break;
  }
}

/* go: every press to play ends here. At the start of a match players take the pads in slot order (a gap closes); in
 * the middle of one every seat keeps its number, a seat nobody holds reads no input. The keyboard, mouse and touch
 * belong to the keyboard's player, else to the first player */
static void join_go(void) {
  int p = 0, kb_slot = -1, first = -1, last = -1;
  for (int s = 0; s < 4; s++) {
    if (S.joined_pad[s] == CW_MAX_PADS) kb_slot = s;
    if (S.joined_pad[s] >= 0) {
      if (first < 0) first = s;
      last = s;
    }
  }
  if (S.mid_join) {
    for (int s = 0; s < 4; s++) {
      S.seat_pad[s] = S.joined_pad[s];
      S.seat_seen[s] = false;
      bool k = S.joined_pad[s] == CW_MAX_PADS;
      bool devices = S.joined_pad[s] >= 0 && (k || (kb_slot < 0 && s == first));
      cw_input_assign(s, S.joined_pad[s] < 0 || k ? -1 : S.joined_pad[s], devices ? CW_DEVICE_KEYBOARD | CW_DEVICE_MOUSE | CW_DEVICE_TOUCH : 0);
    }
    sound(CW_SHELL_SND_SELECT);
    S.mid_join = false;
    S.players = last + 1;
    S.dbg_players = S.players;
    go(CW_SHELL_PLAY);
    S.just_resumed = true; /* the Start that confirmed is not a new pause */
    if (S.d.players_changed) S.d.players_changed(S.players, S.d.user);
    return;
  }
  for (int s = 0; s < 4; s++) {
    if (S.joined_pad[s] < 0) continue;
    bool k = S.joined_pad[s] == CW_MAX_PADS;
    bool devices = k || (kb_slot < 0 && p == 0);
    S.seat_pad[p] = S.joined_pad[s];
    S.seat_seen[p] = false;
    cw_input_assign(p++, k ? -1 : S.joined_pad[s], devices ? CW_DEVICE_KEYBOARD | CW_DEVICE_MOUSE | CW_DEVICE_TOUCH : 0);
  }
  for (int q = p; q < 4; q++) cw_input_assign(q, -1, 0), S.seat_pad[q] = -1;
  begin_starting(p);
}

static int free_seat(void) {
  for (int s = 0; s < S.d.max_players; s++)
    if (S.joined_pad[s] < 0) return s;
  return -1;
}

/* join: a pad joins with South and leaves with East; the keyboard joins with Enter and leaves with Esc. Start on any
 * pad plays, seating that pad first if it has no seat and one is free (no seat free: the seated players play). Enter
 * on a keyboard that holds a seat plays, and with every seat taken by others. */
static void join_update(void) {
  bool play = false;
  for (int pad = 0; pad <= CW_MAX_PADS; pad++) {
    bool kb = pad == CW_MAX_PADS;
    uint32_t pressed = kb ? 0 : cw__input_pad_pressed(pad);
    bool south = kb ? (cw__input_key_pressed(40) || cw__input_key_pressed(44)) /* return, space */ : (pressed & (1u << CW_PAD_SOUTH));
    bool east = kb ? cw__input_key_pressed(41) /* escape */ : (pressed & (1u << CW_PAD_EAST));
    bool start = pressed & (1u << CW_PAD_START);
    int slot = -1, n = 0;
    for (int s = 0; s < 4; s++) {
      if (S.joined_pad[s] == pad) slot = s;
      if (S.joined_pad[s] >= 0) n++;
    }
    if (slot < 0 && south) {
      int s = free_seat();
      if (s >= 0) {
        S.joined_pad[s] = pad;
        slot = s;
        sound(CW_SHELL_SND_JOIN);
      } else if (kb) play = true;
    } else if (slot >= 0 && east) {
      if (S.mid_join && n <= 1) continue; /* the match keeps at least one player */
      S.joined_pad[slot] = -1;
      sound(CW_SHELL_SND_BACK);
      continue; /* a pad that left this tick does not also play */
    } else if (slot < 0 && east && S.mid_join) { /* cancel: the match goes on as it was */
      S.mid_join = false;
      sound(CW_SHELL_SND_BACK);
      go(CW_SHELL_PLAY);
      S.just_resumed = true;
      return;
    } else if (slot < 0 && east && n == 0) {
      sound(CW_SHELL_SND_BACK);
      go(CW_SHELL_TITLE);
      return;
    } else if (slot >= 0 && south && kb) play = true;
    if (start) {
      if (slot < 0) {
        int s = free_seat();
        if (s >= 0) S.joined_pad[s] = pad;
      }
      play = true;
    }
  }
  if (play) join_go();
}

static void menu_tick(void);
/* a scene app's tree is paused under the menus, which stops the app's
 * fixed_update: the menus then tick from a hook of the tree's own */
static void tree_hook(cw_tree *t, double dt, void *user) {
  (void)dt, (void)user;
  if (S.inited && S.screen != CW_SHELL_PLAY && cw_tree_paused(t)) menu_tick();
}

bool cw_shell_update(void) {
  if (!S.inited) return true;
  if (S.screen != CW_SHELL_PLAY && S.hooked) return false; /* the tree's hook runs the menus */
  if (S.just_resumed) { /* resumed by the hook this tick: that press is not a new pause */
    S.just_resumed = false;
    return true;
  }
  menu_tick();
  return S.screen == CW_SHELL_PLAY;
}

static void menu_tick(void) {
  if (S.starting) { /* the answer to the press is on screen (presented when its frame ended): now the game's start */
    if (S.starting_shown || ++S.starting_ticks > STARTING_MAX_TICKS) begin_match(S.starting);
    return;
  }
  switch (S.screen) {
  case CW_SHELL_LOADING: return;
  case CW_SHELL_PLAY: {
    /* a pad that goes away (unplugged, out of battery) frees its seat: the match goes on with the others, and the
     * game hears players_changed; when every pad that was held is gone the pause menu opens instead */
    const cw_raw_input *raw = cw_input_raw();
    if (raw && S.d.max_players > 1) {
      int held = 0, gone = 0;
      for (int p = 0; p < S.players; p++) {
        int pd = S.seat_pad[p];
        if (pd < 0 || pd >= CW_MAX_PADS) { held += pd >= 0; continue; }
        held++;
        if (raw->pads[pd].connected) S.seat_seen[p] = true;
        else if (S.seat_seen[p]) gone++;
      }
      if (gone && gone >= held) {
        sound(CW_SHELL_SND_PAUSE);
        go(CW_SHELL_PAUSE);
        return;
      }
      if (gone) {
        int last = -1;
        for (int p = 0; p < S.players; p++) {
          int pd = S.seat_pad[p];
          if (pd >= 0 && pd < CW_MAX_PADS && !raw->pads[pd].connected && S.seat_seen[p]) {
            S.seat_pad[p] = -1;
            S.seat_seen[p] = false;
            cw_input_assign(p, -1, 0);
          }
          if (S.seat_pad[p] >= 0) last = p;
        }
        S.players = last + 1;
        S.dbg_players = S.players;
        if (S.d.players_changed) S.d.players_changed(S.players, S.d.user);
      }
    }
    for (int p = 0; p < S.players; p++)
      if (cw_action_just_pressed(p, S.act[A_PAUSE])) {
        sound(CW_SHELL_SND_PAUSE);
        go(CW_SHELL_PAUSE);
        return;
      }
    return;
  }
  case CW_SHELL_JOIN: join_update(); return;
  default: break;
  }
  if (S.screen == CW_SHELL_SETTINGS && cw_quality_menu_is_open()) { /* the graphics page */
    int dy = any_pressed(A_DOWN) - any_pressed(A_UP), dx = any_pressed(A_RIGHT) - any_pressed(A_LEFT);
    bool acc = any_pressed(A_ACCEPT), bk = any_pressed(A_BACK);
    const cw_raw_input *in = cw_input_raw();
    bool tap = in && (cw__input_mouse_pressed() & 1) && cw_quality_menu_tap(cw_r2d_window_to_canvas(cw_v2(in->mouse_x, in->mouse_y)));
    if (!tap) cw_quality_menu_input(dy, dx, acc, bk);
    if (dy || dx || acc || tap) sound(CW_SHELL_SND_MOVE);
    if (!cw_quality_menu_is_open()) {
      sound(CW_SHELL_SND_BACK);
      if (cw_quality_menu_changed()) cw_shell_save_commit();
    }
    return;
  }
  build_items();
  if (any_pressed(A_UP)) { S.item = (S.item + n_items - 1) % n_items; sound(CW_SHELL_SND_MOVE); }
  if (any_pressed(A_DOWN)) { S.item = (S.item + 1) % n_items; sound(CW_SHELL_SND_MOVE); }
  if (S.screen == CW_SHELL_SETTINGS) {
    if (any_pressed(A_LEFT)) settings_change(-1);
    if (any_pressed(A_RIGHT)) settings_change(1);
  }
  int tap = tapped_item();
  if (tap >= 0) accept(tap);
  else if (any_pressed(A_ACCEPT) || (S.screen == CW_SHELL_TITLE && any_pad_start())) accept(S.item);
  else if (any_pressed(A_BACK) || (S.screen == CW_SHELL_PAUSE && any_pressed(A_PAUSE))) {
    if (S.screen == CW_SHELL_PAUSE) accept(0);
    else if (S.screen == CW_SHELL_SETTINGS) accept(SET_BACK);
  }
  S.dbg_item = S.item;
}

/* ── drawing ────────────────────────────────────────────────────────── */

/* Sizes are shares of the canvas height (u, below, is a twentieth of it) at a text scale of 100%. The player's
 * Text size (cartwheel/render2d.h) grows the text, and every gap here comes from the size the text draws at
 * (cw_text_drawn_size), so nothing overlaps at 200%: a list that no longer fits scrolls with its selection, a hint
 * line too wide for the canvas breaks into lines. */
static float drawn(float size) { return cw_text_drawn_size(S.d.font, size); }
static cw_text_style tstyle(float size, cw_color c) {
  return (cw_text_style){ .size = size, .color = c, .outline_width = size * 0.06f, .outline_color = cw_rgba(0, 0, 0, 0.8f) };
}
static float text_w(const char *s, float size) {
  cw_text_style st = tstyle(size, CW_WHITE);
  return cw_font_measure(S.d.font, s, &st).width;
}
static void text(const char *s, float x, float y, float size, cw_color c, int align /* 0 left 1 center 2 right */) {
  cw_text_style st = tstyle(size, c);
  if (align) {
    cw_text_metrics m = cw_font_measure(S.d.font, s, &st);
    x -= align == 1 ? m.width * 0.5f : m.width;
  }
  cw_r2d_text(S.d.font, s, cw_v2(x, y), &st);
}

/* A line of controls under a menu, drawn with button prompts for the devices the players hold (cartwheel/prompt.h).
 * Left out when no seat is on a pad, keyboard or mouse: on a touch screen the menu items are tapped. */
static bool hints_wanted(void) {
  const cw_raw_input *raw = cw_input_raw();
  for (int p = 0; p < 4; p++) {
    cw_prompt_device d = cw_prompt_device_of(p);
    int pad = cw_input_player_pad(p);
    if (d == CW_PROMPT_PAD && raw && pad >= 0 && raw->pads[pad].connected) return true;
    if ((d == CW_PROMPT_KEYBOARD || d == CW_PROMPT_MOUSE) && (cw_input_player_devices(p) & (CW_DEVICE_KEYBOARD | CW_DEVICE_MOUSE)))
      return true;
  }
  return false;
}
/* markup that fits maxw: as it is, else broken at its wide gaps, else at every space after a glyph (frame arena) */
static const char *fit_markup(const char *markup, float size, float maxw) {
  if (cw_prompt_text_size(S.d.font, markup, -1, size).x <= maxw) return markup;
  size_t n = strlen(markup);
  char *o = cw_frame_alloc(n + 1);
  size_t k = 0;
  for (size_t i = 0; i < n;) {
    if (!strncmp(markup + i, "    ", 4)) {
      o[k++] = '\n';
      i += 4;
    } else o[k++] = markup[i++];
  }
  o[k] = 0;
  if (cw_prompt_text_size(S.d.font, o, -1, size).x <= maxw) return o;
  for (size_t i = 1; i < k; i++)
    if (o[i] == ' ' && o[i - 1] == '}') o[i] = '\n';
  return o;
}
/* the hint's top when its bottom sits at `bottom` (the space a menu above it must leave) */
static float hint_top(const char *markup, float cx, float bottom, float size) {
  (void)cx;
  const char *m = fit_markup(markup, size, cw_r2d_canvas_size().x * 0.94f);
  return bottom - cw_prompt_text_size(S.d.font, m, -1, size).y;
}
static void hint(const char *markup, float cx, float bottom, float size) {
  if (!hints_wanted()) return;
  const char *m = fit_markup(markup, size, cw_r2d_canvas_size().x * 0.94f);
  float h = cw_prompt_text_size(S.d.font, m, -1, size).y;
  cw_prompt_text(S.d.font, m, -1, cw_v2(cx, bottom - h), size, cw_rgba(1, 1, 1, 0.75f), CW_ALIGN_CENTER);
}

/* a small triangle: more rows above (up) or below */
static void more_mark(float cx, float y, float d, bool up) {
  float s = d * 0.28f;
  cw_vec2 p[3] = { { cx - s, up ? y + s * 0.5f : y - s * 0.5f }, { cx + s, up ? y + s * 0.5f : y - s * 0.5f }, { cx, up ? y - s * 0.5f : y + s * 0.5f } };
  cw_r2d_polygon(p, 3, cw_rgba(1, 1, 1, 0.7f));
}

static void item_line(int i, char *line, size_t cap) {
  if (items[i].value[0]) snprintf(line, cap, "%s   %s", items[i].label, items[i].value);
  else snprintf(line, cap, "%s", items[i].label);
}
/* the widest row at this size */
static float menu_width(float size) {
  float w = 0;
  for (int i = 0; i < n_items; i++) {
    char line[96];
    item_line(i, line, sizeof line);
    w = fmaxf(w, text_w(line, size));
  }
  return w;
}
/* the menu's rows from y0 down to no lower than y1, centred on cx */
static void draw_menu(float cx, float y0, float y1, float size) {
  float d = drawn(size), step = d * 1.45f;
  for (int i = 0; i < MAX_ITEMS; i++) item_rects[i] = cw_rect_make(0, 0, 0, 0);
  int first = 0, count = n_items;
  if ((float)n_items * step > y1 - y0) { /* scroll: the selection stays in view, a mark says there is more */
    y0 += d * 0.55f;
    count = CW_MAX(1, (int)((y1 - y0 - d * 0.55f) / step));
    first = CW_CLAMP(S.item - count / 2, 0, n_items - count);
  }
  float y = y0;
  for (int i = first; i < first + count && i < n_items; i++) {
    bool sel = i == S.item;
    cw_color c = sel ? S.d.accent : cw_rgba(1, 1, 1, 0.85f);
    char line[96];
    item_line(i, line, sizeof line);
    text(line, cx, y, size, c, 1);
    float w = fmaxf(text_w(line, size) + d * 2, d * 8);
    item_rects[i] = cw_rect_make(cx - w * 0.5f, y - d * 0.15f, w, d * 1.3f);
    y += step;
  }
  if (first > 0) more_mark(cx, y0 - d * 0.3f, d, true);
  if (first + count < n_items) more_mark(cx, y + d * 0.05f, d, false);
}

/* each action and its bindings, in [x, right] from y down to bottom: names in one column, bindings wrapped in the
 * next */
static void draw_controls(float x, float y, float right, float bottom, float size) {
  text("Controls", x, y, size, S.d.accent, 0);
  y += drawn(size) * 1.5f;
  float s2 = size * 0.8f, d2 = drawn(s2), name_w = 0;
  for (int a = 0; a < cw_action_count(); a++) {
    const char *name = cw_action_name(a);
    if (name && strncmp(name, "ui_", 3)) name_w = fmaxf(name_w, text_w(name, s2));
  }
  float col = x + name_w + d2 * 1.2f, wrap = fmaxf(right - col, d2 * 5);
  for (int a = 0; a < cw_action_count(); a++) {
    const char *name = cw_action_name(a);
    if (!name || !strncmp(name, "ui_", 3)) continue;
    cw_binding b[CW_INPUT_MAX_BINDINGS];
    int nb = cw_action_bindings(a, b, CW_INPUT_MAX_BINDINGS);
    char line[160] = "", one[48];
    for (int i = 0; i < nb && i < 3; i++) {
      cw_binding_format(b[i], one, sizeof one);
      snprintf(line + strlen(line), sizeof line - strlen(line), "%s%s", i ? ", " : "", one);
    }
    cw_text_style st = tstyle(s2, cw_rgba(1, 1, 1, 0.7f));
    st.wrap_width = wrap;
    float h = cw_font_measure(S.d.font, line, &st).height;
    if (y + h > bottom - d2 * 1.25f) { /* no room for this row and a mark under it: the mark says there is more */
      text("...", x, y, s2, cw_rgba(1, 1, 1, 0.7f), 0);
      break;
    }
    text(name, x, y, s2, cw_rgba(1, 1, 1, 0.9f), 0);
    cw_r2d_text(S.d.font, line, cw_v2(col, y), &st);
    y += h + d2 * 0.25f;
  }
}

static const char *const HINT_TITLE = "{ui_up|ui_down} choose    {ui_accept} select";
static const char *const HINT_PAUSE = "{ui_up|ui_down} choose    {ui_accept} select    {ui_back} resume";
static const char *const HINT_SETTINGS = "{ui_up|ui_down} choose    {ui_left|ui_right} change    {ui_back} back";

void cw_shell_draw(void) {
  if (S.own_audio) cw_audio_update();
  if (S.inited && cw_quality_is_auto()) { /* the level Auto holds now outlives the run */
    uint32_t saved = (uint32_t)SAVE.gfx[0] | (uint32_t)SAVE.gfx[1] << 8 | (uint32_t)SAVE.gfx[2] << 16 | (uint32_t)SAVE.gfx[3] << 24;
    if (cw_quality_saved() != saved) cw_shell_save_commit();
  }
  if (!S.inited || S.screen == CW_SHELL_PLAY) return;
  cw_r2d_begin(NULL);
  cw_vec2 sz = cw_r2d_canvas_size();
  float u = sz.y / 20.0f; /* the menus scale with the canvas height */
  float cx = sz.x * 0.5f, margin = sz.x * 0.04f, foot = sz.y - u; /* the hint line's bottom */
  if (S.d.draw_background) S.d.draw_background(S.screen, sz, S.d.user);
  cw_r2d_rect(cw_rect_make(0, 0, sz.x, sz.y), cw_rgba(0.02f, 0.02f, 0.05f, S.d.draw_background ? 0.3f : S.screen == CW_SHELL_PAUSE ? 0.55f : 0.85f));
  build_items();
  switch (S.screen) {
  case CW_SHELL_LOADING: {
    float y = sz.y * 0.42f, bar = fmaxf(sz.y * 0.55f, y + drawn(u * 1.2f) * 1.3f);
    text("Loading", cx, y, u * 1.2f, cw_rgba(1, 1, 1, 1), 1);
    cw_r2d_rect(cw_rect_make(sz.x * 0.3f, bar, sz.x * 0.4f, u * 0.4f), cw_rgba(1, 1, 1, 0.2f));
    cw_r2d_rect(cw_rect_make(sz.x * 0.3f, bar, sz.x * 0.4f * S.progress, u * 0.4f), S.d.accent);
    break;
  }
  case CW_SHELL_TITLE: {
    float y = sz.y * 0.2f;
    text(S.d.title, cx, y, u * 2.4f, cw_rgba(1, 1, 1, 1), 1);
    y += drawn(u * 2.4f) * 1.17f;
    if (S.d.subtitle) {
      text(S.d.subtitle, cx, y, u * 0.9f, cw_rgba(1, 1, 1, 0.75f), 1);
      y += drawn(u * 0.9f) * 1.4f;
    }
    float hs = u * 0.75f, top = hint_top(HINT_TITLE, cx, foot, hs);
    draw_menu(cx, fmaxf(sz.y * 0.5f, y + u * 0.4f), top - drawn(hs) * 0.4f, u * 1.1f);
    hint(HINT_TITLE, cx, foot, hs);
    break;
  }
  case CW_SHELL_PAUSE: {
    text("Paused", cx, sz.y * 0.22f, u * 1.8f, cw_rgba(1, 1, 1, 1), 1);
    float hs = u * 0.75f, top = hint_top(HINT_PAUSE, cx, foot, hs);
    draw_menu(cx, fmaxf(sz.y * 0.45f, sz.y * 0.22f + drawn(u * 1.8f) * 1.3f), top - drawn(hs) * 0.4f, u * 1.1f);
    hint(HINT_PAUSE, cx, foot, hs);
    break;
  }
  case CW_SHELL_SETTINGS: {
    if (cw_quality_menu_is_open()) {
      cw_quality_menu_draw(&(cw_quality_menu_style){ .font = S.d.font, .accent = S.d.accent, .set_backdrop = true });
      break;
    }
    text("Settings", cx, sz.y * 0.1f, u * 1.6f, cw_rgba(1, 1, 1, 1), 1);
    float hs = u * 0.75f, rs = u * 0.85f, top = hint_top(HINT_SETTINGS, cx, foot, hs);
    float y0 = fmaxf(sz.y * 0.26f, sz.y * 0.1f + drawn(u * 1.6f) * 1.35f), y1 = top - drawn(hs) * 0.4f;
    if (S.item == SET_CONTROLS) { /* the list beside the menu, or in its place when there is no room beside it */
      float mw = menu_width(rs), mx = fmaxf(sz.x * 0.3f, margin + mw * 0.5f), lx = mx + mw * 0.5f + drawn(rs);
      if (sz.x - margin - lx >= sz.x * 0.3f) {
        draw_menu(mx, y0, y1, rs);
        draw_controls(fmaxf(lx, sz.x * 0.52f), y0, sz.x - margin, y1, rs);
      } else {
        for (int i = 0; i < MAX_ITEMS; i++) item_rects[i] = cw_rect_make(0, 0, 0, 0);
        draw_controls(margin, y0, sz.x - margin, y1, rs);
      }
    } else draw_menu(cx, y0, y1, rs);
    hint(HINT_SETTINGS, cx, foot, hs);
    break;
  }
  case CW_SHELL_JOIN: {
    text("Join", cx, sz.y * 0.12f, u * 1.6f, cw_rgba(1, 1, 1, 1), 1);
    float w = sz.x / 4.0f;
    for (int s = 0; s < S.d.max_players; s++) {
      float x = w * s + w * 0.5f, inner = w - u * 0.8f;
      bool in = S.joined_pad[s] >= 0;
      cw_r2d_rect(cw_rect_make(w * s + u * 0.4f, sz.y * 0.3f, inner, sz.y * 0.4f),
                  in ? cw_rgba(S.d.accent.r, S.d.accent.g, S.d.accent.b, 0.25f) : cw_rgba(1, 1, 1, 0.06f));
      char p[16];
      snprintf(p, sizeof p, "P%d", s + 1);
      float py = sz.y * 0.36f, ly = fmaxf(sz.y * 0.5f, py + drawn(u * 1.4f) * 1.25f);
      text(p, x, py, u * 1.4f, in ? S.d.accent : cw_rgba(1, 1, 1, 0.4f), 1);
      char dev[24];
      if (!in) {
        const char *m = fit_markup("{ui_accept} join", u, inner * 0.92f);
        cw_prompt_text(S.d.font, m, -1, cw_v2(x, ly - u * 0.1f), u, cw_rgba(1, 1, 1, 0.7f), CW_ALIGN_CENTER);
      } else {
        if (S.joined_pad[s] == CW_MAX_PADS) snprintf(dev, sizeof dev, "keyboard");
        else snprintf(dev, sizeof dev, "pad %d", S.joined_pad[s] + 1);
        text(dev, x, ly, u * 0.8f, cw_rgba(1, 1, 1, 0.9f), 1);
      }
    }
    hint(S.mid_join ? "{ui_accept} join    {ui_back} leave    {ui_start} back to the game" : "{ui_accept} join    {ui_back} leave    {ui_start} play",
         cx, sz.y - u * 1.2f, u * 0.8f);
    break;
  }
  default: break;
  }
  if (S.starting) {
    float d = drawn(u * 1.8f);
    cw_r2d_rect(cw_rect_make(0, 0, sz.x, sz.y), cw_rgba(0.02f, 0.02f, 0.05f, 0.6f));
    cw_r2d_rect(cw_rect_make(0, sz.y * 0.5f - d * 0.95f, sz.x, d * 1.9f), cw_rgba(0.02f, 0.02f, 0.05f, 0.9f));
    text("Starting...", cx, sz.y * 0.5f - d * 0.5f, u * 1.8f, S.d.accent, 1);
    S.starting_shown = true;
    S.dbg_starting = 2;
  }
  cw_r2d_end();
}

/* ── split screen ───────────────────────────────────────────────────── */

int cw_shell_split(cw_pipeline *p, const cw_node *cameras) {
  if (!p && cw_scene_app_tree()) p = cw_pipeline_of(cw_scene_app_tree());
  if (!p) return 0;
  if (S.players <= 1 || S.d.shared_screen) {
    cw_pipeline_set_views(p, NULL, 0);
    return 0;
  }
  return cw_pipeline_split(p, cameras, S.players);
}

/* the layout cw_pipeline_split makes, in canvas units */
static cw_rect quadrant(int i, cw_vec2 sz) {
  return cw_rect_make((i & 1) * sz.x * 0.5f, (i >> 1) * sz.y * 0.5f, sz.x * 0.5f, sz.y * 0.5f);
}
cw_rect cw_shell_player_rect(int player) {
  cw_vec2 sz = cw_r2d_canvas_size();
  if (S.players <= 1 || S.d.shared_screen || player < 0 || player >= S.players) return cw_rect_make(0, 0, sz.x, sz.y);
  if (S.players == 2) {
    if (sz.y > sz.x) return cw_rect_make(0, player * sz.y * 0.5f, sz.x, sz.y * 0.5f);
    return cw_rect_make(player * sz.x * 0.5f, 0, sz.x * 0.5f, sz.y);
  }
  return quadrant(player, sz);
}
cw_rect cw_shell_shared_rect(void) {
  if (S.players != 3 || S.d.shared_screen) return cw_rect_make(0, 0, 0, 0);
  return quadrant(3, cw_r2d_canvas_size());
}
