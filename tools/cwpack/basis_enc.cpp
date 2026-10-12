// RGBA8 -> KTX2 (Basis Universal UASTC LDR 4x4 + zstd, or ETC1S), for
// cwpack's texture importer. Single-threaded on purpose: identical input must
// give identical bytes.
#include <stdio.h>
#include <string.h>

#include "basisu_comp.h"
#include "basisu_enc.h"

extern "C" {
#include "cwpack.h"
}

extern "C" void cwp_basis_init(void) {
  static bool inited;
  if (!inited) {
    basisu::basisu_encoder_init(false, false);
    inited = true;
  }
}

extern "C" bool cwp_basis_encode(const uint8_t *rgba, int w, int h, const cwp_basis_opts *o, uint8_t **out,
                                 size_t *out_len, char *err, size_t err_cap) {
  cwp_basis_init();
  basisu::image img((uint32_t)w, (uint32_t)h);
  memcpy(img.get_ptr(), rgba, (size_t)w * h * 4);
  basisu::basis_compressor_params p;
  p.m_source_images.push_back(img);
  p.set_format_mode_and_quality_effort(o->uastc ? basist::basis_tex_format::cUASTC_LDR_4x4 : basist::basis_tex_format::cETC1S,
                                       o->quality, o->effort);
  p.set_srgb_options(o->srgb);
  p.m_mip_gen = o->mips;
  p.m_create_ktx2_file = true;
  if (o->uastc) p.m_ktx2_uastc_supercompression = basist::KTX2_SS_ZSTANDARD;
  p.m_multithreading = false;
  p.m_status_output = false;
  p.m_debug = false;
  p.m_compute_stats = false;
  p.m_read_source_images = false;
  p.m_write_output_basis_or_ktx2_files = false;
  basisu::job_pool pool(1);
  p.m_pJob_pool = &pool;
  basisu::basis_compressor c;
  if (!c.init(p)) { snprintf(err, err_cap, "compressor init failed"); return false; }
  basisu::basis_compressor::error_code e = c.process();
  if (e != basisu::basis_compressor::cECSuccess) { snprintf(err, err_cap, "compressor error %d", (int)e); return false; }
  const basisu::uint8_vec &k = c.get_output_ktx2_file();
  *out_len = k.size();
  *out = (uint8_t *)cw_alloc(k.size() ? k.size() : 1, CW_MEM_GENERAL);
  memcpy(*out, k.data(), k.size());
  return true;
}
