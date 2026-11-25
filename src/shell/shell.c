/* The game shell (cartwheel/shell.h): loading, title, join, pause and
 * settings screens around a game's play, its saved settings and save data,
 * split screen views. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
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
#include "../render2d/text_raw.h"

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
  uint8_t shake;     /* the old Screen shake setting, kept so saves keep their layout; no longer applied (screen shake is gone) */
  uint8_t text;      /* cw_text_scale_saved: Text size and Text background (0: none chosen, also a save from before them) */
  uint8_t seats[3];  /* the join screen's hot seats and CPUs, three bits a seat for eight (0 none, 1 a hot seat, 2 + cw_shell_cpu_level); zeros in older saves */
  uint8_t reserved[3];
  uint8_t game[CW_SHELL_GAME_SAVE_MAX];
} shell_save;
static shell_save SAVE;

void *cw_shell_save_region(void) { return &SAVE; }
size_t cw_shell_save_region_size(void) { return sizeof SAVE; }

/* game_version of a save whose game data is a slot store (cw_shell_desc.game_save_slots) */
#define SLOTS_VERSION 0xC5A7u

/* The checksum covers the settings and the game's struct. A slot store is left out of it: every slot has its
 * own checksum (cartwheel/save.h), so a block the host captured between a slot write and the next
 * cw_shell_save_commit still loads, settings and slots both. */
static uint32_t save_sum(void) {
  const uint8_t *b = (const uint8_t *)&SAVE + offsetof(shell_save, quality);
  size_t end = SAVE.game_version == SLOTS_VERSION ? offsetof(shell_save, game) : sizeof SAVE;
  uint32_t h = 2166136261u;
  for (size_t i = 0; i < end - offsetof(shell_save, quality); i++) h = (h ^ b[i]) * 16777619u;
  return h;
}

/* ── state ──────────────────────────────────────────────────────────── */

#define SEATS CW_SHELL_MAX_SEATS
enum { KIND_NONE, KIND_HOT, KIND_CPU }; /* a join slot no pad holds: empty, a hot seat, a CPU (KIND_CPU + level) */

/* the saved join setup: three bits a seat */
static int saved_kind(int s) { return (int)((((uint32_t)SAVE.seats[0] | (uint32_t)SAVE.seats[1] << 8 | (uint32_t)SAVE.seats[2] << 16) >> (3 * s)) & 7u); }
static void save_kind(int s, int v) {
  uint32_t w = (uint32_t)SAVE.seats[0] | (uint32_t)SAVE.seats[1] << 8 | (uint32_t)SAVE.seats[2] << 16;
  w = (w & ~(7u << (3 * s))) | ((uint32_t)v & 7u) << (3 * s);
  for (int i = 0; i < 3; i++) SAVE.seats[i] = (uint8_t)(w >> (8 * i));
}

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
  int joined_pad[SEATS]; /* join screen: each slot's pad (-1 none, CW_MAX_PADS = the keyboard) */
  int seat_pad[SEATS];   /* the pads the players hold in a running match, in player order (-1 none, CW_MAX_PADS = the keyboard) */
  int join_kind[SEATS];  /* join screen: what holds a slot no pad holds (KIND_NONE, KIND_HOT, KIND_CPU + level) */
  int seat_cpu[SEATS];   /* the CPU playing each seat of a running match (-1 none, else its cw_shell_cpu_level) */
  int seat_hot[SEATS];   /* the seat is a hot seat: people taking turns on the pads no seat holds (cw_shell_set_turn) */
  int turn;              /* cw_shell_set_turn's seat (-1 none) */
  int join_cursor;       /* join screen with cpu_seats or hot_seats: the seat ui_up / ui_down change */
  int dbg_join_kind[SEATS];
  bool mid_join;       /* the join screen was opened during play (cw_shell_join_open) */
  int starting;        /* a Play press was answered: this many players, the game's start waits for that answer to be on screen */
  bool starting_shown; /* cw_shell_draw drew the answer */
  int starting_ticks;
  bool seat_seen[SEATS];   /* the seat's pad has been seen connected (an unplugged pad frees its seat; one never seen is not "gone") */
  float progress;
  cw_action act[A_COUNT];
  cw_bus music, sfx;
  int dbg_screen, dbg_hud, dbg_players, dbg_item, dbg_shake, dbg_starting, dbg_selection, dbg_two_colour;
  uint32_t dbg_ink, dbg_paper; /* 0xRRGGBB, the colours two-colour mode draws in (shell.ink, shell.paper) */
  bool plates;         /* cw_shell_draw, two colours over a picture (draw_background, or the pause menu over play): text sits on paper plates */
  bool inited, own_audio, hooked, just_resumed;
  bool resume_quiet;   /* the pause menu went back to play this tick: gameplay waits for the next tick, so the press is not the game's */
  const char *pause_label[2]; /* the game's pause items, read from the desc when the pause menu opens */
  int pause_index[2];  /* each shown item's index in cw_shell_desc.pause_items */
  int n_pause;         /* how many are shown (0..2), between Resume and Settings */
  cw_shell_setting set[CW_SHELL_SETTINGS_MAX]; /* the game's Settings rows (copied from the desc, or the setter) */
  int n_set, dbg_settings;
} S;

static void sound(cw_shell_sound s) {
  if (S.d.sound) S.d.sound(s, S.d.user);
}
static void route_turn(void);
static void route_hot(int to);
static bool any_hot(void);
/* a hot-seat match under the pause or Settings menus: the free pads and the keyboard drive the menus through every hot
 * seat (the menus answer any player); back in play they follow the turn again */
static void hot_menu_routing(cw_shell_screen from, cw_shell_screen to) {
  if (!any_hot() || to == CW_SHELL_TITLE || to == CW_SHELL_JOIN) return;
  if (to == CW_SHELL_PLAY) {
    if (from != CW_SHELL_PLAY) route_turn();
    return;
  }
  route_hot(-2); /* every hot seat */
}
static void go(cw_shell_screen to) {
  cw_shell_screen from = S.screen;
  S.screen = to;
  S.item = 0;
  if (to == CW_SHELL_PAUSE) { /* the game's items as the desc names them now (cw_shell_set_pause_items changes them) */
    S.n_pause = 0;
    for (int i = 0; i < 2; i++)
      if (S.d.pause_items[i] && S.d.pause_items[i][0]) {
        S.pause_label[S.n_pause] = S.d.pause_items[i];
        S.pause_index[S.n_pause++] = i;
      }
  }
  if (to != CW_SHELL_JOIN) S.mid_join = false;
  if (to != CW_SHELL_SETTINGS) cw_quality_menu_close();
  S.dbg_screen = (int)to;
  S.dbg_hud = to == CW_SHELL_PLAY;
  /* a scene app's tree (cw_scene_app) holds still outside play */
  cw_tree *t = cw_scene_app_tree();
  if (t) cw_tree_set_paused(t, to != CW_SHELL_PLAY);
  if (to == CW_SHELL_PLAY && from != CW_SHELL_PLAY && S.hooked) S.just_resumed = true;
  hot_menu_routing(from, to);
  if (from != to && S.d.screen_changed) S.d.screen_changed(from, to, S.d.user);
}
static void apply_volumes(void) {
  cw_bus_set_volume(S.music, SAVE.music / 10.0f);
  cw_bus_set_volume(S.sfx, SAVE.sfx / 10.0f);
}

static cw_save_store STORE;
static uint16_t want_game_version(void) { return S.d.game_save_slots ? SLOTS_VERSION : S.d.game_save_version; }
static uint32_t want_game_size(void) {
  if (S.d.game_save_slots) return CW_SHELL_GAME_SAVE_MAX;
  return (uint32_t)(S.d.game_save_size > CW_SHELL_GAME_SAVE_MAX ? CW_SHELL_GAME_SAVE_MAX : S.d.game_save_size);
}

static void save_reset(void) {
  memset(&SAVE, 0, sizeof SAVE);
  SAVE.magic = SHELL_MAGIC;
  SAVE.version = SHELL_VERSION;
  SAVE.game_version = want_game_version();
  SAVE.game_size = want_game_size();
  SAVE.music = 8;
  SAVE.sfx = 8;
  SAVE.checksum = save_sum();
}

/* The settings loaded; make the game's part of the save what this build of the game expects, keeping what it
 * can: a struct of another size keeps the bytes both have, another version goes through the game's hook, a
 * struct becomes slots through its import hook. */
static void game_save_adopt(void) {
  uint16_t old_version = SAVE.game_version, version = want_game_version();
  uint32_t old_size = SAVE.game_size > CW_SHELL_GAME_SAVE_MAX ? CW_SHELL_GAME_SAVE_MAX : SAVE.game_size, size = want_game_size();
  if (S.d.game_save_slots) {
    if (old_version != SLOTS_VERSION) {
      uint8_t *old = cw_alloc(old_size ? old_size : 1, CW_MEM_CORE);
      memcpy(old, SAVE.game, old_size);
      memset(SAVE.game, 0, sizeof SAVE.game);
      cw_save_open(&STORE, SAVE.game, sizeof SAVE.game, NULL, NULL);
      if (old_size && S.d.game_save_import) S.d.game_save_import(&STORE, old_version, old, old_size, S.d.user);
      else if (old_size) cw_warn("shell: the save holds game data from before the game used slots and there is no game_save_import: starting over");
      cw_free(old);
    } else cw_save_open(&STORE, SAVE.game, sizeof SAVE.game, NULL, NULL);
  } else if (old_version != version) {
    uint8_t *old = cw_alloc(old_size ? old_size : 1, CW_MEM_CORE);
    memcpy(old, SAVE.game, old_size);
    memset(SAVE.game, 0, sizeof SAVE.game);
    bool kept = old_size && S.d.game_save_migrate && S.d.game_save_migrate(old_version, old, old_size, SAVE.game, size, S.d.user);
    if (!kept) {
      memset(SAVE.game, 0, sizeof SAVE.game);
      if (old_size) cw_warn("shell: the game's save data is version %u, the game wants %u and %s: the game starts over (settings kept)",
                            old_version, version, S.d.game_save_migrate ? "its game_save_migrate refused it" : "has no game_save_migrate");
    }
    cw_free(old);
  } else if (old_size != size) {
    /* the same version, another size: the shared bytes stay, the rest is zeros */
    uint32_t keep = old_size < size ? old_size : size;
    memset(SAVE.game + keep, 0, sizeof SAVE.game - keep);
  }
  SAVE.game_version = version;
  SAVE.game_size = size;
  SAVE.checksum = save_sum();
}

cw_save_store *cw_shell_save_store(void) {
  if (!S.d.game_save_slots || !STORE.base) {
    cw_error("shell: cw_shell_save_store needs cw_shell_desc.game_save_slots = true (and cw_shell_init first)");
    return NULL;
  }
  return &STORE;
}

/* ── the game's Settings rows in the save ─────────────────────────── */

/* With a game struct the rows have a store of their own, in the last CW_SHELL_SETTINGS_SAVE_BYTES of the game area
 * (the struct stops before it); with slots they are slots of the game's store. */
static cw_save_store SET_STORE;
#define SET_TAIL (sizeof SAVE.game - CW_SHELL_SETTINGS_SAVE_BYTES)
static bool set_saved_any(const cw_shell_setting *items, int n) {
  for (int i = 0; i < n; i++)
    if (items[i].key && items[i].key[0]) return true;
  return false;
}
static cw_save_store *set_store(void) {
  if (S.d.game_save_slots) return STORE.base ? &STORE : NULL;
  if (!SET_STORE.base && set_saved_any(S.set, S.n_set)) {
    if (S.d.game_save_size > SET_TAIL) {
      static bool told;
      if (!told) cw_error("shell: game_save_size %zu leaves no room for the Settings rows' save (at most %d bytes with saved rows): "
                          "they are not saved", S.d.game_save_size, (int)SET_TAIL);
      told = true;
      return NULL;
    }
    cw_save_open(&SET_STORE, SAVE.game + SET_TAIL, CW_SHELL_SETTINGS_SAVE_BYTES, NULL, NULL);
  }
  return SET_STORE.base ? &SET_STORE : NULL;
}
static bool set_slot_name(const cw_shell_setting *it, char *name, size_t cap) {
  size_t n = it->key ? strlen(it->key) : 0;
  if (!n) return false;
  if (n > CW_SHELL_SETTING_KEY_MAX) {
    cw_error("shell: the Settings row key \"%s\" is over %d bytes: not saved", it->key, CW_SHELL_SETTING_KEY_MAX);
    return false;
  }
  snprintf(name, cap, "shell:%s", it->key);
  return true;
}
enum { SET_SLOT_VERSION = 1, SET_SLOT_MAX = 4 + 64 }; /* a slot: the index (int32 little-endian), then the choice's label */
static size_t set_slot_bytes(const cw_shell_setting *it, uint8_t *b) {
  int32_t v = *it->value;
  for (int k = 0; k < 4; k++) b[k] = (uint8_t)((uint32_t)v >> (8 * k));
  size_t n = 4;
  if (v >= 0 && v < it->count && it->choices && it->choices[v]) {
    size_t l = strlen(it->choices[v]);
    if (l > SET_SLOT_MAX - 4) l = SET_SLOT_MAX - 4;
    memcpy(b + 4, it->choices[v], l);
    n += l;
  }
  return n;
}
/* the saved choice into *value: by its label, else its index, else the default stays */
static void set_load(int i) {
  cw_shell_setting *it = &S.set[i];
  char name[72];
  cw_save_store *st = set_store();
  if (!st || !set_slot_name(it, name, sizeof name)) return;
  uint16_t ver;
  size_t size;
  if (!cw_save_info(st, name, &ver, &size)) return; /* a new row: its default */
  uint8_t b[SET_SLOT_MAX] = { 0 };
  if (size > sizeof b) size = sizeof b;
  if (size < 4 || cw_save_read(st, name, SET_SLOT_VERSION, b, size, NULL, NULL) != CW_SAVE_OK) {
    cw_warn("shell: the saved Settings row \"%s\" is unreadable: its default", it->key);
    return;
  }
  int32_t v = (int32_t)((uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24);
  size_t l = size - 4;
  for (int c = 0; l && c < it->count; c++)
    if (it->choices[c] && strlen(it->choices[c]) == l && !memcmp(it->choices[c], b + 4, l)) {
      *it->value = c;
      return;
    }
  if (v >= 0 && v < it->count) *it->value = v;
  else cw_warn("shell: the saved Settings row \"%s\" holds %d, out of its %d choices: its default", it->key, (int)v, it->count);
}
/* each saved row whose slot differs from *value (unchanged slots are not rewritten) */
static void set_save_all(void) {
  if (!S.n_set) return;
  static int test_no_save = -1;
  if (test_no_save < 0) test_no_save = getenv("CW_TEST_SHELL_SETTINGS_NO_SAVE") && atoi(getenv("CW_TEST_SHELL_SETTINGS_NO_SAVE")) > 0;
  if (test_no_save) return; /* the persistence test's control: the rows never reach the save */
  cw_save_store *st = set_store();
  if (!st) return;
  for (int i = 0; i < S.n_set; i++) {
    char name[72];
    if (!set_slot_name(&S.set[i], name, sizeof name)) continue;
    uint8_t now[SET_SLOT_MAX], was[SET_SLOT_MAX];
    size_t n = set_slot_bytes(&S.set[i], now), size;
    uint16_t ver;
    if (cw_save_info(st, name, &ver, &size) && ver == SET_SLOT_VERSION && size == n &&
        cw_save_read(st, name, SET_SLOT_VERSION, was, n, NULL, NULL) == CW_SAVE_OK && !memcmp(was, now, n)) continue;
    cw_save_write(st, name, SET_SLOT_VERSION, now, n);
  }
}
/* the rows (copied), each *value checked against its choices */
static void set_items(const cw_shell_setting *items, int count, bool load_all) {
  if (count < 0 || !items) count = 0;
  if (count > CW_SHELL_SETTINGS_MAX) {
    cw_error("shell: %d Settings rows, at most %d: the first %d are kept", count, CW_SHELL_SETTINGS_MAX, CW_SHELL_SETTINGS_MAX);
    count = CW_SHELL_SETTINGS_MAX;
  }
  cw_shell_setting old[CW_SHELL_SETTINGS_MAX];
  int n_old = S.n_set;
  memcpy(old, S.set, sizeof old);
  S.n_set = 0;
  for (int i = 0; i < count; i++) {
    const cw_shell_setting *it = &items[i];
    if (!it->value || !it->choices || it->count < 1) {
      cw_error("shell: Settings row %d (\"%s\") needs value, choices and a count of at least 1: left out", i, it->label ? it->label : "");
      continue;
    }
    if (*it->value < 0 || *it->value >= it->count) {
      cw_error("shell: Settings row \"%s\" starts at %d, out of its %d choices: 0", it->label ? it->label : "", *it->value, it->count);
      *it->value = 0;
    }
    S.set[S.n_set++] = *it;
  }
  S.dbg_settings = S.n_set;
  for (int i = 0; i < S.n_set; i++) {
    bool known = false;
    for (int k = 0; k < n_old && !load_all; k++)
      known |= old[k].key && S.set[i].key && !strcmp(old[k].key, S.set[i].key);
    if (!known) set_load(i);
  }
}
void cw_shell_set_settings_items(const cw_shell_setting *items, int count) { set_items(items, count, false); }

void cw_shell_save_commit(void) {
  set_save_all();
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
/* screen shake is gone: no camera shakes, at any setting (a hit or a blast is felt in the pads, cw_input_rumble) */
float cw_shell_screen_shake(void) { return 0.0f; }
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
  if (S.d.max_players > SEATS) S.d.max_players = SEATS;
  if (!S.d.font.id) S.d.font = cw_ui_font_sans();
  if (S.d.accent.a == 0) S.d.accent = cw_rgba(1.0f, 0.78f, 0.25f, 1);
  cw_shell_set_selection(S.d.selection);
  if (S.d.selection_text.a == 0) S.d.selection_text = cw_rgba(0.02f, 0.02f, 0.05f, 1);
  cw_shell_set_colours(S.d.accent, S.d.selection_text, S.d.ink, S.d.paper);
  cw_shell_set_two_colour(S.d.two_colour);
  if (S.d.game_save_size > CW_SHELL_GAME_SAVE_MAX)
    cw_error("shell: game_save_size %zu is over CW_SHELL_GAME_SAVE_MAX (%d): the save keeps the first %d bytes",
             S.d.game_save_size, CW_SHELL_GAME_SAVE_MAX, CW_SHELL_GAME_SAVE_MAX);
  STORE = (cw_save_store){ 0 };
  if (SAVE.magic != SHELL_MAGIC || SAVE.version != SHELL_VERSION || SAVE.checksum != save_sum()) {
    if (SAVE.magic) cw_warn("shell: the save is from another shell version or damaged: starting with defaults");
    save_reset();
  }
  {
    /* the game's Settings rows ride in the last bytes of the game area when the game keeps a struct: a struct of
     * another size or version (zeros after it, or a migration) keeps them, unless the old struct reached into them.
     * A game that never had rows has zeros there, which stay zeros. */
    static uint8_t tail[CW_SHELL_SETTINGS_SAVE_BYTES];
    bool keep = !S.d.game_save_slots && SAVE.game_version != SLOTS_VERSION && SAVE.game_size <= SET_TAIL;
    if (keep) memcpy(tail, SAVE.game + SET_TAIL, sizeof tail);
    game_save_adopt();
    if (keep && S.d.game_save_size <= SET_TAIL && memcmp(tail, SAVE.game + SET_TAIL, sizeof tail)) {
      memcpy(SAVE.game + SET_TAIL, tail, sizeof tail);
      SAVE.checksum = save_sum();
    }
  }
  SET_STORE = (cw_save_store){ 0 };
  set_items(S.d.settings_items, S.d.settings_items ? S.d.settings_item_count : 0, true);
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
  for (int i = 0; i < SEATS; i++) S.joined_pad[i] = -1;
  cw_debug_field("shell.screen", &S.dbg_screen, CW_DBG_I32, 1);
  cw_debug_field("shell.selection", &S.dbg_selection, CW_DBG_I32, 1);
  cw_debug_field("shell.two_colour", &S.dbg_two_colour, CW_DBG_I32, 1);
  cw_debug_field("shell.ink", &S.dbg_ink, CW_DBG_U32, 1);
  cw_debug_field("shell.paper", &S.dbg_paper, CW_DBG_U32, 1);
  cw_debug_field("shell.players", &S.dbg_players, CW_DBG_I32, 1);
  cw_debug_field("shell.item", &S.dbg_item, CW_DBG_I32, 1);
  S.dbg_shake = cw_shell_shake_level();
  cw_debug_field("shell.shake", &S.dbg_shake, CW_DBG_I32, 1);
  cw_debug_field("shell.starting", &S.dbg_starting, CW_DBG_I32, 1);
  cw_debug_field("shell.hud", &S.dbg_hud, CW_DBG_I32, 1);
  cw_debug_field("shell.settings", &S.dbg_settings, CW_DBG_I32, 1);
  /* CPU seats: each seat's CPU in the match (-1 none, else its level), on the join screen (0 none, 1 + level), the cursor */
  for (int i = 0; i < SEATS; i++) S.seat_cpu[i] = -1, S.seat_hot[i] = 0;
  S.turn = -1;
  cw_debug_field("shell.seat_cpu", S.seat_cpu, CW_DBG_I32, SEATS);
  cw_debug_field("shell.seat_hot", S.seat_hot, CW_DBG_I32, SEATS);
  cw_debug_field("shell.turn", &S.turn, CW_DBG_I32, 1);
  cw_debug_field("shell.join_kind", S.dbg_join_kind, CW_DBG_I32, SEATS);
  cw_debug_field("shell.join_cursor", &S.join_cursor, CW_DBG_I32, 1);
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
  S.turn = -1;
  for (int p = 0; p < SEATS; p++) {
    bool pad = p < n && p < CW_MAX_PADS;
    cw_input_assign(p, pad ? p : -1, p == 0 ? CW_DEVICE_KEYBOARD | CW_DEVICE_MOUSE | CW_DEVICE_TOUCH : 0);
    S.seat_pad[p] = pad ? p : -1;
    S.seat_cpu[p] = -1;
    S.seat_hot[p] = 0;
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
  for (int i = 0; i < SEATS; i++) {
    S.joined_pad[i] = i < S.players ? S.seat_pad[i] : -1;
    S.join_kind[i] = i >= S.players ? KIND_NONE : S.seat_cpu[i] >= 0 ? KIND_CPU + S.seat_cpu[i] : S.seat_hot[i] ? KIND_HOT : KIND_NONE;
  }
  S.join_cursor = 0;
  S.mid_join = true;
  go(CW_SHELL_JOIN);
  return true;
}

bool cw_shell_seat_taken(int seat) {
  return S.inited && seat >= 0 && seat < S.players && (S.seat_pad[seat] >= 0 || S.seat_cpu[seat] >= 0 || S.seat_hot[seat]);
}

bool cw_shell_seat_hot(int seat) { return S.inited && seat >= 0 && seat < S.players && S.seat_hot[seat]; }
static bool any_hot(void) {
  for (int s = 0; s < S.players; s++)
    if (S.seat_hot[s]) return true;
  return false;
}

/* The pads no seat holds, and the keyboard, mouse and touch when no seat holds them, go to the hot seat whose turn it
 * is; every other hot seat reads nothing. CW_TEST_TURN_WRONG_SEAT (a gate control) hands them to the next seat. */
static void route_turn(void) {
  int to = S.turn;
  if (to >= 0 && S.players > 0 && cw_test_env("CW_TEST_TURN_WRONG_SEAT")) to = (to + 1) % S.players;
  route_hot(to);
}
/* the free devices to hot seat `to` (-2: to every hot seat, under the menus; -1: to none) */
static void route_hot(int to) {
  uint32_t held = 0;
  bool kb_held = false;
  for (int s = 0; s < S.players; s++) {
    if (S.seat_pad[s] >= 0 && S.seat_pad[s] < CW_MAX_PADS) held |= 1u << S.seat_pad[s];
    if (S.seat_pad[s] >= 0 && (cw_input_player_devices(s) & CW_DEVICE_KEYBOARD)) kb_held = true;
  }
  for (int s = 0; s < S.players; s++)
    if (S.seat_hot[s])
      cw_input_assign_pads(s, s == to || to == -2 ? ((1u << CW_MAX_PADS) - 1) & ~held : 0,
                           (s == to || to == -2) && !kb_held ? CW_DEVICE_KEYBOARD | CW_DEVICE_MOUSE | CW_DEVICE_TOUCH : 0);
}

void cw_shell_set_turn(int seat) {
  if (!S.inited) return;
  if (seat < 0 || seat >= S.players) seat = -1;
  if (seat == S.turn) return;
  S.turn = seat;
  route_turn();
}
int cw_shell_turn(void) { return S.inited ? S.turn : -1; }

bool cw_shell_seat_cpu(int seat, cw_shell_cpu_level *level) {
  if (!S.inited || seat < 0 || seat >= S.players || seat >= SEATS || S.seat_cpu[seat] < 0) return false;
  if (level) *level = (cw_shell_cpu_level)S.seat_cpu[seat];
  return true;
}

const char *cw_shell_cpu_level_label(cw_shell_cpu_level level) {
  static const char *const L[CW_SHELL_CPU_LEVELS] = { "Easy", "Normal", "Hard" };
  return (int)level >= 0 && level < CW_SHELL_CPU_LEVELS ? L[level] : "?";
}

void cw_shell_start_seats(int seats, const int *cpu) {
  int n = CW_CLAMP(seats, 1, S.d.max_players > 0 ? S.d.max_players : 1);
  int pad = 0, first_person = -1;
  S.turn = -1;
  for (int s = 0; s < SEATS; s++) {
    bool is_cpu = s < n && cpu && cpu[s] >= 0, hot = s < n && cpu && cpu[s] == CW_SHELL_SEAT_HOT;
    S.seat_cpu[s] = is_cpu ? CW_CLAMP(cpu[s], 0, CW_SHELL_CPU_LEVELS - 1) : -1;
    S.seat_hot[s] = hot;
    S.seat_pad[s] = s < n && !is_cpu && !hot && pad < CW_MAX_PADS ? pad++ : -1;
    S.seat_seen[s] = false;
    if (S.seat_pad[s] >= 0 && first_person < 0) first_person = s;
    cw_input_assign(s, S.seat_pad[s], s == first_person ? CW_DEVICE_KEYBOARD | CW_DEVICE_MOUSE | CW_DEVICE_TOUCH : 0);
  }
  begin_match(n);
}

bool cw_shell_join_requested(void) {
  if (!S.inited || S.screen != CW_SHELL_PLAY || S.d.max_players < 2 || any_hot()) return false; /* free pads drive the hot seats */
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
  if (!S.inited || S.screen != CW_SHELL_PLAY || S.d.max_players < 2 || any_hot()) return false; /* free pads drive the hot seats */
  /* pads only: the keyboard usually drives a seated player already (Enter there is that player's own key) */
  for (int pad = 0; pad < CW_MAX_PADS; pad++) {
    bool seated = false;
    for (int p = 0; p < S.players; p++) seated |= S.seat_pad[p] == pad;
    if (seated || !(cw__input_pad_pressed(pad) & (1u << CW_PAD_SOUTH))) continue;
    int seat = -1;
    for (int s = 0; s < S.d.max_players && s < SEATS; s++)
      if (s >= S.players || (S.seat_pad[s] < 0 && S.seat_cpu[s] < 0)) { seat = s; break; }
    for (int s = 0; s < S.players && seat < 0; s++) /* every seat taken: a person takes a CPU's */
      if (S.seat_cpu[s] >= 0) seat = s;
    if (seat < 0) return false;
    S.seat_pad[seat] = pad;
    S.seat_cpu[seat] = -1;
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

void cw_shell_set_selection(cw_shell_selection style) {
  S.d.selection = (int)style >= 0 && style < CW_SHELL_SELECT_COUNT ? style : CW_SHELL_SELECT_COLOUR;
  S.dbg_selection = (int)S.d.selection;
}
cw_shell_selection cw_shell_selection_style(void) { return S.d.selection; }

static uint32_t rgb24(cw_color c) {
  int r = (int)lroundf(CW_CLAMP(c.r, 0.0f, 1.0f) * 255), g = (int)lroundf(CW_CLAMP(c.g, 0.0f, 1.0f) * 255), b = (int)lroundf(CW_CLAMP(c.b, 0.0f, 1.0f) * 255);
  return (uint32_t)(r << 16 | g << 8 | b);
}
void cw_shell_set_colours(cw_color accent, cw_color selection_text, cw_color ink, cw_color paper) {
  S.d.accent = accent.a > 0 ? accent : cw_rgba(1.0f, 0.78f, 0.25f, 1);
  S.d.selection_text = selection_text.a > 0 ? selection_text : cw_rgba(0.02f, 0.02f, 0.05f, 1);
  S.d.ink = ink.a > 0 ? cw_rgba(ink.r, ink.g, ink.b, 1) : cw_rgba(1, 1, 1, 1);
  S.d.paper = paper.a > 0 ? cw_rgba(paper.r, paper.g, paper.b, 1) : cw_rgba(0.02f, 0.02f, 0.05f, 1);
  S.dbg_ink = rgb24(S.d.ink), S.dbg_paper = rgb24(S.d.paper);
}
void cw_shell_set_two_colour(bool on) {
  S.d.two_colour = on;
  S.dbg_two_colour = on;
}
bool cw_shell_two_colour(void) { return S.d.two_colour; }

void cw_shell_set_pause_items(const char *const items[2]) {
  for (int i = 0; i < 2; i++) S.d.pause_items[i] = items ? items[i] : NULL;
}

void cw_shell_to_title(void) {
  assign_default(4); /* menus answer every pad again */
  for (int i = 0; i < SEATS; i++) S.joined_pad[i] = -1;
  S.starting = S.dbg_starting = 0;
  go(CW_SHELL_TITLE);
}

/* the join screen opens from the title: nobody seated, the CPUs as the player left them last time */
static void join_fresh(void) {
  for (int k = 0; k < SEATS; k++) {
    int v = k < S.d.max_players ? saved_kind(k) : KIND_NONE;
    if ((v == KIND_HOT && !S.d.hot_seats) || (v >= KIND_CPU && (!S.d.cpu_seats || v >= KIND_CPU + CW_SHELL_CPU_LEVELS))) v = KIND_NONE;
    S.joined_pad[k] = -1;
    S.join_kind[k] = v;
  }
  S.join_cursor = 0;
}

/* ── menus ──────────────────────────────────────────────────────────── */

/* any player's action this tick (before a match every pad is a player) */
static bool any_pressed(int a) {
  for (int p = 0; p < SEATS; p++)
    if (cw_action_just_pressed(p, S.act[a])) return true;
  return false;
}
static bool any_pad_start(void) {
  for (int pad = 0; pad < CW_MAX_PADS; pad++)
    if (cw__input_pad_pressed(pad) & (1u << CW_PAD_START)) return true;
  return false;
}

typedef struct { const char *label; char value[48]; } item;
enum { MAX_ITEMS = 16 }; /* the settings page: the shell's seven rows and CW_SHELL_SETTINGS_MAX of the game's */
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
  for (int i = 0; i < S.n_pause; i++) items[n_items++] = (item){ S.pause_label[i], "" };
  items[n_items++] = (item){ "Settings", "" };
  items[n_items++] = (item){ "Quit to title", "" };
}
/* no Screen shake row: nothing shakes. The game's rows (S.set) follow Controls, then Back */
enum { SET_GRAPHICS, SET_MUSIC, SET_SFX, SET_TEXT, SET_TEXT_BG, SET_CONTROLS, SET_GAME };
static int set_back(void) { return SET_GAME + S.n_set; }
static void settings_items(void) {
  n_items = 0;
  items[n_items] = (item){ "Graphics", "" };
  snprintf(items[n_items++].value, sizeof items[0].value, "%s  (%s)  ...", cw_quality_label(), cw_render_res_label());
  items[n_items] = (item){ "Music", "" };
  snprintf(items[n_items++].value, sizeof items[0].value, "%d%%", SAVE.music * 10);
  items[n_items] = (item){ "Effects", "" };
  snprintf(items[n_items++].value, sizeof items[0].value, "%d%%", SAVE.sfx * 10);
  items[n_items] = (item){ "Text size", "" };
  snprintf(items[n_items++].value, sizeof items[0].value, "%s", cw_text_scale_label());
  items[n_items] = (item){ "Text background", "" };
  snprintf(items[n_items++].value, sizeof items[0].value, "%s", cw_text_backdrop_label());
  items[n_items++] = (item){ "Controls", "" };
  for (int i = 0; i < S.n_set; i++) {
    const cw_shell_setting *it = &S.set[i];
    int v = *it->value;
    items[n_items] = (item){ it->label ? it->label : "", "" };
    snprintf(items[n_items++].value, sizeof items[0].value, "%s", v >= 0 && v < it->count && it->choices[v] ? it->choices[v] : "?");
  }
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
  float tx, ty;
  if (!cw__input_tap(&tx, &ty)) return -1;
  cw_vec2 p = cw_r2d_window_to_canvas(cw_v2(tx, ty));
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
  case SET_TEXT: /* 100, 125, 150, 200%: left / right step it (stopping at the ends), accept goes round */
    if (dir < 0 && cw_text_scale() > 1.01f) cw_text_scale_cycle_back();
    else if ((dir > 0 && cw_text_scale() < 1.99f) || !dir) cw_text_scale_cycle();
    break;
  case SET_TEXT_BG: { /* Off, Outline, Panel: the same */
    int b = (int)cw_text_backdrop_get();
    cw_text_backdrop_set((cw_text_backdrop)(!dir ? (b + 1) % 3 : CW_CLAMP(b + dir, 0, 2)));
    break;
  }
  default: {
    int g = S.item - SET_GAME;
    if (g < 0 || g >= S.n_set) return;
    /* the game's row: left / right step and stop at the ends, accept steps forward and goes round (as Text size) */
    cw_shell_setting *it = &S.set[g];
    int was = CW_CLAMP(*it->value, 0, it->count - 1), now = !dir ? (was + 1) % it->count : CW_CLAMP(was + dir, 0, it->count - 1);
    bool changed = now != *it->value;
    *it->value = now;
    if (changed && S.d.settings_changed) S.d.settings_changed(g, now, S.d.user);
    break;
  }
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
        join_fresh();
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
    /* Resume, the game's items (1 .. n_pause), Settings, Quit to title */
    sound(i <= S.n_pause ? CW_SHELL_SND_BACK : CW_SHELL_SND_SELECT);
    if (i <= S.n_pause) { /* back to play, as Resume; a game item then tells the game, which switches its own screen */
      go(CW_SHELL_PLAY);
      S.resume_quiet = true;
      if (i > 0 && S.d.pause_item) S.d.pause_item(S.pause_index[i - 1], S.d.user);
    } else if (i == S.n_pause + 1) {
      S.settings_from = CW_SHELL_PAUSE;
      go(CW_SHELL_SETTINGS);
    } else {
      if (S.d.quit) S.d.quit(S.d.user);
      cw_shell_to_title();
    }
    break;
  case CW_SHELL_SETTINGS:
    if (i == set_back()) {
      sound(CW_SHELL_SND_BACK);
      cw_shell_screen back = S.settings_from;
      go(back);
      S.item = back == CW_SHELL_PAUSE ? S.n_pause + 1 : 1; /* Settings, in the title and pause menus */
    } else if (i == SET_CONTROLS) {
      sound(CW_SHELL_SND_SELECT); /* the list shows beside the menu while Controls is highlighted */
    } else settings_change(0);
    break;
  default: break;
  }
}

static cw_rect join_seat_rect(int seat, cw_vec2 sz); /* the drawing's, below: a tap on a seat */

/* go: every press to play ends here. At the start of a match players take the pads in slot order (a gap closes; a CPU
 * or hot seat keeps its place in that order); in the middle of one every seat keeps its number, a seat nobody holds
 * reads no input. The keyboard, mouse and touch belong to the keyboard's player, else to the first person on a pad, else
 * they follow the turn with the free pads (hot seats) */
static void join_go(void) {
  int p = 0, kb_slot = -1, last = -1, first_person = -1;
  for (int s = 0; s < SEATS; s++) {
    if (S.joined_pad[s] == CW_MAX_PADS) kb_slot = s;
    if (S.joined_pad[s] >= 0 && first_person < 0) first_person = s;
    if (S.joined_pad[s] >= 0 || S.join_kind[s] != KIND_NONE) last = s;
  }
  if ((S.d.cpu_seats || S.d.hot_seats) && !S.mid_join) { /* the hot seats and CPUs the player set, for next time */
    bool changed = false;
    for (int s = 0; s < SEATS; s++) {
      int v = S.joined_pad[s] < 0 ? S.join_kind[s] : KIND_NONE;
      if (saved_kind(s) != v) save_kind(s, v), changed = true;
    }
    if (changed) cw_shell_save_commit();
  }
  S.turn = -1;
  if (S.mid_join) {
    for (int s = 0; s < SEATS; s++) {
      int k = S.joined_pad[s] < 0 ? S.join_kind[s] : KIND_NONE;
      S.seat_pad[s] = S.joined_pad[s];
      S.seat_cpu[s] = k >= KIND_CPU ? k - KIND_CPU : -1;
      S.seat_hot[s] = k == KIND_HOT;
      S.seat_seen[s] = false;
      bool kb = S.joined_pad[s] == CW_MAX_PADS;
      bool devices = S.joined_pad[s] >= 0 && (kb || (kb_slot < 0 && s == first_person));
      cw_input_assign(s, S.joined_pad[s] < 0 || kb ? -1 : S.joined_pad[s], devices ? CW_DEVICE_KEYBOARD | CW_DEVICE_MOUSE | CW_DEVICE_TOUCH : 0);
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
  bool kb_given = false;
  for (int s = 0; s < SEATS; s++) {
    bool person = S.joined_pad[s] >= 0;
    int k = person ? KIND_NONE : S.join_kind[s];
    if (!person && k == KIND_NONE) continue;
    bool kb = S.joined_pad[s] == CW_MAX_PADS;
    bool devices = person && (kb || (kb_slot < 0 && !kb_given));
    kb_given |= devices;
    S.seat_pad[p] = person ? S.joined_pad[s] : -1;
    S.seat_cpu[p] = k >= KIND_CPU ? k - KIND_CPU : -1;
    S.seat_hot[p] = k == KIND_HOT;
    S.seat_seen[p] = false;
    cw_input_assign(p++, !person || kb ? -1 : S.joined_pad[s], devices ? CW_DEVICE_KEYBOARD | CW_DEVICE_MOUSE | CW_DEVICE_TOUCH : 0);
  }
  for (int q = p; q < SEATS; q++) cw_input_assign(q, -1, 0), S.seat_pad[q] = -1, S.seat_cpu[q] = -1, S.seat_hot[q] = 0;
  begin_starting(p);
}

static int empty_seat(void) {
  for (int s = 0; s < S.d.max_players; s++)
    if (S.joined_pad[s] < 0 && S.join_kind[s] == KIND_NONE) return s;
  return -1;
}
/* a seat nobody holds, else the first a CPU holds: a person who joins takes a CPU's seat (never a hot seat's: the
 * free pads already play those) */
static int free_seat(void) {
  int s = empty_seat();
  if (s >= 0) return s;
  for (s = 0; s < S.d.max_players; s++)
    if (S.joined_pad[s] < 0 && S.join_kind[s] >= KIND_CPU) return s;
  return -1;
}
static bool join_has_hot(void) {
  for (int s = 0; s < S.d.max_players; s++)
    if (S.joined_pad[s] < 0 && S.join_kind[s] == KIND_HOT) return true;
  return false;
}

/* step what holds a seat: empty, a hot seat (hot_seats), CPU Easy, Normal, Hard (cpu_seats), and round (dir +1), or
 * back (-1); a person's seat stays theirs */
static void step_kind(int seat, int dir) {
  if (seat < 0 || seat >= S.d.max_players || S.joined_pad[seat] >= 0) return;
  int order[2 + CW_SHELL_CPU_LEVELS], n = 0, at = 0;
  order[n++] = KIND_NONE;
  if (S.d.hot_seats) order[n++] = KIND_HOT;
  if (S.d.cpu_seats)
    for (int l = 0; l < CW_SHELL_CPU_LEVELS; l++) order[n++] = KIND_CPU + l;
  for (int i = 0; i < n; i++)
    if (order[i] == S.join_kind[seat]) at = i;
  S.join_kind[seat] = order[(at + dir + n) % n];
  sound(CW_SHELL_SND_MOVE);
}

/* join: a pad joins with South and leaves with East; the keyboard joins with Enter and leaves with Esc. Start on any
 * pad plays, seating that pad first if it has no seat and one is free (no seat free: the seated players play). Enter
 * on a keyboard that holds a seat plays, and with every seat taken by others. With cpu_seats, left / right move the
 * seat cursor and up / down step its CPU (any player's ui actions: before a match every pad is a player). */
static void join_update(void) {
  bool play = false;
  for (int s = 0; s < SEATS; s++) S.dbg_join_kind[s] = S.joined_pad[s] < 0 ? S.join_kind[s] : 0;
  if (S.d.cpu_seats || S.d.hot_seats) {
    int dx = any_pressed(A_RIGHT) - any_pressed(A_LEFT), dy = any_pressed(A_UP) - any_pressed(A_DOWN);
    if (dx) {
      S.join_cursor = (S.join_cursor + dx + S.d.max_players) % S.d.max_players;
      sound(CW_SHELL_SND_MOVE);
    }
    if (dy) step_kind(S.join_cursor, dy);
  }
  /* a tap (the mouse, or a finger on a touch screen): with cpu_seats a tap on a seat no person holds steps its CPU;
   * any other tap seats the keyboard's player (who holds the mouse and touch) if it has no seat, and the game starts
   * (in a match, goes back to it) */
  float tx, ty;
  if (cw__input_tap(&tx, &ty)) {
    if (S.d.cpu_seats || S.d.hot_seats) {
      cw_vec2 c = cw_r2d_window_to_canvas(cw_v2(tx, ty));
      cw_vec2 sz = cw_r2d_canvas_size();
      for (int s = 0; s < S.d.max_players; s++) {
        cw_rect r = join_seat_rect(s, sz);
        if (c.x >= r.x && c.x < r.x + r.w && c.y >= r.y && c.y < r.y + r.h && S.joined_pad[s] < 0) {
          S.join_cursor = s;
          step_kind(s, 1);
          return;
        }
      }
    }
    bool seated = false;
    for (int s = 0; s < SEATS; s++) seated |= S.joined_pad[s] == CW_MAX_PADS;
    int s = seated || join_has_hot() ? -1 : free_seat(); /* with hot seats the keyboard follows the turn */
    if (s >= 0) S.joined_pad[s] = CW_MAX_PADS, S.join_kind[s] = KIND_NONE, sound(CW_SHELL_SND_JOIN);
    join_go();
    return;
  }
  for (int pad = 0; pad <= CW_MAX_PADS; pad++) {
    bool kb = pad == CW_MAX_PADS;
    uint32_t pressed = kb ? 0 : cw__input_pad_pressed(pad);
    bool south = kb ? (cw__input_key_pressed(40) || cw__input_key_pressed(44)) /* return, space */ : (pressed & (1u << CW_PAD_SOUTH));
    bool east = kb ? cw__input_key_pressed(41) /* escape */ : (pressed & (1u << CW_PAD_EAST));
    bool start = pressed & (1u << CW_PAD_START);
    int slot = -1, n = 0;
    for (int s = 0; s < SEATS; s++) {
      if (S.joined_pad[s] == pad) slot = s;
      if (S.joined_pad[s] >= 0) n++;
    }
    if (slot < 0 && south) {
      int s = free_seat();
      if (s >= 0) {
        S.joined_pad[s] = pad;
        S.join_kind[s] = KIND_NONE;
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
      if (slot < 0 && !join_has_hot()) { /* an empty seat only: Start never replaces a CPU, and pads drive hot seats */
        int s = empty_seat();
        if (s >= 0) S.joined_pad[s] = pad;
      }
      play = true;
    }
  }
  for (int s = 0; s < SEATS; s++) S.dbg_join_kind[s] = S.joined_pad[s] < 0 ? S.join_kind[s] : 0;
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
    if (S.resume_quiet) { /* nor, from the pause menu, the game's: its first tick is the next */
      S.resume_quiet = false;
      return false;
    }
    return true;
  }
  menu_tick();
  if (S.resume_quiet) { /* the pause menu's press (Resume, a game item, Back) stays the menu's: gameplay runs from the next tick */
    S.resume_quiet = false;
    return false;
  }
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
          if (S.seat_pad[p] >= 0 || S.seat_cpu[p] >= 0) last = p;
        }
        S.players = last + 1;
        S.dbg_players = S.players;
        if (S.d.players_changed) S.d.players_changed(S.players, S.d.user);
      }
    }
    if (any_hot()) /* a free pad (one that drives the hot seats) pauses on any turn, a CPU's too */
      for (int pd = 0; pd < CW_MAX_PADS; pd++) {
        bool held = false;
        for (int p = 0; p < S.players; p++) held |= S.seat_pad[p] == pd;
        if (!held && (cw__input_pad_pressed(pd) & (1u << CW_PAD_START))) {
          sound(CW_SHELL_SND_PAUSE);
          go(CW_SHELL_PAUSE);
          return;
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
    float tx, ty;
    bool tap = cw__input_tap(&tx, &ty) && cw_quality_menu_tap(cw_r2d_window_to_canvas(cw_v2(tx, ty)));
    if (!tap) cw_quality_menu_input(dy, dx, acc, bk);
    if (dy || dx || acc || tap) sound(CW_SHELL_SND_MOVE);
    if (!cw_quality_menu_is_open()) {
      sound(CW_SHELL_SND_BACK);
      if (cw_quality_menu_changed()) cw_shell_save_commit();
    }
    return;
  }
  build_items();
  if (S.item >= n_items) S.item = n_items - 1; /* the game's rows went away under the selection (cw_shell_set_settings_items) */
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
    else if (S.screen == CW_SHELL_SETTINGS) accept(set_back());
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
  return (cw_text_style){ .size = size, .color = c, .outline_width = size * 0.06f, .outline_color = S.d.two_colour ? S.d.paper : cw_rgba(0, 0, 0, 0.8f) };
}
/* Two colours (cw_shell_desc.two_colour): every element in ink or paper. fg is light text and marks of this alpha
 * (ink), acc the accent (ink); the selection shows by shape (COLOUR cannot: INVERSE) */
static cw_color fg(float a) { return S.d.two_colour ? S.d.ink : cw_rgba(1, 1, 1, a); }
static cw_color acc(void) { return S.d.two_colour ? S.d.ink : S.d.accent; }
static cw_shell_selection sel_style(void) {
  return S.d.two_colour && S.d.selection == CW_SHELL_SELECT_COLOUR ? CW_SHELL_SELECT_INVERSE : S.d.selection;
}
/* a paper plate under text over a picture (two colours: S.plates), d the text's drawn size */
static void plate(float x, float y, float w, float h, float d) {
  if (S.plates) cw_r2d_rect(cw_rect_make(x - d * 0.3f, y - d * 0.15f, w + d * 0.6f, h + d * 0.3f), S.d.paper);
}
static float text_w(const char *s, float size) {
  cw_text_style st = tstyle(size, CW_WHITE);
  return cw_font_measure(S.d.font, s, &st).width;
}
static bool text_plain; /* the next text() without its outline (a selected row on its INVERSE plate) */
static void text(const char *s, float x, float y, float size, cw_color c, int align /* 0 left 1 center 2 right */) {
  cw_text_style st = tstyle(size, c);
  bool plain = text_plain;
  if (text_plain) st.outline_width = 0, text_plain = false;
  if (align || (S.plates && !plain)) {
    cw_text_metrics m = cw_font_measure(S.d.font, s, &st);
    if (align) x -= align == 1 ? m.width * 0.5f : m.width;
    if (!plain) plate(x, y, m.width, m.height, drawn(size));
  }
  cw_r2d_text(S.d.font, s, cw_v2(x, y), &st);
}

/* A line of controls under a menu, drawn with button prompts for the devices the players hold (cartwheel/prompt.h).
 * Left out when no seat is on a pad, keyboard or mouse: on a touch screen the menu items are tapped. */
static bool hints_wanted(void) {
  const cw_raw_input *raw = cw_input_raw();
  for (int p = 0; p < SEATS; p++) {
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
  cw_vec2 b = cw_prompt_text_size(S.d.font, m, -1, size);
  float h = b.y;
  plate(cx - b.x * 0.5f, bottom - h, b.x, h, drawn(size));
  cw_prompt_text(S.d.font, m, -1, cw_v2(cx, bottom - h), size, fg(0.75f), CW_ALIGN_CENTER);
}

/* the selected row (selection style): its text's colour, the plate under it (INVERSE, before the text) and the mark
 * on it (MARKER, UNDERLINE, after), for text drawn from x0 to x0 + w at y, d its drawn size */
static cw_color sel_text_colour(void) {
  return sel_style() == CW_SHELL_SELECT_INVERSE ? (S.d.two_colour ? S.d.paper : S.d.selection_text) : acc();
}
static void sel_plate(float x0, float y, float w, float d) {
  if (sel_style() != CW_SHELL_SELECT_INVERSE) return;
  if (S.plates) cw_r2d_rect(cw_rect_make(x0 - d * 0.6f, y - d * 0.27f, w + d * 1.2f, d * 1.64f), S.d.paper); /* apart from the picture */
  cw_r2d_rect(cw_rect_make(x0 - d * 0.45f, y - d * 0.12f, w + d * 0.9f, d * 1.34f), acc());
}
static void sel_mark(float x0, float y, float w, float d) {
  if (sel_style() == CW_SHELL_SELECT_MARKER) {
    cw_vec2 p[3] = { { x0 - d * 0.95f, y + d * 0.2f }, { x0 - d * 0.95f, y + d * 0.95f }, { x0 - d * 0.4f, y + d * 0.575f } };
    plate(x0 - d * 0.95f, y + d * 0.2f, d * 0.55f, d * 0.75f, d * 0.5f);
    cw_r2d_polygon(p, 3, acc());
  } else if (sel_style() == CW_SHELL_SELECT_UNDERLINE) {
    plate(x0, y + d * 1.18f, w, fmaxf(2.0f, d * 0.09f), d * 0.5f);
    cw_r2d_rect(cw_rect_make(x0, y + d * 1.18f, w, fmaxf(2.0f, d * 0.09f)), acc());
  }
}

/* a small triangle: more rows above (up) or below */
static void more_mark(float cx, float y, float d, bool up) {
  float s = d * 0.28f;
  cw_vec2 p[3] = { { cx - s, up ? y + s * 0.5f : y - s * 0.5f }, { cx + s, up ? y + s * 0.5f : y - s * 0.5f }, { cx, up ? y - s * 0.5f : y + s * 0.5f } };
  plate(cx - s, y - s * 0.5f, s * 2, s, d * 0.5f);
  cw_r2d_polygon(p, 3, fg(0.7f));
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
    cw_color c = sel ? sel_text_colour() : fg(0.85f);
    char line[96];
    item_line(i, line, sizeof line);
    float tw = text_w(line, size);
    if (sel) sel_plate(cx - tw * 0.5f, y, tw, d);
    text_plain = sel && sel_style() == CW_SHELL_SELECT_INVERSE;
    text(line, cx, y, size, c, 1);
    if (sel) sel_mark(cx - tw * 0.5f, y, tw, d);
    float w = fmaxf(tw + d * 2, d * 8);
    item_rects[i] = cw_rect_make(cx - w * 0.5f, y - d * 0.15f, w, d * 1.3f);
    y += step;
  }
  if (first > 0) more_mark(cx, y0 - d * 0.3f, d, true);
  if (first + count < n_items) more_mark(cx, y + d * 0.05f, d, false);
}

/* each action and its bindings, in [x, right] from y down to bottom: names in one column, bindings wrapped in the
 * next */
static void draw_controls(float x, float y, float right, float bottom, float size) {
  text("Controls", x, y, size, acc(), 0);
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
    cw_text_style st = tstyle(s2, fg(0.7f));
    st.wrap_width = wrap;
    float h = cw_font_measure(S.d.font, line, &st).height;
    if (y + h > bottom - d2 * 1.25f) { /* no room for this row and a mark under it: the mark says there is more */
      text("...", x, y, s2, fg(0.7f), 0);
      break;
    }
    text(name, x, y, s2, fg(0.9f), 0);
    plate(col, y, cw_font_measure(S.d.font, line, &st).width, h, d2);
    cw_r2d_text(S.d.font, line, cw_v2(col, y), &st);
    y += h + d2 * 0.25f;
  }
}

static const char *const HINT_TITLE = "{ui_up|ui_down} choose    {ui_accept} select";
static const char *const HINT_PAUSE = "{ui_up|ui_down} choose    {ui_accept} select    {ui_back} resume";
static const char *const HINT_SETTINGS = "{ui_up|ui_down} choose    {ui_left|ui_right} change    {ui_back} back";

/* a seat's box on the join screen: a quarter of the canvas wide whatever the seat count, the row centred under the
 * heading (four seats fill the width, as they always did) */
static float join_seat_x(int seat, cw_vec2 sz) { /* the seat's centre; more than four seats: rows of four */
  int row = seat / 4, in_row = S.d.max_players - row * 4;
  if (in_row > 4) in_row = 4;
  return sz.x * 0.5f + ((float)(seat % 4) - (float)(in_row - 1) * 0.5f) * (sz.x / 4.0f);
}
static cw_rect join_seat_rect(int seat, cw_vec2 sz) {
  float u = sz.y / 20.0f, w = sz.x / 4.0f;
  if (S.d.max_players > 4) /* two rows, each a little shorter */
    return cw_rect_make(join_seat_x(seat, sz) - w * 0.5f + u * 0.4f, sz.y * (seat < 4 ? 0.22f : 0.53f), w - u * 0.8f, sz.y * 0.29f);
  return cw_rect_make(join_seat_x(seat, sz) - w * 0.5f + u * 0.4f, sz.y * 0.3f, w - u * 0.8f, sz.y * 0.4f);
}
cw_rect cw_shell_join_seat_rect(int seat) {
  if (!S.inited || seat < 0 || seat >= S.d.max_players) return cw_rect_make(0, 0, 0, 0);
  return join_seat_rect(seat, cw_r2d_canvas_size());
}

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
  bool two = S.d.two_colour;
  S.plates = two && (S.d.draw_background || S.screen == CW_SHELL_PAUSE);
  if (two) { /* the picture behind stays as it is (its text on paper plates), else a sheet of paper; every edge one bit */
    if (!S.plates) cw_r2d_rect(cw_rect_make(0, 0, sz.x, sz.y), S.d.paper);
    cw__r2d_set_hard(true);
    cw__prompt_two_colour(true, S.d.ink, S.d.paper);
  } else
    cw_r2d_rect(cw_rect_make(0, 0, sz.x, sz.y), cw_rgba(0.02f, 0.02f, 0.05f, S.d.draw_background ? 0.3f : S.screen == CW_SHELL_PAUSE ? 0.55f : 0.85f));
  build_items();
  switch (S.screen) {
  case CW_SHELL_LOADING: {
    float y = sz.y * 0.42f, bar = fmaxf(sz.y * 0.55f, y + drawn(u * 1.2f) * 1.3f);
    text("Loading", cx, y, u * 1.2f, fg(1), 1);
    if (two) { /* the bar's track an ink outline */
      plate(sz.x * 0.3f, bar, sz.x * 0.4f, u * 0.4f, u * 0.3f);
      cw_r2d_rect_lines(cw_rect_make(sz.x * 0.3f, bar, sz.x * 0.4f, u * 0.4f), fmaxf(2.0f, u * 0.06f), S.d.ink);
    } else cw_r2d_rect(cw_rect_make(sz.x * 0.3f, bar, sz.x * 0.4f, u * 0.4f), cw_rgba(1, 1, 1, 0.2f));
    cw_r2d_rect(cw_rect_make(sz.x * 0.3f, bar, sz.x * 0.4f * S.progress, u * 0.4f), acc());
    break;
  }
  case CW_SHELL_TITLE: {
    float y = sz.y * 0.2f;
    text(S.d.title, cx, y, u * 2.4f, fg(1), 1);
    y += drawn(u * 2.4f) * 1.17f;
    if (S.d.subtitle) {
      text(S.d.subtitle, cx, y, u * 0.9f, fg(0.75f), 1);
      y += drawn(u * 0.9f) * 1.4f;
    }
    float hs = u * 0.75f, top = hint_top(HINT_TITLE, cx, foot, hs);
    draw_menu(cx, fmaxf(sz.y * 0.5f, y + u * 0.4f), top - drawn(hs) * 0.4f, u * 1.1f);
    hint(HINT_TITLE, cx, foot, hs);
    break;
  }
  case CW_SHELL_PAUSE: {
    text("Paused", cx, sz.y * 0.22f, u * 1.8f, fg(1), 1);
    float hs = u * 0.75f, top = hint_top(HINT_PAUSE, cx, foot, hs);
    draw_menu(cx, fmaxf(sz.y * 0.45f, sz.y * 0.22f + drawn(u * 1.8f) * 1.3f), top - drawn(hs) * 0.4f, u * 1.1f);
    hint(HINT_PAUSE, cx, foot, hs);
    break;
  }
  case CW_SHELL_SETTINGS: {
    if (cw_quality_menu_is_open()) {
      cw_quality_menu_draw(&(cw_quality_menu_style){ .font = S.d.font, .accent = S.d.accent, .set_backdrop = true, .selection = (int)S.d.selection,
                                                     .selection_text = S.d.selection_text, .two_colour = two, .ink = S.d.ink,
                                                     .paper = S.d.paper, .backdrop = S.plates ? S.d.paper : (cw_color){ 0 } });
      if (two) cw__r2d_set_hard(true), cw__prompt_two_colour(true, S.d.ink, S.d.paper); /* the page turns them off when it is done */
      break;
    }
    text("Settings", cx, sz.y * 0.1f, u * 1.6f, fg(1), 1);
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
    text("Join", cx, sz.y * 0.12f, u * 1.6f, fg(1), 1);
    for (int s = 0; s < S.d.max_players; s++) {
      cw_rect r = join_seat_rect(s, sz);
      float x = join_seat_x(s, sz), inner = r.w;
      int kind = S.joined_pad[s] < 0 ? S.join_kind[s] : KIND_NONE; /* a hot seat or a CPU on the seat */
      bool in = S.joined_pad[s] >= 0 || kind != KIND_NONE, inv = in && sel_style() == CW_SHELL_SELECT_INVERSE;
      cw_color tint = s < SEATS ? S.d.seat_colors[s] : (cw_color){ 0 };
      if (two) { /* a sheet of paper in an ink frame, a taken seat's frame heavier (INVERSE: all ink) */
        cw_r2d_rect(r, inv ? S.d.ink : S.d.paper);
        cw_r2d_rect_lines(r, fmaxf(2.0f, u * (in ? 0.16f : 0.06f)), S.d.ink);
      } else
      {
        cw_r2d_rect(r,
                    inv ? S.d.accent : in ? cw_rgba(S.d.accent.r, S.d.accent.g, S.d.accent.b, 0.25f) : cw_rgba(1, 1, 1, 0.06f));
        if (tint.a > 0) /* the seat's colour (cw_shell_desc.seat_colors): a band along its top */
          cw_r2d_rect(cw_rect_make(r.x, r.y, r.w, r.h * 0.08f), cw_rgba(tint.r, tint.g, tint.b, in ? 1.0f : 0.45f));
      }
      char p[40];
      const char *name = s < SEATS ? S.d.seat_names[s] : NULL;
      if (name && name[0]) snprintf(p, sizeof p, "%s", name);
      else snprintf(p, sizeof p, "P%d", s + 1);
      float ls = S.d.max_players > 4 ? u * 1.1f : u * 1.4f; /* two rows of seats: a smaller name */
      while (ls > u * 0.7f && text_w(p, ls) > inner * 0.9f) ls *= 0.9f;
      float py = r.y + r.h * 0.15f, ly = fmaxf(r.y + r.h * 0.5f, py + drawn(ls) * 1.25f);
      text_plain = inv;
      text(p, x, py, ls, in ? sel_text_colour() : fg(0.4f), 1);
      if (in) {
        float pw = text_w(p, ls);
        sel_mark(x - pw * 0.5f, py, pw, drawn(ls));
      }
      char dev[24];
      if (!in) {
        const char *m = fit_markup("{ui_accept} join", u, inner * 0.92f);
        cw_prompt_text(S.d.font, m, -1, cw_v2(x, ly - u * 0.1f), u, fg(0.7f), CW_ALIGN_CENTER);
      } else {
        if (kind >= KIND_CPU) snprintf(dev, sizeof dev, "CPU %s", cw_shell_cpu_level_label((cw_shell_cpu_level)(kind - KIND_CPU)));
        else if (kind == KIND_HOT) snprintf(dev, sizeof dev, "pass the pad");
        else if (S.joined_pad[s] == CW_MAX_PADS) snprintf(dev, sizeof dev, "keyboard");
        else snprintf(dev, sizeof dev, "pad %d", S.joined_pad[s] + 1);
        text_plain = inv;
        text(dev, x, ly, u * 0.8f, inv ? sel_text_colour() : fg(0.9f), 1);
      }
      if ((S.d.cpu_seats || S.d.hot_seats) && s == S.join_cursor && hints_wanted()) { /* the seat the up / down buttons change: a frame round it */
        float t = fmaxf(2.0f, u * 0.1f);
        cw_r2d_rect_lines(cw_rect_make(r.x - t * 1.5f, r.y - t * 1.5f, r.w + t * 3, r.h + t * 3), t, acc());
        if (S.joined_pad[s] < 0) {
          const char *m = fit_markup(S.d.hot_seats && S.d.cpu_seats ? "{ui_up|ui_down} seat" : S.d.hot_seats ? "{ui_up|ui_down} player" : "{ui_up|ui_down} CPU", u * 0.8f, inner * 0.92f);
          cw_prompt_text(S.d.font, m, -1, cw_v2(x, r.y + r.h - drawn(u * 0.8f) * 1.6f), u * 0.8f, fg(0.7f), CW_ALIGN_CENTER);
        }
      }
    }
    if (S.d.cpu_seats)
      hint(S.mid_join ? "{ui_accept} join    {ui_back} leave    {ui_left|ui_right} seat    {ui_start} back to the game"
                      : "{ui_accept} join    {ui_back} leave    {ui_left|ui_right} seat    {ui_start} play",
           cx, sz.y - u * 1.2f, u * 0.8f);
    else
      hint(S.mid_join ? "{ui_accept} join    {ui_back} leave    {ui_start} back to the game" : "{ui_accept} join    {ui_back} leave    {ui_start} play",
           cx, sz.y - u * 1.2f, u * 0.8f);
    /* a touch screen has no buttons to name: a tap plays (join_update), a tap on a seat sets its CPU */
    if (!hints_wanted())
      text(S.d.cpu_seats ? (S.mid_join ? "tap a seat for a CPU, elsewhere to go back" : "tap a seat for a CPU, elsewhere to play")
                         : (S.mid_join ? "tap to go back to the game" : "tap to play"),
           cx, sz.y - u * 1.2f - drawn(u * 0.9f), u * 0.9f, fg(0.8f), 1);
    break;
  }
  default: break;
  }
  if (S.starting) {
    float d = drawn(u * 1.8f);
    if (two) cw_r2d_rect(cw_rect_make(0, sz.y * 0.5f - d * 0.95f, sz.x, d * 1.9f), S.d.paper);
    else {
      cw_r2d_rect(cw_rect_make(0, 0, sz.x, sz.y), cw_rgba(0.02f, 0.02f, 0.05f, 0.6f));
      cw_r2d_rect(cw_rect_make(0, sz.y * 0.5f - d * 0.95f, sz.x, d * 1.9f), cw_rgba(0.02f, 0.02f, 0.05f, 0.9f));
    }
    text("Starting...", cx, sz.y * 0.5f - d * 0.5f, u * 1.8f, acc(), 1);
    S.starting_shown = true;
    S.dbg_starting = 2;
  }
  if (two) cw__r2d_set_hard(false), cw__prompt_two_colour(false, S.d.ink, S.d.paper);
  S.plates = false;
  cw_r2d_end();
}

/* ── split screen ───────────────────────────────────────────────────── */

int cw_shell_split(cw_pipeline *p, const cw_node *cameras) {
  if (!p && cw_scene_app_tree()) p = cw_pipeline_of(cw_scene_app_tree());
  if (!p) return 0;
  if (S.players <= 1 || S.d.shared_screen || S.players > 4) { /* more than four seats (hot seats): one shared view */
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
  if (S.players <= 1 || S.d.shared_screen || S.players > 4 || player < 0 || player >= S.players) return cw_rect_make(0, 0, sz.x, sz.y);
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
