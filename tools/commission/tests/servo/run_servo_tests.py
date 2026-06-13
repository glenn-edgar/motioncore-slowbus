"""run_servo_tests.py -- run the SERVO-mode serial tests, summarize.

REQUIRES the D9->D6 jumper (D9 bench output drives the e-stop line for the
latch/restart checks).

    ssh robot
    cd /tmp/commission/tests/servo
    python3 run_servo_tests.py
"""

import importlib
import sys

TESTS = ["test_servo"]


def main():
    results = []
    for name in TESTS:
        print("\n========== %s ==========" % name)
        try:
            ok = importlib.import_module(name).run()
        except Exception as e:
            print("  EXCEPTION:", e)
            ok = False
        results.append((name, ok))
    print("\n================ summary ================")
    for name, ok in results:
        print("  %-22s %s" % (name, "PASS" if ok else "FAIL"))
    npass = sum(1 for _, ok in results if ok)
    print("  %d/%d passed" % (npass, len(results)))
    return 0 if npass == len(results) else 1


if __name__ == "__main__":
    sys.exit(main())
