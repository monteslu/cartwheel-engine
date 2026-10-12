/* Decoders behind one pull interface: miniaudio (WAV, FLAC, MP3),
 * stb_vorbis (Ogg Vorbis), libopus + libogg (Ogg Opus), libxmp-lite
 * (MOD, XM, S3M, IT). All decode from memory, output interleaved float with
 * at most two channels, and run on whichever thread owns the decoder (the
 * game thread, or a stream's decode job). */
#include <string.h>

#include "internal.h"

#define STB_VORBIS_HEADER_ONLY
#define STB_VORBIS_NO_STDIO
#include "stb_vorbis.c"

#include <ogg/ogg.h>
#include <opus.h>
#include <xmp.h>

/* ── detection ───────────────────────────────────────────────────────── */

static bool find(const uint8_t *p, size_t len, const char *needle, size_t limit) {
  size_t n = strlen(needle);
  if (limit > len) limit = len;
  for (size_t i = 0; i + n <= limit; i++)
    if (!memcmp(p + i, needle, n)) return true;
  return false;
}

cw__dec_kind cw__dec_detect(const void *data, size_t len) {
  const uint8_t *p = data;
  if (len < 4) return CW__DEC_NONE;
  if (!memcmp(p, "OggS", 4)) {
    if (find(p, len, "OpusHead", 256)) return CW__DEC_OPUS;
    if (find(p, len, "\x01vorbis", 256)) return CW__DEC_VORBIS;
    return CW__DEC_NONE;
  }
  if (!memcmp(p, "RIFF", 4) || !memcmp(p, "RIFX", 4) || !memcmp(p, "RF64", 4) || !memcmp(p, "fLaC", 4) ||
      !memcmp(p, "ID3", 3) || (p[0] == 0xff && (p[1] & 0xe0) == 0xe0))
    return CW__DEC_MA;
  if (xmp_test_module_from_memory(data, (long)len, NULL) == 0) return CW__DEC_XMP;
  return CW__DEC_MA; /* let miniaudio try the rest */
}

/* ── miniaudio ───────────────────────────────────────────────────────── */

static bool ma_open(cw__decoder *d) {
  ma_decoder *dec = cw_calloc(1, sizeof *dec, CW_MEM_AUDIO);
  ma_decoder_config cfg = ma_decoder_config_init(ma_format_f32, 0, 0);
  if (ma_decoder_init_memory(d->data, d->len, &cfg, dec) != MA_SUCCESS) {
    cw_free(dec);
    return false;
  }
  if (dec->outputChannels > 2) { /* keep the first two: reopen as stereo */
    ma_decoder_uninit(dec);
    cfg.channels = 2;
    if (ma_decoder_init_memory(d->data, d->len, &cfg, dec) != MA_SUCCESS) {
      cw_free(dec);
      return false;
    }
  }
  d->impl = dec;
  d->channels = (int)dec->outputChannels;
  d->rate = (int)dec->outputSampleRate;
  ma_uint64 n = 0;
  d->length = ma_decoder_get_length_in_pcm_frames(dec, &n) == MA_SUCCESS && n > 0 ? (int64_t)n : -1;
  return true;
}

/* ── stb_vorbis ──────────────────────────────────────────────────────── */

typedef struct { stb_vorbis *v; int src_ch; } vorbis_dec;

static bool vorbis_open(cw__decoder *d) {
  int err = 0;
  stb_vorbis *v = stb_vorbis_open_memory(d->data, (int)d->len, &err, NULL);
  if (!v) return false;
  vorbis_dec *vd = cw_calloc(1, sizeof *vd, CW_MEM_AUDIO);
  vd->v = v;
  stb_vorbis_info info = stb_vorbis_get_info(v);
  vd->src_ch = info.channels;
  d->impl = vd;
  d->channels = info.channels > 1 ? 2 : 1;
  d->rate = (int)info.sample_rate;
  unsigned n = stb_vorbis_stream_length_in_samples(v);
  d->length = n ? (int64_t)n : -1;
  return true;
}

static int vorbis_read(cw__decoder *d, float *out, int frames) {
  vorbis_dec *vd = d->impl;
  /* stb's interleaved reader takes the output channel count and drops or
   * mixes the rest; asking for 1 or 2 is exactly what we want */
  int got = 0;
  while (got < frames) {
    int n = stb_vorbis_get_samples_float_interleaved(vd->v, d->channels, out + got * d->channels,
                                                     (frames - got) * d->channels);
    if (n <= 0) break;
    got += n;
  }
  return got;
}

/* ── Ogg Opus: libogg pages -> libopus packets ───────────────────────── */

typedef struct {
  ogg_sync_state sync;
  ogg_stream_state os;
  bool os_ready;
  OpusDecoder *od;
  size_t fed;          /* bytes of the file handed to ogg_sync */
  int header_packets;  /* seen so far (2 = OpusHead + OpusTags) */
  int preskip, skip;   /* frames to drop at the start */
  int64_t total;       /* frames after pre-skip, from the last granule; -1 unknown */
  int64_t produced;    /* frames handed out */
  float pcm[5760 * 2];
  int pcm_pos, pcm_len;
  float gain;          /* OpusHead output gain, linear */
} opus_dec;

static int64_t opus_last_granule(const uint8_t *p, size_t len) {
  /* scan backwards for the last page header ("OggS", version 0) */
  for (size_t i = len >= 27 ? len - 27 : 0;; i--) {
    if (!memcmp(p + i, "OggS", 4) && p[i + 4] == 0) {
      int64_t g = 0;
      for (int k = 7; k >= 0; k--) g = (g << 8) | p[i + 6 + k];
      if (g >= 0) return g;
    }
    if (i == 0) break;
  }
  return -1;
}

static void opus_restart(opus_dec *o) {
  ogg_sync_reset(&o->sync);
  if (o->os_ready) ogg_stream_clear(&o->os);
  o->os_ready = false;
  o->fed = 0;
  o->header_packets = 0;
  o->skip = o->preskip;
  o->produced = 0;
  o->pcm_pos = o->pcm_len = 0;
  if (o->od) opus_decoder_ctl(o->od, OPUS_RESET_STATE);
}

/* Next packet of the logical stream; false at the end of the data. */
static bool opus_packet(cw__decoder *d, ogg_packet *pk) {
  opus_dec *o = d->impl;
  for (;;) {
    if (o->os_ready && ogg_stream_packetout(&o->os, pk) == 1) return true;
    ogg_page pg;
    while (ogg_sync_pageout(&o->sync, &pg) != 1) {
      if (o->fed >= d->len) return false;
      size_t n = CW_MIN((size_t)4096, d->len - o->fed);
      char *buf = ogg_sync_buffer(&o->sync, (long)n);
      memcpy(buf, d->data + o->fed, n);
      ogg_sync_wrote(&o->sync, (long)n);
      o->fed += n;
    }
    if (!o->os_ready) {
      ogg_stream_init(&o->os, ogg_page_serialno(&pg));
      o->os_ready = true;
    }
    if (ogg_page_serialno(&pg) == o->os.serialno) ogg_stream_pagein(&o->os, &pg);
  }
}

static bool opus_open(cw__decoder *d) {
  opus_dec *o = cw_calloc(1, sizeof *o, CW_MEM_AUDIO);
  d->impl = o;
  ogg_sync_init(&o->sync);
  ogg_packet pk;
  if (!opus_packet(d, &pk) || pk.bytes < 19 || memcmp(pk.packet, "OpusHead", 8)) goto fail;
  int ch = pk.packet[9];
  o->preskip = pk.packet[10] | (pk.packet[11] << 8);
  int16_t gain_q8 = (int16_t)(pk.packet[16] | (pk.packet[17] << 8));
  if (pk.packet[18] != 0 || ch < 1 || ch > 2) {
    cw_error("audio: Ogg Opus with %d channels / mapping family %d: only mono and stereo (family 0) are supported",
             ch, pk.packet[18]);
    goto fail;
  }
  o->gain = gain_q8 ? cw_pow(10.0f, (float)gain_q8 / (20.0f * 256.0f)) : 1.0f;
  int err = 0;
  o->od = opus_decoder_create(48000, ch, &err);
  if (!o->od || err != OPUS_OK) goto fail;
  d->channels = ch;
  d->rate = 48000; /* Opus always decodes at 48 kHz */
  int64_t g = opus_last_granule(d->data, d->len);
  o->total = g >= o->preskip ? g - o->preskip : -1;
  d->length = o->total;
  opus_restart(o);
  return true;
fail:
  if (o->od) opus_decoder_destroy(o->od);
  if (o->os_ready) ogg_stream_clear(&o->os);
  ogg_sync_clear(&o->sync);
  cw_free(o);
  d->impl = NULL;
  return false;
}

static int opus_read(cw__decoder *d, float *out, int frames) {
  opus_dec *o = d->impl;
  int ch = d->channels, got = 0;
  while (got < frames) {
    if (o->pcm_pos < o->pcm_len) {
      int n = CW_MIN(frames - got, o->pcm_len - o->pcm_pos);
      if (o->total >= 0 && o->produced + n > o->total) n = (int)(o->total - o->produced); /* end trim */
      if (n <= 0) break;
      memcpy(out + got * ch, o->pcm + o->pcm_pos * ch, (size_t)n * ch * sizeof(float));
      if (o->gain != 1.0f)
        for (int i = 0; i < n * ch; i++) out[got * ch + i] *= o->gain;
      o->pcm_pos += n;
      o->produced += n;
      got += n;
      continue;
    }
    ogg_packet pk;
    if (!opus_packet(d, &pk)) break;
    if (o->header_packets < 2) { o->header_packets++; continue; } /* OpusHead, OpusTags */
    int n = opus_decode_float(o->od, pk.packet, (opus_int32)pk.bytes, o->pcm, 5760, 0);
    if (n < 0) continue; /* corrupt packet: skip it */
    o->pcm_pos = 0;
    o->pcm_len = n;
    if (o->skip > 0) {
      int s = CW_MIN(o->skip, n);
      o->pcm_pos = s;
      o->skip -= s;
    }
  }
  return got;
}

static void opus_close(cw__decoder *d) {
  opus_dec *o = d->impl;
  opus_decoder_destroy(o->od);
  if (o->os_ready) ogg_stream_clear(&o->os);
  ogg_sync_clear(&o->sync);
  cw_free(o);
}

/* ── libxmp ──────────────────────────────────────────────────────────── */

typedef struct { xmp_context ctx; bool ended; int16_t buf[2048 * 2]; } xmp_dec;

#define CW__XMP_RATE 48000

static bool xmp_open(cw__decoder *d) {
  xmp_context ctx = xmp_create_context();
  if (!ctx) return false;
  if (xmp_load_module_from_memory(ctx, d->data, (long)d->len) != 0 || xmp_start_player(ctx, CW__XMP_RATE, 0) != 0) {
    xmp_free_context(ctx);
    return false;
  }
  xmp_dec *x = cw_calloc(1, sizeof *x, CW_MEM_AUDIO);
  x->ctx = ctx;
  d->impl = x;
  d->channels = 2;
  d->rate = CW__XMP_RATE;
  d->length = -1;
  return true;
}

static int xmp_read(cw__decoder *d, float *out, int frames) {
  xmp_dec *x = d->impl;
  int got = 0;
  while (got < frames && !x->ended) {
    int n = CW_MIN(frames - got, 2048);
    if (xmp_play_buffer(x->ctx, x->buf, n * 4, 1) != 0) { /* 1 = stop after one pass */
      x->ended = true;
      break;
    }
    for (int i = 0; i < n * 2; i++) out[got * 2 + i] = (float)x->buf[i] * (1.0f / 32768.0f);
    got += n;
  }
  return got;
}

static bool xmp_restart(cw__decoder *d) {
  xmp_dec *x = d->impl;
  xmp_end_player(x->ctx);
  x->ended = false;
  return xmp_start_player(x->ctx, CW__XMP_RATE, 0) == 0;
}

static void xmp_close(cw__decoder *d) {
  xmp_dec *x = d->impl;
  xmp_end_player(x->ctx);
  xmp_release_module(x->ctx);
  xmp_free_context(x->ctx);
  cw_free(x);
}

/* ── the interface ───────────────────────────────────────────────────── */

bool cw__dec_open(cw__decoder *d, const void *data, size_t len) {
  memset(d, 0, sizeof *d);
  d->data = data;
  d->len = len;
  d->kind = cw__dec_detect(data, len);
  bool ok = false;
  switch (d->kind) {
  case CW__DEC_MA: ok = ma_open(d); break;
  case CW__DEC_VORBIS: ok = vorbis_open(d); break;
  case CW__DEC_OPUS: ok = opus_open(d); break;
  case CW__DEC_XMP: ok = xmp_open(d); break;
  default: break;
  }
  if (!ok) d->kind = CW__DEC_NONE;
  return ok;
}

int cw__dec_read(cw__decoder *d, float *out, int frames) {
  switch (d->kind) {
  case CW__DEC_MA: {
    ma_uint64 n = 0;
    ma_decoder_read_pcm_frames(d->impl, out, (ma_uint64)frames, &n);
    return (int)n;
  }
  case CW__DEC_VORBIS: return vorbis_read(d, out, frames);
  case CW__DEC_OPUS: return opus_read(d, out, frames);
  case CW__DEC_XMP: return xmp_read(d, out, frames);
  default: return 0;
  }
}

bool cw__dec_seek(cw__decoder *d, int64_t frame) {
  switch (d->kind) {
  case CW__DEC_MA: return ma_decoder_seek_to_pcm_frame(d->impl, (ma_uint64)frame) == MA_SUCCESS;
  case CW__DEC_VORBIS: {
    vorbis_dec *vd = d->impl;
    return frame == 0 ? stb_vorbis_seek_start(vd->v) != 0 : stb_vorbis_seek(vd->v, (unsigned)frame) != 0;
  }
  case CW__DEC_OPUS:
  case CW__DEC_XMP: {
    /* no index: restart and decode forward */
    if (d->kind == CW__DEC_OPUS) opus_restart(d->impl);
    else if (!xmp_restart(d)) return false;
    float tmp[1024 * 2];
    while (frame > 0) {
      int n = cw__dec_read(d, tmp, (int)CW_MIN(frame, (int64_t)1024));
      if (n <= 0) return false;
      frame -= n;
    }
    return true;
  }
  default: return false;
  }
}

void cw__dec_close(cw__decoder *d) {
  switch (d->kind) {
  case CW__DEC_MA: ma_decoder_uninit(d->impl); cw_free(d->impl); break;
  case CW__DEC_VORBIS: {
    vorbis_dec *vd = d->impl;
    stb_vorbis_close(vd->v);
    cw_free(vd);
    break;
  }
  case CW__DEC_OPUS: opus_close(d); break;
  case CW__DEC_XMP: xmp_close(d); break;
  default: break;
  }
  memset(d, 0, sizeof *d);
}
