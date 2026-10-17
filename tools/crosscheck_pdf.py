#!/usr/bin/env python3
"""Cross-check a PDF that pdflib produced, using an INDEPENDENT parser.

The point of this tool is independence. standalone/pdf_inspect.h is
pdflib's own reader, written by the same people who wrote the writer, so
"my reader agrees with my writer" is weak evidence. This script re-reads
the same bytes with PyMuPDF (MuPDF's Python binding) and asserts the
things a real consumer would care about: the page geometry, the text that
comes out of the content streams, the images, the annotations, the
bookmark tree and the metadata.

It also renders each page, which is the only check that catches a page
that is structurally perfect and visually empty.

    python tools/crosscheck_pdf.py FILE [FILE...] [--render DIR]
    python tools/crosscheck_pdf.py --profile pdfx pdfx-showcase-*.pdf

`--profile` picks which document to expect. Both profiles do the same
kind of work; they differ only in the pages, text and metadata the
document is supposed to have. The pdfx profile additionally asserts that
MuPDF did not have to repair the file, because a content stream that does
not decode still opens - it just opens wrong.

Exit status is non-zero on any mismatch. If PyMuPDF is not installed the
script says so and exits 0, so it can sit in a test suite that must also
build on machines with no Python packages.
"""

import argparse
import os
import re
import sys


def fail(problems, msg):
    problems.append(msg)
    print("  [FAIL] %s" % msg)


def ok(msg):
    print("  [ok]   %s" % msg)


def check(problems, cond, msg):
    if cond:
        ok(msg)
    else:
        fail(problems, msg)
    return cond


def render_pages(doc, name, problems, render_dir):
    """Render every page and assert none of them is visually empty.

    A page can be structurally perfect - page tree, MediaBox, a stream
    that inflates - and still carry nothing. Counting distinct colours in
    the rendered raster is the cheapest way to catch that.
    """
    if not render_dir:
        return
    os.makedirs(render_dir, exist_ok=True)
    stem = os.path.splitext(name)[0]
    for i in range(doc.page_count):
        pix = doc[i].get_pixmap(dpi=72)
        out = os.path.join(render_dir, "%s-p%d.png" % (stem, i + 1))
        pix.save(out)
        samples = pix.samples
        distinct = len(set(samples[k:k + 3]
                           for k in range(0, len(samples), 3 * 97)))
        check(problems, distinct > 3,
              "%s: page %d renders with content (%d sampled colours)" %
              (name, i + 1, distinct))
    print("  [i]    rendered %d page(s) to %s" % (doc.page_count, render_dir))


def check_markup_annotations(doc, page, page_label, name, problems):
    """Assert the markup annotations on `page` are actually usable.

    A markup annotation without /QuadPoints is unusable: it is the
    quadrilateral that tells the viewer what to mark up, and it is
    required by the spec. MuPDF falls back to a rectangle spanning
    INT_MIN..INT_MAX when it is missing, which is how this was caught in
    the first place.
    """
    annots = list(page.annots() or [])
    types = sorted({a.type[1] for a in annots})
    check(problems, len(annots) >= 7,
          "%s: %s has %d markup/note annotations" %
          (name, page_label, len(annots)))
    for want in ("Text", "Highlight", "Underline", "Squiggly",
                 "StrikeOut", "Square", "Circle"):
        check(problems, want in types,
              "%s: a /%s annotation is present" % (name, want))

    for t in page.annot_xrefs():
        obj = doc.xref_object(t[0])
        for kind in ("/Highlight", "/Underline", "/Squiggly",
                     "/StrikeOut"):
            if kind in obj:
                check(problems, "/QuadPoints" in obj,
                      "%s: %s carries /QuadPoints" % (name, kind))

    for a in annots:
        if a.type[1] in ("Highlight", "Underline", "Squiggly",
                         "StrikeOut"):
            r = a.rect
            sane = (abs(r.x0) < 1e4 and abs(r.y1) < 1e4 and
                    r.width > 1 and r.height > 1)
            check(problems, sane,
                  "%s: %s has a sane rectangle (%s)" %
                  (name, a.type[1], r))
    return annots


def check_pdfx_showcase(path, problems, render_dir):
    """Cross-check a document written through the pdfx (libharu-style) API.

    The pdfx showcase is a different document from the pdflib one - seven
    pages instead of eight, the annotations on page 5 rather than page 6,
    and its own text - so it needs its own expectations. What it shares
    with the pdflib showcase is the reason for checking it at all: the
    same bytes are read back by a parser that did not write them.

    The two engines are expected to agree page for page here. That is a
    stronger claim than either engine passing on its own, and it is what
    caught the PoDoFo content-stream corruption: the file opened, the
    page count was right, and MuPDF still reported "incorrect data check"
    because an operator had been appended past the flate payload.
    """
    import pymupdf

    name = os.path.basename(path)
    print("\n=== %s (independent parser: PyMuPDF %s) ===" %
          (name, pymupdf.__doc__ or pymupdf.version[0]))

    try:
        doc = pymupdf.open(path)
    except Exception as exc:                       # noqa: BLE001
        fail(problems, "%s: cannot open: %s" % (name, exc))
        return

    # --- structure --------------------------------------------------
    check(problems, not doc.is_encrypted,
          "%s: opens unencrypted" % name)
    # is_repaired is the single most valuable flag here: MuPDF repairs a
    # file it cannot parse (bad xref, wrong /Length, a stream that does
    # not decode). A repaired file opens, so without this the checks
    # below would all pass on a document that is quietly broken.
    check(problems, not doc.is_repaired,
          "%s: MuPDF did not have to repair the file" % name)
    check(problems, doc.page_count == 7,
          "%s: page count is 7 (got %d)" % (name, doc.page_count))

    # --- geometry ---------------------------------------------------
    if doc.page_count >= 7:
        # MediaBox, not rect: PyMuPDF applies /Rotate to `rect`, so a
        # 400x300 page with /Rotate 90 legitimately reports 300x400.
        p7 = doc[6]
        mb = p7.mediabox
        check(problems,
              abs(mb.width - 400) < 1 and abs(mb.height - 300) < 1,
              "%s: page 7 MediaBox is 400x300 (got %.0fx%.0f)" %
              (name, mb.width, mb.height))
        check(problems, p7.rotation == 90,
              "%s: page 7 /Rotate is 90 (got %d)" % (name, p7.rotation))

        p5 = doc[4]
        check(problems,
              abs(p5.mediabox.width - 842) < 1 and
              abs(p5.mediabox.height - 595) < 1,
              "%s: page 5 is A4 landscape 842x595 (got %.0fx%.0f)" %
              (name, p5.mediabox.width, p5.mediabox.height))

    # --- text -------------------------------------------------------
    joined = "\n".join(doc[i].get_text() for i in range(doc.page_count))

    check(problems, "libharu-style C API" in joined,
          "%s: the cover subtitle extracts" % name)
    # The specimen line is drawn once per standard font, so 14 lines
    # exist. How many a text extractor gets back depends on the engine:
    # Symbol and ZapfDingbats use symbol encodings, and PoDoFo writes a
    # ToUnicode map for them while libharu does not, so PoDoFo yields 14
    # and libharu yields 12. The band [12, 14] therefore is the assertion
    # that means something here - a line drawn twice or not at all would
    # fall outside it, which "contains" would not catch.
    if doc.page_count >= 2:
        specimen = doc[1].get_text().count("The quick brown fox")
        check(problems, 12 <= specimen <= 14,
              "%s: page 2 draws %d of the 14 font specimens "
              "(12-14 expected)" % (name, specimen))
    check(problems, "Correction" in joined and "Index cards" in joined,
          "%s: the table body extracts (first and last row)" % name)
    check(problems, "400 x 300, rotated 90" in joined,
          "%s: the rotated page's text extracts" % name)
    check(problems, "\u4e2d\u6587\u793a\u4f8b" in joined,
          "%s: the CJK string extracts as real Unicode" % name)

    # Every page must carry text; a page that lost its content stream
    # still reports the right geometry, so check the count per page.
    for i in range(doc.page_count):
        n = len(doc[i].get_text().strip())
        check(problems, n > 10,
              "%s: page %d carries text (%d chars)" % (name, i + 1, n))

    # --- fonts ------------------------------------------------------
    # The 14 standard fonts are referenced by name in the resource
    # dictionary, so this check does not depend on whether a glyph can be
    # mapped back to Unicode - which is exactly what varies per engine.
    standard = {"Courier", "Courier-Bold", "Courier-Oblique",
                "Courier-BoldOblique", "Helvetica", "Helvetica-Bold",
                "Helvetica-Oblique", "Helvetica-BoldOblique",
                "Times-Roman", "Times-Bold", "Times-Italic",
                "Times-BoldItalic", "Symbol", "ZapfDingbats"}
    fonts = set()
    for i in range(doc.page_count):
        for f in doc[i].get_fonts(full=True):
            fonts.add(re.sub(r"^[A-Z]{6}\+", "", f[3]))
    missing = sorted(standard - fonts)
    check(problems, not missing,
          "%s: all 14 standard fonts are in the resources (missing %s)" %
          (name, missing or "none"))

    # Evidence of embedding is the font PROGRAM in a /FontDescriptor, not
    # the "ABCDEF+Family" subset tag: the tag is a convention, and PoDoFo
    # omits it for fonts built from a memory buffer.
    faces = set()
    for xref in range(1, doc.xref_length()):
        obj = doc.xref_object(xref)
        if "/FontDescriptor" not in obj:
            continue
        for key in ("/FontFile", "/FontFile2", "/FontFile3"):
            if key in obj:
                m = re.search(r"/FontName\s*/([^\s/\]>]+)", obj)
                faces.add(m.group(1) if m else "?")
                break
    check(problems, len(faces) >= 2,
          "%s: %d distinct embedded face(s): %s" %
          (name, len(faces), sorted(faces)))

    # --- images -----------------------------------------------------
    total_images = 0
    for i in range(doc.page_count):
        total_images += len(doc[i].get_images(full=True))
    check(problems, total_images >= 3,
          "%s: %d image placement(s) found" % (name, total_images))

    # --- annotations and links --------------------------------------
    if doc.page_count >= 5:
        # Bind the page to a local first: PyMuPDF's Annot keeps only a
        # weak reference to its page, so `doc[4].annots()` yields annots
        # that report "not bound to any page" once the temporary dies.
        page5 = doc[4]
        check_markup_annotations(doc, page5, "page 5", name, problems)

        links = list(page5.links() or [])
        check(problems, len(links) >= 2,
              "%s: page 5 has %d link(s)" % (name, len(links)))
        uris = [l.get("uri") for l in links if l.get("uri")]
        check(problems, any("github.com/tdyx87/pdflib" in (u or "")
                            for u in uris),
              "%s: the external link carries its URI" % name)
        check(problems, any(l.get("page", -1) == 5 for l in links),
              "%s: the internal link jumps to the table page" % name)

    # --- outline ----------------------------------------------------
    toc = doc.get_toc()
    check(problems, len(toc) >= 12,
          "%s: bookmark tree has %d entries" % (name, len(toc)))
    if toc:
        check(problems, toc[0][0] == 1 and toc[0][1] == "pdfx showcase",
              "%s: the first bookmark is the top-level title" % name)
        check(problems, any(t[0] >= 3 for t in toc),
              "%s: the tree is nested three deep" % name)

    # --- metadata ---------------------------------------------------
    md = doc.metadata or {}
    check(problems, md.get("title") == "pdfx showcase",
          "%s: /Title is %r" % (name, md.get("title")))
    check(problems, md.get("author") == "pdflib test suite",
          "%s: /Author is %r" % (name, md.get("author")))

    render_pages(doc, name, problems, render_dir)
    doc.close()


def check_showcase(path, problems, render_dir, expect):
    import pymupdf

    name = os.path.basename(path)
    print("\n=== %s (independent parser: PyMuPDF %s) ===" %
          (name, pymupdf.__doc__ or pymupdf.version[0]))

    try:
        doc = pymupdf.open(path)
    except Exception as exc:                       # noqa: BLE001
        fail(problems, "%s: cannot open: %s" % (name, exc))
        return

    # --- structure --------------------------------------------------
    check(problems, not doc.is_encrypted or doc.needs_pass,
          "%s: opens (encrypted=%s)" % (name, doc.is_encrypted))
    check(problems, doc.page_count == expect["pages"],
          "%s: page count is %d (got %d)" % (name, expect["pages"],
                                             doc.page_count))

    # --- geometry ---------------------------------------------------
    if doc.page_count >= 8:
        p8 = doc[7]
        # Compare against mediabox, NOT rect: PyMuPDF's `rect` has the
        # page's /Rotate applied, so a 400x300 page with /Rotate 90
        # legitimately reports 300x400 there.
        mb = p8.mediabox
        check(problems,
              abs(mb.width - 400) < 1 and abs(mb.height - 300) < 1,
              "%s: page 8 MediaBox is 400x300 (got %.0fx%.0f)" %
              (name, mb.width, mb.height))
        check(problems, p8.rotation == 90,
              "%s: page 8 /Rotate is 90 (got %d)" % (name, p8.rotation))

        p5 = doc[4]
        check(problems, p5.mediabox.width > p5.mediabox.height,
              "%s: page 5 is landscape (%.0fx%.0f)" %
              (name, p5.mediabox.width, p5.mediabox.height))

    # --- text -------------------------------------------------------
    all_text = []
    for i in range(doc.page_count):
        all_text.append(doc[i].get_text())
    joined = "\n".join(all_text)

    check(problems, "pdflib" in joined, "%s: the cover title extracts" % name)
    check(problems, "left aligned" in joined and "right aligned" in joined,
          "%s: aligned text extracts" % name)
    check(problems, "Wrapped text is laid out" in joined,
          "%s: the wrapped paragraph extracts" % name)
    check(problems, "pdf_page_draw_text" in joined,
          "%s: the table body extracts" % name)
    check(problems, "\u4e2d\u6587\u793a\u4f8b" in joined,
          "%s: the CJK string extracts as real Unicode" % name)

    # --- fonts ------------------------------------------------------
    fonts = set()
    for i in range(doc.page_count):
        for f in doc[i].get_fonts(full=True):
            fonts.add(f[3])

    # Count embedded faces by looking for the font PROGRAM, not by the
    # "ABCDEF+Family" subset tag: a subset tag is conventional, not
    # required, and PoDoFo omits it for fonts built from a memory buffer.
    # A /FontDescriptor holding /FontFile, /FontFile2 or /FontFile3 is the
    # actual evidence that the program is in the file.
    faces = set()
    for xref in range(1, doc.xref_length()):
        obj = doc.xref_object(xref)
        if "/FontDescriptor" not in obj:
            continue
        for key in ("/FontFile", "/FontFile2", "/FontFile3"):
            if key in obj:
                m = re.search(r"/FontName\s*/([^\s/\]>]+)", obj)
                faces.add(m.group(1) if m else "?")
                break

    check(problems, len(fonts) >= 4,
          "%s: %d distinct font(s) in use" % (name, len(fonts)))
    # Two embedded faces is the floor this document guarantees: an embedded
    # Latin face and an embedded CJK face. libharu has no in-memory font
    # loader, so the third face (loaded through that entry point) exists on
    # PoDoFo only - the showcase's own read-back asserts the exact count
    # per engine, because only it knows how many faces it asked for.
    check(problems, len(faces) >= 2,
          "%s: %d distinct embedded face(s): %s" %
          (name, len(faces), sorted(faces)))

    # --- images -----------------------------------------------------
    total_images = 0
    got_256 = False
    for i in range(doc.page_count):
        for img in doc[i].get_images(full=True):
            total_images += 1
            if img[2] == 256 and img[3] == 256:
                got_256 = True
    check(problems, total_images >= 1,
          "%s: %d image placement(s) found" % (name, total_images))
    check(problems, got_256,
          "%s: a 256x256 image XObject is present" % name)

    # --- annotations ------------------------------------------------
    # Note: PyMuPDF exposes link annotations through page.links(), not
    # through page.annots(), so the two are collected separately.
    if doc.page_count >= 6:
        page6 = doc[5]
        check_markup_annotations(doc, page6, "page 6", name, problems)

        links = list(page6.links() or [])
        check(problems, len(links) >= 2,
              "%s: page 6 has %d link(s)" % (name, len(links)))
        uris = [l.get("uri") for l in links if l.get("uri")]
        check(problems, any("example.com/pdflib" in (u or "") for u in uris),
              "%s: the external link carries its URI" % name)
        check(problems, any(l.get("page", -1) == 1 for l in links),
              "%s: the internal link jumps to page 2" % name)

    # --- outline ----------------------------------------------------
    toc = doc.get_toc()
    check(problems, len(toc) >= 12,
          "%s: bookmark tree has %d entries" % (name, len(toc)))
    if toc:
        check(problems, toc[0][0] == 1,
              "%s: the first bookmark is top level" % name)
        check(problems, any(t[0] >= 2 for t in toc),
              "%s: the tree is nested" % name)

    # --- metadata ---------------------------------------------------
    md = doc.metadata or {}
    check(problems, md.get("title") == "pdflib feature showcase",
          "%s: /Title is %r" % (name, md.get("title")))
    check(problems, md.get("author") == "pdflib test suite",
          "%s: /Author is %r" % (name, md.get("author")))

    # --- rendering --------------------------------------------------
    render_pages(doc, name, problems, render_dir)

    doc.close()


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("files", nargs="+", help="PDF files to cross-check")
    ap.add_argument("--render", metavar="DIR",
                    help="render every page to DIR as PNG")
    ap.add_argument("--profile", choices=("showcase", "pdfx"),
                    default="showcase",
                    help="which document to expect: the pdflib showcase "
                         "(default) or the pdfx (libharu-style API) showcase")
    ap.add_argument("--pages", type=int, default=8,
                    help="expected page count for a showcase document")
    args = ap.parse_args()

    try:
        import pymupdf  # noqa: F401
    except ImportError:
        print("PyMuPDF is not installed - skipping the independent "
              "cross-check (pip install pymupdf).")
        return 0

    problems = []
    for path in args.files:
        if not os.path.exists(path):
            fail(problems, "%s does not exist" % path)
            continue
        if args.profile == "pdfx":
            check_pdfx_showcase(path, problems, args.render)
        else:
            check_showcase(path, problems, args.render,
                           {"pages": args.pages})

    if problems:
        print("\n=== CROSS-CHECK FAILED: %d problem(s) ===" % len(problems))
        return 1
    print("\n=== cross-check passed ===")
    return 0


if __name__ == "__main__":
    sys.exit(main())
