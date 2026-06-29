// ============================================================================
// spectral.c -- continuous averaged windowed FFT + cepstrum, double-buffered.
// FFT engine = CMSIS-DSP arm_rfft_fast_f32 (real FFT, M33 DSP extension). Pattern
// unchanged from the RA4M1 model: mean-subtract -> Hann -> |X|^2 accumulate over
// SPEC_AVG frames -> cepstrum = inverse-rfft(log|X|).
//
// Single writer = core1 (spectral_feed). The bench reads spectral_get() = the
// PUBLISHED buffer; core1 fills the OTHER one and swaps the index (no locks).
//
// arm_rfft_fast_f32 packed output (forward): [0]=X[0] (DC, real), [1]=X[N/2]
// (Nyquist, real), then [2k],[2k+1] = re,im of X[k] for k=1..N/2-1.
// ============================================================================
#include "spectral.h"
#include "arm_math.h"
#include <math.h>
#include <string.h>

// Shared FFT scratch -- core1 feeds instances sequentially, so the window LUT,
// the rfft instance, and the work/spec scratch are reused across instances.
static float    g_win[SPEC_N];                 // Hann
static float    g_work[SPEC_N];                // windowed input / rfft in-place scratch
static float    g_spec[SPEC_N];                // packed rfft output (and cepstrum out)
static arm_rfft_fast_instance_f32 g_rfft;
static int      g_shared_ready;

static void shared_init(void) {
    if (g_shared_ready) return;
    for (uint32_t n = 0; n < SPEC_N; n++)
        g_win[n] = 0.5f * (1.0f - cosf(6.28318530717958647692f * (float)n / (float)(SPEC_N - 1u)));
    arm_rfft_fast_init_f32(&g_rfft, SPEC_N);
    g_shared_ready = 1;
}

void spectral_init(spectral_t *s, float fs_hz) {
    shared_init();
    memset(s, 0, sizeof *s);
    s->fs = fs_hz;
    s->out[0].fs = s->out[1].fs = fs_hz;
}

// |X[k]|^2 from the packed rfft output.
static inline float binpow(uint32_t k) {
    if (k == 0u)            return g_spec[0] * g_spec[0];
    if (k == SPEC_N / 2u)   return g_spec[1] * g_spec[1];
    float re = g_spec[2u*k], im = g_spec[2u*k + 1u];
    return re * re + im * im;
}

static void finalize_and_publish(spectral_t *s) {
    int w = 1 - s->pub;
    spec_result_t *o = &s->out[w];
    float inv = 1.0f / (float)s->navg;

    int pk = 1; float pkm = 0.0f;
    o->power[0] = s->psum[0] * inv;
    for (uint32_t k = 1; k < SPEC_BINS; k++) {
        float p = s->psum[k] * inv; o->power[k] = p;
        if (p > pkm) { pkm = p; pk = (int)k; }
    }
    o->peak_bin = pk; o->peak_mag = sqrtf(pkm);

    // cepstrum: inverse rfft of the (packed) log-magnitude spectrum. log|X| is
    // real -> imag slots = 0. Quefrency peak = the signal period (peak magnitude
    // is scale-dependent; we only report the dominant bin).
    g_work[0] = 0.5f * logf(o->power[0] > 1e-6f ? o->power[0] : 1e-6f);
    g_work[1] = 0.5f * logf(o->power[SPEC_N/2] > 1e-6f ? o->power[SPEC_N/2] : 1e-6f);
    for (uint32_t k = 1; k < SPEC_N/2u; k++) {
        g_work[2u*k]      = 0.5f * logf(o->power[k] > 1e-6f ? o->power[k] : 1e-6f);
        g_work[2u*k + 1u] = 0.0f;
    }
    arm_rfft_fast_f32(&g_rfft, g_work, g_spec, 1);   // inverse -> g_spec = real cepstrum
    int qb = 3; float qm = 0.0f;
    for (uint32_t n = 0; n < SPEC_BINS; n++) {
        float c = g_spec[n]; o->ceps[n] = c;
        if (n >= 3 && fabsf(c) > qm) { qm = fabsf(c); qb = (int)n; }
    }
    o->q_bin = qb;

    o->gen = s->out[s->pub].gen + 1u;
    s->pub = w;
    memset(s->psum, 0, sizeof(s->psum)); s->navg = 0;
}

void spectral_feed(spectral_t *s, uint16_t sample) {
    s->frame[s->fidx++] = (float)sample;
    if (s->fidx < SPEC_N) return;
    s->fidx = 0;

    float mean = 0.0f;
    for (uint32_t n = 0; n < SPEC_N; n++) mean += s->frame[n];
    mean *= (1.0f / (float)SPEC_N);
    for (uint32_t n = 0; n < SPEC_N; n++) g_work[n] = (s->frame[n] - mean) * g_win[n];

    arm_rfft_fast_f32(&g_rfft, g_work, g_spec, 0);   // forward -> g_spec packed
    for (uint32_t k = 0; k < SPEC_BINS; k++) s->psum[k] += binpow(k);

    if (++s->navg >= SPEC_AVG) finalize_and_publish(s);
}

const spec_result_t *spectral_get(const spectral_t *s) { return &s->out[s->pub]; }
