/* The save store (cartwheel/save.h) and the game shell's use of it: slots by name, growing and shrinking a
 * slot, versions and the migrate hook, damage (one slot, the tail), a block from before the store, the app's own
 * store, and the shell's save: a game struct that grows, a version change with and without a hook, a struct
 * turned into slots, a slot written without a commit, and the game's own Settings rows (cw_shell_setting): saved
 * under their keys beside a struct and in a slot store, a new row, a removed row, choices added, moved, renamed and
 * cut, an out-of-range save, a struct that changes size or version, a row the game changed itself. The store's bytes fold into the checksum (the format is
 * the same on every host). */
#include <string.h>

#include "cartwheel/save.h"
#include "cartwheel/shell.h"

static uint8_t app_region[1024];
#define CW_TEST_SAVE app_region
#define CW_TEST_SAVE_SIZE sizeof app_region
#include "test.h"

typedef struct { uint32_t score; uint16_t level; uint16_t pad; } progress_v1;
typedef struct { uint32_t level; uint32_t pad; uint64_t score; uint8_t stars[8]; } progress_v2;

static int migrate_calls;
static bool migrate_progress(uint16_t old_version, const void *old, size_t old_size, void *out, size_t out_size, void *user) {
  migrate_calls++;
  if (user) return false; /* a hook that gives up */
  if (old_version != 1 || old_size != sizeof(progress_v1) || out_size != sizeof(progress_v2)) return false;
  progress_v1 a;
  memcpy(&a, old, sizeof a);
  progress_v2 *b = out;
  b->level = a.level;
  b->score = a.score;
  b->stars[0] = 3;
  return true;
}

static void fold_block(const uint8_t *b, size_t n) {
  uint32_t h = cw_hash32(b, n, CW_HASH32_INIT);
  FOLD(h);
}

static void t_slots(void) {
  static uint8_t block[256];
  memset(block, 0, sizeof block);
  cw_save_store st;
  cw_info("the save store error below is expected");
  CHECK(!cw_save_open(&st, block, 8, NULL, NULL)); /* too small */
  CHECK(cw_save_open(&st, block, sizeof block, NULL, NULL));
  CHECK_EQ_INT(cw_save_count(&st), 0);
  CHECK_EQ_INT(cw_save_used(&st), 8);
  uint32_t a = 0xdeadbeef, b = 0;
  CHECK(cw_save_read(&st, "coins", 1, &b, sizeof b, NULL, NULL) == CW_SAVE_NONE && b == 0);
  CHECK(cw_save_write(&st, "coins", 1, &a, sizeof a));
  CHECK(cw_save_read(&st, "coins", 1, &b, sizeof b, NULL, NULL) == CW_SAVE_OK && b == a);
  char text[] = "the quick brown fox";
  CHECK(cw_save_write(&st, "name", 7, text, sizeof text));
  CHECK_EQ_INT(cw_save_count(&st), 2);
  uint16_t ver;
  size_t size;
  CHECK(cw_save_info(&st, "name", &ver, &size) && ver == 7 && size == sizeof text);
  CHECK(!cw_save_info(&st, "nope", NULL, NULL));
  char nm[16];
  CHECK(cw_save_name(&st, 0, nm, sizeof nm) && !strcmp(nm, "coins"));
  CHECK(cw_save_name(&st, 1, nm, sizeof nm) && !strcmp(nm, "name"));
  CHECK(!cw_save_name(&st, 2, nm, sizeof nm));
  /* 8 + (12 + 8 + 4) + (12 + 4 + 20) */
  CHECK_EQ_INT(cw_save_used(&st), 68);
  fold_block(block, sizeof block);
  /* in place, then another size: the other slot is untouched, the rewritten one moves to the end */
  a = 7;
  CHECK(cw_save_write(&st, "coins", 1, &a, sizeof a));
  CHECK_EQ_INT(cw_save_used(&st), 68);
  uint64_t big = 0x1122334455667788ull, big2 = 0;
  CHECK(cw_save_write(&st, "coins", 2, &big, sizeof big));
  CHECK(cw_save_read(&st, "coins", 2, &big2, sizeof big2, NULL, NULL) == CW_SAVE_OK && big2 == big);
  char back[sizeof text];
  CHECK(cw_save_read(&st, "name", 7, back, sizeof back, NULL, NULL) == CW_SAVE_OK && !strcmp(back, text));
  CHECK(cw_save_name(&st, 0, nm, sizeof nm) && !strcmp(nm, "name"));
  /* does not fit: refused, nothing changed */
  static uint8_t huge[512];
  size_t used = cw_save_used(&st);
  cw_info("the two save store errors below are expected");
  CHECK(!cw_save_write(&st, "huge", 1, huge, sizeof huge));
  CHECK(!cw_save_write(&st, "", 1, huge, 4));
  CHECK(cw_save_used(&st) == used && cw_save_count(&st) == 2);
  size_t room = cw_save_room(&st, "fill");
  CHECK(room > 0 && cw_save_write(&st, "fill", 1, huge, room));
  CHECK_EQ_INT(cw_save_used(&st), sizeof block);
  CHECK_EQ_INT(cw_save_room(&st, "more"), 0);
  CHECK(cw_save_erase(&st, "fill") && !cw_save_erase(&st, "fill"));
  CHECK_EQ_INT(cw_save_count(&st), 2);
  /* what the host keeps is the block: another store opened over a copy of it reads the same */
  static uint8_t copy[256];
  memcpy(copy, block, sizeof copy);
  cw_save_store st2;
  CHECK(cw_save_open(&st2, copy, sizeof copy, NULL, NULL));
  big2 = 0;
  CHECK(cw_save_read(&st2, "coins", 2, &big2, sizeof big2, NULL, NULL) == CW_SAVE_OK && big2 == big);
  CHECK(!memcmp(copy, block, sizeof copy)); /* opening a sound store changes nothing */
  /* a larger block (the game gave its save more room) still opens */
  static uint8_t larger[512];
  memset(larger, 0, sizeof larger);
  memcpy(larger, block, sizeof block);
  CHECK(cw_save_open(&st2, larger, sizeof larger, NULL, NULL) && cw_save_count(&st2) == 2);
  cw_save_clear(&st);
  CHECK(cw_save_count(&st) == 0 && cw_save_used(&st) == 8);
}

static void t_grow_and_migrate(void) {
  static uint8_t block[512];
  memset(block, 0, sizeof block);
  cw_save_store st;
  cw_save_open(&st, block, sizeof block, NULL, NULL);
  progress_v1 p1 = { .score = 123456, .level = 9 };
  cw_save_write(&st, "progress", 1, &p1, sizeof p1);
  /* the struct grew at its end, same version: nothing is lost, the new part reads zero */
  struct { progress_v1 old; uint32_t stars; uint8_t tail[8]; } grown;
  memset(&grown, 0xff, sizeof grown);
  CHECK(cw_save_read(&st, "progress", 1, &grown, sizeof grown, NULL, NULL) == CW_SAVE_RESIZED);
  CHECK(grown.old.score == 123456 && grown.old.level == 9 && grown.stars == 0 && grown.tail[7] == 0);
  /* and shrank: the part that is left */
  uint32_t only_score = 0;
  CHECK(cw_save_read(&st, "progress", 1, &only_score, sizeof only_score, NULL, NULL) == CW_SAVE_RESIZED && only_score == 123456);
  /* a new layout, version 2: the hook converts */
  progress_v2 p2;
  migrate_calls = 0;
  CHECK(cw_save_read(&st, "progress", 2, &p2, sizeof p2, migrate_progress, NULL) == CW_SAVE_MIGRATED);
  CHECK(migrate_calls == 1 && p2.level == 9 && p2.score == 123456 && p2.stars[0] == 3);
  /* no hook, or a hook that gives up: zeros, and the old bytes are still there for a build that can read them */
  memset(&p2, 0xff, sizeof p2);
  CHECK(cw_save_read(&st, "progress", 2, &p2, sizeof p2, NULL, NULL) == CW_SAVE_DISCARDED);
  CHECK(p2.level == 0 && p2.score == 0);
  CHECK(cw_save_read(&st, "progress", 2, &p2, sizeof p2, migrate_progress, &st) == CW_SAVE_DISCARDED && p2.score == 0);
  uint16_t ver;
  CHECK(cw_save_info(&st, "progress", &ver, NULL) && ver == 1);
  /* written back as version 2, it reads plainly; a build that only knows version 1 is told it is newer */
  cw_save_read(&st, "progress", 2, &p2, sizeof p2, migrate_progress, NULL);
  CHECK(cw_save_write(&st, "progress", 2, &p2, sizeof p2));
  progress_v2 again;
  migrate_calls = 0;
  CHECK(cw_save_read(&st, "progress", 2, &again, sizeof again, migrate_progress, NULL) == CW_SAVE_OK && migrate_calls == 0);
  CHECK(again.score == 123456 && again.stars[0] == 3);
  progress_v1 older = { 1, 1, 1 };
  CHECK(cw_save_read(&st, "progress", 1, &older, sizeof older, NULL, NULL) == CW_SAVE_NEWER && older.score == 0);
  CHECK(cw_save_info(&st, "progress", &ver, NULL) && ver == 2); /* left alone */
  fold_block(block, sizeof block);
}

static int legacy_calls;
static void import_raw(cw_save_store *st, const void *old, size_t old_size, void *user) {
  (void)user;
  legacy_calls++;
  progress_v1 p;
  if (old_size < sizeof p) return;
  memcpy(&p, old, sizeof p);
  cw_save_write(st, "progress", 1, &p, sizeof p);
}

static void t_damage_and_legacy(void) {
  static uint8_t block[256];
  memset(block, 0, sizeof block);
  cw_save_store st;
  cw_save_open(&st, block, sizeof block, NULL, NULL);
  uint32_t a = 11, b = 22, c = 33, out = 0;
  cw_save_write(&st, "a", 1, &a, sizeof a);
  cw_save_write(&st, "b", 1, &b, sizeof b);
  cw_save_write(&st, "c", 1, &c, sizeof c);
  /* each slot is 12 + 4 + 4 = 20 bytes: a at 8, b at 28, c at 48 */
  CHECK_EQ_INT(cw_save_used(&st), 68);
  static uint8_t good[256];
  memcpy(good, block, sizeof good);
  /* a flipped bit in b's data: b is dropped, a and c survive */
  block[28 + 16] ^= 0x10;
  cw_info("the save store warnings below are expected");
  CHECK(cw_save_open(&st, block, sizeof block, NULL, NULL));
  CHECK_EQ_INT(cw_save_count(&st), 2);
  CHECK(cw_save_read(&st, "a", 1, &out, sizeof out, NULL, NULL) == CW_SAVE_OK && out == 11);
  CHECK(cw_save_read(&st, "b", 1, &out, sizeof out, NULL, NULL) == CW_SAVE_NONE);
  CHECK(cw_save_read(&st, "c", 1, &out, sizeof out, NULL, NULL) == CW_SAVE_OK && out == 33);
  /* b's size field wrecked: everything from b on is cut, a survives */
  memcpy(block, good, sizeof block);
  block[28 + 4] = 0xff, block[28 + 5] = 0xff, block[28 + 6] = 0xff;
  CHECK(cw_save_open(&st, block, sizeof block, NULL, NULL));
  CHECK_EQ_INT(cw_save_count(&st), 1);
  CHECK(cw_save_read(&st, "a", 1, &out, sizeof out, NULL, NULL) == CW_SAVE_OK && out == 11);
  CHECK(cw_save_write(&st, "b", 1, &b, sizeof b)); /* and the store is usable */
  /* the used count wrecked: not a store any more: empty */
  memcpy(block, good, sizeof block);
  block[5] = 0xff;
  CHECK(cw_save_open(&st, block, sizeof block, NULL, NULL) && cw_save_count(&st) == 0);
  /* the undamaged block is the control: all three read */
  memcpy(block, good, sizeof block);
  CHECK(cw_save_open(&st, block, sizeof block, NULL, NULL) && cw_save_count(&st) == 3);
  CHECK(cw_save_read(&st, "b", 1, &out, sizeof out, NULL, NULL) == CW_SAVE_OK && out == 22);
  /* a block from before the game used a store: a raw struct. The legacy hook turns it into a slot. */
  memset(block, 0, sizeof block);
  progress_v1 raw = { .score = 777, .level = 4 };
  memcpy(block, &raw, sizeof raw);
  legacy_calls = 0;
  CHECK(cw_save_open(&st, block, sizeof block, import_raw, NULL));
  CHECK_EQ_INT(legacy_calls, 1);
  progress_v1 got;
  CHECK(cw_save_read(&st, "progress", 1, &got, sizeof got, NULL, NULL) == CW_SAVE_OK && got.score == 777 && got.level == 4);
  /* opened again it is a store: the hook is not called twice; zeros are a first run, not legacy data */
  CHECK(cw_save_open(&st, block, sizeof block, import_raw, NULL) && legacy_calls == 1);
  memset(block, 0, sizeof block);
  CHECK(cw_save_open(&st, block, sizeof block, import_raw, NULL) && legacy_calls == 1 && cw_save_count(&st) == 0);
  /* without a hook the old bytes are dropped (said in the log) */
  memcpy(block, &raw, sizeof raw);
  CHECK(cw_save_open(&st, block, sizeof block, NULL, NULL) && cw_save_count(&st) == 0);
}

/* the app's own save data as a store */
static void t_app_store(void) {
  size_t size = 0;
  CHECK(cw_save_data(&size) == app_region && size == sizeof app_region);
  cw_save_store *st = cw_save_app_store(NULL, NULL);
  CHECK(st != NULL);
  if (!st) return;
  CHECK(st->base == app_region && cw_save_app_store(NULL, NULL) == st);
  uint32_t best = 4242, got = 0;
  CHECK(cw_save_write(st, "best", 1, &best, sizeof best));
  cw_save_commit(); /* a test run writes no file; the call is what a game does */
  cw_save_store again;
  CHECK(cw_save_open(&again, app_region, sizeof app_region, NULL, NULL));
  CHECK(cw_save_read(&again, "best", 1, &got, sizeof got, NULL, NULL) == CW_SAVE_OK && got == 4242);
  cw_save_clear(st);
}

/* ── the game shell's save ─────────────────────────────────────────── */

typedef struct { uint32_t best; uint32_t unlocked; } game_v1;
typedef struct { uint32_t best; uint32_t unlocked; uint8_t stars[24]; } game_v1_grown;
typedef struct { uint64_t best; uint32_t unlocked; uint32_t stars; } game_v2;

static bool shell_migrate(uint16_t old_version, const void *old, size_t old_size, void *out, size_t out_size, void *user) {
  (void)user;
  if (old_version != 1 || old_size < sizeof(game_v1) || out_size != sizeof(game_v2)) return false;
  game_v1 a;
  memcpy(&a, old, sizeof a);
  game_v2 *b = out;
  b->best = a.best;
  b->unlocked = a.unlocked;
  b->stars = 1;
  return true;
}
static int import_calls;
static void shell_import(cw_save_store *st, uint16_t old_version, const void *old, size_t old_size, void *user) {
  (void)user;
  import_calls++;
  if (old_version == 2 && old_size == sizeof(game_v2)) cw_save_write(st, "career", 1, old, old_size);
}
static void fresh_shell_save(void) { memset(cw_shell_save_region(), 0, cw_shell_save_region_size()); }

static void t_shell(void) {
  fresh_shell_save();
  /* a first run: a game with an 8 byte struct at version 1, and a setting the player changed */
  cw_shell_init(&(cw_shell_desc){ .title = "t", .game_save_size = sizeof(game_v1), .game_save_version = 1 });
  game_v1 *g = cw_shell_game_save();
  CHECK(g->best == 0);
  g->best = 9000;
  g->unlocked = 5;
  cw_shell_set_shake_level(CW_SHELL_SHAKE_REDUCED); /* a stored setting (it commits) */
  cw_shell_save_commit();
  fold_block(cw_shell_save_region(), 64);
  /* the next run of the same build reads it back */
  cw_shell_init(&(cw_shell_desc){ .title = "t", .game_save_size = sizeof(game_v1), .game_save_version = 1 });
  g = cw_shell_game_save();
  CHECK(g->best == 9000 && g->unlocked == 5);
  /* the game's struct grew (stars): progress and settings are kept, the new part is zero */
  cw_shell_init(&(cw_shell_desc){ .title = "t", .game_save_size = sizeof(game_v1_grown), .game_save_version = 1 });
  game_v1_grown *gg = cw_shell_game_save();
  CHECK(gg->best == 9000 && gg->unlocked == 5 && gg->stars[0] == 0 && gg->stars[23] == 0);
  CHECK_EQ_INT((int)cw_shell_shake_level(), (int)CW_SHELL_SHAKE_REDUCED);
  gg->stars[23] = 3;
  cw_shell_save_commit();
  /* back to the smaller struct and up again: what was cut off does not come back as stale bytes */
  cw_shell_init(&(cw_shell_desc){ .title = "t", .game_save_size = sizeof(game_v1), .game_save_version = 1 });
  cw_shell_init(&(cw_shell_desc){ .title = "t", .game_save_size = sizeof(game_v1_grown), .game_save_version = 1 });
  gg = cw_shell_game_save();
  CHECK(gg->best == 9000 && gg->stars[23] == 0);
  /* a new layout, version 2, with a hook: converted */
  cw_shell_init(&(cw_shell_desc){ .title = "t", .game_save_size = sizeof(game_v2), .game_save_version = 2, .game_save_migrate = shell_migrate });
  game_v2 *g2 = cw_shell_game_save();
  CHECK(g2->best == 9000 && g2->unlocked == 5 && g2->stars == 1);
  CHECK_EQ_INT((int)cw_shell_shake_level(), (int)CW_SHELL_SHAKE_REDUCED);
  cw_shell_save_commit();
  /* a version change with no hook: the game starts over, the settings do not */
  cw_info("the shell warning below is expected");
  cw_shell_init(&(cw_shell_desc){ .title = "t", .game_save_size = sizeof(game_v2), .game_save_version = 3 });
  g2 = cw_shell_game_save();
  CHECK(g2->best == 0 && g2->unlocked == 0);
  CHECK_EQ_INT((int)cw_shell_shake_level(), (int)CW_SHELL_SHAKE_REDUCED);
  /* put version 2 back for the next step */
  cw_shell_init(&(cw_shell_desc){ .title = "t", .game_save_size = sizeof(game_v2), .game_save_version = 2 });
  g2 = cw_shell_game_save();
  g2->best = 31337;
  cw_shell_save_commit();
  /* the game moves to slots: its import hook gets the old struct once */
  import_calls = 0;
  cw_shell_desc slots = { .title = "t", .game_save_slots = true, .game_save_import = shell_import };
  cw_shell_init(&slots);
  cw_save_store *st = cw_shell_save_store();
  CHECK(st != NULL && import_calls == 1);
  if (!st) return;
  game_v2 career;
  CHECK(cw_save_read(st, "career", 1, &career, sizeof career, NULL, NULL) == CW_SAVE_OK && career.best == 31337);
  uint32_t coins = 55, got = 0;
  CHECK(cw_save_write(st, "coins", 1, &coins, sizeof coins));
  cw_shell_save_commit();
  cw_shell_init(&slots);
  st = cw_shell_save_store();
  CHECK(import_calls == 1 && cw_save_count(st) == 2);
  CHECK(cw_save_read(st, "coins", 1, &got, sizeof got, NULL, NULL) == CW_SAVE_OK && got == 55);
  /* a slot written with no commit after it (the host may capture the block at any moment): it still loads,
   * and so do the settings */
  coins = 56;
  cw_save_write(st, "coins", 1, &coins, sizeof coins);
  cw_shell_init(&slots);
  st = cw_shell_save_store();
  CHECK(cw_save_read(st, "coins", 1, &got, sizeof got, NULL, NULL) == CW_SAVE_OK && got == 56);
  CHECK_EQ_INT((int)cw_shell_shake_level(), (int)CW_SHELL_SHAKE_REDUCED);
  fold_block((const uint8_t *)cw_shell_save_region() + 32, 96);
  /* a struct game written the same way without a commit is what the checksum is for: it starts with defaults
   * (the control for the line above: the slot store is what makes an uncommitted write safe) */
  fresh_shell_save();
  cw_shell_init(&(cw_shell_desc){ .title = "t", .game_save_size = sizeof(game_v1), .game_save_version = 1 });
  cw_shell_set_shake_level(CW_SHELL_SHAKE_REDUCED);
  g = cw_shell_game_save();
  g->best = 1;
  cw_info("the shell warning below is expected");
  cw_shell_init(&(cw_shell_desc){ .title = "t", .game_save_size = sizeof(game_v1), .game_save_version = 1 });
  g = cw_shell_game_save();
  CHECK(g->best == 0);
  CHECK_EQ_INT((int)cw_shell_shake_level(), (int)CW_SHELL_SHAKE_FULL);
  /* a game without slots asking for the store is told so */
  cw_info("the shell error below is expected");
  CHECK(cw_shell_save_store() == NULL);
  cw_shell_set_shake_level(CW_SHELL_SHAKE_FULL);
  fresh_shell_save();
}

/* ── the game's Settings rows ──────────────────────────────────────── */

static const char *const STYLE3[3] = { "All", "Neon", "Paper" };
static const char *const STYLE_MOVED[4] = { "Chalk", "All", "Paper", "Neon" }; /* a choice added in front, two swapped */
static const char *const STYLE_RENAMED[3] = { "Every", "Neon glow", "Paper only" };
static const char *const STYLE_CUT[2] = { "All", "Neon" };
static const char *const ONOFF[2] = { "Off", "On" };

/* a run of a game with these rows (its defaults in the ints), as the shell sees it at init */
static int sv_style, sv_hits, sv_new;
static void sv_init(const cw_shell_desc *base, const char *const *styles, int n_styles, bool hits, bool extra) {
  cw_shell_setting rows[3];
  int n = 0;
  rows[n++] = (cw_shell_setting){ .label = "Visual style", .key = "style", .value = &sv_style, .choices = styles, .count = n_styles };
  if (hits) rows[n++] = (cw_shell_setting){ .label = "Hit sounds", .key = "hits", .value = &sv_hits, .choices = ONOFF, .count = 2 };
  if (extra) rows[n++] = (cw_shell_setting){ .label = "New", .key = "new", .value = &sv_new, .choices = ONOFF, .count = 2 };
  cw_shell_desc d = *base;
  d.settings_items = rows;
  d.settings_item_count = n;
  cw_shell_init(&d);
}

static void t_shell_settings(void) {
  static const cw_shell_desc as_struct = { .title = "t", .game_save_size = sizeof(game_v1), .game_save_version = 1 };
  static const cw_shell_desc as_slots = { .title = "t", .game_save_slots = true };
  const cw_shell_desc *modes[2] = { &as_struct, &as_slots };
  for (int m = 0; m < 2; m++) {
    const cw_shell_desc *b = modes[m];
    fresh_shell_save();
    /* a first run: the defaults stay; the player picks Paper and Off */
    sv_style = 1, sv_hits = 1;
    sv_init(b, STYLE3, 3, true, false);
    CHECK(sv_style == 1 && sv_hits == 1);
    sv_style = 2, sv_hits = 0; /* as the menu (or the game itself) sets them */
    cw_shell_save_commit();
    /* the next run, defaults back in the ints: the saved choices come in at init */
    sv_style = 1, sv_hits = 1;
    sv_init(b, STYLE3, 3, true, false);
    CHECK_EQ_INT(sv_style, 2);
    CHECK_EQ_INT(sv_hits, 0);
    /* a new row starts at its default; the others keep theirs */
    sv_style = 0, sv_hits = 1, sv_new = 1;
    sv_init(b, STYLE3, 3, true, true);
    CHECK(sv_style == 2 && sv_hits == 0 && sv_new == 1);
    /* a removed row: the rest still load; the removed row's slot stays and comes back with it */
    sv_style = 0, sv_hits = 1;
    sv_init(b, STYLE3, 3, false, false);
    CHECK_EQ_INT(sv_style, 2);
    cw_shell_save_commit();
    sv_init(b, STYLE3, 3, true, false);
    CHECK_EQ_INT(sv_hits, 0);
    /* choices added and moved: the pick follows its label */
    sv_style = 1; /* Neon */
    cw_shell_save_commit();
    sv_style = 0;
    sv_init(b, STYLE_MOVED, 4, true, false);
    CHECK_EQ_INT(sv_style, 3); /* Neon moved to the end */
    /* choices renamed (another language): no label matches, the saved index (Neon's 1) holds */
    sv_style = 0;
    sv_init(b, STYLE_RENAMED, 3, true, false);
    CHECK_EQ_INT(sv_style, 1);
    /* a saved index past the choices with no label match: the default */
    sv_style = 2; /* Paper only */
    cw_shell_save_commit();
    sv_style = 1;
    cw_info("the shell warning below is expected");
    sv_init(b, STYLE_CUT, 2, true, false);
    CHECK_EQ_INT(sv_style, 1);
    /* a default out of range is set to 0 */
    sv_style = 7;
    cw_info("the shell error below is expected");
    sv_init(b, STYLE3, 3, true, false);
    CHECK(sv_style >= 0 && sv_style < 3);
    /* the setter: a renamed row (same key) keeps the game's int; a row with a new key loads its save */
    sv_style = 0, sv_hits = 1;
    cw_shell_set_settings_items((cw_shell_setting[]){ { .label = "Look", .key = "style", .value = &sv_style, .choices = STYLE3, .count = 3 } }, 1);
    CHECK_EQ_INT(sv_style, 0);
    cw_shell_set_settings_items((cw_shell_setting[]){ { .label = "Look", .key = "style", .value = &sv_style, .choices = STYLE3, .count = 3 },
                                                      { .label = "Hits", .key = "hits", .value = &sv_hits, .choices = ONOFF, .count = 2 } }, 2);
    CHECK_EQ_INT(sv_hits, 0);
    fold_block((const uint8_t *)cw_shell_save_region(), 32);
  }
  /* a struct game: its struct changes size, then version with no hook, and the rows stay */
  fresh_shell_save();
  sv_style = 0, sv_hits = 1;
  sv_init(&as_struct, STYLE3, 3, true, false);
  sv_style = 2, sv_hits = 0;
  cw_shell_save_commit();
  sv_style = 0, sv_hits = 1;
  sv_init(&(cw_shell_desc){ .title = "t", .game_save_size = sizeof(game_v1_grown), .game_save_version = 1 }, STYLE3, 3, true, false);
  CHECK(sv_style == 2 && sv_hits == 0);
  cw_info("the shell warning below is expected");
  sv_style = 0, sv_hits = 1;
  sv_init(&(cw_shell_desc){ .title = "t", .game_save_size = sizeof(game_v2), .game_save_version = 9 }, STYLE3, 3, true, false);
  CHECK(sv_style == 2 && sv_hits == 0);
  /* no commit after a change: the next run has the old choice (the control for every load above) */
  sv_style = 1;
  sv_init(&(cw_shell_desc){ .title = "t", .game_save_size = sizeof(game_v2), .game_save_version = 9 }, STYLE3, 3, true, false);
  CHECK_EQ_INT(sv_style, 2);
  /* a struct too big for the rows' space: told, not saved, the defaults hold */
  fresh_shell_save();
  cw_info("the shell error below is expected");
  sv_style = 1;
  sv_init(&(cw_shell_desc){ .title = "t", .game_save_size = CW_SHELL_GAME_SAVE_MAX }, STYLE3, 3, true, false);
  sv_style = 2;
  cw_shell_save_commit();
  sv_style = 1;
  sv_init(&(cw_shell_desc){ .title = "t", .game_save_size = CW_SHELL_GAME_SAVE_MAX }, STYLE3, 3, true, false);
  CHECK_EQ_INT(sv_style, 1);
  /* a game with no rows writes nothing past its struct (the tail stays zeros) */
  fresh_shell_save();
  cw_shell_init(&as_struct);
  cw_shell_save_commit();
  const uint8_t *tail = (const uint8_t *)cw_shell_game_save() + CW_SHELL_GAME_SAVE_MAX - CW_SHELL_SETTINGS_SAVE_BYTES;
  bool zeros = true;
  for (int i = 0; i < CW_SHELL_SETTINGS_SAVE_BYTES; i++) zeros &= tail[i] == 0;
  CHECK(zeros);
  fresh_shell_save();
}

CW_TEST_SUITE("save", { "slots", t_slots }, { "grow_and_migrate", t_grow_and_migrate }, { "damage_and_legacy", t_damage_and_legacy },
              { "app_store", t_app_store }, { "shell", t_shell }, { "shell_settings", t_shell_settings })
