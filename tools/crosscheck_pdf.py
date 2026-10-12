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
        # Bind the page to a local FIRST. `doc[5].annots()` builds a
        # temporary Page object; PyMuPDF's Annot holds only a weak
        # reference to its page, so once the temporary is collected every
        # annot reports "annotation not bound to any page".
        page6 = doc[5]
        annots = list(page6.annots() or [])
        types = sorted({a.type[1] for a in annots})
        check(problems, len(annots) >= 7,
              "%s: page 6 has %d markup/note annotations" % (name, len(annots)))
        for want in ("Text", "Highlight", "Underline", "Squiggly",
                     "StrikeOut", "Square", "Circle"):
            check(problems, want in types,
                  "%s: a /%s annotation is present" % (name, want))

        links = list(page6.links() or [])
        check(problems, len(links) >= 2,
              "%s: page 6 has %d link(s)" % (name, len(links)))
        uris = [l.get("uri") for l in links if l.get("uri")]
        check(problems, any("example.com/pdflib" in (u or "") for u in uris),
              "%s: the external link carries its URI" % name)
        check(problems, any(l.get("page", -1) == 1 for l in links),
              "%s: the internal link jumps to page 2" % name)

        # A markup annotation without /QuadPoints is unusable: it is the
        # quadrilateral that tells the viewer what to mark up, and it is
        # required by the spec. MuPDF falls back to a rectangle spanning
        # INT_MIN..INT_MAX when it is missing, which is how this was
        # caught in the first place.
        for t in page6.annot_xrefs():
            obj = doc.xref_object(t[0])
            for kind in ("/Highlight", "/Underline", "/Squiggly",
                         "/StrikeOut"):
                if kind in obj:
                    check(problems, "/QuadPoints" in obj,
                          "%s: %s carries /QuadPoints" % (name, kind))

        # Every markup annot must also resolve to the rectangle that was
        # asked for, not to a degenerate one.
        for a in annots:
            if a.type[1] in ("Highlight", "Underline", "Squiggly",
                             "StrikeOut"):
                r = a.rect
                sane = (abs(r.x0) < 1e4 and abs(r.y1) < 1e4 and
                        r.width > 1 and r.height > 1)
                check(problems, sane,
                      "%s: %s has a sane rectangle (%s)" %
                      (name, a.type[1], r))

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
    if render_dir:
        os.makedirs(render_dir, exist_ok=True)
        stem = os.path.splitext(name)[0]
        for i in range(doc.page_count):
            pix = doc[i].get_pixmap(dpi=72)
            out = os.path.join(render_dir, "%s-p%d.png" % (stem, i + 1))
            pix.save(out)
            # A page that renders to a single flat colour is almost
            # certainly a page nothing was drawn on. Count distinct
            # colours as a cheap proxy for "there is content here".
            samples = pix.samples
            distinct = len(set(samples[k:k + 3]
                               for k in range(0, len(samples), 3 * 97)))
            check(problems, distinct > 3,
                  "%s: page %d renders with content (%d sampled colours)" %
                  (name, i + 1, distinct))
        print("  [i]    rendered %d page(s) to %s" % (doc.page_count,
                                                      render_dir))

    doc.close()


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("files", nargs="+", help="PDF files to cross-check")
    ap.add_argument("--render", metavar="DIR",
                    help="render every page to DIR as PNG")
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
        check_showcase(path, problems, args.render,
                       {"pages": args.pages})

    if problems:
        print("\n=== CROSS-CHECK FAILED: %d problem(s) ===" % len(problems))
        return 1
    print("\n=== cross-check passed ===")
    return 0


if __name__ == "__main__":
    sys.exit(main())
