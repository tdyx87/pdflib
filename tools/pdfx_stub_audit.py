#!/usr/bin/env python3
"""Report which PDFX_* entry points are real, which refuse, and which accept
the call and do nothing.

pdfx is a source-compatible facade, so a libharu program that renames HPDF_ to
PDFX_ compiles and links even when the underlying core cannot express some of
the calls. Three outcomes are possible for such a call, and only two of them
are safe:

  real      forwards to the pdflib core and does the work
  declines  returns PDFX_UNSUPPORTED_FUNC, so the caller sees the failure
  silent    returns PDFX_OK and does nothing

"silent" is the one that hurts: the program keeps running, every call reports
success, and the difference only shows up in the output file. This tool exists
to make that set visible, and to intersect it with the calls a particular
translation unit actually makes.

Usage:
    python tools/pdfx_stub_audit.py                  # summary + full lists
    python tools/pdfx_stub_audit.py --list silent    # just one category
    python tools/pdfx_stub_audit.py --used-by app.c  # what app.c is exposed to

Exit status is 1 if --used-by finds a silent stub in the given sources, so it
can gate a migration in CI.
"""

import argparse
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
COMPAT = os.path.join(ROOT, "pdfx", "pdfx_compat.c")
DEF = os.path.join(ROOT, "pdfx", "pdfx.def")

CATEGORIES = ("real", "declines", "silent", "constant")


def strip_comments(text):
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    text = re.sub(r"//[^\n]*", "", text)
    return text


def find_functions(src):
    """Return [(name, body)] for every PDFX_* definition.

    Definition lines start at column 0; argument continuation lines are
    indented, which is what separates a definition from its parameter list.
    """
    lines = src.split("\n")
    out = []
    for i, line in enumerate(lines):
        if not line or line[0].isspace() or line.startswith("#"):
            continue
        m = re.search(r"\b(PDFX_[A-Za-z0-9_]+)\s*\(", line)
        if not m or line.rstrip().endswith(";"):
            continue
        body = read_body(lines, i)
        if body is not None:
            out.append((m.group(1), body))
    return out


def read_body(lines, start):
    depth, started, buf = 0, False, []
    for line in lines[start:start + 300]:
        for ch in line:
            if ch == "{":
                depth += 1
                started = True
            elif ch == "}":
                depth -= 1
            if started:
                buf.append(ch)
        if started:
            buf.append("\n")
        if started and depth == 0:
            return "".join(buf)
    return None


def classify(body):
    code = strip_comments(body)
    if "PDFX_UNSUPPORTED_FUNC" in code:
        return "declines", None
    if re.search(r"\b(pdf_[a-z_0-9]+|pdfx_[a-z_0-9]+)\s*\(", code):
        return "real", None
    stripped = re.sub(r"PDFX_UNUSED\s*\([^)]*\)\s*;", "", code)
    stripped = stripped.replace("{", "").replace("}", "").strip()
    parts = [p.strip() for p in stripped.split(";") if p.strip()]
    if parts and all(p.startswith("return") for p in parts):
        values = [re.match(r"return\s+(.*)", p).group(1).strip() for p in parts]
        if all(v == "PDFX_OK" for v in values):
            return "silent", values[0]
        return "constant", ", ".join(values)
    return "real", None


def def_names(path):
    names = []
    with open(path, encoding="utf-8", errors="replace") as fh:
        for line in fh:
            line = line.split(";")[0].strip()
            if line.startswith("PDFX_"):
                names.append(line.split()[0])
    return names


def collect():
    with open(COMPAT, encoding="utf-8", errors="replace") as fh:
        src = fh.read()
    table = {}
    for name, body in find_functions(src):
        table[name] = classify(body)
    return table


def calls_in(paths):
    """PDFX_* identifiers referenced by the given source files."""
    seen = set()
    for path in paths:
        with open(path, encoding="utf-8", errors="replace") as fh:
            text = strip_comments(fh.read())
        for m in re.finditer(r"\b(PDFX_[A-Za-z0-9_]+)\s*\(", text):
            seen.add(m.group(1))
    return seen


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--list", choices=CATEGORIES,
                    help="print only this category, one name per line")
    ap.add_argument("--used-by", nargs="+", metavar="FILE",
                    help="check the PDFX_* calls made by these sources")
    ap.add_argument("--quiet", action="store_true",
                    help="only print the verdict for --used-by")
    args = ap.parse_args(argv)

    exports = def_names(DEF)
    table = collect()
    missing = [n for n in exports if n not in table]
    counts = {c: 0 for c in CATEGORIES}
    by_cat = {c: [] for c in CATEGORIES}
    for name in exports:
        cat = table.get(name, ("declines", None))[0]
        counts[cat] += 1
        by_cat[cat].append(name)

    if args.list:
        for name in by_cat[args.list]:
            print(name)
        return 0

    if not args.quiet:
        print("pdfx entry points: %d exported" % len(exports))
        print("  real      %4d  forwards to the core and does the work"
              % counts["real"])
        print("  declines  %4d  returns PDFX_UNSUPPORTED_FUNC (safe)"
              % counts["declines"])
        print("  silent    %4d  returns PDFX_OK and does nothing"
              % counts["silent"])
        print("  constant  %4d  returns a fixed value (alias or limit)"
              % counts["constant"])
        if missing:
            print("\n[!] %d export(s) have no definition in pdfx_compat.c:"
                  % len(missing))
            for name in missing:
                print("    " + name)

        print("\n--- silent: accepted, then dropped ---")
        for name in by_cat["silent"]:
            print("  %-44s %s" % (name, table[name][1]))
        print("\n--- constant: fixed return ---")
        for name in by_cat["constant"]:
            print("  %-44s %s" % (name, table[name][1]))

    if not args.used_by:
        return 0

    used = calls_in(args.used_by)
    known = [n for n in sorted(used) if n in table]
    unknown = [n for n in sorted(used) if n not in table]
    risky = [(n, table[n][0]) for n in known
             if table[n][0] in ("silent", "declines")]

    print("\n=== calls in %s ===" % ", ".join(args.used_by))
    print("  PDFX_* calls found : %d" % len(used))
    print("  resolved           : %d" % len(known))
    if unknown:
        print("  not in this build  : %d" % len(unknown))
        for name in unknown:
            print("      %s" % name)

    silent_hits = [n for n, c in risky if c == "silent"]
    decline_hits = [n for n, c in risky if c == "declines"]

    if silent_hits:
        print("\n  [!] %d call(s) will be accepted and silently ignored:"
              % len(silent_hits))
        for name in silent_hits:
            print("      %-42s returns PDFX_OK, does nothing" % name)
    if decline_hits:
        print("\n  [i] %d call(s) will report PDFX_UNSUPPORTED_FUNC:"
              % len(decline_hits))
        for name in decline_hits:
            print("      %s" % name)
    if not risky:
        print("\n  [ok] no call in these sources hits a stub")

    return 1 if silent_hits else 0


if __name__ == "__main__":
    sys.exit(main())
