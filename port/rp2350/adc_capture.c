// ============================================================================
// adc_capture.c — Pico 2 W (RP2350) fixed ADC front end.
//
// A 20 kHz center-aligned (phase-correct) PWM on PWM_20K_PIN provides the sample
// timebase; its wrap interrupt fires once per 50 us period and triggers a
// round-robin scan of the 3 ADC channels (GP26/27/28). Each sample is pushed to a
// per-channel SPSC ring; the DSP/measurement task (same core) drains them via
// adc_capture_pop(). Net: 20 kS/s per channel, phase-locked to the PWM.
//
// PHASE NOTE: the wrap IRQ fires at the PWM counter wrap; with floating bench
// inputs (no motor/load yet) the exact center-of-cycle alignment isn't yet
// observable, so the trigger phase is left at the wrap. When a real PWM load is
// present, nudge the sample point to the cycle center (e.g. an offset / counter-
// compare) — the capture rate + plumbing here don't change.
//
// adc_read() in the ISR: 3 conversions * ~2 us = ~6 us per 50 us period (~12% of
// this core), comfortably inside the window. The heavy DSP runs in the task, not
// the ISR.
// ============================================================================
#include "adc_capture.h"
#include "board.h"            // ADC_PIN_CH0/1/2, PWM_20K_PIN, PWM_20K_HZ
#include "pico/stdlib.h"
#include "hardware/adc.h"
#include "hardware/pwm.h"
#include "hardware/irq.h"
#include "hardware/clocks.h"

#define RING       512u           // power of two; ~25 ms backlog at 20 kHz (absorbs
#define RING_MASK  (RING - 1u)     // transient drain stalls, e.g. a debug printf)

static volatile uint16_t s_ring[ADC_CAP_CH][RING];
static volatile uint32_t s_head[ADC_CAP_CH], s_tail[ADC_CAP_CH];
static volatile uint32_t s_count[ADC_CAP_CH], s_ovr[ADC_CAP_CH];
static uint s_slice;

// capture channel -> ADC input index (GP26->0, GP27->1, GP28->2)
static const uint8_t s_adc_in[ADC_CAP_CH] = {
    (uint8_t)(ADC_PIN_CH0 - 26u), (uint8_t)(ADC_PIN_CH1 - 26u), (uint8_t)(ADC_PIN_CH2 - 26u)
};

static void pwm_wrap_isr(void) {
    pwm_clear_irq(s_slice);
    for (uint i = 0; i < ADC_CAP_CH; i++) {
        adc_select_input(s_adc_in[i]);
        uint16_t v = (uint16_t)(adc_read() & 0x0FFFu);
        uint32_t h = s_head[i];
        if ((uint32_t)(h - s_tail[i]) < RING) { s_ring[i][h & RING_MASK] = v; s_head[i] = h + 1u; s_count[i]++; }
        else s_ovr[i]++;
    }
}

void adc_capture_init(void) {
    // ---- ADC: 3 analog inputs, manual round-robin in the ISR --------------
    adc_init();
    adc_gpio_init(ADC_PIN_CH0);
    adc_gpio_init(ADC_PIN_CH1);
    adc_gpio_init(ADC_PIN_CH2);

    // ---- 20 kHz phase-correct (center-aligned) PWM = the sample timebase ----
    gpio_set_function(PWM_20K_PIN, GPIO_FUNC_PWM);
    s_slice = pwm_gpio_to_slice_num(PWM_20K_PIN);
    uint32_t fsys = clock_get_hz(clk_sys);
    uint32_t top  = fsys / (2u * PWM_20K_HZ);   // phase-correct: period = 2*TOP counts
    pwm_config c = pwm_get_default_config();
    pwm_config_set_phase_correct(&c, true);
    pwm_config_set_clkdiv_int(&c, 1);
    pwm_config_set_wrap(&c, (uint16_t)top);
    pwm_init(s_slice, &c, false);
    pwm_set_chan_level(s_slice, pwm_gpio_to_channel(PWM_20K_PIN), (uint16_t)(top / 2u)); // 50% duty

    pwm_clear_irq(s_slice);
    pwm_set_irq_enabled(s_slice, true);
    irq_set_exclusive_handler(PWM_IRQ_WRAP, pwm_wrap_isr);
    irq_set_enabled(PWM_IRQ_WRAP, true);
    pwm_set_enabled(s_slice, true);
}

bool adc_capture_pop(int ch, uint16_t *out) {
    if (s_head[ch] == s_tail[ch]) return false;
    if (out) *out = s_ring[ch][s_tail[ch] & RING_MASK];
    s_tail[ch]++;
    return true;
}
uint32_t adc_capture_count(int ch)   { return s_count[ch]; }
uint32_t adc_capture_overrun(int ch) { return s_ovr[ch]; }
