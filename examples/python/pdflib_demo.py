#!/usr/bin/env python3
"""
pdflib_demo.py - drive pdflib.dll from Python via ctypes.

This demonstrates the two things that matter for cross-language use:

  1. a plain FFI binding (no compiler, no SWIG) - the DLL exports
     undecorated __cdecl symbols, so ctypes finds them directly;
  2. streaming generation - a page count that is not known up front and
     bounded memory, by passing a Python callback as the sink.

Run:
    python pdflib_demo.py            # writes demo_out.pdf + demo_stream.pdf
"""

import ctypes
import os
import sys

# --------------------------------------------------------------------------
# Locate the DLL (adapt to wherever you installed it).
# --------------------------------------------------------------------------
HERE = os.path.dirname(os.path.abspath(__file__))
CANDIDATES = [
    os.path.join(HERE, "pdflib.dll"),
    os.path.join(HERE, "..", "..", "dist", "x64", "pdflib.dll"),
    os.path.join(HERE, "..", "..", "dist", "x86", "pdflib.dll"),
    "pdflib.dll",
]
dll_path = next((p for p in CANDIDATES if os.path.isfile(p)), None)
if dll_path is None:
    sys.exit("pdflib.dll not found; pass its path or drop it next to this script")

pdf = ctypes.CDLL(dll_path)

# --------------------------------------------------------------------------
# Status codes / enums (from pdflib.h).
# --------------------------------------------------------------------------
PDF_OK = 0
PDF_ERR_BACKEND_UNSUPPORTED = 4
PDF_BACKEND_LIBHARU = 2
PDF_BACKEND_PODOFO = 3

PDF_PAGE_A4 = 0
PDF_ORIENTATION_PORTRAIT = 0
PDF_STD_FONT_HELVETICA = 4
PDF_STD_FONT_TIMES_ROMAN = 0
PDF_PATH_FILL_STROKE = 2
PDF_COLOR_RGB = 1
PDF_ALIGN_CENTER = 1


class PdfColor(ctypes.Structure):
    _fields_ = [
        ("kind", ctypes.c_int),
        ("c1", ctypes.c_double),
        ("c2", ctypes.c_double),
        ("c3", ctypes.c_double),
        ("c4", ctypes.c_double),
        ("alpha", ctypes.c_double),
    ]


class PdfRect(ctypes.Structure):
    _fields_ = [
        ("x", ctypes.c_double),
        ("y", ctypes.c_double),
        ("w", ctypes.c_double),
        ("h", ctypes.c_double),
    ]


# --------------------------------------------------------------------------
# Prototypes. Handles are opaque pointers -> c_void_p.
# --------------------------------------------------------------------------
pdf.pdf_dll_version.restype = ctypes.c_char_p
pdf.pdf_dll_backends.restype = ctypes.c_char_p
pdf.pdf_doc_create.argtypes = [ctypes.POINTER(ctypes.c_void_p)]
pdf.pdf_page_add.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_int,
                             ctypes.POINTER(ctypes.c_void_p)]
pdf.pdf_font_get_std.argtypes = [ctypes.c_void_p, ctypes.c_int,
                                 ctypes.POINTER(ctypes.c_void_p)]
pdf.pdf_page_draw_text.argtypes = [ctypes.c_void_p, ctypes.c_void_p,
                                   ctypes.c_char_p, ctypes.c_double,
                                   ctypes.c_double, ctypes.c_double]
pdf.pdf_set_fill_color.argtypes = [ctypes.c_void_p, ctypes.POINTER(PdfColor)]
pdf.pdf_page_draw_rect.argtypes = [ctypes.c_void_p, ctypes.POINTER(PdfRect),
                                   ctypes.c_int]
pdf.pdf_page_draw_text_aligned.argtypes = [
    ctypes.c_void_p, ctypes.c_void_p, ctypes.c_char_p,
    ctypes.c_double, ctypes.c_double, ctypes.c_double,
    ctypes.c_double, ctypes.c_int]
pdf.pdf_doc_save_to_file.argtypes = [ctypes.c_void_p, ctypes.c_char_p]
pdf.pdf_doc_free.argtypes = [ctypes.c_void_p]
pdf.pdf_doc_set_info.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_char_p]
pdf.pdf_doc_set_compression.argtypes = [ctypes.c_void_p, ctypes.c_uint]
pdf.pdf_font_load_ttf.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_int,
                                  ctypes.POINTER(ctypes.c_void_p)]

# Streamed document + sink callback.
SINK = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.POINTER(ctypes.c_ubyte),
                        ctypes.c_size_t, ctypes.c_void_p)
pdf.pdf_doc_create_streamed.argtypes = [SINK, ctypes.c_void_p,
                                        ctypes.POINTER(ctypes.c_void_p)]
pdf.pdf_doc_create_streamed.restype = ctypes.c_int
pdf.pdf_doc_finish.argtypes = [ctypes.c_void_p]
pdf.pdf_page_add_custom.argtypes = [ctypes.c_void_p, ctypes.c_double,
                                    ctypes.c_double,
                                    ctypes.POINTER(ctypes.c_void_p)]


def check(rc, what):
    if rc != PDF_OK:
        raise RuntimeError(f"{what} failed: status {rc}")


# --------------------------------------------------------------------------
# 1) Buffered document: build fully, then save.
# --------------------------------------------------------------------------
def make_buffered(path):
    pdf.pdflib_set_backend(PDF_BACKEND_PODOFO)
    doc = ctypes.c_void_p()
    check(pdf.pdf_doc_create(ctypes.byref(doc)), "pdf_doc_create")
    check(pdf.pdf_doc_set_info(doc, 0, b"pdflib Python demo"), "set title")
    check(pdf.pdf_doc_set_compression(doc, 0xFF), "set compression")

    page = ctypes.c_void_p()
    check(pdf.pdf_page_add(doc, PDF_PAGE_A4, PDF_ORIENTATION_PORTRAIT,
                           ctypes.byref(page)), "page add")
    font = ctypes.c_void_p()
    check(pdf.pdf_font_get_std(doc, PDF_STD_FONT_HELVETICA,
                               ctypes.byref(font)), "std font")

    check(pdf.pdf_page_draw_text(page, font, b"Hello from Python via ctypes",
                                 72, 720, 20), "draw text")
    check(pdf.pdf_page_draw_text_aligned(page, font, b"centered line",
                                         72, 680, 14, 451, PDF_ALIGN_CENTER),
          "aligned text")

    fill = PdfColor(PDF_COLOR_RGB, 0.16, 0.44, 0.86, 0.0, 1.0)
    check(pdf.pdf_set_fill_color(page, ctypes.byref(fill)), "set fill")
    rect = PdfRect(72, 560, 240, 70)
    check(pdf.pdf_page_draw_rect(page, ctypes.byref(rect), PDF_PATH_FILL_STROKE),
          "draw rect")

    check(pdf.pdf_doc_save_to_file(doc, path.encode()), "save to file")
    check(pdf.pdf_doc_free(doc), "free")
    print(f"  wrote {path}")


# --------------------------------------------------------------------------
# 2) Streamed document: page count unknown up front, sink gets bytes early.
# --------------------------------------------------------------------------
def make_streamed(path, pages=150):
    pdf.pdflib_set_backend(PDF_BACKEND_PODOFO)
    fh = open(path, "wb")
    received = {"total": 0, "calls": 0, "header_seen": 0}

    def sink(data, length, ctx):
        # Copy out of the native buffer and append. Returning 0 accepts.
        chunk = ctypes.string_at(data, length) if length else b""
        fh.write(chunk)
        received["total"] += length
        received["calls"] += 1
        if not received["header_seen"] and chunk.startswith(b"%PDF-"):
            received["header_seen"] = received["calls"]
        return 0

    cb = SINK(sink)
    doc = ctypes.c_void_p()
    rc = pdf.pdf_doc_create_streamed(cb, None, ctypes.byref(doc))
    if rc != PDF_OK:
        fh.close()
        os.remove(path)
        print(f"  streaming unsupported (status {rc}); skipped")
        return

    font = ctypes.c_void_p()
    for i in range(pages):
        page = ctypes.c_void_p()
        check(pdf.pdf_page_add_custom(doc, 595.0, 842.0, ctypes.byref(page)),
              "page add custom")
        if not font:
            check(pdf.pdf_font_get_std(doc, PDF_STD_FONT_HELVETICA,
                                       ctypes.byref(font)), "std font")
        check(pdf.pdf_page_draw_text(
            page, font, f"Sheet {i + 1} / {pages}".encode(), 60, 780, 14),
            "draw text")

    check(pdf.pdf_doc_finish(doc), "finish")
    check(pdf.pdf_doc_free(doc), "free")
    fh.close()
    print(f"  wrote {path}: {pages} pages, {received['calls']} sink calls, "
          f"{received['total']} bytes, first chunk at call "
          f"#{received['header_seen']} (streamed, not buffered)")


if __name__ == "__main__":
    print("pdflib DLL :", pdf.pdf_dll_version().decode())
    print("backends   :", pdf.pdf_dll_backends().decode())
    print("[1] buffered document")
    make_buffered("demo_out.pdf")
    print("[2] streamed document (unknown page count)")
    make_streamed("demo_stream.pdf")
    print("done.")
