#ifndef DSP_PIPELINE_H
#define DSP_PIPELINE_H
#include <stdint.h>
#include "spectral.h"

// Multi-rate signal-processing pipeline (the SAMD21-ADC model, scaled to 44 kHz).
// One top-rate sample stream feeds a boxcar/CIC-1 DECIMATION CASCADE producing
// band-limited streams at progressively lower rates; each band carries running
// rms/avg/min/max published as a band_stat_t. The SPU feeds it the 44 kHz ADC
// stream; the interlock keys off a slow band (= SAMD21 .rms.hz10), the FFT taps
// the top band. Single writer (core1); readers (core0 bench/supervisor) read the
// published band_stat_t (gen bumps per update).
//
//   band 0 = 44 kHz   (full band, Nyquist 22 kHz) -> FFT/cepstrum tap
//   band 1 = 1 kHz    (/44)   Nyquist 500 Hz
//   band 2 = 100 Hz   (/10)   Nyquist  50 Hz
//   band 3 = 10 Hz    (/10)   Nyquist   5 Hz       -> interlock tap

#define PIPE_BANDS 4

typedef struct {
    uint32_t gen;          // bumped on each window publish
    float    rate;         // band sample rate (Hz)
    uint16_t avg, rms;     // window mean + AC-rms (stddev), ADC counts
    uint16_t mn, mx;
} band_stat_t;

void pipeline_init(float top_fs_hz);     // top_fs_hz = band-0 rate (e.g. 44000)
void pipeline_feed(uint16_t sample);     // one top-rate sample (core1)
const band_stat_t *pipeline_band(int n); // 0..PIPE_BANDS-1, published stats

// FFT/cepstrum streams: 0 = coarse (band 0, 44 kHz, 172 Hz bins),
//                       1 = fine   (band 1,  1 kHz, 3.9 Hz bins, low-freq detail).
const spec_result_t *pipeline_spectral(int which);

// ---- measurement tap: (stream, metric) -> value ----------------------------
// The uniform measurement surface the bench, the interlock supervisor (the
// chain_tree seat), and the future I2C register bank all read.
enum { TAP_DC = 0, TAP_RMS, TAP_MIN, TAP_MAX, TAP_NMETRIC };
uint16_t    pipeline_tap(int band, int metric);   // band 0..PIPE_BANDS-1, metric TAP_*
const char *pipeline_metric_name(int metric);     // "dc"/"rms"/"min"/"max"
int         pipeline_metric_parse(const char *s); // name -> TAP_*, or -1

#endif // DSP_PIPELINE_H
