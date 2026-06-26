#ifndef DSP_PIPELINE_H
#define DSP_PIPELINE_H
#include <stdint.h>
#include "spectral.h"

// Multi-rate signal-processing pipeline. One top-rate sample stream feeds a
// linear-phase FIR DECIMATION CASCADE producing band-limited streams at
// progressively lower rates; each band carries running rms/avg/min/max published
// as a band_stat_t. slow_bus Pico 2 W feeds it the 20 kHz center-captured ADC
// stream; the interlock keys off a slow band, the FFT taps the top band.
//
// INSTANCE-BASED: one pipeline_t per ADC channel (3 channels each get the full
// decimation + coarse/fine FFT + cepstrum + band stats). Single writer (core1
// feeds all instances sequentially); readers (core0) read the published results.
//
//   band 0 = 20 kHz   (full band, Nyquist 10 kHz) -> FFT/cepstrum tap
//   band 1 = 1 kHz    (/20)   Nyquist 500 Hz       -> fine FFT tap
//   band 2 = 100 Hz   (/10)   Nyquist  50 Hz
//   band 3 = 10 Hz    (/10)   Nyquist   5 Hz       -> interlock tap

#define PIPE_BANDS 4

typedef struct {
    uint32_t gen;          // bumped on each window publish
    float    rate;         // band sample rate (Hz)
    uint16_t avg, rms;     // window mean + AC-rms (stddev), ADC counts
    uint16_t mn, mx;
} band_stat_t;

typedef struct pipeline pipeline_t;             // opaque; allocated from a fixed pool

pipeline_t *pipeline_create(float top_fs_hz);   // one per channel; NULL if pool exhausted
void        pipeline_feed(pipeline_t *p, uint16_t sample);     // one top-rate sample (core1)
const band_stat_t   *pipeline_band(pipeline_t *p, int n);      // 0..PIPE_BANDS-1
const spec_result_t *pipeline_spectral(pipeline_t *p, int which); // 0=coarse(20k) 1=fine(1k)

// ---- measurement tap: (stream, metric) -> value ----------------------------
// The uniform measurement surface the interlock engine + the future I2C register
// bank read.
enum { TAP_DC = 0, TAP_RMS, TAP_MIN, TAP_MAX, TAP_NMETRIC };
uint16_t    pipeline_tap(pipeline_t *p, int band, int metric);
const char *pipeline_metric_name(int metric);     // "dc"/"rms"/"min"/"max"
int         pipeline_metric_parse(const char *s); // name -> TAP_*, or -1

#endif // DSP_PIPELINE_H
