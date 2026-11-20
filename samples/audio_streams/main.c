/* audio_streams: randomized sounds, a sound's own loop points and interactive music (cartwheel/audio.h), from C and
 * from scene players naming .cwaudio resources. The mixer runs OFFLINE (one 1/60 s slice a tick), so every check is
 * exact and every host must give the same samples.
 *
 *   tick 2   the loop points: loop.wav's sampler chunk loops frames 24000..71999. Played once as a sample and once
 *            as a stream, 150000 frames each: after frame 72000 every frame repeats the one 48000 earlier (to the
 *            bit), and the stream's frames are the sample's (loop.exact, loop.stream_same, loop.points).
 *            A randomizer from C (no repeats, pitch up to 1.2, volume 3 dB), 24 picks: never the same sound twice in
 *            a row, every pitch inside [1/1.2, 1.2] and not all the same (rnd.no_repeat, rnd.ranges, rnd.picks).
 *            (tick 2: test switches reach a cart from frame 2 on)
 *   play     the scene: Steps (an AudioStreamPlayer of steps.cwaudio) plays a step every 10 ticks to tick 80, each
 *            play picking its sound (steps.picks). Music (match.cwaudio) starts calm on tick 2; tick 90 sets its
 *            clip to battle: on calm's next bar a one-beat sting, then battle exactly one beat later; tick 240 sets
 *            calm again: on battle's next beat, a cross fade. Every switch is checked on the mixer clock: it is on its
 *            clip's grid, at least CW_MUSIC_LEAD frames after the request, and each voice started on that frame
 *            (music.grid_ok, music.filler_ok, music.order_ok, music.frames).
 *   done     tick 360: a hash of all 288000 stereo frames (streams.hash) and streams.done.
 * Controls: CW_TEST_RANDOM_ANY=1 (the randomizer may repeat) fails rnd.no_repeat; CW_TEST_LOOP_WHOLE=1 (the
 * sound loops its whole file instead of its points) fails loop.exact; CW_TEST_MUSIC_NOW=1 (match_now.cwaudio:
 * every switch at once) fails music.grid_ok. */
#include <math.h>
#include <string.h>

#include "cartwheel/audio.h"
#include "cartwheel/audio_nodes.h"
#include "cartwheel/core.h"
#include "cartwheel/debug.h"
#include "cartwheel/platform.h"
#include "cartwheel/render2d.h"
#include "cartwheel/scene.h"
#include "cartwheel/scene_file.h"

#define RATE 48000
#define SLICE (RATE / 60)
#define TICKS 360
#define LOOP_FRAMES 150000

static float *buf;
static int tick;
static uint32_t hash = 2166136261u;
static int32_t done, loop_exact, loop_same, loop_points[2], rnd_no_repeat, rnd_ranges, rnd_picks[24];
static int32_t steps_picks[8], nsteps;
static int32_t grid_ok = 1, filler_ok, order_ok, frames_dbg[6]; /* calm start, switch 1, sting start, battle start, switch 2, calm again */
static int64_t req1, req2, calm_start, sw1 = -1, sw2 = -1;
static char order[64];
static float col_peak[TICKS];

static cw_node N(const char *path) { return cw_node_get(cw_node_get(cw_tree_root(cw_scene_app_tree()), "Main"), path); }
static void fold(const void *p, size_t n) {
  const uint8_t *b = p;
  for (size_t i = 0; i < n; i++) hash = (hash ^ b[i]) * 16777619u;
}

/* one sound of loop.wav played alone for LOOP_FRAMES frames from a fresh start; out: mono (left) */
static int64_t render_loop(cw_sound s, float *out) {
  static float slice[1024 * 2];
  cw_voice v = cw_audio_play(s, NULL);
  int64_t start = -1, from = cw_audio_mixed_frames(), got = 0;
  while (got < LOOP_FRAMES + 8192) {
    cw_audio_render(slice, 1024);
    if (start < 0) start = cw_voice_started_at(v);
    for (int i = 0; i < 1024; i++) {
      int64_t f = from + got + i - (start >= 0 ? start : (int64_t)1 << 40);
      if (f >= 0 && f < LOOP_FRAMES) out[f] = slice[2 * i];
    }
    got += 1024;
  }
  cw_voice_stop(v, 0);
  cw_audio_render(slice, 1024);
  return start;
}

static void check_loop(void) {
  cw_sound smp = cw_sound_load("loop.wav", &(cw_sound_desc){ .mode = CW_SOUND_SAMPLE });
  cw_sound str = cw_sound_load("loop.wav", &(cw_sound_desc){ .mode = CW_SOUND_STREAM });
  int64_t ls = 0, le = 0;
  bool own = cw_sound_get_loop(smp, &ls, &le);
  loop_points[0] = own ? (int32_t)ls : -1;
  loop_points[1] = own ? (int32_t)le : -1;
  if (cw_test_env("CW_TEST_LOOP_WHOLE")) { /* the control: the whole file loops, not its points */
    cw_sound_set_loop(smp, true, 0, 0);
    cw_sound_set_loop(str, true, 0, 0);
  }
  float *a = cw_calloc(LOOP_FRAMES, sizeof(float), CW_MEM_GAME), *b = cw_calloc(LOOP_FRAMES, sizeof(float), CW_MEM_GAME);
  render_loop(smp, a);
  render_loop(str, b);
  int bad = 0;
  for (int i = 72000; i < LOOP_FRAMES; i++) bad += a[i] != a[i - 48000];
  int nz = 0;
  for (int i = 70000; i < 74000; i++) nz += a[i] != 0;
  loop_exact = bad == 0 && nz > 3000;
  loop_same = memcmp(a, b, LOOP_FRAMES * sizeof(float)) == 0;
  fold(a, LOOP_FRAMES * sizeof(float));
  cw_free(a);
  cw_free(b);
  cw_sound_free(smp);
  cw_sound_free(str);
}

static void check_randomizer(void) {
  cw_randomizer r = cw_randomizer_new(&(cw_randomizer_desc){
    .mode = cw_test_env("CW_TEST_RANDOM_ANY") ? CW_RANDOM_ANY : CW_RANDOM_NO_REPEAT, .random_pitch = 1.2f, .random_volume_db = 3, .seed = 11 });
  /* the sounds only need to exist: picks are not played here */
  float pcm[64] = { 0 };
  for (int i = 0; i < 3; i++) cw_randomizer_add(r, cw_sound_from_pcm(pcm, 64, 1, RATE, NULL), i == 1 ? 2.0f : 1.0f);
  rnd_no_repeat = 1;
  rnd_ranges = 1;
  float pmin = 9, pmax = 0;
  for (int i = 0; i < 24; i++) {
    float p, v;
    cw_randomizer_next(r, &p, &v);
    rnd_picks[i] = cw_randomizer_last(r);
    if (i && rnd_picks[i] == rnd_picks[i - 1]) rnd_no_repeat = 0;
    if (p < 1 / 1.2f - 1e-6f || p > 1.2f + 1e-6f || v < 0.7079f - 1e-4f || v > 1.4126f + 1e-4f) rnd_ranges = 0;
    pmin = fminf(pmin, p), pmax = fmaxf(pmax, p);
    fold(&p, 4);
    fold(&v, 4);
  }
  if (pmax - pmin < 0.05f) rnd_ranges = 0;
  fold(rnd_picks, sizeof rnd_picks);
  cw_randomizer_free(r);
}

static void init(void *u) {
  (void)u;
  buf = cw_calloc((size_t)SLICE * TICKS * 2, sizeof(float), CW_MEM_GAME);
  cw_audio_init(&(cw_audio_desc){ .sample_rate = RATE, .offline = true, .max_voices = 32 });
  cw_debug_i32("loop.exact", &loop_exact);
  cw_debug_i32("loop.stream_same", &loop_same);
  cw_debug_field("loop.points", loop_points, CW_DBG_I32, 2);
  cw_debug_i32("rnd.no_repeat", &rnd_no_repeat);
  cw_debug_i32("rnd.ranges", &rnd_ranges);
  cw_debug_field("rnd.picks", rnd_picks, CW_DBG_I32, 24);
  cw_debug_field("steps.picks", steps_picks, CW_DBG_I32, 8);
  cw_debug_i32("music.grid_ok", &grid_ok);
  cw_debug_i32("music.filler_ok", &filler_ok);
  cw_debug_i32("music.order_ok", &order_ok);
  cw_debug_field("music.frames", frames_dbg, CW_DBG_I32, 6);
  cw_debug_u32("streams.hash", &hash);
  cw_debug_i32("streams.done", &done);
}

static void note_clip(const char *c) {
  size_t L = strlen(order);
  const char *last = L ? strrchr(order, ' ') : NULL;
  if (c && (!L || strcmp(last ? last + 1 : order, c)) && L + strlen(c) + 2 < sizeof order) {
    if (L) strcat(order, " ");
    strcat(order, c);
  }
}

static void fixed(void *u, double dt) {
  (void)u, (void)dt;
  if (tick >= TICKS) return;
  cw_node steps = N("Steps"), mus = N("Music");
  if (tick == 2) { /* (test switches are read from frame 2 on: a cart host delivers them with its first request) */
    check_loop();
    check_randomizer();
    if (cw_test_env("CW_TEST_MUSIC_NOW")) cw_node_set(mus, "stream", cw_val_string("match_now.cwaudio"));
    cw_audio_player_play(mus, 0);
  }
  if (tick > 0 && tick % 10 == 0 && tick <= 80) {
    cw_audio_player_play(steps, 0);
    const cw_audio_stream_res *res = cw_audio_stream_load("steps.cwaudio");
    steps_picks[nsteps++] = cw_randomizer_last(cw_audio_stream_randomizer(res));
  }
  cw_music m = cw_audio_player_music(mus);
  if (tick == 90) {
    req1 = cw_audio_mixed_frames();
    calm_start = cw_music_clip_start(m);
    cw_node_set(mus, "clip", cw_val_string("battle"));
  }
  if (tick == 240) {
    req2 = cw_audio_mixed_frames();
    cw_node_set(mus, "clip", cw_val_string("calm"));
  }
  float *out = buf + (size_t)tick * SLICE * 2;
  cw_audio_render(out, SLICE);
  /* the switches as the mixer made them */
  cw_value cur;
  cw_node_get_prop(mus, "current_clip", &cur);
  note_clip(cur.s && *cur.s ? cur.s : NULL);
  int64_t sf = cw_music_switch_frame(m);
  if (tick >= 90 && tick < 240 && sw1 < 0 && sf >= 0) sw1 = sf;
  if (tick >= 240 && sw2 < 0 && sf >= 0) sw2 = sf;
  int64_t st = cw_music_clip_start(m);
  if (cur.s && !strcmp(cur.s, "sting") && !frames_dbg[2]) frames_dbg[2] = (int32_t)cw_voice_started_at(cw_music_voice(m));
  if (cur.s && !strcmp(cur.s, "battle") && !frames_dbg[3]) frames_dbg[3] = (int32_t)st;
  if (tick > 240 && cur.s && !strcmp(cur.s, "calm") && !frames_dbg[5]) frames_dbg[5] = (int32_t)cw_voice_started_at(cw_music_voice(m));
  float pk = 0;
  for (int i = 0; i < SLICE * 2; i++) pk = fmaxf(pk, fabsf(out[i]));
  col_peak[tick] = pk;
  if (++tick == TICKS) {
    fold(buf, (size_t)SLICE * TICKS * 2 * sizeof(float));
    fold(steps_picks, sizeof steps_picks);
    frames_dbg[0] = (int32_t)calm_start;
    frames_dbg[1] = (int32_t)sw1;
    frames_dbg[4] = (int32_t)sw2;
    const int64_t bar = 4 * 24000, beat = 24000;
    /* switch 1: on calm's bar grid, no sooner than the lead after the request; the sting on that frame, battle a beat on */
    grid_ok = sw1 >= req1 + CW_MUSIC_LEAD && (sw1 - calm_start) % bar == 0 &&
              sw2 >= req2 + CW_MUSIC_LEAD && (sw2 - frames_dbg[3]) % beat == 0;
    filler_ok = frames_dbg[2] == sw1 && frames_dbg[3] == sw1 + beat && frames_dbg[5] == sw2;
    order_ok = !strcmp(order, "calm sting battle calm");
    fold(frames_dbg, sizeof frames_dbg);
    done = 1;
    cw_info("audio_streams: hash %08x loop %d/%d rnd %d/%d music grid %d filler %d order \"%s\" frames %d %d %d %d %d %d", hash,
            loop_exact, loop_same, rnd_no_repeat, rnd_ranges, grid_ok, filler_ok, order, frames_dbg[0], frames_dbg[1],
            frames_dbg[2], frames_dbg[3], frames_dbg[4], frames_dbg[5]);
  }
}

static void rect(float x, float y, float w, float h, cw_color c) { cw_r2d_rect((cw_rect){ x, y, w, h }, c); }
static void render(void *u, double alpha) {
  (void)u, (void)alpha;
  cw_vec2 c = cw_r2d_canvas_size();
  cw_r2d_begin(&(cw_r2d_pass_desc){ .clear = true, .clear_color = cw_rgba(0.06f, 0.07f, 0.09f, 1) });
  float bw = c.x / TICKS;
  for (int t = 0; t < tick; t++) rect(t * bw, c.y * 0.75f - col_peak[t] * c.y * 0.6f, fmaxf(1, bw), col_peak[t] * c.y * 0.6f, cw_rgba(0.3f, 0.7f, 1, 1));
  for (int i = 0; i < 32; i++) {
    float v = (hash >> i) & 1 ? 1.0f : 0.2f;
    rect(8 + i * (c.x - 16) / 32, c.y * 0.85f, (c.x - 16) / 32 - 2, c.y * 0.1f, cw_rgba(v, done ? v : 0.2f, v, 1));
  }
  cw_r2d_end();
}

cw_app_desc cw_main(int argc, char **argv) {
  (void)argc, (void)argv;
  static cw_app_desc base = { .title = "audio_streams", .width = 480, .height = 270, .debug = true, .init = init, .fixed_update = fixed,
                              .render = render };
  return cw_scene_app("main.cwscene", &base);
}
