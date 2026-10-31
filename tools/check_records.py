"""Check a 9_multi UART log: round extremes, record updates and counters.

Usage: python tools/check_records.py <log file>

Rules checked:
  * the printed round max/min equal the max/min of the printed sensor temps;
  * peak record changes only on a strict increase of the round max, low only
    on a strict decrease of the round min;
  * a NEW line is followed by a write whose value equals the round extreme,
    and the record counter goes up by exactly one;
  * a held line keeps the record value unchanged;
  * no write failures or timeouts appear in the log.
"""
import re
import sys

CYCLE = re.compile(r"^cycle (\d+): (.*?)\| max (-?\d+\.\d) C \(s(\d)\)(.*?)\| min (-?\d+\.\d) C \(s(\d)\)(.*)$")
TOKEN = re.compile(r"(--\.-|-?\d+\.\d)")
REC_PEAK = re.compile(r"^peak: #(\d+) (-?\d+\.\d) C")
REC_LOW = re.compile(r"^low: #(\d+) (-?\d+\.\d) C")
REC_IN_LINE = re.compile(r"record (-?\d+\.\d) C #(\d+)")
NEW_WAS = re.compile(r"\(was (-?\d+\.\d)\)")


def tenths(text):
    return round(float(text) * 10)


def read_text(path):
    raw = open(path, "rb").read()
    if raw[:2] in (b"\xff\xfe", b"\xfe\xff"):
        return raw.decode("utf-16", errors="replace")
    return raw.decode("utf-8", errors="replace")


def main(path):
    lines = read_text(path).splitlines()
    cycles = 0
    errors = []
    writes = 0
    pending = None  # (kind, expected_tenths, expected_counter, cycle)
    peak = None  # [tenths, counter]
    low = None
    failures = [l for l in lines if "failed" in l or "timeout" in l or "disabled" in l]

    for ln in lines:
        ln = ln.strip()
        m = CYCLE.match(ln)
        if m:
            cycles += 1
            cyc = int(m.group(1))
            temps = [tenths(t) for t in TOKEN.findall(m.group(2)) if t != "--.-"]
            rmax = tenths(m.group(3))
            rmin = tenths(m.group(6))
            if temps and (max(temps) != rmax or min(temps) != rmin):
                errors.append(f"cycle {cyc}: printed extremes {rmin/10}/{rmax/10} "
                              f"differ from sensor list min {min(temps)/10}/max {max(temps)/10}")
            if pending is not None:
                errors.append(f"cycle {cyc}: NEW record was not written before the next cycle ({pending[0]})")
                pending = None

            max_tail = m.group(5)
            min_tail = m.group(7)
            if "NEW record" in max_tail:
                if peak is not None and rmax <= peak[0]:
                    errors.append(f"cycle {cyc}: NEW peak {rmax/10} not above stored {peak[0]/10}")
                pending = ("peak", rmax, None, cyc)
            else:
                rec = REC_IN_LINE.search(max_tail)
                if rec:
                    if peak is None:
                        peak = [tenths(rec.group(1)), int(rec.group(2))]
                    elif tenths(rec.group(1)) != peak[0]:
                        errors.append(f"cycle {cyc}: peak record {rec.group(1)} changed without a write (was {peak[0]/10})")
                    if rmax > peak[0]:
                        errors.append(f"cycle {cyc}: round max {rmax/10} above record {peak[0]/10} but not NEW")
            if "NEW record" in min_tail:
                if low is not None and rmin >= low[0]:
                    errors.append(f"cycle {cyc}: NEW low {rmin/10} not below stored {low[0]/10}")
                pending = ("low", rmin, None, cyc)
            else:
                rec = REC_IN_LINE.search(min_tail)
                if rec:
                    if low is None:
                        low = [tenths(rec.group(1)), int(rec.group(2))]
                    elif tenths(rec.group(1)) != low[0]:
                        errors.append(f"cycle {cyc}: low record {rec.group(1)} changed without a write (was {low[0]/10})")
                    if rmin < low[0]:
                        errors.append(f"cycle {cyc}: round min {rmin/10} below record {low[0]/10} but not NEW")
            continue

        for kind, rx in (("peak", REC_PEAK), ("low", REC_LOW)):
            m = rx.match(ln)
            if not m:
                continue
            if pending is None or pending[0] != kind:
                # boot report or a write that was not announced by a NEW line
                if kind == "peak":
                    peak = [tenths(m.group(2)), int(m.group(1))]
                else:
                    low = [tenths(m.group(2)), int(m.group(1))]
                continue
            writes += 1
            value = tenths(m.group(2))
            counter = int(m.group(1))
            if value != pending[1]:
                errors.append(f"cycle {pending[3]}: written {kind} {value/10} != NEW {pending[1]/10}")
            if kind == "peak":
                if peak is not None and counter != (peak[1] + 1) % 256:
                    errors.append(f"cycle {pending[3]}: peak counter {counter} after {peak[1]}")
                peak = [value, counter]
            else:
                if low is not None and counter != (low[1] + 1) % 256:
                    errors.append(f"cycle {pending[3]}: low counter {counter} after {low[1]}")
                low = [value, counter]
            pending = None

    print(f"cycles checked : {cycles}")
    print(f"record writes  : {writes}")
    print(f"final peak     : {peak[0]/10 if peak else None} (#{peak[1] if peak else None})")
    print(f"final low      : {low[0]/10 if low else None} (#{low[1] if low else None})")
    print(f"failure lines  : {len(failures)}")
    for f in failures[:10]:
        print("   ", f)
    print(f"errors         : {len(errors)}")
    for e in errors[:30]:
        print("   ", e)
    return 0 if not errors and not failures else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1]))
