// ============================================================================
// pipeline.c -- 44 kHz -> 10 Hz LINEAR-PHASE FIR decimation cascade (CMSIS-DSP).
// Replaces the boxcar/CIC-1 with arm_fir_decimate_f32: symmetric (linear-phase)
// windowed-sinc anti-alias FIRs -> clean band separation. Multistage so each
// filter stays short:  44k --/4--> 11k --/11--> 1k --/10--> 100 --/10--> 10 Hz.
// Published bands: 0=44k 1=1k 2=100 3=10 (11k is an internal intermediate).
//
// Block-based: band 0 (FFT + stats) runs per-sample (spread); the FIR cascade
// runs once per BLK (100 ms) -- a few arm_fir_decimate calls. Single writer
// (core1); readers (core0) read the published band_stat_t.
// ============================================================================
#include "pipeline.h"
#include "spectral.h"
#include "arm_math.h"
#include "fir_coeffs.h"
#include <math.h>
#include <string.h>

#define BLK      4400u            // 100 ms at 44 kHz; cascades cleanly /4 /11 /10 /10
#define N_11K    (BLK / 4u)       // 1100
#define N_1K     (N_11K / 11u)    // 100
#define N_100    (N_1K / 10u)     // 10
#define N_10     (N_100 / 10u)    // 1

// stats window per band (samples at that band's rate) -> ~100 ms (band 3 = 1 s)
static const uint32_t WIN[PIPE_BANDS] = { 4400, 100, 10, 10 };

// --- FIR decimators + their state buffers (size = numTaps + blockSize - 1) ---
static arm_fir_decimate_instance_f32 D1a, D1b, D2, D3;
static float st_D1a[FIR_D1A_N + BLK    - 1];
static float st_D1b[FIR_D1B_N + N_11K  - 1];
static float st_D2 [FIR_D2_N  + N_1K   - 1];
static float st_D3 [FIR_D3_N  + N_100  - 1];

// --- scratch (the cascade's per-stage output) ---
static uint16_t g_blk[BLK]; static uint32_t g_bn;
static float    f_in[BLK], f_11k[N_11K], f_1k[N_1K], f_100[N_100], f_10[N_10];

// --- per-band stat accumulators + published results ---
typedef struct { uint32_t sn; double ssum, ssq; uint16_t smn, smx; } acc_t;
static acc_t       g_acc[PIPE_BANDS];
static band_stat_t g_pub[PIPE_BANDS];

// --- two FFT instances: coarse on band 0 (44 kHz, 172 Hz bins, 0-22 kHz) and
//     fine on band 1 (1 kHz, 3.9 Hz bins, 0-500 Hz -- low-freq vibration). ---
static spectral_t  g_sp_hi, g_sp_lo;

void pipeline_init(float top_fs_hz) {
    float r = top_fs_hz;
    static const float RATE_DIV[PIPE_BANDS] = { 1.0f, 44.0f, 10.0f, 10.0f };
    for (int i = 0; i < PIPE_BANDS; i++) {
        if (i) r /= RATE_DIV[i];
        g_acc[i].sn = 0; g_acc[i].ssum = g_acc[i].ssq = 0.0; g_acc[i].smn = 4095; g_acc[i].smx = 0;
        g_pub[i].rate = r; g_pub[i].gen = 0;
        g_pub[i].avg = g_pub[i].rms = g_pub[i].mn = g_pub[i].mx = 0;
    }
    arm_fir_decimate_init_f32(&D1a, FIR_D1A_N, FIR_D1A_M, FIR_D1A_COEF, st_D1a, BLK);
    arm_fir_decimate_init_f32(&D1b, FIR_D1B_N, FIR_D1B_M, FIR_D1B_COEF, st_D1b, N_11K);
    arm_fir_decimate_init_f32(&D2,  FIR_D2_N,  FIR_D2_M,  FIR_D2_COEF,  st_D2,  N_1K);
    arm_fir_decimate_init_f32(&D3,  FIR_D3_N,  FIR_D3_M,  FIR_D3_COEF,  st_D3,  N_100);
    g_bn = 0;
    spectral_init(&g_sp_hi, top_fs_hz);              // coarse FFT on band 0 (44 kHz)
    spectral_init(&g_sp_lo, top_fs_hz / 44.0f);      // fine FFT on band 1 (1 kHz)
}

const spec_result_t *pipeline_spectral(int which) {  // 0 = hi/band0, 1 = lo/band1
    return spectral_get(which ? &g_sp_lo : &g_sp_hi);
}

// running stats per band; finalize + publish when the window fills.
static void band_acc(int i, float x) {
    acc_t *p = &g_acc[i];
    uint16_t v = (x < 0) ? 0 : (x > 4095 ? 4095 : (uint16_t)(x + 0.5f));
    if (p->sn == 0) { p->smn = v; p->smx = v; }
    else { if (v < p->smn) p->smn = v; if (v > p->smx) p->smx = v; }
    p->ssum += x; p->ssq += (double)x * x; p->sn++;
    if (p->sn >= WIN[i]) {
        double mean = p->ssum / (double)p->sn, var = p->ssq / (double)p->sn - mean * mean;
        if (var < 0) var = 0;
        band_stat_t *o = &g_pub[i];
        o->avg = (uint16_t)(mean + 0.5); o->rms = (uint16_t)(sqrt(var) + 0.5);
        o->mn = p->smn; o->mx = p->smx; o->gen++;
        p->sn = 0; p->ssum = p->ssq = 0.0; p->smn = 4095; p->smx = 0;
    }
}

// the FIR cascade: one 100 ms block of 44 kHz input -> bands 1/2/3.
static void run_cascade(void) {
    for (uint32_t j = 0; j < BLK; j++) f_in[j] = (float)g_blk[j];
    arm_fir_decimate_f32(&D1a, f_in,  f_11k, BLK);    // 44k -> 11k
    arm_fir_decimate_f32(&D1b, f_11k, f_1k,  N_11K);  // 11k -> 1k  (band 1)
    for (uint32_t j = 0; j < N_1K;  j++) {
        band_acc(1, f_1k[j]);
        float v = f_1k[j]; uint16_t u = (v < 0) ? 0 : (v > 65535.0f ? 65535 : (uint16_t)(v + 0.5f));
        spectral_feed(&g_sp_lo, u);                   // fine FFT on the 1 kHz stream
    }
    arm_fir_decimate_f32(&D2,  f_1k,  f_100, N_1K);   // 1k -> 100  (band 2)
    for (uint32_t j = 0; j < N_100; j++) band_acc(2, f_100[j]);
    arm_fir_decimate_f32(&D3,  f_100, f_10,  N_100);  // 100 -> 10  (band 3)
    for (uint32_t j = 0; j < N_10;  j++) band_acc(3, f_10[j]);
}

void pipeline_feed(uint16_t sample) {
    spectral_feed(&g_sp_hi, sample);    // band 0 -> coarse FFT/cepstrum (per-sample)
    band_acc(0, (float)sample);         // band 0 stats
    g_blk[g_bn++] = sample;
    if (g_bn >= BLK) { run_cascade(); g_bn = 0; }   // bands 1/2/3 once per 100 ms
}

const band_stat_t *pipeline_band(int n) {
    if (n < 0 || n >= PIPE_BANDS) n = 0;
    return &g_pub[n];
}

// ---- measurement tap -------------------------------------------------------
static const char *const METRIC_NAME[TAP_NMETRIC] = { "dc", "rms", "min", "max" };

uint16_t pipeline_tap(int band, int metric) {
    const band_stat_t *s = pipeline_band(band);
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
