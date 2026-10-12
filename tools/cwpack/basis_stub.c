/* Basis Universal's encoder is C++ with exceptions, which the cart
 * toolchain does not build: in a cart (the editor as a cart) KTX2 encoding
 * reports that and the import fails with that message. The native editor
 * and cwpack (which runs at build time for every cart) encode KTX2. */
#include <stdio.h>

#include "cwpack.h"

void cwp_basis_init(void) {}

bool cwp_basis_encode(const uint8_t *rgba, int w, int h, const cwp_basis_opts *o, uint8_t **out, size_t *out_len,
                      char *err, size_t err_cap) {
  (void)rgba, (void)w, (void)h, (void)o;
  *out = NULL;
  *out_len = 0;
  snprintf(err, err_cap, "KTX2 encoding is not built into carts (Basis Universal's encoder needs C++ exceptions); "
                         "the native editor or cwpack at build time encodes it: keep texture.format = keep here");
  return false;
}
