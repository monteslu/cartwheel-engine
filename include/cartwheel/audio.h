/* cartwheel audio: mixer, buses, effects, one-shots, streams, 3D sound.
 *
 * ── Threads ──────────────────────────────────────────────────────────
 * Every function here is called from the GAME thread (the one running the
 * app callbacks). The mixer runs in the platform's audio fill callback
 * (cw_audio_open): natively on SDL's audio thread, in carts on a job-pool
 * thread while the frame renders (commands sent during the render wait for
 * the next frame's mix). The two sides talk through lock-free
 * single-producer/single-consumer queues: calls here enqueue commands
 * (applied at the start of the next mixed block, at most CW_AUDIO_BLOCK
 * frames later), the mixer posts events back (voice finished, memory it no
 * longer touches). The fill callback never locks, allocates, frees or waits.
 *
 * Call cw_audio_update() once per rendered frame: it applies those events,
 * refreshes the stats and keeps streams decoding.
 *
 * ── Sounds and voices ────────────────────────────────────────────────
 * A cw_sound is audio data: either fully decoded PCM (a SAMPLE, for one-shots
 * and short loops) or the encoded file kept in memory and decoded while it
 * plays (a STREAM, for music and ambience). A cw_voice is one playing
 * instance of a sound. Both are generational handles: a voice handle goes
 * stale when the voice ends, and every call on a stale handle is a no-op.
 *
 * Formats: WAV, FLAC, MP3 (miniaudio's decoders), Ogg Vorbis (stb_vorbis),
 * Ogg Opus (libopus), tracker modules MOD/XM/S3M/IT (libxmp-lite; always
 * streamed). Sources may be mono or stereo at any sample rate; more channels
 * are reduced to the first two. Samples are resampled to the mix rate once at
 * load (miniaudio's linear resampler); streams and pitch use per-voice linear
 * interpolation.
 *
 * Stream decoding runs on the engine job pool (cartwheel/jobs.h) into a ring
 * per playing stream; the mixer only reads the ring. When a ring is nearly
 * dry (under 1/8 full) and its job is still out, cw_audio_update waits for it,
 * running it on the game thread if no worker has started it. A ring that
 * still runs dry plays silence for that voice and counts an underrun
 * (cw_audio_stats).
 *
 * ── Buses ────────────────────────────────────────────────────────────
 * Bus 0 is the master (CW_AUDIO_MASTER). Every other bus outputs into a
 * parent created before it and may send into up to CW_AUDIO_MAX_SENDS buses
 * created before it (post-fader), so the bus graph is always a DAG processed
 * in reverse creation order. Each bus has volume, mute, solo and up to
 * CW_AUDIO_MAX_FX effects run in order. Solo: when any bus is soloed, only
 * soloed buses, their ancestors (so the signal reaches the master) and their
 * descendants stay audible.
 *
 * ── 3D ───────────────────────────────────────────────────────────────
 * A listener (position, velocity, forward, up); splitscreen games set one
 * per view (cw_audio_set_listener_at / _count, up to CW_AUDIO_MAX_LISTENERS)
 * and each spatial voice is heard by the nearest. A spatial voice has a
 * position, velocity, direction (for its cone), a distance model, and an
 * occlusion amount the game sets (0 = clear, 1 = fully blocked: a low-pass,
 * e.g. from a physics ray cast between voice and listener). Doppler shifts
 * pitch from the radial velocities. Spatial sources are mixed to mono, then
 * placed by the output mode (cw_audio_set_output):
 *   SPEAKERS (the default): an equal-power pan from the listener-relative
 *     direction (left/right only: speakers cannot place above or behind).
 *   HEADPHONES: head-related filtering (HRTF) for the nearest voices (up to
 *     cw_audio_set_hrtf_voice_limit), so a sound is heard above, behind or
 *     to the side: per ear, the interaural delay and a 64-tap filter blended
 *     from the three nearest measured directions (the built-in table is the
 *     MIT KEMAR dummy head; cw_audio_load_hrtf loads another). A voice that
 *     moves keeps a smooth sound (old and new filters crossfade across each
 *     block); one that enters or leaves the HRTF set crossfades with its
 *     panned version over one block. Cost per voice per 256-frame block on a
 *     Ryzen AI 9 HX 370 core: about 2 us panned, 5 us with the HRTF, 8 us
 *     while it moves (docs/guides/audio.md).
 * A distance low-pass (air_absorption) dulls far sources; it combines with
 * the occlusion low-pass.
 *
 * ── Determinism ──────────────────────────────────────────────────────
 * The mixer uses only + - * / sqrt and cartwheel/math.h's deterministic
 * functions (never libm), so cw_audio_render in offline mode produces
 * bit-identical samples on every host for the same calls. That is what the
 * cross-host audio gate checks. */
#ifndef CW_AUDIO_H
#define CW_AUDIO_H

#include "base.h"
#include "three.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CW_AUDIO_BLOCK 256    /* mixer block (frames); command latency bound */
#define CW_AUDIO_MAX_BUSES 16
#define CW_AUDIO_MAX_FX 4     /* effects per bus */
#define CW_AUDIO_MAX_SENDS 2  /* sends per bus */

CW_HANDLE_TYPE(cw_sound);
CW_HANDLE_TYPE(cw_voice);
typedef int cw_bus; /* bus index: 0 = master; -1 = none/failed */
#define CW_AUDIO_MASTER 0

/* ── system ─────────────────────────────────────────────────────────── */

typedef struct cw_audio_desc {
  int sample_rate;        /* 0 = 48000 (cart rings are 48000; natively the device may differ) */
  int max_voices;         /* simultaneous voices; 0 = 64 */
  int max_streams;        /* simultaneous streaming voices; 0 = 8 */
  int stream_ring_frames; /* per playing stream; 0 = 16384 (~340 ms at 48 kHz) */
  int command_capacity;   /* queued commands between mixed blocks; 0 = 4096 */
  bool offline;           /* no device: the game pulls samples with cw_audio_render */
} cw_audio_desc;

/* Starts the mixer and (unless offline) opens the device through
 * cw_audio_open. Allocates everything the mixer will ever use. Returns false
 * (logged) when already initialized; a missing device is not a failure (the
 * mixer then runs silent, stats.device = false). Registers the debug fields
 * audio.mixed_frames, audio.underruns, audio.voices, audio.dropped_commands. */
bool cw_audio_init(const cw_audio_desc *desc);
/* Stops the device, waits for decode jobs, frees every sound and voice. */
void cw_audio_shutdown(void);
/* Once per rendered frame on the game thread: applies mixer events (finished
 * voices free their handles, released memory is freed), refreshes stats and
 * starts stream decode jobs. Cheap when nothing changed. */
void cw_audio_update(void);
int cw_audio_sample_rate(void); /* mix rate in Hz; 0 before cw_audio_init */

/* Offline mode only: mixes `frames` stereo frames into out (interleaved
 * float). Applies every queued command first and makes every playing stream's
 * ring hold enough data before each block (waiting for its decode job), so
 * the result depends only on the calls made, never on timing. Calls
 * cw_audio_update() itself. */
void cw_audio_render(float *out, int frames);

typedef struct cw_audio_stats {
  uint64_t mixed_frames;     /* frames the mixer has produced since init */
  uint32_t blocks;           /* mixed blocks */
  uint32_t underruns;        /* times a stream ring ran dry (silence inserted) */
  uint32_t underrun_frames;  /* frames of silence those cost */
  uint32_t dropped_commands; /* calls lost because the command queue was full */
  int32_t voices;            /* voices the mixer is playing */
  int32_t streams;           /* of those, streams */
  int32_t hrtf_voices;       /* of those, filtered with the HRTF in the last block (headphones mode) */
  float peak;                /* master peak of the last block, 0..1+ */
  bool device;               /* a real output device is open */
} cw_audio_stats;
/* As of the last cw_audio_update (or render). The profiler reads this. */
void cw_audio_get_stats(cw_audio_stats *out);

/* ── sounds ─────────────────────────────────────────────────────────── */

typedef enum {
  CW_SOUND_AUTO,   /* sample when decoded size <= 2 MB, stream otherwise; modules always stream */
  CW_SOUND_SAMPLE, /* decode fully at load */
  CW_SOUND_STREAM, /* keep encoded; decode while playing */
} cw_sound_mode;

typedef struct cw_sound_desc {
  cw_sound_mode mode;
  int max_voices;    /* simultaneous voices of this sound; 0 = unlimited.
                        Playing past the limit steals this sound's oldest voice */
  int priority;      /* default voice priority (higher wins a full mixer); 0 = 0 */
  float volume;      /* default voice volume; 0 = 1 */
  cw_bus bus;        /* default bus; 0 = master */
} cw_sound_desc;

/* Load from the asset bundle (cw_asset_load). The format is detected from
 * the contents. Logs and returns the null handle when the asset is missing
 * or not decodable. Decoding a SAMPLE happens here, on the calling thread. */
cw_sound cw_sound_load(const char *asset_path, const cw_sound_desc *desc);
/* Same from memory. The bytes are copied (streams keep the copy). */
cw_sound cw_sound_load_memory(const void *data, size_t len, const cw_sound_desc *desc);
/* A sample from PCM (interleaved float, 1 or 2 channels, any rate): the way
 * to play generated sound. The data is copied (and resampled to the mix rate). */
cw_sound cw_sound_from_pcm(const float *pcm, int frames, int channels, int sample_rate, const cw_sound_desc *desc);
/* Stops its voices; the memory goes once the mixer lets go of it. */
void cw_sound_free(cw_sound s);
/* Length in frames at cw_sound_source_rate (-1 when unknown: modules). */
int64_t cw_sound_length_frames(cw_sound s);
/* The rate of the frames a voice of this sound counts (cw_voice_frames_played):
 * the mix rate for samples (resampled at load), the file's rate for streams. */
int cw_sound_source_rate(cw_sound s);
bool cw_sound_is_stream(cw_sound s); /* false for a sample or a stale handle */

/* ── voices ─────────────────────────────────────────────────────────── */

typedef enum {
  CW_ATTEN_NONE,        /* gain 1 */
  CW_ATTEN_INVERSE,     /* ref / (ref + rolloff * (d - ref)) */
  CW_ATTEN_LINEAR,      /* 1 - rolloff * (d - ref) / (max - ref) */
  CW_ATTEN_EXPONENTIAL, /* (d / ref) ^ -rolloff */
} cw_atten_model;         /* d is clamped to [ref, max] first (OpenAL's _CLAMPED models) */

typedef enum {
  CW_SPATIAL_AUTO, /* HRTF in headphones mode (the nearest voices up to the limit), panned on speakers */
  CW_SPATIAL_PAN,  /* always the equal-power pan (a voice whose direction does not matter) */
} cw_spatial_mode;

typedef struct cw_voice_3d {
  t3_vec3 position, velocity;
  t3_vec3 direction;           /* cone axis; zero = omnidirectional */
  cw_atten_model model;        /* 0 = CW_ATTEN_NONE: set one (INVERSE is the usual) */
  float ref_distance;          /* 0 = 1 */
  float max_distance;          /* 0 = 1000 */
  float rolloff;               /* 0 = 1 */
  float cone_inner_deg;        /* full angle at full gain; 0 = 360 (no cone) */
  float cone_outer_deg;        /* full angle where outer_gain is reached; 0 = 360 */
  float cone_outer_gain;       /* gain outside the outer cone */
  float doppler;               /* doppler factor; 0 = none, 1 = physical */
  cw_spatial_mode mode;        /* 0 = AUTO: HRTF in headphones mode (within the voice limit), else panned */
  float air_absorption;        /* distance low-pass: the cutoff (from 20 kHz) halves every 1/air_absorption
                                  units beyond ref_distance (0.02: halves every 50 units); 0 = none */
  float reverb_send;           /* level into the reverb zones (after distance and occlusion);
                                  0 = 1, negative = none */
} cw_voice_3d;

typedef struct cw_play_desc {
  float volume;        /* 0 = the sound's default (use cw_voice_set_volume(v, 0) for silence) */
  float pitch;         /* playback rate multiplier; 0 = 1 */
  float pan;           /* -1 left .. 1 right (2D voices) */
  bool loop;
  int64_t loop_start, loop_end; /* samples only, source frames; loop_end 0 = end */
  float fade_in;       /* seconds */
  cw_bus bus;          /* 0 = the sound's default bus (master unless set) */
  int priority;        /* 0 = the sound's default */
  int64_t start_frame; /* absolute mixer frame to start on (sample-accurate scheduling,
                          see cw_audio_mixed_frames); 0 = as soon as possible */
  int64_t offset;      /* source frame to start from (samples and seekable streams) */
  bool paused;         /* start paused */
  bool spatial;        /* use `space` */
  cw_voice_3d space;
} cw_play_desc;

/* Starts a voice. NULL desc = defaults. Returns the null handle when the
 * sound is stale, or when every voice is busy with a higher priority (or
 * every stream slot is busy). Otherwise a full mixer steals the oldest voice
 * of the lowest priority <= this one. Stolen voices stop at once. */
cw_voice cw_audio_play(cw_sound s, const cw_play_desc *desc);
/* Fade out over `fade` seconds (0 = at once), then end. */
void cw_voice_stop(cw_voice v, float fade);
void cw_voice_set_paused(cw_voice v, bool paused); /* queued, like every voice setter; stale handle = no-op */
void cw_voice_set_volume(cw_voice v, float volume); /* linear gain, not clamped */
void cw_voice_set_pitch(cw_voice v, float pitch); /* rate multiplier; <= 0 means 1 */
void cw_voice_set_pan(cw_voice v, float pan); /* clamped to -1..1; 2D voices */
void cw_voice_set_bus(cw_voice v, cw_bus bus); /* a bus that does not exist plays into the master */
/* Linear fade from the current fade level to `target` (0..1) over seconds. */
void cw_voice_fade(cw_voice v, float target, float seconds);
/* Copies *space and makes the voice spatial; NULL ignored. */
void cw_voice_set_3d(cw_voice v, const cw_voice_3d *space);
/* Per-frame move; keeps the rest of the 3D params. */
void cw_voice_set_position(cw_voice v, t3_vec3 position, t3_vec3 velocity);
void cw_voice_set_occlusion(cw_voice v, float amount); /* 0 clear .. 1 blocked */
/* Still playing (or scheduled / paused), as of the last cw_audio_update. */
bool cw_voice_is_playing(cw_voice v);
/* The music clock: source frames played so far (counting every loop pass),
 * exact to the sample as of the last mixed block. Divide by
 * cw_sound_source_rate for seconds. -1 for a stale handle. */
int64_t cw_voice_frames_played(cw_voice v);
/* Mixer frames produced so far: the clock start_frame is scheduled on. */
int64_t cw_audio_mixed_frames(void);

/* ── listener ───────────────────────────────────────────────────────── */

void cw_audio_set_listener(t3_vec3 position, t3_vec3 velocity, t3_vec3 forward, t3_vec3 up);
#define CW_AUDIO_MAX_LISTENERS 4
/* Listener `index` (0 = cw_audio_set_listener's). With several (splitscreen:
 * one per player's view), each spatial voice is attenuated, panned and
 * doppler-shifted from the listener nearest to it. */
void cw_audio_set_listener_at(int index, t3_vec3 position, t3_vec3 velocity, t3_vec3 forward, t3_vec3 up);
/* How many listeners are live (1..CW_AUDIO_MAX_LISTENERS; default 1). */
void cw_audio_set_listener_count(int n);
/* Speed of sound in world units per second (default 343) for doppler. */
void cw_audio_set_speed_of_sound(float units_per_second);

typedef enum {
  CW_AUDIO_SPEAKERS,   /* spatial voices pan left/right (the default) */
  CW_AUDIO_HEADPHONES, /* spatial voices use the HRTF (see 3D above) */
} cw_audio_output;
/* The player's output: offer it in the audio settings. Switching to
 * HEADPHONES loads the built-in HRTF table first if none is loaded (about
 * 360 KB, a few ms, on this thread). Takes effect at the next block, each
 * voice crossfading over one block. */
void cw_audio_set_output(cw_audio_output output);
cw_audio_output cw_audio_get_output(void); /* the output set last (SPEAKERS until set) */
/* Loads an HRTF table (.cwhrtf, tools/hrtf/import-kemar.mjs) from the asset
 * bundle and uses it from the next block; NULL = the built-in KEMAR table.
 * Returns false (logged, the current table kept) when the asset is missing
 * or not a table. Its rate may differ from the mix rate (resampled here). */
bool cw_audio_load_hrtf(const char *asset_path);
/* The most voices filtered with the HRTF at once (default 16; 0 = none, all
 * pan). The nearest spatial voices (to their listener; ties by voice slot)
 * get it, the rest pan. Scale it with the quality preset if CPU is tight:
 * cw_audio_set_hrtf_voice_limit(cw_quality_count(16, "hrtf_voices")). */
void cw_audio_set_hrtf_voice_limit(int voices);

/* ── buses ──────────────────────────────────────────────────────────── */

/* A new bus feeding `parent` (which must already exist). Returns -1 when all
 * CW_AUDIO_MAX_BUSES are used. Names are for debugging/the editor. */
cw_bus cw_bus_create(const char *name, cw_bus parent);
cw_bus cw_bus_find(const char *name); /* -1 when absent; "master" is 0 */
void cw_bus_set_volume(cw_bus b, float volume); /* linear, ramped over one block */
void cw_bus_set_mute(cw_bus b, bool mute); /* invalid bus ignored */
void cw_bus_set_solo(cw_bus b, bool solo); /* see Solo above; invalid bus ignored */
/* Post-fader send of this bus into `target` (created before it) at `level`;
 * level 0 removes it. slot in [0, CW_AUDIO_MAX_SENDS). */
void cw_bus_set_send(cw_bus b, int slot, cw_bus target, float level);

typedef enum {
  CW_FX_NONE,
  CW_FX_LOWPASS,    /* biquad (miniaudio), RBJ coefficients */
  CW_FX_HIGHPASS,
  CW_FX_DELAY,      /* feedback echo (miniaudio's ma_delay) */
  CW_FX_REVERB,     /* Freeverb: 8 combs + 4 allpasses per channel */
  CW_FX_COMPRESSOR, /* feed-forward peak compressor */
  CW_FX_LIMITER,    /* peak limiter: instant attack, exponential release, hard ceiling */
  CW_FX_FDN_REVERB, /* feedback delay network: 8 lines, Hadamard feedback, set by decay time */
} cw_fx_type;

typedef struct cw_fx_desc {
  cw_fx_type type;
  union {
    struct { float cutoff_hz, q; } filter;                    /* q 0 = 0.7071 */
    struct { float time_s, feedback, wet, dry; } delay;       /* time fixed at add */
    struct { float room_size, damping, wet, dry, width; } reverb; /* 0..1 each */
    struct { float threshold_db, ratio, attack_s, release_s, makeup_db; } compressor;
    struct { float ceiling_db, release_s; } limiter;
    /* decay_s: time to fall 60 dB (RT60); hf_damp_hz: highs above it die faster (0 = no damping);
     * predelay_s (up to 0.5) and size (0..1: line lengths 18..75 ms) are fixed when it is made;
     * wet, dry: output levels */
    struct { float decay_s, hf_damp_hz, predelay_s, size, wet, dry; } fdn;
  };
} cw_fx_desc;

/* Adds an effect to the end of the bus's chain (allocates here, on the game
 * thread). Returns its slot, or -1 when the chain is full. */
int cw_bus_add_fx(cw_bus b, const cw_fx_desc *fx);
/* New parameters for the effect in `slot` (same type; a delay keeps its time). */
void cw_bus_set_fx(cw_bus b, int slot, const cw_fx_desc *fx);
/* Bypassed effects pass audio through; bad bus or slot ignored. */
void cw_bus_set_fx_bypass(cw_bus b, int slot, bool bypass);
/* Removes it; later effects move up one slot. */
void cw_bus_remove_fx(cw_bus b, int slot);

/* ── reverb zones ───────────────────────────────────────────────────
 * Places with their own reverb: a cave, a hall, a corridor. Each zone owns
 * an FDN reverb (CW_FX_FDN_REVERB) fed by every spatial voice's reverb send
 * (cw_voice_3d.reverb_send) and played into a bus. How much a zone hears is
 * set by where listener 0 is: 1 inside the shape, fading to 0 over the blend distance
 * units outside it. Where zones overlap the higher priority takes its share
 * first and the rest is left to the others (a small cave inside a big valley
 * zone: inside the cave you hear only the cave). Shares ramp across each
 * block, and a zone keeps ringing after the listener leaves it (its input
 * fades, not its tail), so walking from a cave into a hall sounds right.
 * A zone whose input is silent and whose tail has died costs nothing.
 *
 * Cost: one FDN per zone that is audible or still ringing, about 11 us per
 * 256-frame block on a Ryzen AI 9 HX 370 core (docs/guides/audio.md). */

#define CW_AUDIO_MAX_REVERB_ZONES 8
typedef enum { CW_ZONE_BOX, CW_ZONE_SPHERE } cw_zone_shape;
typedef int cw_reverb_zone; /* index; -1 = none/failed */

typedef struct cw_reverb_zone_desc {
  cw_zone_shape shape;
  t3_vec3 center;
  t3_vec3 half_extents;  /* box: axis-aligned half sizes */
  float radius;          /* sphere */
  float blend;           /* units outside the shape over which the zone fades out; 0 = a hard edge */
  int priority;          /* higher takes its share first where zones overlap */
  float level;           /* input level; 0 = 1 */
  cw_bus bus;            /* where the reverb plays; 0 = master */
  float decay_s, hf_damp_hz, predelay_s, size; /* as CW_FX_FDN_REVERB; 0 decay = 1.5 s */
} cw_reverb_zone_desc;

/* Adds a zone (allocates its reverb here, on the game thread). Returns -1
 * (logged) when CW_AUDIO_MAX_REVERB_ZONES exist. */
cw_reverb_zone cw_audio_add_reverb_zone(const cw_reverb_zone_desc *desc);
/* New shape, place, blend, priority, level, bus, decay and damping (size and
 * pre-delay stay as made). A stale zone is ignored. */
void cw_audio_set_reverb_zone(cw_reverb_zone zone, const cw_reverb_zone_desc *desc);
/* Removes it at once (its tail stops). */
void cw_audio_remove_reverb_zone(cw_reverb_zone zone);
/* Its share of the reverb sends as of the last mixed block (0..1), for
 * debugging and tests. */
float cw_audio_reverb_zone_weight(cw_reverb_zone zone);

/* ── test helpers ──────────────────────────────────────────────────── */

typedef struct cw_audio_analysis {
  uint32_t hash; /* FNV-1a over the float bytes: equal only if every sample is */
  float peak;    /* max |sample| */
  float rms;     /* over all samples */
} cw_audio_analysis;
/* Count = floats (frames * channels); pure, any thread. */
cw_audio_analysis cw_audio_analyze(const float *samples, size_t count);

/* Decodes a whole asset to interleaved float at its own rate (no mixer
 * needed): what the decoder tests hash. Returns frames (-1 on failure);
 * *pcm is cw_alloc'd (CW_MEM_AUDIO), free with cw_free. */
int64_t cw_audio_decode_asset(const char *asset_path, float **pcm, int *channels, int *sample_rate);

#ifdef __cplusplus
}
#endif
#endif
