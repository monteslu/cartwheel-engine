/* Audio importer: WAV / FLAC / MP3 kept as they are (audio.format = keep,
 * the default) or encoded to Ogg Opus (audio.format = opus), which the audio
 * module decodes on every host (src/audio/decode.c):
 *   audio.bitrate     bits per second (default 64000 mono, 96000 stereo)
 *   audio.complexity  0..10 (default 10)
 * The file keeps its resource path (music.wav may hold Ogg Opus: loaders
 * detect the format from the bytes, as with KTX2 textures). Ogg Vorbis, Ogg
 * Opus and tracker modules pass through untouched. libopus is the
 * fixed-point build, so the encoded bytes are the same on every machine. */
#include <stdlib.h>
#include <string.h>

#include "cwpack.h"
#include "miniaudio.h"
#include "ogg/ogg.h"
#include "opus.h"

static bool passthrough(const uint8_t *b, size_t n) {
  if (n >= 4 && !memcmp(b, "OggS", 4)) return true; /* Vorbis or Opus already */
  /* tracker modules: XM, IT, S3M signatures (MOD has no reliable magic: by extension) */
  if (n >= 17 && !memcmp(b, "Extended Module:", 16)) return true;
  if (n >= 4 && !memcmp(b, "IMPM", 4)) return true;
  if (n >= 48 && !memcmp(b + 44, "SCRM", 4)) return true;
  return false;
}

static void put16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) { put16(p, v); put16(p + 2, v >> 16); }

typedef CW_ARRAY(uint8_t) bytes;
static void emit_page(bytes *o, const ogg_page *pg) {
  cw_array_reserve(o, o->count + (int)(pg->header_len + pg->body_len));
  memcpy(o->data + o->count, pg->header, (size_t)pg->header_len);
  o->count += (int)pg->header_len;
  memcpy(o->data + o->count, pg->body, (size_t)pg->body_len);
  o->count += (int)pg->body_len;
}

uint8_t *cwp_import_audio(const char *name, const uint8_t *src, size_t len, const cwp_settings *set,
                          size_t *out_len, char *note, size_t note_cap) {
  const char *fmt = cwp_settings_get(set, "audio.format", "keep");
  if (strcmp(fmt, "opus") || passthrough(src, len)) {
    if (strcmp(fmt, "keep") && strcmp(fmt, "opus"))
      cwp_report("%s: audio.format \"%s\" unknown (keep | opus): keeping it", name, fmt);
    uint8_t *o = cw_alloc(len ? len : 1, CW_MEM_GENERAL);
    memcpy(o, src, len);
    *out_len = len;
    return o;
  }
  /* decode to 16-bit at 48 kHz (Opus's rate), mono or stereo */
  ma_decoder probe;
  if (ma_decoder_init_memory(src, len, NULL, &probe) != MA_SUCCESS) {
    cwp_report("%s: not audio miniaudio decodes (WAV, FLAC, MP3)", name);
    return NULL;
  }
  ma_uint32 src_ch, src_rate;
  ma_decoder_get_data_format(&probe, NULL, &src_ch, &src_rate, NULL, 0);
  ma_decoder_uninit(&probe);
  int ch = src_ch >= 2 ? 2 : 1;
  ma_decoder_config cfg = ma_decoder_config_init(ma_format_s16, (ma_uint32)ch, 48000);
  ma_decoder dec;
  if (ma_decoder_init_memory(src, len, &cfg, &dec) != MA_SUCCESS) {
    cwp_report("%s: cannot decode it", name);
    return NULL;
  }
  CW_ARRAY(int16_t) pcm = { 0 };
  for (;;) {
    int16_t buf[4096 * 2];
    ma_uint64 got = 0;
    ma_decoder_read_pcm_frames(&dec, buf, 4096, &got);
    if (!got) break;
    cw_array_reserve(&pcm, pcm.count + (int)got * ch);
    memcpy(pcm.data + pcm.count, buf, (size_t)got * (size_t)ch * 2);
    pcm.count += (int)got * ch;
  }
  ma_decoder_uninit(&dec);
  int frames = pcm.count / ch;
  int err;
  OpusEncoder *enc = opus_encoder_create(48000, ch, OPUS_APPLICATION_AUDIO, &err);
  if (err != OPUS_OK) {
    cwp_report("%s: opus_encoder_create: %s", name, opus_strerror(err));
    cw_array_free(&pcm);
    return NULL;
  }
  int bitrate = (int)cwp_settings_num(set, "audio.bitrate", ch == 2 ? 96000 : 64000);
  opus_encoder_ctl(enc, OPUS_SET_BITRATE(bitrate));
  opus_encoder_ctl(enc, OPUS_SET_COMPLEXITY((int)cwp_settings_num(set, "audio.complexity", 10)));
  opus_encoder_ctl(enc, OPUS_SET_VBR(1));
  opus_int32 lookahead = 0;
  opus_encoder_ctl(enc, OPUS_GET_LOOKAHEAD(&lookahead));
  /* Ogg Opus (RFC 7845): OpusHead, OpusTags, then 20 ms packets */
  ogg_stream_state os;
  ogg_stream_init(&os, 0x63777061); /* a fixed serial: identical input, identical bytes */
  bytes out = { 0 };
  ogg_page pg;
  uint8_t head[19];
  memcpy(head, "OpusHead", 8);
  head[8] = 1;
  head[9] = (uint8_t)ch;
  put16(head + 10, (uint32_t)lookahead);
  put32(head + 12, src_rate);
  put16(head + 16, 0);
  head[18] = 0;
  ogg_packet op = { head, sizeof head, 1, 0, 0, 0 };
  ogg_stream_packetin(&os, &op);
  while (ogg_stream_flush(&os, &pg)) emit_page(&out, &pg);
  static const char vendor[] = "cartwheel cwpack (libopus)";
  uint8_t tags[8 + 4 + sizeof vendor - 1 + 4];
  memcpy(tags, "OpusTags", 8);
  put32(tags + 8, sizeof vendor - 1);
  memcpy(tags + 12, vendor, sizeof vendor - 1);
  put32(tags + 12 + sizeof vendor - 1, 0);
  op = (ogg_packet){ tags, sizeof tags, 0, 0, 0, 1 };
  ogg_stream_packetin(&os, &op);
  while (ogg_stream_flush(&os, &pg)) emit_page(&out, &pg);
  const int frame = 960;
  int16_t *block = cw_calloc((size_t)frame * (size_t)ch, 2, CW_MEM_GENERAL);
  uint8_t packet[4000];
  int64_t packetno = 2, encoded = 0;
  /* encode past the end by the lookahead so the last real samples come out */
  int64_t total = (int64_t)frames + lookahead;
  bool ok = true;
  for (int64_t at = 0; at < total || at == 0; at += frame) {
    int n = (int)CW_MIN((int64_t)frame, (int64_t)frames - at);
    memset(block, 0, (size_t)frame * (size_t)ch * 2);
    if (n > 0) memcpy(block, pcm.data + at * ch, (size_t)n * (size_t)ch * 2);
    int bytes_n = opus_encode(enc, block, frame, packet, sizeof packet);
    if (bytes_n < 0) {
      cwp_report("%s: opus_encode: %s", name, opus_strerror(bytes_n));
      ok = false;
      break;
    }
    encoded += frame;
    bool last = at + frame >= total;
    /* granule: samples decoded so far including the pre-skip; the last page
     * trims to the real length */
    int64_t granule = last ? (int64_t)frames + lookahead : encoded;
    op = (ogg_packet){ packet, bytes_n, 0, last, granule, packetno++ };
    ogg_stream_packetin(&os, &op);
    while (ogg_stream_pageout(&os, &pg)) emit_page(&out, &pg);
  }
  while (ogg_stream_flush(&os, &pg)) emit_page(&out, &pg);
  ogg_stream_clear(&os);
  opus_encoder_destroy(enc);
  cw_free(block);
  cw_array_free(&pcm);
  if (!ok) { cw_array_free(&out); return NULL; }
  if (note)
    snprintf(note, note_cap, "Ogg Opus %d kbps, %d ch, %.2f s (from %u Hz)", bitrate / 1000, ch, frames / 48000.0, src_rate);
  *out_len = (size_t)out.count;
  return out.data;
}
