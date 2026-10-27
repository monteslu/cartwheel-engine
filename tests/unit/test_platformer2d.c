/* Platformer controller feel constants, each measured by replaying input on a small course (cartwheel/platformer2d.h):
 * coyote time, jump buffer, variable jump height, fall cap, acceleration, corner correction and ledge forgiveness (the round feet).
 * Every feature that forgives something has its control inside the test: the same course with the forgiveness
 * turned off must NOT pass the same check. Results fold into unit.checksum (cross-host). */
#include "cartwheel/platformer2d.h"
#include "test.h"

#define DT (1.0f / 60)
#define NEAR(a, b, eps) (fabsf((a) - (b)) <= (eps))

static cw_world2d new_world(void) {
  cw_world2d_def wd = cw_world2d_default_def();
  wd.gravity = (cw_vec2){ 0, 0 };
  return cw_world2d_create(&wd);
}
static void block(cw_world2d w, float x0, float x1, float y0, float y1) {
  cw_body2d_def bd = cw_body2d_default_def();
  bd.type = CW_BODY_STATIC;
  cw_body2d b = cw_body2d_create(w, &bd);
  cw_shape2d_def sd = cw_shape2d_default_def();
  cw_shape2d_box(b, &sd, (cw_vec2){ (x1 - x0) / 2, (y1 - y0) / 2 }, cw_xform2_at((x0 + x1) / 2, (y0 + y1) / 2));
}
static cw_platformer2d_def def(void) {
  cw_platformer2d_def d = cw_platformer2d_default_def();
  d.mover = cw_mover2d_default_def();
  return d;
}
static void settle(cw_platformer2d *p) {
  cw_platformer2d_input none = { 0 };
  for (int i = 0; i < 30; i++) cw_platformer2d_step(p, &none, DT);
}

/* walk off a ledge and press jump k frames after the last grounded frame: did it jump? */
static bool coyote_jump(int k) {
  cw_world2d w = new_world();
  block(w, -30, 0, -1, 0); /* floor ends at x = 0 */
  cw_platformer2d_def d = def();
  cw_platformer2d p;
  cw_platformer2d_init(&p, w, &d, (cw_pos2){ -2, 0.01f });
  settle(&p);
  bool jumped = false;
  int air = -1; /* steps taken since the feet left the ground (0 = the first step that started in the air) */
  for (int i = 0; i < 200 && !jumped; i++) {
    cw_platformer2d_input in = { .move_x = 1 };
    if (air >= 0 && air == k) in.jump_pressed = in.jump_held = true;
    bool airborne_at_start = !p.body.on_floor;
    cw_platformer2d_step(&p, &in, DT);
    if (airborne_at_start) air++;
    else if (!p.body.on_floor) air = 0; /* this step carried it off the edge: the next step is air step 0 */
    jumped = (p.events & CW_PF_JUMP) != 0;
  }
  cw_world2d_destroy(w);
  return jumped;
}

/* drop from a height; press jump k frames before touching down; did it jump within 3 frames of landing? */
static bool buffer_jump(int k) {
  cw_world2d w = new_world();
  block(w, -30, 30, -1, 0);
  cw_platformer2d_def d = def();
  cw_platformer2d p;
  cw_platformer2d_init(&p, w, &d, (cw_pos2){ 0, 4 });
  /* find the landing frame first */
  int land = -1;
  for (int i = 0; i < 300 && land < 0; i++) {
    cw_platformer2d_input none = { 0 };
    cw_platformer2d_step(&p, &none, DT);
    if (p.events & CW_PF_LAND) land = i;
  }
  cw_platformer2d_init(&p, w, &d, (cw_pos2){ 0, 4 });
  bool jumped = false;
  for (int i = 0; i < land + 6; i++) {
    cw_platformer2d_input in = { 0 };
    if (i == land - k) in.jump_pressed = in.jump_held = true;
    if (i >= land - k && i < land - k + 3) in.jump_held = true;
    cw_platformer2d_step(&p, &in, DT);
    if (i >= land - 1 && (p.events & CW_PF_JUMP)) jumped = true;
  }
  cw_world2d_destroy(w);
  return jumped;
}

static float jump_height(int hold_frames) {
  cw_world2d w = new_world();
  block(w, -30, 30, -1, 0);
  cw_platformer2d_def d = def();
  cw_platformer2d p;
  cw_platformer2d_init(&p, w, &d, (cw_pos2){ 0, 0.01f });
  settle(&p);
  float top = 0;
  for (int i = 0; i < 120; i++) {
    cw_platformer2d_input in = { .jump_pressed = i == 0, .jump_held = i < hold_frames };
    cw_platformer2d_step(&p, &in, DT);
    if ((float)p.body.position.y > top) top = (float)p.body.position.y;
  }
  cw_world2d_destroy(w);
  return top;
}

static float fall_speed(void) {
  cw_world2d w = new_world();
  cw_platformer2d_def d = def();
  cw_platformer2d p;
  cw_platformer2d_init(&p, w, &d, (cw_pos2){ 0, 200 });
  float vmax = 0;
  for (int i = 0; i < 240; i++) {
    cw_platformer2d_input none = { 0 };
    cw_platformer2d_step(&p, &none, DT);
    if (-p.body.velocity.y > vmax) vmax = -p.body.velocity.y;
  }
  cw_world2d_destroy(w);
  return vmax;
}

static void test_feel(void) {
  int c_ok = -1, c_fail = -1;
  for (int k = 0; k <= 14; k++) {
    bool j = coyote_jump(k);
    if (j) c_ok = k;
    else if (c_fail < 0) c_fail = k;
  }
  /* rubric: succeeds for k <= 6, fails for k >= 8 */
  cw_info("platformer2d: coyote window ends after k=%d", c_ok);
  CHECK(c_ok >= 5 && c_ok <= 7);
  CHECK(c_fail > c_ok && !coyote_jump(10) && !coyote_jump(14));
  CHECK(coyote_jump(0) && coyote_jump(3));
  FOLD(c_ok);
  int b_ok = -1;
  for (int k = 0; k <= 14; k++)
    if (buffer_jump(k)) b_ok = k;
  cw_info("platformer2d: jump buffer window k=%d", b_ok);
  CHECK(b_ok >= 4 && b_ok <= 8); /* the window: at least 4 frames, at most 8 */
  CHECK(!buffer_jump(14) && !buffer_jump(30));
  CHECK(buffer_jump(1) && buffer_jump(4));
  FOLD(b_ok);
  float full = jump_height(40), tap = jump_height(1);
  CHECK(NEAR(full, 3.2f, 0.25f));
  CHECK(tap <= full * 0.5f && tap > 0.3f); /* a tap is a hop, not nothing */
  FOLD(full);
  FOLD(tap);
  float fall = fall_speed();
  float jv = sqrtf(2 * 38.0f * 3.2f);
  CHECK(fall <= 22.01f && fall < 3 * jv);
  FOLD(fall);
}

/* a rising head that clips the corner of a ceiling block, `overlap` metres of its width under the block */
static float corner_top(float correction, float overlap) {
  cw_world2d w = new_world();
  block(w, -30, 30, -1, 0);
  block(w, 0, 10, 3.3f, 5.0f); /* ceiling block with its left edge at x = 0 and its underside at y = 3.3 */
  cw_platformer2d_def d = def();
  d.corner_correction = correction;
  cw_platformer2d p;
  float x = -0.3f + overlap; /* the capsule's right edge is x + 0.3 */
  cw_platformer2d_init(&p, w, &d, (cw_pos2){ x, 0.01f });
  settle(&p);
  float top = 0;
  for (int i = 0; i < 80; i++) {
    cw_platformer2d_input in = { .jump_pressed = i == 0, .jump_held = i < 40 };
    cw_platformer2d_step(&p, &in, DT);
    if ((float)p.body.position.y > top) top = (float)p.body.position.y;
  }
  cw_world2d_destroy(w);
  return top;
}

static void test_corner(void) {
  /* the underside is 3.3 m up: the head (1.8 m) stops the feet at 1.5 m unless the body gets round the corner */
  float on = corner_top(0.25f, 0.15f), off = corner_top(0, 0.15f);
  cw_info("platformer2d: corner on=%.3f off=%.3f deep=%.3f", on, off, corner_top(0.25f, 0.28f));
  CHECK(on > 2.9f); /* kept rising past the block's underside level */
  CHECK(off < 2.0f); /* CONTROL: without the correction the same jump bonks the ceiling */
  FOLD(on);
  FOLD(off);
  /* a deep clip (more than the nudge) still bonks */
  float deep = corner_top(0.25f, 0.28f);
  CHECK(deep < 2.5f);
  FOLD(deep);
}

/* falling toward a ledge: feet `below` metres under its top edge, pushing into the wall */
static float ledge_end_y(float below) {
  cw_world2d w = new_world();
  block(w, -30, 30, -1, 0);   /* floor, top at 0 */
  block(w, 2, 30, 0, 1.0f);   /* a ledge, top at 1.0 */
  cw_platformer2d_def d = def();
  cw_platformer2d p;
  cw_platformer2d_init(&p, w, &d, (cw_pos2){ 1.69f, 1.0f - below });
  p.body.velocity = (cw_vec2){ 0, -1 };
  for (int i = 0; i < 40; i++) {
    cw_platformer2d_input in = { .move_x = 1 };
    cw_platformer2d_step(&p, &in, DT);
  }
  float y = (float)p.body.position.y;
  cw_world2d_destroy(w);
  return y;
}

/* ledge forgiveness comes from the capsule's round feet: a fall that misses the top by up to 0.2 m (3 px at 16 px a
 * tile) ends on the ledge; one that misses by 0.3 m or more is a wall and slides down it */
static void test_ledge(void) {
  float a = ledge_end_y(0.05f), b = ledge_end_y(0.2f), c = ledge_end_y(0.3f), d = ledge_end_y(0.5f);
  CHECK(NEAR(a, 1.0f, 0.03f) && NEAR(b, 1.0f, 0.03f)); /* up on the ledge */
  CHECK(c < 0.2f && d < 0.2f);                           /* CONTROL: further under the edge it is a wall: down to the floor */
  FOLD(a);
  FOLD(b);
  FOLD(c);
  FOLD(d);
}

static void test_accel(void) {
  cw_world2d w = new_world();
  block(w, -30, 60, -1, 0);
  cw_platformer2d_def d = def();
  cw_platformer2d p;
  cw_platformer2d_init(&p, w, &d, (cw_pos2){ 0, 0.01f });
  settle(&p);
  int to_full = 0, to_stop = 0;
  for (int i = 0; i < 60 && p.body.velocity.x < d.run_speed - 0.01f; i++, to_full++) {
    cw_platformer2d_input in = { .move_x = 1 };
    cw_platformer2d_step(&p, &in, DT);
  }
  for (int i = 0; i < 60 && p.body.velocity.x > 0.01f; i++, to_stop++) {
    cw_platformer2d_input none = { 0 };
    cw_platformer2d_step(&p, &none, DT);
  }
  CHECK(to_full >= 6 && to_full <= 12); /* rubric: 6 to 12 frames up to speed */
  CHECK(to_stop >= 4 && to_stop <= 8);  /* 4 to 8 frames to stop */
  CHECK(d.air_accel >= 0.6f * d.ground_accel);
  FOLD(to_full);
  FOLD(to_stop);
  cw_world2d_destroy(w);
}

CW_TEST_SUITE("platformer2d", { "feel", test_feel }, { "corner", test_corner }, { "ledge", test_ledge }, { "accel", test_accel })
