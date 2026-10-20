#!/usr/bin/env python3
"""Mutation reconnaissance: does the DMA contract test catch a broken driver?

Run:
    python tests/mutation_check.py --list
    python tests/mutation_check.py --families f1,f4,h5
    python tests/mutation_check.py --families f1 --mutations M1,M3

What it does: for each (family, mutation) pair it applies a deliberate defect
to the *port* source in a scratch git worktree, builds the host suite for that
family, runs it, and records whether the suite failed. A mutation that the
suite survives is a hole in the proof: the contract that the README claims is
not actually enforced on that port.

Why a worktree: the defect must never reach the working tree. The scratch
copy is a detached worktree at HEAD; the host suite needs no CMSIS and no
network, so it builds there as-is.

The mutations are the ones a DMA contract is most likely to be broken by:
a wrong buffer, a wrong count, a wrong transfer class, a wrong repetition
count, a missing request routing. The portable ones live in the shared body of
src/ow_port.h and are applied to every family; the per-family ones (the DMAMUX
request id, the GPDMA REQSEL, the F4 CHSEL) are applied only where they
apply, because the guarantee they break does not exist elsewhere.
"""

import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

FAMILIES = ["f0", "f1", "f3", "f4", "g0", "g4", "h5"]

# --- the mutations ---------------------------------------------------------
# find/replace are literal (not regex) and must match exactly once in the
# file, so a mutation can never silently become a no-op.
MUTATIONS = [
    {
        "id": "M1",
        "what": "feed source address: one element too early",
        "file": "src/ow_port.h",
        "find": "OW_PORT_DMA_PROG_FEED(&cmd[1], slots, OW_PORT_DMA_CR_TX);",
        "replace": "OW_PORT_DMA_PROG_FEED(&cmd[0], slots, OW_PORT_DMA_CR_TX);",
        "families": None,  # all
    },
    {
        "id": "M2",
        "what": "feed transfer count: one too few",
        "file": "src/ow_port.h",
        "find": "OW_PORT_DMA_PROG_FEED(&cmd[1], slots, OW_PORT_DMA_CR_TX);",
        "replace": "OW_PORT_DMA_PROG_FEED(&cmd[1], (slots - 1u), OW_PORT_DMA_CR_TX);",
        "families": None,
    },
    {
        "id": "M3",
        "what": "feed transfer class: direction/width of the feed",
        "file": "src/ow_port.h",
        "find": "OW_PORT_DMA_PROG_FEED(&cmd[1], slots, OW_PORT_DMA_CR_TX);",
        "replace": "OW_PORT_DMA_PROG_FEED(&cmd[1], slots, OW_PORT_DMA_CR_RX16);",
        "families": None,
    },
    {
        "id": "M4",
        "what": "timer repetition count: one too many",
        "file": "src/ow_port.h",
        "find": "T1.RCR = slots - 1;",
        "replace": "T1.RCR = slots;",
        "families": None,
    },
    {
        "id": "M5",
        "what": "capture transfer width: always 8-bit",
        "file": "src/ow_port.h",
        "find": "OW_PORT_DMA_PROG_CAPTURE(dst, count, ((width == 16) ? OW_PORT_DMA_CR_RX16 : OW_PORT_DMA_CR_RX8));",
        "replace": "OW_PORT_DMA_PROG_CAPTURE(dst, count, OW_PORT_DMA_CR_RX8);",
        "families": None,
    },
    {
        "id": "M6",
        "what": "feed request routing: not programmed (write path)",
        "file": "src/ow_port.h",
        "find": "    ow_port_update_event();\n    OW_PORT_DMA_DISABLE_FEED();\n    OW_PORT_ROUTE_FEED();\n",
        "replace": "    ow_port_update_event();\n    OW_PORT_DMA_DISABLE_FEED();\n",
        "families": ["g0", "g4"],  # H5 routes inside its PROG macro; the classic
                                      # families have a fixed request map
    },
    {
        "id": "M11",
        "what": "feed request routing: not programmed (merged path)",
        "file": "src/ow_port.h",
        "find": "    OW_PORT_DMA_DISABLE_FEED();\n    OW_PORT_ROUTE_FEED();\n    OW_PORT_DMA_PROG_FEED(read_pulse, 3, OW_PORT_DMA_CR_TX);",
        "replace": "    OW_PORT_DMA_DISABLE_FEED();\n    OW_PORT_DMA_PROG_FEED(read_pulse, 3, OW_PORT_DMA_CR_TX);",
        "families": ["g0", "g4"],
    },
    {
        "id": "M12",
        "what": "capture request routing: not programmed (merged path)",
        "file": "src/ow_port.h",
        "find": "    OW_PORT_ROUTE_CAPTURE();\n    OW_PORT_DMA_PROG_CAPTURE(pulse3, 3, OW_PORT_DMA_CR_RX16);",
        "replace": "    OW_PORT_DMA_PROG_CAPTURE(pulse3, 3, OW_PORT_DMA_CR_RX16);",
        "families": ["g0", "g4"],
    },
    {
        "id": "M7",
        "what": "DMAMUX request id for the feed (G0)",
        "file": "src/ow_mcu.h",
        "find": "#define OW_PORT_DMAMUX_REQ_TIM1_CC2 21u",
        "replace": "#define OW_PORT_DMAMUX_REQ_TIM1_CC2 20u",
        "families": ["g0"],
    },
    {
        "id": "M8",
        "what": "DMAMUX request id for the feed (G4)",
        "file": "src/ow_mcu.h",
        "find": "#define OW_PORT_DMAMUX_REQ_TIM1_CC2 43u",
        "replace": "#define OW_PORT_DMAMUX_REQ_TIM1_CC2 42u",
        "families": ["g4"],
    },
    {
        "id": "M9",
        "what": "GPDMA REQSEL for the feed (H5)",
        "file": "src/ow_mcu.h",
        "find": "#define OW_PORT_GPDMA_REQ_TIM1_CC2 59u",
        "replace": "#define OW_PORT_GPDMA_REQ_TIM1_CC2 58u",
        "families": ["h5"],
    },
    {
        "id": "M10",
        "what": "DMA2 stream CHSEL for the feed (F4)",
        "file": "src/ow_mcu.h",
        "find": "#define OW_PORT_DMA_CHSEL (6u << DMA_SxCR_CHSEL_Pos)",
        "replace": "#define OW_PORT_DMA_CHSEL (5u << DMA_SxCR_CHSEL_Pos)",
        "families": ["f4"],
    },
]

FAIL_RE = re.compile(r"^FAIL (\S+):(\d+)\s+(.*)$")
SUMMARY_RE = re.compile(r"^(PASS|FAIL): (\d+) failure")


def run(cmd, cwd, env=None):
    return subprocess.run(
        cmd, cwd=cwd, shell=True, capture_output=True, text=True,
        env=env, encoding="utf-8", errors="replace",
    )


def make_worktree(path):
    # A previous run may have left the registration behind (the directory is
    # gone but git still lists it), and "git worktree add" then refuses. Prune
    # the stale entries first.
    run("git worktree prune", REPO)
    shutil.rmtree(path, ignore_errors=True)
    r = run("git worktree add --detach --force \"%s\" HEAD" % path, REPO)
    if r.returncode != 0:
        raise RuntimeError("worktree failed: %s%s" % (r.stdout, r.stderr))


def drop_worktree(path):
    run("git worktree remove --force \"%s\"" % path, REPO)
    run("git worktree prune", REPO)
    shutil.rmtree(path, ignore_errors=True)


def apply_mutation(wt, mut):
    path = os.path.join(wt, mut["file"])
    with open(path, "r", encoding="utf-8", newline="") as f:
        text = f.read()
    n = text.count(mut["find"])
    if n != 1:
        return False, "find matched %d times" % n
    with open(path, "w", encoding="utf-8", newline="") as f:
        f.write(text.replace(mut["find"], mut["replace"]))
    return True, ""


def revert(wt, mut):
    run("git checkout -- \"%s\"" % mut["file"], wt)


def parse_failures(out, exit_ok):
    """Return (failed, catching_test_files, n_failures).

    `failed` must not be derived from the summary line alone: the suite
    prints it at the very end, and a broken DMA programming can crash the
    mock before it gets there. The exit status and the assertion count are
    the reliable signals.
    """
    files = []
    n = 0
    for line in out.splitlines():
        m = FAIL_RE.match(line)
        if m:
            n += 1
            f = m.group(1).replace("\\", "/")
            if f not in files:
                files.append(f)
    return (not exit_ok) or n > 0, files, n


def binary_digest(wt):
    """Hash the built host executable, so a mutation that changed nothing
    can be told apart from one the suite failed to notice."""
    exe = os.path.join(wt, "build", "test", "ds18b20_test.exe")
    if not os.path.exists(exe):
        return ""
    h = hashlib.sha256()
    with open(exe, "rb") as f:
        h.update(f.read())
    return h.hexdigest()


def test_family(wt, fam):
    """Build and run the host suite for one family; return (ok, output)."""
    r = run("make test-%s" % fam, wt)
    return r.returncode == 0, (r.stdout or "") + (r.stderr or "")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--families", default=",".join(FAMILIES))
    ap.add_argument("--mutations", default="")
    ap.add_argument("--json", default="")
    ap.add_argument("--list", action="store_true")
    args = ap.parse_args()

    if args.list:
        for m in MUTATIONS:
            print("%-4s %-52s %s" % (m["id"], m["what"], m["file"]))
        return 0

    fams = [f for f in args.families.split(",") if f]
    muts = MUTATIONS
    if args.mutations:
        want = set(args.mutations.split(","))
        muts = [m for m in MUTATIONS if m["id"] in want]

    wt = os.path.join(tempfile.gettempdir(), "ow-mutation-scratch")
    make_worktree(wt)
    results = []
    try:
        # Control: the unmutated suite must pass, or "not caught" means nothing.
        for fam in fams:
            ok, out = test_family(wt, fam)
            if not ok:
                print("CONTROL FAILED on %s - the unmutated suite does not pass"
                      % fam)
                print(out[-3000:])
                return 1
        print("control: all %d families pass unmutated\n" % len(fams))
        control_digest = binary_digest(wt)

        for mut in muts:
            for fam in fams:
                if mut["families"] is not None and fam not in mut["families"]:
                    results.append({
                        "mutation": mut["id"], "family": fam,
                        "applies": False, "caught": None, "by": [], "n": 0,
                    })
                    continue
                applied, why = apply_mutation(wt, mut)
                if not applied:
                    results.append({
                        "mutation": mut["id"], "family": fam,
                        "applies": False, "caught": None, "by": [],
                        "n": 0, "note": why,
                    })
                    revert(wt, mut)
                    continue
                ok, out = test_family(wt, fam)
                failed, files, n = parse_failures(out, ok)
                digest = binary_digest(wt)
                revert(wt, mut)
                no_effect = (digest == control_digest) and not failed
                results.append({
                    "mutation": mut["id"], "family": fam,
                    "applies": True, "caught": bool(failed), "by": files, "n": n,
                    "no_effect": no_effect,
                })
                if failed:
                    mark = "caught"
                elif no_effect:
                    mark = "NO EFFECT"
                else:
                    mark = "SURVIVED"
                print("%-4s %-6s %-9s (%d assertion(s)%s)"
                      % (mut["id"], fam, mark, n,
                         (": " + ", ".join(os.path.basename(f) for f in files[:3]))
                         if files else ""))
    finally:
        drop_worktree(wt)

    print("\n=== matrix ===")
    hdr = "mutation  " + "".join("%-10s" % f for f in fams)
    print(hdr)
    for mut in muts:
        row = "%-10s" % mut["id"]
        for fam in fams:
            r = [x for x in results
                 if x["mutation"] == mut["id"] and x["family"] == fam]
            if not r:
                row += "%-10s" % "-"
            elif not r[0]["applies"]:
                row += "%-10s" % "n/a"
            elif r[0]["caught"]:
                row += "%-10s" % "caught"
            else:
                row += "%-10s" % "SURVIVED"
        print(row)

    holes = [r for r in results if r["applies"] and not r["caught"]]
    print("\nmutations applied: %d, caught: %d, survived: %d"
          % (len([r for r in results if r["applies"]]),
             len([r for r in results if r["applies"] and r["caught"]]),
             len(holes)))
    for r in holes:
        print("  HOLE: %s survives on %s" % (r["mutation"], r["family"]))

    if args.json:
        with open(args.json, "w", encoding="utf-8") as f:
            json.dump({"results": results,
                       "mutations": [{"id": m["id"], "what": m["what"]}
                                     for m in MUTATIONS]}, f, indent=2)
        print("\njson: %s" % args.json)
    return 0


if __name__ == "__main__":
    sys.exit(main())
