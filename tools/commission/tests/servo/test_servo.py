"""test_servo.py -- SERVO mode: config roles, enable gating, width clamp, e-stop.

Commission (8 servo-bank pads + D6 e-stop):
  D0, D1 = servo channels
  D9     = gpio-out  (drives the e-stop line via the D9->D6 jumper)
  D8     = adc       D10 = in   (role readback only)
  D6     = e-stop interlock (always; input pull-up, active-low)

REQUIRES the D9->D6 jumper: the D9 bench output drives the e-stop line so the test
can assert (low) / release (high) the e-stop and watch the latch + restart logic.
NOTE: at boot D9 (a gpio-out pad) starts low, so the e-stop reads asserted until the
test drives D9 high -- the suite does that first.

Pulse-WIDTH timing is not measured here (needs a scope / the Pico HIL capture); this
suite verifies the control/config logic. Width is checked via the clamp readback.
"""

from servo_harness import (R, Checker, commission, servo_enable, servo_width,
                           servo_ctrl, servo_state, bench_select, bench_role, bench_gpo,
                           BENCH_OK, BENCH_BAD_PIN, ROLE_IN, ROLE_OUT, ROLE_ADC)
import slave_dsl
import time


def run():
    c = Checker()
    u = slave_dsl.Unit(0x55, "SERVO").servo()
    u.pins(D0="servo", D1="servo", D9="out", D8="adc", D10="in")
    dg, _ = commission(u)
    c.eq("MODE (4=SERVO)", dg.reg_read(R["MODE"]), 4)

    # 1. Servo + e-stop pads are reserved (not benchable); bench pads report roles.
    c.eq("SEL D0 (servo) -> BAD_PIN", bench_select(dg, "D0"), BENCH_BAD_PIN)
    c.eq("SEL D1 (servo) -> BAD_PIN", bench_select(dg, "D1"), BENCH_BAD_PIN)
    c.eq("SEL D6 (e-stop) -> BAD_PIN", bench_select(dg, "D6"), BENCH_BAD_PIN)
    c.eq("role D9 = out", bench_role(dg, "D9"), ROLE_OUT)
    c.eq("role D8 = adc", bench_role(dg, "D8"), ROLE_ADC)
    c.eq("role D10 = in", bench_role(dg, "D10"), ROLE_IN)

    # 2. ENABLE gating: only declared servo pads (CH0,CH1) can be enabled.
    c.eq("enable 0xFF -> declared only (0x03)", servo_enable(dg, 0xFF), 0x03)
    c.eq("enable 0x02 -> CH1", servo_enable(dg, 0x02), 0x02)
    c.eq("enable CH2 (undeclared) -> masked 0", servo_enable(dg, 0x04), 0x00)
    servo_enable(dg, 0x03)

    # 3. Width clamp on CH0 (500..2500 us).
    c.eq("width 1500 -> 1500", servo_width(dg, 0, 1500), 1500)
    c.eq("width 3000 -> clamp 2500", servo_width(dg, 0, 3000), 2500)
    c.eq("width 200 -> clamp 500", servo_width(dg, 0, 200), 500)
    c.eq("width 900 -> 900", servo_width(dg, 0, 900), 900)

    # 4. E-stop latch + restart (D9->D6 jumper drives the e-stop line).
    bench_gpo(dg, "D9", 1); time.sleep(0.03)          # release the e-stop (line high)
    c.eq("released: e-stop line high", servo_state(dg)["estop"], False)
    servo_ctrl(dg, True); time.sleep(0.02)            # START while clear -> running
    st = servo_state(dg)
    c.eq("START clear -> running", (st["run"], st["estop"]), (True, False))

    bench_gpo(dg, "D9", 0); time.sleep(0.03)          # assert e-stop (line low)
    st = servo_state(dg)
    c.eq("e-stop low -> stopped+latched", (st["run"], st["latched"], st["estop"]),
         (False, True, True))
    servo_ctrl(dg, True); time.sleep(0.02)            # START while faulted -> rejected
    c.eq("START while faulted -> still stopped", servo_state(dg)["run"], False)

    bench_gpo(dg, "D9", 1); time.sleep(0.03)          # release, latch persists
    st = servo_state(dg)
    c.eq("released but latch persists", (st["run"], st["latched"], st["estop"]),
         (False, True, False))
    servo_ctrl(dg, True); time.sleep(0.02)            # START clears latch -> running
    st = servo_state(dg)
    c.eq("START clears latch -> running", (st["run"], st["latched"]), (True, False))
    servo_ctrl(dg, False)                             # STOP -> limp
    c.eq("STOP -> stopped", servo_state(dg)["run"], False)

    dg.close()
    return c.done("test_servo")


if __name__ == "__main__":
    import sys
    sys.exit(0 if run() else 1)
