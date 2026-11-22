/* Tempo with the pitch kept: a C face on Signalsmith Stretch (deps/signalsmith-stretch, MIT), the phase vocoder
 * the mixer runs for a voice whose tempo is not 1 (mixer.c). Every allocation happens in cw__stretch_new, on the
 * game thread; reset, seek and process only touch memory made there, so the mixer may call them.
 *
 * One configuration for every source: two channels (a mono source is fed to both), 5760-frame spectra every 1440
 * frames (Signalsmith's default preset at 48 kHz) with the computation split across the output (one more interval of
 * output latency, and no spike on the block that computes a spectrum). The latencies are therefore the same for every
 * voice (internal.h). */
#ifndef _USE_MATH_DEFINES
#define _USE_MATH_DEFINES
#endif
#include <cstdint>
#include <new>

#include "signalsmith-stretch.h"

#include "stretch.h"

namespace {

/* The stretcher draws random phases only when a spectrum is stretched past 2x (rounding at tempo 0.5 can get there).
 * Its engine reads the state of the stretcher being run (set before every call; one mixer thread runs them all), and
 * a reset puts that state back, so a voice's output never depends on what its stretcher played before. */
uint32_t *g_rng_state;

struct Rng {
  using result_type = uint32_t;
  static constexpr result_type min() { return 1; }
  static constexpr result_type max() { return 0xffffffffu; }
  Rng() = default;
  explicit Rng(long) {}
  result_type operator()() { /* xorshift32 */
    uint32_t s = *g_rng_state;
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    return *g_rng_state = s;
  }
};

using Stretch = signalsmith::stretch::SignalsmithStretch<float, Rng>;

const int kBlock = 5760, kInterval = 1440;

} // namespace

struct cw__stretch {
  Stretch st{ 1L };
  uint32_t rng = 1;
  float *in[2] = { nullptr, nullptr }, *out[2] = { nullptr, nullptr };
};

extern "C" cw__stretch *cw__stretch_new(void) {
  cw__stretch *s = new (std::nothrow) cw__stretch;
  if (!s) return nullptr;
  s->st.configure(2, kBlock, kInterval, true);
  for (int c = 0; c < 2; c++) {
    s->in[c] = new (std::nothrow) float[CW__STRETCH_IN_MAX];
    s->out[c] = new (std::nothrow) float[CW__STRETCH_CHUNK];
    if (!s->in[c] || !s->out[c]) {
      cw__stretch_free(s);
      return nullptr;
    }
  }
  cw__stretch_reset(s);
  return s;
}

extern "C" void cw__stretch_free(cw__stretch *s) {
  if (!s) return;
  for (int c = 0; c < 2; c++) {
    delete[] s->in[c];
    delete[] s->out[c];
  }
  delete s;
}

extern "C" float *cw__stretch_in(cw__stretch *s, int ch) { return s->in[ch]; }
extern "C" float *cw__stretch_out(cw__stretch *s, int ch) { return s->out[ch]; }

extern "C" void cw__stretch_reset(cw__stretch *s) {
  s->rng = 1;
  s->st.reset();
}

extern "C" void cw__stretch_seek(cw__stretch *s, int n) {
  const float *in[2] = { s->in[0], s->in[1] };
  g_rng_state = &s->rng;
  s->st.seek(in, n, 1.0);
}

extern "C" void cw__stretch_process(cw__stretch *s, int nin, int nout) {
  const float *in[2] = { s->in[0], s->in[1] };
  float *out[2] = { s->out[0], s->out[1] };
  g_rng_state = &s->rng;
  s->st.process(in, nin, out, nout);
}

/* the fixed configuration's latencies (internal.h names them; checked once against the library here) */
static_assert(CW__STRETCH_IN_LAT == kBlock / 2, "input latency");
static_assert(CW__STRETCH_OUT_LAT == kBlock / 2 + kInterval, "output latency (split computation)");
static_assert(CW__STRETCH_SEEK == kBlock + kInterval, "seek length");
