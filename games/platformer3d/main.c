/* platformer3d: a small 3D physics
 * platformer played start to finish: title -> level 1 -> level 2 -> end.
 *
 * Everything in the world is authored in .cwscene files (assets/scenes):
 * the levels place StaticBody3D terrain, KinematicBody3D platforms moved and
 * spun by AnimationPlayer tracks, RigidBody3D crates and balls, Area3D
 * pickups / hazards / checkpoints / goals (their signals connected by name in
 * the scenes), lights, the FollowCamera and the player scene (a Hero
 * CharacterBody3D with a primitive robot animated by an AnimationTree state
 * machine). The C is what scripts would hold: the node types (nodes.c) and,
 * here, the game flow (title, levels, lives, respawn, scene switching), the
 * HUD (render2d over the 3D frame), music + sound (audio.h, 3D voices on
 * moving platforms and goals) and input actions (bindings from input.cfg).
 *
 * Determinism: the whole run replays identically from the input stream
 * (fixed ticks, the tick action snapshot, cartwheel/math.h, Box3D). The
 * debug fields name the state the gate compares across hosts: game.state,
 * level, score, coins, gems, lives, deaths, checkpoint, timer, player.pos,
 * player.vel, cam.pos, anim.state, props.hash (every rigid prop's transform
 * each tick), phys.hash and state.hash (all of it folded every tick).
 *
 * Built twice: `platformer3d` reads devices (gate input:
 * tests/platformer3d-input.json); `platformer3d_auto` plays the same script
 * itself through cw_set_raw_input_filter, so input-free hosts
 * (wasmcart-native) run the whole game too. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cartwheel/debug.h"
#include "cartwheel/platform.h"
#include "cartwheel/render2d.h"
#include "cartwheel/render3d.h"
#include "cartwheel/resource.h"
#include "game.h"

game G;

/* ── autopilot: the gate's input script compiled in ───────────────── */

#ifdef PLATFORMER3D_AUTOPILOT
static const struct { int frame; uint32_t buttons; float lx, ly; } SCRIPT[] = {
#include "script.inc"
};
/* the same semantics as tools/run-cart.mjs: an entry sets pad 0 from its
 * 1-based frame on (engine frame index frame-1) */
static void autopilot(cw_raw_input *in, uint64_t frame, void *user) {
  (void)user;
  int cur = -1;
  for (int i = 0; i < (int)CW_ARRAY_LEN(SCRIPT) && (uint64_t)(SCRIPT[i].frame - 1) <= frame; i++) cur = i;
  memset(in->pads, 0, sizeof in->pads);
  memset(in->keys, 0, sizeof in->keys);
  memset(in->keys_pressed, 0, sizeof in->keys_pressed);
  in->pads[0].connected = true;
  if (cur < 0) return;
  in->pads[0].buttons = SCRIPT[cur].buttons;
  in->pads[0].axes[CW_AXIS_LX] = SCRIPT[cur].lx;
  in->pads[0].axes[CW_AXIS_LY] = SCRIPT[cur].ly;
}
#endif

/* ── audio ──────────────────────────────────────────────────────────── */

static const char *const SFX_FILES[SFX_COUNT] = {
  "audio/jump.wav", "audio/double_jump.wav", "audio/land.wav", "audio/dash.wav", "audio/coin.wav",
  "audio/gem.wav", "audio/hurt.wav", "audio/checkpoint.wav", "audio/goal.wav", "audio/bump.wav",
};
#define MAX_LOOPS 16
static struct {
  bool on;
  cw_bus music_bus, sfx_bus, world_bus;
  cw_res sfx_res[SFX_COUNT], whir_res, portal_res, music_res;
  cw_sound sfx[SFX_COUNT];
  cw_voice music;
  const char *music_path;
  struct { cw_node node; cw_voice voice; } loops[MAX_LOOPS];
  int loop_count;
} A;

static cw_voice_3d space_at(t3_vec3 p, float ref) {
  return (cw_voice_3d){ .position = p, .model = CW_ATTEN_INVERSE, .ref_distance = ref, .max_distance = 80, .rolloff = 1.2f };
}

void game_sfx(sfx_id id, const t3_vec3 *at) {
  if (!A.on) return;
  cw_play_desc d = { .bus = A.sfx_bus };
  if (at) {
    d.spatial = true;
    d.space = space_at(*at, 4);
  }
  cw_audio_play(A.sfx[id], &d);
}

static void audio_init(void) {
  A.on = getenv("PLAT3D_NO_AUDIO") == NULL && cw_audio_init(&(cw_audio_desc){ 0 });
  if (!A.on) return;
  A.music_bus = cw_bus_create("music", CW_AUDIO_MASTER);
  A.sfx_bus = cw_bus_create("sfx", CW_AUDIO_MASTER);
  A.world_bus = cw_bus_create("world", A.sfx_bus);
  cw_bus_set_volume(A.music_bus, 0.55f);
  cw_bus_set_volume(A.world_bus, 0.6f);
  cw_bus_add_fx(CW_AUDIO_MASTER, &(cw_fx_desc){ .type = CW_FX_LIMITER, .limiter = { -1, 0.1f } });
  for (int i = 0; i < SFX_COUNT; i++) {
    A.sfx_res[i] = cw_res_load(SFX_FILES[i], 0);
    A.sfx[i] = cw_res_audio_sound(A.sfx_res[i], &(cw_sound_desc){ .mode = CW_SOUND_SAMPLE, .max_voices = 4, .bus = A.sfx_bus });
  }
  A.whir_res = cw_res_load("audio/whir.wav", 0);
  A.portal_res = cw_res_load("audio/portal.wav", 0);
}

static void play_music(const char *path) {
  if (!A.on || !path || !path[0] || (A.music_path && !strcmp(A.music_path, path))) return;
  cw_voice_stop(A.music, 0.6f);
  cw_res_release(A.music_res);
  A.music_res = cw_res_load(path, 0);
  cw_sound s = cw_res_audio_sound(A.music_res, &(cw_sound_desc){ .mode = CW_SOUND_STREAM, .bus = A.music_bus });
  A.music = cw_audio_play(s, &(cw_play_desc){ .loop = true, .fade_in = 1.0f, .bus = A.music_bus });
  A.music_path = path;
}

static void stop_loops(void) {
  for (int i = 0; i < A.loop_count; i++) cw_voice_stop(A.loops[i].voice, 0.2f);
  A.loop_count = 0;
}

/* looping 3D voices that follow world nodes: platform machinery, goal portals */
static void start_loops(void) {
  if (!A.on) return;
  cw_tree *t = cw_scene_app_tree();
  const char *groups[2] = { "machines", "goals" };
  cw_res res[2] = { A.whir_res, A.portal_res };
  for (int g = 0; g < 2; g++) {
    cw_node *nodes;
    int n = cw_tree_group(t, groups[g], &nodes);
    cw_sound s = cw_res_audio_sound(res[g], &(cw_sound_desc){ .mode = CW_SOUND_SAMPLE, .bus = A.world_bus });
    for (int i = 0; i < n && A.loop_count < MAX_LOOPS; i++) {
      t3_vec3 p = cw_node3d_global_position(nodes[i]);
      cw_voice v = cw_audio_play(s, &(cw_play_desc){ .loop = true, .spatial = true, .space = space_at(p, g ? 3 : 2),
                                                     .bus = A.world_bus, .volume = g ? 0.8f : 0.6f });
      A.loops[A.loop_count].node = nodes[i];
      A.loops[A.loop_count++].voice = v;
    }
  }
}

static void audio_tick(void) {
  if (!A.on) return;
  for (int i = 0; i < A.loop_count; i++)
    if (cw_node_valid(A.loops[i].node))
      cw_voice_set_position(A.loops[i].voice, cw_node3d_global_position(A.loops[i].node), (t3_vec3){ 0, 0, 0 });
  cw_node cam = cw_render3d_camera(cw_scene_app_tree());
  if (cw_node_valid(cam)) {
    const t3_mat4 *m = cw_node3d_global_transform(cam);
    t3_vec3 p = { m->e[12], m->e[13], m->e[14] }, f = { -m->e[8], -m->e[9], -m->e[10] }, up = { m->e[4], m->e[5], m->e[6] };
    cw_audio_set_listener(p, (t3_vec3){ 0, 0, 0 }, f, up);
  }
}

/* ── deferred frees ─────────────────────────────────────────────────── */

#define MAX_FREES 64
typedef struct pending_free {
  cw_node node;
  int32_t at; /* tick */
} pending_free;
static pending_free frees[MAX_FREES];
static int free_count;
void game_free_later(cw_node n, int ticks) {
  if (free_count < MAX_FREES) frees[free_count++] = (pending_free){ n, G.ticks + ticks };
}
static void run_frees(void) {
  int k = 0;
  for (int i = 0; i < free_count; i++) {
    if (frees[i].at <= G.ticks) {
      if (cw_node_valid(frees[i].node)) cw_node_free(frees[i].node);
    } else {
      frees[k++] = frees[i];
    }
  }
  free_count = k;
}

/* ── scenes ─────────────────────────────────────────────────────────── */

static bool find_first(cw_node n, void *user) {
  cw_node *out = user;
  if (CW_HANDLE_IS_NULL(out[0]) && cw_node_is(n, "Hero")) out[0] = n;
  if (CW_HANDLE_IS_NULL(out[1]) && cw_node_is(n, "FollowCamera")) out[1] = n;
  return true;
}

static void load_scene(const char *path) {
  cw_tree *t = cw_scene_app_tree();
  stop_loops();
  free_count = 0;
  if (cw_node_valid(G.scene)) cw_node_free(G.scene);
  G.scene = cw_scene_instance_path(path);
  if (CW_HANDLE_IS_NULL(G.scene)) {
    cw_error("platformer3d: scene '%s' failed to load", path);
    return;
  }
  cw_node_add_child(cw_tree_root(t), G.scene);
  G.info = cw_node_data(G.scene, "LevelInfo");
  if (!G.info) cw_error("platformer3d: %s: the root must be a LevelInfo node", path);
  cw_node found[2] = { { 0 }, { 0 } };
  cw_node_walk(G.scene, find_first, found);
  G.hero = found[0];
  G.camera = found[1];
  G.respawn = cw_node_valid(G.hero) ? cw_node3d_position(G.hero) : (t3_vec3){ 0, 0, 0 };
  G.level = G.info ? G.info->index : 0;
  G.level_ticks = 0;
  G.state_ticks = 0;
  G.banner_ticks = 150;
  G.toast_ticks = 0;
  G.cam_snap = true;
  int kind = G.info ? G.info->kind : LEVEL_PLAY;
  G.state = kind == LEVEL_TITLE ? GS_TITLE : kind == LEVEL_END ? GS_END : GS_PLAYING;
  if (G.info) play_music(G.info->music);
  start_loops();
  cw_info("platformer3d: scene %s (%s)", path, G.info && G.info->title ? G.info->title : "");
}

static void new_game(void) {
  G.score = G.coins = G.gems = G.deaths = G.checkpoint = G.timer = 0;
  G.jumps = G.double_jumps = G.dashes = 0;
  G.lives = 3;
}

void game_hurt(void) {
  if (G.state != GS_PLAYING) return;
  G.lives--;
  G.deaths++;
  G.state = GS_DYING;
  G.state_ticks = 0;
  t3_vec3 at = cw_node3d_position(G.hero);
  game_sfx(SFX_HURT, &at);
  cw_node_set(cw_node_get(G.hero, "Model"), "visible", cw_val_bool(false));
  cw_node_set(G.hero, "velocity", cw_val_vec3(0, 0, 0));
}

/* ── debug channel: jump to a scene (`level scenes/level2.cwscene`) ─── */

static void cmd_level(int argc, const char **argv, cw_reply *r) {
  if (argc < 2) {
    cw_reply_error(r, "usage: level <scene path>");
    return;
  }
  G.pending_scene = cw_intern(argv[1]);
  cw_reply_printf(r, "{\"loading\":\"%s\"}", argv[1]);
}

/* ── the app ────────────────────────────────────────────────────────── */

static cw_font font_ui, font_title;

static void init(void *u) {
  (void)u;
  if (!cw_input_load_bindings_asset("input.cfg")) cw_warn("platformer3d: input.cfg had errors (see above)");
  cw_input_commit_defaults();
  G.a_left = cw_action_find("move_left");
  G.a_right = cw_action_find("move_right");
  G.a_forward = cw_action_find("move_forward");
  G.a_back = cw_action_find("move_back");
  G.a_jump = cw_action_find("jump");
  G.a_dash = cw_action_find("dash");
  G.a_start = cw_action_find("start");
  audio_init();
  font_ui = cw_font_load("fonts/FiraSans-Regular.ttf", NULL);
  font_title = cw_font_load("fonts/PressStart2P-Regular.ttf", NULL);

  cw_debug_i32("game.state", &G.state);
  cw_debug_i32("game.level", &G.level);
  cw_debug_i32("score", &G.score);
  cw_debug_i32("coins", &G.coins);
  cw_debug_i32("gems", &G.gems);
  cw_debug_i32("lives", &G.lives);
  cw_debug_i32("deaths", &G.deaths);
  cw_debug_i32("checkpoint", &G.checkpoint);
  cw_debug_i32("timer", &G.timer);
  cw_debug_i32("level.ticks", &G.level_ticks);
  cw_debug_i32("player.jumps", &G.jumps);
  cw_debug_i32("player.double_jumps", &G.double_jumps);
  cw_debug_i32("player.dashes", &G.dashes);
  cw_debug_i32("player.floor", &G.floor);
  cw_debug_i32("anim.state", &G.anim_state);
  cw_debug_f32_array("player.pos", G.player_pos, 3);
  cw_debug_f32_array("player.vel", G.player_vel, 3);
  cw_debug_f32_array("cam.pos", G.cam_pos, 3);
  cw_debug_u32("props.hash", &G.props_hash);
  cw_debug_u32("phys.hash", &G.phys_hash);
  cw_debug_u32("state.hash", &G.state_hash);
  cw_debug_i32("ticks", &G.ticks);
  cw_debug_command("level", "level <scene path>: load a scene now (scenes/level2.cwscene)", cmd_level);
  G.props_hash = G.state_hash = CW_HASH32_INIT;
  /* record / replay of the action stream (native dev + tests/platformer3d-replay.mjs):
   * PLAT3D_RECORD=file writes every tick's actions at exit, PLAT3D_REPLAY=file
   * plays them back instead of the devices */
  const char *rp = getenv("PLAT3D_REPLAY");
  if (rp) {
    FILE *fp = fopen(rp, "rb");
    static uint8_t rec[1 << 20];
    size_t len = fp ? fread(rec, 1, sizeof rec, fp) : 0;
    if (fp) fclose(fp);
    if (!len || !cw_input_replay_begin(rec, len)) cw_error("platformer3d: cannot replay %s", rp);
  }
  if (getenv("PLAT3D_RECORD")) cw_input_record_begin();
  new_game();
  load_scene("scenes/title.cwscene");
}

static void put3(float *o, t3_vec3 v) {
  o[0] = v.x;
  o[1] = v.y;
  o[2] = v.z;
}

static void fixed(void *u, double dt) {
  (void)u;
  (void)dt;
  cw_tree *t = cw_scene_app_tree();
  G.ticks++;
  G.state_ticks++;
  G.level_ticks++;
  if (G.banner_ticks > 0) G.banner_ticks--;
  if (G.toast_ticks > 0) G.toast_ticks--;
  G.cam_snap = false;
  run_frees();

  switch (G.state) {
  case GS_TITLE:
    if (G.state_ticks > 20 && cw_action_just_pressed(0, G.a_start)) {
      new_game();
      G.pending_scene = G.info ? G.info->next_scene : NULL;
    }
    break;
  case GS_PLAYING:
    G.timer++;
    break;
  case GS_DYING:
    G.timer++;
    if (G.state_ticks >= 75) {
      if (G.lives <= 0) {
        G.state = GS_GAMEOVER;
        G.state_ticks = 0;
      } else {
        hero_respawn(G.hero, (t3_vec3){ G.respawn.x, G.respawn.y + 0.05f, G.respawn.z });
        cw_node_set(cw_node_get(G.hero, "Model"), "visible", cw_val_bool(true));
        G.state = GS_PLAYING;
        G.state_ticks = 0;
        G.cam_snap = true;
      }
    }
    break;
  case GS_CLEAR:
    if (G.state_ticks >= 150 && G.info) G.pending_scene = G.info->next_scene;
    break;
  case GS_GAMEOVER:
    if (G.state_ticks >= 200) G.pending_scene = "scenes/title.cwscene";
    break;
  case GS_END:
    if (G.state_ticks > 120 && cw_action_just_pressed(0, G.a_start)) G.pending_scene = G.info ? G.info->next_scene : NULL;
    break;
  }
  if (G.pending_scene) {
    const char *p = G.pending_scene;
    G.pending_scene = NULL;
    load_scene(p);
  }

  /* named state + the per-tick fold */
  if (cw_node_valid(G.hero)) {
    put3(G.player_pos, cw_node3d_position(G.hero));
    cw_value v;
    cw_node_get_prop(G.hero, "velocity", &v);
    memcpy(G.player_vel, v.v, sizeof G.player_vel);
  }
  if (cw_node_valid(G.camera)) put3(G.cam_pos, cw_node3d_position(G.camera));
  cw_node *props;
  int n = cw_tree_group(t, "props", &props);
  for (int i = 0; i < n; i++) {
    t3_vec3 p = cw_node3d_position(props[i]);
    t3_quat q = cw_node3d_quaternion(props[i]);
    G.props_hash = cw_hash32(&p, sizeof p, G.props_hash);
    G.props_hash = cw_hash32(&q, sizeof q, G.props_hash);
  }
  G.phys_hash = cw_world3d_stats(cw_physics3d_world(t)).hash;
  int32_t ints[] = { G.state, G.level, G.score, G.coins, G.gems, G.lives, G.deaths, G.checkpoint, G.timer, G.anim_state, G.floor };
  G.state_hash = cw_hash32(ints, sizeof ints, G.state_hash);
  G.state_hash = cw_hash32(G.player_pos, sizeof G.player_pos, G.state_hash);
  G.state_hash = cw_hash32(G.player_vel, sizeof G.player_vel, G.state_hash);
  G.state_hash = cw_hash32(G.cam_pos, sizeof G.cam_pos, G.state_hash);
  G.state_hash = cw_hash32(&G.props_hash, sizeof G.props_hash, G.state_hash);
  if (getenv("PLAT3D_TRACE")) {
    /* dev aid: the first machine and hazard positions alongside the hero */
    cw_node *m, *hz;
    t3_vec3 mp = { 0, 0, 0 }, hp = { 0, 0, 0 };
    if (cw_tree_group(t, "machines", &m) > 0) mp = cw_node3d_position(m[0]);
    int nh = cw_tree_group(t, "hazards", &hz);
    if (nh > 0) hp = cw_node3d_position(hz[nh - 1]);
    cw_info("t=%d mach=%.2f,%.2f,%.2f haz=%.2f,%.2f,%.2f", G.ticks, mp.x, mp.y, mp.z, hp.x, hp.y, hp.z);
    cw_info("t=%d st=%d lv=%d pos=%.2f,%.2f,%.2f vel=%.2f,%.2f,%.2f floor=%d sc=%d c=%d g=%d lives=%d cp=%d anim=%d", G.ticks,
            G.state, G.level, G.player_pos[0], G.player_pos[1], G.player_pos[2], G.player_vel[0], G.player_vel[1],
            G.player_vel[2], G.floor, G.score, G.coins, G.gems, G.lives, G.checkpoint, G.anim_state);
  }
  audio_tick();
}

static void update(void *u, double dt) {
  (void)u;
  (void)dt;
  if (A.on) cw_audio_update();
}

/* ── HUD (render2d over the 3D frame; render-only, never feeds state) ── */

static cw_color rgba(float r, float g, float b, float a) { return (cw_color){ r, g, b, a }; }

static void text_shadow(cw_font f, const char *s, float x, float y, float size, cw_color c, cw_text_align align, float wrap) {
  cw_text_style st = { .size = size, .color = rgba(0, 0, 0, 0.55f * c.a), .align = align, .wrap_width = wrap };
  float o = size * 0.06f + 1;
  cw_r2d_text(f, s, cw_v2(x + o, y + o), &st);
  st.color = c;
  cw_r2d_text(f, s, cw_v2(x, y), &st);
}

static void coin_icon(float x, float y, float r) {
  cw_r2d_circle(cw_v2(x, y), r, 24, rgba(0.75f, 0.45f, 0.02f, 1));
  cw_r2d_circle(cw_v2(x, y), r * 0.8f, 24, rgba(1, 0.8f, 0.15f, 1));
  cw_r2d_rect(cw_rect_make(x - r * 0.12f, y - r * 0.45f, r * 0.24f, r * 0.9f), rgba(1, 0.93f, 0.55f, 1));
}
static void gem_icon(float x, float y, float r) {
  cw_vec2 p[4] = { { x, y - r }, { x + r * 0.75f, y }, { x, y + r }, { x - r * 0.75f, y } };
  cw_r2d_polygon(p, 4, rgba(0.15f, 0.5f, 1, 1));
  cw_vec2 q[3] = { { x, y - r }, { x + r * 0.75f, y }, { x, y } };
  cw_r2d_polygon(q, 3, rgba(0.55f, 0.85f, 1, 1));
}
static void heart_icon(float x, float y, float r, bool full) {
  cw_color c = full ? rgba(1, 0.22f, 0.3f, 1) : rgba(0.25f, 0.25f, 0.3f, 0.8f);
  cw_r2d_circle(cw_v2(x - r * 0.5f, y - r * 0.2f), r * 0.55f, 16, c);
  cw_r2d_circle(cw_v2(x + r * 0.5f, y - r * 0.2f), r * 0.55f, 16, c);
  cw_vec2 tri[3] = { { x - r * 1.03f, y }, { x + r * 1.03f, y }, { x, y + r * 1.0f } };
  cw_r2d_polygon(tri, 3, c);
}

static void panel(float x, float y, float w, float h) {
  cw_r2d_rect(cw_rect_make(x, y, w, h), rgba(0.03f, 0.05f, 0.1f, 0.55f));
  cw_r2d_rect_lines(cw_rect_make(x, y, w, h), 2, rgba(1, 1, 1, 0.18f));
}

static void render(void *u, double alpha) {
  (void)u;
  (void)alpha;
  cw_r2d_begin(&(cw_r2d_pass_desc){ .clear = false });
  cw_vec2 cs = cw_r2d_canvas_size();
  float W = cs.x, H = cs.y, s = H / 540.0f;
  char buf[96];
  int frame = (int)cw_frame_index();

  if (G.state == GS_TITLE) {
    cw_r2d_rect(cw_rect_make(0, H * 0.12f, W, H * 0.28f), rgba(0.02f, 0.04f, 0.12f, 0.45f));
    text_shadow(font_title, "ROBO HOP", 0, H * 0.16f, 64 * s, rgba(1, 0.75f, 0.15f, 1), CW_ALIGN_CENTER, W);
    text_shadow(font_ui, "a cartwheel engine 3D platformer", 0, H * 0.3f, 24 * s, rgba(0.85f, 0.92f, 1, 1), CW_ALIGN_CENTER, W);
    if ((frame / 30) % 2 == 0)
      text_shadow(font_title, "PRESS START", 0, H * 0.74f, 24 * s, rgba(1, 1, 1, 1), CW_ALIGN_CENTER, W);
    text_shadow(font_ui, "stick / WASD move    A / Space jump (twice in the air)    X / Shift dash", 0, H * 0.86f, 18 * s,
                rgba(0.8f, 0.85f, 0.95f, 0.9f), CW_ALIGN_CENTER, W);
  } else if (G.state == GS_END) {
    cw_r2d_rect(cw_rect_make(0, H * 0.06f, W, 84 * s), rgba(0.02f, 0.04f, 0.12f, 0.45f));
    text_shadow(font_title, "YOU DID IT!", 0, H * 0.06f + 18 * s, 44 * s, rgba(1, 0.8f, 0.2f, 1), CW_ALIGN_CENTER, W);
    /* the run's stats beside the cheering robot */
    float px = W - 300 * s, py = H * 0.3f;
    panel(px, py, 270 * s, 210 * s);
    int secs = G.timer / 60;
    const char *names[5] = { "Score", "Coins", "Gems", "Time", "Falls" };
    char vals[5][24];
    snprintf(vals[0], sizeof vals[0], "%d", G.score);
    snprintf(vals[1], sizeof vals[1], "%d", G.coins);
    snprintf(vals[2], sizeof vals[2], "%d", G.gems);
    snprintf(vals[3], sizeof vals[3], "%d:%02d", secs / 60, secs % 60);
    snprintf(vals[4], sizeof vals[4], "%d", G.deaths);
    for (int i = 0; i < 5; i++) {
      text_shadow(font_ui, names[i], px + 22 * s, py + (14 + 38 * i) * s, 28 * s, rgba(0.8f, 0.88f, 1, 1), CW_ALIGN_LEFT, 0);
      text_shadow(font_ui, vals[i], px + 22 * s, py + (14 + 38 * i) * s, 28 * s, rgba(1, 1, 1, 1), CW_ALIGN_RIGHT, 226 * s);
    }
    if (G.state_ticks > 120 && (frame / 30) % 2 == 0)
      text_shadow(font_title, "PRESS START", 0, H * 0.86f, 20 * s, rgba(1, 1, 1, 1), CW_ALIGN_CENTER, W);
  } else {
    /* top-left: score, coins, gems */
    panel(14 * s, 14 * s, 250 * s, 92 * s);
    snprintf(buf, sizeof buf, "%06d", G.score);
    text_shadow(font_title, buf, 28 * s, 26 * s, 22 * s, rgba(1, 1, 1, 1), CW_ALIGN_LEFT, 0);
    coin_icon(40 * s, 78 * s, 13 * s);
    snprintf(buf, sizeof buf, "x %d", G.coins);
    text_shadow(font_ui, buf, 60 * s, 63 * s, 26 * s, rgba(1, 0.9f, 0.5f, 1), CW_ALIGN_LEFT, 0);
    gem_icon(150 * s, 78 * s, 14 * s);
    snprintf(buf, sizeof buf, "x %d", G.gems);
    text_shadow(font_ui, buf, 168 * s, 63 * s, 26 * s, rgba(0.6f, 0.85f, 1, 1), CW_ALIGN_LEFT, 0);
    /* top-right: lives and time */
    panel(W - 234 * s, 14 * s, 220 * s, 92 * s);
    for (int i = 0; i < 3; i++) heart_icon(W - 206 * s + i * 36 * s, 40 * s, 13 * s, i < G.lives);
    if (G.info && G.info->index > 0) {
      snprintf(buf, sizeof buf, "LV %d", G.info->index);
      text_shadow(font_ui, buf, W - 86 * s, 24 * s, 22 * s, rgba(0.85f, 0.9f, 1, 1), CW_ALIGN_LEFT, 0);
    }
    int cs100 = G.timer * 100 / 60;
    snprintf(buf, sizeof buf, "%d:%02d.%02d", cs100 / 6000, (cs100 / 100) % 60, cs100 % 100);
    text_shadow(font_title, buf, W - 222 * s, 66 * s, 20 * s, rgba(1, 1, 1, 1), CW_ALIGN_LEFT, 0);

    if (G.banner_ticks > 0 && G.info && G.info->title && G.state == GS_PLAYING) {
      float a = cw_clampf(G.banner_ticks / 30.0f, 0, 1);
      cw_r2d_rect(cw_rect_make(0, H * 0.2f, W, 86 * s), rgba(0.02f, 0.04f, 0.12f, 0.5f * a));
      snprintf(buf, sizeof buf, "LEVEL %d", G.info->index);
      text_shadow(font_title, buf, 0, H * 0.2f + 10 * s, 26 * s, rgba(1, 0.8f, 0.2f, a), CW_ALIGN_CENTER, W);
      text_shadow(font_ui, G.info->title, 0, H * 0.2f + 44 * s, 28 * s, rgba(1, 1, 1, a), CW_ALIGN_CENTER, W);
    }
    if (G.toast_ticks > 0) {
      float a = cw_clampf(G.toast_ticks / 20.0f, 0, 1);
      text_shadow(font_title, "CHECKPOINT!", 0, H * 0.62f, 24 * s, rgba(0.3f, 1, 0.45f, a), CW_ALIGN_CENTER, W);
    }
    if (G.state == GS_DYING) {
      float a = 0.45f * (1 - cw_clampf(G.state_ticks / 30.0f, 0, 1));
      cw_r2d_rect(cw_rect_make(0, 0, W, H), rgba(0.8f, 0.05f, 0.05f, a));
      text_shadow(font_title, "OUCH!", 0, H * 0.42f, 36 * s, rgba(1, 0.4f, 0.35f, 1), CW_ALIGN_CENTER, W);
    }
    if (G.state == GS_CLEAR) {
      cw_r2d_rect(cw_rect_make(0, H * 0.7f, W, 112 * s), rgba(0.02f, 0.04f, 0.12f, 0.55f));
      text_shadow(font_title, "LEVEL CLEAR!", 0, H * 0.7f + 12 * s, 40 * s, rgba(0.4f, 1, 0.6f, 1), CW_ALIGN_CENTER, W);
      snprintf(buf, sizeof buf, "score %d   coins %d   gems %d", G.score, G.coins, G.gems);
      text_shadow(font_ui, buf, 0, H * 0.7f + 66 * s, 26 * s, rgba(1, 1, 1, 1), CW_ALIGN_CENTER, W);
    }
    if (G.state == GS_GAMEOVER) {
      cw_r2d_rect(cw_rect_make(0, 0, W, H), rgba(0, 0, 0, 0.5f));
      text_shadow(font_title, "GAME OVER", 0, H * 0.42f, 48 * s, rgba(1, 0.3f, 0.25f, 1), CW_ALIGN_CENTER, W);
    }
  }
  cw_r2d_end();
}

static void shutdown(void *u) {
  (void)u;
  const char *rp = getenv("PLAT3D_RECORD");
  if (rp) {
    size_t len;
    const uint8_t *rec = cw_input_recording(&len);
    FILE *fp = fopen(rp, "wb");
    if (fp && rec) fwrite(rec, 1, len, fp);
    if (fp) fclose(fp);
    cw_info("platformer3d: recorded %u ticks (%zu bytes, hash %08x) to %s", cw_input_record_ticks(), len,
            cw_input_record_hash(), rp);
  }
  if (A.on) cw_audio_shutdown();
}

cw_app_desc cw_main(int argc, char **argv) {
  (void)argc;
  (void)argv;
  cw_scene_register_builtin_types();
  game_register_types();
  cw_set_method_resolver(game_resolve_method);
#ifdef PLATFORMER3D_AUTOPILOT
  cw_set_raw_input_filter(autopilot, NULL);
#endif
  static cw_app_desc base = { .title = "Robo Hop (platformer3d)", .width = 960, .height = 540, .debug = true,
                              .init = init, .fixed_update = fixed, .update = update, .render = render, .shutdown = shutdown };
  return cw_scene_app(NULL, &base);
}
