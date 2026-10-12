// KTX2 (Basis Universal ETC1S / UASTC) transcoding for texture resources.
// C++ only because the transcoder is; the C side sees resource_internal.h.
// Worker-safe: every call builds its own ktx2_transcoder (and state).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "basisu_transcoder.h"

extern "C" {
#include "resource_internal.h"
}

static const uint8_t kKtx2Magic[12] = { 0xAB, 'K', 'T', 'X', ' ', '2', '0', 0xBB, '\r', '\n', 0x1A, '\n' };

extern "C" bool cw__ktx2_is(const uint8_t *bytes, size_t len) {
  return len >= 12 && memcmp(bytes, kKtx2Magic, 12) == 0;
}

extern "C" void cw__ktx2_init(void) {
  static bool done;
  if (!done) {
    basist::basisu_transcoder_init();
    done = true;
  }
}

/* GL internal formats (GLES 3.0 + extensions) */
#define GL_RGBA8_ 0x8058
#define GL_COMPRESSED_RGBA_BPTC_UNORM_EXT_ 0x8E8C
#define GL_COMPRESSED_RGBA_ASTC_4x4_KHR_ 0x93B0
#define GL_COMPRESSED_RGBA8_ETC2_EAC_ 0x9278
#define GL_COMPRESSED_RGBA_S3TC_DXT5_EXT_ 0x83F3

extern "C" bool cw__ktx2_transcode(const uint8_t *bytes, size_t len, int target, cw_tex_cpu *out, char *err,
                                   size_t err_cap) {
  memset(out, 0, sizeof *out);
  basist::ktx2_transcoder tr;
  if (!tr.init(bytes, (uint32_t)len)) { snprintf(err, err_cap, "not a KTX2 file Basis Universal can read"); return false; }
  if (!(tr.is_etc1s() || tr.is_uastc())) { snprintf(err, err_cap, "KTX2 is not ETC1S or UASTC LDR (re-encode with cwpack)"); return false; }
  if (tr.get_layers() > 1 || tr.get_faces() > 1) { snprintf(err, err_cap, "KTX2 arrays and cubemaps are not supported yet"); return false; }
  if (!tr.start_transcoding()) { snprintf(err, err_cap, "KTX2 start_transcoding failed"); return false; }
  basist::transcoder_texture_format fmt;
  uint32_t glf;
  const char *name;
  switch (target) {
  case CW_KTX2_BC7: fmt = basist::transcoder_texture_format::cTFBC7_RGBA; glf = GL_COMPRESSED_RGBA_BPTC_UNORM_EXT_; name = "bc7"; break;
  case CW_KTX2_ASTC4x4: fmt = basist::transcoder_texture_format::cTFASTC_LDR_4x4_RGBA; glf = GL_COMPRESSED_RGBA_ASTC_4x4_KHR_; name = "astc4x4"; break;
  case CW_KTX2_ETC2: fmt = basist::transcoder_texture_format::cTFETC2_RGBA; glf = GL_COMPRESSED_RGBA8_ETC2_EAC_; name = "etc2"; break;
  case CW_KTX2_BC3: fmt = basist::transcoder_texture_format::cTFBC3_RGBA; glf = GL_COMPRESSED_RGBA_S3TC_DXT5_EXT_; name = "bc3"; break;
  default: fmt = basist::transcoder_texture_format::cTFRGBA32; glf = GL_RGBA8_; name = "rgba8"; break;
  }
  bool raw = basist::basis_transcoder_format_is_uncompressed(fmt);
  uint32_t bpb = basist::basis_get_bytes_per_block_or_pixel(fmt);
  int levels = (int)tr.get_levels();
  if (levels < 1) levels = 1;
  if (levels > 16) levels = 16;
  for (int l = 0; l < levels; l++) {
    basist::ktx2_image_level_info li;
    if (!tr.get_image_level_info(li, (uint32_t)l, 0, 0)) { snprintf(err, err_cap, "KTX2 level %d info failed", l); cw__tex_cpu_free(out); return false; }
    uint32_t units = raw ? li.m_orig_width * li.m_orig_height : li.m_total_blocks;
    size_t size = (size_t)units * bpb;
    uint8_t *dst = (uint8_t *)cw_alloc(size ? size : 1, CW_MEM_RESOURCE);
    if (!tr.transcode_image_level((uint32_t)l, 0, 0, dst, units, fmt)) {
      cw_free(dst);
      snprintf(err, err_cap, "KTX2 level %d transcode to %s failed", l, name);
      cw__tex_cpu_free(out);
      return false;
    }
    out->level[l].data = dst;
    out->level[l].size = size;
    out->level[l].w = (int)li.m_orig_width;
    out->level[l].h = (int)li.m_orig_height;
  }
  out->width = out->level[0].w;
  out->height = out->level[0].h;
  out->levels = levels;
  out->gl_format = glf;
  out->compressed = !raw;
  out->from_ktx2 = true;
  out->srgb = tr.is_srgb();
  out->gen_mips = false;
  out->format = name;
  return true;
}
