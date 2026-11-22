/* audio_tempo: tempo with the pitch kept (cw_voices_set_tempo, cartwheel/audio.h).
 *
 * A two-bar loop at 120 BPM made here as five stems (kick, clap, bass, chords, lead), each a stream started on the
 * same mixer frame. Left / right (or the shoulders) change the tempo of all five together in 5% steps from 50% to
 * 200%: faster or slower, in the same key. cw_voices_set_tempo hands the five changes to the mixer as one command, so
 * the stems change on the same block and stay locked to the sample. The song position (bar and beat) comes from the
 * stems' clock, cw_voice_frames_played, which counts SOURCE frames at any tempo, so the beat lights stay on the music.
 *
 * CW_TEMPO_OFFLINE=1 mixes offline, 800 frames a tick (what the gate runs: the same clocks on every host); audio
 * starts at frame 1 so a cart host's env request is in. State: tempo.percent, tempo.position (stem 0's clock),
 * tempo.locked (every stem's clock equal), tempo.beat. */
#include <stdio.h>
#include <string.h>

#include "cartwheel/audio.h"
#include "cartwheel/core.h"
#include "cartwheel/debug.h"
#include "cartwheel/input.h"
#include "cartwheel/math.h"
#include "cartwheel/platform.h"
#include "cartwheel/render2d.h"
#include "cartwheel/ui.h"

#define RATE 48000
#define BPM 120
#define BEAT (RATE * 60 / BPM)  /* frames per beat */
#define LEN (BEAT * 8)          /* two bars of four */
#define STEMS 5

static const char *stem_names[STEMS] = { "kick", "clap", "bass", "chords", "lead" };

typedef struct {
  bool on, offline;
  cw_action down, up;
  cw_sound sound[STEMS];
  cw_voice voice[STEMS];
  int step; /* tempo in 5% steps from 100% */
  int32_t percent, locked, beat;
  int64_t clock[STEMS];
  uint32_t position;
} tempo_app;
static tempo_app app;

/* ── the song ─────────────────────────────────────────────────────────── */

static float note_hz(int semis_from_a4) { return 440.0f * cw_pow(2.0f, (float)semis_from_a4 / 12.0f); }

/* One stem, mono, LEN frames. Chords: A minor then F major, a bar each. */
static void synth(int stem, float *out) {
  static const int roots[2] = { -36, -40 };                         /* A1, F1 */
  static const int triads[2][3] = { { 0, 3, 7 }, { 0, 4, 7 } };      /* minor, major */
  cw_rng rng;
  cw_rng_seed(&rng, 11);
  memset(out, 0, LEN * sizeof(float));
  for (int i = 0; i < LEN; i++) {
    int bar = i / (BEAT * 4), in_beat = i % BEAT, beat = i / BEAT;
    float tb = (float)in_beat / RATE, x = 0;
    switch (stem) {
    case 0: { /* kick on every beat: a falling sine */
      float f = 45.0f + 75.0f * cw_exp(-tb * 30.0f);
      static float ph;
      if (in_beat == 0) ph = 0;
      ph += CW_TAU * f / RATE;
      x = 0.8f * cw_sin(ph) * cw_exp(-tb * 9.0f);
      break;
    }
    case 1: /* clap on two and four: a short noise burst */
      if (beat % 2 == 1) x = 0.35f * (cw_rng_float(&rng) * 2 - 1) * cw_exp(-tb * 28.0f);
      break;
    case 2: { /* bass: eighth notes on the root, a few harmonics */
      float t8 = (float)(i % (BEAT / 2)) / RATE, f = note_hz(roots[bar]), t = (float)i / RATE;
      for (int h = 1; h <= 5; h++) x += cw_sin(CW_TAU * f * (float)h * t) / (float)h;
      x *= 0.22f * cw_exp(-t8 * 6.0f);
      break;
    }
    case 3: { /* chords: the triad an octave and a half up, swelling over each bar */
      float t = (float)i / RATE, tbar = (float)(i % (BEAT * 4)) / RATE;
      for (int k = 0; k < 3; k++) x += cw_sin(CW_TAU * note_hz(roots[bar] + 24 + triads[bar][k]) * t);
      x *= 0.07f * CW_MIN(tbar * 4.0f, 1.0f) * (1.0f - 0.3f * tbar / 2.0f);
      break;
    }
    case 4: { /* lead: sixteenth-note arpeggio of the chord, two octaves up */
      int n16 = i / (BEAT / 4);
      float t16 = (float)(i % (BEAT / 4)) / RATE, t = (float)i / RATE;
      float f = note_hz(roots[bar] + 36 + triads[bar][n16 % 3] + (n16 % 4 == 3 ? 12 : 0));
      x = 0.12f * (cw_sin(CW_TAU * f * t) + cw_sin(CW_TAU * 3 * f * t) / 3.0f) * cw_exp(-t16 * 14.0f);
      break;
    }
    }
    out[i] = x;
  }
}

/* A mono 16-bit WAV in memory, loaded as a stream (how a game would play its stems from files). */
static cw_sound stream_of(const float *pcm) {
  static uint8_t w[44 + LEN * 2];
  uint32_t u32;
  uint16_t u16;
#define P32(o, v) (u32 = (uint32_t)(v), memcpy(w + (o), &u32, 4))
#define P16(o, v) (u16 = (uint16_t)(v), memcpy(w + (o), &u16, 2))
  memcpy(w, "RIFF", 4);
  P32(4, 36 + LEN * 2);
  memcpy(w + 8, "WAVEfmt ", 8);
  P32(16, 16); P16(20, 1); P16(22, 1); P32(24, RATE); P32(28, RATE * 2); P16(32, 2); P16(34, 16);
  memcpy(w + 36, "data", 4);
  P32(40, LEN * 2);
  for (int i = 0; i < LEN; i++) {
    int16_t s = (int16_t)(cw_clampf(pcm[i], -1, 1) * 32767.0f);
    memcpy(w + 44 + i * 2, &s, 2);
  }
  return cw_sound_load_memory(w, sizeof w, &(cw_sound_desc){ .mode = CW_SOUND_STREAM });
}

static void start_audio(tempo_app *a) {
  a->offline = cw_test_env("CW_TEMPO_OFFLINE") != NULL;
  a->on = cw_audio_init(&(cw_audio_desc){ .offline = a->offline });
  static float pcm[LEN];
  for (int s = 0; s < STEMS; s++) {
    synth(s, pcm);
    a->sound[s] = stream_of(pcm);
  }
  /* #region play */
  /* every stem on the same mixer frame, a little ahead so the device's buffering cannot split them */
  int64_t at = cw_audio_mixed_frames() + CW_AUDIO_BLOCK * 4;
  for (int s = 0; s < STEMS; s++)
    a->voice[s] = cw_audio_play(a->sound[s], &(cw_play_desc){ .loop = true, .start_frame = at, .volume = 0.8f });
  /* #endregion */
}

static void init(void *u) {
  tempo_app *a = u;
  a->down = cw_action_add("tempo_down", 0.5f);
  a->up = cw_action_add("tempo_up", 0.5f);
  cw_action_bind_text(a->down, "pad:left, pad:lshoulder, key:left");
  cw_action_bind_text(a->up, "pad:right, pad:rshoulder, key:right");
  a->percent = 100;
  cw_debug_i32("tempo.percent", &a->percent);
  cw_debug_u32("tempo.position", &a->position);
  cw_debug_i32("tempo.locked", &a->locked);
  cw_debug_i32("tempo.beat", &a->beat);
}

static void fixed_update(void *u, double dt) {
  (void)dt;
  tempo_app *a = u;
  if (!a->on) {
    if (cw_frame_index() >= 1) start_audio(a);
    return;
  }
  int step = a->step;
  if (cw_action_just_pressed(0, a->down)) step--;
  if (cw_action_just_pressed(0, a->up)) step++;
  step = CW_CLAMP(step, -10, 20);
  if (step != a->step) {
    a->step = step;
    a->percent = 100 + 5 * step;
    /* #region tempo */
    cw_voices_set_tempo(a->voice, STEMS, (float)a->percent / 100.0f); /* one command: the same block for all five */
    /* #endregion */
  }
  if (a->offline) {
    static float mix[800 * 2];
    cw_audio_render(mix, 800); /* a tick's worth at 48 kHz */
  } else {
    cw_audio_update();
  }
  a->locked = 1;
  for (int s = 0; s < STEMS; s++) {
    a->clock[s] = cw_voice_frames_played(a->voice[s]);
    a->locked &= a->clock[s] == a->clock[0];
  }
  a->position = (uint32_t)a->clock[0];
  a->beat = (int32_t)((a->clock[0] % LEN) / BEAT);
}

static void render(void *u, double alpha) {
  (void)alpha;
  tempo_app *a = u;
  cw_r2d_begin(&(cw_r2d_pass_desc){ .clear = true, .clear_color = cw_rgba(0.07f, 0.07f, 0.1f, 1) });
  cw_font f = cw_ui_font_sans();
  char b[96];
  snprintf(b, sizeof b, "tempo %d%%   %d BPM", a->percent, BPM * a->percent / 100);
  cw_r2d_text(f, b, (cw_vec2){ 30, 28 }, &(cw_text_style){ .size = 34 });
  cw_r2d_text(f, "left / right: tempo (the key stays)", (cw_vec2){ 30, 74 },
              &(cw_text_style){ .size = 18, .color = cw_rgba(0.7f, 0.7f, 0.75f, 1) });
  /* the song position from the stems' clock: bar.beat, and each stem's own beat light */
  int64_t pos = a->clock[0] % LEN;
  snprintf(b, sizeof b, "bar %d  beat %d   %s", (int)(pos / (BEAT * 4)) + 1, (int)(pos / BEAT) % 4 + 1,
           a->locked ? "stems locked" : "stems apart");
  cw_r2d_text(f, b, (cw_vec2){ 30, 110 }, &(cw_text_style){ .size = 20 });
  for (int s = 0; s < STEMS; s++) {
    float y = 150.0f + 34.0f * (float)s;
    float phase = (float)(a->clock[s] % BEAT) / (float)BEAT; /* 0 on the beat */
    float glow = 1.0f - CW_MIN(phase * 4.0f, 1.0f);
    cw_r2d_text(f, stem_names[s], (cw_vec2){ 30, y + 4 }, &(cw_text_style){ .size = 18 });
    cw_r2d_rect((cw_rect){ 130, y, 24, 24 }, cw_rgba(0.2f + 0.8f * glow, 0.25f + 0.55f * glow, 0.3f, 1));
    float x = 170.0f + 400.0f * (float)(a->clock[s] % LEN) / (float)LEN; /* where in the two bars */
    cw_r2d_rect((cw_rect){ 170, y + 10, 400, 4 }, cw_rgba(0.25f, 0.25f, 0.3f, 1));
    cw_r2d_rect((cw_rect){ x - 3, y + 4, 6, 16 }, cw_rgba(0.9f, 0.9f, 1, 1));
  }
  cw_r2d_end();
}

static void shutdown(void *u) {
  (void)u;
  cw_audio_shutdown();
}

cw_app_desc cw_main(int argc, char **argv) {
  (void)argc;
  (void)argv;
  return (cw_app_desc){ .title = "cartwheel audio tempo", .width = 640, .height = 340, .debug = true, .init = init,
                        .fixed_update = fixed_update, .render = render, .shutdown = shutdown, .user = &app };
}
