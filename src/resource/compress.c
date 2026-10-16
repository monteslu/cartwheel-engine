/* Whole-buffer decompression for packed resources (cartwheel/resource.h):
 * zstd frames through the zstd decoder basis_universal ships (the one KTX2
 * supercompression already links), lz4 frames and blocks through lz4's lib/.
 * Every call makes its own context, so any thread may call. */
#include <string.h>

#include "cartwheel/core.h"
#include "cartwheel/resource.h"
#include "lz4.h"
#include "lz4frame.h"
#include "zstd.h"

cw_compression cw_compression_of(const char *path) {
  size_t n = path ? strlen(path) : 0;
  if (n > 4 && !strcmp(path + n - 4, ".zst")) return CW_COMPRESS_ZSTD;
  if (n > 4 && !strcmp(path + n - 4, ".lz4")) return CW_COMPRESS_LZ4;
  return CW_COMPRESS_NONE;
}

/* grow a cw_alloc'd buffer to at least `need` (+1 for the NUL) */
static uint8_t *grow(uint8_t *b, size_t *cap, size_t need) {
  if (need + 1 <= *cap) return b;
  size_t c = *cap ? *cap : 4096;
  while (c < need + 1) c *= 2;
  *cap = c;
  return cw_realloc(b, c, CW_MEM_RESOURCE);
}

static uint8_t *unzstd(const uint8_t *src, size_t len, size_t *out_len) {
  unsigned long long size = ZSTD_getFrameContentSize(src, len);
  if (size == ZSTD_CONTENTSIZE_ERROR) return NULL;
  if (size != ZSTD_CONTENTSIZE_UNKNOWN) { /* one call into the exact size */
    if (size > ((size_t)-1) / 2) return NULL;
    uint8_t *out = cw_alloc((size_t)size + 1, CW_MEM_RESOURCE);
    ZSTD_DCtx *dc = ZSTD_createDCtx();
    size_t r = dc ? ZSTD_decompressDCtx(dc, out, (size_t)size, src, len) : (size_t)-1;
    ZSTD_freeDCtx(dc);
    if (ZSTD_isError(r) || r != size) { cw_free(out); return NULL; }
    out[size] = 0;
    *out_len = (size_t)size;
    return out;
  }
  /* streamed without a size: grow as it decodes */
  ZSTD_DStream *ds = ZSTD_createDStream();
  if (!ds) return NULL;
  size_t cap = 0, n = 0;
  uint8_t *out = grow(NULL, &cap, len * 4);
  ZSTD_inBuffer in = { src, len, 0 };
  size_t r = 1;
  while (r != 0) {
    out = grow(out, &cap, n + ZSTD_DStreamOutSize());
    ZSTD_outBuffer o = { out + n, cap - 1 - n, 0 };
    r = ZSTD_decompressStream(ds, &o, &in);
    if (ZSTD_isError(r)) { cw_free(out); ZSTD_freeDStream(ds); return NULL; }
    n += o.pos;
    if (in.pos == in.size && o.pos == 0 && r != 0) { cw_free(out); ZSTD_freeDStream(ds); return NULL; } /* truncated */
  }
  ZSTD_freeDStream(ds);
  out[n] = 0;
  *out_len = n;
  return out;
}

static uint8_t *unlz4(const uint8_t *src, size_t len, size_t *out_len) {
  LZ4F_dctx *dc;
  if (LZ4F_isError(LZ4F_createDecompressionContext(&dc, LZ4F_VERSION))) return NULL;
  LZ4F_frameInfo_t fi;
  size_t used = len;
  size_t r = LZ4F_getFrameInfo(dc, &fi, src, &used);
  if (LZ4F_isError(r)) { LZ4F_freeDecompressionContext(dc); return NULL; }
  size_t cap = 0, n = 0, at = used;
  uint8_t *out = grow(NULL, &cap, fi.contentSize ? (size_t)fi.contentSize : len * 4);
  while (r != 0) {
    if (cap - 1 - n < 65536) out = grow(out, &cap, cap * 2);
    size_t dst = cap - 1 - n, srcn = len - at;
    r = LZ4F_decompress(dc, out + n, &dst, src + at, &srcn, NULL);
    if (LZ4F_isError(r)) { cw_free(out); LZ4F_freeDecompressionContext(dc); return NULL; }
    n += dst;
    at += srcn;
    if (at >= len && dst == 0 && r != 0) { cw_free(out); LZ4F_freeDecompressionContext(dc); return NULL; } /* truncated */
  }
  LZ4F_freeDecompressionContext(dc);
  if (fi.contentSize && n != fi.contentSize) { cw_free(out); return NULL; }
  out[n] = 0;
  *out_len = n;
  return out;
}

uint8_t *cw_decompress(cw_compression c, const void *src, size_t len, size_t *out_len) {
  *out_len = 0;
  if (!src) return NULL;
  switch (c) {
  case CW_COMPRESS_ZSTD: return unzstd(src, len, out_len);
  case CW_COMPRESS_LZ4: return unlz4(src, len, out_len);
  default: {
    uint8_t *b = cw_alloc(len + 1, CW_MEM_RESOURCE);
    memcpy(b, src, len);
    b[len] = 0;
    *out_len = len;
    return b;
  }
  }
}

bool cw_decompress_block(cw_compression c, const void *src, size_t len, void *dst, size_t raw_len) {
  if (c == CW_COMPRESS_ZSTD) {
    ZSTD_DCtx *dc = ZSTD_createDCtx();
    size_t r = dc ? ZSTD_decompressDCtx(dc, dst, raw_len, src, len) : (size_t)-1;
    ZSTD_freeDCtx(dc);
    return !ZSTD_isError(r) && r == raw_len;
  }
  if (c == CW_COMPRESS_LZ4) {
    if (len > 0x7fffffff || raw_len > 0x7fffffff) return false;
    return LZ4_decompress_safe(src, dst, (int)len, (int)raw_len) == (int)raw_len;
  }
  if (len != raw_len) return false;
  memcpy(dst, src, len);
  return true;
}
