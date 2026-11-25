/* story_scene, speech_face, director_shots: the story runtime, voiced lines with faces, and the cinematic director
 * (cartwheel/story.h, cartwheel/speech.h, cartwheel/director.h), one source built three ways (STORY_MODE).
 *
 * The stage: a keeper and a traveller (character_skin's generated character, morph targets smile, jawOpen, browUp,
 * blink) face each other in a walled yard; a third figure, the passer-by, walks across in director_shots.
 *
 * story_scene (STORY_MODE 0): the script samples/story_scene/story.story (compiled to assets/main.cwstory; its lines
 * voiced by tools/speech-bake.mjs into assets/main.cwspeech). The gate scene starts at tick 30 and plays on the pad
 * alone: South ends a line early and picks an option, up and down move the cursor, East held skips the scene. The
 * script's shots cut the director; the speakers' faces take each line's mouth and emotion; subtitles and the choice
 * menu are drawn by cw_story_draw. After the scene the keeper barks from his pool every few seconds.
 *   STORY_WAIT=1: lines wait for South after they end (visual novel pacing).
 *
 * speech_face (STORY_MODE 1): a close-up on the keeper speaking every line of the pack in turn, 0.6 s apart, each with
 * the next emotion (calm, joy, anger, sorrow, fear, surprise), through cw_speech_mouth and cw_face. Debug fields
 * face.jaw_speaking (the largest jaw weight while a line plays), face.jaw_silent (the largest in the gaps after the
 * mouth has had 0.25 s to close), face.smile_joy (the largest smile during the joy line), face.brow_surprise, and
 * the checks face.mouth_ok (jaw_speaking over 0.35 and jaw_silent under 0.05) and face.emotion_ok (smile_joy and
 * brow_surprise over 0.5), face.lines_done.
 *   SPEECH_FACE_OFF=1: the face is never driven (the gate's control: the mouth must be seen to move).
 *
 * director_shots (STORY_MODE 2): every shot type in turn (wide, two, over the shoulder, a close-up the passer-by
 * walks straight through, push, react, hero, top, insert, whip, release), each solved against the three figures and
 * the walls every frame.
 *   DIRECTOR_NAIVE=1: no solving (the first placement of each type, never re-scored): the passer-by crosses the
 *   close-up's line and dir.blocked_frames counts it (a control).
 *   DIRECTOR_NOISE=1: 1 cm of handheld-style noise on the output: dir.reversals counts it (a control).
 *
 * Debug fields: story.*, speech.*, dir.* (see the headers), face.keeper.w / face.traveller.w, sample.tick. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cartwheel/anim.h"
#include "cartwheel/debug.h"
#include "cartwheel/director.h"
#include "cartwheel/input.h"
#include "cartwheel/platform.h"
#include "cartwheel/render2d.h"
#include "cartwheel/resource.h"
#include "cartwheel/scene_file.h"
#include "cartwheel/speech.h"
#include "cartwheel/story.h"
#include "cartwheel/ui.h"

#ifndef STORY_MODE
#define STORY_MODE 0
#endif
enum { KEEPER, TRAVELLER, PASSER, NFIG };

/* the yard's walls (boxes in main.cwscene: keep both in step) */
typedef struct { float lo[3], hi[3]; } box_t;
static const box_t WALLS[] = {
  { { -5, 0, -3.2f }, { 5, 2.6f, -2.9f } },      /* behind the keeper */
  { { 3.4f, 0, -3 }, { 3.7f, 2.6f, 4 } },        /* the east wall */
  { { -2.6f, 0, -1.2f }, { -2.1f, 2.4f, -.7f } }, /* a pillar by the keeper */
};

static struct {
  cw_res model;
  cw_node fig[NFIG], cam;
  t3_object *obj[NFIG];
  float yaw[NFIG];
  cw_face *face[NFIG];
  cw_story *story;
  cw_speech *speech;
  cw_director *dir;
  bool started, configured, face_off, naive, noise;
  int32_t tick;
  float bark_t;
  /* speech_face */
  int line, gap_ticks; float line_t; bool in_line;
  float jaw_speaking, jaw_silent, smile_joy, brow_surprise;
  int32_t mouth_ok, emotion_ok, lines_done;
  /* director_shots */
  int shot_i; float shot_clock;
} G;

static bool ray_box(t3_vec3 a, t3_vec3 b, const box_t *x, float *t) {
  float o[3] = { a.x, a.y, a.z }, d[3] = { b.x - a.x, b.y - a.y, b.z - a.z }, t0 = 0, t1 = 1;
  for (int k = 0; k < 3; k++) {
    if (fabsf(d[k]) < 1e-9f) { if (o[k] < x->lo[k] || o[k] > x->hi[k]) return false; continue; }
    float u = (x->lo[k] - o[k]) / d[k], v = (x->hi[k] - o[k]) / d[k];
    if (u > v) { float w = u; u = v; v = w; }
    if (u > t0) t0 = u;
    if (v < t1) t1 = v;
    if (t0 > t1) return false;
  }
  *t = t0;
  return true;
}
static bool world_ray(t3_vec3 a, t3_vec3 b, float *t, void *u) {
  (void)u;
  float best = 2;
  for (size_t i = 0; i < sizeof WALLS / sizeof *WALLS; i++) { float tt; if (ray_box(a, b, &WALLS[i], &tt) && tt < best) best = tt; }
  if (b.y < 0 && a.y >= 0) { float tg = a.y / (a.y - b.y); if (tg < best) best = tg; }
  if (best > 1) return false;
  *t = best;
  return true;
}

static t3_vec3 bone(int f, const char *name, float up) {
  t3_object *b = G.obj[f] ? t3_object_get_by_name(G.obj[f], name) : NULL;
  if (b) { t3_object_update_matrix_world(b, false); return t3_object_get_world_position(b); }
  t3_vec3 p = cw_node3d_position(G.fig[f]);
  p.y += up;
  return p;
}
static void subjects(void) {
  for (int f = 0; f < NFIG; f++) {
    if (!G.dir) break;
    t3_vec3 p = cw_node3d_position(G.fig[f]);
    float yaw = G.yaw[f];
    t3_vec3 head = bone(f, "Head", 1.6f), eye = { head.x + sinf(yaw) * .09f, head.y + .1f, head.z + cosf(yaw) * .09f };
    cw_dir_subject s = { .eye = eye, .chest = bone(f, "Chest", 1.3f), .feet = p, .face = { sinf(yaw), 0, cosf(yaw) },
                         .hand_l = bone(f, "HandL", .9f), .hand_r = bone(f, "HandR", .9f), .radius = .24f, .head_radius = .15f,
                         .visible = cw_node_visible_in_tree(G.fig[f]) };
    cw_director_set_subject(G.dir, f, &s);
  }
}

static void on_shot(const cw_story_shot *s, void *u) { (void)u; if (G.dir) cw_director_cut_story(G.dir, s, NULL); }
static void on_scene_end(const char *scene, bool skipped, void *u) { (void)scene, (void)skipped, (void)u; if (G.dir) cw_director_release(G.dir, .8f); }

static void add_figure(int f, const char *name, t3_vec3 at, float yaw) {
  cw_node level = cw_node_get(cw_tree_root(cw_scene_app_tree()), "Level");
  cw_node n = cw_node_new("Node3D", name);
  cw_node_add_child(level, n);
  cw_node3d_set_position(n, at);
  cw_node3d_set_rotation(n, (t3_vec3){ 0, yaw, 0 });
  G.yaw[f] = yaw;
  t3_object *o = cw_gltf_instantiate(G.model);
  cw_node3d_set_object(n, o);
  t3_release(o);
  G.obj[f] = o;
  G.fig[f] = n;
  const cw_gltf *g = cw_res_gltf(G.model);
  cw_node anim = cw_node_new("AnimationPlayer", "Anim");
  cw_node_add_child(n, anim);
  for (int k = 0; g && k < g->gltf->animation_count; k++) {
    cw_anim a = cw_anim_from_t3_clip(g->gltf->animations[k], "");
    cw_anim_player_add(anim, g->gltf->animations[k]->name, a);
    cw_anim_release(a);
  }
  cw_anim_player_play_ex(anim, "idle", 0, 0.9f + 0.1f * (float)f, false);
  G.face[f] = cw_face_new(o, NULL);
  cw_face_set_blinking(G.face[f], true, (uint32_t)f + 1);
}

static void init(void *u) {
  (void)u;
  /* the pad's menu actions (a game with the shell has these already) */
  cw_action_bind_text(cw_action_add("ui_accept", .5f), "pad:south, key:return, key:space");
  cw_action_bind_text(cw_action_add("ui_back", .5f), "pad:east, key:escape");
  cw_action_bind_text(cw_action_add("ui_up", .5f), "pad:up, key:up, axis:ly-");
  cw_action_bind_text(cw_action_add("ui_down", .5f), "pad:down, key:down, axis:ly+");
  G.model = cw_res_load("character.glb", CW_RES_GLTF);
  if (!cw_res_gltf(G.model)) { cw_error("story_scene: character.glb did not load"); return; }
  add_figure(KEEPER, "Keeper", (t3_vec3){ 0, 0, 0 }, 0);
  add_figure(TRAVELLER, "Traveller", (t3_vec3){ 0, 0, 1.6f }, 3.14159f);
  add_figure(PASSER, "Passer", (t3_vec3){ -3, 0, .75f }, 1.5708f);
  cw_node_set(G.fig[PASSER], "visible", cw_val_bool(STORY_MODE == 2));
  G.cam = cw_node_get(cw_tree_root(cw_scene_app_tree()), "Level/Camera");
  G.speech = cw_speech_load("main.cwspeech");
  cw_speech_publish(G.speech, "speech");
  if (STORY_MODE == 0) {
    G.story = cw_story_new(&(cw_story_desc){ .path = "main.cwstory", .speech = G.speech, .on_shot = on_shot, .on_scene_end = on_scene_end });
    cw_story_publish(G.story, "story");
  }
  cw_face_publish(G.face[KEEPER], "face.keeper");
  cw_face_publish(G.face[TRAVELLER], "face.traveller");
  cw_debug_i32("sample.tick", &G.tick);
  cw_debug_f32("face.jaw_speaking", &G.jaw_speaking);
  cw_debug_f32("face.jaw_silent", &G.jaw_silent);
  cw_debug_f32("face.smile_joy", &G.smile_joy);
  cw_debug_f32("face.brow_surprise", &G.brow_surprise);
  cw_debug_i32("face.mouth_ok", &G.mouth_ok);
  cw_debug_i32("face.emotion_ok", &G.emotion_ok);
  cw_debug_i32("face.lines_done", &G.lines_done);
}

/* the switches reach a cart host at frame 2 (cw_test_env) */
static void configure(void) {
  G.face_off = cw_test_env("SPEECH_FACE_OFF") != NULL;
  G.naive = cw_test_env("DIRECTOR_NAIVE") != NULL;
  G.noise = cw_test_env("DIRECTOR_NOISE") != NULL;
  G.dir = cw_director_new(&(cw_director_desc){ .aspect = 16.f / 9, .ray = world_ray, .no_solve = G.naive, .test_noise = G.noise ? .01f : 0 });
  cw_director_publish(G.dir, "dir");
  cw_director_set_gameplay(G.dir, &(cw_dir_camera){ { 2.6f, 1.7f, 3.6f }, { 0, 1.2f, .8f }, 40 });
  if (G.story && cw_test_env("STORY_WAIT")) cw_story_free(G.story),
    G.story = cw_story_new(&(cw_story_desc){ .path = "main.cwstory", .speech = G.speech, .on_shot = on_shot, .on_scene_end = on_scene_end, .wait_for_advance = true }),
    cw_story_publish(G.story, "story");
  G.configured = true;
}

/* speech_face: every line in turn */
static const uint32_t *pack_keys(int *n) {
  static uint32_t keys[64]; static int count = -1;
  if (count < 0) {
    /* the story's own lines, in the script's order (the pack is sorted by key) */
    static const char *const L[][2] = {
      { "keeper", "Halt. Nobody crosses the bridge after dark." }, { "keeper", "The coast. Go on, then, and mind the third plank." },
      { "keeper", "Then your business waits here until morning." }, { "keeper", "Silence. Fine. Sit by the fire until you find your tongue." },
      { "keeper", "You came back." }, { "keeper", "Lanterns are lit at the far side." } };
    count = 0;
    for (size_t i = 0; i < sizeof L / sizeof *L; i++) keys[count++] = cw_speech_key(L[i][0], L[i][1]);
  }
  *n = count;
  return keys;
}
static void speech_tick(float dt) {
  int n; const uint32_t *keys = pack_keys(&n);
  cw_face *f = G.face[KEEPER];
  float open = 0, shape = .5f;
  if (G.in_line) {
    G.line_t += dt;
    cw_speech_mouth(G.speech, keys[G.line], G.line_t, &open, &shape);
    if (G.line_t >= cw_speech_seconds(G.speech, keys[G.line])) { G.in_line = false; G.gap_ticks = 0; G.line = (G.line + 1) % n; G.lines_done++; cw_speech_set_playing(G.speech, 0); }
  } else if (++G.gap_ticks >= 36) {
    G.in_line = true; G.line_t = 0;
    cw_speech_play(G.speech, keys[G.line], NULL);
    cw_speech_set_playing(G.speech, keys[G.line]);
  }
  cw_emotion emo = (cw_emotion)(G.line % CW_EMO_COUNT);
  if (!G.face_off) {
    cw_face_set_mouth(f, open, shape);
    cw_face_set_emotion(f, G.in_line ? emo : CW_EMO_CALM, G.in_line ? 1 : 0);
    cw_face_update(f, dt);
  }
  float jaw = cw_face_weight(f, CW_FACE_JAW_OPEN);
  if (G.in_line && jaw > G.jaw_speaking) G.jaw_speaking = jaw;
  if (!G.in_line && G.gap_ticks > 15 && jaw > G.jaw_silent) G.jaw_silent = jaw;
  if (G.in_line && emo == CW_EMO_JOY) G.smile_joy = fmaxf(G.smile_joy, cw_face_weight(f, CW_FACE_SMILE));
  if (G.in_line && emo == CW_EMO_SURPRISE) G.brow_surprise = fmaxf(G.brow_surprise, cw_face_weight(f, CW_FACE_BROW_UP));
  /* the checks: the jaw opens with the sound and closes in the gaps; joy smiles, surprise lifts the brows */
  G.mouth_ok = G.jaw_speaking > .35f && G.jaw_silent < .05f;
  G.emotion_ok = G.smile_joy > .5f && G.brow_surprise > .5f;
  cw_speech_tick(G.speech, dt);
}

/* director_shots: the reel */
typedef struct { float at; cw_shot shot; } reel_t;
static const reel_t REEL[] = {
  { .5f, { .type = CW_SHOT_WIDE, .subj = { KEEPER, TRAVELLER }, .nsubj = 2 } },
  { 2.5f, { .type = CW_SHOT_TWO, .subj = { KEEPER, TRAVELLER }, .nsubj = 2 } },
  { 4.5f, { .type = CW_SHOT_OTS, .subj = { TRAVELLER, KEEPER }, .nsubj = 2 } },
  { 6.5f, { .type = CW_SHOT_CU, .subj = { KEEPER, TRAVELLER }, .nsubj = 2 } }, /* the passer-by crosses here */
  { 10.f, { .type = CW_SHOT_PUSH, .subj = { TRAVELLER }, .nsubj = 1, .dur = 2.5f } },
  { 12.5f, { .type = CW_SHOT_REACT, .subj = { KEEPER, TRAVELLER }, .nsubj = 2 } },
  { 14.5f, { .type = CW_SHOT_HERO, .subj = { TRAVELLER }, .nsubj = 1 } },
  { 16.5f, { .type = CW_SHOT_TOP, .subj = { KEEPER, TRAVELLER }, .nsubj = 2 } },
  { 18.5f, { .type = CW_SHOT_INSERT, .subj = { KEEPER }, .nsubj = 1, .part = CW_SHOT_PART_HEAD } },
  { 20.f, { .type = CW_SHOT_WHIP, .subj = { TRAVELLER }, .nsubj = 1 } },
  { 22.f, { .type = CW_SHOT_RELEASE, .dur = .8f } },
};
static void director_tick(float dt) {
  G.shot_clock += dt;
  float t = G.shot_clock;
  /* the passer-by walks across the yard from 6.6 s to 9.6 s, 0.75 m in front of the keeper */
  float k = (t - 6.6f) / 3.f;
  cw_node3d_set_position(G.fig[PASSER], (t3_vec3){ -3 + 6 * (k < 0 ? 0 : k > 1 ? 1 : k), 0, .75f });
  while (G.shot_i < (int)(sizeof REEL / sizeof *REEL) && t >= REEL[G.shot_i].at) cw_director_cut(G.dir, &REEL[G.shot_i++].shot);
}

static void fixed(void *u, double dt_) {
  (void)u;
  float dt = (float)dt_;
  if (!cw_res_gltf(G.model)) return;
  if (!G.configured) { if (cw_frame_index() < 2) return; configure(); }
  G.tick++;
  subjects();
  if (STORY_MODE == 0) {
    if (!G.started && G.tick >= 30) { G.started = cw_story_play(G.story, "gate"); }
    cw_story_tick(G.story, dt);
    if (G.started && !cw_story_active(G.story) && (G.bark_t += dt) > 3) { G.bark_t = 0; cw_story_bark(G.story, "keeper.idle"); }
    cw_speech_tick(G.speech, dt);
    cw_speech_drive_faces(G.story, G.speech, G.face, 2, dt, true);
  } else if (STORY_MODE == 1) speech_tick(dt);
  else director_tick(dt);
  cw_dir_camera cam;
  if (STORY_MODE == 1) cam = (cw_dir_camera){ { .38f, 1.68f, .62f }, { 0, 1.62f, .05f }, 30 };
  else if (!cw_director_update(G.dir, dt, &cam)) cam = (cw_dir_camera){ { 2.6f, 1.7f, 3.6f }, { 0, 1.2f, .8f }, 40 };
  if (G.dir) { /* the log names each frame a measure counts (what to look at when a gate fails) */
    static float last[3];
    float m[3] = { cw_director_measure(G.dir, CW_DIR_M_BLOCKED), cw_director_measure(G.dir, CW_DIR_M_AWAY), cw_director_measure(G.dir, CW_DIR_M_OUT) };
    for (int k = 0; k < 3; k++) if (m[k] > last[k]) cw_info("director: %s at tick %d (shot %d, solved %d)", k == 0 ? "blocked" : k == 1 ? "away" : "out", G.tick, G.shot_i - 1, cw_director_solved_type(G.dir)), last[k] = m[k];
  }
  cw_node3d_set_position(G.cam, cam.pos);
  cw_node3d_look_at(G.cam, cam.look, (t3_vec3){ 0, 1, 0 });
  cw_node_set(G.cam, "fov", cw_val_float(cam.fov));
}

static void render(void *u, double alpha) {
  (void)u, (void)alpha;
  if (STORY_MODE != 0 || !G.story) return;
  cw_r2d_begin(NULL);
  cw_story_draw(G.story, cw_ui_font_sans(), (cw_rect){ 0 });
  cw_r2d_end();
}

static void shutdown(void *u) {
  (void)u;
  cw_story_free(G.story);
  cw_director_free(G.dir);
  for (int f = 0; f < NFIG; f++) cw_face_free(G.face[f]);
  cw_speech_free(G.speech);
  cw_res_release(G.model);
}

cw_app_desc cw_main(int argc, char **argv) {
  (void)argc, (void)argv;
  static cw_app_desc base = { .title = STORY_MODE == 0 ? "story_scene" : STORY_MODE == 1 ? "speech_face" : "director_shots",
                              .width = 960, .height = 540, .debug = true, .init = init, .fixed_update = fixed, .render = render,
                              .shutdown = shutdown };
  return cw_scene_app("main.cwscene", &base);
}
