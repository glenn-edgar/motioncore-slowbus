#ifndef ARM_DSP_CONFIG_H
#define ARM_DSP_CONFIG_H
// Minimal stand-in for the CMSIS-DSP build-generated arm_dsp_config.h. We compile
// only a curated subset of CMSIS-DSP (f32 FIR decimation) for the RP2350 (M33 +
// DSP extension, no Helium/MVE), so the architecture is autodetected from the
// compiler macros and no extra config is needed here. Loop-unroll on for speed.
#define ARM_MATH_LOOPUNROLL
#endif // ARM_DSP_CONFIG_H
