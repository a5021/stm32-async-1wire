#!/usr/bin/env python3
"""Extract and check the public API declarations of the library.

The snapshot in tests/api/public-api.txt is a text record of what a consumer
can see in inc/: public macros, typedefs, structs and function declarations.
It is not an ABI snapshot (there is no sizeof/offsetof probe): its job is to
make an accidental rename, removal or signature change show up in review as a
diff, which is exactly the class of breakage 3.0.0's macro rename created.

inc/ow_version.h is deliberately excluded: the version bump already has its own
gate in tests/check_version.sh, and including it here would add churn to every
snapshot update.

Run:
    python tests/api/public_api.py --check     # make test-public-api
    python tests/api/public_api.py --update    # make update-public-api
"""

from __future__ import annotations

import argparse
import difflib
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
DEFAULT_INC = ROOT / "inc"
DEFAULT_SNAPSHOT = Path(__file__).resolve().parent / "public-api.txt"

COMMENT_RE = re.compile(r"/\*.*?\*/", re.DOTALL)
LINE_COMMENT_RE = re.compile(r"//[^\n]*")
MACRO_RE = re.compile(r"^\s*#\s*define\s+([A-Za-z_]\w*)(.*)$")


def _comment_to_space(match: re.Match[str]) -> str:
    """Keep the newline count so line structure survives comment removal."""
    return " " + "\n" * match.group(0).count("\n")


def strip_comments(text: str) -> str:
    text = COMMENT_RE.sub(_comment_to_space, text)
    return LINE_COMMENT_RE.sub("", text)


def normalize_space(text: str) -> str:
    return re.sub(r"\s+", " ", text).strip()


def normalize_decl(text: str) -> str:
    text = normalize_space(text)
    text = re.sub(r"\s*([(),;{}\[\]])\s*", r"\1", text)
    text = re.sub(r"\s*\*\s*", "*", text)
    return text


def strip_preprocessor(text: str) -> str:
    """Drop directives and their continuation lines, keeping line numbers."""
    lines = text.splitlines()
    out: list[str] = []
    i = 0
    while i < len(lines):
        if re.match(r"^\s*#", lines[i]):
            while lines[i].rstrip().endswith("\\") and i + 1 < len(lines):
                i += 1
            out.append("")
        else:
            out.append(lines[i])
        i += 1
    return "\n".join(out)


def extract_macros(text: str) -> list[str]:
    lines = text.splitlines()
    macros: list[str] = []
    i = 0
    while i < len(lines):
        match = MACRO_RE.match(lines[i])
        if not match:
            i += 1
            continue
        name = match.group(1)
        body = match.group(2)
        while body.rstrip().endswith("\\") and i + 1 < len(lines):
            body = body.rstrip()[:-1]
            i += 1
            body += " " + lines[i].strip()
        body = normalize_space(body.replace("\\", " "))
        # An empty body is an include guard (ONEWIRE_H, OW_BOARD_H, ...), not
        # an API value; the guard exists to make the header idempotent.
        if body:
            macros.append(("#define %s %s" % (name, body)).rstrip())
        i += 1
    return macros


def looks_like_function(prefix: str) -> bool:
    text = normalize_decl(prefix)
    if "typedef" in text or "{" in text or "}" in text:
        return False
    return bool(re.search(r"\)$", text)) and "(" in text


def skip_braced(text: str, start: int) -> int:
    """Return the index just after the brace block beginning at `start`."""
    depth = 0
    i = start
    while i < len(text):
        char = text[i]
        if char in "\"'":
            quote = char
            i += 1
            while i < len(text):
                if text[i] == "\\":
                    i += 2
                    continue
                if text[i] == quote:
                    i += 1
                    break
                i += 1
            continue
        if char == "{":
            depth += 1
        elif char == "}":
            depth -= 1
            if depth == 0:
                return i + 1
        i += 1
    return i


def extract_declarations(text: str) -> list[str]:
    text = strip_preprocessor(text)
    declarations: list[str] = []
    buf: list[str] = []
    depth = 0
    i = 0
    while i < len(text):
        char = text[i]
        if char == "{":
            prefix = "".join(buf)
            if re.fullmatch(r'\s*extern\s+"C"\s*', prefix):
                # C++ linkage wrapper: transparent, not a C declaration.
                buf.clear()
                i += 1
                continue
            if depth == 0 and looks_like_function(prefix):
                declarations.append(normalize_decl(prefix) + " { ... }")
                i = skip_braced(text, i)
                buf.clear()
                continue
            depth += 1
            buf.append(char)
        elif char == "}":
            if depth == 0:
                # Closing brace of the transparent extern "C" block.
                buf.clear()
            else:
                depth -= 1
                buf.append(char)
        elif char == ";" and depth == 0:
            declaration = "".join(buf)
            if declaration.strip():
                declarations.append(normalize_decl(declaration) + ";")
            buf.clear()
        else:
            buf.append(char)
        i += 1
    return declarations


def build_snapshot(inc_dir: Path = DEFAULT_INC) -> str:
    macros: set[str] = set()
    declarations: set[str] = set()

    headers = sorted(inc_dir.glob("*.h"))
    if not headers:
        raise FileNotFoundError("no headers found in %s" % inc_dir)
    for header in headers:
        if header.name == "ow_version.h":
            continue
        text = strip_comments(header.read_text(encoding="utf-8"))
        macros.update(extract_macros(text))
        declarations.update(extract_declarations(text))

    lines = [
        "# Public API snapshot for stm32-async-1wire.",
        "# Generated by tests/api/public_api.py; do not edit by hand.",
        "# Regenerate with `make update-public-api`.",
        "# inc/ow_version.h is excluded: the version has its own gate.",
        "#",
        "# This records declarations, not ABI: renames, removals and signature",
        "# changes show up as a diff. Struct layout is intentionally not pinned.",
        "",
        "[MACROS]",
    ]
    lines.extend(sorted(macros))
    lines.extend(["", "[DECLARATIONS]"])
    lines.extend(sorted(declarations))
    return "\n".join(lines) + "\n"


def write_text(path: Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8", newline="\n") as handle:
        handle.write(text)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true",
                        help="compare inc/ against the committed snapshot (default)")
    parser.add_argument("--update", action="store_true",
                        help="rewrite the snapshot from inc/")
    parser.add_argument("--inc", type=Path, default=DEFAULT_INC,
                        help="header directory (default: inc/)")
    parser.add_argument("--snapshot", type=Path, default=DEFAULT_SNAPSHOT,
                        help="snapshot path (default: tests/api/public-api.txt)")
    args = parser.parse_args(argv)

    if args.update:
        text = build_snapshot(args.inc)
        write_text(args.snapshot, text)
        print("  public_api: wrote %s" % args.snapshot)
        return 0

    expected_path = args.snapshot
    if not expected_path.is_file():
        print("  public_api: snapshot %s is missing; run `make update-public-api`"
              % expected_path, file=sys.stderr)
        return 1
    expected = expected_path.read_text(encoding="utf-8")
    actual = build_snapshot(args.inc)
    if actual != expected:
        print("  public_api: inc/ no longer matches %s" % expected_path, file=sys.stderr)
        diff = difflib.unified_diff(
            expected.splitlines(), actual.splitlines(),
            fromfile=str(expected_path), tofile="inc/ (current)",
            lineterm="",
        )
        for line in diff:
            print(line, file=sys.stderr)
        print("  public_api: review the change; if it is intentional, run "
              "`make update-public-api`", file=sys.stderr)
        return 1

    macro_count = actual.split("[MACROS]", 1)[1].split("[DECLARATIONS]", 1)[0].count("#define")
    decl_count = len(actual.split("[DECLARATIONS]", 1)[1].strip().splitlines())
    print("  public_api: OK (%d macros, %d declarations match %s)"
          % (macro_count, decl_count, expected_path.name))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
