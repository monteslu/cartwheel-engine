/* The story runtime and speech packs (cartwheel/story.h, cartwheel/speech.h), on the story_scene sample's script
 * (tests/unit/story_data.c is tools/story-compile.mjs --c of samples/story_scene/story.story; CI checks it is fresh).
 * Each path through the gate scene against what the script says it does: the lines in order, the choice waiting,
 * each option's flags and stats, the elif and else branches, the default on a timeout, a skipped scene ending in the
 * same state as a scene played with defaults, save points, call and return, pools that never repeat a line twice
 * running and honour cooldowns, once and conditions, saves that survive a round trip, malformed data refused, and a
 * speech pack built here (its lengths and its mouth by time).
 * Control (CW_STORY_ALLOW_REPEAT=1, read late): the pool rule that keeps a line from being said twice running is off,
 * and the late check of 40 barks must fail. */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "cartwheel/speech.h"
#include "cartwheel/story.h"
#include "test.h"

extern const unsigned char story_data[];
extern const unsigned long story_data_size;

static struct { int emits, emit_arg, shots, last_shot, lines, ends; char emit_name[32], last_text[96]; bool skipped; } R;
static void on_emit(const char *n, int a, void *u) { (void)u; R.emits++; R.emit_arg = a; snprintf(R.emit_name, sizeof R.emit_name, "%s", n); }
static void on_shot(const cw_story_shot *s, void *u) { (void)u; R.shots++; R.last_shot = s->type; }
static void on_line(const cw_story_line *l, void *u) { (void)u; R.lines++; snprintf(R.last_text, sizeof R.last_text, "%s", l->text); }
static void on_end(const char *s, bool skipped, void *u) { (void)u; (void)s; R.ends++; R.skipped = skipped; }

static cw_story *make(void) {
  memset(&R, 0, sizeof R);
  return cw_story_new(&(cw_story_desc){ .data = story_data, .size = story_data_size, .on_emit = on_emit, .on_shot = on_shot,
                                        .on_line = on_line, .on_scene_end = on_end });
}
/* tick until a choice waits or the scene ends (at most n seconds) */
static void run_until_choice(cw_story *st, float secs) {
  for (int i = 0; i < (int)(secs * 60) && cw_story_active(st) && !cw_story_choosing(st); i++) cw_story_tick(st, 1.f / 60);
}
static void run_out(cw_story *st, float secs) { for (int i = 0; i < (int)(secs * 60) && cw_story_active(st); i++) cw_story_tick(st, 1.f / 60); }

static void test_load(void) {
  cw_story *st = make();
  CHECK(st != NULL);
  if (!st) return;
  CHECK_EQ_INT(cw_story_cast_count(st), 2);
  CHECK_EQ_INT(cw_story_cast_find(st, "traveller"), 1);
  CHECK(!strcmp(cw_story_cast_name(st, 0), "The Keeper"));
  CHECK_EQ_INT(cw_story_scene_index(st, "farewell"), 1);
  CHECK_EQ_INT(cw_story_stat(st, "trust"), 0);
  CHECK(!cw_story_flag(st, "gave_name"));
  CHECK(!cw_story_play(st, "nowhere"));
  cw_story_free(st);
}

static void test_courier(void) {
  cw_story *st = make();
  CHECK(cw_story_play(st, "gate"));
  /* the first line is the keeper's, at once; the scene's shot came first */
  const cw_story_line *l = cw_story_current(st);
  CHECK(l && l->who == 0 && !strcmp(l->text, "Halt. Nobody crosses the bridge after dark."));
  CHECK(l && l->key == cw_speech_key("keeper", l->text));
  CHECK_EQ_INT(R.shots, 1);
  CHECK_EQ_INT(R.last_shot, CW_SHOT_TWO);
  run_until_choice(st, 30);
  CHECK(cw_story_choosing(st));
  CHECK_EQ_INT(R.lines, 3);
  const char *opt[6]; bool on[6];
  CHECK_EQ_INT(cw_story_options(st, opt, on, 6), 3);
  CHECK(!strcmp(opt[0], "I am a courier from the coast.") && on[0] && on[1] && on[2]);
  CHECK(!strcmp(cw_story_prompt(st), "Answer the keeper"));
  /* a choice waits: ten seconds pass and it still waits (its timeout is twelve) */
  for (int i = 0; i < 600; i++) cw_story_tick(st, 1.f / 60);
  CHECK(cw_story_choosing(st));
  cw_story_choose(st, 0);
  run_out(st, 30);
  CHECK(!cw_story_active(st));
  CHECK_EQ_INT(cw_story_stat(st, "trust"), 2);
  CHECK(cw_story_flag(st, "gave_name") && cw_story_flag(st, "gate_open"));
  CHECK_EQ_INT(R.emits, 1);
  CHECK(!strcmp(R.emit_name, "open_gate") && R.emit_arg == 1);
  CHECK(cw_story_seen(st, "gate") && cw_story_seen(st, "farewell"));
  CHECK_EQ_INT(R.ends, 1);
  CHECK(!R.skipped);
  CHECK(!strcmp(cw_story_resume_scene(st), "gate"));
  CHECK(!strcmp(R.last_text, "Safe roads.")); /* or: the gate is open */
  /* courier line, joy reply, "Safe roads" from the called scene: 3 + 3 */
  CHECK_EQ_INT(R.lines, 6);
  cw_story_free(st);
}

static void test_refuse(void) {
  cw_story *st = make();
  cw_story_play(st, "gate");
  run_until_choice(st, 30);
  cw_story_choose(st, 1);
  int shots0 = R.shots;
  run_out(st, 30);
  CHECK_EQ_INT(cw_story_stat(st, "trust"), -1);
  CHECK(!cw_story_flag(st, "gave_name") && !cw_story_flag(st, "gate_open"));
  CHECK_EQ_INT(R.emits, 0);
  CHECK(R.shots > shots0); /* the elif's push */
  CHECK_EQ_INT(R.last_shot, CW_SHOT_PUSH);
  CHECK(!strcmp(R.last_text, "Safe roads.")); /* or: trust fell below 0 */
  cw_story_free(st);
  /* silence: the else */
  st = make();
  cw_story_play(st, "gate");
  run_until_choice(st, 30);
  cw_story_choose(st, 2);
  run_out(st, 30);
  CHECK_EQ_INT(cw_story_stat(st, "trust"), 0);
  CHECK(!cw_story_flag(st, "gate_open"));
  CHECK(!strcmp(R.last_text, "Mind the cold.")); /* neither side of the or */
  cw_story_free(st);
}

static void test_timeout(void) {
  cw_story *st = make();
  cw_story_play(st, "gate");
  run_until_choice(st, 30);
  for (int i = 0; i < 12 * 60 + 2 && cw_story_choosing(st); i++) cw_story_tick(st, 1.f / 60);
  CHECK(!cw_story_choosing(st));
  run_out(st, 30);
  CHECK_EQ_INT(cw_story_stat(st, "trust"), 2); /* default=1: the courier */
  CHECK(cw_story_flag(st, "gate_open"));
  cw_story_free(st);
}

static void test_skip(void) {
  cw_story *st = make();
  cw_story_play(st, "gate");
  cw_story_tick(st, 1.f / 60);
  cw_story_skip_scene(st);
  CHECK(!cw_story_active(st));
  CHECK(R.skipped);
  /* the same state as the defaults played through */
  CHECK_EQ_INT(cw_story_stat(st, "trust"), 2);
  CHECK(cw_story_flag(st, "gate_open"));
  CHECK_EQ_INT(R.emits, 1);
  CHECK(cw_story_seen(st, "farewell"));
  CHECK(cw_story_current(st) == NULL);
  cw_story_free(st);
}

static void test_advance(void) {
  cw_story *st = make();
  cw_story_play(st, "gate");
  /* advance ends a line early, but not in its first 0.15 s */
  cw_story_advance(st);
  CHECK(cw_story_current(st) && !strcmp(cw_story_current(st)->text, "Halt. Nobody crosses the bridge after dark."));
  for (int i = 0; i < 12; i++) cw_story_tick(st, 1.f / 60);
  cw_story_advance(st);
  CHECK(cw_story_current(st) && !strcmp(cw_story_current(st)->text, "It is barely dusk."));
  cw_story_free(st);
}

static int bark_repeats(cw_story *st, int n) {
  uint32_t last = 0; int rep = 0;
  for (int i = 0; i < n; i++) {
    for (int k = 0; k < 6 * 60; k++) cw_story_tick(st, 1.f / 60); /* past the cooldown and the line */
    const cw_story_line *l = cw_story_bark(st, "keeper.idle");
    if (!l) continue;
    if (l->key == last) rep++;
    last = l->key;
  }
  return rep;
}

static void test_pools(void) {
  cw_story *st = make();
  CHECK_EQ_INT(bark_repeats(st, 30), 0);
  /* every line of the pool comes round */
  /* cooldown: right after a bark the pool is not ready */
  for (int k = 0; k < 6 * 60; k++) cw_story_tick(st, 1.f / 60);
  CHECK(cw_story_bark(st, "keeper.idle") != NULL);
  CHECK(!cw_story_pool_ready(st, "keeper.idle"));
  CHECK(cw_story_bark(st, "keeper.idle") == NULL);
  /* once + conditions: without gate_open only "Still here?", once */
  for (int k = 0; k < 6 * 60; k++) cw_story_tick(st, 1.f / 60);
  const cw_story_line *l = cw_story_bark(st, "keeper.once");
  CHECK(l && !strcmp(l->text, "Still here?"));
  for (int k = 0; k < 6 * 60; k++) cw_story_tick(st, 1.f / 60);
  CHECK(cw_story_bark(st, "keeper.once") == NULL);
  cw_story_set_flag(st, "gate_open", true);
  l = cw_story_bark(st, "keeper.once");
  CHECK(l && !strcmp(l->text, "You came back.") && l->emotion == CW_EMO_SURPRISE);
  /* an aside never interrupts a scene's line */
  for (int k = 0; k < 6 * 60; k++) cw_story_tick(st, 1.f / 60);
  cw_story_play(st, "gate");
  CHECK(cw_story_bark(st, "keeper.idle") == NULL);
  cw_story_free(st);
}

static void test_saves(void) {
  cw_story *st = make();
  cw_story_play(st, "gate");
  run_until_choice(st, 30);
  cw_story_choose(st, 0);
  run_out(st, 30);
  cw_story_bark(st, "keeper.once");
  uint8_t buf[512];
  size_t n = cw_story_save(st, buf, sizeof buf);
  CHECK(n > 12 && n < sizeof buf);
  CHECK_EQ_INT(cw_story_save(st, NULL, 0), n);
  cw_story *b = make();
  CHECK(cw_story_load(b, buf, n));
  CHECK_EQ_INT(cw_story_stat(b, "trust"), 2);
  CHECK(cw_story_flag(b, "gave_name") && cw_story_flag(b, "gate_open"));
  CHECK(cw_story_seen(b, "farewell"));
  CHECK(cw_story_resume_scene(b) && !strcmp(cw_story_resume_scene(b), "gate"));
  CHECK(cw_story_bark(b, "keeper.once") == NULL); /* "You came back." was said before the save, once */
  /* a short or foreign buffer is refused and changes nothing */
  CHECK(!cw_story_load(b, buf, 8));
  buf[0] = 'X';
  CHECK(!cw_story_load(b, buf, n));
  CHECK_EQ_INT(cw_story_stat(b, "trust"), 2);
  cw_story_reset(b);
  CHECK_EQ_INT(cw_story_stat(b, "trust"), 0);
  cw_story_free(b);
  cw_story_free(st);
}

static void test_malformed(void) {
  unsigned char *bad = malloc(story_data_size);
  memcpy(bad, story_data, story_data_size);
  CHECK(cw_story_new(&(cw_story_desc){ .data = bad, .size = story_data_size - 1 }) == NULL);
  bad[0] = 'X';
  CHECK(cw_story_new(&(cw_story_desc){ .data = bad, .size = story_data_size }) == NULL);
  memcpy(bad, story_data, story_data_size);
  bad[4] = 9; /* version */
  CHECK(cw_story_new(&(cw_story_desc){ .data = bad, .size = story_data_size }) == NULL);
  free(bad);
}

/* a speech pack built here, as tools/speech-bake.mjs writes one */
static void test_speech(void) {
  const int rate = 8000, frames = 4000, ecount = 50;
  size_t alen = (frames + 1) / 2, elen = (size_t)ecount * 2, size = 20 + 2 * 28 + 2 * (alen + elen);
  uint8_t *p = calloc(1, size);
  memcpy(p, "CWSP", 4);
  uint32_t hdr[4] = { 1, 2, 100, 1 }; memcpy(p + 4, hdr, 16);
  uint32_t keys[2] = { cw_speech_key("keeper", "a"), cw_speech_key("keeper", "b") };
  if (keys[0] > keys[1]) { uint32_t t = keys[0]; keys[0] = keys[1]; keys[1] = t; }
  uint32_t off = 0;
  for (int i = 0; i < 2; i++) {
    uint32_t e[7] = { keys[i], (uint32_t)rate, (uint32_t)(frames / (i + 1)), off, (uint32_t)alen, off + (uint32_t)alen, (uint32_t)ecount };
    memcpy(p + 20 + i * 28, e, 28);
    uint8_t *env = p + 20 + 56 + off + alen;
    for (int k = 0; k < ecount; k++) { env[k * 2] = (uint8_t)(k * 5); env[k * 2 + 1] = 200; }
    off += (uint32_t)(alen + elen);
  }
  cw_speech *sp = cw_speech_load_memory(p, size);
  CHECK(sp != NULL);
  CHECK_EQ_INT(cw_speech_count(sp), 2);
  CHECK(cw_speech_has(sp, keys[1]) && !cw_speech_has(sp, keys[0] ^ 1));
  CHECK(fabsf(cw_speech_seconds(sp, keys[0]) - 0.5f) < 1e-4f && fabsf(cw_speech_seconds(sp, keys[1]) - 0.25f) < 1e-4f);
  float o, s;
  cw_speech_mouth(sp, keys[0], 0.105f, &o, &s); /* between envelope frames 10 (50) and 11 (55) */
  CHECK(fabsf(o - 52.5f / 255) < 0.01f && fabsf(s - 200.f / 255) < 0.01f);
  cw_speech_mouth(sp, keys[0], 5, &o, &s); /* past the line: closed */
  CHECK(o == 0 && s == 0.5f);
  /* a corrupt pack (an entry past the data) is refused */
  uint32_t big = 1u << 30; memcpy(p + 20 + 12, &big, 4);
  CHECK(cw_speech_load_memory(p, size) == NULL);
  cw_speech_free(sp);
  free(p);
}

void cw_test_late(void) {
  /* the control switch reaches cw_story_bark (CW_STORY_ALLOW_REPEAT): the no-repeat rule is gone */
  cw_story *st = make();
  int rep = bark_repeats(st, 40);
  if (cw_test_env("CW_STORY_ALLOW_REPEAT")) cw_info("story control: %d barks repeated the last", rep);
  CHECK_EQ_INT(rep, 0);
  cw_story_free(st);
}

CW_TEST_SUITE("story", { "load", test_load }, { "courier", test_courier }, { "refuse", test_refuse }, { "timeout", test_timeout },
              { "skip", test_skip }, { "advance", test_advance }, { "pools", test_pools }, { "saves", test_saves },
              { "malformed", test_malformed }, { "speech", test_speech })
