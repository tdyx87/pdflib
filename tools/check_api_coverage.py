#!/usr/bin/env python3
"""check_api_coverage.py - prove the API test really touches every export.

Usage:
    check_api_coverage.py [test_source] [pdflib.def]

Defaults: standalone/apitest.c and src/pdflib.def, resolved relative to the
repository root (the parent of this script's directory).

Why this exists: standalone/apitest.c is the "does the whole library work"
program, and it is easy for it to drift - a new export gets added to the
.def and the header, the test keeps passing, and the new entry point is
never called by anything. tools/check_exports.py already proves the built
DLL matches the .def; this proves the test exercises what the .def
promises.

Comments and string literals are stripped before the search. That matters:
apitest.c's banner comment lists the functions it covers, so a naive grep
would "find" every name in the comment and the check would be worthless.

Exit 0 only when every name in the .def appears as an identifier in the
test source.
"""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

DEFAULT_TEST = os.path.join(ROOT, "standalone", "apitest.c")
DEFAULT_DEF = os.path.join(ROOT, "src", "pdflib.def")


def parse_def(path):
    """Return the export names listed under EXPORTS in a .def file."""
    names = []
    in_exports = False
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        for line in f:
            s = line.split(";", 1)[0].strip()
            if not s:
                continue
            if s.upper().startswith("EXPORTS"):
                in_exports = True
                continue
            if not in_exports:
                continue
            names.append(s.split()[0])
    return names


def strip_comments_and_strings(src):
    """Blank out comments and literals, preserving newlines and length.

    Keeping the length identical means line numbers in any later report
    stay correct.
    """
    out = []
    i = 0
    n = len(src)
    while i < n:
        c = src[i]
        nxt = src[i + 1] if i + 1 < n else ""

        if c == "/" and nxt == "*":
            out.append("  ")
            i += 2
            while i < n and not (src[i] == "*" and i + 1 < n and src[i + 1] == "/"):
                out.append("\n" if src[i] == "\n" else " ")
                i += 1
            if i < n:
                out.append("  ")
                i += 2
            continue

        if c == "/" and nxt == "/":
            while i < n and src[i] != "\n":
                out.append(" ")
                i += 1
            continue

        if c in ('"', "'"):
            quote = c
            out.append(" ")
            i += 1
            while i < n and src[i] != quote:
                if src[i] == "\\" and i + 1 < n:
                    out.append("  ")
                    i += 2
                    continue
                out.append("\n" if src[i] == "\n" else " ")
                i += 1
            if i < n:
                out.append(" ")
                i += 1
            continue

        out.append(c)
        i += 1
    return "".join(out)


def main():
    test_path = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_TEST
    def_path = sys.argv[2] if len(sys.argv) > 2 else DEFAULT_DEF

    for p in (test_path, def_path):
        if not os.path.isfile(p):
            print(f"  [FAIL] not found: {p}")
            return 2

    exports = parse_def(def_path)
    with open(test_path, "r", encoding="utf-8", errors="replace") as f:
        code = strip_comments_and_strings(f.read())

    covered = []
    missing = []
    for name in exports:
        if re.search(r"\b" + re.escape(name) + r"\b", code):
            covered.append(name)
        else:
            missing.append(name)

    print(f"  exports in .def : {len(exports)}")
    print(f"  called by test  : {len(covered)}")

    if missing:
        print(f"  [FAIL] never referenced by {os.path.basename(test_path)} "
              f"({len(missing)}):")
        for n in missing:
            print(f"      {n}")
        return 1

    print(f"  [ok] all {len(exports)} exports are exercised")
    return 0


if __name__ == "__main__":
    sys.exit(main())
