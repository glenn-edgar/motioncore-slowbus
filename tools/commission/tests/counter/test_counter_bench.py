"""test_counter_bench.py -- COUNTER mode bench tools on the spare (non-counter) pads.

Exercises the fixed-role bench: a spare pad is commissioned `dac` / `adc` / `out` /
`in` and the bench API (request/get over the register bank) validates each op
against that role, reporting via BENCH_STAT. Addressed by Seeed pad name.

Commission for this test (no counters -- all pads are bench roles):
  A0 = dac   (square/DC stimulus on D0/PA02)
  D1 = adc   (16x oneshot; A1 = AIN4 = D1)
  D9 = out   (gpio write)        D10 = in  (gpio read)
  D2..        undeclared -> role NONE (every op errors)

REQUIRES two jumpers:
  A0->A1  (A1 = D1): feeds the DAC into the ADC bench pad (ADC-tracks-DAC check).
  D9->D10         : gpio-out drives gpio-in (the write->read loopback check).
The role/error checks need no wiring.
"""

from counter_harness import (R, Checker, commission, dac_dc,
                             bench_select, bench_role, bench_gpo, bench_gpi,
                             bench_adc, bench_unsupported,
                             BENCH_OK, BENCH_BAD_PIN, BENCH_WRONG_ROLE, BENCH_UNSUPPORTED,
                             ROLE_NONE, ROLE_IN, ROLE_OUT, ROLE_ADC, ROLE_DAC)
import slave_dsl
import time


def run():
    c = Checker()
    u = slave_dsl.Unit(0x55, "COUNTER").counter(rate=1000)
    u.pins(A0="dac", D1="adc", D9="out", D10="in")
    dg, _ = commission(u)
    c.eq("MODE (5=COUNTER)", dg.reg_read(R["MODE"]), 5)

    # 1. ROLE readback per pad (commissioned roles, Seeed-name addressed).
    c.eq("role A0 = dac", bench_role(dg, "A0"), ROLE_DAC)
    c.eq("role D1 = adc", bench_role(dg, "D1"), ROLE_ADC)
    c.eq("role D9 = out", bench_role(dg, "D9"), ROLE_OUT)
    c.eq("role D10 = in", bench_role(dg, "D10"), ROLE_IN)
    c.eq("role D2 = none (undeclared)", bench_role(dg, "D2"), ROLE_NONE)
    c.eq("A1 aliases D1 (=adc)", bench_role(dg, "A1"), ROLE_ADC)

    # 2. Selecting an unbenchable pad -> BAD_PIN (D4 = I2C bus).
    c.eq("SEL D4 (I2C) -> BAD_PIN", bench_select(dg, "D4"), BENCH_BAD_PIN)
    c.eq("SEL D1 (adc) -> OK",      bench_select(dg, "D1"), BENCH_OK)

    # 3. ADC tracks the DAC (A0->A1 jumper). DAC 0..1023 -> ~4x ADC counts.
    pts = []
    for lvl in (128, 512, 896):
        dac_dc(dg, lvl)
        time.sleep(0.05)
        val, st = bench_adc(dg, "D1")
        print("    (DAC=%d -> bench ADC D1 = %d, stat=%d)" % (lvl, val, st))
        c.eq("ADC@DAC%d stat OK" % lvl, st, BENCH_OK)
        pts.append(val)
    c.eq("ADC monotonic with DAC", pts[0] < pts[1] < pts[2], True)
    c.eq("ADC low (<800 @ DAC128)", pts[0] < 800, True)
    c.eq("ADC mid (~2040 @ DAC512)", 1700 < pts[1] < 2400, True)
    c.eq("ADC high (>3000 @ DAC896)", pts[2] > 3000, True)

    # 3b. GPIO out->in loopback (D9->D10 jumper): driving D9 is read back on D10.
    for lvl in (0, 1, 0, 1):
        c.eq("GPO D9=%d stat OK" % lvl, bench_gpo(dg, "D9", lvl), BENCH_OK)
        time.sleep(0.01)
        got, st = bench_gpi(dg, "D10")
        c.eq("GPI D10 reads D9=%d" % lvl, (got, st), (lvl, BENCH_OK))

    # 4. Role mismatch -> WRONG_ROLE (and reads return the 0xFF/0xFFFF sentinel).
    c.eq("GPO on adc pad -> WRONG_ROLE", bench_gpo(dg, "D1", 1), BENCH_WRONG_ROLE)
    lvl, st = bench_gpi(dg, "D1")
    c.eq("GPI on adc pad -> WRONG_ROLE", st, BENCH_WRONG_ROLE)
    c.eq("GPI wrong-role data = 0xFF", lvl, 0xFF)
    c.eq("GPI on out pad -> WRONG_ROLE", bench_gpi(dg, "D9")[1], BENCH_WRONG_ROLE)
    c.eq("GPO on in pad -> WRONG_ROLE", bench_gpo(dg, "D10", 1), BENCH_WRONG_ROLE)
    c.eq("ADCRQ on out pad -> WRONG_ROLE", bench_adc(dg, "D9")[1], BENCH_WRONG_ROLE)
    c.eq("ADCRQ on none pad -> WRONG_ROLE", bench_adc(dg, "D2")[1], BENCH_WRONG_ROLE)

    # 5. Unknown bench register -> UNSUPPORTED.
    c.eq("unknown bench reg -> UNSUPPORTED", bench_unsupported(dg), BENCH_UNSUPPORTED)

    dg.close()
    return c.done("test_counter_bench")


if __name__ == "__main__":
    import sys
    sys.exit(0 if run() else 1)
