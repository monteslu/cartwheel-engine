/* The game shell (cartwheel/shell.h): loading, title, join, pause and
 * settings screens around a game's play, its saved settings and save data,
 * split screen views. */
#include <stdio.h>
#include <string.h>

#include "cartwheel/shell.h"
#include "cartwheel/core.h"
#include "cartwheel/debug.h"
#include "cartwheel/input.h"
#include "cartwheel/platform.h"
#include "cartwheel/quality.h"
#include "cartwheel/quality_menu.h"
#include "cartwheel/scene.h"
#include "cartwheel/scene_file.h"
#include "cartwheel/ui.h"

/* ── the save block ────────────────────────────────────────────────── */

#define SHELL_MAGIC 0x4c485357u /* "WSHL" */
#define SHELL_VERSION 1
typedef struct shell_save {
  uint32_t magic;
  uint16_t version, game_version;
  uint32_t game_size;
  uint32_t checksum; /* FNV-1a of everything after this field */
  uint8_t quality;   /* cw_quality_saved when it is a preset (1..4), else 0 (saves before gfx) */
  uint8_t music, sfx; /* volume steps 0..10 */
  uint8_t crisp;     /* cw_crisp_ui_saved */
  uint8_t gfx[4];    /* cw_quality_saved, little-endian (0: none, or a save from before it) */
  uint8_t reserved[8];
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

enum { A_UP, A_DOWN, A_LEFT, A_RIGHT, A_ACCEPT, A_BACK, A_PAUSE, A_COUNT };
static const char *const ACTION_NAMES[A_COUNT] = { "ui_up", "ui_down", "ui_left", "ui_right", "ui_accept", "ui_back", "pause" };
static const char *const ACTION_BINDS[A_COUNT] = {
  "key:up, key:w, pad:up, axis:ly-@0.5", "key:down, key:s, pad:down, axis:ly+@0.5",
  "key:left, key:a, pad:left, axis:lx-@0.5", "key:right, key:d, pad:right, axis:lx+@0.5",
  "key:return, key:space, pad:south", "key:escape, key:backspace, pad:east", "key:escape, key:p, pad:start",
};

static struct {
  cw_shell_desc d;
  cw_shell_screen screen, settings_from;
  int item;            /* the highlighted menu item */
  int players;
  int joined_pad[4];   /* join screen: each slot's pad (-1 none, CW_MAX_PADS = the keyboard) */
  float progress;
  cw_action act[A_COUNT];
  cw_bus music, sfx;
  int dbg_screen, dbg_players, dbg_item;
  bool inited, own_audio, hooked, just_resumed;
} S;

static void sound(cw_shell_sound s) {
  if (S.d.sound) S.d.sound(s, S.d.user);
}
static void go(cw_shell_screen to) {
  cw_shell_screen from = S.screen;
  S.screen = to;
  S.item = 0;
  if (to != CW_SHELL_SETTINGS) cw_quality_menu_close();
  S.dbg_screen = (int)to;
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
  SAVE.quality = q <= CW_QUALITY_COUNT ? (uint8_t)q : 0;
  for (int i = 0; i < 4; i++) SAVE.gfx[i] = (uint8_t)(q >> (8 * i));
  SAVE.crisp = cw_crisp_ui_saved();
  SAVE.checksum = save_sum();
  cw_save_commit();
}

void *cw_shell_game_save(void) { return SAVE.game; }

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
  S.inited = true;
  S.screen = S.d.loading ? CW_SHELL_LOADING : CW_SHELL_TITLE;
  S.dbg_screen = (int)S.screen;
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
int cw_shell_players(void) { return S.players; }
cw_bus cw_shell_music_bus(void) { return S.music; }
cw_bus cw_shell_sfx_bus(void) { return S.sfx; }
void cw_shell_loading(float progress) { S.progress = CW_CLAMP(progress, 0.0f, 1.0f); }
void cw_shell_loaded(void) {
  if (S.screen == CW_SHELL_LOADING) go(CW_SHELL_TITLE);
}

/* players: pads 0..n-1 in order, player 0 also on the keyboard, mouse and touch */
static void assign_default(int n) {
  for (int p = 0; p < 4; p++)
    cw_input_assign(p, p < n ? p : -1, p == 0 ? CW_DEVICE_KEYBOARD | CW_DEVICE_MOUSE | CW_DEVICE_TOUCH : 0);
}

static void begin_match(int n) {
  S.players = CW_CLAMP(n, 1, S.d.max_players);
  S.dbg_players = S.players;
  go(CW_SHELL_PLAY);
  if (S.d.start) S.d.start(S.players, S.d.user);
}

void cw_shell_start(int players) {
  assign_default(players);
  begin_match(players);
}

void cw_shell_to_title(void) {
  assign_default(4); /* menus answer every pad again */
  for (int i = 0; i < 4; i++) S.joined_pad[i] = -1;
  go(CW_SHELL_TITLE);
}

/* ── menus ──────────────────────────────────────────────────────────── */

/* any player's action this tick (before a match every pad is a player) */
static bool any_pressed(int a) {
  for (int p = 0; p < 4; p++)
    if (cw_action_just_pressed(p, S.act[a])) return true;
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
enum { SET_GRAPHICS, SET_MUSIC, SET_SFX, SET_CONTROLS, SET_BACK };
static void settings_items(void) {
  n_items = 0;
  items[n_items] = (item){ "Graphics", "" };
  snprintf(items[n_items++].value, sizeof items[0].value, "%s  (%s)  ...", cw_quality_label(), cw_render_res_label());
  items[n_items] = (item){ "Music", "" };
  snprintf(items[n_items++].value, sizeof items[0].value, "%d%%", SAVE.music * 10);
  items[n_items] = (item){ "Effects", "" };
  snprintf(items[n_items++].value, sizeof items[0].value, "%d%%", SAVE.sfx * 10);
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
  if (!in || !(in->mouse_pressed & 1)) return -1;
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
  default: return;
  }
  apply_volumes();
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
      } else cw_shell_start(1);
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

/* join: pads press South to join, East to leave; the keyboard joins with Enter */
static void join_update(void) {
  const cw_raw_input *in = cw_input_raw();
  for (int pad = 0; pad <= CW_MAX_PADS; pad++) {
    bool kb = pad == CW_MAX_PADS;
    bool south = kb ? (in && (in->keys_pressed[40] || in->keys_pressed[44])) /* return, space */
                    : (in && in->pads[pad].connected && (in->pads[pad].pressed & (1u << CW_PAD_SOUTH)));
    bool east = kb ? (in && in->keys_pressed[41]) /* escape */ : (in && (in->pads[pad].pressed & (1u << CW_PAD_EAST)));
    bool start = kb ? false : (in && (in->pads[pad].pressed & (1u << CW_PAD_START)));
    int slot = -1, n = 0;
    for (int s = 0; s < 4; s++) {
      if (S.joined_pad[s] == pad) slot = s;
      if (S.joined_pad[s] >= 0) n++;
    }
    if (slot < 0 && south) {
      for (int s = 0; s < S.d.max_players; s++)
        if (S.joined_pad[s] < 0) {
          S.joined_pad[s] = pad;
          sound(CW_SHELL_SND_JOIN);
          break;
        }
    } else if (slot >= 0 && east) {
      S.joined_pad[slot] = -1;
      sound(CW_SHELL_SND_BACK);
    } else if (slot < 0 && east && n == 0) {
      sound(CW_SHELL_SND_BACK);
      go(CW_SHELL_TITLE);
      return;
    } else if (slot >= 0 && (start || (south && slot == 0 && kb))) {
      /* go: players in slot order, each on its pad; the keyboard, mouse and
       * touch belong to the keyboard's player, else to player 1 */
      int p = 0, kb_slot = -1;
      for (int s = 0; s < 4; s++)
        if (S.joined_pad[s] == CW_MAX_PADS) kb_slot = s;
      for (int s = 0; s < 4; s++) {
        if (S.joined_pad[s] < 0) continue;
        bool k = S.joined_pad[s] == CW_MAX_PADS;
        bool devices = k || (kb_slot < 0 && p == 0);
        cw_input_assign(p++, k ? -1 : S.joined_pad[s], devices ? CW_DEVICE_KEYBOARD | CW_DEVICE_MOUSE | CW_DEVICE_TOUCH : 0);
      }
      for (int q = p; q < 4; q++) cw_input_assign(q, -1, 0);
      sound(CW_SHELL_SND_SELECT);
      begin_match(p);
      return;
    }
  }
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
  switch (S.screen) {
  case CW_SHELL_LOADING: return;
  case CW_SHELL_PLAY:
    for (int p = 0; p < S.players; p++)
      if (cw_action_just_pressed(p, S.act[A_PAUSE])) {
        sound(CW_SHELL_SND_PAUSE);
        go(CW_SHELL_PAUSE);
        return;
      }
    return;
  case CW_SHELL_JOIN: join_update(); return;
  default: break;
  }
  if (S.screen == CW_SHELL_SETTINGS && cw_quality_menu_is_open()) { /* the graphics page */
    int dy = any_pressed(A_DOWN) - any_pressed(A_UP), dx = any_pressed(A_RIGHT) - any_pressed(A_LEFT);
    bool acc = any_pressed(A_ACCEPT), bk = any_pressed(A_BACK);
    const cw_raw_input *in = cw_input_raw();
    bool tap = in && (in->mouse_pressed & 1) && cw_quality_menu_tap(cw_r2d_window_to_canvas(cw_v2(in->mouse_x, in->mouse_y)));
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
  else if (any_pressed(A_ACCEPT)) accept(S.item);
  else if (any_pressed(A_BACK) || (S.screen == CW_SHELL_PAUSE && any_pressed(A_PAUSE))) {
    if (S.screen == CW_SHELL_PAUSE) accept(0);
    else if (S.screen == CW_SHELL_SETTINGS) accept(SET_BACK);
  }
  S.dbg_item = S.item;
}

/* ── drawing ────────────────────────────────────────────────────────── */

static void text(const char *s, float x, float y, float size, cw_color c, int align /* 0 left 1 center 2 right */) {
  cw_text_style st = { .size = size, .color = c, .outline_width = size * 0.06f, .outline_color = cw_rgba(0, 0, 0, 0.8f) };
  if (align) {
    cw_text_metrics m = cw_font_measure(S.d.font, s, &st);
    x -= align == 1 ? m.width * 0.5f : m.width;
  }
  cw_r2d_text(S.d.font, s, cw_v2(x, y), &st);
}

static void draw_menu(float cx, float y, float size) {
  for (int i = 0; i < n_items; i++) {
    bool sel = i == S.item;
    cw_color c = sel ? S.d.accent : cw_rgba(1, 1, 1, 0.85f);
    char line[96];
    if (items[i].value[0]) snprintf(line, sizeof line, "%s   %s", items[i].label, items[i].value);
    else snprintf(line, sizeof line, "%s", items[i].label);
    text(line, cx, y, size, c, 1);
    item_rects[i] = cw_rect_make(cx - size * 8, y - size * 0.15f, size * 16, size * 1.3f);
    y += size * 1.5f;
  }
}

static void draw_controls(float x, float y, float size) {
  text("Controls", x, y, size, S.d.accent, 0);
  y += size * 1.6f;
  for (int a = 0, shown = 0; a < cw_action_count() && shown < 14; a++) {
    const char *name = cw_action_name(a);
    if (!name || !strncmp(name, "ui_", 3)) continue;
    cw_binding b[CW_INPUT_MAX_BINDINGS];
    int nb = cw_action_bindings(a, b, CW_INPUT_MAX_BINDINGS);
    char line[160] = "", one[48];
    for (int i = 0; i < nb && i < 3; i++) {
      cw_binding_format(b[i], one, sizeof one);
      snprintf(line + strlen(line), sizeof line - strlen(line), "%s%s", i ? ", " : "", one);
    }
    text(name, x, y, size * 0.8f, cw_rgba(1, 1, 1, 0.9f), 0);
    text(line, x + size * 7, y, size * 0.8f, cw_rgba(1, 1, 1, 0.7f), 0);
    y += size * 1.1f;
    shown++;
  }
}

void cw_shell_draw(void) {
  if (S.own_audio) cw_audio_update();
  if (!S.inited || S.screen == CW_SHELL_PLAY) return;
  cw_r2d_begin(NULL);
  cw_vec2 sz = cw_r2d_canvas_size();
  float u = sz.y / 20.0f; /* the menus scale with the canvas height */
  float cx = sz.x * 0.5f;
  if (S.d.draw_background) S.d.draw_background(S.screen, sz, S.d.user);
  cw_r2d_rect(cw_rect_make(0, 0, sz.x, sz.y), cw_rgba(0.02f, 0.02f, 0.05f, S.d.draw_background ? 0.3f : S.screen == CW_SHELL_PAUSE ? 0.55f : 0.85f));
  build_items();
  switch (S.screen) {
  case CW_SHELL_LOADING:
    text("Loading", cx, sz.y * 0.42f, u * 1.2f, cw_rgba(1, 1, 1, 1), 1);
    cw_r2d_rect(cw_rect_make(sz.x * 0.3f, sz.y * 0.55f, sz.x * 0.4f, u * 0.4f), cw_rgba(1, 1, 1, 0.2f));
    cw_r2d_rect(cw_rect_make(sz.x * 0.3f, sz.y * 0.55f, sz.x * 0.4f * S.progress, u * 0.4f), S.d.accent);
    break;
  case CW_SHELL_TITLE:
    text(S.d.title, cx, sz.y * 0.2f, u * 2.4f, cw_rgba(1, 1, 1, 1), 1);
    if (S.d.subtitle) text(S.d.subtitle, cx, sz.y * 0.2f + u * 2.8f, u * 0.9f, cw_rgba(1, 1, 1, 0.75f), 1);
    draw_menu(cx, sz.y * 0.5f, u * 1.1f);
    break;
  case CW_SHELL_PAUSE:
    text("Paused", cx, sz.y * 0.22f, u * 1.8f, cw_rgba(1, 1, 1, 1), 1);
    draw_menu(cx, sz.y * 0.45f, u * 1.1f);
    break;
  case CW_SHELL_SETTINGS:
    if (cw_quality_menu_is_open()) {
      cw_quality_menu_draw(&(cw_quality_menu_style){ .font = S.d.font, .accent = S.d.accent, .set_backdrop = true });
      break;
    }
    text("Settings", cx, sz.y * 0.12f, u * 1.6f, cw_rgba(1, 1, 1, 1), 1);
    draw_menu(S.item == SET_CONTROLS ? sz.x * 0.3f : cx, sz.y * 0.32f, u);
    if (S.item == SET_CONTROLS) draw_controls(sz.x * 0.52f, sz.y * 0.32f, u);
    text("left / right to change", cx, sz.y - u * 1.6f, u * 0.7f, cw_rgba(1, 1, 1, 0.6f), 1);
    break;
  case CW_SHELL_JOIN: {
    text("Join", cx, sz.y * 0.12f, u * 1.6f, cw_rgba(1, 1, 1, 1), 1);
    float w = sz.x / 4.0f;
    for (int s = 0; s < S.d.max_players; s++) {
      float x = w * s + w * 0.5f;
      bool in = S.joined_pad[s] >= 0;
      cw_r2d_rect(cw_rect_make(w * s + u * 0.4f, sz.y * 0.3f, w - u * 0.8f, sz.y * 0.4f),
                  in ? cw_rgba(S.d.accent.r, S.d.accent.g, S.d.accent.b, 0.25f) : cw_rgba(1, 1, 1, 0.06f));
      char p[16];
      snprintf(p, sizeof p, "P%d", s + 1);
      text(p, x, sz.y * 0.36f, u * 1.4f, in ? S.d.accent : cw_rgba(1, 1, 1, 0.4f), 1);
      char dev[24];
      if (!in) snprintf(dev, sizeof dev, "press A");
      else if (S.joined_pad[s] == CW_MAX_PADS) snprintf(dev, sizeof dev, "keyboard");
      else snprintf(dev, sizeof dev, "pad %d", S.joined_pad[s] + 1);
      text(dev, x, sz.y * 0.5f, u * 0.8f, cw_rgba(1, 1, 1, in ? 0.9f : 0.5f), 1);
    }
    text("A or Enter: join    B or Esc: leave    Start or Enter: play", cx, sz.y - u * 2.2f, u * 0.75f, cw_rgba(1, 1, 1, 0.7f), 1);
    break;
  }
  default: break;
  }
  cw_r2d_end();
}

/* ── split screen ───────────────────────────────────────────────────── */

int cw_shell_split(cw_pipeline *p, const cw_node *cameras) {
  if (!p && cw_scene_app_tree()) p = cw_pipeline_of(cw_scene_app_tree());
  if (!p) return 0;
  if (S.players <= 1) {
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
  if (S.players <= 1 || player < 0 || player >= S.players) return cw_rect_make(0, 0, sz.x, sz.y);
  if (S.players == 2) {
    if (sz.y > sz.x) return cw_rect_make(0, player * sz.y * 0.5f, sz.x, sz.y * 0.5f);
    return cw_rect_make(player * sz.x * 0.5f, 0, sz.x * 0.5f, sz.y);
  }
  return quadrant(player, sz);
}
cw_rect cw_shell_shared_rect(void) {
  if (S.players != 3) return cw_rect_make(0, 0, 0, 0);
  return quadrant(3, cw_r2d_canvas_size());
}
