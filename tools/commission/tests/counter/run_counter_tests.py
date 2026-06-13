"""run_counter_tests.py -- run the COUNTER-mode serial tests, summarize.

REQUIRES the A0->A1 jumper (A1 = AIN4 = D1, so it's the DAC -> D1 stimulus path).

    ssh robot
    cd /tmp/commission/tests/counter
    python3 run_counter_tests.py
"""

import importlib
import sys

TESTS = ["test_counter", "test_counter_bench"]


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
