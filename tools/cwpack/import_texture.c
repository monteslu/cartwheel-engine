/* Texture importer: PNG/JPEG kept as they are (texture.format = keep, the
 * default) or encoded to KTX2 with Basis Universal (texture.format = ktx2):
 *   texture.mode     uastc (default: high quality, ~1 byte/texel before zstd)
 *                    | etc1s (small, lower quality)
 *   texture.quality  1..100 (codec default when absent; 60 for ETC1S)
 *   texture.effort   0..10  (codec default when absent)
 *   texture.mips     true (default): a full mip chain in the file */
#include <stdlib.h>
#include <string.h>

#include "cwpack.h"

unsigned char *stbi_load_from_memory(const unsigned char *buffer, int len, int *x, int *y, int *comp, int req);
void stbi_image_free(void *p);
const char *stbi_failure_reason(void);

static const uint8_t kKtx2[12] = { 0xAB, 'K', 'T', 'X', ' ', '2', '0', 0xBB, '\r', '\n', 0x1A, '\n' };

uint8_t *cwp_import_texture(const char *name, const uint8_t *src, size_t len, const cwp_settings *set, bool srgb,
                            size_t *out_len) {
  const char *fmt = cwp_settings_get(set, "texture.format", "keep");
  bool already = len >= 12 && !memcmp(src, kKtx2, 12);
  if (strcmp(fmt, "ktx2") || already) {
    if (strcmp(fmt, "keep") && strcmp(fmt, "ktx2")) cwp_report("%s: texture.format \"%s\" unknown (keep | ktx2): keeping it", name, fmt);
    uint8_t *o = cw_alloc(len ? len : 1, CW_MEM_GENERAL);
    memcpy(o, src, len);
    *out_len = len;
    return o;
  }
  int w, h, c;
  uint8_t *px = stbi_load_from_memory(src, (int)len, &w, &h, &c, 4);
  if (!px) {
    cwp_report("%s: cannot decode the image (%s)", name, stbi_failure_reason());
    return NULL;
  }
  cwp_basis_opts o = { 0 };
  const char *mode = cwp_settings_get(set, "texture.mode", "uastc");
  o.uastc = strcmp(mode, "etc1s") != 0;
  o.srgb = srgb;
  o.mips = cwp_settings_bool(set, "texture.mips", true);
  o.quality = (int)cwp_settings_num(set, "texture.quality", -1);
  o.effort = (int)cwp_settings_num(set, "texture.effort", -1);
  if (!o.uastc && o.quality < 0) o.quality = 60; /* ETC1S has no usable codec default (its frontend refuses -1) */
  uint8_t *out = NULL;
  char err[256];
  if (!cwp_basis_encode(px, w, h, &o, &out, out_len, err, sizeof err)) {
    cwp_report("%s: KTX2 encode failed: %s", name, err);
    stbi_image_free(px);
    return NULL;
  }
  stbi_image_free(px);
  return out;
}
