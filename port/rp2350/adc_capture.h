// ============================================================================
// adc_capture.h — Pico 2 W fixed ADC front end: 3 channels (GP26/27/28) sampled
// at 20 kS/s each, phase-locked to a 20 kHz PWM (one round-robin scan per PWM
// period), feeding per-channel SPSC rings the DSP drains. See adc_capture.c.
//
// adc_capture_init() must be called ON the core that should own the PWM-wrap ISR
// (the DSP/measurement core), so the ISR and its ADC reads run there.
// ============================================================================
#pragma once
#include <stdint.h>
#include <stdbool.h>

#define ADC_CAP_CH  3u

void     adc_capture_init(void);                 // PWM(20kHz) + ADC(3ch) + wrap ISR
bool     adc_capture_pop(int ch, uint16_t *out); // SPSC pop one 12-bit sample (false=empty)
uint32_t adc_capture_count(int ch);              // total samples captured (rate check)
uint32_t adc_capture_overrun(int ch);            // ring-full drops
