#!/usr/bin/env python3
"""Write a trimmed logic-analyzer window as a sigrok `.sr` archive.

Shared by the HIL capture scripts: the stress report links one frame per cell,
so the archive opens on the traffic instead of 200 ms of boot settle and 99
more frames. The layout is the same one sigrok writes: `version`, `metadata`,
`logic-1-1`. `label` only tags the producer in the metadata.
"""

import zipfile

import numpy as np


def write_sr(raw, rate_hz, dst, start_us, end_us, rate_text, label):
    """Write one trimmed window as a sigrok `.sr` archive."""
    start = max(0, int(round(start_us * 1e-6 * rate_hz)))
    end = min(len(raw), int(round(end_us * 1e-6 * rate_hz)))
    if end - start < 16:
        raise RuntimeError("capture window is empty")
    meta = (
        "[global]\n"
        "sigrok version=0.6.0-%s\n\n"
        "[device 1]\n"
        "capturefile=logic-1\n"
        "total probes=8\n"
        "samplerate=%s\n"
        "total analog=0\n"
        "probe1=D0\n"
        "probe2=D1\n"
        "probe3=D2\n"
        "probe4=D3\n"
        "probe5=D4\n"
        "probe6=D5\n"
        "probe7=D6\n"
        "probe8=D7\n"
        "unitsize=1\n" % (label, rate_text)
    )
    with zipfile.ZipFile(dst, "w", zipfile.ZIP_DEFLATED) as z:
        z.writestr("version", "2")
        z.writestr("metadata", meta)
        z.writestr("logic-1-1", np.asarray(raw[start:end], dtype=np.uint8).tobytes())
