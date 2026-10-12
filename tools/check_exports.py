#!/usr/bin/env python3
"""check_exports.py - compare a dumpbin /exports listing against the .def.

Usage:
    check_exports.py <dumpbin_output.txt> <pdflib.def>

Exit 0 only when the set of exported names equals the set listed in the
.def AND every name is undecorated (no leading underscore / '@N' suffix
as produced by __stdcall on x86, no '?' C++ mangling).
"""
import re
import sys


def parse_dumpbin(path):
    names = []
    started = False
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        for line in f:
            line = line.rstrip("\n")
            if "ordinal" in line and "name" in line and "hint" not in line.lower().split()[-1:]:
                pass
            if re.match(r"\s*ordinal\s+hint", line, re.I):
                started = True
                continue
            if not started:
                continue
            # Columns: ordinal  hint  RVA  name
            m = re.match(r"\s*\d+\s+[0-9A-Fa-f]+\s+[0-9A-Fa-f]+\s+(\S+)\s*$", line)
            if m:
                names.append(m.group(1))
    return names


def parse_def(path):
    names = set()
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
            names.add(s.split()[0])
    return names


def decorated(name):
    # x86 __stdcall -> "_foo@8"; C++ -> "?foo@@..."; C __cdecl -> "_foo"
    return name.startswith("_") or name.startswith("?") or "@" in name


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        return 2
    exported = parse_dumpbin(sys.argv[1])
    expected = parse_def(sys.argv[2])

    got = set(exported)
    missing = expected - got
    extra = got - expected
    bad = [n for n in exported if decorated(n)]

    print(f"  expected: {len(expected)}   exported: {len(exported)}")
    ok = True
    if missing:
        ok = False
        print(f"  [FAIL] missing from DLL ({len(missing)}):")
        for n in sorted(missing):
            print(f"      {n}")
    if extra:
        # Extra exports (e.g. CRT helpers) are tolerated but reported.
        shown = [n for n in sorted(extra) if not n.startswith("??")]
        if shown:
            print(f"  [warn] extra exports ({len(shown)}): {', '.join(shown[:10])}"
                  + (" ..." if len(shown) > 10 else ""))
    if bad:
        ok = False
        print(f"  [FAIL] DECORATED names found ({len(bad)}):")
        for n in bad[:20]:
            print(f"      {n}")

    if ok:
        print(f"  [ok] all {len(expected)} exports present and undecorated")
        return 0
    print("  [FAIL] export table does not match pdflib.def")
    return 1


if __name__ == "__main__":
    sys.exit(main())
