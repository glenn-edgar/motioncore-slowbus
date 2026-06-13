"""run_mixed_tests.py -- run the MIXED-mode serial tests, summarize.

REQUIRES the A0->A1 DAC jumper (the DAC is the controllable ADC source).

    ssh robot
    cd /tmp/commission/tests/mixed   # (staged: harness + tests + ../../{libcomm,slave_dsl}.py)
    python3 run_mixed_tests.py
"""

import importlib
import sys

TESTS = ["test_mixed_interlock", "test_mixed_trip", "test_mixed_oc"]


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
