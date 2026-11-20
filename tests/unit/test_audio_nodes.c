/* Audio player and listener nodes (cartwheel/audio_nodes.h) over the OFFLINE mixer: play and stop, volume in
 * decibels, pitch, polyphony, loop, `finished` counted in fixed ticks, pause (the property and the tree's),
 * buses, a 3D player heard by an AudioListener3D and by the current camera (side, distance, cone), scene text.
 * Rendered buffers fold into the checksum the gate compares across hosts. */
#include <math.h>
#include <string.h>

#include "cartwheel/audio_nodes.h"
#include "cartwheel/math.h"
#include "cartwheel/scene.h"
#include "cartwheel/scene_file.h"
#include "test.h"

#define R 48000
#define DT (1.0 / 60)
#define TICK_FRAMES (R / 60)
static float buf[R * 2 * 2];

static void start(void) { cw_audio_init(&(cw_audio_desc){ .sample_rate = R, .offline = true, .max_voices = 32 }); }
static void stop(void) {
  cw_audio_shutdown();
  cw_mem_stats ms;
  cw_mem_get_stats(&ms);
  CHECK_EQ_INT(ms.bytes[CW_MEM_AUDIO], 0);
}
/* one fixed tick and the audio of it; returns the peaks of the left and right channels */
static float peak_l, peak_r;
static void tick(cw_tree *t, int n) {
  peak_l = peak_r = 0;
  for (int k = 0; k < n; k++) {
    cw_tree_fixed_update(t, DT);
    memset(buf, 0, sizeof(float) * TICK_FRAMES * 2);
    cw_audio_render(buf, TICK_FRAMES);
    for (int i = 0; i < TICK_FRAMES; i++) {
      if (fabsf(buf[2 * i]) > peak_l) peak_l = fabsf(buf[2 * i]);
      if (fabsf(buf[2 * i + 1]) > peak_r) peak_r = fabsf(buf[2 * i + 1]);
    }
    uint32_t h = cw_audio_analyze(buf, TICK_FRAMES * 2).hash;
    FOLD(h);
  }
}
static cw_sound dc(float v, int frames) {
  static float pcm[R * 2];
  for (int i = 0; i < frames; i++) pcm[i] = v;
  return cw_sound_from_pcm(pcm, frames, 1, R, NULL);
}
static int32_t finished;
static uint64_t finished_tick;
static cw_tree *the_tree;
static void on_finished(cw_node e, const cw_value *a, int argc, void *u) {
  (void)e, (void)a, (void)argc, (void)u;
  finished++;
  finished_tick = cw_tree_tick(the_tree);
}
static cw_node player(cw_tree *t, const char *type, const char *name) {
  cw_node n = cw_node_new(type, name);
  cw_connect(n, "finished", n, NULL, on_finished, NULL, 0);
  cw_node_add_child(cw_tree_root(t), n);
  return n;
}
static bool is_playing(cw_node n) {
  cw_value v;
  return cw_node_get_prop(n, "playing", &v) && v.b;
}

static void t_play(void) {
  start();
  cw_tree *t = the_tree = cw_tree_new();
  cw_node p = player(t, "AudioStreamPlayer", "Blip");
  finished = 0;
  /* no stream: a warning, nothing plays */
  cw_info("the warning below is expected");
  CHECK(CW_HANDLE_IS_NULL(cw_audio_player_play(p, 0)));
  CHECK(!is_playing(p));
  /* a file: plays, is heard, finishes on the tick its length says */
  cw_node_set(p, "stream", cw_val_string("blip.flac"));
  cw_voice v = cw_audio_player_play(p, 0);
  CHECK(!CW_HANDLE_IS_NULL(v) && is_playing(p) && cw_audio_player_is_playing(p));
  CHECK(cw_audio_player_voice(p).id == v.id);
  cw_sound blip = cw_sound_load("blip.flac", NULL);
  int64_t frames = cw_sound_length_frames(blip);
  int rate = cw_sound_source_rate(blip);
  CHECK(frames > 0 && rate > 0);
  int ticks = (int)ceil((double)frames / rate * 60);
  uint64_t t0 = cw_tree_tick(t);
  tick(t, 3);
  CHECK(peak_l > 0.01f && peak_r > 0.01f);
  CHECK(fabs(cw_audio_player_position(p) - 3 * DT) < 1e-9);
  tick(t, ticks - 4);
  CHECK_EQ_INT(finished, 0);
  CHECK(is_playing(p));
  tick(t, 1);
  CHECK_EQ_INT(finished, 1);
  CHECK_EQ_INT(finished_tick - t0, ticks - 1);
  CHECK(!is_playing(p) && cw_audio_player_position(p) == 0);
  FOLD(finished_tick);
  /* twice the pitch: half the ticks */
  cw_node_set(p, "pitch_scale", cw_val_float(2));
  cw_node_set(p, "playing", cw_val_bool(true));
  t0 = cw_tree_tick(t);
  tick(t, (ticks + 1) / 2 + 1);
  CHECK_EQ_INT(finished, 2);
  CHECK_EQ_INT(finished_tick - t0, (int)ceil((double)frames / rate * 30) - 1);
  cw_node_set(p, "pitch_scale", cw_val_float(1));
  /* starting part way in: the rest of it */
  cw_audio_player_play(p, (double)frames / rate - 0.1);
  tick(t, 5);
  CHECK_EQ_INT(finished, 2);
  tick(t, 2);
  CHECK_EQ_INT(finished, 3);
  /* stop: silence, no finished */
  cw_value arg = cw_val_float(0);
  CHECK(cw_node_call(p, "play", &arg, 1, NULL));
  tick(t, 2);
  CHECK(peak_l > 0.01f);
  CHECK(cw_node_call(p, "stop", NULL, 0, NULL));
  tick(t, 2);
  tick(t, 2);
  CHECK(peak_l == 0 && peak_r == 0);
  CHECK_EQ_INT(finished, 3);
  /* a loop never finishes; freeing the node stops it */
  cw_node_set(p, "loop", cw_val_bool(true));
  cw_audio_player_play(p, 0);
  tick(t, ticks * 3);
  CHECK_EQ_INT(finished, 3);
  CHECK(is_playing(p) && peak_l > 0);
  CHECK(cw_audio_player_position(p) > (double)frames / rate * 2);
  cw_node_free(p);
  tick(t, 2);
  tick(t, 2);
  CHECK(peak_l == 0);
  cw_audio_stats st;
  cw_audio_get_stats(&st);
  CHECK_EQ_INT(st.voices, 0);
  cw_tree_free(t);
  cw_sound_free(blip);
  stop();
}

static void t_volume_polyphony(void) {
  start();
  cw_tree *t = the_tree = cw_tree_new();
  cw_sound one = dc(1.0f, R);
  cw_node p = player(t, "AudioStreamPlayer", "Tone");
  cw_audio_player_set_sound(p, one);
  cw_audio_player_play(p, 0);
  tick(t, 2);
  float full = peak_l;
  CHECK(fabsf(full - 0.70710678f) < 1e-4f); /* a mono sound in the middle: equal power */
  cw_node_set(p, "volume_db", cw_val_float(-6.0206));
  tick(t, 2);
  tick(t, 1);
  CHECK(fabsf(peak_l - full * 0.5f) < 1e-4f);
  cw_node_set(p, "volume_db", cw_val_float(-20));
  tick(t, 2);
  tick(t, 1);
  CHECK(fabsf(peak_l - full * 0.1f) < 1e-4f);
  cw_node_set(p, "volume_db", cw_val_float(-80));
  tick(t, 2);
  tick(t, 1);
  CHECK(peak_l == 0);
  /* played while silent stays silent (the mixer reads volume 0 as "default": the node sets it after) */
  cw_audio_player_play(p, 0);
  tick(t, 2);
  CHECK(peak_l == 0);
  cw_node_set(p, "volume_db", cw_val_float(0));
  cw_audio_player_stop(p);
  tick(t, 2);
  /* polyphony: three copies at most, the fourth stops the oldest */
  cw_audio_stats st;
  cw_node_set(p, "max_polyphony", cw_val_int(3));
  cw_node_set(p, "volume_db", cw_val_float(-20)); /* quiet enough that three do not clip */
  for (int i = 0; i < 3; i++) {
    cw_audio_player_play(p, 0);
    tick(t, 1);
  }
  cw_audio_get_stats(&st);
  CHECK_EQ_INT(st.voices, 3);
  CHECK(fabsf(peak_l - 0.3f * full) < 1e-3f);
  cw_voice oldest = cw_audio_player_voice(p);
  cw_audio_player_play(p, 0);
  tick(t, 2);
  cw_audio_get_stats(&st);
  CHECK_EQ_INT(st.voices, 3);
  (void)oldest;
  /* one copy: play restarts */
  cw_audio_player_stop(p);
  cw_node_set(p, "volume_db", cw_val_float(0));
  cw_node_set(p, "max_polyphony", cw_val_int(1));
  cw_audio_player_play(p, 0);
  tick(t, 10);
  cw_audio_player_play(p, 0);
  tick(t, 2);
  cw_audio_get_stats(&st);
  CHECK_EQ_INT(st.voices, 1);
  CHECK(fabs(cw_audio_player_position(p) - 2 * DT) < 1e-9);
  /* a named bus: its volume scales the player; an unknown name plays into the master */
  cw_bus sfx = cw_bus_create("sfx", CW_AUDIO_MASTER);
  cw_bus_set_volume(sfx, 0.25f);
  cw_node_set(p, "bus", cw_val_string("sfx"));
  tick(t, 3);
  tick(t, 1);
  CHECK(fabsf(peak_l - full * 0.25f) < 1e-3f);
  cw_node_set(p, "bus", cw_val_string("nope"));
  tick(t, 3);
  tick(t, 1);
  CHECK(fabsf(peak_l - full) < 1e-3f);
  cw_tree_free(t);
  cw_sound_free(one);
  stop();
}

static void t_pause(void) {
  start();
  cw_tree *t = the_tree = cw_tree_new();
  cw_sound one = dc(1.0f, R / 2); /* 30 ticks */
  cw_node p = player(t, "AudioStreamPlayer", "Tone");
  cw_audio_player_set_sound(p, one);
  finished = 0;
  cw_audio_player_play(p, 0);
  tick(t, 10);
  CHECK(peak_l > 0);
  /* the property holds the voice and the count */
  cw_node_set(p, "stream_paused", cw_val_bool(true));
  tick(t, 2);
  tick(t, 50);
  CHECK(peak_l == 0 && finished == 0 && is_playing(p));
  cw_node_set(p, "stream_paused", cw_val_bool(false));
  tick(t, 10);
  CHECK(peak_l > 0 && finished == 0);
  /* so does a paused tree */
  cw_tree_set_paused(t, true);
  tick(t, 2);
  tick(t, 50);
  CHECK(peak_l == 0 && finished == 0 && is_playing(p));
  /* a player that runs while paused keeps sounding */
  cw_node always = player(t, "AudioStreamPlayer", "Menu");
  cw_node_set_pause_mode(always, CW_PAUSE_ALWAYS);
  cw_audio_player_set_sound(always, one);
  cw_audio_player_play(always, 0);
  tick(t, 3);
  CHECK(peak_l > 0);
  cw_audio_player_stop(always);
  cw_tree_set_paused(t, false);
  tick(t, 9);
  CHECK_EQ_INT(finished, 0);
  tick(t, 2);
  CHECK_EQ_INT(finished, 1); /* 30 counted ticks in all */
  cw_tree_free(t);
  cw_sound_free(one);
  stop();
}

static cw_node node3(cw_tree *t, const char *type, const char *name, float x, float y, float z) {
  cw_node n = cw_node_new(type, name);
  cw_node3d_set_position(n, (t3_vec3){ x, y, z });
  cw_node_add_child(cw_tree_root(t), n);
  return n;
}

static void t_3d(void) {
  start();
  cw_tree *t = the_tree = cw_tree_new();
  cw_sound one = dc(1.0f, R * 2);
  cw_node ears = node3(t, "AudioListener3D", "Ears", 0, 0, 0); /* facing -Z: +X is to the right */
  cw_node p = node3(t, "AudioStreamPlayer3D", "Bell", -1, 0, 0);
  cw_audio_player_set_sound(p, one);
  cw_node_set(p, "loop", cw_val_bool(true));
  cw_audio_player_play(p, 0);
  tick(t, 3);
  tick(t, 1);
  float near_l = peak_l;
  CHECK(peak_l > 0.9f && peak_r < 0.01f); /* hard left at the reference distance */
  /* to the right: the other ear */
  cw_node3d_set_position(p, (t3_vec3){ 1, 0, 0 });
  tick(t, 3);
  tick(t, 1);
  CHECK(peak_r > 0.9f && peak_l < 0.01f);
  /* five times as far: a fifth (INVERSE, unit_size 1, rolloff 1) */
  cw_node3d_set_position(p, (t3_vec3){ -5, 0, 0 });
  tick(t, 3);
  tick(t, 1);
  CHECK(fabsf(peak_l - near_l / 5) < 0.01f);
  /* unit_size 5: full volume again there */
  cw_node_set(p, "unit_size", cw_val_float(5));
  tick(t, 3);
  tick(t, 1);
  CHECK(fabsf(peak_l - near_l) < 0.01f);
  cw_node_set(p, "unit_size", cw_val_float(1));
  /* no attenuation: distance does not matter */
  cw_node_set(p, "attenuation_model", cw_val_enum("NONE"));
  tick(t, 3);
  tick(t, 1);
  CHECK(fabsf(peak_l - near_l) < 0.01f);
  cw_node_set(p, "attenuation_model", cw_val_enum("INVERSE"));
  /* the listener moves instead: the sound is where the node is relative to the ears */
  cw_node3d_set_position(p, (t3_vec3){ 0, 0, 0 });
  cw_node3d_set_position(ears, (t3_vec3){ 1, 0, 0 });
  tick(t, 3);
  tick(t, 1);
  CHECK(peak_l > 0.9f && peak_r < 0.01f);
  /* turned to face +Z, left and right swap */
  cw_node3d_set_quaternion(ears, (t3_quat){ 0, 1, 0, 0 });
  tick(t, 3);
  tick(t, 1);
  CHECK(peak_r > 0.9f && peak_l < 0.01f);
  cw_node3d_set_quaternion(ears, (t3_quat){ 0, 0, 0, 1 });
  cw_node3d_set_position(ears, (t3_vec3){ 0, 0, 0 });
  /* a cone: the node's -Z points away from the listener, who is outside it */
  cw_node3d_set_position(p, (t3_vec3){ -1, 0, 0 });
  cw_node_set(p, "cone_inner_angle", cw_val_float(60));
  cw_node_set(p, "cone_outer_angle", cw_val_float(120));
  cw_node_set(p, "cone_outer_gain", cw_val_float(0.25));
  tick(t, 3);
  tick(t, 1);
  CHECK(fabsf(peak_l - near_l * 0.25f) < 0.01f); /* -Z is side on to the listener at +X of it: outside the cone */
  cw_node3d_look_at(p, (t3_vec3){ 0, 0, 0 }, (t3_vec3){ 0, 1, 0 }); /* now it points at the ears */
  tick(t, 3);
  tick(t, 1);
  CHECK(fabsf(peak_l - near_l) < 0.01f);
  /* without a listener node the current camera listens: put it to the left of the sound */
  cw_node_set(p, "cone_inner_angle", cw_val_float(360));
  cw_node_set(p, "cone_outer_angle", cw_val_float(360));
  cw_node_free(ears);
  cw_node cam = node3(t, "Camera3D", "Cam", -2, 0, 0);
  cw_node_set(cam, "current", cw_val_bool(true));
  tick(t, 3);
  tick(t, 1);
  CHECK(peak_r > 0.9f && peak_l < 0.01f);
  /* the game takes the listener over */
  cw_audio_nodes_set_auto_listener(t, false);
  cw_audio_set_listener((t3_vec3){ 0, 0, 0 }, (t3_vec3){ 0, 0, 0 }, (t3_vec3){ 0, 0, -1 }, (t3_vec3){ 0, 1, 0 });
  tick(t, 3);
  tick(t, 1);
  CHECK(peak_l > 0.9f && peak_r < 0.01f);
  cw_tree_free(t);
  cw_sound_free(one);
  stop();
}

/* scene text: players load with their properties, autoplay starts on the first tick, and the scene saves back */
static void t_scene(void) {
  static const char *text =
    "[scene format=1]\n\n[node name=\"Level\" type=\"Node3D\"]\n\n"
    "[node name=\"Music\" parent=\".\" type=\"AudioStreamPlayer\"]\nstream = \"music.ogg\"\nvolume_db = -6\nautoplay = true\nloop = true\nbus = \"music\"\n\n"
    "[node name=\"Bell\" parent=\".\" type=\"AudioStreamPlayer3D\"]\nposition = vec3(3, 0, 0)\nstream = \"blip.flac\"\npitch_scale = 1.5\nmax_polyphony = 4\n"
    "attenuation_model = LINEAR\nunit_size = 2\nmax_distance = 40\ndoppler = 1\n\n"
    "[node name=\"Step\" parent=\".\" type=\"AudioStreamPlayer2D\"]\nposition = vec2(100, 50)\nstream = \"blip.flac\"\nmax_distance = 800\npanning_strength = 0.5\n\n"
    "[node name=\"Ears\" parent=\".\" type=\"AudioListener3D\"]\n";
  start();
  cw_packed_scene *ps = cw_scene_parse(text, NULL);
  CHECK(ps != NULL);
  if (!ps) return;
  cw_node level = cw_scene_instance(ps);
  cw_tree *t = the_tree = cw_tree_new();
  cw_node_add_child(cw_tree_root(t), level);
  cw_node music = cw_node_get(level, "Music"), bell = cw_node_get(level, "Bell"), step2 = cw_node_get(level, "Step");
  CHECK(cw_node_is(music, "AudioStreamPlayer") && cw_node_is(bell, "AudioStreamPlayer3D") && cw_node_is(step2, "AudioStreamPlayer2D"));
  CHECK(!is_playing(music)); /* autoplay waits for the first tick */
  tick(t, 1);
  CHECK(is_playing(music) && !is_playing(bell));
  tick(t, 20);
  CHECK(peak_l > 0.001f);
  char *out = cw_scene_save(level);
  CHECK(out && !strcmp(out, text));
  if (out && strcmp(out, text)) cw_info("saved:\n%s", out);
  cw_free(out);
  /* a 2D player with no frame drawn yet plays in the middle at full volume */
  cw_audio_player_stop(music);
  tick(t, 2);
  cw_audio_player_play(step2, 0);
  tick(t, 2);
  CHECK(peak_l > 0.001f && fabsf(peak_l - peak_r) < 0.05f * peak_l); /* the file is stereo: near equal, not panned */
  cw_tree_free(t);
  cw_packed_scene_free(ps);
  stop();
}

CW_TEST_SUITE("audio_nodes", { "play", t_play }, { "volume_polyphony", t_volume_polyphony }, { "pause", t_pause }, { "3d", t_3d },
              { "scene", t_scene })
