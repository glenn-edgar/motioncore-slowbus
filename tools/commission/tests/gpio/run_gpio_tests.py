"""run_gpio_tests.py -- run the non-interactive GPIO serial tests, summarize.

    ssh robot
    cd /tmp/commission/tests/gpio   # (staged: harness + tests + ../../{libcomm,slave_dsl}.py)
    python3 run_gpio_tests.py

The jumper test (test_gpio_wired_or.py) is interactive -- run it on its own.
"""

import importlib
import sys

TESTS = ["test_gpio_pinmap", "test_gpio_outputs", "test_gpio_interlock"]


def main():
    results = []
    for name in TESTS:
        print("\n========== %s ==========" % name)
        try:
            ok = importlib.import_module(name).run()
        except Exception as e:               # keep going; report the failure
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
