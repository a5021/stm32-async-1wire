#!/usr/bin/env python3
"""Unit tests for the Search ROM wire decoder - no hardware needed.

Run: python tests/hil/test_search_trace.py   (make test-search-trace)

The synthetic traces start with a reset, presence, the 0xF0 Search ROM command
and then the exact id/cmp/direction slot order. The tests cover a complete
64-bit pass and every way the capture can stop early.
"""

import os
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import search_trace  # noqa: E402

RATE_HZ = search_trace.RATE_HZ
failures = []


def check(what, condition, detail=""):
    if condition:
        print("  ok   %s" % what)
    else:
        print("  FAIL %s%s" % (what, (" - " + detail) if detail else ""))
        failures.append(what)


def synth(runs):
    """Build packed D0 samples from (level, duration_us) runs."""
    out = []
    for level, duration_us in runs:
        count = max(1, int(round(duration_us * RATE_HZ / 1e6)))
        out.extend([level] * count)
    return np.asarray(out, dtype=np.uint8)


def pair(id_bit, cmp_bit):
    """One read pair: low pulse + high recovery, LSB-first id/cmp."""
    def slot(bit):
        return [(0, 5.0 if bit else 60.0), (1, 10.0)]
    return slot(id_bit) + slot(cmp_bit)


def direction(bit):
    return [(0, 5.0 if bit else 60.0), (1, 10.0)]


def base():
    """Idle, reset, presence and the 0xF0 Search ROM command."""
    runs = [(1, 10.0), (0, 480.0), (1, 10.0), (0, 30.0), (1, 10.0)]
    for bit in (0, 0, 0, 0, 1, 1, 1, 1):
        runs += [(0, 60.0 if bit == 0 else 5.0), (1, 10.0)]
    return runs


def decode(runs):
    return search_trace.decode(synth(runs))


def main():
    # A complete 64-bit pass ends with the direction for the last bit.
    full = base()
    for _ in range(64):
        full += pair(1, 1) + direction(1)
    res = decode(full)
    check("full pass decodes 64 bits", len(res.get("bits", [])) == 64,
          "got %d" % len(res.get("bits", [])))
    check("full pass has every direction",
          all(b["direction"] is not None for b in res.get("bits", [])))
    check("full pass is not truncated", res.get("truncated_after_pair") is False)
    check("full pass has no contradiction", res.get("contradiction") is None)

    # Abort immediately after the first read pair.
    res = decode(base() + pair(1, 1))
    check("first-pair abort keeps one bit", len(res.get("bits", [])) == 1)
    check("first-pair abort has no direction",
          res["bits"][0]["direction"] is None)
    check("first-pair abort is marked truncated",
          res.get("truncated_after_pair") is True)

    # Two complete bits, then a complete next pair with no direction.
    runs = base() + pair(1, 1) + direction(1)
    runs += pair(1, 1) + direction(1)
    runs += pair(1, 1)
    res = decode(runs)
    check("next-pair abort keeps the new pair", len(res.get("bits", [])) == 3,
          "got %d" % len(res.get("bits", [])))
    check("next-pair abort leaves its direction empty",
          res["bits"][-1]["direction"] is None)
    check("next-pair abort keeps both pair bits",
          res["bits"][-1]["id"] == 1 and res["bits"][-1]["cmp"] == 1)
    check("next-pair abort is marked truncated",
          res.get("truncated_after_pair") is True)

    # Abort after a direction but before the next pair: complete bit.
    res = decode(base() + pair(1, 1) + direction(1))
    check("direction abort keeps one complete bit",
          len(res.get("bits", [])) == 1 and res["bits"][0]["direction"] == 1)
    check("direction abort is not marked truncated",
          res.get("truncated_after_pair") is False)

    # Direction plus only the first slot of the next pair: keep the partial
    # pair as evidence, do not turn the direction into a pair.
    res = decode(base() + pair(1, 1) + direction(1) + [(0, 60.0), (1, 10.0)])
    check("partial-pair abort keeps a partial bit", len(res.get("bits", [])) == 2,
          "got %d" % len(res.get("bits", [])))
    check("partial-pair abort keeps the direction",
          res["bits"][0]["direction"] == 1)
    check("partial-pair abort has no cmp",
          res["bits"][1]["id"] == 0 and res["bits"][1]["cmp"] is None)
    check("partial-pair abort is marked truncated",
          res.get("truncated_after_pair") is True)
    check("partial-pair abort has no false contradiction",
          res.get("contradiction") is None)

    # A completed bit where id == cmp but the direction is opposite.
    res = decode(base() + pair(1, 1) + direction(0))
    check("completed-bit contradiction is reported",
          res.get("contradiction") is not None)
    check("contradiction points at bit 1",
          res.get("contradiction", {}).get("bit") == 1)

    if failures:
        print("FAIL: %d check(s) failed" % len(failures))
        return 1
    print("PASS: Search ROM trace decoder")
    return 0


if __name__ == "__main__":
    sys.exit(main())
