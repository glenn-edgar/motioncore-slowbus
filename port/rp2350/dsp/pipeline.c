// ============================================================================
// pipeline.c -- 20 kHz -> 10 Hz LINEAR-PHASE FIR decimation cascade (CMSIS-DSP).
// arm_fir_decimate_f32: symmetric (linear-phase) windowed-sinc anti-alias FIRs
// -> clean band separation. Multistage so each filter stays short (slow_bus Pico
// 2 W, re-cut from the vendor's 44 kHz):
//   20k --/4--> 5k --/5--> 1k --/10--> 100 --/10--> 10 Hz.
// Published bands: 0=20k 1=1k 2=100 3=10 (5k is an internal intermediate).
//
// INSTANCE-BASED: one pipeline per ADC channel (allocated from a fixed pool). All
// per-stream state (FIR delay lines, block buffer, band stats, the two spectral
// instances) lives in struct pipeline. The cascade SCRATCH is shared static: core1
// feeds the instances sequentially, so each run_cascade() uses+releases it within
// one call (same discipline spectral.c uses for its FFT scratch).
//
// Block-based: band 0 (FFT + stats) runs per-sample; the FIR cascade runs once per
// BLK (100 ms). Single writer (core1); readers (core0) read the published stats.
// ============================================================================
#include "pipeline.h"
#include "spectral.h"
#include "arm_math.h"
#include "fir_coeffs.h"
#include <math.h>
#include <string.h>

#define BLK      2000u            // 100 ms at 20 kHz; cascades cleanly /4 /5 /10 /10
#define N_5K     (BLK / 4u)       // 500
#define N_1K     (N_5K / 5u)      // 100
#define N_100    (N_1K / 10u)     // 10
#define N_10     (N_100 / 10u)    // 1
#define PIPE_MAX 3u               // pool size: one per ADC channel

// stats window per band (samples at that band's rate) -> ~100 ms (band 3 = 1 s)
static const uint32_t WIN[PIPE_BANDS] = { 2000, 100, 10, 10 };

// Welford online mean/variance in FLOAT -- the M33 FPU is single-precision, so the
// old double ssum/ssq cost ~100s of soft-float cycles per sample (60k/s across 3
// channels -> overruns). Welford is numerically stable in float for this range.
typedef struct { uint32_t sn; float mean, M2; uint16_t smn, smx; } acc_t;

struct pipeline {
    float fs;
    // FIR decimators + their state buffers (size = numTaps + blockSize - 1)
    arm_fir_decimate_instance_f32 D1a, D1b, D2, D3;
    float st_D1a[FIR_D1A_N + BLK   - 1];
    float st_D1b[FIR_D1B_N + N_5K  - 1];
    float st_D2 [FIR_D2_N  + N_1K  - 1];
    float st_D3 [FIR_D3_N  + N_100 - 1];
    // band-0 block accumulator
    uint16_t blk[BLK]; uint32_t bn;
    // per-band stat accumulators + published results
    acc_t       acc[PIPE_BANDS];
    band_stat_t pub[PIPE_BANDS];
    // coarse FFT on band 0 (20 kHz), fine FFT on band 1 (1 kHz)
    spectral_t  sp_hi, sp_lo;
};

static struct pipeline g_pool[PIPE_MAX];
static uint32_t        g_npool;

// shared cascade scratch (sequential use across instances on core1)
static float f_in[BLK], f_5k[N_5K], f_1k[N_1K], f_100[N_100], f_10[N_10];

pipeline_t *pipeline_create(float top_fs_hz) {
    if (g_npool >= PIPE_MAX) return 0;
    struct pipeline *p = &g_pool[g_npool++];
    memset(p, 0, sizeof *p);
    p->fs = top_fs_hz;

    float r = top_fs_hz;
    static const float RATE_DIV[PIPE_BANDS] = { 1.0f, 20.0f, 10.0f, 10.0f };
    for (int i = 0; i < PIPE_BANDS; i++) {
        if (i) r /= RATE_DIV[i];
        p->acc[i].smn = 4095; p->acc[i].smx = 0;
        p->pub[i].rate = r;
    }
    arm_fir_decimate_init_f32(&p->D1a, FIR_D1A_N, FIR_D1A_M, FIR_D1A_COEF, p->st_D1a, BLK);
    arm_fir_decimate_init_f32(&p->D1b, FIR_D1B_N, FIR_D1B_M, FIR_D1B_COEF, p->st_D1b, N_5K);
    arm_fir_decimate_init_f32(&p->D2,  FIR_D2_N,  FIR_D2_M,  FIR_D2_COEF,  p->st_D2,  N_1K);
    arm_fir_decimate_init_f32(&p->D3,  FIR_D3_N,  FIR_D3_M,  FIR_D3_COEF,  p->st_D3,  N_100);
    spectral_init(&p->sp_hi, top_fs_hz);          // coarse FFT on band 0 (20 kHz)
    spectral_init(&p->sp_lo, top_fs_hz / 20.0f);  // fine FFT on band 1 (1 kHz)
    return p;
}

const spec_result_t *pipeline_spectral(pipeline_t *p, int which) {
    return spectral_get(which ? &p->sp_lo : &p->sp_hi);
}

// running stats per band; finalize + publish when the window fills.
static void band_acc(struct pipeline *p, int i, float x) {
    acc_t *a = &p->acc[i];
    uint16_t v = (x < 0) ? 0 : (x > 4095 ? 4095 : (uint16_t)(x + 0.5f));
    if (a->sn == 0) { a->smn = v; a->smx = v; }
    else { if (v < a->smn) a->smn = v; if (v > a->smx) a->smx = v; }
    a->sn++;
    float d = x - a->mean; a->mean += d / (float)a->sn; a->M2 += d * (x - a->mean);
    if (a->sn >= WIN[i]) {
        float var = a->M2 / (float)a->sn; if (var < 0) var = 0;
        band_stat_t *o = &p->pub[i];
        o->avg = (uint16_t)(a->mean + 0.5f); o->rms = (uint16_t)(sqrtf(var) + 0.5f);
        o->mn = a->smn; o->mx = a->smx; o->gen++;
        a->sn = 0; a->mean = 0.0f; a->M2 = 0.0f; a->smn = 4095; a->smx = 0;
    }
}

// the FIR cascade: one 100 ms block of 20 kHz input -> bands 1/2/3.
static void run_cascade(struct pipeline *p) {
    for (uint32_t j = 0; j < BLK; j++) f_in[j] = (float)p->blk[j];
    arm_fir_decimate_f32(&p->D1a, f_in,  f_5k, BLK);     // 20k -> 5k
    arm_fir_decimate_f32(&p->D1b, f_5k,  f_1k, N_5K);    // 5k  -> 1k  (band 1)
    for (uint32_t j = 0; j < N_1K;  j++) {
        band_acc(p, 1, f_1k[j]);
        float v = f_1k[j]; uint16_t u = (v < 0) ? 0 : (v > 65535.0f ? 65535 : (uint16_t)(v + 0.5f));
        spectral_feed(&p->sp_lo, u);                     // fine FFT on the 1 kHz stream
    }
    arm_fir_decimate_f32(&p->D2,  f_1k,  f_100, N_1K);   // 1k -> 100  (band 2)
    for (uint32_t j = 0; j < N_100; j++) band_acc(p, 2, f_100[j]);
    arm_fir_decimate_f32(&p->D3,  f_100, f_10,  N_100);  // 100 -> 10  (band 3)
    for (uint32_t j = 0; j < N_10;  j++) band_acc(p, 3, f_10[j]);
}

void pipeline_feed(pipeline_t *p, uint16_t sample) {
    spectral_feed(&p->sp_hi, sample);   // band 0 -> coarse FFT/cepstrum (per-sample)
    band_acc(p, 0, (float)sample);      // band 0 stats
    p->blk[p->bn++] = sample;
    if (p->bn >= BLK) { run_cascade(p); p->bn = 0; }   // bands 1/2/3 once per 100 ms
}

const band_stat_t *pipeline_band(pipeline_t *p, int n) {
    if (n < 0 || n >= PIPE_BANDS) n = 0;
    return &p->pub[n];
}

// ---- measurement tap -------------------------------------------------------
static const char *const METRIC_NAME[TAP_NMETRIC] = { "dc", "rms", "min", "max" };

uint16_t pipeline_tap(pipeline_t *p, int band, int metric) {
    const band_stat_t *s = pipeline_band(p, band);
    switch (metric) {
        case TAP_DC:  return s->avg;   // band mean = DC level
        case TAP_RMS: return s->rms;   // AC-rms
        case TAP_MIN: return s->mn;
        case TAP_MAX: return s->mx;
        default:      return 0;
    }
}
const char *pipeline_metric_name(int metric) {
    return (metric >= 0 && metric < TAP_NMETRIC) ? METRIC_NAME[metric] : "?";
}
int pipeline_metric_parse(const char *s) {
    for (int m = 0; m < TAP_NMETRIC; m++)
        if (strcmp(s, METRIC_NAME[m]) == 0) return m;
    return -1;
}
