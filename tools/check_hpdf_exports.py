"""Compare a dumpbin /exports listing of pdflib_hpdf.dll against hpdf.h."""
import re
import sys

exports_path = sys.argv[1] if len(sys.argv) > 1 else "build/hpdf-shared-exports.txt"
header_path = sys.argv[2] if len(sys.argv) > 2 else "compat/include/hpdf.h"

exported = set()
started = False
for line in open(exports_path, encoding="utf-8", errors="replace"):
    if "ordinal" in line and "name" in line:
        started = True
        continue
    if started:
        m = re.match(r"\s*\d+\s+[0-9A-Fa-f]+\s+[0-9A-Fa-f]+\s+(\S+)", line)
        if m:
            exported.add(m.group(1))

declared = set()
for line in open(header_path, encoding="utf-8", errors="replace"):
    m = re.match(r"(HPDF_[A-Za-z0-9_]+)\s*\(", line)
    if m:
        declared.add(m.group(1))

print("exported:", len(exported))
print("declared:", len(declared))
missing = sorted(declared - exported)
extra = sorted(exported - declared)
if missing:
    print("missing exports:", missing)
if extra:
    print("extra exports:", extra)
sys.exit(1 if missing else 0)
