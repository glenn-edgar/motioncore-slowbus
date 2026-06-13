"""test_adc_twotone.py -- two-tone DDS DAC validated through the A0->A1 jumper.

Drives the DAC's two independent DDS tones and reads the envelope / RMS the ADC
sampler sees on A1. The sweep runs at ~125 Hz/channel, so these tests use low
frequencies (<=150 Hz) where window min/max/rms are still meaningful; note the
windows take ~0.8 s to fill at that rate, so each step settles ~2 s.

Self-calibrates the DAC-count -> ADC-count gain k from a DC point (both tones
off, output = offset), then predicts each waveform's envelope. Validates:
  * sine   -- avg=offset, peak=offset+-amp, rms=amp/sqrt2
  * square -- avg=offset, rms=amp (full-swing)
  * two summed sines -- wider envelope + combined rms sqrt(r1^2+r2^2)
  * hard clip -- offset 512 + two big sines saturate the DAC at 0 / 1023

Needs the A0->A1 jumper (the same one the rest of the ADC suite uses).
"""

from adc_harness import (R, Checker, commission, set_tone, set_offset, dac_apply,
                         read_avg, read_stats, T_OFF, T_SINE, T_SQUARE, WIN)
import math
import slave_dsl
import time

SETTLE = 2.0   # fast window ~0.8 s at ~125 Hz/ch; ~2 windows to flush old + fill fresh
OFF = 512      # DC center used throughout


def near(got, exp, tol):
    return abs(got - exp) <= tol


def run():
    dg, _ = commission(slave_dsl.Unit(0x55, "ADC"))   # bare ADC unit, null ilcf
    c = Checker()
    c.eq("MODE (2=ADC)", dg.reg_read(R["MODE"]), 2)

    # --- calibrate gain k = ADC counts per DAC count (tones off, DC = 512) ---
    set_tone(dg, 0, T_OFF); set_tone(dg, 1, T_OFF); set_offset(dg, OFF); dac_apply(dg)
    time.sleep(SETTLE)
    dc = read_avg(dg, 0, WIN["fast"])
    k = dc / float(OFF)
    print("    gain k = %.3f ADC-counts/DAC-count (DC=%d -> %d)" % (k, OFF, dc))
    c.eq("gain ~4.0", 3.7 <= k <= 4.3, True)

    # --- single sine tone @100 Hz, amp 300 ---
    set_tone(dg, 0, T_SINE, amp=300, freq=100); set_tone(dg, 1, T_OFF)
    set_offset(dg, OFF); dac_apply(dg); time.sleep(SETTLE)
    s = read_stats(dg, 0, WIN["fast"])
    print("    sine  A300 f100 : min=%d max=%d avg=%d rms=%d" % (s["min"], s["max"], s["avg"], s["rms"]))
    c.eq("sine avg~offset",    near(s["avg"], OFF * k,         0.04 * OFF * k), True)
    c.eq("sine max~off+amp",   near(s["max"], (OFF + 300) * k, 0.12 * 300 * k), True)
    c.eq("sine min~off-amp",   near(s["min"], (OFF - 300) * k, 0.12 * 300 * k), True)
    c.eq("sine rms~amp/sqrt2", near(s["rms"], 300 / 1.4142 * k, 0.20 * 300 / 1.4142 * k), True)

    # --- single square tone @100 Hz, amp 250: levels + rms ~ full amp ---
    # NOTE: avg is NOT asserted for non-sine AC. The ADC is keyed off the same
    # TC3 tone clock that generates the DAC, so sampling is phase-locked to the
    # waveform (coherent). A square's fixed sample phases bias the mean (avg ~1800
    # not 2048), though min/max/rms are exact. The DC mean is already proven by the
    # calibration point and the sine avg above.
    set_tone(dg, 0, T_SQUARE, amp=250, freq=100); set_tone(dg, 1, T_OFF)
    set_offset(dg, OFF); dac_apply(dg); time.sleep(SETTLE)
    s = read_stats(dg, 0, WIN["fast"])
    print("    sqr   A250 f100 : min=%d max=%d avg=%d rms=%d" % (s["min"], s["max"], s["avg"], s["rms"]))
    c.eq("square hi~off+amp", near(s["max"], (OFF + 250) * k, 0.10 * 250 * k), True)
    c.eq("square lo~off-amp", near(s["min"], (OFF - 250) * k, 0.10 * 250 * k), True)
    c.eq("square rms~amp",    near(s["rms"], 250 * k, 0.20 * 250 * k), True)

    # --- two tones summed: sine 200@80 Hz + sine 150@130 Hz ---
    set_tone(dg, 0, T_SINE, amp=200, freq=80); set_tone(dg, 1, T_SINE, amp=150, freq=130)
    set_offset(dg, OFF); dac_apply(dg); time.sleep(SETTLE)
    s = read_stats(dg, 0, WIN["fast"])
    print("    2tone 200@80+150@130: min=%d max=%d avg=%d rms=%d" % (s["min"], s["max"], s["avg"], s["rms"]))
    c.eq("2tone max>single", s["max"] > (OFF + 200) * k, True)     # crests sum above either alone
    c.eq("2tone min<single", s["min"] < (OFF - 200) * k, True)
    rms_pred = math.sqrt(200 ** 2 + 150 ** 2) / 1.4142 * k         # uncorrelated power sum
    c.eq("2tone rms~combined", near(s["rms"], rms_pred, 0.22 * rms_pred), True)

    # --- hard clip: offset 512 + two sines amp 400 each -> sum saturates 0/1023 ---
    set_tone(dg, 0, T_SINE, amp=400, freq=80); set_tone(dg, 1, T_SINE, amp=400, freq=130)
    set_offset(dg, OFF); dac_apply(dg); time.sleep(SETTLE)
    s = read_stats(dg, 0, WIN["fast"])
    print("    clip  400+400  : min=%d max=%d avg=%d rms=%d" % (s["min"], s["max"], s["avg"], s["rms"]))
    c.eq("clip max saturates", s["max"] >= int(1000 * k), True)    # peak pinned near full-scale
    c.eq("clip min floors",    s["min"] <= int(30 * k), True)      # trough pinned near 0

    # park the DAC quiet
    set_tone(dg, 0, T_OFF); set_tone(dg, 1, T_OFF); set_offset(dg, OFF); dac_apply(dg)
    dg.close()
    return c.done("test_adc_twotone")


if __name__ == "__main__":
    import sys
    sys.exit(0 if run() else 1)
