/* Looks (cartwheel/look.h): the 64 x 64 blue noise tile the dither looks and cw_look_blue_noise() read is a table the
 * engine ships (src/render/look_blue_noise.h); here the void and cluster that made it at run time up to v0.66.0 runs
 * again (tests/unit/look_blue_noise_gen.h) and the table must be those bytes, on every host (native, and the cart on
 * GL and WebGPU: wasm's float maths is the cart's). Every rank appears 16 times. The table is folded into
 * unit.checksum, which the gate pins. And the prewarm API refuses what it cannot prewarm. */
#include <string.h>

#include "cartwheel/look.h"
#include "look_blue_noise_gen.h"
#include "render/look_blue_noise.h"
#include "test.h"

static void test_blue_noise_table(void) {
  static uint8_t made[LBN * LBN];
  look_blue_noise_gen(made);
  int differ = 0, first = -1;
  for (int i = 0; i < LBN * LBN; i++)
    if (made[i] != cw__look_blue_noise[i]) differ++, first = first < 0 ? i : first;
  if (differ) cw_error("the blue noise table differs from the void and cluster at %d texels (first %d: %d, made %d)", differ, first,
                       cw__look_blue_noise[first], made[first]);
  CHECK_EQ_INT(differ, 0);
  int count[256] = { 0 };
  for (int i = 0; i < LBN * LBN; i++) count[cw__look_blue_noise[i]]++;
  int even = 1;
  for (int v = 0; v < 256; v++) even &= count[v] == 16;
  CHECK(even);
  /* the control: one texel changed must be told apart */
  made[17] ^= 1;
  CHECK(memcmp(made, cw__look_blue_noise, sizeof made) != 0);
  uint32_t h = cw_hash32(cw__look_blue_noise, sizeof cw__look_blue_noise, 0);
  FOLD(h);
}

static void test_prewarm_api(void) {
  CHECK(!cw_look_prewarm(NULL, (cw_look){ 0 }));
  CHECK(cw_look_prewarm_all(NULL) == 0);
  CHECK(cw_pipeline_look_prewarms(NULL) == 0);
}

CW_TEST_SUITE("looks", { "blue_noise_table", test_blue_noise_table }, { "prewarm_api", test_prewarm_api })
