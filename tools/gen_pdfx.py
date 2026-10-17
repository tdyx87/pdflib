#!/usr/bin/env python3
"""Generate the libharu-style PDFX_* API from the HPDF_* compatibility layer.

PDFX is deliberately *not* a second implementation. The libharu-compatible
facade in compat/ already implements every public 2.4.4 entry point over the
backend-agnostic pdflib core. This generator renames that surface:

    HPDF_New        -> PDFX_New
    HPDF_Page       -> PDFX_Page
    HPDF_PAGE_SIZE_A4 -> PDFX_PAGE_SIZE_A4
    HPDF_OK         -> PDFX_OK
    ...             (all public HPDF identifiers, types, enums, constants)

The generated translation unit is compiled as a separate library (pdfx.lib /
pdfx.dll) and therefore does not collide with the HPDF_* names. It still
dispatches to the selected pdflib backend (libharu, PoDoFo, or a future one).

Usage (from the repository root):
    python tools/gen_pdfx.py
"""
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC_INCLUDE = os.path.join(ROOT, "compat", "include")
DST_INCLUDE = os.path.join(ROOT, "pdfx", "include")
DST_SRC = os.path.join(ROOT, "pdfx")

HEADERS = [
    ("hpdf.h", "pdfx.h"),
    ("hpdf_types.h", "pdfx_types.h"),
    ("hpdf_consts.h", "pdfx_consts.h"),
    ("hpdf_errors.h", "pdfx_errors.h"),
    ("hpdf_config.h", "pdfx_config.h"),
    ("hpdf_version.h", "pdfx_version.h"),
]


def rename(text):
    """Rename every public/internal HPDF identifier and include file."""
    # Include names first (they contain the lower-case prefix followed by _).
    text = text.replace('"hpdf.h"', '"pdfx.h"')
    text = text.replace('"hpdf_', '"pdfx_')
    # Public identifiers.
    text = text.replace("HPDF_", "PDFX_")
    # Internal helper names / include guards / version macros.
    text = text.replace("_HPDF_", "_PDFX_")
    text = text.replace("hpdf_", "pdfx_")
    # The config header uses LIBHPDF_* macros.
    text = text.replace("LIBHPDF_", "LIBPDFX_")
    return text


def main():
    os.makedirs(DST_INCLUDE, exist_ok=True)
    os.makedirs(DST_SRC, exist_ok=True)

    for src_name, dst_name in HEADERS:
        src_path = os.path.join(SRC_INCLUDE, src_name)
        dst_path = os.path.join(DST_INCLUDE, dst_name)
        with open(src_path, "r", encoding="utf-8") as f:
            data = f.read()
        data = rename(data)
        with open(dst_path, "w", encoding="utf-8", newline="\n") as f:
            f.write(data)
        print("generated", os.path.relpath(dst_path, ROOT))

    src_path = os.path.join(ROOT, "compat", "hpdf_compat.c")
    dst_path = os.path.join(DST_SRC, "pdfx_compat.c")
    with open(src_path, "r", encoding="utf-8") as f:
        data = f.read()
    data = rename(data)
    # Distinct version text so PDFX_GetVersion identifies this facade.
    data = data.replace('return "2.4.4 (pdflib compat)";',
                        'return "pdflib pdfx 1.0 (multi-backend)";')
    with open(dst_path, "w", encoding="utf-8", newline="\n") as f:
        f.write(data)
    print("generated", os.path.relpath(dst_path, ROOT))
    return 0


if __name__ == "__main__":
    sys.exit(main())
