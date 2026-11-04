#!/usr/bin/env python3
"""Run the 1-Wire visualizer browser self-test in headless Chromium.

`tools/1wire_vis/selftest.html` is the only executable check of the decoder
that ships to users: it parses the bundled 32.8 M-sample capture, decodes 44
transactions and runs the golden slot/CRC/overdrive vectors. Before this, a
green selftest depended on somebody opening the page by hand - a broken decoder
could ship as long as nobody looked. This script is the CI equivalent: it opens
the page, waits for `document.body.dataset.result`, and reproduces every failed
check from `window.__selftest` in the job log.

The page is loaded from `file://` on purpose. That is how the README tells
users to open it, so the check also exercises the no-server path (bundled_sr.js
is embedded for exactly that reason).

Local run:
    python -m pip install -r tools/1wire_vis/requirements-selftest.txt
    python -m playwright install chromium
    python tools/1wire_vis/run_selftest.py       # make test-viewer

CI adds `--with-deps` to the browser install for the system libraries.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
SELFTEST = HERE / "selftest.html"


def describe_failure(result: dict) -> str:
    detail = result.get("detail") or ""
    return "  FAIL %s%s" % (result.get("name", "?"), (": " + detail) if detail else "")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--timeout-ms", type=int, default=300_000,
                        help="time to wait for the page result (default: 300000)")
    args = parser.parse_args(argv)

    if not SELFTEST.is_file():
        print("  viewer selftest: %s is missing" % SELFTEST, file=sys.stderr)
        return 1

    try:
        from playwright.sync_api import Error as PlaywrightError
        from playwright.sync_api import TimeoutError as PlaywrightTimeout
        from playwright.sync_api import sync_playwright
    except ImportError:
        print("  viewer selftest: playwright is not installed; run\n"
              "    python -m pip install -r tools/1wire_vis/requirements-selftest.txt\n"
              "    python -m playwright install chromium", file=sys.stderr)
        return 1

    url = SELFTEST.as_uri()
    console_errors: list[str] = []
    page_errors: list[str] = []

    with sync_playwright() as playwright:
        try:
            browser = playwright.chromium.launch()
        except PlaywrightError as exc:
            print("  viewer selftest: cannot launch Chromium: %s\n"
                  "  run `python -m playwright install --with-deps chromium`"
                  % exc, file=sys.stderr)
            return 1
        try:
            page = browser.new_page()
            page.on("console", lambda msg: console_errors.append(msg.text)
                    if msg.type == "error" else None)
            page.on("pageerror", lambda exc: page_errors.append(str(exc)))

            page.goto(url, wait_until="load")
            page.wait_for_function(
                "() => document.body && document.body.dataset && "
                "document.body.dataset.result",
                timeout=args.timeout_ms,
            )
            result = page.evaluate("() => window.__selftest || null")
        except PlaywrightTimeout:
            print("  viewer selftest: no result after %d ms; the page may have "
                  "thrown before setting body[data-result]" % args.timeout_ms,
                  file=sys.stderr)
            for line in page_errors + console_errors:
                print("    " + line, file=sys.stderr)
            return 1
        except PlaywrightError as exc:
            print("  viewer selftest: browser error: %s" % exc, file=sys.stderr)
            return 1
        finally:
            browser.close()

    if result is None:
        print("  viewer selftest: body[data-result] was set but window.__selftest "
              "is missing", file=sys.stderr)
        return 1

    failed = result.get("failed", 0)
    total = result.get("total", 0)
    if failed:
        for check in result.get("results", []):
            if not check.get("ok"):
                print(describe_failure(check), file=sys.stderr)
        print("  viewer selftest: FAIL (%d of %d checks failed)"
              % (failed, total), file=sys.stderr)
        return 1

    if page_errors:
        # The selftest catches its own exceptions, so an uncaught error is a
        # defect even if enough checks happened to pass before it.
        for line in page_errors:
            print("  uncaught page error: " + line, file=sys.stderr)
        print("  viewer selftest: FAIL (uncaught page error)", file=sys.stderr)
        return 1

    print("  viewer selftest: OK (%d checks passed in headless Chromium)"
          % total)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
