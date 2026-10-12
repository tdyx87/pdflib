#!/usr/bin/env python3
"""Append a catch-all safety net after every `catch (const std::exception &e)`
block in the PoDoFo backend.

A C++ exception that is not derived from std::exception cannot be caught by
`catch (const std::exception &)`; if it is thrown inside a backend function it
unwinds straight across the C ABI boundary and terminates the host process.
For a DLL advertised as callable from any language that is unacceptable, so
every backend entry point gets a second handler that turns the throw into a
PDF_ERR_BACKEND_FAILED status code.

The inserted handler re-uses the same first argument the std::exception
handler uses, so the per-document error string still gets updated.
"""
import re
import sys
import shutil

SRC = "pdflib/backends/pdf_backend_podofo.cpp"

src = open(SRC, "r", encoding="utf-8").read()
lines = src.split("\n")

out = []
i = 0
patched = 0
while i < len(lines):
    line = lines[i]
    out.append(line)
    if line.strip() == "catch (const std::exception &e)":
        # expect: indented '{', the return, and the closing '}'
        if (i + 3 < len(lines)
                and lines[i + 1].strip() == "{"
                and "return pf_catch(" in lines[i + 2]
                and lines[i + 3].strip() == "}"):
            indent = line[:len(line) - len(line.lstrip())]
            ret = lines[i + 2]
            m = re.match(r"\s*return pf_catch\((.*?),\s*e\s*,", ret)
            arg = m.group(1) if m else "nullptr"
            out.append(lines[i + 1])           # {
            out.append(lines[i + 2])           # return pf_catch(...)
            out.append(lines[i + 3])           # }
            out.append(indent + "catch (...)")
            out.append(indent + "{")
            out.append(indent + "    return pf_foreign(%s);" % arg)
            out.append(indent + "}")
            patched += 1
            i += 4
            continue
    i += 1

open(SRC, "w", encoding="utf-8").write("\n".join(out))
print("patched %d catch blocks" % patched)
if patched == 0:
    sys.exit(1)
