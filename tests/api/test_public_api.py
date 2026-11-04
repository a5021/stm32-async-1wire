#!/usr/bin/env python3
"""Canary tests for tests/api/public_api.py - no compiler or hardware needed.

The snapshot has to go red on the three ways a public declaration can break a
consumer: a function rename, a removed macro and a changed signature. A green
snapshot check proves nothing unless this test can make it red, so each case
below mutates a temporary copy of inc/ and runs the real CLI against it.

Run: python tests/api/test_public_api.py   (make test-public-api)
"""

from __future__ import annotations

import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
SCRIPT = HERE / "public_api.py"
REPO = HERE.parents[1]
SRC_INC = REPO / "inc"

failures: list[str] = []


def check(what: str, condition: bool, detail: str = "") -> None:
    if condition:
        print("  ok   %s" % what)
    else:
        print("  FAIL %s%s" % (what, (": " + detail) if detail else ""))
        failures.append(what)


def run_check(inc: Path, snapshot: Path) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [sys.executable, str(SCRIPT), "--check", "--inc", str(inc),
         "--snapshot", str(snapshot)],
        text=True, capture_output=True,
    )


def snapshot_of(inc: Path, path: Path) -> None:
    result = subprocess.run(
        [sys.executable, str(SCRIPT), "--update", "--inc", str(inc),
         "--snapshot", str(path)],
        text=True, capture_output=True,
    )
    if result.returncode != 0:
        raise RuntimeError(result.stderr or result.stdout)


def mutate(inc: Path, header: str, old: str, new: str) -> None:
    path = inc / header
    text = path.read_text(encoding="utf-8")
    if old not in text:
        raise AssertionError("%s does not contain the mutation anchor %r"
                             % (header, old))
    path.write_text(text.replace(old, new, 1), encoding="utf-8")


def main() -> int:
    with tempfile.TemporaryDirectory() as tmp:
        tmpdir = Path(tmp)
        inc = tmpdir / "inc"
        shutil.copytree(SRC_INC, inc)
        baseline = tmpdir / "baseline.txt"
        snapshot_of(inc, baseline)

        clean = run_check(inc, baseline)
        check("unmodified inc/: the snapshot passes", clean.returncode == 0,
              clean.stderr.strip())

        # 1. A function rename. This is the common accidental break.
        mutate(inc, "onewire.h", "onewire_write_slots(", "onewire_write_slots_x(")
        renamed = run_check(inc, baseline)
        check("renamed function: the snapshot goes red",
              renamed.returncode != 0 and "onewire_write_slots" in renamed.stderr)
        shutil.copyfile(SRC_INC / "onewire.h", inc / "onewire.h")

        # 2. A removed macro.
        mutate(inc, "onewire.h", "#define ONEWIRE_ROM_BYTES 8\n", "")
        removed = run_check(inc, baseline)
        check("removed macro: the snapshot goes red",
              removed.returncode != 0 and "ONEWIRE_ROM_BYTES" in removed.stderr)
        shutil.copyfile(SRC_INC / "onewire.h", inc / "onewire.h")

        # 3. A changed function signature.
        mutate(inc, "onewire.h", "uint8_t onewire_reset_captured(void);",
               "uint8_t onewire_reset_captured(uint8_t);")
        signature = run_check(inc, baseline)
        check("changed signature: the snapshot goes red",
              signature.returncode != 0
              and "onewire_reset_captured" in signature.stderr)

    if failures:
        print("  public_api canary: FAIL (%d case(s))" % len(failures))
        return 1
    print("  public_api canary: OK (rename, removal and signature change all go red)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
