"""run_adc_tests.py -- run the ADC-mode serial tests, summarize.

REQUIRES the A0->A1 DAC self-test jumper.

    ssh robot
    cd /tmp/commission/tests/adc
    python3 run_adc_tests.py
"""

import importlib
import sys

TESTS = ["test_adc_selftest", "test_adc_interlock"]


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
