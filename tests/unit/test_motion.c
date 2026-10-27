/* The command buffer and motion matcher (cartwheel/motion.h): motions with
 * leniency, double taps, charges, buffered buttons, consume, facing. */
#include <string.h>

#include "cartwheel/motion.h"
#include "test.h"

/* push a run of frames: dirs as a numpad string (relative, facing right) */
static void push_dirs(cw_cmd_buf *b, const char *dirs, int each, uint16_t btn_last) {
  for (const char *p = dirs; *p; p++) {
    int d = *p - '0';
    int x = (d % 3 == 0) - (d % 3 == 1), y = d >= 7 ? 1 : (d <= 3 ? -1 : 0);
    for (int i = 0; i < each; i++) {
      bool last = !p[1] && i == each - 1;
      cw_cmd_push(b, x < 0, x > 0, y > 0, y < 0, 1, last ? btn_last : 0);
    }
  }
}

static cw_motion mo(const char *t) {
  cw_motion m;
  if (!cw_motion_parse(t, &m)) memset(&m, 0, sizeof m);
  return m;
}

static void test_parse(void) {
  cw_motion m;
  CHECK(cw_motion_parse("236", &m) && m.count == 3 && m.steps[0] == 2 && !m.charge);
  CHECK(cw_motion_parse("[4]6", &m) && m.charge && m.count == 2 && m.steps[0] == 4);
  CHECK(!cw_motion_parse("", &m));
  CHECK(!cw_motion_parse("206", &m));
  CHECK(!cw_motion_parse("[4]", &m));
  CHECK(!cw_motion_parse("[46", &m));
  CHECK(!cw_motion_parse("123456789", &m));
  CHECK_EQ_INT(cw_cmd_dir(true, false, false, true, 1), 1);  /* down-left facing right = down-back */
  CHECK_EQ_INT(cw_cmd_dir(true, false, false, true, -1), 3); /* facing left: left is forward */
  CHECK_EQ_INT(cw_cmd_dir(true, true, false, false, 1), 5);  /* left+right cancel */
  CHECK_EQ_INT(cw_cmd_dir(false, false, true, true, 1), 8);  /* up wins */
}

static void test_quarter_circle(void) {
  cw_cmd_buf b; cw_motion qcf = mo("236");
  cw_cmd_clear(&b);
  push_dirs(&b, "2365", 2, 0);
  CHECK(cw_motion_match(&b, &qcf, 20) >= 0);
  /* lenient: no diagonal */
  cw_cmd_clear(&b); push_dirs(&b, "265", 2, 0);
  CHECK(cw_motion_match(&b, &qcf, 20) >= 0);
  /* wrong order must not match */
  cw_cmd_clear(&b); push_dirs(&b, "632", 2, 0);
  CHECK(cw_motion_match(&b, &qcf, 20) < 0);
  /* forward only is not a quarter circle */
  cw_cmd_clear(&b); push_dirs(&b, "666666", 1, 0);
  CHECK(cw_motion_match(&b, &qcf, 20) < 0);
  /* too slow: 2 then 30 frames then 6 */
  cw_cmd_clear(&b); push_dirs(&b, "2", 1, 0); push_dirs(&b, "5", 30, 0); push_dirs(&b, "6", 1, 0);
  CHECK(cw_motion_match(&b, &qcf, 12) < 0);
  /* a diagonal alone does not do it */
  cw_cmd_clear(&b); push_dirs(&b, "3333", 1, 0);
  CHECK(cw_motion_match(&b, &qcf, 20) < 0);
  cw_cmd_clear(&b); push_dirs(&b, "2365", 1, 0);
  int ago = cw_motion_match(&b, &qcf, 20);
  CHECK(ago < 0 || ago >= 0); /* completion age is reported (6 was 1 frame ago) */
  CHECK_EQ_INT(ago, 1);
  uint32_t v = (uint32_t)ago; FOLD(v);
}

static void test_dp_and_super(void) {
  cw_cmd_buf b; cw_motion dp = mo("623"), sup = mo("236236");
  cw_cmd_clear(&b); push_dirs(&b, "623", 2, 0);
  CHECK(cw_motion_match(&b, &dp, 20) >= 0);
  cw_cmd_clear(&b); push_dirs(&b, "236236", 2, 0);
  CHECK(cw_motion_match(&b, &sup, 30) >= 0);
  cw_cmd_clear(&b); push_dirs(&b, "236", 2, 0);
  CHECK(cw_motion_match(&b, &sup, 30) < 0);
}

static void test_double_tap(void) {
  cw_cmd_buf b; cw_motion dash = mo("66");
  cw_cmd_clear(&b); push_dirs(&b, "656", 2, 0);
  CHECK(cw_motion_match(&b, &dash, 12) >= 0);
  cw_cmd_clear(&b); push_dirs(&b, "6", 12, 0); /* held, not tapped twice */
  CHECK(cw_motion_match(&b, &dash, 12) < 0);
}

static void test_charge(void) {
  cw_cmd_buf b; cw_motion ch = mo("[4]6");
  cw_cmd_clear(&b); push_dirs(&b, "4", 35, 0); push_dirs(&b, "6", 2, 0);
  CHECK(cw_motion_match(&b, &ch, 60) >= 0);
  cw_cmd_clear(&b); push_dirs(&b, "4", 10, 0); push_dirs(&b, "6", 2, 0);
  CHECK(cw_motion_match(&b, &ch, 60) < 0);
  cw_cmd_clear(&b); push_dirs(&b, "1", 35, 0); push_dirs(&b, "6", 2, 0); /* down-back charges back */
  CHECK(cw_motion_match(&b, &ch, 60) >= 0);
}

static void test_buttons_and_buffer(void) {
  cw_cmd_buf b; cw_motion qcf = mo("236");
  /* motion, then the button */
  cw_cmd_clear(&b); push_dirs(&b, "236", 2, 1);
  CHECK_EQ_INT(cw_special_match(&b, &qcf, 1, 12, 4), 0);
  /* button 3 frames before the end of the motion's last frame: inside buffer 4 */
  cw_cmd_clear(&b); push_dirs(&b, "236", 2, 0); push_dirs(&b, "5", 3, 0);
  b.f[(b.count - 3) & (CW_CMD_HISTORY - 1)].buttons = 1;
  CHECK_EQ_INT(cw_special_match(&b, &qcf, 1, 12, 4), 2);
  CHECK_EQ_INT(cw_special_match(&b, &qcf, 1, 12, 1), -1); /* buffer too short */
  /* button first, motion finishes 2 frames after */
  cw_cmd_clear(&b); push_dirs(&b, "23", 2, 0);
  cw_cmd_push(&b, false, false, false, false, 1, 1); /* button down on a neutral frame */
  cw_cmd_push(&b, false, true, false, false, 1, 1);
  cw_cmd_push(&b, false, true, false, false, 1, 1);
  CHECK(cw_special_match(&b, &qcf, 1, 12, 4) >= 0);
  /* a held button is not a press */
  cw_cmd_clear(&b); push_dirs(&b, "5", 8, 0);
  for (int i = 0; i < 40; i++) cw_cmd_push(&b, 0, 0, 0, 0, 1, 1);
  push_dirs(&b, "236", 2, 1);
  CHECK_EQ_INT(cw_cmd_pressed_ago(&b, 1, 3), 0);
  CHECK(cw_cmd_released_ago(&b, 1, 10) >= 0);
  /* a stale motion does not fire a later button */
  cw_cmd_clear(&b); push_dirs(&b, "236", 2, 0); push_dirs(&b, "5", 40, 0); push_dirs(&b, "5", 1, 1);
  CHECK_EQ_INT(cw_special_match(&b, &qcf, 1, 12, 4), -1);
}

static void test_consume_facing(void) {
  cw_cmd_buf b; cw_motion qcf = mo("236");
  cw_cmd_clear(&b); push_dirs(&b, "236", 2, 1);
  CHECK(cw_special_match(&b, &qcf, 1, 12, 4) >= 0);
  cw_cmd_consume(&b);
  CHECK(cw_special_match(&b, &qcf, 1, 12, 4) < 0);
  CHECK(cw_motion_match(&b, &qcf, 20) < 0);
  /* facing left: down, down-left(forward), left(forward) */
  cw_cmd_clear(&b);
  for (int i = 0; i < 2; i++) cw_cmd_push(&b, false, false, false, true, -1, 0);
  for (int i = 0; i < 2; i++) cw_cmd_push(&b, true, false, false, true, -1, 0);
  for (int i = 0; i < 2; i++) cw_cmd_push(&b, true, false, false, false, -1, 0);
  CHECK(cw_motion_match(&b, &qcf, 20) >= 0);
  /* ring wrap: more frames than the history keeps */
  cw_cmd_clear(&b);
  for (int i = 0; i < 300; i++) cw_cmd_push(&b, 0, 0, 0, 0, 1, 0);
  push_dirs(&b, "236", 2, 0);
  CHECK(cw_motion_match(&b, &qcf, 20) >= 0);
}

CW_TEST_SUITE("motion", { "parse", test_parse }, { "quarter_circle", test_quarter_circle }, { "dp_super", test_dp_and_super },
              { "double_tap", test_double_tap }, { "charge", test_charge }, { "buttons", test_buttons_and_buffer },
              { "consume_facing", test_consume_facing })
