"""test_counter.py -- COUNTER mode: cntr config, edge counting, read-all (needs A0->A1 jumper).

D1 is commissioned as a counter (D0 left free so the DAC owns A0); the A0->A1 (=D1)
jumper feeds the DAC square into D1. Assertions are frequency-INDEPENDENT (see the
harness note on the board's slow timer clock):

  1. cntr applied -> MODE=COUNTER, ENABLE bitmap has D1 only.
  2. DC source -> 0 edges (proves D1 is driven, not floating; no false edges).
  3. Square -> edges accumulate on D1, and only D1 (enabled-only).
  4. READ_CLR zeroes; READ leaves the count running.
"""

from counter_harness import R, Checker, commission, dac_dc, dac_square, read_all, enable_bitmap
import slave_dsl
import time

WIN = 1.0   # accumulate window


def run():
    c = Checker()
    u = slave_dsl.Unit(0x55, "COUNTER").counter(rate=1000)
    u.pins(D1="count:none:both")            # D1 counts both edges; D0 free for the DAC
    dg, _ = commission(u)
    c.eq("MODE (5=COUNTER)", dg.reg_read(R["MODE"]), 5)
    c.eq("ENABLE bitmap (D1=bit1)", enable_bitmap(dg), 0b10)

    # 2. DC high / low -> no edges (D1 is driven by the DAC, just not toggling)
    for level in (900, 100):
        dac_dc(dg, level)
        read_all(dg, clear=True)            # zero
        time.sleep(WIN)
        cnt = read_all(dg)
        c.eq("DC=%d -> D1 edges" % level, cnt[1], 0)

    # 3. Square -> D1 counts; other channels stay 0
    dac_square(dg, freq=200, amp=511, offset=512)
    time.sleep(0.3)
    read_all(dg, clear=True)
    time.sleep(WIN)
    cnt = read_all(dg)
    print("    (square: D1=%d edges over %.0fs)" % (cnt[1], WIN))
    c.eq("square -> D1 counts (>0)", cnt[1] > 0, True)
    c.eq("disabled channels stay 0", sum(cnt) - cnt[1], 0)

    # 4. READ_CLR zeroes; READ does not
    read_all(dg, clear=True)                # zero
    a = read_all(dg, clear=True)[1]         # immediately after clear -> ~0 (few stray edges ok)
    c.eq("READ_CLR zeroes (D1<5)", a < 5, True)
    time.sleep(0.3)
    b1 = read_all(dg)[1]                    # READ (no clear)
    b2 = read_all(dg)[1]                    # READ again -> not reset, >= previous
    c.eq("READ does not clear (D1 grows)", b2 >= b1 and b1 > 0, True)

    dg.close()
    return c.done("test_counter")


if __name__ == "__main__":
    import sys
    sys.exit(0 if run() else 1)
