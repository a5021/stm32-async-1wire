#!/usr/bin/env python3
"""Analyze a sigrok srzip capture of the 1-Wire bus (D0 = PA10).

Measures low/high pulse durations on D0 and prints the high-level structure:
reset pulses (>= 380us low), the presence window that follows, and the low
duration distributions of write/read bit slots.
"""
import re
import sys
import zipfile


def read_srzip(path):
    with zipfile.ZipFile(path) as z:
        meta = z.read("metadata").decode("utf-8", "replace")
        m = re.search(r"samplerate=(\d+)\s*MHz", meta)
        rate = int(m.group(1)) * 1000000 if m else 16000000
        data = z.read("logic-1-1")
    return rate, data


def pulse_edges(data, rate):
    """Yield (t_start_us, t_end_us, level) for each run, sampled at `rate` Hz."""
    edges = []
    prev = None
    run_start = 0
    for i, byte in enumerate(data):
        bit = byte & 1
        if bit != prev:
            if prev is not None:
                edges.append((run_start * 1e6 / rate, i * 1e6 / rate, prev))
            run_start = i
            prev = bit
    if prev is not None and len(data):
        edges.append((run_start * 1e6 / rate, len(data) * 1e6 / rate, prev))
    return edges


def main(argv):
    path = argv[1]
    rate, data = read_srzip(path)
    print(f"samplerate={rate/1e6:g} MHz  samples={len(data)}")
    edges = pulse_edges(data, rate)
    lows = [(s, e) for (s, e, lv) in edges if lv == 0]
    resets = [(s, e) for (s, e) in lows if (e - s) >= 380]
    print(f"\npulse runs={len(edges)}  lows={len(lows)}  reset pulses(>=380us)={len(resets)}")
    for (s, e) in resets[:12]:
        d = e - s
        print(f"  reset: {s:10.2f}us -> {e:10.2f}us  ({d:.2f}us)")
    if len(resets) > 12:
        print(f"  ... ({len(resets)} total)")
    if resets:
        print("presence windows (high after each reset, first 8):")
        # find the high run that follows each reset
        for (rs, re_) in resets[:8]:
            following = None
            for (hs0, he0, lv) in edges:
                if lv == 1 and hs0 >= re_ - 1.0:
                    following = (hs0, he0)
                    break
            if following:
                print(f"  after reset@{rs:.0f}us: high {following[1]-following[0]:.2f}us")
    slot_lows = [(s, e) for (s, e) in lows if (e - s) < 200]
    if slot_lows:
        ds = sorted(e - s for (s, e) in slot_lows)
        n = len(ds)
        def pts(p):
            return ds[min(n - 1, int(round((n - 1) * p)))]
        print(f"\nslot lows (<200us): n={n}  min={ds[0]:.2f}  p25={pts(0.25):.2f}  "
              f"median={pts(0.5):.2f}  p75={pts(0.75):.2f}  max={ds[-1]:.2f} us")
        w1 = [d for d in ds if d < 15]
        w0 = [d for d in ds if d >= 40]
        print(f"  write-1 exits (low<15us): {len(w1)}   write-0/read slots (low>=40us): {len(w0)}")


if __name__ == "__main__":
    main(sys.argv)