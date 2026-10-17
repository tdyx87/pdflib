"""Count public HPDF functions defined by the compat layer and how many are
unsupported stubs. Used for the compatibility report; not part of the build."""
import re
import sys

path = sys.argv[1] if len(sys.argv) > 1 else "compat/hpdf_compat.c"
lines = open(path, encoding="utf-8").read().splitlines()
defs = []
cur = None
for line in lines:
    m = re.match(r"^[A-Za-z_][A-Za-z0-9_ \*]*\b(HPDF_[A-Za-z0-9_]+)\s*\(", line)
    if m:
        if cur:
            defs.append(cur)
        cur = {"name": m.group(1), "stub": False}
    elif cur and "HPDF_UNSUPPORTED_FUNC" in line:
        cur["stub"] = True
if cur:
    defs.append(cur)
print("public definitions:", len(defs))
print("unsupported stubs:", sum(1 for d in defs if d["stub"]))
print("working implementations:", sum(1 for d in defs if not d["stub"]))
