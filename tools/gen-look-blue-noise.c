/* Writes src/render/look_blue_noise.h, the looks' 64 x 64 blue noise tile, from tests/unit/look_blue_noise_gen.h (the
 * void and cluster the engine ran at run time up to v0.66.0). unit_looks holds the table to the generator on every
 * host (native and both cart backends), so a table made on one machine is checked everywhere.
 *   cc -O2 -ffp-contract=off -I tests/unit tools/gen-look-blue-noise.c -lm -o gen && ./gen > src/render/look_blue_noise.h */
#include <stdio.h>

#include "look_blue_noise_gen.h"

int main(void) {
  static uint8_t t[LBN * LBN];
  look_blue_noise_gen(t);
  printf("/* The looks' 64 x 64 blue noise tile (void and cluster, ranks scaled to 0..255), row by row from the top. Made by\n"
         " * tools/gen-look-blue-noise.c; unit_looks makes it again on every host and holds this table to it. */\n");
  printf("#ifndef CW_LOOK_BLUE_NOISE_H\n#define CW_LOOK_BLUE_NOISE_H\n\n#include <stdint.h>\n\n");
  printf("static const uint8_t cw__look_blue_noise[%d] = {\n", LBN * LBN);
  for (int i = 0; i < LBN * LBN; i++) printf("%s%3d,%s", i % 16 == 0 ? "  " : "", t[i], i % 16 == 15 ? "\n" : " ");
  printf("};\n\n#endif\n");
  return 0;
}
