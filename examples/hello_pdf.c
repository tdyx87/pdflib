#include "pdflib/pdf_api.h"

#include <stdio.h>
#include <string.h>

/*
 * hello_pdf - draws a one-page document with text and a rectangle using
 * whichever backend is currently active. Run it with different engines:
 *
 *   pdf_hello            -> default (PoDoFo, else libharu, else dummy)
 *   PDFLIB_BACKEND=podofo pdf_hello
 *   PDFLIB_BACKEND=libharu pdf_hello
 */
int main(int argc, char **argv)
{
    const char *out_path = argc > 1 ? argv[1] : "hello.pdf";
    const char *want = argc > 2 ? argv[2] : NULL;
    pdf_doc doc;
    pdf_page page;
    pdf_font font;
    pdf_color blue, dark;
    pdf_rect box;
    double w, h;
    pdf_status rc;

    if (want)
    {
        if (strcmp(want, "podofo") == 0)
            pdflib_set_backend(PDF_BACKEND_PODOFO);
        else if (strcmp(want, "libharu") == 0)
            pdflib_set_backend(PDF_BACKEND_LIBHARU);
        else if (strcmp(want, "dummy") == 0)
            pdflib_set_backend(PDF_BACKEND_DUMMY);
    }

    rc = pdf_doc_create(&doc);
    if (rc != PDF_OK)
    {
        fprintf(stderr, "create failed: %d (%s)\n", rc, pdflib_last_error(doc));
        return 1;
    }

    pdf_doc_set_info(doc, PDF_INFO_TITLE, "pdflib hello");
    pdf_doc_set_info(doc, PDF_INFO_CREATOR, "hello_pdf example");

    pdf_page_add(doc, PDF_PAGE_A4, PDF_ORIENTATION_PORTRAIT, &page);
    pdf_page_get_size(page, &w, &h);

    if (pdf_font_get_std(doc, PDF_STD_FONT_HELVETICA, &font) != PDF_OK)
    {
        fprintf(stderr, "font failed: %s\n", pdflib_last_error(doc));
        return 1;
    }

    /* banner */
    blue.kind = PDF_COLOR_RGB;
    blue.c1 = 0.15;
    blue.c2 = 0.35;
    blue.c3 = 0.75;
    blue.c4 = 0;
    blue.alpha = 1;
    pdf_set_fill_color(page, &blue);
    box.x = 0;
    box.y = h - 100;
    box.w = w;
    box.h = 100;
    pdf_page_draw_rect(page, &box, PDF_PATH_FILL);

    dark.kind = PDF_COLOR_RGB;
    dark.c1 = dark.c2 = dark.c3 = 0.1;
    dark.c4 = 0;
    dark.alpha = 1;
    pdf_set_fill_color(page, &dark);
    pdf_page_draw_text(page, font, "Hello from pdflib", 48, h - 60, 28);
    pdf_page_draw_text(page, font, pdflib_backend_version(), 48, h - 88, 12);

    /* stroked circle */
    pdf_set_line_width(page, 2.0);
    pdf_set_stroke_color(page, &blue);
    pdf_path_circle(page, w / 2, h / 2, 80);
    pdf_path_paint(page, PDF_PATH_STROKE);

    rc = pdf_doc_save_to_file(doc, out_path);
    if (rc != PDF_OK)
    {
        fprintf(stderr, "save failed: %d (%s)\n", rc, pdflib_last_error(doc));
        pdf_doc_free(doc);
        return 1;
    }

    printf("wrote %s (%s backend)\n", out_path, pdflib_backend_version());
    pdf_doc_free(doc);
    return 0;
}
