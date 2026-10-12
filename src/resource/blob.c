/* Byte resources (fonts, JSON, text, anything else) and audio clips (the
 * encoded file plus what miniaudio, or the Ogg headers, say about it). The
 * file bytes are kept, not copied. */
#include <stdio.h>
#include <stdlib.h>

#include "json.h"
#include "audio/internal.h"
#include "cartwheel/audio.h"
#include "resource_internal.h"

static void *blob_decode(const cw_res_decode_args *a, size_t *cost) {
  cw_blob *b = cw_alloc(sizeof *b, CW_MEM_RESOURCE);
  b->data = a->bytes;
  b->len = a->len;
  *a->take_bytes = true;
  *cost = a->len;
  return b;
}

static void *json_decode(const cw_res_decode_args *a, size_t *cost) {
  char err[160];
  cwj *v = cwj_parse((const char *)a->bytes, a->len, err, sizeof err);
  if (!v) { cw_error("res://%s: invalid JSON, %s", a->path, err); return NULL; }
  cwj_free(v);
  return blob_decode(a, cost);
}

static void *blob_finish(void *decoded, cw_res self, const cw_res *deps, int n, void *u) { return decoded; }
static void blob_unload(void *p, void *u) {
  cw_blob *b = p;
  cw_free((void *)b->data);
  cw_free(b);
}
static void blob_reload(void *p, void *fresh, void *u) {
  cw_blob *b = p, *f = fresh, t = *b;
  *b = *f;
  *f = t;
  blob_unload(f, u);
}
static size_t blob_size(const void *p, void *u) { return ((const cw_blob *)p)->len; }

#define BLOB_TYPE(nm, ext, dec) \
  { .name = nm, .extensions = ext, .decode = dec, .finish = blob_finish, .unload = blob_unload, \
    .discard = blob_unload, .reload = blob_reload, .size = blob_size }
const cw_res_type_desc cw__res_font_type = BLOB_TYPE("font", ".ttf .otf .ttc", blob_decode);
const cw_res_type_desc cw__res_json_type = BLOB_TYPE("json", ".json", json_decode);
const cw_res_type_desc cw__res_text_type = BLOB_TYPE("text", ".txt .csv .md .cfg .ini .glsl .vert .frag .cwproj .cwscene", blob_decode);
const cw_res_type_desc cw__res_binary_type = BLOB_TYPE("binary", ".bin", blob_decode);

const cw_blob *cw_res_blob(cw_res h) {
  cw_res_type t = cw_res_type_of(h);
  if (t != CW_RES_FONT && t != CW_RES_JSON && t != CW_RES_TEXT && t != CW_RES_BINARY) return NULL;
  return cw_res_get(h);
}

/* ── audio clips: the audio module's own decoder says what the file is
 *    (src/audio/decode.c); cw_res_audio_sound hands the bytes to its loader */
typedef struct clip_payload {
  cw_audio_clip pub; /* first: cw_res_audio returns it */
  cw_sound sound;    /* made on first cw_res_audio_sound */
} clip_payload;

static void *audio_decode(const cw_res_decode_args *a, size_t *cost) {
  cw__decoder dec;
  if (!cw__dec_open(&dec, a->bytes, a->len)) {
    cw_error("res://%s: not audio the audio module decodes (WAV, FLAC, MP3, Ogg Vorbis, Ogg Opus, tracker modules)", a->path);
    return NULL;
  }
  clip_payload *p = cw_calloc(1, sizeof *p, CW_MEM_RESOURCE);
  cw_audio_clip *c = &p->pub;
  c->data = a->bytes;
  c->len = a->len;
  c->channels = (uint32_t)dec.channels;
  c->sample_rate = (uint32_t)dec.rate;
  c->frames = dec.length > 0 ? (uint64_t)dec.length : 0;
  switch (dec.kind) {
  case CW__DEC_VORBIS: c->container = CW_AUDIO_OGG_VORBIS; break;
  case CW__DEC_OPUS: c->container = CW_AUDIO_OGG_OPUS; break;
  case CW__DEC_XMP: c->container = CW_AUDIO_MODULE; break;
  default:
    c->container = !memcmp(a->bytes, "RIFF", 4) ? CW_AUDIO_WAV : !memcmp(a->bytes, "fLaC", 4) ? CW_AUDIO_FLAC : CW_AUDIO_MP3;
  }
  cw__dec_close(&dec);
  c->seconds = c->sample_rate ? (double)c->frames / c->sample_rate : 0;
  *a->take_bytes = true;
  *cost = a->len;
  return p;
}

static void *audio_finish(void *decoded, cw_res self, const cw_res *deps, int n, void *u) { return decoded; }
static void audio_unload(void *pl, void *u) {
  clip_payload *p = pl;
  if (!CW_HANDLE_IS_NULL(p->sound)) cw_sound_free(p->sound);
  cw_free((void *)p->pub.data);
  cw_free(p);
}
static void audio_reload(void *pl, void *fresh, void *u) {
  clip_payload *p = pl, *f = fresh, t = *p;
  *p = *f;
  *f = t;
  audio_unload(f, u); /* frees the old sound: cw_res_audio_sound makes a new one */
}
static size_t audio_size(const void *p, void *u) { return ((const cw_audio_clip *)p)->len; }

const cw_res_type_desc cw__res_audio_type = {
  .name = "audio_clip", .extensions = ".wav .ogg .opus .mp3 .flac .xm .mod .s3m .it", .decode = audio_decode,
  .finish = audio_finish, .unload = audio_unload, .discard = audio_unload, .reload = audio_reload, .size = audio_size,
};

const cw_audio_clip *cw_res_audio(cw_res h) { return cw__res_get_typed(h, CW_RES_AUDIO_CLIP); }

cw_sound cw_res_audio_sound(cw_res h, const cw_sound_desc *desc) {
  clip_payload *p = cw__res_get_typed(h, CW_RES_AUDIO_CLIP);
  if (!p) return (cw_sound){ 0 };
  if (CW_HANDLE_IS_NULL(p->sound)) p->sound = cw_sound_load_memory(p->pub.data, p->pub.len, desc);
  return p->sound;
}
