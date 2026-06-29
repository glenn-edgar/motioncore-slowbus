#ifndef DSP_SPECTRAL_H
#define DSP_SPECTRAL_H
#include <stdint.h>

// Continuous averaged windowed FFT + cepstrum on one sample stream, DOUBLE-
// BUFFERED: core1 (the DSP, single writer) fills the working buffer while the
// bench/console reads the published one. Multiple INSTANCES are supported (e.g. a
// coarse FFT on the 44 kHz stream + a fine FFT on the 1 kHz stream) -- each owns
// its accumulators; the FFT scratch/window/rfft are shared (core1 feeds them
// sequentially, so no contention).
//
//   spectral_init(&s, fs_hz)             -- one-time per instance
//   spectral_feed(&s, sample)            -- per sample (core1); runs the FFT/publish
//   const spec_result_t *spectral_get(&s)-- the published (bench-readable) result
//
// Cepstrum: real cepstrum c[q] = IDFT(log|X(f)|) -- dominant quefrency q gives
// the signal period; fundamental ~ fs/q.

#ifndef SPEC_N
#define SPEC_N     1024u           // FFT size (power of two). slow_bus Pico 2 W = 1024
#endif                             // (19.5 Hz/bin @ 20 kHz); -DSPEC_N=... to override.
#define SPEC_BINS  (SPEC_N/2u + 1u)
#ifndef SPEC_AVG
#define SPEC_AVG   4u              // Welch frames averaged per published result
#endif

typedef struct {
    uint32_t gen;                  // bumped on each publish (reader change-detect)
    float    fs;                   // sample rate (Hz) for bin->Hz
    float    power[SPEC_BINS];     // averaged |X[k]|^2
    float    ceps[SPEC_BINS];      // real cepstrum (quefrency bins)
    int      peak_bin;             // dominant spectral bin (>0)
    float    peak_mag;             // sqrt(power[peak_bin])
    int      q_bin;                // dominant quefrency bin (>2)
} spec_result_t;

typedef struct {                   // one analysis stream (per-instance state)
    float        frame[SPEC_N]; uint32_t fidx;
    float        psum[SPEC_BINS]; uint32_t navg;
    float        fs;
    spec_result_t out[2];
    volatile int pub;
} spectral_t;

void spectral_init(spectral_t *s, float fs_hz);
void spectral_feed(spectral_t *s, uint16_t sample);
const spec_result_t *spectral_get(const spectral_t *s);

#endif // DSP_SPECTRAL_H
