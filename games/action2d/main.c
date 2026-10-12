/* action2d, "Crypt of Embers": a
 * top-down action game built from .cwscene files: three rooms (TileMap2D
 * floors and walls with collision built from the tiles), a sword-swinging
 * hero, slimes, bats, orb-spitting skulls and a boss, hearts and gems, doors
 * that open when a room is clear, torchlit darkness (Light2D + occluders),
 * AnimatedSprite2D characters, AnimationPlayer screen shake / flashes /
 * torch flicker, tweens for the UI, music and effects through the mixer, and
 * a menu UI (title, options with volume sliders and attack rebinding, pause,
 * game over, victory) from the main scene's Control nodes, driven by
 * keyboard, mouse and gamepad.
 *
 * This file is the app: game states, menus, HUD, audio, rooms. The node types
 * that act in the world are in actors.c.
 *
 * Built twice: `action2d` reads the devices (gates: tests/action2d-pad.json,
 * tests/action2d-keys.json); `action2d_auto` replays the pad script compiled
 * in (script.inc, written by tests/action2d-script.mjs) through the platform's
 * raw-input filter, so input-free hosts play the whole game too.
 *
 * Debug fields (g.*): state, score, hp, room, enemies, kills, hits_taken,
 * pickups, attacks, rebinds, boss_hp, pauses, deaths, doors, play_ticks,
 * player (x, y), hash (every unpaused tick's game state folded), music_vol,
 * sfx_vol, focus_moves, menu_presses. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "game.h"

game G;

#ifdef ACTION2D_AUTOPILOT
static const struct { int frame; uint32_t buttons; float lx, ly; int mx, my; uint32_t mb; } SCRIPT[] = {
#include "script.inc"
};
/* the compiled-in script drives pad 0 and the mouse exactly as the CW_INPUT
 * file does natively: an entry for frame F applies from engine frame F-1 on */
static void autopilot(cw_raw_input *in, uint64_t frame, void *user) {
  (void)user;
  int k = -1;
  for (size_t i = 0; i < CW_ARRAY_LEN(SCRIPT) && (uint64_t)(SCRIPT[i].frame - 1) <= frame; i++) k = (int)i;
  memset(&in->pads[0], 0, sizeof in->pads[0]);
  in->pads[0].connected = true;
  in->mouse_buttons = 0;
  if (k < 0) return;
  in->pads[0].buttons = SCRIPT[k].buttons;
  in->pads[0].axes[CW_AXIS_LX] = SCRIPT[k].lx;
  in->pads[0].axes[CW_AXIS_LY] = SCRIPT[k].ly;
  if (SCRIPT[k].mx >= 0) {
    in->mouse_present = true;
    in->mouse_x = (float)SCRIPT[k].mx;
    in->mouse_y = (float)SCRIPT[k].my;
    in->mouse_buttons = SCRIPT[k].mb;
  }
}
#endif

/* ── audio ─────────────────────────────────────────────────────────── */

static struct {
  bool on;
  cw_bus music_bus, sfx_bus;
  cw_sound sfx[SFX_COUNT];
  cw_sound crypt, boss;
  cw_voice music;
  const char *playing;
} A;

static const char *const SFX_FILE[SFX_COUNT] = { "swing", "hit", "hurt", "pickup", "gem", "die", "shoot", "door",
                                                 "move", "select", "roar", "boom", "victory", "gameover" };

static void audio_init(void) {
  A.on = cw_audio_init(NULL);
  if (!A.on) return;
  A.music_bus = cw_bus_create("music", CW_AUDIO_MASTER);
  A.sfx_bus = cw_bus_create("sfx", CW_AUDIO_MASTER);
  cw_bus_add_fx(CW_AUDIO_MASTER, &(cw_fx_desc){ .type = CW_FX_LIMITER, .limiter = { -1.0f, 0.1f } });
  for (int i = 0; i < SFX_COUNT; i++)
    A.sfx[i] = cw_sound_load(cw_frame_printf("audio/%s.wav", SFX_FILE[i]),
                             &(cw_sound_desc){ .bus = A.sfx_bus, .max_voices = 4, .volume = 0.7f });
  A.crypt = cw_sound_load("audio/crypt.ogg", &(cw_sound_desc){ .mode = CW_SOUND_STREAM, .bus = A.music_bus, .volume = 0.55f });
  A.boss = cw_sound_load("audio/boss.ogg", &(cw_sound_desc){ .mode = CW_SOUND_STREAM, .bus = A.music_bus, .volume = 0.55f });
}

static void music(const char *track) {
  if (!A.on) return;
  const char *t = track ? cw_intern(track) : NULL;
  if (t == A.playing) return;
  cw_voice_stop(A.music, 0.4f);
  A.music = (cw_voice){ 0 };
  A.playing = t;
  if (!t) return;
  cw_sound s = !strcmp(t, "boss") ? A.boss : A.crypt;
  A.music = cw_audio_play(s, &(cw_play_desc){ .loop = true, .fade_in = 0.6f });
}

static float vol_curve(int v) { return (float)(v * v) / 100.0f; }

void game_sfx(sfx_id id) {
  if (A.on) cw_audio_play(A.sfx[id], NULL);
}

/* ── nodes of the main scene ───────────────────────────────────────── */

static struct {
  cw_node game, ui, hud, hearts[3], score, foes, banner;
  cw_node title, logo, start, options_btn, quit;
  cw_node options, music, sound, rebind, back;
  cw_node pause, resume, pause_options, pause_quit;
  cw_node gameover, go_info, retry, go_title;
  cw_node victory, v_info, v_time, v_title;
  cw_node fade, screenfx;
  cw_node boss_bar, boss_health;
  bool capturing;
  int shown_score, shown_hp, shown_boss;
} U;

static cw_node N(const char *path) {
  cw_node n = cw_node_get(U.game, path);
  if (!cw_node_valid(n)) cw_error("action2d: node '%s' missing from main.cwscene", path);
  return n;
}

static void tween_color(cw_node n, const char *prop, cw_color from, cw_color to, float secs, cw_ease ease) {
  cw_tween tw = cw_tween_property(n, prop, cw_val_color(to.r, to.g, to.b, to.a), secs, ease);
  cw_tween_from(tw, cw_val_color(from.r, from.g, from.b, from.a));
}

/* show one menu panel: fade it in (and its box drops into place) */
static void show_panel(cw_node panel, cw_node focus) {
  cw_control_set_visible(panel, true);
  tween_color(panel, "modulate", (cw_color){ 1, 1, 1, 0 }, (cw_color){ 1, 1, 1, 1 }, 0.25f, CW_EASE_OUT_QUAD);
  cw_node box = cw_node_get(panel, "Box");
  if (cw_node_valid(box)) {
    cw_tween tw = cw_tween_property(box, "offset_top", cw_val_float(cw_control_of(box)->offset_top), 0.3f, CW_EASE_OUT_BACK);
    cw_tween_from(tw, cw_val_float(cw_control_of(box)->offset_top - 14));
  }
  if (cw_node_valid(focus)) cw_control_grab_focus(focus);
}
static void hide_panel(cw_node panel) {
  cw_control_set_visible(panel, false);
  cw_node f = cw_ui_focus_owner();
  if (cw_node_valid(f)) cw_control_release_focus(f);
}

static void banner(const char *text) {
  cw_label_set_text(U.banner, text);
  cw_tween tw = cw_tween_new(NULL);
  cw_tween_bind(tw, U.banner);
  cw_tween_add_property(tw, U.banner, "modulate", cw_val_color(1, 1, 1, 1), 0.4f, CW_EASE_OUT_QUAD);
  cw_tween_from(tw, cw_val_color(1, 1, 1, 0));
  cw_tween_add_interval(tw, 1.2f);
  cw_tween_add_property(tw, U.banner, "modulate", cw_val_color(1, 1, 1, 0), 0.5f, CW_EASE_IN_QUAD);
}

/* ── rooms ─────────────────────────────────────────────────────────── */

static void load_room(int index) {
  if (cw_node_valid(G.room_node)) cw_node_free(G.room_node);
  G.room_node = (cw_node){ 0 };
  G.hero = (cw_node){ 0 };
  cw_node r = cw_scene_instance_path(cw_frame_printf("rooms/room%d.cwscene", index));
  if (!cw_node_valid(r)) { cw_error("action2d: room %d failed to load", index); return; }
  cw_node_add_child(G.world, r);
  G.room_node = r;
  G.room = index;
  cw_value v;
  if (G.state == ST_PLAY || G.state == ST_TRANSITION) {
    if (cw_node_get_prop(r, "music", &v)) music(v.s);
    if (cw_node_get_prop(r, "title", &v)) banner(v.s);
  }
}

static void fade_in_done(cw_tween tw, void *u) {
  (void)tw, (void)u;
  if (G.state == ST_TRANSITION) G.state = ST_PLAY;
}
static void swap_room(cw_tween tw, void *u) {
  (void)tw;
  load_room((int)(intptr_t)u);
}

void game_room_exit(int next_room) {
  if (G.state != ST_PLAY || next_room < 1 || next_room > ROOM_COUNT) return;
  G.state = ST_TRANSITION;
  game_sfx(SFX_SELECT);
  cw_tween tw = cw_tween_new(NULL);
  cw_tween_bind(tw, U.fade);
  cw_tween_add_property(tw, U.fade, "modulate", cw_val_color(1, 1, 1, 1), 0.35f, CW_EASE_IN_QUAD);
  cw_tween_add_callback(tw, swap_room, (void *)(intptr_t)next_room);
  cw_tween_add_interval(tw, 0.1f);
  cw_tween_add_property(tw, U.fade, "modulate", cw_val_color(1, 1, 1, 0), 0.35f, CW_EASE_OUT_QUAD);
  cw_tween_on_finished(tw, fade_in_done, NULL);
}

/* ── game flow ─────────────────────────────────────────────────────── */

void game_score(int add) { G.score += add; }

void game_shake(bool big) {
  if (!cw_node_valid(G.hero)) return;
  cw_node s = cw_node_get(G.hero, "Shake");
  cw_anim_player_stop(s, false);
  cw_anim_player_play(s, big ? "big_shake" : "shake", 0);
}

void game_flash(const char *anim) {
  cw_anim_player_stop(U.screenfx, false);
  cw_anim_player_play(U.screenfx, anim, 0);
}

void game_hero_hurt(int dmg) {
  G.hp -= dmg;
  if (G.hp < 0) G.hp = 0;
  G.hits_taken++;
  game_sfx(SFX_HURT);
  game_shake(false);
  game_flash("hurt");
  if (G.hp == 0) {
    G.deaths++;
    G.gameover_ticks = 80;
    music(NULL);
  }
}

void game_hero_heal(int amount) {
  G.hp += amount;
  if (G.hp > HERO_MAX_HP) G.hp = HERO_MAX_HP;
  tween_color(cw_node_get(U.hud, "Hearts"), "modulate", (cw_color){ 0.5f, 1, 0.5f, 1 }, (cw_color){ 1, 1, 1, 1 }, 0.5f,
              CW_EASE_OUT_QUAD);
}

void game_boss_defeated(void) {
  G.boss_dead = 1;
  G.victory_ticks = 60;
}

static void reset_run(void) {
  G.score = G.kills = G.hits_taken = G.pickups = G.attacks = G.boss_hp = G.doors = 0;
  G.play_ticks = G.victory_ticks = G.gameover_ticks = G.boss_dead = 0;
  G.hp = HERO_MAX_HP;
}

static void to_title(void) {
  G.state = ST_TITLE;
  cw_tree_set_paused(cw_scene_app_tree(), false);
  cw_control_set_visible(U.hud, false);
  cw_control_set_visible(U.pause, false);
  cw_control_set_visible(U.gameover, false);
  cw_control_set_visible(U.victory, false);
  cw_control_set_visible(U.options, false);
  reset_run();
  load_room(1);
  music("crypt");
  show_panel(U.title, U.start);
  cw_tween tw = cw_tween_property(U.logo, "offset_top", cw_val_float(26), 0.7f, CW_EASE_OUT_BOUNCE);
  cw_tween_from(tw, cw_val_float(-20));
}

static void start_game(void) {
  reset_run();
  hide_panel(U.title);
  hide_panel(U.gameover);
  G.state = ST_PLAY;
  load_room(1);
  cw_control_set_visible(U.hud, true);
  tween_color(U.hud, "modulate", (cw_color){ 1, 1, 1, 0 }, (cw_color){ 1, 1, 1, 1 }, 0.4f, CW_EASE_OUT_QUAD);
  U.shown_score = -1;
  U.shown_hp = -1;
  U.shown_boss = -1;
  cw_range_set_value(U.boss_health, 12);
}

static void set_paused(bool p) {
  cw_tree_set_paused(cw_scene_app_tree(), p);
  if (p) {
    G.state = ST_PAUSE;
    G.pauses++;
    if (A.on) cw_bus_set_volume(A.music_bus, vol_curve(G.music_vol) * 0.35f);
    show_panel(U.pause, U.resume);
  } else {
    G.state = ST_PLAY;
    if (A.on) cw_bus_set_volume(A.music_bus, vol_curve(G.music_vol));
    hide_panel(U.pause);
  }
}

static void open_options(void) {
  G.options_from = G.state;
  G.state = ST_OPTIONS;
  cw_control_set_visible(G.options_from == ST_PAUSE ? U.pause : U.title, false);
  show_panel(U.options, U.music);
}
static void close_options(void) {
  if (U.capturing) { cw_input_capture_cancel(); U.capturing = false; }
  hide_panel(U.options);
  G.state = G.options_from;
  if (G.state == ST_PAUSE) show_panel(U.pause, U.pause_options);
  else show_panel(U.title, U.options_btn);
}

/* ── rebinding ─────────────────────────────────────────────────────── */

static const char *binding_label(cw_binding b) {
  static const char *const PAD[] = { "A", "B", "X", "Y", "BACK", "GUIDE", "START", "LS", "RS", "LB", "RB", "UP", "DOWN", "LEFT", "RIGHT" };
  if (b.kind == CW_BIND_PAD_BUTTON && b.code < CW_ARRAY_LEN(PAD)) return PAD[b.code];
  char buf[48];
  cw_binding_format(b, buf, sizeof buf);
  const char *s = strchr(buf, ':');
  char *out = cw_frame_printf("%s", s ? s + 1 : buf);
  for (char *c = out; *c; c++)
    if (*c >= 'a' && *c <= 'z') *c = (char)(*c - 32);
  return out;
}

static void rebind_label(void) {
  cw_binding b[CW_INPUT_MAX_BINDINGS];
  int n = cw_action_bindings(G.a_attack, b, CW_INPUT_MAX_BINDINGS);
  const char *key = "-", *pad = "-";
  for (int i = n - 1; i >= 0; i--) {
    if (b[i].kind == CW_BIND_KEY) key = binding_label(b[i]);
    if (b[i].kind == CW_BIND_PAD_BUTTON || b[i].kind == CW_BIND_PAD_AXIS) pad = binding_label(b[i]);
  }
  cw_button_set_text(U.rebind, cw_frame_printf("ATTACK  %s / %s", key, pad));
}

static void rebind_apply(cw_binding nb) {
  cw_binding b[CW_INPUT_MAX_BINDINGS];
  int n = cw_action_bindings(G.a_attack, b, CW_INPUT_MAX_BINDINGS);
  bool pad = nb.kind == CW_BIND_PAD_BUTTON || nb.kind == CW_BIND_PAD_AXIS;
  int slot = n;
  for (int i = 0; i < n; i++) {
    bool ipad = b[i].kind == CW_BIND_PAD_BUTTON || b[i].kind == CW_BIND_PAD_AXIS;
    if (ipad == pad && (pad || b[i].kind == CW_BIND_KEY)) { slot = i; break; }
  }
  cw_action_set_binding(G.a_attack, slot, nb);
  G.rebinds++;
}

/* ── UI signals ────────────────────────────────────────────────────── */

#define SIG(name) static void name(cw_node e, const cw_value *a, int c, void *u)
SIG(on_start) { (void)e, (void)a, (void)c, (void)u; G.menu_presses++; game_sfx(SFX_SELECT); start_game(); }
SIG(on_options) { (void)e, (void)a, (void)c, (void)u; G.menu_presses++; game_sfx(SFX_SELECT); open_options(); }
SIG(on_quit) { (void)e, (void)a, (void)c, (void)u; G.menu_presses++; cw_quit(); }
SIG(on_back) { (void)e, (void)a, (void)c, (void)u; G.menu_presses++; game_sfx(SFX_SELECT); close_options(); }
SIG(on_resume) { (void)e, (void)a, (void)c, (void)u; G.menu_presses++; game_sfx(SFX_SELECT); set_paused(false); }
SIG(on_to_title) { (void)e, (void)a, (void)c, (void)u; G.menu_presses++; game_sfx(SFX_SELECT); to_title(); }
SIG(on_retry) { (void)e, (void)a, (void)c, (void)u; G.menu_presses++; game_sfx(SFX_SELECT); start_game(); }
SIG(on_rebind) {
  (void)e, (void)a, (void)c, (void)u;
  G.menu_presses++;
  U.capturing = true;
  cw_button_set_text(U.rebind, "PRESS KEY / BUTTON");
  cw_input_capture_begin(0, CW_DEVICE_KEYBOARD, true);
}
SIG(on_music) {
  (void)e, (void)c, (void)u;
  G.music_vol = (int32_t)a[0].f;
  if (A.on) cw_bus_set_volume(A.music_bus, vol_curve(G.music_vol));
}
SIG(on_sound) {
  (void)e, (void)c, (void)u;
  G.sfx_vol = (int32_t)a[0].f;
  if (A.on) cw_bus_set_volume(A.sfx_bus, vol_curve(G.sfx_vol));
  game_sfx(SFX_GEM);
}
/* menu juice: the focused control flashes gold and ticks */
SIG(on_focus) {
  (void)a, (void)c, (void)u;
  G.focus_moves++;
  game_sfx(SFX_MOVE);
  tween_color(e, "modulate", (cw_color){ 1, 0.85f, 0.4f, 1 }, (cw_color){ 1, 1, 1, 1 }, 0.25f, CW_EASE_OUT_QUAD);
}
#undef SIG

static void on_unhandled(const cw_ui_event *e, void *u) {
  (void)u;
  if (e->type == CW_UI_NAV && e->nav == CW_NAV_CANCEL && G.state == ST_OPTIONS) close_options();
}

static void connect(cw_node n, const char *sig, cw_signal_fn fn) { cw_connect(n, sig, n, NULL, fn, NULL, 0); }

/* ── HUD ───────────────────────────────────────────────────────────── */

static void hud_update(void) {
  for (int i = 0; i < 3; i++) {
    int full = G.hp >= 2 * (i + 1), half = G.hp == 2 * i + 1;
    float x = full ? 0 : half ? 10 : 20;
    cw_node_set(U.hearts[i], "region_rect", (cw_value){ .type = CW_T_VEC4, .v = { x, 0, 10, 9 } });
  }
  if (G.score != U.shown_score) {
    if (U.shown_score >= 0) tween_color(U.score, "modulate", (cw_color){ 1, 1, 1, 1 }, (cw_color){ 1, 0.85f, 0.4f, 1 }, 0.3f, CW_EASE_OUT_QUAD);
    U.shown_score = G.score;
    cw_label_set_text(U.score, cw_frame_printf("SCORE %d", G.score));
  }
  if (G.hp < U.shown_hp)
    tween_color(cw_node_get(U.hud, "Hearts"), "modulate", (cw_color){ 1, 0.2f, 0.2f, 1 }, (cw_color){ 1, 1, 1, 1 }, 0.5f, CW_EASE_OUT_QUAD);
  U.shown_hp = G.hp;
  cw_label_set_text(U.foes, G.enemies ? cw_frame_printf("FOES %d", G.enemies) : G.room < ROOM_COUNT ? "DOOR OPEN" : "");
  /* the boss bar: shown in the boss room while it stands; the fill eases down */
  bool boss = G.room == ROOM_COUNT && G.boss_hp > 0;
  if (boss != cw_control_visible_in_tree(U.boss_bar) && boss) {
    cw_control_set_visible(U.boss_bar, true);
    tween_color(U.boss_bar, "modulate", (cw_color){ 1, 1, 1, 0 }, (cw_color){ 1, 1, 1, 1 }, 0.6f, CW_EASE_OUT_QUAD);
  } else if (!boss) {
    cw_control_set_visible(U.boss_bar, false);
  }
  if (boss && G.boss_hp != U.shown_boss) {
    U.shown_boss = G.boss_hp;
    cw_tween_property(U.boss_health, "value", cw_val_float(G.boss_hp), 0.3f, CW_EASE_OUT_QUAD);
  }
}

/* ── app callbacks ─────────────────────────────────────────────────── */

static void init(void *u) {
  (void)u;
  cw_tree *t = cw_scene_app_tree();
  U.game = cw_node_get(cw_tree_root(t), "Game");
  G.root = U.game;
  G.world = N("World");
  cw_ui_set_scale(3);
  /* input: the shipped defaults, then (a real game would) the player's overrides */
  cw_input_load_bindings_asset("bindings.txt");
  cw_input_commit_defaults();
  G.a_left = cw_action_find("left");
  G.a_right = cw_action_find("right");
  G.a_up = cw_action_find("up");
  G.a_down = cw_action_find("down");
  G.a_attack = cw_action_find("attack");
  G.a_pause = cw_action_find("pause");
#ifdef ACTION2D_AUTOPILOT
  cw_set_raw_input_filter(autopilot, NULL);
#else
  const char *bot = getenv("ACTION2D_BOT"); /* native authoring tool (bot.c) */
  if (bot && *bot) bot_install(bot);
#endif
  audio_init();
  U.ui = N("UI");
  U.hud = N("UI/Hud");
  for (int i = 0; i < 3; i++) U.hearts[i] = N(cw_frame_printf("UI/Hud/Hearts/Heart%d", i + 1));
  U.score = N("UI/Hud/Score");
  U.foes = N("UI/Hud/Foes");
  U.banner = N("UI/Hud/Banner");
  U.boss_bar = N("UI/Hud/Boss");
  U.boss_health = N("UI/Hud/Boss/Health");
  U.title = N("UI/Title");
  U.logo = N("UI/Title/Logo");
  U.start = N("UI/Title/Menu/Start");
  U.options_btn = N("UI/Title/Menu/Options");
  U.quit = N("UI/Title/Menu/Quit");
  U.options = N("UI/Options");
  U.music = N("UI/Options/Box/V/Grid/Music");
  U.sound = N("UI/Options/Box/V/Grid/Sound");
  U.rebind = N("UI/Options/Box/V/Rebind");
  U.back = N("UI/Options/Box/V/Back");
  U.pause = N("UI/Pause");
  U.resume = N("UI/Pause/Box/V/Resume");
  U.pause_options = N("UI/Pause/Box/V/Options");
  U.pause_quit = N("UI/Pause/Box/V/Quit");
  U.gameover = N("UI/GameOver");
  U.go_info = N("UI/GameOver/Box/V/Info");
  U.retry = N("UI/GameOver/Box/V/Retry");
  U.go_title = N("UI/GameOver/Box/V/Title");
  U.victory = N("UI/Victory");
  U.v_info = N("UI/Victory/Box/V/Info");
  U.v_time = N("UI/Victory/Box/V/Time");
  U.v_title = N("UI/Victory/Box/V/Title");
  U.fade = N("Overlay/Fade");
  U.screenfx = N("ScreenFx");
  connect(U.start, "pressed", on_start);
  connect(U.options_btn, "pressed", on_options);
  connect(U.quit, "pressed", on_quit);
  connect(U.back, "pressed", on_back);
  connect(U.rebind, "pressed", on_rebind);
  connect(U.music, "value_changed", on_music);
  connect(U.sound, "value_changed", on_sound);
  connect(U.resume, "pressed", on_resume);
  connect(U.pause_options, "pressed", on_options);
  connect(U.pause_quit, "pressed", on_to_title);
  connect(U.retry, "pressed", on_retry);
  connect(U.go_title, "pressed", on_to_title);
  connect(U.v_title, "pressed", on_to_title);
  cw_node focusables[] = { U.start, U.options_btn, U.quit, U.music, U.sound, U.rebind, U.back, U.resume,
                           U.pause_options, U.pause_quit, U.retry, U.go_title, U.v_title };
  for (size_t i = 0; i < CW_ARRAY_LEN(focusables); i++) connect(focusables[i], "focus_entered", on_focus);
  cw_ui_set_unhandled_input(on_unhandled, NULL);
  G.music_vol = (int32_t)cw_range_value(U.music);
  G.sfx_vol = (int32_t)cw_range_value(U.sound);
  if (A.on) {
    cw_bus_set_volume(A.music_bus, vol_curve(G.music_vol));
    cw_bus_set_volume(A.sfx_bus, vol_curve(G.sfx_vol));
  }
  rebind_label();

  cw_debug_i32("g.state", &G.state);
  cw_debug_i32("g.score", &G.score);
  cw_debug_i32("g.hp", &G.hp);
  cw_debug_i32("g.room", &G.room);
  cw_debug_i32("g.enemies", &G.enemies);
  cw_debug_i32("g.kills", &G.kills);
  cw_debug_i32("g.hits_taken", &G.hits_taken);
  cw_debug_i32("g.pickups", &G.pickups);
  cw_debug_i32("g.attacks", &G.attacks);
  cw_debug_i32("g.rebinds", &G.rebinds);
  cw_debug_i32("g.boss_hp", &G.boss_hp);
  cw_debug_i32("g.pauses", &G.pauses);
  cw_debug_i32("g.deaths", &G.deaths);
  cw_debug_i32("g.doors", &G.doors);
  cw_debug_i32("g.play_ticks", &G.play_ticks);
  cw_debug_f32_array("g.player", G.player, 2);
  cw_debug_u32("g.hash", &G.hash);
  cw_debug_i32("g.music_vol", &G.music_vol);
  cw_debug_i32("g.sfx_vol", &G.sfx_vol);
  cw_debug_i32("g.focus_moves", &G.focus_moves);
  cw_debug_i32("g.menu_presses", &G.menu_presses);
  cw_ui_debug_register("ui");
  G.hash = CW_HASH32_INIT;
  to_title();
}

/* simulation: runs on fixed ticks while the tree is not paused */
static void fixed(void *u, double dt) {
  (void)u, (void)dt;
  G.enemies = actors_enemies_alive();
  if (G.state == ST_PLAY || G.state == ST_TRANSITION) G.play_ticks++;
  if (G.state == ST_PLAY && G.enemies == 0 && G.room < ROOM_COUNT) actors_open_doors();
  if (G.gameover_ticks > 0 && --G.gameover_ticks == 0) {
    G.state = ST_GAMEOVER;
    cw_label_set_text(U.go_info, cw_frame_printf("SCORE %d", G.score));
    cw_control_set_visible(U.hud, false);
    game_sfx(SFX_GAMEOVER);
    show_panel(U.gameover, U.retry);
  }
  if (G.victory_ticks > 0 && --G.victory_ticks == 0) {
    G.state = ST_VICTORY;
    music(NULL);
    game_sfx(SFX_VICTORY);
    int secs = G.play_ticks / 60;
    cw_label_set_text(U.v_info, cw_frame_printf("SCORE %d   FOES %d", G.score, G.kills));
    cw_label_set_text(U.v_time, cw_frame_printf("TIME %d:%02d", secs / 60, secs % 60));
    cw_control_set_visible(U.hud, false);
    show_panel(U.victory, U.v_title);
  }
  uint32_t h = G.hash;
  h = cw_hash32(&G.state, sizeof(int32_t) * 15, h); /* state .. pauses */
  h = cw_hash32(G.player, sizeof G.player, h);
  G.hash = actors_hash(h);
}

/* per frame: menus, pause, HUD, audio (frame input snapshot) */
static void update(void *u, double dt) {
  (void)u, (void)dt;
  if (G.state == ST_PLAY && cw_action_just_pressed(0, G.a_pause) && !G.gameover_ticks && !G.victory_ticks) set_paused(true);
  else if (G.state == ST_PAUSE && cw_action_just_pressed(0, G.a_pause)) set_paused(false);
  if (U.capturing) {
    cw_binding b;
    if (cw_input_capture_poll(&b)) {
      U.capturing = false;
      if (!(b.kind == CW_BIND_KEY && b.code == 41 /* escape */)) rebind_apply(b);
      rebind_label();
      game_sfx(SFX_SELECT);
    }
  }
  if (G.state != ST_TITLE) hud_update();
  if (A.on) cw_audio_update();
}

static cw_signal_fn resolve(cw_node target, const char *method, void **user) {
  (void)target;
  *user = NULL;
  cw_signal_fn fn = actors_resolve(method);
  if (!fn) cw_error("action2d: no method '%s' for a scene connection", method);
  return fn;
}

cw_app_desc cw_main(int argc, char **argv) {
  (void)argc, (void)argv;
  cw_scene_register_builtin_types();
  actors_register();
  cw_set_method_resolver(resolve);
  static cw_app_desc base = { .title = "Crypt of Embers", .width = 960, .height = 540, .debug = true, .init = init,
                              .fixed_update = fixed, .update = update };
  return cw_scene_app("main.cwscene", &base);
}
