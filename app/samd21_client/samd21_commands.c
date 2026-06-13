// ============================================================================
// samd21_commands.c — SAMD21-specific shell command implementations.
//
// Hosts the chip_commands_table() / chip_commands_count() exports that
// shell_commands.c's shell_find_cmd() falls through to. RA4M1/RP2350/ESP32-C6
// will each have their own equivalent file when those ports land.
//
// GPIO commands use raw chip coordinates (port:u8 in {0=PA, 1=PB}, pin:u8 in
// 0..31). The host-side resolver in dongle_console.lua translates board
// labels (e.g., Xiao "D2") to (port, pin) before sending.
//
// Pin reference (per Seeed XIAO SAMD21 wiki):
//   D0=PA02  D1=PA04  D2=PA10  D3=PA11  D4=PA08(SDA)  D5=PA09(SCL)
//   D6=PB08(TX)  D7=PB09(RX)  D8=PA07(SCK)  D9=PA05(MISO)  D10=PA06(MOSI)
//   LED=PA17 (yellow user LED, currently driven by chain toggle_led)
//   TX_LED=PA18 (blue)   RX_LED=PA19 (blue)
//
// SAMD21 PORT register reference: datasheet DS40001882 §22.
// ============================================================================

#include "shell_commands.h"
#include "vendor/libcomm/opcodes.h"   // SHELL_STATUS_*
#include "samd21.h"
#include "bsp/board_api.h"           // board_millis()
#include "samd21_adc.h"              // samd21_adc_read_oneshot public API
#include "samd21_pin_table.h"        // board-label -> AIN + reserved-pin guard (dac_follow)
#include "samd21_interlocks.h"       // il_parse + il_inst_t (reused by PIO-b gpio interlock)
#include "samd21_hal_pin.h"          // hal_pin_claim*/read* (reused by MIXED interlock)

// ---------- pin validation -------------------------------------------------

static bool validate_pin(uint8_t port, uint8_t pin) {
    return port <= 1u && pin <= 31u;
}

// Pins statically owned by always-on peripherals — GPIO commands refuse them.
// D0 = PA02 (DAC0 output, init'd at boot by samd21_peripherals_init)
// D4 = PA08 (I2C SDA, SERCOM2)
// D5 = PA09 (I2C SCL, SERCOM2)
// D6 = PB08 (RS-485 TX, SERCOM4) — reserved even pre-RS-485-init
// D7 = PB09 (RS-485 RX, SERCOM4) — reserved even pre-RS-485-init
static bool pin_is_reserved(uint8_t port, uint8_t pin) {
    // Only I2C (D4/D5) is STATICALLY reserved. A0/DAC and D6/INT are mode-assigned
    // roles (see samd21_pin_table.c) owned by the active mode, not by this guard.
    if (port == 0u && pin ==  8u) return true;  // D4 / SDA
    if (port == 0u && pin ==  9u) return true;  // D5 / SCL
    return false;
}

// ---------- CMD_GPIO_CONFIG -----------------------------------------------
// args:   port:u8  pin:u8  mode:u8
// result: empty
// status: OK / BAD_ARGS

static uint8_t cmd_gpio_config(shell_reader_t* args, shell_writer_t* result) {
    (void)result;
    uint8_t port = sr_u8(args);
    uint8_t pin  = sr_u8(args);
    uint8_t mode = sr_u8(args);
    if (args->overflow)        return SHELL_STATUS_BAD_ARGS;
    if (sr_remaining(args) != 0) return SHELL_STATUS_BAD_ARGS;
    if (!validate_pin(port, pin)) return SHELL_STATUS_BAD_ARGS;
    if (pin_is_reserved(port, pin)) return SHELL_STATUS_BAD_ARGS;
    if (mode > GPIO_MODE_INPUT_PULLDOWN) return SHELL_STATUS_BAD_ARGS;

    const uint32_t mask = (1u << pin);

    // Always select the GPIO function (clear peripheral mux).
    PORT->Group[port].PINCFG[pin].bit.PMUXEN = 0;

    if (mode == GPIO_MODE_OUTPUT) {
        // Output: clear INEN, clear pull, set DIR.
        PORT->Group[port].PINCFG[pin].bit.INEN   = 0;
        PORT->Group[port].PINCFG[pin].bit.PULLEN = 0;
        PORT->Group[port].DIRSET.reg = mask;
    } else {
        // Input variants: clear DIR, set INEN, configure pull per mode.
        PORT->Group[port].DIRCLR.reg = mask;
        PORT->Group[port].PINCFG[pin].bit.INEN = 1;
        if (mode == GPIO_MODE_INPUT_PULLUP) {
            PORT->Group[port].PINCFG[pin].bit.PULLEN = 1;
            PORT->Group[port].OUTSET.reg = mask;  // OUT=1 → pull-up
        } else if (mode == GPIO_MODE_INPUT_PULLDOWN) {
            PORT->Group[port].PINCFG[pin].bit.PULLEN = 1;
            PORT->Group[port].OUTCLR.reg = mask;  // OUT=0 → pull-down
        } else {
            PORT->Group[port].PINCFG[pin].bit.PULLEN = 0;
        }
    }
    return SHELL_STATUS_OK;
}

// ---------- CMD_GPIO_WRITE ------------------------------------------------
// args:   port:u8  pin:u8  level:u8 (0=low, 1=high)
// result: empty
// status: OK / BAD_ARGS

static uint8_t cmd_gpio_write(shell_reader_t* args, shell_writer_t* result) {
    (void)result;
    uint8_t port  = sr_u8(args);
    uint8_t pin   = sr_u8(args);
    uint8_t level = sr_u8(args);
    if (args->overflow)         return SHELL_STATUS_BAD_ARGS;
    if (sr_remaining(args) != 0) return SHELL_STATUS_BAD_ARGS;
    if (!validate_pin(port, pin)) return SHELL_STATUS_BAD_ARGS;
    if (pin_is_reserved(port, pin)) return SHELL_STATUS_BAD_ARGS;
    if (level > 1u)              return SHELL_STATUS_BAD_ARGS;

    const uint32_t mask = (1u << pin);
    if (level == 1u) PORT->Group[port].OUTSET.reg = mask;
    else             PORT->Group[port].OUTCLR.reg = mask;
    return SHELL_STATUS_OK;
}

// ---------- CMD_GPIO_READ -------------------------------------------------
// args:   port:u8  pin:u8
// result: level:u8 (0 or 1)
// status: OK / BAD_ARGS

static uint8_t cmd_gpio_read(shell_reader_t* args, shell_writer_t* result) {
    uint8_t port = sr_u8(args);
    uint8_t pin  = sr_u8(args);
    if (args->overflow)         return SHELL_STATUS_BAD_ARGS;
    if (sr_remaining(args) != 0) return SHELL_STATUS_BAD_ARGS;
    if (!validate_pin(port, pin)) return SHELL_STATUS_BAD_ARGS;
    if (pin_is_reserved(port, pin)) return SHELL_STATUS_BAD_ARGS;

    uint8_t level = (uint8_t)((PORT->Group[port].IN.reg >> pin) & 1u);
    sw_u8(result, level);
    return result->overflow ? SHELL_STATUS_RESULT_TOO_BIG : SHELL_STATUS_OK;
}

// ============================================================================
// DAC — single channel on PA02 (D0). 10-bit, AVCC reference (0..3.3V).
//
// Statically initialised at boot via samd21_peripherals_init() — PA02 is a
// hard-reserved pin in the dongle/slave role (GPIO commands refuse it via
// pin_is_reserved). CMD_DAC_STOP only tears down the waveform-generator
// timer; the DAC stays enabled with the last sample held.
// ============================================================================

static bool g_dac_initialized = false;

static void dac_init(void) {
    if (g_dac_initialized) return;

    // 1. Bus clock on APBC.
    PM->APBCMASK.reg |= PM_APBCMASK_DAC;

    // 2. Generic clock — drive DAC from GCLK0 (48 MHz default).
    GCLK->CLKCTRL.reg = (uint16_t)(GCLK_CLKCTRL_ID(DAC_GCLK_ID)
                                 | GCLK_CLKCTRL_GEN_GCLK0
                                 | GCLK_CLKCTRL_CLKEN);
    while (GCLK->STATUS.bit.SYNCBUSY) { /* spin */ }

    // 3. Reset DAC peripheral.
    DAC->CTRLA.bit.SWRST = 1;
    while (DAC->STATUS.bit.SYNCBUSY) { /* spin */ }
    while (DAC->CTRLA.bit.SWRST)     { /* spin */ }

    // 4. Reference AVCC (3.3V) + external output buffer enabled.
    DAC->CTRLB.reg = DAC_CTRLB_REFSEL_AVCC | DAC_CTRLB_EOEN;

    // 5. PA02 to DAC function. PA02 is pin 2 in port A; alt function B = DAC.
    //    PINCFG[2].PMUXEN=1 + PMUX[1].PMUXE = MUX_B (=1) selects function B.
    PORT->Group[0].PINCFG[2].bit.PMUXEN = 1;
    PORT->Group[0].PMUX[1].bit.PMUXE    = PORT_PMUX_PMUXE_B_Val;

    // 6. Enable DAC.
    DAC->CTRLA.bit.ENABLE = 1;
    while (DAC->STATUS.bit.SYNCBUSY) { /* spin */ }

    g_dac_initialized = true;
}

// ----------------------------------------------------------------------------
// DAC two-tone DDS engine — TC3 timer-IRQ driven, fixed sample rate + per-tone
// phase accumulators. Two independent tones (off / constant / sine / square,
// 50 Hz–2 kHz, independent amplitude) are SUMMED and HARD-CLIPPED to the 10-bit
// DAC range. Each ISR advances both accumulators by inc = freq·2^32/Fs and
// writes offset + a1·f1(ph1) + a2·f2(ph2). A bench/diagnostic instrument, not an
// application output.
//
// Fixed Fs = 32 kHz (TC3 period 1500 @ 48 MHz) — same ISR load as the old
// engine's max. Square edges quantize to the ISR tick (~31 µs); fine for bench
// use. Higher rates / crisp edges need DMA (deferred).
// ----------------------------------------------------------------------------

#define DAC_WF_PHASE_STEPS  64u                       // sine LUT length (top-6-bit index)
#define DAC_WF_FREQ_MIN     50u
#define DAC_WF_FREQ_MAX     2000u
#define DAC_FS_HZ           32000u                    // fixed DDS sample rate / ADC master clock
#define DAC_TC3_PERIOD      (48000000u / DAC_FS_HZ)   // = 1500

#define DAC_NTONES          2u
#define DAC_TONE_OFF        0u
#define DAC_TONE_CONST      1u
#define DAC_TONE_SINE       2u
#define DAC_TONE_SQUARE     3u

// 64-point sine LUT, centered at 512, peak amplitude 511 (full DAC swing).
// Scaling/offset to the user-requested amplitude+offset happens in the ISR.
static const uint16_t g_sine_lut[DAC_WF_PHASE_STEPS] = {
    512, 562, 612, 660, 708, 753, 796, 836,
    873, 907, 937, 963, 984, 1001, 1013, 1021,
   1023, 1021, 1013, 1001, 984, 963, 937, 907,
    873, 836, 796, 753, 708, 660, 612, 562,
    512, 462, 412, 364, 316, 271, 228, 188,
    151, 117,  87,  61,  40,  23,  11,   3,
      1,   3,  11,  23,  40,  61,  87, 117,
    151, 188, 228, 271, 316, 364, 412, 462,
};

static volatile struct {
    bool     active;
    uint8_t  type[DAC_NTONES];     // off / constant / sine / square
    uint16_t amp[DAC_NTONES];      // peak amplitude, 0..1023
    uint32_t inc[DAC_NTONES];      // phase increment per ISR (0 for off/constant)
    uint32_t ph[DAC_NTONES];       // phase accumulator
    int16_t  offset;               // DC center, 0..1023
    uint32_t isrs_remaining;       // 0 = infinite
} g_dds;

// One tone's signed contribution at its current phase. amp is the peak.
// Division-free: the Cortex-M0+ has no HW divide, and this runs at 32 kHz, so the
// sine scale uses a >>9 shift (512 = 2^9) instead of /512. Two active tones would
// otherwise do two software divides per ISR and starve the USB service loop.
static inline int32_t dds_tone_sample(uint8_t type, uint32_t ph, int32_t amp) {
    switch (type) {
    case DAC_TONE_CONST:  return amp;                          // +amp DC term
    case DAC_TONE_SINE: {
        uint32_t idx = ph >> 26;                               // top 6 bits -> 0..63
        return (((int32_t)g_sine_lut[idx] - 512) * amp) >> 9;  // ±amp peak (÷512)
    }
    case DAC_TONE_SQUARE: return (ph & 0x80000000u) ? amp : -amp;
    default:              return 0;                            // off
    }
}

static bool g_tc3_initialized = false;

// ADC->DAC follow mode (bench): a TC3 tick mirrors the latest hardware-averaged
// ADC conversion on the configured channel onto the DAC (A0), then kicks the
// next conversion. Shares TC3 + the DAC with the waveform generator (mutually
// exclusive). The ISR step NEVER spins on the ADC, so the RS-485 ISR is
// undisturbed; if a conversion is still in flight on a given tick the DAC simply
// holds its last value.
static volatile struct {
    bool    active;
    uint8_t adc_channel;
} g_dac_follow;

static inline void dac_follow_step(void) {
    if (!ADC->INTFLAG.bit.RESRDY) return;          // conversion still running
    uint16_t v = (uint16_t)ADC->RESULT.reg;        // 12-bit (ADJRES set at start)
    ADC->INTFLAG.reg = ADC_INTFLAG_RESRDY;
    DAC->DATA.reg = (uint16_t)(v >> 2);            // 12-bit ADC -> 10-bit DAC
    ADC->SWTRIG.bit.START = 1;                     // begin the next averaged conversion
}

static void tc3_init_once(void) {
    if (g_tc3_initialized) return;
    PM->APBCMASK.reg |= PM_APBCMASK_TC3;
    GCLK->CLKCTRL.reg = (uint16_t)(GCLK_CLKCTRL_ID(TC3_GCLK_ID)
                                 | GCLK_CLKCTRL_GEN_GCLK0
                                 | GCLK_CLKCTRL_CLKEN);
    while (GCLK->STATUS.bit.SYNCBUSY) { /* spin */ }
    g_tc3_initialized = true;
}

static void tc3_stop(void) {
    NVIC_DisableIRQ(TC3_IRQn);
    TC3->COUNT16.INTENCLR.reg = TC_INTENCLR_MC0;
    TC3->COUNT16.CTRLA.bit.ENABLE = 0;
    while (TC3->COUNT16.STATUS.bit.SYNCBUSY) { /* spin */ }
}

static void tc3_start_at_period(uint16_t period) {
    // Reset.
    TC3->COUNT16.CTRLA.bit.SWRST = 1;
    while (TC3->COUNT16.STATUS.bit.SYNCBUSY) { /* spin */ }
    while (TC3->COUNT16.CTRLA.bit.SWRST)     { /* spin */ }

    // 16-bit count, MFRQ wavegen (CC0=TOP and resets counter), prescaler 1.
    TC3->COUNT16.CTRLA.reg = TC_CTRLA_MODE_COUNT16
                           | TC_CTRLA_WAVEGEN_MFRQ
                           | TC_CTRLA_PRESCALER_DIV1;
    TC3->COUNT16.CC[0].reg = period;
    while (TC3->COUNT16.STATUS.bit.SYNCBUSY) { /* spin */ }

    TC3->COUNT16.INTFLAG.reg = TC_INTFLAG_MC0;     // clear stale
    TC3->COUNT16.INTENSET.reg = TC_INTENSET_MC0;
    // The DAC waveform ISR fires up to 32 kHz. Keep it at the LOWEST priority so
    // the USB IRQ (default priority 0) always preempts it — SAMD21 USB endpoints
    // have tiny buffers, and a USB IRQ delayed by an in-progress TC3 ISR drops
    // packets, which breaks CDC framing and hangs host register reads.
    NVIC_SetPriority(TC3_IRQn, 3);
    NVIC_EnableIRQ(TC3_IRQn);

    TC3->COUNT16.CTRLA.bit.ENABLE = 1;
    while (TC3->COUNT16.STATUS.bit.SYNCBUSY) { /* spin */ }
}

static void adc_isr_service(void);   // ADC step, keyed off this tone clock (defined below)

// TC3 is the master clock: it generates the DAC waveform AND, in ADC mode, keys
// the ADC sampler. Both run from this one ISR.
void TC3_Handler(void) {
    if (!TC3->COUNT16.INTFLAG.bit.MC0) return;
    TC3->COUNT16.INTFLAG.reg = TC_INTFLAG_MC0;

    if (g_dac_follow.active) { dac_follow_step(); return; }

    // DAC DDS update: advance both phase accumulators, sum, hard-clip.
    if (g_dds.active) {
        int32_t sample = g_dds.offset;
        for (uint8_t t = 0; t < DAC_NTONES; t++) {
            if (g_dds.type[t] == DAC_TONE_OFF) continue;
            g_dds.ph[t] += g_dds.inc[t];
            sample += dds_tone_sample(g_dds.type[t], g_dds.ph[t], (int32_t)g_dds.amp[t]);
        }
        if (sample <    0) sample = 0;
        if (sample > 1023) sample = 1023;
        DAC->DATA.reg = (uint16_t)sample;

        // Duration: 0 = infinite (ADC mode); otherwise the standalone bench
        // waveform decrements and stops TC3 when it expires.
        if (g_dds.isrs_remaining != 0u && --g_dds.isrs_remaining == 0u) {
            g_dds.active = false;
            tc3_stop();
            return;                       // TC3 disabled; nothing left to service
        }
    }

    adc_isr_service();                    // ADC step (no-op unless the ADC engine is running)
}

// ---------- CMD_DAC_WAVEFORM_WRITE ---------------------------------------
// args:   waveform:u8 (1=constant, 2=sine, 3=square)
//         amplitude:u16 (peak, 0..1023)
//         offset:u16    (DC center, 0..1023)
//         frequency_hz:u32  (50..2000 inclusive)
//         duration_ms:u32   (0 = infinite)
// result: empty
// status: OK / BAD_ARGS
//
// Drives ONE tone (tone 0) of the two-tone DDS engine; tone 1 is forced off.
// For full two-tone control use the ADC mode-bank registers (0x20..0x2C).

static uint8_t cmd_dac_waveform_write(shell_reader_t* args, shell_writer_t* result) {
    (void)result;
    uint8_t  waveform   = sr_u8(args);
    uint16_t amplitude  = sr_u16(args);
    uint16_t offset     = sr_u16(args);
    uint32_t frequency  = sr_u32(args);
    uint32_t duration_ms = sr_u32(args);
    if (args->overflow)            return SHELL_STATUS_BAD_ARGS;
    if (sr_remaining(args) != 0)   return SHELL_STATUS_BAD_ARGS;
    if (waveform < DAC_TONE_CONST || waveform > DAC_TONE_SQUARE) return SHELL_STATUS_BAD_ARGS;
    if (amplitude > 1023u || offset > 1023u) return SHELL_STATUS_BAD_ARGS;
    if (frequency < DAC_WF_FREQ_MIN || frequency > DAC_WF_FREQ_MAX) return SHELL_STATUS_BAD_ARGS;

    dac_init();
    tc3_init_once();

    // Duration -> ISR count at the fixed sample rate. 0 = infinite.
    uint32_t isrs = (duration_ms == 0u) ? 0u : (duration_ms * DAC_FS_HZ / 1000u);

    // Atomically install the new tone state — disable IRQ during update.
    NVIC_DisableIRQ(TC3_IRQn);
    g_dds.type[0] = waveform;
    g_dds.amp[0]  = amplitude;
    g_dds.inc[0]  = (waveform == DAC_TONE_CONST)
                  ? 0u : (uint32_t)(((uint64_t)frequency << 32) / DAC_FS_HZ);
    g_dds.ph[0]   = 0u;
    g_dds.type[1] = DAC_TONE_OFF;
    g_dds.amp[1]  = 0u; g_dds.inc[1] = 0u; g_dds.ph[1] = 0u;
    g_dds.offset  = (int16_t)offset;
    g_dds.isrs_remaining = isrs;
    g_dds.active  = true;
    tc3_start_at_period(DAC_TC3_PERIOD);   // re-enables NVIC

    return SHELL_STATUS_OK;
}

// ---------- CMD_DAC_STOP -------------------------------------------------
// args:   empty
// result: empty
// status: OK
//
// Disables the TC3 IRQ; DAC parks at whatever sample was last written.

static uint8_t cmd_dac_stop(shell_reader_t* args, shell_writer_t* result) {
    (void)result;
    if (sr_remaining(args) != 0) return SHELL_STATUS_BAD_ARGS;
    g_dds.active = false;
    g_dac_follow.active = false;         // stop follow too (shared TC3 + DAC)
    tc3_stop();
    return SHELL_STATUS_OK;
}

// ---------- CMD_DAC_WRITE ------------------------------------------------
// args:   value:u16 (0..1023, 10-bit DAC output level)
// result: empty
// status: OK / BAD_ARGS (value > 1023)
//
// Output voltage = (value / 1023) * 3.3 V.

static uint8_t cmd_dac_write(shell_reader_t* args, shell_writer_t* result) {
    (void)result;
    uint16_t value = sr_u16(args);
    if (args->overflow)          return SHELL_STATUS_BAD_ARGS;
    if (sr_remaining(args) != 0) return SHELL_STATUS_BAD_ARGS;
    if (value > 1023u)           return SHELL_STATUS_BAD_ARGS;

    dac_init();

    // If a waveform or follow mode is running, stop it — static write wins.
    if (g_dds.active || g_dac_follow.active) {
        g_dds.active = false;
        g_dac_follow.active = false;
        tc3_stop();
    }

    DAC->DATA.reg = value;
    while (DAC->STATUS.bit.SYNCBUSY) { /* spin */ }
    return SHELL_STATUS_OK;
}

// ============================================================================
// ADC — 12-bit, 20 channels (AIN[0..19]).
//
// Configured for full-scale 0..VDDANA (≈ 3.3 V) via INTVCC1 reference
// (= VDDANA/2) + GAIN=DIV2 (input attenuated by 2 before comparison). Net
// effective input range: 2 × reference = 2 × (VDDANA/2) = VDDANA.
//
// Channel mapping (AIN <-> Xiao pad), per the SAMD21 datasheet pin mux + the
// Xiao-SAMD21 trace doc:
//   D0=PA02=AIN0   D1=PA04=AIN4   D2=PA10=AIN18  D3=PA11=AIN19
//   D4=PA08=AIN16  D5=PA09=AIN17  D6=PB08=AIN2   D7=PB09=AIN3
//   D8=PA07=AIN7   D9=PA05=AIN5   D10=PA06=AIN6
// Host-side translation lives in dongle_console.lua.
// ============================================================================

static bool g_adc_initialized = false;

static void adc_init(void) {
    if (g_adc_initialized) return;

    // 1. Bus clock.
    PM->APBCMASK.reg |= PM_APBCMASK_ADC;

    // 2. Generic clock — drive ADC from GCLK0 (48 MHz).
    GCLK->CLKCTRL.reg = (uint16_t)(GCLK_CLKCTRL_ID(ADC_GCLK_ID)
                                 | GCLK_CLKCTRL_GEN_GCLK0
                                 | GCLK_CLKCTRL_CLKEN);
    while (GCLK->STATUS.bit.SYNCBUSY) { /* spin */ }

    // 3. Reset peripheral.
    ADC->CTRLA.bit.SWRST = 1;
    while (ADC->STATUS.bit.SYNCBUSY) { /* spin */ }
    while (ADC->CTRLA.bit.SWRST)     { /* spin */ }

    // 4. Load factory calibration (NVMCTRL software-cal row) — required by
    //    datasheet 33.6.6. Without this the ADC has significant offset.
    uint32_t bias = (*((uint32_t*)ADC_FUSES_BIASCAL_ADDR) & ADC_FUSES_BIASCAL_Msk) >> ADC_FUSES_BIASCAL_Pos;
    uint32_t linearity = (*((uint32_t*)ADC_FUSES_LINEARITY_0_ADDR) & ADC_FUSES_LINEARITY_0_Msk) >> ADC_FUSES_LINEARITY_0_Pos;
    linearity |= ((*((uint32_t*)ADC_FUSES_LINEARITY_1_ADDR) & ADC_FUSES_LINEARITY_1_Msk) >> ADC_FUSES_LINEARITY_1_Pos) << 5;
    ADC->CALIB.reg = ADC_CALIB_BIAS_CAL(bias) | ADC_CALIB_LINEARITY_CAL(linearity);

    // 5. Reference INTVCC1 (= VDDANA/2). Combined with GAIN=DIV2 below, gives
    //    effective full-scale input = VDDANA (≈ 3.3 V).
    ADC->REFCTRL.reg = ADC_REFCTRL_REFSEL_INTVCC1;

    // 6. Sample rate / averaging — default single sample.
    ADC->AVGCTRL.reg = ADC_AVGCTRL_SAMPLENUM_1 | ADC_AVGCTRL_ADJRES(0);
    ADC->SAMPCTRL.reg = 0x05;   // default 5 ADC clock cycles sample time
                                // (overridden per-call by adc_apply_avg_hold)

    // 7. CTRLB: prescaler /256 → 48 MHz / 256 ≈ 187.5 kHz ADC clock, 12-bit
    //    single-conversion. /256 doubles per-sample throughput vs the historical
    //    /512 default while staying well below the 2.1 MHz max ADC clock.
    //    RESSEL toggled to 16BIT per-call when oversample > 0.
    ADC->CTRLB.reg = ADC_CTRLB_PRESCALER_DIV256 | ADC_CTRLB_RESSEL_12BIT;
    while (ADC->STATUS.bit.SYNCBUSY) { /* spin */ }

    // 8. INPUTCTRL: GAIN=DIV2, MUXNEG=GND (single-ended), MUXPOS set per-read.
    ADC->INPUTCTRL.reg = ADC_INPUTCTRL_GAIN_DIV2 | ADC_INPUTCTRL_MUXNEG_GND;
    while (ADC->STATUS.bit.SYNCBUSY) { /* spin */ }

    // 9. Enable.
    ADC->CTRLA.bit.ENABLE = 1;
    while (ADC->STATUS.bit.SYNCBUSY) { /* spin */ }

    // 10. Throw away the first conversion (per datasheet — first sample after
    //     enable is unreliable). MUXPOS=0 is fine for this dummy.
    ADC->SWTRIG.bit.START = 1;
    while (!ADC->INTFLAG.bit.RESRDY) { /* spin */ }
    (void)ADC->RESULT.reg;
    ADC->INTFLAG.reg = ADC_INTFLAG_RESRDY;

    g_adc_initialized = true;
}

// Configure the pin associated with an AIN channel to analog (PMUX function B).
// Caller supplies the chip (port, pin) coordinates of the pad.
static void adc_pin_config(uint8_t port, uint8_t pin) {
    PORT->Group[port].PINCFG[pin].bit.PMUXEN = 1;
    if (pin & 1u) {
        PORT->Group[port].PMUX[pin / 2u].bit.PMUXO = PORT_PMUX_PMUXO_B_Val;
    } else {
        PORT->Group[port].PMUX[pin / 2u].bit.PMUXE = PORT_PMUX_PMUXE_B_Val;
    }
}

// Translate an AIN[ch] index back to the (port, pin) of its pad. Used to
// configure the pin's PMUX before sampling. Covers channels 0..19 on
// SAMD21G18A; entries outside the package map are {0xFF, 0xFF} (unused).
typedef struct { uint8_t port; uint8_t pin; } ain_to_pad_t;
static const ain_to_pad_t g_ain_to_pad[20] = {
    [0]  = {0,  2}, [1]  = {0,  3}, [2]  = {1,  8}, [3]  = {1,  9},
    [4]  = {0,  4}, [5]  = {0,  5}, [6]  = {0,  6}, [7]  = {0,  7},
    [8]  = {1,  0}, [9]  = {1,  1}, [10] = {1,  2}, [11] = {1,  3},
    [12] = {0, 10}, [13] = {0, 11}, [14] = {0, 12}, [15] = {0, 13},
    [16] = {0,  8}, [17] = {0,  9}, [18] = {0, 10}, [19] = {0, 11},
};

// ---- per-call ADC configuration (oversample + sample-hold) ---------------
// oversample_exp:   0..7 → SAMPLENUM = 2^N samples averaged (1, 2, 4, 8, 16, 32, 64, 128)
//                   Result is hardware-averaged to 12-bit equivalent (0..4095).
//
//                   ADJRES is set to min(oversample_exp, 4), NOT to oversample_exp
//                   directly. Empirical SAMD21 behaviour (bench-verified 2026-05-24):
//                   for SAMPLENUM > 4, the hardware pre-right-shifts each sample by
//                   (SAMPLENUM-4) bits to keep the accumulator in its fixed width,
//                   THEN applies ADJRES. So writing ADJRES=SAMPLENUM gives a result
//                   already further shifted by (SAMPLENUM-4), under-reporting the
//                   average by 2^(SAMPLENUM-4). Capping ADJRES at 4 unwinds that.
//                   Trade-off: SAMPLENUM > 4 loses (SAMPLENUM-4) low bits of each
//                   raw sample to the pre-shift — noise-reduction benefit caps at
//                   ~SAMPLENUM=16 in practice. Going to 32/64/128 still works but
//                   the marginal stddev improvement is small.
//
// sample_hold_cyc:  0..63 → SAMPCTRL.SAMPLEN. Time = (cyc + 1) ADC clocks.
//                   At /256 prescaler (5.33 µs/cycle): 5 µs..341 µs hold time.
//                   Pick 5..10 for low-impedance sources, 20+ for high-Z sensors
//                   (≥ 100 kΩ thermistors / photoresistors).
#define ADC_OVERSAMPLE_MAX  7u
#define ADC_SAMPLE_HOLD_MAX 63u

static uint8_t adc_apply_avg_hold(uint8_t oversample_exp, uint8_t sample_hold_cyc) {
    if (oversample_exp > ADC_OVERSAMPLE_MAX) return SHELL_STATUS_BAD_ARGS;
    if (sample_hold_cyc > ADC_SAMPLE_HOLD_MAX) return SHELL_STATUS_BAD_ARGS;

    uint8_t adjres = (oversample_exp <= 4u) ? oversample_exp : 4u;
    ADC->AVGCTRL.reg = ADC_AVGCTRL_SAMPLENUM(oversample_exp)
                     | ADC_AVGCTRL_ADJRES(adjres);
    ADC->SAMPCTRL.reg = sample_hold_cyc;

    // RESSEL: 12-bit single sample vs 16-bit averaging mode.
    uint32_t ctrlb = ADC->CTRLB.reg & ~ADC_CTRLB_RESSEL_Msk;
    ctrlb |= (oversample_exp == 0u) ? ADC_CTRLB_RESSEL_12BIT
                                    : ADC_CTRLB_RESSEL_16BIT;
    ADC->CTRLB.reg = (uint16_t)ctrlb;
    while (ADC->STATUS.bit.SYNCBUSY) { /* spin */ }
    return SHELL_STATUS_OK;
}

// Minimum per-sample wall-clock at /256 prescaler. One conversion takes
// (sample_hold_cyc + 7) ADC clocks (6 for 12-bit conv + 1 propagation +
// sample-hold). Multiplied by oversample count for averaging. 5333 ns/cycle
// (= 1e9 / 187500). Returns µs, rounded up.
static uint32_t adc_min_sample_period_us(uint8_t oversample_exp, uint8_t sample_hold_cyc) {
    uint32_t samples = 1u << oversample_exp;
    uint32_t cyc     = (uint32_t)sample_hold_cyc + 7u;
    uint32_t ns      = samples * cyc * 5333u;
    return (ns + 999u) / 1000u;
}

// ---------- Public single-shot ADC reader ---------------------------------
// Exposed via samd21_adc.h for use by the interlock framework's adc_int
// input source. Mirrors the conversion path used by CMD_ADC_READ; callers
// are responsible for not interleaving with a long-running ADC capture.
uint16_t samd21_adc_read_oneshot(uint8_t channel,
                                 uint8_t oversample_exp,
                                 uint8_t sh_cyc) {
    if (channel > 19u) return 0;

    adc_init();
    (void)adc_apply_avg_hold(oversample_exp, sh_cyc);

    ain_to_pad_t pad = g_ain_to_pad[channel];
    if (pad.port != 0xFFu) {
        adc_pin_config(pad.port, pad.pin);
    }

    ADC->INPUTCTRL.bit.MUXPOS = channel;
    while (ADC->STATUS.bit.SYNCBUSY) { /* spin */ }
    ADC->INTFLAG.reg = ADC_INTFLAG_RESRDY;
    ADC->SWTRIG.bit.START = 1;
    while (!ADC->INTFLAG.bit.RESRDY) { /* spin */ }
    uint16_t value = (uint16_t)ADC->RESULT.reg;
    ADC->INTFLAG.reg = ADC_INTFLAG_RESRDY;
    return value;
}

// ---------- CMD_ADC_READ -------------------------------------------------
// args:   channel:u8 (AIN index, 0..19)
//         oversample_exp:u8  (0..7 → 1..128 samples averaged)
//         sample_hold_cyc:u8 (0..63 ADC clock cycles)
// result: value:u16 (12-bit equivalent, 0..4095; 4095 ≈ VDDANA ≈ 3.3 V)
// status: OK / BAD_ARGS

static uint8_t cmd_adc_read(shell_reader_t* args, shell_writer_t* result) {
    uint8_t channel         = sr_u8(args);
    uint8_t oversample_exp  = sr_u8(args);
    uint8_t sample_hold_cyc = sr_u8(args);
    if (args->overflow)                       return SHELL_STATUS_BAD_ARGS;
    if (sr_remaining(args) != 0)              return SHELL_STATUS_BAD_ARGS;
    if (channel > 19u)                        return SHELL_STATUS_BAD_ARGS;
    if (oversample_exp > ADC_OVERSAMPLE_MAX)  return SHELL_STATUS_BAD_ARGS;
    if (sample_hold_cyc > ADC_SAMPLE_HOLD_MAX) return SHELL_STATUS_BAD_ARGS;

    uint16_t value = samd21_adc_read_oneshot(channel, oversample_exp, sample_hold_cyc);
    sw_u16(result, value);
    return result->overflow ? SHELL_STATUS_RESULT_TOO_BIG : SHELL_STATUS_OK;
}

// ---------- CMD_DAC_FOLLOW_START -----------------------------------------
// Bench mode: continuously sample one ADC input (hardware-averaged) and mirror
// it to the DAC (A0). Interrupt-driven (TC3); the ISR step is non-blocking.
// Mutually exclusive with the DAC waveform generator (shared TC3 + DAC).
//
// args:   oversample_exp:u8  (0..7  -> 1..128 samples averaged, like adc_read)
//         sample_hold_cyc:u8 (0..63)
//         update_hz:u16      (1..10000; 0 -> default 1000)
//         pin:str            (board label "A1".."A10" / "D1".."D10", trailing)
// result: empty
// status: OK / BAD_ARGS
//
// The input is a USER board label, validated against the pin table — A0 (DAC),
// D4/D5 (I2C), D6/D7 (UART) are RESERVED and rejected; the label must have ADC.
#define DAC_FOLLOW_HZ_MIN      1u
#define DAC_FOLLOW_HZ_MAX      10000u
#define DAC_FOLLOW_HZ_DEFAULT  1000u

static uint8_t cmd_dac_follow_start(shell_reader_t* args, shell_writer_t* result) {
    (void)result;
    uint8_t  oversample_exp  = sr_u8(args);
    uint8_t  sample_hold_cyc = sr_u8(args);
    uint16_t update_hz       = sr_u16(args);
    if (args->overflow)                           return SHELL_STATUS_BAD_ARGS;

    uint16_t label_len = sr_remaining(args);
    const char* label  = (const char*)args->p;        // trailing bytes = board label
    if (label_len == 0u || label_len > 3u)        return SHELL_STATUS_BAD_ARGS;
    if (oversample_exp > ADC_OVERSAMPLE_MAX)      return SHELL_STATUS_BAD_ARGS;
    if (sample_hold_cyc > ADC_SAMPLE_HOLD_MAX)    return SHELL_STATUS_BAD_ARGS;
    if (update_hz == 0u) update_hz = DAC_FOLLOW_HZ_DEFAULT;
    if (update_hz < DAC_FOLLOW_HZ_MIN || update_hz > DAC_FOLLOW_HZ_MAX) return SHELL_STATUS_BAD_ARGS;

    const board_pin_t* p = board_pin_lookup(label, (uint8_t)label_len);
    if (p == NULL)                                return SHELL_STATUS_BAD_ARGS;
    if (board_pin_is_reserved(p))                 return SHELL_STATUS_BAD_ARGS;  // A0/D4/D5/D6/D7
    if (!(p->caps & BOARD_PIN_CAP_ADC) || p->adc_channel == BOARD_PIN_ADC_NONE)
                                                  return SHELL_STATUS_BAD_ARGS;

    uint32_t period = 48000000u / (uint32_t)update_hz;
    if (period < 2u || period > 65535u)           return SHELL_STATUS_BAD_ARGS;

    dac_init();
    tc3_init_once();
    adc_init();
    (void)adc_apply_avg_hold(oversample_exp, sample_hold_cyc);

    // Configure the input pad + ADC mux ONCE; the ISR then free-runs conversions.
    ain_to_pad_t pad = g_ain_to_pad[p->adc_channel];
    if (pad.port != 0xFFu) adc_pin_config(pad.port, pad.pin);
    ADC->INPUTCTRL.bit.MUXPOS = p->adc_channel;
    while (ADC->STATUS.bit.SYNCBUSY) { /* spin */ }
    ADC->INTFLAG.reg = ADC_INTFLAG_RESRDY;
    ADC->SWTRIG.bit.START = 1;                    // kick the first conversion

    NVIC_DisableIRQ(TC3_IRQn);
    g_dds.active             = false;             // mutual exclusion with waveform
    g_dac_follow.adc_channel = p->adc_channel;
    g_dac_follow.active      = true;
    tc3_start_at_period((uint16_t)period);        // re-enables the TC3 NVIC line
    return SHELL_STATUS_OK;
}

// ---------- CMD_DAC_FOLLOW_STOP ------------------------------------------
// args: empty   result: empty   status: OK   (DAC parks at its last value)
static uint8_t cmd_dac_follow_stop(shell_reader_t* args, shell_writer_t* result) {
    (void)result;
    if (sr_remaining(args) != 0) return SHELL_STATUS_BAD_ARGS;
    g_dac_follow.active = false;
    tc3_stop();
    return SHELL_STATUS_OK;
}

// ---------- CMD_ADC_CAPTURE -----------------------------------------------
// Multi-channel buffered ADC capture.
//
// args:   num_channels:u8 (1..8)
//         channels:u8[num_channels] (each 0..19)
//         num_samples:u16 (per channel)
//         delta_time_us:u32 (≥ 1000; board_millis() timing, ms granularity)
//         oversample_exp:u8  (0..7 → 1..128 samples averaged per result)
//         sample_hold_cyc:u8 (0..63 ADC clock cycles)
// result: num_channels:u8 (echo)
//         num_samples:u16 (echo)
//         samples:u16[num_channels * num_samples]  (interleaved: sample0_ch0,
//                                                   sample0_ch1, ..., sample1_ch0, ...)
// status: OK / BAD_ARGS / RESULT_TOO_BIG
//
// delta_time_us must be ≥ adc_min_sample_period_us(oversample_exp, sample_hold_cyc)
// times num_channels — refused with BAD_ARGS otherwise so the captured samples
// don't silently smear.
//
// v1 cap: total samples (num_channels × num_samples) ≤ 60 to fit one OP_SHELL_REPLY
// frame (≤ 125 B result_message after the 3-byte shell wrapper). Larger captures
// will need chunked replies; deferred.

#define ADC_CAPTURE_MAX_CHANNELS  8u
#define ADC_CAPTURE_MAX_SAMPLES   60u   // total across all channels
#define ADC_CAPTURE_MIN_DELTA_US  1000u

static uint8_t cmd_adc_capture(shell_reader_t* args, shell_writer_t* result) {
    uint8_t num_channels = sr_u8(args);
    if (args->overflow) return SHELL_STATUS_BAD_ARGS;
    if (num_channels == 0u || num_channels > ADC_CAPTURE_MAX_CHANNELS) return SHELL_STATUS_BAD_ARGS;

    uint8_t channels[ADC_CAPTURE_MAX_CHANNELS];
    for (uint8_t i = 0; i < num_channels; i++) {
        channels[i] = sr_u8(args);
        if (args->overflow)        return SHELL_STATUS_BAD_ARGS;
        if (channels[i] > 19u)     return SHELL_STATUS_BAD_ARGS;
    }

    uint16_t num_samples    = sr_u16(args);
    uint32_t delta_time_us  = sr_u32(args);
    uint8_t  oversample_exp = sr_u8 (args);
    uint8_t  sample_hold    = sr_u8 (args);
    if (args->overflow)            return SHELL_STATUS_BAD_ARGS;
    if (sr_remaining(args) != 0)   return SHELL_STATUS_BAD_ARGS;
    if (num_samples == 0u)         return SHELL_STATUS_BAD_ARGS;
    if (delta_time_us < ADC_CAPTURE_MIN_DELTA_US) return SHELL_STATUS_BAD_ARGS;

    uint32_t total = (uint32_t)num_channels * (uint32_t)num_samples;
    if (total > ADC_CAPTURE_MAX_SAMPLES) return SHELL_STATUS_BAD_ARGS;

    // Refuse delta_time_us that won't fit one full per-channel scan with the
    // requested oversample/sample-hold — prevents silently-smeared samples.
    uint32_t min_per_sample_us = adc_min_sample_period_us(oversample_exp, sample_hold);
    uint32_t min_slot_us       = min_per_sample_us * (uint32_t)num_channels;
    if (delta_time_us < min_slot_us) return SHELL_STATUS_BAD_ARGS;

    // Cold-path safety: cmd_adc_capture may be the first ADC op after boot
    // (e.g. waveform-capture-only workflows). g_adc_initialized is in zero-init
    // .bss, so without this the ADC has no clock/enable/calibration and the
    // inner spin loop hangs forever. No-op if already initialised.
    adc_init();

    uint8_t st = adc_apply_avg_hold(oversample_exp, sample_hold);
    if (st != SHELL_STATUS_OK) return st;

    // Pre-configure each channel's pad for analog mux.
    for (uint8_t i = 0; i < num_channels; i++) {
        ain_to_pad_t pad = g_ain_to_pad[channels[i]];
        if (pad.port != 0xFFu) {
            adc_pin_config(pad.port, pad.pin);
        }
    }

    uint32_t delta_ms = delta_time_us / 1000u;

    // Emit result header up front.
    sw_u8 (result, num_channels);
    sw_u16(result, num_samples);

    // Capture loop. Sample-slot timing: anchor on board_millis() at start of
    // each slot, sample all channels back-to-back, then sleep remaining time.
    for (uint16_t s = 0; s < num_samples; s++) {
        uint32_t slot_start_ms = board_millis();

        for (uint8_t c = 0; c < num_channels; c++) {
            ADC->INPUTCTRL.bit.MUXPOS = channels[c];
            while (ADC->STATUS.bit.SYNCBUSY) { /* spin */ }
            ADC->INTFLAG.reg = ADC_INTFLAG_RESRDY;
            ADC->SWTRIG.bit.START = 1;
            while (!ADC->INTFLAG.bit.RESRDY) { /* spin */ }
            uint16_t v = (uint16_t)ADC->RESULT.reg;
            ADC->INTFLAG.reg = ADC_INTFLAG_RESRDY;
            sw_u16(result, v);
            if (result->overflow) return SHELL_STATUS_RESULT_TOO_BIG;
        }

        if (s < num_samples - 1u) {
            while ((board_millis() - slot_start_ms) < delta_ms) { /* spin */ }
        }
    }

    return SHELL_STATUS_OK;
}

// ============================================================================
// I2C master — SERCOM2 on D4=PA08 (SDA) / D5=PA09 (SCL), 100 kHz.
//
// PMUX function D selects SERCOM2 PAD[0]/[1] on PA08/PA09. Statically
// initialised at boot via samd21_peripherals_init(); D4/D5 are reserved
// from GPIO commands by pin_is_reserved().
//
// Smart mode disabled; we drive CTRLB.CMD + ACKACT explicitly so error
// paths (NACK / bus error / arb lost) can issue STOP cleanly.
//
// BAUD = (f_GCLK / (2 × f_SCL)) - 5 = (48 MHz / 200 kHz) - 5 = 235.
// Rise time term ignored — acceptable for the slow-bus role.
//
// Polling-mode. Bus hangs are caught by layer-2 WDT (max ~4 s).
// ============================================================================

#define I2C_SERCOM           SERCOM2
#define I2C_GCLK_ID_CORE     SERCOM2_GCLK_ID_CORE
#define I2C_GCLK_ID_SLOW     SERCOM2_GCLK_ID_SLOW
#define I2C_BAUD_100K        235u

static void i2c_init(void) {
    // 1. Bus clock.
    PM->APBCMASK.reg |= PM_APBCMASK_SERCOM2;

    // 2. SERCOM2 core + slow clocks → GCLK0 (48 MHz).
    GCLK->CLKCTRL.reg = (uint16_t)(GCLK_CLKCTRL_ID(I2C_GCLK_ID_CORE)
                                 | GCLK_CLKCTRL_GEN_GCLK0
                                 | GCLK_CLKCTRL_CLKEN);
    while (GCLK->STATUS.bit.SYNCBUSY) { /* spin */ }
    GCLK->CLKCTRL.reg = (uint16_t)(GCLK_CLKCTRL_ID(I2C_GCLK_ID_SLOW)
                                 | GCLK_CLKCTRL_GEN_GCLK0
                                 | GCLK_CLKCTRL_CLKEN);
    while (GCLK->STATUS.bit.SYNCBUSY) { /* spin */ }

    // 3. Reset SERCOM2.
    I2C_SERCOM->I2CM.CTRLA.bit.SWRST = 1;
    while (I2C_SERCOM->I2CM.SYNCBUSY.bit.SWRST) { /* spin */ }

    // 4. Configure as I2C master, 300 ns SDA hold, smart mode OFF.
    I2C_SERCOM->I2CM.CTRLA.reg =
        SERCOM_I2CM_CTRLA_MODE_I2C_MASTER |
        SERCOM_I2CM_CTRLA_SDAHOLD(2);

    // 5. 100 kHz baud.
    I2C_SERCOM->I2CM.BAUD.reg = SERCOM_I2CM_BAUD_BAUD(I2C_BAUD_100K);

    // 6. PMUX PA08/PA09 → function D (SERCOM-ALT = SERCOM2 PAD[0]/[1]).
    PORT->Group[0].PINCFG[8].bit.PMUXEN = 1;
    PORT->Group[0].PMUX[4].bit.PMUXE    = PORT_PMUX_PMUXE_D_Val;  // PA08 even
    PORT->Group[0].PINCFG[9].bit.PMUXEN = 1;
    PORT->Group[0].PMUX[4].bit.PMUXO    = PORT_PMUX_PMUXO_D_Val;  // PA09 odd

    // 7. Enable.
    I2C_SERCOM->I2CM.CTRLA.bit.ENABLE = 1;
    while (I2C_SERCOM->I2CM.SYNCBUSY.bit.ENABLE) { /* spin */ }

    // 8. Force bus state to IDLE (1). On power-up the controller reports
    //    UNKNOWN (0) and refuses transactions until told the bus is idle.
    I2C_SERCOM->I2CM.STATUS.bit.BUSSTATE = 1;
    while (I2C_SERCOM->I2CM.SYNCBUSY.bit.SYSOP) { /* spin */ }
}

#ifdef I2C_CLIENT
// ============================================================================
// I2C CLIENT (slave) mode — SERCOM2 reconfigured as an I2C slave so the Pico
// I2C master can talk to this dongle. Same pins (PA08/PA09 = D4/D5), same GCLK0
// as the master path; -DI2C_CLIENT selects slave instead of master at boot.
//
// Register-mapped device (like a classic I2C chip): the master writes a register
// address (pointer), then reads/writes with auto-increment. SERCOM slave uses
// smart mode + SCLSM; the ISR loads the tx byte into DATA — and must pre-load the
// first byte on AMATCH(read) or the stale address byte clocks out first.
//
// M2a — control bank (always live, every mode):
//   0x00 WHO_AM_I(ro)=0x5A  0x01 VERSION(ro)  0x02 MODE(rw)  0x03 STATUS(ro)
//   0x04 INT_FLAGS(ro/W1C)  0x05 I2C_ADDR(ro)  0x06..0x0D UNIQUE_ID[8](ro)
// Mode switching is stubbed (stores MODE); per-mode config + the store window
// (0x40..) land in M2b/M2c.
// ============================================================================
#define I2C_CLIENT_WHOAMI       0x5Au
#define I2C_CLIENT_VERSION      0x01u
#define I2C_CLIENT_ADDR_DEFAULT 0x55u   // 7-bit; M2c makes this come from the store

enum { MODE_IDLE = 0, MODE_PIO, MODE_ADC, MODE_MIXED, MODE_SERVO, MODE_COUNTER, MODE_MAX = MODE_COUNTER };

static volatile uint8_t i2c_reg_ptr;        // register address pointer
static volatile bool    i2c_reg_ptr_known;  // first write byte after addr = the pointer
static volatile uint8_t g_mode      = MODE_IDLE;
static volatile uint8_t g_status;           // bit0 flash-busy, bit1 store-err, bit2 int-pending
static volatile bool    g_offline = false;  // commissioning state: false=ONLINE, true=OFFLINE (STATUS bit3)
static volatile uint8_t g_int_flags;        // interlock trip flags (W1C)
static uint8_t          g_i2c_addr  = I2C_CLIENT_ADDR_DEFAULT;
static uint8_t          g_unique_id[8];

// SAMD21 factory serial: word0 @ 0x0080A00C, word1 @ 0x0080A040 (low 64 bits).
static void i2c_read_unique_id(void) {
    uint32_t w0 = *(const volatile uint32_t *)0x0080A00Cu;
    uint32_t w1 = *(const volatile uint32_t *)0x0080A040u;
    g_unique_id[0] = (uint8_t)w0;         g_unique_id[1] = (uint8_t)(w0 >> 8);
    g_unique_id[2] = (uint8_t)(w0 >> 16); g_unique_id[3] = (uint8_t)(w0 >> 24);
    g_unique_id[4] = (uint8_t)w1;         g_unique_id[5] = (uint8_t)(w1 >> 8);
    g_unique_id[6] = (uint8_t)(w1 >> 16); g_unique_id[7] = (uint8_t)(w1 >> 24);
}

// ---- M2b: log-structured, wear-leveled NAME-KEYED config store -------------
// 256 rows (64 KB) just below the commission A/B slots (0x3FE00). Each commit
// appends a 256-B entry to a round-robin NON-LIVE row; latest seq per NAME
// wins on read; CRC-8 guards torn writes (power-safe). RAM-shadowed into a
// fixed bank of slots (each slot = one 4-char name); the flash write (~ms) is
// deferred to the main loop (i2c_store_service) so the I2C ISR stays fast.
// Write-coalescing skips a commit whose bytes are unchanged. This is the
// foundation for a named-file config filesystem (4-char names).
extern uint8_t crc8_autosar_update(uint8_t crc, uint8_t byte);   // libcomm

#define STORE_ROWS        256u
#define STORE_ROW_SIZE    256u
#define STORE_PAGE_SIZE   64u
#define STORE_BASE        (0x3FE00u - STORE_ROWS * STORE_ROW_SIZE)  // 0x2FE00
#define STORE_ENTRY_MAGIC 0x10C0FFEEu
#define STORE_NAME_LEN    4u
#define STORE_DATA_MAX    240u
#define STORE_SLOTS       16u    // RAM-shadow cap: at most 16 distinct names live

// store-window register addresses
#define REG_REC_SEL    0x40u
#define REG_REC_LEN    0x41u
#define REG_REC_OFF    0x42u
#define REG_REC_DATA   0x43u
#define REG_REC_CTRL   0x44u
#define REG_STORE_STAT 0x45u
#define REG_SET_ADDR   0x0Fu   // control-bank: magic-guarded address commissioning (data-port)

// FILE bank (0x50..0x55) — name-keyed, read-only-for-master config-file access.
// Lives in the MAIN switch so it's available in every mode (the Pico reads
// config regardless of the active HIL mode). LIST enumerates; OPEN+SIZE+DATA
// reads a known file by name. File writing is the USB path (a later step).
#define REG_FILE_NAME  0x50u   // rw, data-port: burst the 4-byte name in/out
#define REG_FILE_CTRL  0x51u   // w: 1=OPEN 2=CLOSE 3=LIST_FIRST 4=LIST_NEXT
#define REG_FILE_STAT  0x52u   // r: 0=OK (open) 1=NOT_FOUND (closed)
#define REG_FILE_SIZE  0x53u   // r: open file length (≤240, u8)
#define REG_FILE_SEEK  0x54u   // w: set read cursor
#define REG_FILE_DATA  0x55u   // r, data-port: next file byte (0xFF past EOF)

// Registers the master bursts -> the ISR must NOT advance the register pointer.
#define IS_DATA_PORT(r) ((r) == REG_REC_DATA || (r) == REG_SET_ADDR \
                      || (r) == REG_FILE_NAME || (r) == REG_FILE_DATA)

typedef struct {
    uint32_t magic;                  // STORE_ENTRY_MAGIC
    uint32_t seq;                    // monotonic; highest per name wins
    uint8_t  name[STORE_NAME_LEN];   // 4-char key (space/zero padded)
    uint8_t  len;
    uint8_t  crc;                    // crc8_autosar(name[4], len, data[len])
    uint8_t  pad[2];
    uint8_t  data[STORE_DATA_MAX];
} store_entry_t;                     // 256 B = 1 flash row
_Static_assert(sizeof(store_entry_t) == 256, "store_entry_t must be exactly one 256-B flash row");

// Per-slot RAM shadow. A slot is in use once a name has been bound to it.
static uint8_t  g_rec_name[STORE_SLOTS][STORE_NAME_LEN]; // 4-char key
static bool     g_rec_used[STORE_SLOTS];                 // slot bound to a name
static uint8_t  g_rec_data[STORE_SLOTS][STORE_DATA_MAX]; // working / shadow copies
static uint8_t  g_rec_len[STORE_SLOTS];
static uint32_t g_rec_seq[STORE_SLOTS];                  // 0 = none committed
static int16_t  g_rec_row[STORE_SLOTS];                  // flash row of current entry, -1 none
static bool     g_rec_dirty[STORE_SLOTS];
static uint16_t g_store_wr_cursor;                       // round-robin write cursor
static volatile int8_t g_commit_pending = -1;            // slot index to commit (ISR -> main loop)
static volatile bool   g_reset_pending;                  // soft-reset request (ISR -> main loop)
static bool            g_setaddr_armed;                  // SET_ADDR magic seen, awaiting the new addr
static uint8_t  g_store_rec_sel;                         // selected SLOT index (0..STORE_SLOTS-1)
static uint8_t  g_store_rec_off;

// FILE bank state — name-keyed read-only access for the I2C master (Pico).
static uint8_t  g_file_name[STORE_NAME_LEN];             // burst-built name key
static uint8_t  g_file_name_idx;                         // 0..3, name burst cursor
static int      g_file_slot = -1;                        // open slot, -1 = closed/not-found
static uint8_t  g_file_off;                              // read cursor into the open file
static uint8_t  g_file_list_i;                           // LIST iterator cursor over slots

// NVM primitives (PAGE=64, ROW=256, ADDR = byte/2). Mirrors flash_storage.c.
static void cs_nvm_wait(void) { while (NVMCTRL->INTFLAG.bit.READY == 0) { /* spin */ } }
static void cs_nvm_erase_row(uint32_t addr) {
    cs_nvm_wait();
    NVMCTRL->ADDR.reg = addr / 2u;
    NVMCTRL->CTRLA.reg = NVMCTRL_CTRLA_CMD_ER | NVMCTRL_CTRLA_CMDEX_KEY;
    cs_nvm_wait();
}
static void cs_nvm_write_page(uint32_t addr, const void *data, uint32_t bytes) {
    cs_nvm_wait();
    NVMCTRL->CTRLA.reg = NVMCTRL_CTRLA_CMD_PBC | NVMCTRL_CTRLA_CMDEX_KEY;
    cs_nvm_wait();
    NVMCTRL->CTRLB.bit.MANW = 1;
    volatile uint32_t *dst = (volatile uint32_t *)(uintptr_t)addr;
    const uint8_t *src = (const uint8_t *)data;
    uint32_t words = (bytes + 3u) / 4u;
    if (words * 4u > STORE_PAGE_SIZE) words = STORE_PAGE_SIZE / 4u;
    for (uint32_t i = 0; i < words; i++) { uint32_t w; memcpy(&w, &src[i*4u], 4u); dst[i] = w; }
    NVMCTRL->ADDR.reg = addr / 2u;
    NVMCTRL->CTRLA.reg = NVMCTRL_CTRLA_CMD_WP | NVMCTRL_CTRLA_CMDEX_KEY;
    cs_nvm_wait();
}

static const store_entry_t *store_row(uint16_t row) {
    return (const store_entry_t *)(uintptr_t)(STORE_BASE + (uint32_t)row * STORE_ROW_SIZE);
}
static uint8_t store_crc(const uint8_t name[STORE_NAME_LEN], uint8_t len, const uint8_t *data) {
    uint8_t c = 0xFFu;
    for (uint8_t i = 0; i < STORE_NAME_LEN; i++) c = crc8_autosar_update(c, name[i]);
    c = crc8_autosar_update(c, len);
    for (uint8_t i = 0; i < len; i++) c = crc8_autosar_update(c, data[i]);
    return (uint8_t)(c ^ 0xFFu);
}
static bool store_entry_valid(const store_entry_t *e) {
    return e->magic == STORE_ENTRY_MAGIC && e->name[0] != 0xFFu && e->len <= STORE_DATA_MAX
        && store_crc(e->name, e->len, e->data) == e->crc;
}

// Slot lookup: index of a USED slot whose name matches, else -1.
static int store_find(const uint8_t name[STORE_NAME_LEN]) {
    for (uint8_t i = 0; i < STORE_SLOTS; i++)
        if (g_rec_used[i] && memcmp(g_rec_name[i], name, STORE_NAME_LEN) == 0) return (int)i;
    return -1;
}
// Get the slot for `name`, allocating a free one if needed. -1 if the bank is full.
static int store_alloc(const uint8_t name[STORE_NAME_LEN]) {
    int s = store_find(name);
    if (s >= 0) return s;
    for (uint8_t i = 0; i < STORE_SLOTS; i++) {
        if (!g_rec_used[i]) {
            g_rec_used[i] = true;
            memcpy(g_rec_name[i], name, STORE_NAME_LEN);
            g_rec_len[i] = 0; g_rec_seq[i] = 0; g_rec_row[i] = -1; g_rec_dirty[i] = false;
            return (int)i;
        }
    }
    return -1;
}
// Convenience: build a 4-byte name from a C string (space-pad / truncate to 4).
static int store_find_str(const char *s4) {
    uint8_t name[STORE_NAME_LEN];
    for (uint8_t i = 0; i < STORE_NAME_LEN; i++) name[i] = (s4[i] != '\0') ? (uint8_t)s4[i] : (uint8_t)' ';
    return store_find(name);
}
static int store_alloc_str(const char *s4) {
    uint8_t name[STORE_NAME_LEN];
    for (uint8_t i = 0; i < STORE_NAME_LEN; i++) name[i] = (s4[i] != '\0') ? (uint8_t)s4[i] : (uint8_t)' ';
    return store_alloc(name);
}

// Boot scan: load the latest valid entry per name into a RAM slot.
static void store_load(void) {
    for (uint8_t i = 0; i < STORE_SLOTS; i++) {
        g_rec_used[i] = false;
        g_rec_seq[i] = 0; g_rec_len[i] = 0; g_rec_row[i] = -1; g_rec_dirty[i] = false;
    }
    for (uint16_t r = 0; r < STORE_ROWS; r++) {
        const store_entry_t *e = store_row(r);
        if (!store_entry_valid(e)) continue;
        int slot = store_alloc(e->name);
        if (slot < 0) continue;                 // RAM-shadow cap: >STORE_SLOTS distinct names on flash
        if (e->seq > g_rec_seq[slot]) {
            g_rec_seq[slot] = e->seq;
            g_rec_len[slot] = e->len;
            g_rec_row[slot] = (int16_t)r;
            memcpy(g_rec_data[slot], e->data, e->len);
        }
    }
    g_store_wr_cursor = 0;
}

static bool store_row_live(uint16_t r) {
    for (uint8_t i = 0; i < STORE_SLOTS; i++)
        if (g_rec_used[i] && g_rec_row[i] == (int16_t)r) return true;
    return false;
}

// Append slot `slot`'s working copy to flash (round-robin non-live row).
static bool store_commit(uint8_t slot) {
    if (slot >= STORE_SLOTS || !g_rec_used[slot]) return false;
    // coalesce: identical to the live entry -> skip the flash write.
    if (g_rec_row[slot] >= 0) {
        const store_entry_t *cur = store_row((uint16_t)g_rec_row[slot]);
        if (cur->len == g_rec_len[slot] && memcmp(cur->data, g_rec_data[slot], g_rec_len[slot]) == 0) {
            g_rec_dirty[slot] = false; return true;
        }
    }
    uint16_t picked = 0xFFFFu;
    for (uint16_t n = 0; n < STORE_ROWS; n++) {
        uint16_t rr = (uint16_t)((g_store_wr_cursor + n) % STORE_ROWS);
        if (!store_row_live(rr)) { picked = rr; break; }
    }
    if (picked == 0xFFFFu) return false;   // impossible: rows > slots
    g_store_wr_cursor = (uint16_t)((picked + 1u) % STORE_ROWS);

    store_entry_t e;
    memset(&e, 0xFF, sizeof e);
    e.magic  = STORE_ENTRY_MAGIC;
    e.seq    = g_rec_seq[slot] + 1u;
    memcpy(e.name, g_rec_name[slot], STORE_NAME_LEN);
    e.len    = g_rec_len[slot];
    e.crc    = store_crc(e.name, e.len, g_rec_data[slot]);
    memcpy(e.data, g_rec_data[slot], e.len);

    uint32_t addr = STORE_BASE + (uint32_t)picked * STORE_ROW_SIZE;
    cs_nvm_erase_row(addr);
    for (uint32_t p = 0; p < STORE_ROW_SIZE; p += STORE_PAGE_SIZE)
        cs_nvm_write_page(addr + p, (const uint8_t *)&e + p, STORE_PAGE_SIZE);

    const store_entry_t *chk = store_row(picked);
    if (!store_entry_valid(chk) || chk->seq != e.seq ||
        memcmp(chk->name, e.name, STORE_NAME_LEN) != 0) return false;
    g_rec_seq[slot] = e.seq;
    g_rec_row[slot] = (int16_t)picked;
    g_rec_dirty[slot] = false;
    return true;
}

// Called from the main superloop: service a pending commit (flash write ~ms) or
// a deferred soft reset (so the master's RESET write is ACKed before we drop).
static void pio_il_tick(void);             // PIO-b gpio interlock eval (defined below)
static void adc_sample_tick(void);         // ADC-a sampler (defined below)
static void adc_il_tick(void);             // ADC-b interlock eval (defined below)
static void adc_sampler_start(void);       // tone-clocked ADC sweep (defined below)
static void adc_sampler_stop(void);
static void mixed_tick(void);              // MIXED-a interlock eval (defined below)

void i2c_store_service(void) {
    if (g_commit_pending >= 0) {
        g_status |= 0x01u;                 // flash-busy
        bool ok = store_commit((uint8_t)g_commit_pending);
        if (!ok) g_status |= 0x02u;        // store-err (sticky until reload)
        g_status &= (uint8_t)~0x01u;       // clear busy
        g_commit_pending = -1;
    }
    if (g_reset_pending) NVIC_SystemReset();
    pio_il_tick();                         // evaluate the gpio interlock (no-op unless armed in PIO)
    adc_sample_tick();                     // ADC interlock eval (sampling runs in the TC3 ISR)
    mixed_tick();                          // ~100 Hz mixed ADC+GPIO interlock (no-op unless MODE_MIXED)
}

// ---- PIO mode (PIO-a): 8-bit GPIO expander, CH0..CH7 = D0-D3, D6-D9 ---------
// MCP23008-style IODIR/GPPU/IPOL/OLAT/GPIO. Pins are claimed only while MODE=PIO
// (entering overrides DAC/SERCOM4 on those pads; leaving sets them safe=inputs).
// Outputs keep INEN on so GPIO-read returns the driven level (no jumper needed).
// PIO-b will layer the gpio interlock (INT on D10 + safe-drive) on top.
#define REG_PIO_IODIR  0x10u
#define REG_PIO_GPPU   0x11u
#define REG_PIO_IPOL   0x12u
#define REG_PIO_GPIO   0x13u
#define REG_PIO_OLAT   0x14u
#define REG_PIO_GPPD   0x17u            // pull-DOWN enable (companion to GPPU pull-up)
#define REG_PIO_OD     0x18u            // RO: open-drain output mask (commission-static)

static const struct { uint8_t group, pin; } g_pio_ch[8] = {
    {0, 2}, {0, 4}, {0, 10}, {0, 11},   // CH0..3 = D0 D1 D2 D3
    {1, 9}, {0,  7}, {0,  5}, {0,  6},   // CH4..7 = D7 D8 D9 D10  (D6 is now INT)
};
static uint8_t g_pio_iodir = 0xFFu;   // power-on default: all inputs (safe)
static uint8_t g_pio_gppu;            // pull-UP enable   (gpmp PULLEN &  OUT)
static uint8_t g_pio_gppd;            // pull-DOWN enable (gpmp PULLEN & ~OUT)
static uint8_t g_pio_od;              // open-drain outputs (gpmp OD): value 1 -> Hi-Z
static uint8_t g_pio_ipol;
static uint8_t g_pio_olat;

// Drive output channel ch to its g_pio_olat value, honoring open-drain (g_pio_od):
// push-pull -> drive the level; open-drain -> value 0 = drive low, value 1 = Hi-Z
// (released), so several chips can wire-OR a shared line.
static void pio_drive_ch_out(uint8_t ch) {
    uint8_t g = g_pio_ch[ch].group, p = g_pio_ch[ch].pin, bit = (uint8_t)(1u << ch);
    uint32_t m = 1u << p;
    if ((g_pio_od & bit) && (g_pio_olat & bit)) {       // open-drain, value 1 -> release
        PORT->Group[g].DIRCLR.reg = m;                  // Hi-Z
    } else {                                            // drive (push-pull, or od value 0 = low)
        if (g_pio_olat & bit) PORT->Group[g].OUTSET.reg = m;
        else                  PORT->Group[g].OUTCLR.reg = m;
        PORT->Group[g].DIRSET.reg = m;
    }
}
static void pio_apply_ch(uint8_t ch) {
    uint8_t g = g_pio_ch[ch].group, p = g_pio_ch[ch].pin, bit = (uint8_t)(1u << ch);
    if (g_pio_iodir & bit) {                            // input
        PORT->Group[g].DIRCLR.reg = (1u << p);
        if (g_pio_gppu & bit) {                         // input + pull-up
            PORT->Group[g].OUTSET.reg = (1u << p);      // OUT=1 selects pull-up direction
            PORT->Group[g].PINCFG[p].reg = PORT_PINCFG_INEN | PORT_PINCFG_PULLEN;
        } else if (g_pio_gppd & bit) {                  // input + pull-down
            PORT->Group[g].OUTCLR.reg = (1u << p);      // OUT=0 selects pull-down direction
            PORT->Group[g].PINCFG[p].reg = PORT_PINCFG_INEN | PORT_PINCFG_PULLEN;
        } else {
            PORT->Group[g].PINCFG[p].reg = PORT_PINCFG_INEN;
        }
    } else {                                            // output (INEN kept for read-back)
        PORT->Group[g].PINCFG[p].reg = PORT_PINCFG_INEN;
        pio_drive_ch_out(ch);                           // od-aware: drive level or Hi-Z
    }
}
static void pio_apply_all(void) { for (uint8_t i = 0; i < 8; i++) pio_apply_ch(i); }
static void pio_update_outputs(void) {                  // OLAT change: just the output pins
    for (uint8_t i = 0; i < 8; i++) {
        if (g_pio_iodir & (1u << i)) continue;
        pio_drive_ch_out(i);                            // od-aware
    }
}
static void pio_safe_all(void) {                        // leaving PIO: all inputs, no pull
    g_pio_iodir = 0xFFu; g_pio_olat = 0u; g_pio_gppu = 0u; g_pio_gppd = 0u; g_pio_od = 0u;
    for (uint8_t i = 0; i < 8; i++) {
        uint8_t g = g_pio_ch[i].group, p = g_pio_ch[i].pin;
        PORT->Group[g].DIRCLR.reg = (1u << p);
        PORT->Group[g].PINCFG[p].reg = PORT_PINCFG_INEN;
    }
}
static uint8_t pio_read_gpio(void) {
    uint8_t v = 0u;
    for (uint8_t i = 0; i < 8; i++) {
        uint8_t g = g_pio_ch[i].group, p = g_pio_ch[i].pin;
        if (PORT->Group[g].IN.reg & (1u << p)) v |= (uint8_t)(1u << i);
    }
    return (uint8_t)(v ^ g_pio_ipol);                   // input polarity invert
}

// Apply the commissioned GPIO power-on pin map ("gpmp") to the channel shadow
// registers; pio_apply_all() then programs the pads. Bit i = channel i.
//   v1 (5 B): [VER=1, DIR(output=1), PULLEN, OUT(level/pull-dir), INTCFG]
//   v2 (6 B): [VER=2, DIR, PULLEN, OUT, OD(open-drain outputs), INTCFG]
// Absent / short / unknown-version -> leave the safe default (all inputs, no pull).
static void gpmp_apply(void) {
    int s = store_find_str("gpmp");
    if (s < 0 || g_rec_len[s] < 5u) return;
    const uint8_t *m = g_rec_data[s];
    if (m[0] == 2u && g_rec_len[s] >= 6u) g_pio_od = m[4];
    else if (m[0] == 1u)                  g_pio_od = 0u;
    else                                  return;          // unknown version
    uint8_t dir = m[1], pullen = m[2], out = m[3];
    g_pio_iodir = (uint8_t)~dir;                          // firmware convention: input = 1
    g_pio_olat  = out;                                    // output drive levels (input bits unused)
    g_pio_gppu  = (uint8_t)(pullen & out);                // pulled input, OUT=1 -> pull-up
    g_pio_gppd  = (uint8_t)(pullen & (uint8_t)~out);      // pulled input, OUT=0 -> pull-down
}

// ---- PIO mode (PIO-b): gpio interlock layered on the expander -----------------
// The interlock_cfg record holds a DSL string (reuses il_parse from the RS-485
// interlock engine). Entering PIO parses it; a tick run from i2c_store_service
// (superloop) evaluates the watches against the channel pins. On trip it LATCHES:
// drives the out_err pins to their safe value (overriding the master's OLAT) and
// asserts INT on D10 (PA06). The master sees the trip in INT_FLAGS (0x04) and
// clears it with a write-1 — that write IS the manual reset: if the fault has
// cleared the interlock re-arms, otherwise the next tick re-trips.
#define REG_PIO_ILSTAT  0x15u           // RO: last il_parse status (0 = OK/armed, 0xFF = no record)
#define REG_PIO_ILSTATE 0x16u           // RO: bit0=tripped bit1=condition-ok bit2=valid/armed
#define PIO_INT_GROUP  1u
#define PIO_INT_PIN    8u               // INT = D6 = PB08 (not one of the 8 channels; D10 is now CH7)

static il_inst_t g_pio_il;
static bool      g_pio_il_valid;        // a DSL parsed OK and has >=1 watch
static bool      g_pio_il_tripped;      // latched trip state
static uint8_t   g_pio_il_pstat = 0xFFu; // il_parse_status_t (0xFF = no record yet)
static volatile uint8_t g_pio_il_state;  // tick snapshot: bit0 tripped, bit1 cond-ok, bit2 valid

static uint8_t pio_phys_read(uint8_t phys) {
    return (PORT->Group[phys >> 5].IN.reg & (1u << (phys & 0x1Fu))) ? 1u : 0u;
}
static void pio_phys_drive(uint8_t phys, uint8_t v) {
    uint8_t grp = phys >> 5, pin = phys & 0x1Fu;
    uint32_t m = 1u << pin;
    // Open-drain if this phys is a channel flagged in g_pio_od: value 1 -> release.
    for (uint8_t ch = 0; ch < 8; ch++) {
        if (g_pio_ch[ch].group == grp && g_pio_ch[ch].pin == pin) {
            if ((g_pio_od & (1u << ch)) && v) { PORT->Group[grp].DIRCLR.reg = m; return; }
            break;
        }
    }
    if (v) PORT->Group[grp].OUTSET.reg = m;
    else   PORT->Group[grp].OUTCLR.reg = m;
    PORT->Group[grp].DIRSET.reg = m;                    // ensure driven (od low / push-pull)
}
// INT (D6) is open-drain, active-low: asserted -> actively drive 0; released ->
// Hi-Z (pin to input), so several chips can wire-OR onto one shared INT line
// (external pull-up holds it high when nobody is tripped).
static void pio_int_assert(bool on) {
    uint32_t m = 1u << PIO_INT_PIN;
    if (on) {
        PORT->Group[PIO_INT_GROUP].OUTCLR.reg = m;   // drive low
        PORT->Group[PIO_INT_GROUP].DIRSET.reg = m;
    } else {
        PORT->Group[PIO_INT_GROUP].DIRCLR.reg = m;   // release: Hi-Z
    }
}

static bool pio_watch_pass(const il_watch_t* w) {
    uint16_t v = pio_phys_read(g_pio_il.inputs[w->input_idx].phys_id);   // digital 0/1
    switch (w->op) {
    case IL_OP_EQ: return v == w->threshold;
    case IL_OP_NE: return v != w->threshold;
    case IL_OP_LT: return v <  w->threshold;
    case IL_OP_GT: return v >  w->threshold;
    case IL_OP_LE: return v <= w->threshold;
    case IL_OP_GE: return v >= w->threshold;
    default:       return true;
    }
}
static void pio_il_drive_safe(void) {                   // force out_err pins to the safe value
    for (uint8_t i = 0; i < g_pio_il.output_count; i++)
        pio_phys_drive(g_pio_il.outputs[i].phys_id, g_pio_il.outputs[i].err_value);
}
// Arm from the interlock_cfg record; set up INT pin (D10 output, deasserted).
static void pio_il_arm(void) {
    g_pio_il_valid = false; g_pio_il_tripped = false;
    PORT->Group[PIO_INT_GROUP].PINCFG[PIO_INT_PIN].reg = PORT_PINCFG_INEN;
    PORT->Group[PIO_INT_GROUP].DIRCLR.reg = (1u << PIO_INT_PIN);   // open-drain INT: Hi-Z idle
    g_pio_il_state = 0u;
    int ils = store_find_str("ilcf");      // interlock-config named slot
    if (ils >= 0 && g_rec_len[ils] > 0u) {
        g_pio_il_pstat = (uint8_t)il_parse((const char*)g_rec_data[ils],
                                           g_rec_len[ils], &g_pio_il, NULL);
        if (g_pio_il_pstat == (uint8_t)IL_PARSE_OK && g_pio_il.watch_count > 0u)
            g_pio_il_valid = true;
    } else {
        g_pio_il_pstat = 0xFFu;             // no interlock_cfg record present
    }
}
static void pio_il_disarm(void) {           // leaving PIO mode
    g_pio_il_valid = false; g_pio_il_tripped = false;
    pio_int_assert(false);
}
static void pio_il_tick(void) {             // run from i2c_store_service (superloop)
    if (g_mode != MODE_PIO || !g_pio_il_valid) return;
    bool wpass[IL_MAX_WATCHES] = {0};
    for (uint8_t i = 0; i < g_pio_il.watch_count; i++)
        wpass[i] = pio_watch_pass(&g_pio_il.watches[i]);
    bool all_pass = il_dnf_result(&g_pio_il, wpass);   // DNF: OR of AND-groups
    g_pio_il_state = (uint8_t)((g_pio_il_tripped ? 1u : 0u) | (all_pass ? 2u : 0u) | 4u);
    if (!g_pio_il_tripped) {
        if (!all_pass) {                                     // a watch failed -> TRIP (latched)
            g_pio_il_tripped = true;
            g_int_flags |= 0x01u;
            pio_il_drive_safe();
            pio_int_assert(true);
        }
    } else {
        pio_il_drive_safe();                // hold the safe outputs each tick (override OLAT)
    }
}
// Master cleared the trip bit in INT_FLAGS -> manual reset. Re-arm; if the fault
// persists the next tick re-trips, otherwise the master's OLAT resumes driving.
static void pio_il_manual_reset(void) {
    if (!g_pio_il_tripped) return;
    g_pio_il_tripped = false;
    pio_int_assert(false);
    pio_update_outputs();                   // restore master OLAT on the freed output pins
}

// PIO mode-bank register access (0x10-0x16), dispatched only while MODE_PIO.
static uint8_t pio_reg_read(uint8_t reg) {
    switch (reg) {
    case REG_PIO_IODIR:   return g_pio_iodir;
    case REG_PIO_GPPU:    return g_pio_gppu;
    case REG_PIO_GPPD:    return g_pio_gppd;
    case REG_PIO_IPOL:    return g_pio_ipol;
    case REG_PIO_GPIO:    return pio_read_gpio();
    case REG_PIO_OLAT:    return g_pio_olat;
    case REG_PIO_OD:      return g_pio_od;
    case REG_PIO_ILSTAT:  return g_pio_il_pstat;
    case REG_PIO_ILSTATE: return g_pio_il_state;
    default:              return 0xFFu;
    }
}
static void pio_reg_write(uint8_t reg, uint8_t val) {
    switch (reg) {
    // DIR/PULL/OD are the fixed wiring description: set once by gpmp at boot,
    // read-only at runtime (the board can't change). Only output VALUES + the
    // read-polarity are writable.
    case REG_PIO_IODIR:
    case REG_PIO_GPPU:
    case REG_PIO_GPPD:
    case REG_PIO_OD:    break;                             // commission-static, ignore writes
    case REG_PIO_IPOL:  g_pio_ipol  = val; break;
    case REG_PIO_GPIO:                                     // GPIO write == OLAT (MCP-style)
    case REG_PIO_OLAT:  g_pio_olat  = val; pio_update_outputs(); break;
    default: break;
    }
}

// ---- ADC mode (ADC-a): free-running 8-ch sampler + tumbling-window stats ------
// DIV16 (3 MHz) ADC, 16x hardware oversample, round-robin keyed off the TC3 tone
// ISR. The throwaway-per-mux + 16x averaging make each conversion ~500 us, so the
// real sweep rate is ~125 Hz/channel (8 channels x throwaway+real). Per channel,
// three tumbling windows of 100/1000/10000 samples -> ~0.8 s / ~8 s / ~80 s at
// that rate. (The "fast/mid/slow" names are by length, not the old 10/1/0.1 Hz
// labels, which assumed an unachieved 1 kHz.) On window-fill they snapshot
// min/max/avg/AC-rms into a readable block and bump a per-window seq id (freshness
// + seqlock). DAC sub-state is constant-only here; ADC-b adds waves.
#define ADC_NCH   8u
#define ADC_NWIN  3u
#define REG_ADC_CH_SEL  0x10u            // w: channel 0..7
#define REG_ADC_WIN_SEL 0x11u            // w: window 0=fast(~0.8s) 1=mid(~8s) 2=slow(~80s)
#define REG_ADC_SEQ     0x12u            // r: u16 window snapshot counter
#define REG_ADC_MIN     0x14u            // r: u16  (block 0x12..0x1B = seq,min,max,avg,rms)
#define REG_ADC_MAX     0x16u
#define REG_ADC_AVG     0x18u
#define REG_ADC_RMS     0x1Au
// Two-tone DDS bench generator. Tone 0 keeps the legacy 0x20/0x21/0x23 slots;
// tone 1 + global offset follow APPLY at 0x26+. type 0=off 1=const 2=sine 3=square.
#define REG_DAC_T1_TYPE 0x20u            // w: tone0 type
#define REG_DAC_T1_AMP  0x21u            // w: u16 tone0 peak amplitude 0..1023
#define REG_DAC_T1_FREQ 0x23u            // w: u16 tone0 Hz (50..2000)
#define REG_DAC_APPLY   0x25u            // w: latch both tones + offset
#define REG_DAC_T2_TYPE 0x26u            // w: tone1 type
#define REG_DAC_T2_AMP  0x27u            // w: u16 tone1 peak amplitude 0..1023
#define REG_DAC_T2_FREQ 0x29u            // w: u16 tone1 Hz (50..2000)
#define REG_DAC_OFFSET  0x2Bu            // w: u16 DC center 0..1023 (default 512)

static const uint8_t  g_adc_ain[ADC_NCH]     = { 4u, 18u, 19u, 2u, 3u, 7u, 5u, 6u }; // D1,D2,D3,D6,D7,D8,D9,D10
static const uint16_t g_adc_win_len[ADC_NWIN] = { 100u, 1000u, 10000u };             // ~0.8/8/80 s @ ~125 Hz/ch

typedef struct { uint16_t min, max; uint32_t sum; uint64_t sumsq; uint16_t count; } adc_accum_t;
typedef struct { uint16_t min, max, avg, rms; } adc_stat_t;
static adc_accum_t g_adc_acc[ADC_NCH][ADC_NWIN];
static adc_stat_t  g_adc_stat[ADC_NCH][ADC_NWIN];
static volatile uint16_t g_adc_latest[ADC_NCH];     // most recent raw sample (instantaneous)
static volatile uint16_t g_adc_seq[ADC_NWIN];
static uint8_t   g_adc_ch_sel, g_adc_win_sel;
static uint8_t   g_dac_type[DAC_NTONES]; // per-tone off/const/sine/square (host shadow)
static uint16_t  g_dac_amp[DAC_NTONES];  // per-tone peak 0..1023
static uint16_t  g_dac_freq[DAC_NTONES]; // per-tone Hz
static uint16_t  g_dac_offset = 512u;    // DC center, latched on APPLY

static uint32_t isqrt32(uint32_t x) {    // integer sqrt for AC-rms
    uint32_t r = 0u, b = 1u << 30;
    while (b > x) b >>= 2;
    while (b) { if (x >= r + b) { x -= r + b; r = (r >> 1) + b; } else { r >>= 1; } b >>= 2; }
    return r;
}

static void adc_mode_setup(void) {
    adc_init();                                              // idempotent (leaves DIV256)
    ADC->CTRLB.reg = ADC_CTRLB_PRESCALER_DIV16 | ADC_CTRLB_RESSEL_16BIT;   // 3 MHz + averaging mode
    while (ADC->STATUS.bit.SYNCBUSY) { }
    ADC->AVGCTRL.reg = ADC_AVGCTRL_SAMPLENUM_16 | ADC_AVGCTRL_ADJRES(4);   // 16x, ADJRES quirk
    ADC->SAMPCTRL.reg = 3u;                                 // short sample-hold (low-Z sources)
    while (ADC->STATUS.bit.SYNCBUSY) { }
    for (uint8_t ch = 0; ch < ADC_NCH; ch++) {
        ain_to_pad_t pad = g_ain_to_pad[g_adc_ain[ch]];
        if (pad.port != 0xFFu) adc_pin_config(pad.port, pad.pin);
    }
    memset(g_adc_acc, 0, sizeof g_adc_acc);
    memset(g_adc_stat, 0, sizeof g_adc_stat);
    for (uint8_t w = 0; w < ADC_NWIN; w++) g_adc_seq[w] = 0u;
    dac_init();                                             // D0 = DAC out (constant)
    PORT->Group[0].PINCFG[2].bit.PMUXEN = 1;                // re-route PA02 to the DAC (PIO mode may
    PORT->Group[0].PMUX[1].bit.PMUXE    = PORT_PMUX_PMUXE_B_Val;  // have left it a GPIO; dac_init is 1-shot)
    DAC->CTRLB.bit.EOEN = 1;
    while (DAC->STATUS.bit.SYNCBUSY) { }
    DAC->DATA.reg = g_dac_offset;
    adc_sampler_start();                                    // launch the IRQ-driven sweep
}

// ---- Tone-ISR-keyed ADC sampler --------------------------------------------
// TC3 (the DAC tone clock) keys the ADC: every TC3 tick calls adc_isr_service(),
// which steps the round-robin sweep IF the previous conversion has completed
// (RESRDY) and otherwise returns immediately. g_adc_running gates it; the
// g_adc_throwaway flag absorbs the per-mux settle (a discarded first conversion).
// ALL result processing (accumulate + window snapshot) happens here, in the ISR.
// That is only safe because the host command path is now fast (libcomm reads
// what's available instead of a fixed 256 B, so a reply returns in ~ms not ~1 s);
// a few hundred microseconds of snapshot work in the ISR no longer stalls CDC.
static volatile uint8_t g_adc_cur_ch;       // channel currently converting
static volatile bool    g_adc_throwaway;    // next RESRDY is the post-mux throwaway
static volatile bool    g_adc_running;      // sampler active

static void adc_accum(uint8_t ch, uint16_t v) {
    if (v > 4095u) v = 4095u;
    g_adc_latest[ch] = v;
    for (uint8_t w = 0; w < ADC_NWIN; w++) {
        adc_accum_t *a = &g_adc_acc[ch][w];
        if (a->count == 0u) { a->min = v; a->max = v; }
        else { if (v < a->min) a->min = v; if (v > a->max) a->max = v; }
        a->sum   += v;
        a->sumsq += (uint32_t)v * (uint32_t)v;
        a->count++;
    }
}

// A full sweep just finished (all channels equal count) -> close any full window.
static void adc_snapshot_full(void) {
    for (uint8_t w = 0; w < ADC_NWIN; w++) {
        if (g_adc_acc[0][w].count < g_adc_win_len[w]) continue;
        for (uint8_t ch = 0; ch < ADC_NCH; ch++) {
            adc_accum_t *a = &g_adc_acc[ch][w];
            uint16_t n = a->count;
            uint32_t avg = n ? (uint32_t)(a->sum / n) : 0u;
            // AC-rms = sqrt(var); var = (n*sumsq - sum^2) / n^2, in u64 from the
            // raw sums so avg truncation doesn't pollute it.
            uint32_t var = 0u;
            if (n) {
                uint64_t nss = (uint64_t)n * a->sumsq;
                uint64_t ss  = (uint64_t)a->sum * a->sum;
                if (nss > ss) var = (uint32_t)((nss - ss) / ((uint64_t)n * n));
            }
            g_adc_stat[ch][w].min = a->min;
            g_adc_stat[ch][w].max = a->max;
            g_adc_stat[ch][w].avg = (uint16_t)avg;
            g_adc_stat[ch][w].rms = (uint16_t)isqrt32(var);
            a->min = 0u; a->max = 0u; a->count = 0u; a->sum = 0u; a->sumsq = 0u;
        }
        g_adc_seq[w]++;
    }
}

// One ADC step, called from TC3_Handler (the tone clock) every tick. Flag-guarded:
// a new conversion is scheduled only after the current one's RESRDY is handled, so
// the tone ISR and the ADC stay in lockstep without ever spinning.
static void adc_isr_service(void) {
    if (!g_adc_running) return;
    if (!ADC->INTFLAG.bit.RESRDY) return;       // still converting -> service on a later tick
    uint16_t v = (uint16_t)ADC->RESULT.reg;
    ADC->INTFLAG.reg = ADC_INTFLAG_RESRDY;
    if (g_adc_throwaway) {                       // discard the post-mux settle conversion
        g_adc_throwaway = false;
        ADC->SWTRIG.bit.START = 1;              // launch the real (settled) conversion
        return;
    }
    uint8_t ch = g_adc_cur_ch;
    adc_accum(ch, v);
    if (ch == ADC_NCH - 1u) adc_snapshot_full();           // sweep complete -> close windows
    ch = (uint8_t)((ch + 1u) % ADC_NCH);                   // advance + kick the next channel
    g_adc_cur_ch    = ch;
    g_adc_throwaway = true;
    ADC->INPUTCTRL.bit.MUXPOS = g_adc_ain[ch];             // throwaway absorbs mux-settle (no spin)
    ADC->SWTRIG.bit.START = 1;
}

// Start the tone-clocked ADC engine. TC3 runs continuously in ADC mode (it is the
// master sample clock); the DAC DDS rides the same ISR — with no waveform the DDS
// just holds g_dac_offset each tick.
static void adc_sampler_start(void) {
    g_adc_cur_ch    = 0u;
    g_adc_throwaway = true;
    ADC->INTFLAG.reg = ADC_INTFLAG_RESRDY;                 // clear stale (we POLL it, no ADC IRQ)
    ADC->INPUTCTRL.bit.MUXPOS = g_adc_ain[0];
    g_adc_running = true;
    ADC->SWTRIG.bit.START = 1;                             // first conversion (throwaway)
    // DDS engine: tones off (DC at offset) until a waveform is applied.
    for (uint8_t t = 0; t < DAC_NTONES; t++) { g_dds.type[t] = DAC_TONE_OFF; g_dds.inc[t] = 0u; g_dds.ph[t] = 0u; }
    g_dds.offset         = (int16_t)g_dac_offset;
    g_dds.isrs_remaining = 0u;                             // infinite
    g_dds.active         = true;
    dac_init();
    tc3_init_once();
    tc3_start_at_period(DAC_TC3_PERIOD);                   // 32 kHz master clock for DAC + ADC
}

static void adc_sampler_stop(void) {
    g_adc_running = false;
    g_dds.active  = false;
    tc3_stop();
}

// Superloop hook: sampling runs in the TC3 ISR now; here we only evaluate the
// interlock against the freshly-computed stats (non-blocking).
static void adc_sample_tick(void) {
    if (g_mode != MODE_ADC) return;
    adc_il_tick();
}

// ---- ADC mode interlock (ADC-b): DNF over ADC streams -> drive D6 -------------
// Reuses il_parse_adc + il_dnf_result. Watch operands are ADC streams (A1 /
// A1_avg_fast); il_input_t carries phys_id=AIN, oversample_exp=stat, sh_cyc=win.
// The sweep already samples every channel, so the interlock just reads the named
// stream each tick, ORs the AND-groups, and drives the out_ok/out_err pin (D6).
#define REG_ADC_ILSTAT  0x1Cu   // r: il_parse status (0=OK/armed, 0xFF=no record)
#define REG_ADC_ILSTATE 0x1Du   // r: bit0 tripped, bit1 cond-ok, bit2 valid/armed
#define ADC_IL_INT_BIT  0x08u   // INT_FLAGS bit owned by the ADC interlock

static il_inst_t g_adc_il;
static bool      g_adc_il_valid, g_adc_il_tripped;
static uint8_t   g_adc_il_pstat = 0xFFu;
static volatile uint8_t g_adc_il_state;

static uint16_t adc_stream_value(const il_input_t* in) {
    int ch = -1;
    for (uint8_t i = 0; i < ADC_NCH; i++) if (g_adc_ain[i] == in->phys_id) { ch = (int)i; break; }
    if (ch < 0) return 0u;
    uint8_t w = in->sh_cyc < ADC_NWIN ? in->sh_cyc : 0u;
    switch (in->oversample_exp) {                            // stat selector
    case 1:  return g_adc_stat[ch][w].avg;
    case 2:  return g_adc_stat[ch][w].min;
    case 3:  return g_adc_stat[ch][w].max;
    case 4:  return g_adc_stat[ch][w].rms;
    default: return g_adc_latest[ch];                        // 0 = instantaneous
    }
}
static bool adc_watch_pass(const il_watch_t* w, uint16_t v) {
    switch (w->op) {
    case IL_OP_EQ: return v == w->threshold;
    case IL_OP_NE: return v != w->threshold;
    case IL_OP_LT: return v <  w->threshold;
    case IL_OP_GT: return v >  w->threshold;
    case IL_OP_LE: return v <= w->threshold;
    case IL_OP_GE: return v >= w->threshold;
    default:       return true;
    }
}
static void adc_il_drive(bool ok) {                          // drive every output pin
    for (uint8_t i = 0; i < g_adc_il.output_count; i++) {
        uint8_t phys = g_adc_il.outputs[i].phys_id, g = phys >> 5, p = phys & 0x1Fu;
        uint32_t m = 1u << p;
        if (ok ? g_adc_il.outputs[i].ok_value : g_adc_il.outputs[i].err_value)
            PORT->Group[g].OUTSET.reg = m;
        else PORT->Group[g].OUTCLR.reg = m;
        PORT->Group[g].DIRSET.reg = m;
    }
}
static void adc_il_arm(void) {
    g_adc_il_valid = false; g_adc_il_tripped = false; g_adc_il_state = 0u;
    int ils = store_find_str("ilcf");
    if (ils < 0) { g_adc_il_pstat = 0xFFu; return; }
    g_adc_il_pstat = (uint8_t)il_parse_adc((const char*)g_rec_data[ils], g_rec_len[ils],
                                           &g_adc_il, NULL);
    if (g_adc_il_pstat != (uint8_t)IL_PARSE_OK || g_adc_il.watch_count == 0u) return;
    for (uint8_t i = 0; i < g_adc_il.output_count; i++) {    // each output -> GPIO output
        uint8_t phys = g_adc_il.outputs[i].phys_id, g = phys >> 5, p = phys & 0x1Fu;
        PORT->Group[g].PINCFG[p].reg = PORT_PINCFG_INEN;
    }
    g_adc_il_valid = true;
    adc_il_drive(true);                                      // start in the OK state
}
static void adc_il_tick(void) {
    if (g_mode != MODE_ADC || !g_adc_il_valid) return;
    bool wpass[IL_MAX_WATCHES];
    for (uint8_t i = 0; i < g_adc_il.watch_count; i++) {
        const il_watch_t* w = &g_adc_il.watches[i];
        wpass[i] = adc_watch_pass(w, adc_stream_value(&g_adc_il.inputs[w->input_idx]));
    }
    bool all_pass = il_dnf_result(&g_adc_il, wpass);
    g_adc_il_state = (uint8_t)((g_adc_il_tripped ? 1u : 0u) | (all_pass ? 2u : 0u) | 4u);
    if (!g_adc_il_tripped && !all_pass) {                    // first failure -> latch trip
        g_adc_il_tripped = true;
        g_int_flags |= ADC_IL_INT_BIT;
    }
    adc_il_drive(!g_adc_il_tripped);                         // ok while safe, err (safe) once tripped
}
// Master cleared the trip bit in INT_FLAGS -> un-latch; the next tick re-trips if
// the condition is still failing, otherwise the output resumes its ok value.
static void adc_il_manual_reset(void) {
    if (!g_adc_il_tripped) return;
    g_adc_il_tripped = false;
    adc_il_drive(true);
}

// Latch both DDS tones + DC offset from the shadow registers. Mode-aware TC3 use:
//  - ADC mode: TC3 runs continuously as the ADC master clock, so just update the
//    DDS params atomically (mask the ISR) and keep it active.
//  - MIXED / standalone bench: TC3 is NOT the sampler clock here, so it runs ONLY
//    for a waveform — any active tone starts TC3; all-off writes a steady DC =
//    offset and stops TC3 (no idle ISR). The DAC is a bench instrument in both
//    modes; MIXED uses it as the controllable A0->A1 source for tests.
// Tones sum and hard-clip around g_dac_offset (default 512).
static void dac_apply(void) {
    if (g_dac_offset > 1023u) g_dac_offset = 1023u;
    bool any = false;
    NVIC_DisableIRQ(TC3_IRQn);                              // mask the ISR during the update
    for (uint8_t t = 0; t < DAC_NTONES; t++) {
        if (g_dac_amp[t] > 1023u) g_dac_amp[t] = 1023u;
        uint32_t f = g_dac_freq[t];
        if (f < DAC_WF_FREQ_MIN) f = DAC_WF_FREQ_MIN;
        if (f > DAC_WF_FREQ_MAX) f = DAC_WF_FREQ_MAX;
        g_dds.type[t] = g_dac_type[t];
        g_dds.amp[t]  = g_dac_amp[t];
        g_dds.inc[t]  = (g_dac_type[t] == DAC_TONE_SINE || g_dac_type[t] == DAC_TONE_SQUARE)
                      ? (uint32_t)(((uint64_t)f << 32) / DAC_FS_HZ) : 0u;   // off/const don't advance
        g_dds.ph[t]   = 0u;
        if (g_dac_type[t] != DAC_TONE_OFF) any = true;
    }
    g_dds.offset = (int16_t)g_dac_offset;

    if (g_mode == MODE_ADC) {
        g_dds.active = true;                               // TC3 is the ADC clock -> always on
        NVIC_EnableIRQ(TC3_IRQn);
        return;
    }
    if (any) {                                             // MIXED/standalone waveform -> run TC3
        g_dds.active = true;
        dac_init();
        tc3_init_once();
        tc3_start_at_period(DAC_TC3_PERIOD);               // re-enables NVIC
    } else {                                               // pure DC -> hold offset, no ISR
        g_dds.active = false;
        tc3_stop();
        DAC->DATA.reg = g_dac_offset;
    }
}

// Shared DAC register access (tone0/1 TYPE/AMP/FREQ at 0x20-0x29, OFFSET 0x2B,
// APPLY 0x25). The DAC is a bench tool in BOTH ADC and MIXED modes, so both banks
// delegate here. Returns true if `reg` is a DAC register.
static bool dac_reg_read(uint8_t reg, uint8_t* out) {
    switch (reg) {
    case REG_DAC_T1_TYPE:   *out = g_dac_type[0]; return true;
    case REG_DAC_T1_AMP:    *out = (uint8_t)g_dac_amp[0]; return true;
    case REG_DAC_T1_AMP+1u: *out = (uint8_t)(g_dac_amp[0] >> 8); return true;
    case REG_DAC_T1_FREQ:   *out = (uint8_t)g_dac_freq[0]; return true;
    case REG_DAC_T1_FREQ+1u:*out = (uint8_t)(g_dac_freq[0] >> 8); return true;
    case REG_DAC_T2_TYPE:   *out = g_dac_type[1]; return true;
    case REG_DAC_T2_AMP:    *out = (uint8_t)g_dac_amp[1]; return true;
    case REG_DAC_T2_AMP+1u: *out = (uint8_t)(g_dac_amp[1] >> 8); return true;
    case REG_DAC_T2_FREQ:   *out = (uint8_t)g_dac_freq[1]; return true;
    case REG_DAC_T2_FREQ+1u:*out = (uint8_t)(g_dac_freq[1] >> 8); return true;
    case REG_DAC_OFFSET:    *out = (uint8_t)g_dac_offset; return true;
    case REG_DAC_OFFSET+1u: *out = (uint8_t)(g_dac_offset >> 8); return true;
    default: return false;
    }
}
static bool dac_reg_write(uint8_t reg, uint8_t val) {
    switch (reg) {
    case REG_DAC_T1_TYPE:   g_dac_type[0] = val; return true;
    case REG_DAC_T1_AMP:    g_dac_amp[0]  = (uint16_t)((g_dac_amp[0]  & 0xFF00u) | val); return true;
    case REG_DAC_T1_AMP+1u: g_dac_amp[0]  = (uint16_t)((g_dac_amp[0]  & 0x00FFu) | ((uint16_t)val << 8)); return true;
    case REG_DAC_T1_FREQ:   g_dac_freq[0] = (uint16_t)((g_dac_freq[0] & 0xFF00u) | val); return true;
    case REG_DAC_T1_FREQ+1u:g_dac_freq[0] = (uint16_t)((g_dac_freq[0] & 0x00FFu) | ((uint16_t)val << 8)); return true;
    case REG_DAC_T2_TYPE:   g_dac_type[1] = val; return true;
    case REG_DAC_T2_AMP:    g_dac_amp[1]  = (uint16_t)((g_dac_amp[1]  & 0xFF00u) | val); return true;
    case REG_DAC_T2_AMP+1u: g_dac_amp[1]  = (uint16_t)((g_dac_amp[1]  & 0x00FFu) | ((uint16_t)val << 8)); return true;
    case REG_DAC_T2_FREQ:   g_dac_freq[1] = (uint16_t)((g_dac_freq[1] & 0xFF00u) | val); return true;
    case REG_DAC_T2_FREQ+1u:g_dac_freq[1] = (uint16_t)((g_dac_freq[1] & 0x00FFu) | ((uint16_t)val << 8)); return true;
    case REG_DAC_OFFSET:    g_dac_offset  = (uint16_t)((g_dac_offset  & 0xFF00u) | val); return true;
    case REG_DAC_OFFSET+1u: g_dac_offset  = (uint16_t)((g_dac_offset  & 0x00FFu) | ((uint16_t)val << 8)); return true;
    case REG_DAC_APPLY:     dac_apply(); return true;
    default: return false;
    }
}

// ADC mode-bank register access (0x10-0x2C), dispatched only while MODE_ADC.
static uint8_t adc_reg_read(uint8_t reg) {
    if (reg >= REG_ADC_SEQ && reg <= REG_ADC_RMS + 1u
        && g_adc_ch_sel < ADC_NCH && g_adc_win_sel < ADC_NWIN) {
        uint8_t off = (uint8_t)(reg - REG_ADC_SEQ);          // 0..9 -> seq,min,max,avg,rms (5x u16)
        const adc_stat_t *st = &g_adc_stat[g_adc_ch_sel][g_adc_win_sel];
        uint16_t blk[5] = { g_adc_seq[g_adc_win_sel], st->min, st->max, st->avg, st->rms };
        uint16_t v = blk[off >> 1];
        return (off & 1u) ? (uint8_t)(v >> 8) : (uint8_t)v;
    }
    switch (reg) {
    case REG_ADC_CH_SEL:  return g_adc_ch_sel;
    case REG_ADC_WIN_SEL: return g_adc_win_sel;
    case REG_ADC_ILSTAT:  return g_adc_il_pstat;
    case REG_ADC_ILSTATE: return g_adc_il_state;
    default: { uint8_t v = 0xFFu; dac_reg_read(reg, &v); return v; }   // DAC bench regs
    }
}
static void adc_reg_write(uint8_t reg, uint8_t val) {
    switch (reg) {
    case REG_ADC_CH_SEL:  if (val < ADC_NCH)  g_adc_ch_sel  = val; break;
    case REG_ADC_WIN_SEL: if (val < ADC_NWIN) g_adc_win_sel = val; break;
    default: dac_reg_write(reg, val); break;                 // DAC bench regs
    }
}

// ---- MIXED mode (MIXED-a): mixed ADC+GPIO interlock at ~100 Hz ----------------
// Like PIO-b, the interlock_cfg record holds a DSL string parsed by il_parse —
// SAME record + parser the PIO interlock uses. The declared inputs ARE the active
// channels: ADC inputs are single-shot 16x-oversampled (hal_pin_read_adc), GPIO
// inputs are debounced via a shift register (mirrors eval_slot in samd21_interlocks.c).
// A tick run from i2c_store_service (superloop, gated to ~100 Hz) reads the inputs,
// evaluates every watch against the latest (ADC) / debounced (GPIO) value, and on
// trip LATCHES: drives the out_err pins safe + asserts INT on D6 (PB08, the same
// pin the PIO interlock uses). The master sees the trip in INT_FLAGS (0x04, the
// MIXED bit) and clears it with a write-1 — that write IS the manual reset.
//
// Pins are claimed/released through the HAL pin API exactly like the framework's
// claim_inst_pins(), but under a DEDICATED slot (MIXED_IL_SLOT) so MIXED's claims
// never collide with the RS-485 interlock framework's slots 0..INTERLOCK_MAX_SLOTS-1.
#define REG_MIXED_CH_SEL    0x10u   // w: input index 0..input_count-1 to inspect
#define REG_MIXED_CH_ROLE   0x11u   // r: selected input role 0=unused 1=GPIO 2=ADC
#define REG_MIXED_ADC_VAL   0x12u   // r: u16 latest 16x value of selected input (0x12 lo, 0x13 hi)
#define REG_MIXED_GPIO_RAW  0x14u   // r: u8 raw-level bitmap (bit i = input i if GPIO)
#define REG_MIXED_GPIO_DEB  0x15u   // r: u8 debounced bitmap
#define REG_MIXED_ILSTATE   0x16u   // r: bit0 tripped, bit1 cond-ok, bit2 valid/armed
#define REG_MIXED_ILSTAT    0x17u   // r: last il_parse status (0=OK/armed, 0xFF=no record)
#define MIXED_IL_INT_BIT    0x02u   // INT_FLAGS bit owned by the MIXED interlock

// HAL pin slot owned by MIXED. The RS-485 interlock framework uses slots
// 0..INTERLOCK_MAX_SLOTS-1 (currently 0,1); the HAL slot_mask is an 8-bit field,
// so slot 7 (bit 0x80) is the highest addressable and is guaranteed disjoint from
// any framework slot. MIXED drives its own outputs directly (pio_phys_drive) and
// never calls hal_pin_drive_outputs(), so there is no cross-slot drive interaction
// either — the claim is purely for single-owner reservation + auto-release on exit.
#define MIXED_IL_SLOT       7u
_Static_assert(MIXED_IL_SLOT >= INTERLOCK_MAX_SLOTS, "MIXED_IL_SLOT collides with framework slots");

static il_inst_t g_mixed_il;
static bool      g_mixed_il_valid;          // a DSL parsed OK and has >=1 watch
static bool      g_mixed_il_tripped;        // latched trip state
static uint8_t   g_mixed_il_pstat = 0xFFu;  // il_parse_status_t (0xFF = no record yet)
static volatile uint8_t g_mixed_il_state;   // tick snapshot: bit0 tripped, bit1 cond-ok, bit2 valid
static uint8_t   g_mixed_ch_sel;            // input index selected for the reg-read window

// Per-input debounce shift register + debounced level (GPIO inputs only). Mirrors
// g_input_shift_reg / g_input_debounced in samd21_interlocks.c, sized IL_MAX_INPUTS.
static uint16_t  g_mixed_sr[IL_MAX_INPUTS];
static uint8_t   g_mixed_deb[IL_MAX_INPUTS];
// Cached latest readings for the reg window (no re-reading hardware on I2C reads).
static uint16_t  g_mixed_adc_val[IL_MAX_INPUTS];   // latest 16x value per ADC input
static uint8_t   g_mixed_gpio_raw;                 // raw-level bitmap (bit i = input i)
static uint8_t   g_mixed_gpio_deb;                 // debounced bitmap (bit i = input i)
static uint32_t  g_mixed_next_ms;                  // ~100 Hz tick gate

// Compare one watch against a freshly-read input value. Same op switch as
// eval_slot()/pio_watch_pass(); `v` is the debounced level (GPIO) or 16x ADC
// value (ADC), already selected by the caller.
static bool mixed_watch_pass(const il_watch_t* w, uint16_t v) {
    switch ((il_compare_op_t)w->op) {
    case IL_OP_EQ: return v == w->threshold;
    case IL_OP_NE: return v != w->threshold;
    case IL_OP_LT: return v <  w->threshold;
    case IL_OP_GT: return v >  w->threshold;
    case IL_OP_LE: return v <= w->threshold;
    case IL_OP_GE: return v >= w->threshold;
    default:       return true;
    }
}

// Force out_err pins to their safe value (override whatever drove them before).
static void mixed_il_drive_safe(void) {
    for (uint8_t i = 0; i < g_mixed_il.output_count; i++)
        pio_phys_drive(g_mixed_il.outputs[i].phys_id, g_mixed_il.outputs[i].err_value);
}

// Claim each declared input/output pin under MIXED_IL_SLOT, following
// claim_inst_pins() in samd21_interlocks.c (ADC via hal_pin_claim_adc, GPIO via
// hal_pin_claim, outputs via hal_pin_claim_output). On any failure releases the
// whole slot and reports the claim status. VIRTUAL inputs (no physical pin) are
// skipped, matching the framework.
static hal_pin_claim_status_t mixed_claim_pins(void) {
    hal_pin_claim_status_t cs = HAL_PIN_CLAIM_OK;
    for (uint8_t i = 0; i < g_mixed_il.input_count; i++) {
        il_pin_mode_t m = (il_pin_mode_t)g_mixed_il.inputs[i].mode;
        if (m == IL_PIN_MODE_VIRTUAL) continue;            // no pin to claim
        if (m == IL_PIN_MODE_ADC) {
            cs = hal_pin_claim_adc(g_mixed_il.inputs[i].phys_id, MIXED_IL_SLOT,
                                   g_mixed_il.inputs[i].oversample_exp,
                                   g_mixed_il.inputs[i].sh_cyc);
        } else {
            hal_pin_mode_t hm;
            switch (m) {
            case IL_PIN_MODE_IN:    hm = HAL_PIN_MODE_GPIO_IN;    break;
            case IL_PIN_MODE_IN_PU: hm = HAL_PIN_MODE_GPIO_IN_PU; break;
            case IL_PIN_MODE_IN_PD: hm = HAL_PIN_MODE_GPIO_IN_PD; break;
            default: cs = HAL_PIN_CLAIM_BAD_MODE; goto fail;
            }
            cs = hal_pin_claim(g_mixed_il.inputs[i].phys_id, MIXED_IL_SLOT, hm);
        }
        if (cs != HAL_PIN_CLAIM_OK) goto fail;
    }
    for (uint8_t i = 0; i < g_mixed_il.output_count; i++) {
        cs = hal_pin_claim_output(g_mixed_il.outputs[i].phys_id, MIXED_IL_SLOT,
                                  g_mixed_il.outputs[i].ok_value,
                                  g_mixed_il.outputs[i].err_value);
        if (cs != HAL_PIN_CLAIM_OK) goto fail;
    }
    return HAL_PIN_CLAIM_OK;
fail:
    hal_pin_release_slot(MIXED_IL_SLOT);
    return cs;
}

// Arm from the interlock_cfg record; set up the D6/PB08 INT pin (output, deasserted)
// and claim the declared pins. Mirrors pio_il_arm().
static void mixed_il_arm(void) {
    g_mixed_il_valid = false; g_mixed_il_tripped = false;
    // INT pin = D6 = PB08 (same as PIO): drive low (deasserted), keep INEN for read-back.
    PORT->Group[PIO_INT_GROUP].PINCFG[PIO_INT_PIN].reg = PORT_PINCFG_INEN;
    PORT->Group[PIO_INT_GROUP].DIRCLR.reg = (1u << PIO_INT_PIN);   // open-drain INT: Hi-Z idle
    g_mixed_il_state = 0u;
    // Fresh arm: clear debounce + cached readings so no stale edges/values leak in.
    memset(g_mixed_sr,  0, sizeof g_mixed_sr);
    memset(g_mixed_deb, 0, sizeof g_mixed_deb);
    memset(g_mixed_adc_val, 0, sizeof g_mixed_adc_val);
    g_mixed_gpio_raw = 0u; g_mixed_gpio_deb = 0u;
    int ils = store_find_str("ilcf");      // interlock-config named slot
    if (ils >= 0 && g_rec_len[ils] > 0u) {
        g_mixed_il_pstat = (uint8_t)il_parse((const char*)g_rec_data[ils],
                                             g_rec_len[ils], &g_mixed_il, NULL);
        if (g_mixed_il_pstat == (uint8_t)IL_PARSE_OK && g_mixed_il.watch_count > 0u) {
            // Claim the declared pins; only mark valid if every claim succeeds.
            if (mixed_claim_pins() == HAL_PIN_CLAIM_OK)
                g_mixed_il_valid = true;
        }
    } else {
        g_mixed_il_pstat = 0xFFu;            // no interlock_cfg record present
    }
    g_mixed_next_ms = board_millis();
}

// Leaving MIXED mode: drop the latch, deassert INT, release the HAL claims.
static void mixed_il_disarm(void) {
    g_mixed_il_valid = false; g_mixed_il_tripped = false;
    pio_int_assert(false);                  // deassert INT on D6 (shared helper)
    hal_pin_release_slot(MIXED_IL_SLOT);    // free every pin MIXED claimed -> safe
}

// Master cleared the MIXED trip bit in INT_FLAGS -> manual reset. Re-arm; if the
// fault persists the next tick re-trips, otherwise the outputs resume their ok value.
static void mixed_il_manual_reset(void) {
    if (!g_mixed_il_tripped) return;
    g_mixed_il_tripped = false;
    pio_int_assert(false);
    // Restore each output to its ok value (drop the safe override).
    for (uint8_t i = 0; i < g_mixed_il.output_count; i++)
        pio_phys_drive(g_mixed_il.outputs[i].phys_id, g_mixed_il.outputs[i].ok_value);
}

// Run from i2c_store_service (superloop), gated to ~100 Hz (10 ms) like adc_sample_tick.
// Reads every input (ADC = single-shot 16x; GPIO = raw + debounce shift register),
// caches the values for the reg window, evaluates all watches, and latches on trip.
static void mixed_tick(void) {
    if (g_mode != MODE_MIXED || !g_mixed_il_valid) return;
    uint32_t now = board_millis();
    if ((int32_t)(now - g_mixed_next_ms) < 0) return;        // ~100 Hz gate (10 ms)
    g_mixed_next_ms = now + 10u;

    // Snapshot each input. ADC values feed the watch directly; GPIO levels are
    // debounced through a per-input shift register (Schmitt-on-time), mirroring
    // eval_slot() in samd21_interlocks.c.
    uint16_t vals[IL_MAX_INPUTS] = {0};
    uint8_t  raw_bm = 0u, deb_bm = 0u;
    for (uint8_t i = 0; i < g_mixed_il.input_count; i++) {
        il_pin_mode_t m = (il_pin_mode_t)g_mixed_il.inputs[i].mode;
        if (m == IL_PIN_MODE_ADC) {
            uint16_t v = hal_pin_read_adc(g_mixed_il.inputs[i].phys_id);
            g_mixed_adc_val[i] = v;
            vals[i] = v;
        } else {
            uint16_t raw = (uint16_t)(hal_pin_read(g_mixed_il.inputs[i].phys_id) & 1u);
            uint8_t depth = g_mixed_il.inputs[i].debounce_depth;
            uint16_t level = raw;
            if (depth >= 2u) {
                uint16_t mask = (uint16_t)((1u << depth) - 1u);
                g_mixed_sr[i] = (uint16_t)(((g_mixed_sr[i] << 1) | (raw & 1u)) & mask);
                if      (g_mixed_sr[i] == mask) g_mixed_deb[i] = 1u;
                else if (g_mixed_sr[i] == 0u)   g_mixed_deb[i] = 0u;
                level = g_mixed_deb[i];
            } else {
                g_mixed_deb[i] = (uint8_t)raw;               // depth<2 -> pass-through
            }
            if (raw)   raw_bm |= (uint8_t)(1u << i);
            if (level) deb_bm |= (uint8_t)(1u << i);
            vals[i] = level;
        }
    }
    g_mixed_gpio_raw = raw_bm;
    g_mixed_gpio_deb = deb_bm;

    // Evaluate every watch against the (debounced/latest) input values, then
    // aggregate as DNF (OR of AND-groups).
    bool wpass[IL_MAX_WATCHES] = {0};
    for (uint8_t i = 0; i < g_mixed_il.watch_count; i++) {
        const il_watch_t* w = &g_mixed_il.watches[i];
        wpass[i] = mixed_watch_pass(w, vals[w->input_idx]);
    }
    bool all_pass = il_dnf_result(&g_mixed_il, wpass);
    g_mixed_il_state = (uint8_t)((g_mixed_il_tripped ? 1u : 0u) | (all_pass ? 2u : 0u) | 4u);

    if (!g_mixed_il_tripped) {
        if (!all_pass) {                                     // a watch failed -> TRIP (latched)
            g_mixed_il_tripped = true;
            g_int_flags |= MIXED_IL_INT_BIT;
            mixed_il_drive_safe();
            pio_int_assert(true);
        }
    } else {
        mixed_il_drive_safe();              // hold the safe outputs each tick
    }
}

// MIXED mode-bank register access (0x10-0x17), dispatched only while MODE_MIXED.
static uint8_t mixed_reg_read(uint8_t reg) {
    bool sel_ok = g_mixed_il_valid && (g_mixed_ch_sel < g_mixed_il.input_count);
    il_pin_mode_t selm = sel_ok ? (il_pin_mode_t)g_mixed_il.inputs[g_mixed_ch_sel].mode
                                : IL_PIN_MODE_VIRTUAL;
    switch (reg) {
    case REG_MIXED_CH_SEL:  return g_mixed_ch_sel;
    case REG_MIXED_CH_ROLE:
        if (!sel_ok) return 0u;
        if (selm == IL_PIN_MODE_ADC) return 2u;
        if (selm == IL_PIN_MODE_IN || selm == IL_PIN_MODE_IN_PU || selm == IL_PIN_MODE_IN_PD)
            return 1u;
        return 0u;                                           // VIRTUAL / out-of-range
    case REG_MIXED_ADC_VAL:                                   // lo byte
        return (sel_ok && selm == IL_PIN_MODE_ADC) ? (uint8_t)g_mixed_adc_val[g_mixed_ch_sel] : 0u;
    case REG_MIXED_ADC_VAL + 1u:                              // hi byte
        return (sel_ok && selm == IL_PIN_MODE_ADC) ? (uint8_t)(g_mixed_adc_val[g_mixed_ch_sel] >> 8) : 0u;
    case REG_MIXED_GPIO_RAW: return g_mixed_gpio_raw;
    case REG_MIXED_GPIO_DEB: return g_mixed_gpio_deb;
    case REG_MIXED_ILSTATE:  return g_mixed_il_state;
    case REG_MIXED_ILSTAT:   return g_mixed_il_pstat;
    default: { uint8_t v = 0xFFu; dac_reg_read(reg, &v); return v; }   // DAC bench regs (A0->A1 source)
    }
}
static void mixed_reg_write(uint8_t reg, uint8_t val) {
    switch (reg) {
    case REG_MIXED_CH_SEL: if (val < IL_MAX_INPUTS) g_mixed_ch_sel = val; break;
    default: dac_reg_write(reg, val); break;                 // DAC bench regs (test source)
    }
}

// ============================================================================
// SERVO mode — 9 RC servos on a SOFTWARE common-rising-edge frame.
//
// The 9 servo pins do NOT map to TCC waveform outputs, so we can't use hardware
// PWM. Instead a free hardware timer (TC4) generates a 50 Hz frame: at the frame
// boundary we raise ALL enabled servo pins together (one OUTSET per port group),
// then drop each pin individually at its programmed pulse width. Pure output —
// no interlock, no INT (D6/PB08 is the 9th servo channel here, not the INT pin).
//
// Timebase: TC4 off GCLK0 (48 MHz) with PRESCALER_DIV64 -> 750 kHz tick clock =
// 1.333 µs/tick. No standard divider gives exactly 1 MHz, so we run at 750 kHz
// and convert microseconds to ticks as ticks = us * 3 / 4 (exact for the 4/3
// ratio). Resolution is therefore 1.333 µs/tick — finer than an RC servo's
// effective resolution, so harmless. Frame top = 20000 µs -> 15000 ticks.
//
// CC0 is the MFRQ period (counter auto-resets at frame top -> 50 Hz); the MC0
// interrupt is the frame boundary (raise). CC1 is reprogrammed to the next drop
// time; the MC1 interrupt fires on each drop and reschedules itself to the next
// (strictly larger) event time, or disables MC1 until the next frame.
//
// Mode-bank registers (0x10-0x14), dispatched only while MODE_SERVO. Unlike the
// other modes SERVO needs NO i2c_store_service tick — it is purely ISR-driven.
//   0x10 CH_SEL   (w)   channel 0..8 selected for the WIDTH window
//   0x11 WIDTH_L  (rw)  pulse width µs lo byte (staged)
//   0x12 WIDTH_H  (rw)  pulse width µs hi byte; WRITE latches+clamps+servo_build()
//   0x13 ENABLE_L (rw)  enable mask bits 0..7 (CH0..CH7)
//   0x14 ENABLE_H (rw)  enable mask bit 0 = CH8; write -> servo_build()
// ============================================================================
#define SERVO_CH_COUNT      9u
#define SERVO_FRAME_US      20000u           // 50 Hz frame
#define SERVO_US_MIN        500u             // pulse clamp lo
#define SERVO_US_MAX        2500u            // pulse clamp hi
#define SERVO_US_DEFAULT    1500u            // center
#define SERVO_ENABLE_MASK   0x1FFu           // 9 valid channel bits

#define REG_SERVO_CH_SEL    0x10u
#define REG_SERVO_WIDTH_L   0x11u
#define REG_SERVO_WIDTH_H   0x12u
#define REG_SERVO_ENABLE_L  0x13u
#define REG_SERVO_ENABLE_H  0x14u

// us -> TC4 ticks at 750 kHz (1.333 µs/tick). Exact for the 3/4 ratio.
#define SERVO_US_TO_TICKS(us)  ((uint16_t)((uint32_t)(us) * 3u / 4u))

// Channel index -> (port group, pin). 7 on PORT A (group 0), 2 on PORT B (group 1).
// CH0 D0 PA02  CH1 D1 PA04  CH2 D2 PA10  CH3 D3 PA11  CH4 D7 PB09
// CH5 D8 PA07  CH6 D9 PA05  CH7 D10 PA06  CH8 D6 PB08
typedef struct { uint8_t group; uint8_t pin; } servo_pad_t;
static const servo_pad_t g_servo_pad[SERVO_CH_COUNT] = {
    {0,  2}, {0,  4}, {0, 10}, {0, 11}, {1,  9},
    {0,  7}, {0,  5}, {0,  6}, {1,  8},
};

static uint16_t g_servo_us[SERVO_CH_COUNT] = {
    SERVO_US_DEFAULT, SERVO_US_DEFAULT, SERVO_US_DEFAULT,
    SERVO_US_DEFAULT, SERVO_US_DEFAULT, SERVO_US_DEFAULT,
    SERVO_US_DEFAULT, SERVO_US_DEFAULT, SERVO_US_DEFAULT,
};
static uint16_t g_servo_enable;              // 9-bit enable mask (default 0 -> silent)
static uint8_t  g_servo_ch_sel;              // selected channel for the WIDTH window
static uint16_t g_servo_width_stage;         // WIDTH_L/H accumulator (latched on WIDTH_H write)

// Built schedule (recomputed by servo_build()). The ISR consumes raise masks at
// the frame boundary and walks the sorted/merged drop events.
static uint32_t g_servo_raise_a;             // PORT A bits of all enabled servos
static uint32_t g_servo_raise_b;             // PORT B bits of all enabled servos
static struct { uint16_t t; uint32_t clr_a, clr_b; } g_servo_evt[SERVO_CH_COUNT];
static uint8_t  g_servo_evt_n;               // number of drop events (distinct widths)
static volatile uint8_t g_servo_evt_idx;     // ISR walk cursor

static bool g_servo_tc_initialized;          // TC4 GCLK/APBC wired once

// ---- TC4 timebase (mirrors the TC3 DAC engine: MFRQ CC0=TOP, SYNCBUSY spins) --
static void servo_tc_init_once(void) {
    if (g_servo_tc_initialized) return;
    PM->APBCMASK.reg |= PM_APBCMASK_TC4;
    GCLK->CLKCTRL.reg = (uint16_t)(GCLK_CLKCTRL_ID(TC4_GCLK_ID)   // TC4_GCLK_ID == TC5_GCLK_ID (28)
                                 | GCLK_CLKCTRL_GEN_GCLK0
                                 | GCLK_CLKCTRL_CLKEN);
    while (GCLK->STATUS.bit.SYNCBUSY) { /* spin */ }
    g_servo_tc_initialized = true;
}

static void servo_tc_stop(void) {
    NVIC_DisableIRQ(TC4_IRQn);
    TC4->COUNT16.INTENCLR.reg = TC_INTENCLR_MC0 | TC_INTENCLR_MC1;
    TC4->COUNT16.CTRLA.bit.ENABLE = 0;
    while (TC4->COUNT16.STATUS.bit.SYNCBUSY) { /* spin */ }
}

static void servo_tc_start(void) {
    // Reset.
    TC4->COUNT16.CTRLA.bit.SWRST = 1;
    while (TC4->COUNT16.STATUS.bit.SYNCBUSY) { /* spin */ }
    while (TC4->COUNT16.CTRLA.bit.SWRST)     { /* spin */ }

    // 16-bit count, MFRQ wavegen (CC0=TOP and resets the counter), /64 prescaler.
    TC4->COUNT16.CTRLA.reg = TC_CTRLA_MODE_COUNT16
                           | TC_CTRLA_WAVEGEN_MFRQ
                           | TC_CTRLA_PRESCALER_DIV64;
    TC4->COUNT16.CC[0].reg = SERVO_US_TO_TICKS(SERVO_FRAME_US);   // 15000 ticks = 20 ms
    TC4->COUNT16.CC[1].reg = 0;
    while (TC4->COUNT16.STATUS.bit.SYNCBUSY) { /* spin */ }

    TC4->COUNT16.INTFLAG.reg = TC_INTFLAG_MC0 | TC_INTFLAG_MC1;   // clear stale
    TC4->COUNT16.INTENSET.reg = TC_INTENSET_MC0;                  // frame boundary; MC1 armed per-frame
    NVIC_EnableIRQ(TC4_IRQn);

    TC4->COUNT16.CTRLA.bit.ENABLE = 1;
    while (TC4->COUNT16.STATUS.bit.SYNCBUSY) { /* spin */ }
}

// Recompute the raise masks + sorted/merged drop events from the current
// enable mask + per-channel widths. Servos with EQUAL width merge into one drop
// event (their port bits OR'd into clr_a/clr_b). Updates are made coherent vs the
// ISR by masking the TC4 IRQ around the swap (like the DAC engine masks TC3).
static void servo_build(void) {
    uint32_t raise_a = 0u, raise_b = 0u;
    struct { uint16_t t; uint32_t clr_a, clr_b; } evt[SERVO_CH_COUNT];
    uint8_t n = 0u;

    for (uint8_t ch = 0; ch < SERVO_CH_COUNT; ch++) {
        if (!(g_servo_enable & (1u << ch))) continue;
        uint16_t us = g_servo_us[ch];
        if (us < SERVO_US_MIN) us = SERVO_US_MIN;
        if (us > SERVO_US_MAX) us = SERVO_US_MAX;
        uint16_t t = SERVO_US_TO_TICKS(us);
        uint32_t bit = (1u << g_servo_pad[ch].pin);
        if (g_servo_pad[ch].group == 0u) raise_a |= bit; else raise_b |= bit;

        // Insertion-sort by tick-time, merging equal times into one event.
        uint8_t i = 0;
        while (i < n && evt[i].t < t) i++;
        if (i < n && evt[i].t == t) {
            if (g_servo_pad[ch].group == 0u) evt[i].clr_a |= bit; else evt[i].clr_b |= bit;
        } else {
            for (uint8_t j = n; j > i; j--) evt[j] = evt[j - 1u];
            evt[i].t = t;
            evt[i].clr_a = (g_servo_pad[ch].group == 0u) ? bit : 0u;
            evt[i].clr_b = (g_servo_pad[ch].group == 0u) ? 0u : bit;
            n++;
        }
    }

    // Install atomically vs the ISR: mask TC4, swap, restore.
    NVIC_DisableIRQ(TC4_IRQn);
    g_servo_raise_a = raise_a;
    g_servo_raise_b = raise_b;
    for (uint8_t i = 0; i < n; i++) {
        g_servo_evt[i].t     = evt[i].t;
        g_servo_evt[i].clr_a = evt[i].clr_a;
        g_servo_evt[i].clr_b = evt[i].clr_b;
    }
    g_servo_evt_n   = n;
    g_servo_evt_idx = 0u;
    if (g_servo_tc_initialized) NVIC_EnableIRQ(TC4_IRQn);
}

// Configure all 9 servo pins as GPIO outputs driven low. Mirrors cmd_gpio_config's
// output path (clear PMUXEN, clear INEN/PULLEN, OUTCLR, DIRSET).
static void servo_pins_output_low(void) {
    for (uint8_t ch = 0; ch < SERVO_CH_COUNT; ch++) {
        uint8_t g = g_servo_pad[ch].group, p = g_servo_pad[ch].pin;
        uint32_t m = (1u << p);
        PORT->Group[g].PINCFG[p].bit.PMUXEN = 0;
        PORT->Group[g].PINCFG[p].bit.INEN   = 0;
        PORT->Group[g].PINCFG[p].bit.PULLEN = 0;
        PORT->Group[g].OUTCLR.reg = m;
        PORT->Group[g].DIRSET.reg = m;
    }
}

// Entering MODE_SERVO: pins -> GPIO outputs low, build the schedule, start TC4.
static void servo_enter(void) {
    servo_tc_init_once();
    servo_pins_output_low();
    servo_build();
    servo_tc_start();
}

// Leaving MODE_SERVO: stop TC4 (masks IRQ), drive all 9 pins low (safe).
static void servo_exit(void) {
    servo_tc_stop();
    for (uint8_t ch = 0; ch < SERVO_CH_COUNT; ch++)
        PORT->Group[g_servo_pad[ch].group].OUTCLR.reg = (1u << g_servo_pad[ch].pin);
}

// TC4 ISR. MC0 = frame boundary (raise all enabled, schedule first drop);
// MC1 = drop (clear this event's pins, reschedule to the next larger time).
// Bounded + fast: only OUTSET/OUTCLR writes + a short CC1 SYNCBUSY spin.
void TC4_Handler(void) {
    if (TC4->COUNT16.INTFLAG.bit.MC0) {                  // frame boundary (CC0 wrap)
        TC4->COUNT16.INTFLAG.reg = TC_INTFLAG_MC0;
        PORT->Group[0].OUTSET.reg = g_servo_raise_a;     // raise all enabled servos together
        PORT->Group[1].OUTSET.reg = g_servo_raise_b;
        g_servo_evt_idx = 0u;
        if (g_servo_evt_n > 0u) {
            TC4->COUNT16.CC[1].reg = g_servo_evt[0].t;
            while (TC4->COUNT16.STATUS.bit.SYNCBUSY) { /* spin */ }
            TC4->COUNT16.INTENSET.reg = TC_INTENSET_MC1;
        } else {
            TC4->COUNT16.INTENCLR.reg = TC_INTENCLR_MC1;
        }
        return;
    }
    if (TC4->COUNT16.INTFLAG.bit.MC1) {                  // a drop time was reached
        TC4->COUNT16.INTFLAG.reg = TC_INTFLAG_MC1;
        uint8_t idx = g_servo_evt_idx;
        PORT->Group[0].OUTCLR.reg = g_servo_evt[idx].clr_a;
        PORT->Group[1].OUTCLR.reg = g_servo_evt[idx].clr_b;
        idx++;
        g_servo_evt_idx = idx;
        if (idx < g_servo_evt_n) {                       // next time is strictly larger -> safe
            TC4->COUNT16.CC[1].reg = g_servo_evt[idx].t;
            while (TC4->COUNT16.STATUS.bit.SYNCBUSY) { /* spin */ }
        } else {
            TC4->COUNT16.INTENCLR.reg = TC_INTENCLR_MC1; // done this frame
        }
        return;
    }
}

// SERVO mode-bank register access (0x10-0x14), dispatched only while MODE_SERVO.
static uint8_t servo_reg_read(uint8_t reg) {
    uint8_t sel = (g_servo_ch_sel < SERVO_CH_COUNT) ? g_servo_ch_sel : 0u;
    switch (reg) {
    case REG_SERVO_CH_SEL:   return g_servo_ch_sel;
    case REG_SERVO_WIDTH_L:  return (uint8_t)g_servo_us[sel];
    case REG_SERVO_WIDTH_H:  return (uint8_t)(g_servo_us[sel] >> 8);
    case REG_SERVO_ENABLE_L: return (uint8_t)g_servo_enable;
    case REG_SERVO_ENABLE_H: return (uint8_t)(g_servo_enable >> 8);
    default:                 return 0xFFu;
    }
}
static void servo_reg_write(uint8_t reg, uint8_t val) {
    switch (reg) {
    case REG_SERVO_CH_SEL:
        if (val < SERVO_CH_COUNT) g_servo_ch_sel = val;  // out-of-range clamped (ignored)
        break;
    case REG_SERVO_WIDTH_L:                              // stage lo byte; WIDTH_H write latches
        g_servo_width_stage = (uint16_t)((g_servo_width_stage & 0xFF00u) | val);
        break;
    case REG_SERVO_WIDTH_H: {                            // latch: clamp + store + rebuild
        uint16_t us = (uint16_t)(((uint16_t)val << 8) | (g_servo_width_stage & 0x00FFu));
        if (us < SERVO_US_MIN) us = SERVO_US_MIN;
        if (us > SERVO_US_MAX) us = SERVO_US_MAX;
        uint8_t sel = (g_servo_ch_sel < SERVO_CH_COUNT) ? g_servo_ch_sel : 0u;
        g_servo_us[sel] = us;
        g_servo_width_stage = us;                        // reflect the clamped value back
        servo_build();
        break;
    }
    case REG_SERVO_ENABLE_L:
        g_servo_enable = (uint16_t)((g_servo_enable & 0x0100u) | val) & SERVO_ENABLE_MASK;
        servo_build();
        break;
    case REG_SERVO_ENABLE_H:
        g_servo_enable = (uint16_t)((g_servo_enable & 0x00FFu) | ((uint16_t)val << 8)) & SERVO_ENABLE_MASK;
        servo_build();
        break;
    default: break;                                     // unimplemented regs ignored
    }
}

// ============================================================================
// COUNTER mode — software edge counters sampled by a timer ISR (TC5).
//
// Commissioned via a `cntr` config file: which of the 9 pads are counters, each
// one's pull (up/down/none) and edge (rising/falling/both), and a bank-global
// update rate. Each TC5 tick reads all pins, detects edges vs the previous
// sample, and increments the per-channel u32 for ENABLED channels. Max countable
// frequency ~ rate/2 (Nyquist). Pure input mode — no interlock.
//
// Pads NOT declared as counters are free for the bench tools: the DAC (on A0 =
// PA02 = CH0) is available as a square-wave test stimulus when CH0 is not a
// counter (jumper A0 -> a counter pad). The DAC rides TC3 (mode-aware dac_apply);
// TC5 (this sampler) and TC3 are separate timers.
//
// TC5 shares GCLK_ID 28 with TC4 (servo); TC4 idle here. 750 kHz tick (DIV64 off
// the 48 MHz GCLK0), MFRQ wavegen (CC0=TOP, auto-reset), CC0 = 750000/rate.
//
// Mode-bank registers (read ALL counters in one transaction):
//   0x10 READ      (w)  snapshot all counters -> shadow, reset DATA cursor
//   0x11 READ_CLR  (w)  snapshot all + atomically zero all, reset DATA cursor
//   0x12 DATA      (r)  stream the 36-byte shadow (9 x u32 LE), auto-incrementing
//   0x13-0x14 ENABLE (r) u16 enable bitmap (bit ch = channel ch is a counter)
//   0x1A CLEAR     (w)  0xFF -> zero all counters
//   0x20-0x2C DAC bench tool (square-wave stimulus; when CH0/A0 is free)
// `cntr` file: [VER=1, rate_lo, rate_hi, ch0..ch8]; each ch byte = bit0 enable,
//   bits1-2 pull (0 none/1 up/2 down), bits3-4 edge (0 rise/1 fall/2 both).
// ============================================================================
#define COUNTER_CH_COUNT    9u
#define CNTR_VERSION        1u
#define COUNTER_RATE_MIN    50u
#define COUNTER_RATE_MAX    10000u
#define COUNTER_TICK_HZ     750000u     // 48 MHz / 64 (DIV64 prescaler)

#define REG_COUNTER_READ        0x10u
#define REG_COUNTER_READCLR     0x11u
#define REG_COUNTER_DATA        0x12u
#define REG_COUNTER_ENABLE      0x13u   // 0x13 lo, 0x14 hi
#define REG_COUNTER_CLEAR       0x1Au

// Channel index -> (port group, pin). SAME pins as the servo bank.
// CH0 D0 PA02  CH1 D1 PA04  CH2 D2 PA10  CH3 D3 PA11  CH4 D7 PB09
// CH5 D8 PA07  CH6 D9 PA05  CH7 D10 PA06  CH8 D6 PB08
static const struct { uint8_t group; uint8_t pin; } g_counter_pad[COUNTER_CH_COUNT] = {
    {0,  2}, {0,  4}, {0, 10}, {0, 11}, {1,  9},
    {0,  7}, {0,  5}, {0,  6}, {1,  8},
};

static volatile uint32_t g_counter[COUNTER_CH_COUNT];   // running totals (ISR is sole writer)
static uint16_t g_counter_last;                         // last sampled 9-bit level bitmap (bit ch)
static uint8_t  g_counter_edge[COUNTER_CH_COUNT];       // 0=rising 1=falling 2=both
static uint32_t g_counter_shadow[COUNTER_CH_COUNT];     // coherent read-all snapshot
static uint16_t g_counter_enabled = 0x1FFu;             // bit ch = channel is a counter
static uint8_t  g_counter_data_idx;                     // DATA stream cursor (0..35)
static uint16_t g_counter_cc0 = 750u;                   // TC5 period (= 750000/rate); default 1 kHz

static bool g_counter_tc_initialized;                   // TC5 GCLK/APBC wired once

// Read the live 9-bit level bitmap: bit ch = current level of channel ch's pin.
static inline uint16_t counter_sample_now(void) {
    uint16_t now = 0u;
    for (uint8_t ch = 0; ch < COUNTER_CH_COUNT; ch++)
        if (PORT->Group[g_counter_pad[ch].group].IN.reg & (1u << g_counter_pad[ch].pin))
            now |= (uint16_t)(1u << ch);
    return now;
}

// ---- TC5 timebase (mirrors the TC4 servo engine: MFRQ CC0=TOP, SYNCBUSY spins) --
static void counter_tc_init_once(void) {
    if (g_counter_tc_initialized) return;
    PM->APBCMASK.reg |= PM_APBCMASK_TC5;
    GCLK->CLKCTRL.reg = (uint16_t)(GCLK_CLKCTRL_ID(TC5_GCLK_ID)   // TC5_GCLK_ID == TC4_GCLK_ID (28)
                                 | GCLK_CLKCTRL_GEN_GCLK0
                                 | GCLK_CLKCTRL_CLKEN);
    while (GCLK->STATUS.bit.SYNCBUSY) { /* spin */ }
    g_counter_tc_initialized = true;
}

static void counter_tc_stop(void) {
    NVIC_DisableIRQ(TC5_IRQn);
    TC5->COUNT16.INTENCLR.reg = TC_INTENCLR_MC0;
    TC5->COUNT16.CTRLA.bit.ENABLE = 0;
    while (TC5->COUNT16.STATUS.bit.SYNCBUSY) { /* spin */ }
}

static void counter_tc_start(void) {
    // Reset.
    TC5->COUNT16.CTRLA.bit.SWRST = 1;
    while (TC5->COUNT16.STATUS.bit.SYNCBUSY) { /* spin */ }
    while (TC5->COUNT16.CTRLA.bit.SWRST)     { /* spin */ }

    // 16-bit count, MFRQ wavegen (CC0=TOP and resets the counter), /64 prescaler.
    // 48 MHz / 64 = 750 kHz tick; 1 ms (1 kHz) = 750 ticks -> CC0=750.
    TC5->COUNT16.CTRLA.reg = TC_CTRLA_MODE_COUNT16
                           | TC_CTRLA_WAVEGEN_MFRQ
                           | TC_CTRLA_PRESCALER_DIV64;
    TC5->COUNT16.CC[0].reg = g_counter_cc0;                       // CC0 = 750000/rate
    while (TC5->COUNT16.STATUS.bit.SYNCBUSY) { /* spin */ }

    TC5->COUNT16.INTFLAG.reg = TC_INTFLAG_MC0;                    // clear stale
    TC5->COUNT16.INTENSET.reg = TC_INTENSET_MC0;                  // 1 kHz sampler tick
    NVIC_EnableIRQ(TC5_IRQn);

    TC5->COUNT16.CTRLA.bit.ENABLE = 1;
    while (TC5->COUNT16.STATUS.bit.SYNCBUSY) { /* spin */ }
}

// Configure one counter pad as a GPIO input with the requested pull (0 none, 1
// up, 2 down). On SAMD21 the pull direction is OUT (1=up/0=down) with PULLEN set.
static void counter_pin_input(uint8_t g, uint8_t p, uint8_t pull) {
    PORT->Group[g].PINCFG[p].bit.PMUXEN = 0;
    PORT->Group[g].DIRCLR.reg = (1u << p);
    if (pull == 1u)      PORT->Group[g].OUTSET.reg = (1u << p);   // pull-up
    else if (pull == 2u) PORT->Group[g].OUTCLR.reg = (1u << p);   // pull-down
    PORT->Group[g].PINCFG[p].reg = (uint8_t)(PORT_PINCFG_INEN |
                                   ((pull != 0u) ? PORT_PINCFG_PULLEN : 0u));
}

// Apply the commissioned `cntr` file: enable bitmap, per-channel pull + edge, and
// the bank-global update rate (-> TC5 CC0). No/short/bad file -> safe default
// (all 9 enabled, no pull, rising, 1 kHz). Only ENABLED pads are driven as inputs;
// the rest are left free for the bench tools (e.g. the DAC on A0/CH0).
static void counter_apply_cfg(void) {
    g_counter_enabled = 0x1FFu;
    uint16_t rate = 1000u;
    uint8_t  pull[COUNTER_CH_COUNT] = {0};
    for (uint8_t ch = 0; ch < COUNTER_CH_COUNT; ch++) g_counter_edge[ch] = 0u;

    int s = store_find_str("cntr");
    if (s >= 0 && g_rec_len[s] >= 3u && g_rec_data[s][0] == CNTR_VERSION) {
        const uint8_t* d = g_rec_data[s];
        uint16_t len = g_rec_len[s];
        rate = (uint16_t)(d[1] | ((uint16_t)d[2] << 8));
        g_counter_enabled = 0u;
        for (uint8_t ch = 0; ch < COUNTER_CH_COUNT && (3u + ch) < len; ch++) {
            uint8_t b = d[3u + ch];
            if (b & 1u) g_counter_enabled |= (uint16_t)(1u << ch);
            pull[ch]           = (uint8_t)((b >> 1) & 3u);
            g_counter_edge[ch] = (uint8_t)((b >> 3) & 3u);
        }
    }
    for (uint8_t ch = 0; ch < COUNTER_CH_COUNT; ch++) {
        if (g_counter_enabled & (uint16_t)(1u << ch))
            counter_pin_input(g_counter_pad[ch].group, g_counter_pad[ch].pin, pull[ch]);
    }
    if (rate < COUNTER_RATE_MIN) rate = COUNTER_RATE_MIN;
    if (rate > COUNTER_RATE_MAX) rate = COUNTER_RATE_MAX;
    uint32_t cc0 = COUNTER_TICK_HZ / rate;
    if (cc0 < 2u)     cc0 = 2u;
    if (cc0 > 65535u) cc0 = 65535u;
    g_counter_cc0 = (uint16_t)cc0;
}

// Entering MODE_COUNTER: apply the cntr config (enable/pull/edge/rate), seed the
// last-level (no phantom first edge), zero counters + shadow, optionally bring up
// the DAC bench tool on A0 (CH0) when CH0 isn't a counter, then start TC5.
static void counter_enter(void) {
    counter_tc_init_once();
    counter_apply_cfg();
    g_counter_last = counter_sample_now();                       // seed: no edge on first tick
    for (uint8_t ch = 0; ch < COUNTER_CH_COUNT; ch++) {
        g_counter[ch]        = 0u;
        g_counter_shadow[ch] = 0u;
    }
    g_counter_data_idx = 0u;
    if (!(g_counter_enabled & 0x1u)) {                           // CH0/A0 free -> DAC stimulus
        dac_init();
        PORT->Group[0].PINCFG[2].bit.PMUXEN = 1;                 // route PA02 -> DAC
        PORT->Group[0].PMUX[1].bit.PMUXE    = PORT_PMUX_PMUXE_B_Val;
        DAC->CTRLB.bit.EOEN = 1;
        while (DAC->STATUS.bit.SYNCBUSY) { /* spin */ }
        for (uint8_t t = 0; t < DAC_NTONES; t++) g_dac_type[t] = DAC_TONE_OFF;
        g_dds.active = false;
        DAC->DATA.reg = g_dac_offset;
    }
    counter_tc_start();
}

// Leaving MODE_COUNTER: stop TC5 + any DAC waveform (TC3); pins stay inputs (safe).
static void counter_exit(void) {
    counter_tc_stop();
    NVIC_DisableIRQ(TC3_IRQn);
    g_dds.active = false;
    tc3_stop();
}

// TC5 ISR — the 1 kHz sampler. Reads all 9 pins, detects edges vs the previous
// sample, and increments the per-channel counter on the configured edge. Bounded
// + fast: one IN read + a 9-iteration loop, no blocking. Mirrors pulse_sample_1khz.
void TC5_Handler(void) {
    if (!TC5->COUNT16.INTFLAG.bit.MC0) return;
    TC5->COUNT16.INTFLAG.reg = TC_INTFLAG_MC0;

    uint16_t now     = counter_sample_now();
    uint16_t changed = now ^ g_counter_last;
    uint16_t rose    = changed & now;             // 0 -> 1
    uint16_t fell    = changed & (uint16_t)~now;  // 1 -> 0
    for (uint8_t ch = 0; ch < COUNTER_CH_COUNT; ch++) {
        uint16_t bit = (uint16_t)(1u << ch);
        if (!(g_counter_enabled & bit)) continue;                 // disabled pad -> not counted
        if ((g_counter_edge[ch] == 0u && (rose    & bit)) ||
            (g_counter_edge[ch] == 1u && (fell    & bit)) ||
            (g_counter_edge[ch] == 2u && (changed & bit)))
            g_counter[ch]++;
    }
    g_counter_last = now;
}

// Coherent read-all: snapshot every live counter into the shadow under a brief
// TC5-IRQ mask (optionally zeroing as we go), and reset the DATA stream cursor.
// Edges between the snapshot and the next ISR tick count toward the next read.
static void counter_snapshot(bool clear) {
    NVIC_DisableIRQ(TC5_IRQn);
    for (uint8_t ch = 0; ch < COUNTER_CH_COUNT; ch++) {
        g_counter_shadow[ch] = g_counter[ch];
        if (clear) g_counter[ch] = 0u;
    }
    if (g_counter_tc_initialized) NVIC_EnableIRQ(TC5_IRQn);
    g_counter_data_idx = 0u;
}

// COUNTER mode-bank register access, dispatched only while MODE_COUNTER. The Pico
// triggers READ / READ_CLR (snapshot all), then streams the 36-byte shadow from
// DATA (9 x u32 LE), one byte per read. DAC bench regs (0x20-0x2C) delegate out.
static uint8_t counter_reg_read(uint8_t reg) {
    switch (reg) {
    case REG_COUNTER_DATA: {                                     // stream the snapshot, auto-inc
        uint8_t i = g_counter_data_idx;
        if (i >= COUNTER_CH_COUNT * 4u) return 0xFFu;
        g_counter_data_idx = (uint8_t)(i + 1u);
        return (uint8_t)(g_counter_shadow[i >> 2] >> ((i & 3u) * 8u));
    }
    case REG_COUNTER_ENABLE:      return (uint8_t)g_counter_enabled;
    case REG_COUNTER_ENABLE + 1u: return (uint8_t)(g_counter_enabled >> 8);
    default: { uint8_t v = 0xFFu; dac_reg_read(reg, &v); return v; }   // DAC stimulus regs
    }
}
static void counter_reg_write(uint8_t reg, uint8_t val) {
    switch (reg) {
    case REG_COUNTER_READ:    counter_snapshot(false); break;    // read all
    case REG_COUNTER_READCLR: counter_snapshot(true);  break;    // read + clear all
    case REG_COUNTER_CLEAR:                                       // 0xFF -> zero all
        if (val == 0xFFu) {
            NVIC_DisableIRQ(TC5_IRQn);
            for (uint8_t ch = 0; ch < COUNTER_CH_COUNT; ch++) g_counter[ch] = 0u;
            if (g_counter_tc_initialized) NVIC_EnableIRQ(TC5_IRQn);
        }
        break;
    default: dac_reg_write(reg, val); break;                     // DAC stimulus regs
    }
}

// Register read dispatch (control bank). Out-of-range -> 0xFF.
static uint8_t i2c_reg_read(uint8_t reg) {
    if (reg >= 0x10u && reg <= 0x3Fu) {                  // mode bank — interpreted per active mode
        if (g_mode == MODE_PIO) return pio_reg_read(reg);
        if (g_mode == MODE_ADC) return adc_reg_read(reg);
        if (g_mode == MODE_MIXED) return mixed_reg_read(reg);
        if (g_mode == MODE_SERVO) return servo_reg_read(reg);
        if (g_mode == MODE_COUNTER) return counter_reg_read(reg);
        return 0xFFu;
    }
    switch (reg) {
    case 0x00: return I2C_CLIENT_WHOAMI;
    case 0x01: return I2C_CLIENT_VERSION;
    case 0x02: return g_mode;
    case 0x03: return (uint8_t)(g_status | (g_offline ? 0x08u : 0x00u));   // bit3 = OFFLINE
    case 0x04: return g_int_flags;
    case 0x05: return g_i2c_addr;
    case 0x06: case 0x07: case 0x08: case 0x09:
    case 0x0A: case 0x0B: case 0x0C: case 0x0D:
        return g_unique_id[reg - 0x06u];
    // NOTE: this register window is the legacy SLOT-indexed access path (sel =
    // slot 0..STORE_SLOTS-1). A name-based open/read/write interface is a later
    // step in the named-file-filesystem migration.
    case REG_REC_SEL:  return g_store_rec_sel;
    case REG_REC_LEN:  return (g_store_rec_sel < STORE_SLOTS) ? g_rec_len[g_store_rec_sel] : 0u;
    case REG_REC_OFF:  return g_store_rec_off;
    case REG_REC_DATA: {                              // data port: advances OFFSET, not reg ptr
        uint8_t v = 0xFFu;
        if (g_store_rec_sel < STORE_SLOTS && g_store_rec_off < STORE_DATA_MAX)
            v = g_rec_data[g_store_rec_sel][g_store_rec_off];
        if (g_store_rec_off < STORE_DATA_MAX) g_store_rec_off++;
        return v;
    }
    case REG_STORE_STAT: return g_status;
    case REG_SET_ADDR:   return 0u;
    // FILE bank (name-keyed, read-only). FILE_NAME/FILE_DATA are data-ports:
    // they advance their own cursor, not the register pointer (see IS_DATA_PORT).
    case REG_FILE_NAME: {                             // data port: read back the name byte, advance
        uint8_t v = g_file_name[g_file_name_idx];
        g_file_name_idx = (uint8_t)((g_file_name_idx + 1u) & 3u);
        return v;
    }
    case REG_FILE_STAT: return (g_file_slot >= 0) ? 0u : 1u;
    case REG_FILE_SIZE: return (g_file_slot >= 0) ? g_rec_len[g_file_slot] : 0u;
    case REG_FILE_DATA:                               // data port: next file byte, advance cursor
        if (g_file_slot >= 0 && g_file_off < g_rec_len[g_file_slot])
            return g_rec_data[g_file_slot][g_file_off++];
        return 0xFFu;
    default:   return 0xFFu;
    }
}

// LIST helper: advance g_file_list_i to the next USED slot from its current
// value, then publish that slot (or -1 when exhausted) + its name into the
// FILE bank state. `from_start` resets the cursor to slot 0 first.
static void file_list_advance(bool from_start) {
    uint8_t i = from_start ? 0u : (uint8_t)(g_file_list_i + 1u);
    for (; i < STORE_SLOTS; i++) {
        if (g_rec_used[i]) break;
    }
    g_file_list_i = i;
    if (i < STORE_SLOTS) {
        g_file_slot = (int)i;
        memcpy(g_file_name, g_rec_name[i], STORE_NAME_LEN);
    } else {
        g_file_slot = -1;
    }
    g_file_name_idx = 0u;
    g_file_off = 0u;
}

// ============================================================================
// Commissioning lifecycle: mode enter/leave factoring + offline/tri-state.
//
// mode_leave(m) / mode_enter(m) hold the exact per-mode teardown / setup logic
// that used to be inlined in i2c_reg_write case 0x02. They're shared by the
// runtime MODE-register switch, the boot-time "apply commissioned mode" (B4),
// and the offline transition (which leaves the active mode then tri-states).
// ============================================================================

// "leaving m" — undo whatever mode_enter(m) set up. IDLE = no-op.
static void mode_leave(uint8_t m) {
    if (m == MODE_PIO) {                            // leaving PIO
        pio_il_disarm();                            // deassert INT, drop the latch
        pio_safe_all();                             // pins safe (inputs)
        DAC->CTRLB.bit.EOEN = 1;                    // restore DAC output buffer on PA02
        while (DAC->STATUS.bit.SYNCBUSY) { /* spin */ }
    } else if (m == MODE_ADC) {                     // leaving ADC
        adc_sampler_stop();                         // stop the tone-clocked sweep (TC3 + DDS off)
    } else if (m == MODE_MIXED) {                   // leaving MIXED
        NVIC_DisableIRQ(TC3_IRQn);                  // stop any DAC waveform the bench tool ran
        g_dds.active = false;
        tc3_stop();
        mixed_il_disarm();                          // deassert INT, drop latch, release pins (safe)
    } else if (m == MODE_SERVO) {                   // leaving SERVO
        servo_exit();                               // stop TC4, drive all servo pins low
        DAC->CTRLB.bit.EOEN = 1;                    // restore DAC output buffer on PA02 (CH0)
        while (DAC->STATUS.bit.SYNCBUSY) { /* spin */ }
    } else if (m == MODE_COUNTER) {                 // leaving COUNTER
        counter_exit();                             // stop TC5 (pins stay inputs, safe)
        DAC->CTRLB.bit.EOEN = 1;                    // restore DAC output buffer on PA02 (CH0)
        while (DAC->STATUS.bit.SYNCBUSY) { /* spin */ }
    }
}

// "entering m" — set up the mode's pins/peripherals. IDLE = no-op.
static void mode_enter(uint8_t m) {
    if (m == MODE_PIO) {                            // entering PIO
        DAC->CTRLB.bit.EOEN = 0;                    // free CH0=PA02 from the DAC (else it pins CH0 low)
        while (DAC->STATUS.bit.SYNCBUSY) { /* spin */ }
        gpmp_apply();                               // load the commissioned power-on pin map
        pio_apply_all();                            // claim + configure the 8 channels
        pio_il_arm();                               // parse interlock_cfg, set up INT pin
    } else if (m == MODE_ADC) {                     // entering ADC
        adc_mode_setup();                           // DIV16 sampler + window stats + DAC const
        adc_il_arm();                               // parse ilcf, claim D6 output, arm interlock
    } else if (m == MODE_MIXED) {                   // entering MIXED
        dac_init();                                 // DAC bench source available in MIXED (A0->A1)
        PORT->Group[0].PINCFG[2].bit.PMUXEN = 1;    // route PA02 -> DAC (mode entry may have freed it)
        PORT->Group[0].PMUX[1].bit.PMUXE    = PORT_PMUX_PMUXE_B_Val;
        DAC->CTRLB.bit.EOEN = 1;
        while (DAC->STATUS.bit.SYNCBUSY) { /* spin */ }
        for (uint8_t t = 0; t < DAC_NTONES; t++) g_dac_type[t] = DAC_TONE_OFF;  // idle: no waveform
        g_dds.active = false;
        DAC->DATA.reg = g_dac_offset;               // idle at the DC offset
        mixed_il_arm();                             // parse interlock_cfg, claim pins, set up INT
    } else if (m == MODE_SERVO) {                   // entering SERVO
        DAC->CTRLB.bit.EOEN = 0;                    // free CH0=PA02 from the DAC (else it pins CH0 low)
        while (DAC->STATUS.bit.SYNCBUSY) { /* spin */ }
        servo_enter();                              // pins->GPIO low, build schedule, start TC4
    } else if (m == MODE_COUNTER) {                 // entering COUNTER
        DAC->CTRLB.bit.EOEN = 0;                    // free CH0=PA02 from the DAC so it reads as input
        while (DAC->STATUS.bit.SYNCBUSY) { /* spin */ }
        counter_enter();                            // pins->GPIO inputs, seed last, zero, start TC5
    }
}

// Commissioning offline state. g_offline is declared near g_status (above) so
// the STATUS-register read (case 0x03) can surface it as bit3. false = ONLINE
// (default at boot): serves config, drives commissioned pins, config WRITES
// refused. true = OFFLINE: mode halted, all HIL pins tri-stated, config WRITES
// allowed. OFFLINE->ONLINE only via USB disconnect -> reboot (the cold boot
// re-applies config in i2c_slave_init/B4).
bool samd21_is_offline(void) { return g_offline; }

// True while a deferred config-store flash commit is queued or in flight (the
// i2c_store_service main-loop pump clears g_commit_pending to -1 when done).
// main.c gates the offline-disconnect reboot on this so files finish persisting.
bool samd21_store_commit_pending(void) { return g_commit_pending >= 0; }

// Tri-state the full union of HIL pins (the 9 servo/counter channels, which
// already includes D6/PB08 the INT pin): input, INEN on for read-back, NO pull,
// NO drive => Hi-Z. Then disable the DAC output buffer, stop both mode timers
// (idempotent), deassert INT, and latch g_mode=IDLE / g_offline=true.
void mode_go_offline(void) {
    mode_leave(g_mode);

    // The servo/counter pad table is the union of all 9 driven channel pins and
    // includes D6=PB08 (the INT pin, channel 8 here). Tri-state each.
    for (uint8_t ch = 0; ch < SERVO_CH_COUNT; ch++) {
        uint8_t g = g_servo_pad[ch].group, p = g_servo_pad[ch].pin;
        PORT->Group[g].DIRCLR.reg      = (1u << p);          // input (no drive)
        PORT->Group[g].PINCFG[p].reg   = PORT_PINCFG_INEN;   // INEN on, no pull, no drive => Hi-Z
    }

    DAC->CTRLB.bit.EOEN = 0;                         // release A0/PA02 from the DAC (Hi-Z input)
    while (DAC->STATUS.bit.SYNCBUSY) { /* spin */ }

    servo_tc_stop();                                 // stop TC4 (idempotent)
    counter_tc_stop();                               // stop TC5 (idempotent)

    pio_int_assert(false);                           // deassert INT (D6/PB08)

    g_mode    = MODE_IDLE;
    g_offline = true;
}

// Register write dispatch. Read-only / unimplemented regs are ignored.
static void i2c_reg_write(uint8_t reg, uint8_t val) {
    if (reg >= 0x10u && reg <= 0x3Fu) {                  // mode bank — interpreted per active mode
        if (g_mode == MODE_PIO) pio_reg_write(reg, val);
        else if (g_mode == MODE_ADC) adc_reg_write(reg, val);
        else if (g_mode == MODE_MIXED) mixed_reg_write(reg, val);
        else if (g_mode == MODE_SERVO) servo_reg_write(reg, val);
        else if (g_mode == MODE_COUNTER) counter_reg_write(reg, val);
        return;
    }
    switch (reg) {
    case 0x02:                                            // MODE — apply the switch
        if (val <= MODE_MAX && val != g_mode) {
            mode_leave(g_mode);
            g_mode = val;
            mode_enter(val);
        }
        break;
    case 0x04:                                            // INT_FLAGS: write-1-to-clear
        g_int_flags &= (uint8_t)~val;
        if (val & 0x01u) pio_il_manual_reset();           // acking the trip bit == manual reset
        if ((val & MIXED_IL_INT_BIT) && g_mode == MODE_MIXED) mixed_il_manual_reset();
        if ((val & ADC_IL_INT_BIT) && g_mode == MODE_ADC) adc_il_manual_reset();
        break;
    case 0x0E: if (val == 0xA5u) g_reset_pending = true; break;  // RESET (magic 0xA5; deferred)
    // NOTE: SLOT-indexed legacy access path (sel = slot 0..STORE_SLOTS-1). The
    // name-based open/read/write interface is a later migration step.
    case REG_REC_SEL: if (val < STORE_SLOTS) { g_store_rec_sel = val; g_store_rec_off = 0u; } break;
    case REG_REC_LEN: if (g_store_rec_sel < STORE_SLOTS && val <= STORE_DATA_MAX) g_rec_len[g_store_rec_sel] = val; break;
    case REG_REC_OFF: g_store_rec_off = val; break;
    case REG_REC_DATA:                                    // data port: write byte, grow len, advance offset
        if (g_store_rec_sel < STORE_SLOTS && g_store_rec_off < STORE_DATA_MAX) {
            g_rec_data[g_store_rec_sel][g_store_rec_off] = val;
            if ((uint16_t)g_store_rec_off + 1u > g_rec_len[g_store_rec_sel])
                g_rec_len[g_store_rec_sel] = (uint8_t)(g_store_rec_off + 1u);
            g_rec_dirty[g_store_rec_sel] = true;
        }
        if (g_store_rec_off < STORE_DATA_MAX) g_store_rec_off++;
        break;
    case REG_REC_CTRL:                                    // 0xC0 = commit selected, 0x5E = reload all
        if (val == 0xC0u && g_store_rec_sel < STORE_SLOTS && g_rec_used[g_store_rec_sel])
            g_commit_pending = (int8_t)g_store_rec_sel;
        else if (val == 0x5Eu) store_load();
        break;
    case REG_SET_ADDR:                                    // commissioning data-port: [0xAC magic][new_addr]
        if (!g_setaddr_armed) {
            g_setaddr_armed = (val == 0xACu);
        } else {
            g_setaddr_armed = false;
            if (val >= 0x08u && val <= 0x77u) {           // valid 7-bit address
                int s = store_alloc_str("idnt");          // identity named slot
                if (s >= 0) {
                    g_rec_data[s][0] = val;               // identity record byte 0 = I2C address
                    if (g_rec_len[s] < 1u) g_rec_len[s] = 1u;
                    g_rec_dirty[s] = true;
                    g_commit_pending = (int8_t)s;         // persist; effective on next RESET
                }
            }
        }
        break;
    // FILE bank (name-keyed, read-only for the master).
    case REG_FILE_NAME:                                   // data port: burst the 4-byte name in
        g_file_name[g_file_name_idx] = val;
        g_file_name_idx = (uint8_t)((g_file_name_idx + 1u) & 3u);
        break;
    case REG_FILE_CTRL:
        if (val == 0x01u) {                               // OPEN by name
            g_file_slot = store_find(g_file_name);
            g_file_off = 0u; g_file_name_idx = 0u;
        } else if (val == 0x02u) {                        // CLOSE
            g_file_slot = -1;
        } else if (val == 0x03u) {                        // LIST_FIRST
            file_list_advance(true);
        } else if (val == 0x04u) {                        // LIST_NEXT
            file_list_advance(false);
        }
        break;
    case REG_FILE_SEEK: if (val <= STORE_DATA_MAX) g_file_off = val; break;
    default:   break;
    }
}

static void i2c_slave_init(void) {
    i2c_read_unique_id();   // for commissioning identification (UNIQUE_ID regs)
    store_load();           // scan the config log -> RAM shadow of each record

    // Identity record ("idnt"): byte0 = I2C address, byte1 = power-up mode.
    // Default 0x55 / IDLE if unprovisioned. (Set via SET_ADDR commissioning.)
    int idn = store_find_str("idnt");
    if (idn >= 0 && g_rec_len[idn] >= 1u) {
        uint8_t a = g_rec_data[idn][0];
        if (a >= 0x08u && a <= 0x77u) g_i2c_addr = a;
    }
    if (idn >= 0 && g_rec_len[idn] >= 2u) {
        uint8_t m = g_rec_data[idn][1];
        if (m <= MODE_MAX) g_mode = m;
    }

    // B4: actually ENTER the commissioned mode at boot so the SAMD21 drives its
    // pins per the freshly-loaded config (previously g_mode was set lazily and
    // only took effect on the next MODE-register write). IDLE -> no-op.
    if (g_mode <= MODE_MAX) mode_enter(g_mode);

    // 1. Bus clock.
    PM->APBCMASK.reg |= PM_APBCMASK_SERCOM2;

    // 2. SERCOM2 core + slow clocks -> GCLK0 (48 MHz).
    GCLK->CLKCTRL.reg = (uint16_t)(GCLK_CLKCTRL_ID(I2C_GCLK_ID_CORE)
                                 | GCLK_CLKCTRL_GEN_GCLK0 | GCLK_CLKCTRL_CLKEN);
    while (GCLK->STATUS.bit.SYNCBUSY) { /* spin */ }
    GCLK->CLKCTRL.reg = (uint16_t)(GCLK_CLKCTRL_ID(I2C_GCLK_ID_SLOW)
                                 | GCLK_CLKCTRL_GEN_GCLK0 | GCLK_CLKCTRL_CLKEN);
    while (GCLK->STATUS.bit.SYNCBUSY) { /* spin */ }

    // 3. Reset SERCOM2.
    I2C_SERCOM->I2CS.CTRLA.bit.SWRST = 1;
    while (I2C_SERCOM->I2CS.SYNCBUSY.bit.SWRST) { /* spin */ }

    // 4. I2C slave mode, 300 ns SDA hold, SCLSM = stretch SCL after the ACK bit
    //    (gives the ISR time to load the read byte — required for slave transmit).
    I2C_SERCOM->I2CS.CTRLA.reg =
        SERCOM_I2CS_CTRLA_MODE_I2C_SLAVE |
        SERCOM_I2CS_CTRLA_SDAHOLD(2) |
        SERCOM_I2CS_CTRLA_SCLSM;

    // 4b. Smart mode: a DATA access auto-handles the ACK/byte protocol (SCLSM
    //     requires it). This is what makes the transmit actually drive the byte.
    I2C_SERCOM->I2CS.CTRLB.reg = SERCOM_I2CS_CTRLB_SMEN;

    // 5. 7-bit address (placed in ADDR[7:1] by the macro), exact match. From
    //    g_i2c_addr (default 0x55; M2c loads it from the identity record).
    I2C_SERCOM->I2CS.ADDR.reg = SERCOM_I2CS_ADDR_ADDR(g_i2c_addr);

    // 6. PMUX PA08/PA09 -> function D (SERCOM2 PAD[0]/[1]).
    PORT->Group[0].PINCFG[8].bit.PMUXEN = 1;
    PORT->Group[0].PMUX[4].bit.PMUXE    = PORT_PMUX_PMUXE_D_Val;
    PORT->Group[0].PINCFG[9].bit.PMUXEN = 1;
    PORT->Group[0].PMUX[4].bit.PMUXO    = PORT_PMUX_PMUXO_D_Val;

    // 7. Interrupts: address match, data ready, stop received.
    I2C_SERCOM->I2CS.INTENSET.reg =
        SERCOM_I2CS_INTENSET_AMATCH |
        SERCOM_I2CS_INTENSET_DRDY |
        SERCOM_I2CS_INTENSET_PREC;
    NVIC_EnableIRQ(SERCOM2_IRQn);

    // 8. Enable.
    I2C_SERCOM->I2CS.CTRLA.bit.ENABLE = 1;
    while (I2C_SERCOM->I2CS.SYNCBUSY.bit.ENABLE) { /* spin */ }
}

void SERCOM2_Handler(void) {
    SercomI2cs *s = &I2C_SERCOM->I2CS;

    // Stop received -> end of transaction; next AMATCH starts fresh.
    if (s->INTFLAG.bit.PREC) {
        s->INTFLAG.reg = SERCOM_I2CS_INTFLAG_PREC;
        i2c_reg_ptr_known = false;
        return;
    }

    // Address match -> ACK (CMD=0x3 also clears AMATCH).
    if (s->INTFLAG.bit.AMATCH) {
        i2c_reg_ptr_known = false;
        if (s->STATUS.bit.DIR) {                        // master read -> pre-load 1st tx byte
            s->DATA.reg = i2c_reg_read(i2c_reg_ptr);    // else the stale addr byte goes out first
            if (!IS_DATA_PORT(i2c_reg_ptr)) i2c_reg_ptr++;  // data port advances OFFSET, not reg ptr
        }
        s->CTRLB.bit.ACKACT = 0;
        s->CTRLB.bit.CMD = 0x3;                         // ACK the address (smart mode)
        return;
    }

    // Data ready.
    if (s->INTFLAG.bit.DRDY) {
        if (s->STATUS.bit.DIR) {                        // master read -> slave transmit
            s->DATA.reg = i2c_reg_read(i2c_reg_ptr);    // smart mode sends it
            if (!IS_DATA_PORT(i2c_reg_ptr)) i2c_reg_ptr++;
        } else {                                        // master write -> slave receive
            uint8_t b = (uint8_t)s->DATA.reg;           // smart mode ACKs it
            if (!i2c_reg_ptr_known) { i2c_reg_ptr = b; i2c_reg_ptr_known = true; }
            else { uint8_t reg = i2c_reg_ptr; i2c_reg_write(reg, b); if (!IS_DATA_PORT(reg)) i2c_reg_ptr++; }
        }
        return;
    }
}

// ============================================================================
// USB-CDC chunked config-file write — the Pi host stages a named config file
// into the name-keyed store over the shell, then commits it to flash.
//
// A file larger than ~120 B can't fit one COMM_PAYLOAD_MAX (128 B) shell frame,
// so the host streams it: BEGIN (open a slot by name) -> one or more DATA chunks
// (appended into the per-slot RAM shadow g_rec_data[slot]) -> COMMIT (publish
// g_rec_len + dirty, then hand the slot to the async i2c_store_service via
// g_commit_pending, which does the deferred flash write). The command returns
// immediately; nothing persists without COMMIT. Re-writing a name updates it
// (store_alloc returns the existing slot; commit appends a higher-seq entry).
// DELETE is intentionally out of scope.
// ============================================================================
static int      g_file_wr_slot = -1;   // slot being staged, -1 = no open BEGIN
static uint16_t g_file_wr_len;          // bytes appended into the shadow so far

// ---------- CMD_FILE_BEGIN -----------------------------------------------
// args:   name[4]   result: empty   status: OK / BAD_ARGS (store full / short)
static uint8_t cmd_file_begin(shell_reader_t* args, shell_writer_t* result) {
    (void)result;
    if (!g_offline) return SHELL_STATUS_BUSY;   // online: not accepting commissioning writes
    if (sr_remaining(args) < STORE_NAME_LEN) return SHELL_STATUS_BAD_ARGS;
    uint8_t name[STORE_NAME_LEN];
    sr_bytes(args, name, STORE_NAME_LEN);
    if (args->overflow || sr_remaining(args) != 0) return SHELL_STATUS_BAD_ARGS;

    int s = store_alloc(name);
    if (s < 0) return SHELL_STATUS_CMD_FAILED;   // store full (16 slots used)
    g_file_wr_slot = s;
    g_file_wr_len  = 0;
    return SHELL_STATUS_OK;
}

// ---------- CMD_FILE_DATA ------------------------------------------------
// args:   chunk bytes   result: empty   status: OK / BAD_ARGS
// Appends the chunk into the open slot's shadow; rejects if it would overflow
// STORE_DATA_MAX (240) or if no BEGIN is open.
static uint8_t cmd_file_data(shell_reader_t* args, shell_writer_t* result) {
    (void)result;
    if (!g_offline) return SHELL_STATUS_BUSY;   // online: not accepting commissioning writes
    if (g_file_wr_slot < 0) return SHELL_STATUS_BAD_ARGS;   // no BEGIN
    uint16_t n = sr_remaining(args);
    if ((uint32_t)g_file_wr_len + n > STORE_DATA_MAX) return SHELL_STATUS_BAD_ARGS;
    sr_bytes(args, &g_rec_data[g_file_wr_slot][g_file_wr_len], n);
    if (args->overflow) return SHELL_STATUS_BAD_ARGS;
    g_file_wr_len = (uint16_t)(g_file_wr_len + n);
    return SHELL_STATUS_OK;
}

// ---------- CMD_FILE_COMMIT ----------------------------------------------
// args:   (none)   result: empty   status: OK / BAD_ARGS
// Publishes the staged length, marks dirty, and queues the flash commit for
// i2c_store_service (main loop). Returns immediately; the write is deferred.
static uint8_t cmd_file_commit(shell_reader_t* args, shell_writer_t* result) {
    (void)result;
    if (!g_offline) return SHELL_STATUS_BUSY;   // online: not accepting commissioning writes
    if (sr_remaining(args) != 0) return SHELL_STATUS_BAD_ARGS;
    if (g_file_wr_slot < 0)      return SHELL_STATUS_BAD_ARGS;   // no BEGIN
    g_rec_len[g_file_wr_slot]   = (uint8_t)g_file_wr_len;
    g_rec_dirty[g_file_wr_slot] = true;
    g_commit_pending            = (int8_t)g_file_wr_slot;
    g_file_wr_slot = -1;
    return SHELL_STATUS_OK;
}

// ---------- CMD_FILE_LIST ------------------------------------------------
// args:   (none)
// result: count:u8 then per used slot {name[4], len:u8}  (<=16*5 = 80 B)
static uint8_t cmd_file_list(shell_reader_t* args, shell_writer_t* result) {
    if (sr_remaining(args) != 0) return SHELL_STATUS_BAD_ARGS;
    uint8_t count = 0;
    for (uint8_t i = 0; i < STORE_SLOTS; i++) if (g_rec_used[i]) count++;
    sw_u8(result, count);
    for (uint8_t i = 0; i < STORE_SLOTS; i++) {
        if (!g_rec_used[i]) continue;
        sw_bytes(result, g_rec_name[i], STORE_NAME_LEN);
        sw_u8(result, g_rec_len[i]);
    }
    return result->overflow ? SHELL_STATUS_RESULT_TOO_BIG : SHELL_STATUS_OK;
}

// ---------- CMD_REG_READ / CMD_REG_WRITE / CMD_REG_READN -----------------
// USB->I2C-register bridge: proxy the SAME i2c_reg_read/i2c_reg_write the I2C
// slave ISR uses, so a Python host on ttyACM can drive every mode bank + the
// FILE/store windows over USB and exercise the identical register code the Pico
// runs over I2C. READN mirrors an I2C burst read: a data-port reg streams (its
// cursor advances internally, register held), any other reg auto-advances the
// address. Intended for a USB-only test harness — do not drive concurrently with
// live I2C-master traffic on the same registers.
static uint8_t cmd_reg_read(shell_reader_t* args, shell_writer_t* result) {
    if (sr_remaining(args) < 1u) return SHELL_STATUS_BAD_ARGS;
    sw_u8(result, i2c_reg_read(sr_u8(args)));
    return SHELL_STATUS_OK;
}
static uint8_t cmd_reg_write(shell_reader_t* args, shell_writer_t* result) {
    (void)result;
    if (sr_remaining(args) < 2u) return SHELL_STATUS_BAD_ARGS;
    uint8_t reg = sr_u8(args);
    i2c_reg_write(reg, sr_u8(args));
    return SHELL_STATUS_OK;
}
static uint8_t cmd_reg_readn(shell_reader_t* args, shell_writer_t* result) {
    if (sr_remaining(args) < 2u) return SHELL_STATUS_BAD_ARGS;
    uint8_t reg = sr_u8(args);
    uint8_t n   = sr_u8(args);
    for (uint8_t i = 0; i < n; i++) {
        sw_u8(result, i2c_reg_read(reg));
        if (!IS_DATA_PORT(reg)) reg++;          // mirror the I2C register-pointer auto-advance
    }
    return result->overflow ? SHELL_STATUS_RESULT_TOO_BIG : SHELL_STATUS_OK;
}

// ---------- CMD_OFFLINE --------------------------------------------------
// args: (none)   result: empty   status: OK
// Enter the commissioning OFFLINE state: halt the active mode, tri-state every
// HIL pin (Hi-Z), release the DAC + stop the mode timers, deassert INT, and
// begin accepting config-file writes. Return to ONLINE only via USB disconnect
// -> reboot (the cold boot re-applies the freshly-written config; see B4).
static uint8_t cmd_offline(shell_reader_t* args, shell_writer_t* result) {
    (void)result;
    if (sr_remaining(args) != 0) return SHELL_STATUS_BAD_ARGS;
    mode_go_offline();
    return SHELL_STATUS_OK;
}
#endif // I2C_CLIENT

// --- low-level helpers --------------------------------------------------

// Wait for either MB (master TX done) or SB (slave reply ready). No timeout
// in v1; relies on layer-2 WDT to catch a wedged bus.
static void i2c_wait_complete(void) {
    while (!(I2C_SERCOM->I2CM.INTFLAG.reg
            & (SERCOM_I2CM_INTFLAG_MB | SERCOM_I2CM_INTFLAG_SB))) { /* spin */ }
}

// Returns true on bus-level failure (BUSERR or ARBLOST). RXNACK is checked
// separately by callers because it's a "remote replied" condition vs a
// "wire broke" condition.
static bool i2c_bus_error(void) {
    return (I2C_SERCOM->I2CM.STATUS.bit.BUSERR != 0)
        || (I2C_SERCOM->I2CM.STATUS.bit.ARBLOST != 0);
}

static void i2c_stop(void) {
    I2C_SERCOM->I2CM.CTRLB.bit.CMD = 3;  // STOP
    while (I2C_SERCOM->I2CM.SYNCBUSY.bit.SYSOP) { /* spin */ }
}

// START + addr, write or read direction. Returns true if address ACKed.
// On NACK or bus error, leaves the bus in an indeterminate state; caller
// must issue STOP to recover (i2c_stop()).
static bool i2c_start(uint8_t addr, bool read) {
    I2C_SERCOM->I2CM.ADDR.reg = ((uint32_t)addr << 1) | (read ? 1u : 0u);
    i2c_wait_complete();
    if (i2c_bus_error()) return false;
    return (I2C_SERCOM->I2CM.STATUS.bit.RXNACK == 0);
}

// Write one byte. Returns true if ACKed.
static bool i2c_write_byte(uint8_t data) {
    I2C_SERCOM->I2CM.DATA.reg = data;
    i2c_wait_complete();
    if (i2c_bus_error()) return false;
    return (I2C_SERCOM->I2CM.STATUS.bit.RXNACK == 0);
}

// Read one byte. If is_last, the next-byte ACK is set to NACK (signals
// the slave we're done). Caller should follow last-byte read with i2c_stop().
static uint8_t i2c_read_byte(bool is_last) {
    // ACKACT must be set BEFORE reading DATA (reading DATA triggers next byte).
    I2C_SERCOM->I2CM.CTRLB.bit.ACKACT = is_last ? 1 : 0;
    while (I2C_SERCOM->I2CM.SYNCBUSY.bit.SYSOP) { /* spin */ }
    uint8_t data = (uint8_t)I2C_SERCOM->I2CM.DATA.reg;
    if (!is_last) {
        // Trigger next byte read (CMD=2 = read).
        I2C_SERCOM->I2CM.CTRLB.bit.CMD = 2;
        while (I2C_SERCOM->I2CM.SYNCBUSY.bit.SYSOP) { /* spin */ }
        i2c_wait_complete();
    }
    return data;
}

// --- shell commands ----------------------------------------------------

#define I2C_MAX_WRITE_LEN  32u  // fits within shell args + leaves frame headroom
#define I2C_MAX_READ_LEN   60u  // fits within shell result_message budget

// CMD_I2C_WRITE: addr:u8 (7-bit), data:u8[1..32]
// Sequence: START + addr(W) + data... + STOP. NACK at any byte → STOP + BAD_ARGS.
static uint8_t cmd_i2c_write(shell_reader_t* args, shell_writer_t* result) {
    (void)result;
    uint8_t addr = sr_u8(args);
    if (args->overflow)             return SHELL_STATUS_BAD_ARGS;
    if (addr > 0x7Fu)               return SHELL_STATUS_BAD_ARGS;
    uint16_t n = sr_remaining(args);
    if (n == 0u || n > I2C_MAX_WRITE_LEN) return SHELL_STATUS_BAD_ARGS;

    if (!i2c_start(addr, false)) { i2c_stop(); return SHELL_STATUS_BAD_ARGS; }
    for (uint16_t i = 0; i < n; i++) {
        uint8_t b = sr_u8(args);
        if (!i2c_write_byte(b)) { i2c_stop(); return SHELL_STATUS_BAD_ARGS; }
    }
    i2c_stop();
    return SHELL_STATUS_OK;
}

// CMD_I2C_READ: addr:u8, count:u8 (1..60)
// Sequence: START + addr(R) + read count bytes + STOP.
// Returns bytes read on success.
static uint8_t cmd_i2c_read(shell_reader_t* args, shell_writer_t* result) {
    uint8_t addr  = sr_u8(args);
    uint8_t count = sr_u8(args);
    if (args->overflow)            return SHELL_STATUS_BAD_ARGS;
    if (sr_remaining(args) != 0)   return SHELL_STATUS_BAD_ARGS;
    if (addr > 0x7Fu)              return SHELL_STATUS_BAD_ARGS;
    if (count == 0u || count > I2C_MAX_READ_LEN) return SHELL_STATUS_BAD_ARGS;

    if (!i2c_start(addr, true)) { i2c_stop(); return SHELL_STATUS_BAD_ARGS; }
    for (uint8_t i = 0; i < count; i++) {
        uint8_t b = i2c_read_byte(i == (count - 1u));
        sw_u8(result, b);
        if (result->overflow) { i2c_stop(); return SHELL_STATUS_RESULT_TOO_BIG; }
    }
    i2c_stop();
    return SHELL_STATUS_OK;
}

// CMD_I2C_WRITE_READ: addr:u8, write_count:u8, read_count:u8, write_data:u8[write_count]
// Sequence: START + addr(W) + write_data + repeated-START + addr(R) + read read_count + STOP.
// The canonical sensor pattern: "set register pointer, then read N bytes".
static uint8_t cmd_i2c_write_read(shell_reader_t* args, shell_writer_t* result) {
    uint8_t addr        = sr_u8(args);
    uint8_t write_count = sr_u8(args);
    uint8_t read_count  = sr_u8(args);
    if (args->overflow)             return SHELL_STATUS_BAD_ARGS;
    if (addr > 0x7Fu)               return SHELL_STATUS_BAD_ARGS;
    if (write_count == 0u || write_count > I2C_MAX_WRITE_LEN) return SHELL_STATUS_BAD_ARGS;
    if (read_count  == 0u || read_count  > I2C_MAX_READ_LEN)  return SHELL_STATUS_BAD_ARGS;
    if (sr_remaining(args) != write_count) return SHELL_STATUS_BAD_ARGS;

    // Write phase
    if (!i2c_start(addr, false)) { i2c_stop(); return SHELL_STATUS_BAD_ARGS; }
    for (uint8_t i = 0; i < write_count; i++) {
        uint8_t b = sr_u8(args);
        if (!i2c_write_byte(b)) { i2c_stop(); return SHELL_STATUS_BAD_ARGS; }
    }
    // Repeated START to switch to read phase (no STOP between).
    if (!i2c_start(addr, true)) { i2c_stop(); return SHELL_STATUS_BAD_ARGS; }
    for (uint8_t i = 0; i < read_count; i++) {
        uint8_t b = i2c_read_byte(i == (read_count - 1u));
        sw_u8(result, b);
        if (result->overflow) { i2c_stop(); return SHELL_STATUS_RESULT_TOO_BIG; }
    }
    i2c_stop();
    return SHELL_STATUS_OK;
}

// CMD_I2C_SCAN: no args. Probes 0x08..0x77 with a zero-byte write.
// Returns: addresses:u8[N] (only addresses that ACKed).
static uint8_t cmd_i2c_scan(shell_reader_t* args, shell_writer_t* result) {
    if (sr_remaining(args) != 0) return SHELL_STATUS_BAD_ARGS;
    for (uint8_t addr = 0x08u; addr <= 0x77u; addr++) {
        bool acked = i2c_start(addr, false);
        i2c_stop();
        if (acked) {
            sw_u8(result, addr);
            if (result->overflow) return SHELL_STATUS_RESULT_TOO_BIG;
        }
    }
    return SHELL_STATUS_OK;
}

// ---------- CMD_TEST_HANG -------------------------------------------------
// Layer-2 WDT bench probe. Disables IRQs and spins; the layer-2 WDT bites
// after ~4 s and the chip resets. Never returns a reply frame.
static uint8_t cmd_test_hang(shell_reader_t* args, shell_writer_t* result) {
    (void)args; (void)result;
    __disable_irq();
    for (;;) { __NOP(); }
    return SHELL_STATUS_OK;  // unreachable
}

// ---------- Interlock framework (slice 1) ---------------------------------
#include "samd21_interlocks.h"

static uint8_t cmd_interlock_status(shell_reader_t* args, shell_writer_t* result) {
    if (sr_remaining(args) != 0) return SHELL_STATUS_BAD_ARGS;
    // v2 status: [ver][nslots] + per-slot {state,id,bc,tf,name[16]} + crash. The
    // exact same bytes the slave pushes as its buffer-2 interlock message, so the
    // Pi has ONE parser whether it pulls (here) or receives the async push.
    uint8_t buf[64];
    uint16_t n = interlock_build_status_v2(buf);
    sw_bytes(result, buf, n);
    if (result->overflow) return SHELL_STATUS_RESULT_TOO_BIG;
    return SHELL_STATUS_OK;
}

// 7b piece 3 — the dumb-slave re-push: just flag the request; the slave poll loop
// re-emits buffer 2 on the next poll. No state, no decisions — the Pi owns those.
static uint8_t cmd_interlock_repush(shell_reader_t* args, shell_writer_t* result) {
    (void)result;
    if (sr_remaining(args) != 0) return SHELL_STATUS_BAD_ARGS;
    interlock_request_repush();
    return SHELL_STATUS_OK;
}

static uint8_t cmd_interlock_set(shell_reader_t* args, shell_writer_t* result) {
    uint8_t slot = sr_u8(args);
    if (args->overflow) return SHELL_STATUS_BAD_ARGS;
    uint16_t text_len = sr_remaining(args);
    if (text_len == 0) return SHELL_STATUS_BAD_ARGS;
    const char* text = (const char*)args->p;
    uint8_t err_payload[3] = {0, 0, 0};
    uint8_t st = interlock_set_slot_dsl(slot, text, text_len, err_payload);
    if (st == SHELL_STATUS_BAD_ARGS) {
        sw_u8(result, err_payload[0]);
        sw_u8(result, err_payload[1]);
        sw_u8(result, err_payload[2]);
        if (result->overflow) return SHELL_STATUS_RESULT_TOO_BIG;
    } else if (st == SHELL_STATUS_BUSY) {
        // payload[0]: 0xFF = pin claim conflict, 0 = slot already armed.
        // payload[1] (claim-conflict only): hal_pin_claim_status_t sub-reason
        // (3=TAKEN, 6=VALUE_MISMATCH, 2=RESERVED, ...). Slot-already-armed
        // path leaves [1]=0 which the host renders as the generic message.
        sw_u8(result, err_payload[0]);
        sw_u8(result, err_payload[1]);
        if (result->overflow) return SHELL_STATUS_RESULT_TOO_BIG;
    }
    return st;
}

static uint8_t cmd_interlock_arm_noop(shell_reader_t* args, shell_writer_t* result) {
    uint8_t slot = sr_u8(args);
    if (args->overflow || sr_remaining(args) != 0) return SHELL_STATUS_BAD_ARGS;
    (void)result;
    return interlock_arm_slot_noop(slot);
}

static uint8_t cmd_interlock_disarm(shell_reader_t* args, shell_writer_t* result) {
    uint8_t slot = sr_u8(args);
    if (args->overflow || sr_remaining(args) != 0) return SHELL_STATUS_BAD_ARGS;
    (void)result;
    return interlock_disarm_slot(slot);
}

// Slice-4 stack hardening: surface the runtime stack budget + peak depth.
static uint8_t cmd_stack_hwm(shell_reader_t* args, shell_writer_t* result) {
    if (sr_remaining(args) != 0) return SHELL_STATUS_BAD_ARGS;
    extern volatile uint16_t g_stack_hwm_bytes;
    extern volatile uint16_t g_stack_size_bytes;
    extern volatile uint8_t  g_stack_canary_tripped;
    sw_u16(result, g_stack_hwm_bytes);
    sw_u16(result, g_stack_size_bytes);
    sw_u8 (result, g_stack_canary_tripped);
    if (result->overflow) return SHELL_STATUS_RESULT_TOO_BIG;
    return SHELL_STATUS_OK;
}

// ---------- chip-specific dispatch table ---------------------------------

// Chip-specific command surface. The bus_controller is "nothing but a bus
// manager" — it exposes ONLY the RS-485 transport commands + the stack-HWM
// diagnostic. All HIL (GPIO/DAC/ADC/I2C), the interlock command set, and the
// WDT-hang probe are stripped from the bus_controller build. (The interlock
// *framework* in main.c still compiles for now; removing that for text savings
// is a separate lean-build follow-up.) The slave keeps the full HIL surface so
// the workbench runs on it over the bus.
static const shell_cmd_entry_t g_chip_commands[] = {
#if !defined(ROLE_BUS_CONTROLLER)
    { CMD_GPIO_CONFIG, "gpio_config", cmd_gpio_config },
    { CMD_GPIO_WRITE,  "gpio_write",  cmd_gpio_write  },
    { CMD_GPIO_READ,   "gpio_read",   cmd_gpio_read   },
    { CMD_DAC_WRITE,           "dac_write",          cmd_dac_write          },
    { CMD_ADC_READ,            "adc_read",           cmd_adc_read           },
    { CMD_DAC_WAVEFORM_WRITE,  "dac_waveform_write", cmd_dac_waveform_write },
    { CMD_DAC_STOP,            "dac_stop",           cmd_dac_stop           },
    { CMD_DAC_FOLLOW_START,    "dac_follow_start",   cmd_dac_follow_start   },
    { CMD_DAC_FOLLOW_STOP,     "dac_follow_stop",    cmd_dac_follow_stop    },
    { CMD_ADC_CAPTURE,         "adc_capture",        cmd_adc_capture        },
    { CMD_I2C_WRITE,           "i2c_write",          cmd_i2c_write          },
    { CMD_I2C_READ,            "i2c_read",           cmd_i2c_read           },
    { CMD_I2C_WRITE_READ,      "i2c_write_read",     cmd_i2c_write_read     },
    { CMD_I2C_SCAN,            "i2c_scan",           cmd_i2c_scan           },
#ifdef I2C_CLIENT
    { CMD_FILE_BEGIN,          "file_begin",         cmd_file_begin         },
    { CMD_FILE_DATA,           "file_data",          cmd_file_data          },
    { CMD_FILE_COMMIT,         "file_commit",        cmd_file_commit        },
    { CMD_FILE_LIST,           "file_list",          cmd_file_list          },
    { CMD_REG_READ,            "reg_read",           cmd_reg_read           },
    { CMD_REG_WRITE,           "reg_write",          cmd_reg_write          },
    { CMD_REG_READN,           "reg_readn",          cmd_reg_readn          },
    { CMD_OFFLINE,             "offline",            cmd_offline            },
#endif
#endif
#if !defined(ROLE_BUS_CONTROLLER)
    { CMD_TEST_HANG,           "test_hang",          cmd_test_hang          },
    { CMD_INTERLOCK_STATUS,    "interlock_status",   cmd_interlock_status   },
    { CMD_INTERLOCK_ARM_NOOP,  "interlock_arm_noop", cmd_interlock_arm_noop },
    { CMD_INTERLOCK_DISARM,    "interlock_disarm",   cmd_interlock_disarm   },
    { CMD_INTERLOCK_SET,       "interlock_set",      cmd_interlock_set      },
    { CMD_INTERLOCK_REPUSH,    "interlock_repush",   cmd_interlock_repush   },
#endif
    { CMD_STACK_HWM,           "stack_hwm",          cmd_stack_hwm          },
};

const shell_cmd_entry_t* chip_commands_table(void) {
    return g_chip_commands;
}

uint8_t chip_commands_count(void) {
    return (uint8_t)(sizeof(g_chip_commands) / sizeof(g_chip_commands[0]));
}

// ============================================================================
// samd21_peripherals_init — boot-time init of statically-allocated peripherals.
// Called once from main() after hal_wdt_init(). DAC + ADC are always-on; their
// pins (PA02 for DAC) are locked out from GPIO commands via pin_is_reserved.
// I2C (SERCOM2) and RS-485 (SERCOM4) will hook in here in later commits.
// ============================================================================
void samd21_peripherals_init(void) {
    dac_init();
    adc_init();
#ifdef I2C_CLIENT
    i2c_slave_init();   // SERCOM2 = I2C slave to the Pico master (D4/D5)
#else
    i2c_init();         // SERCOM2 = I2C master for the dongle's own devices
#endif
}
