"""The test-hil recipe forwards HIL_EXT only when the caller set it.

`make test-hil` builds the bench profile's EXT flags when HIL_EXT is unset and
must let an explicit empty value opt out for an externally powered bus. The
harness tells those apart by whether the variable is present at all, so a
Makefile default would erase the difference and silently drop the parasite
pull-up - a bug that no host test of the C code can see.

No bench is needed: `make -n` prints the recipe, and the recipe is the whole
contract. The check runs with HIL_EXT removed from the environment so a
developer's shell cannot make an unset case look set.
"""

import os
import shutil
import subprocess
import sys

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

failures = []


def check(name, ok, detail=""):
    if ok:
        print("  ok   " + name)
    else:
        print("  FAIL " + name + (" (%s)" % detail if detail else ""))
        failures.append(name)


def recipe(extra=(), env=None):
    """The one test-hil command make would run, with `extra` make arguments."""
    make = shutil.which("make")
    if make is None:
        raise SystemExit("make not found: the test-hil contract cannot be checked")
    child_env = os.environ.copy()
    child_env.pop("HIL_EXT", None)
    if env:
        child_env.update(env)
    proc = subprocess.run(
        [make, "-n", "test-hil", "HIL_SCRIPT=ds2431"] + list(extra),
        cwd=REPO,
        env=child_env,
        capture_output=True,
        text=True,
    )
    if proc.returncode != 0:
        raise SystemExit(
            "make -n test-hil failed:\n" + (proc.stdout or "") + (proc.stderr or "")
        )
    lines = [line for line in proc.stdout.splitlines() if "tests/hil/run.py" in line]
    if len(lines) != 1:
        raise SystemExit(
            "expected one test-hil recipe, got %d:\n%s"
            % (len(lines), proc.stdout)
        )
    return lines[0]


def test_unset_uses_the_bench_profile():
    line = recipe()
    check("HIL_EXT is not exported when the caller left it unset",
          "HIL_EXT=" not in line, line)


def test_empty_is_forwarded():
    line = recipe(("HIL_EXT=",))
    check("an explicit empty HIL_EXT is forwarded (opts out)", "HIL_EXT=''" in line,
          line)


def test_value_is_forwarded():
    line = recipe(("HIL_EXT=-DOW_PARASITE_POWER=1",))
    check("an explicit HIL_EXT is forwarded",
          "HIL_EXT='-DOW_PARASITE_POWER=1'" in line, line)


def test_the_environment_is_forwarded():
    line = recipe(env={"HIL_EXT": "-DOW_PARASITE_POWER=1"})
    check("HIL_EXT from the environment is forwarded",
          "HIL_EXT='-DOW_PARASITE_POWER=1'" in line, line)


def main():
    test_unset_uses_the_bench_profile()
    test_empty_is_forwarded()
    test_value_is_forwarded()
    test_the_environment_is_forwarded()
    if failures:
        print("test_make: FAILED (%d)" % len(failures))
        return 1
    print("test_make: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
