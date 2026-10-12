#include "pdflib/pdf_api.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

/*
 * feature_pdf - end-to-end verification of the streaming output and the
 * font-loading paths (file / memory, embed on/off) for a chosen backend:
 *
 *   feature_pdf <podofo|libharu> <output-prefix> [ttf-path]
 *
 * Produces <prefix>-file.pdf   (streamed to an OS handle, TTF loaded from
 *                               file, embedded when available)
 *         <prefix>-stream.pdf  (streamed through the callback API)
 *         <prefix>-mem.pdf     (save_to_memory then fwrite)
 *
 * pdflib takes OS handles rather than FILE* for the "already open" case.
 * The DLL links the CRT statically, so it has its own FILE table: a FILE*
 * opened here belongs to this program's CRT and the DLL's stdio rejects
 * it as an invalid parameter - which aborts the process. An OS handle
 * means the same thing on both sides.
 */

static void *os_open_write(const char *path)
{
#ifdef _WIN32
    HANDLE h = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    return (h == INVALID_HANDLE_VALUE) ? NULL : (void *)h;
#else
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    return (fd < 0) ? NULL : (void *)(intptr_t)fd;
#endif
}

static void os_close(void *h)
{
    if (!h)
        return;
#ifdef _WIN32
    CloseHandle((HANDLE)h);
#else
    close((int)(intptr_t)h);
#endif
}

struct sink {
    FILE *f;
    size_t total;
    int chunks;
};

static pdf_status sink_write(const unsigned char *data, size_t len, void *ud)
{
    struct sink *s = (struct sink *)ud;
    if (len && fwrite(data, 1, len, s->f) != len)
        return PDF_ERR_IO;
    s->total += len;
    s->chunks++;
    return PDF_OK;
}

static int make_doc(pdf_doc *out, const char *ttf)
{
    pdf_doc doc;
    pdf_page page;
    pdf_font std_font, ttf_font;
    double w, h;

    if (pdf_doc_create(&doc) != PDF_OK)
        return 1;
    pdf_doc_set_info(doc, PDF_INFO_TITLE, "pdflib feature check");
    pdf_page_add(doc, PDF_PAGE_A4, PDF_ORIENTATION_PORTRAIT, &page);
    pdf_page_get_size(page, &w, &h);

    if (pdf_font_get_std(doc, PDF_STD_FONT_HELVETICA, &std_font) != PDF_OK)
        return 2;
    pdf_page_draw_text(page, std_font, "standard font (not embedded)",
                       48, h - 60, 16);

    if (ttf)
    {
        pdf_status rc = pdf_font_load_ttf(doc, ttf, 1, &ttf_font);
        if (rc == PDF_OK)
        {
            pdf_page_draw_text(page, ttf_font, "TrueType from file (embedded)",
                               48, h - 100, 16);
            pdf_font_free(ttf_font);
        }
        else
        {
            fprintf(stderr, "note: ttf load from file -> %d (engine limit)\n",
                    rc);
        }
    }

    pdf_set_line_width(page, 1.5);
    pdf_path_circle(page, w / 2, h / 2, 60);
    pdf_path_paint(page, PDF_PATH_STROKE);

    *out = doc;
    return 0;
}

/* Draw one page of a streamed document (printer-driver simulation:
 * the number of pages is only known when the caller runs out of data). */
static int draw_streamed_page(pdf_doc doc, int n, const char *ttf,
                              int *ttf_used)
{
    pdf_page page;
    pdf_font std_font, ttf_font = NULL;
    double w, h;
    char label[128];

    if (pdf_page_add(doc, PDF_PAGE_A4, PDF_ORIENTATION_PORTRAIT, &page)
        != PDF_OK)
        return 1;
    pdf_page_get_size(page, &w, &h);

    if (pdf_font_get_std(doc, PDF_STD_FONT_HELVETICA, &std_font) != PDF_OK)
        return 2;
    snprintf(label, sizeof(label), "streamed page %d (std font)", n);
    pdf_page_draw_text(page, std_font, label, 48, h - 60, 16);

    if (ttf && !*ttf_used)
    {
        if (pdf_font_load_ttf(doc, ttf, 1, &ttf_font) == PDF_OK)
        {
            snprintf(label, sizeof(label),
                     "subset TTF page %d: ABCDEFG 0123", n);
            pdf_page_draw_text(page, ttf_font, label, 48, h - 100, 16);
            pdf_font_free(ttf_font);
            *ttf_used = 1;
        }
    }
    return 0;
}

static int run_streamed(const char *prefix, const char *ttf)
{
    pdf_doc doc;
    char path[512];
    int i, ttf_used = 0;
    const int npages = 5; /* "arrives" one at a time */

    snprintf(path, sizeof(path), "%s-streamed.pdf", prefix);
    if (pdf_doc_create_streamed_to_file(path, &doc) != PDF_OK)
    {
        fprintf(stderr, "create_streamed failed (%s): %s\n", path,
                pdflib_last_error(NULL));
        return 1;
    }

    for (i = 1; i <= npages; i++)
    {
        if (draw_streamed_page(doc, i, ttf, &ttf_used) != 0)
        {
            fprintf(stderr, "draw page %d failed\n", i);
            pdf_doc_free(doc);
            return 1;
        }
        /* pages complete one by one; nothing is buffered whole */
    }

    if (pdf_doc_finish(doc) != PDF_OK)
    {
        fprintf(stderr, "finish failed: %s\n", pdflib_last_error(doc));
        pdf_doc_free(doc);
        return 1;
    }
    pdf_doc_free(doc);
    printf("%s: streamed %d pages, subset TTF=%s\n", path, npages,
           ttf_used ? "yes" : "no");
    return 0;
}

int main(int argc, char **argv)
{
    const char *backend = argc > 1 ? argv[1] : "podofo";
    const char *prefix = argc > 2 ? argv[2] : "feature";
    const char *ttf = argc > 3 ? argv[3] : NULL;
    char path[512];
    pdf_doc doc;
    int build_rc;

    if (strcmp(backend, "podofo") == 0)
        pdflib_set_backend(PDF_BACKEND_PODOFO);
    else if (strcmp(backend, "libharu") == 0)
        pdflib_set_backend(PDF_BACKEND_LIBHARU);
    else
        pdflib_set_backend(PDF_BACKEND_DUMMY);

    build_rc = make_doc(&doc, ttf);
    if (build_rc == 2)
    {
        fprintf(stderr, "font_get_std failed\n");
        return 1;
    }

    /* 0) streamed creation with unknown page count (printer driver path).
     *    podofo supports it natively; other engines report UNSUPPORTED. */
    {
        pdf_doc probe = NULL;
        pdf_status rc = pdf_doc_create_streamed_to_file("feature-probe.pdf",
                                                        &probe);
        if (rc == PDF_ERR_BACKEND_UNSUPPORTED)
        {
            printf("streamed creation: unsupported by this backend\n");
        }
        else if (rc == PDF_OK)
        {
            pdf_doc_free(probe);
            remove("feature-probe.pdf");
            if (run_streamed(prefix, ttf) != 0)
                return 1;
        }
        else
        {
            fprintf(stderr, "create_streamed probe failed: %d\n", rc);
            return 1;
        }
    }

    /* 1) streamed straight into an OS handle (here: a plain file) */
    snprintf(path, sizeof(path), "%s-file.pdf", prefix);
    {
        void *h = os_open_write(path);
        if (!h) { fprintf(stderr, "cannot open %s\n", path); return 1; }
        if (pdf_doc_save_to_handle(doc, h) != PDF_OK)
        {
            fprintf(stderr, "save_to_handle failed: %s\n", pdflib_last_error(doc));
            os_close(h);
            return 1;
        }
        os_close(h);
        printf("%s: streamed to an OS handle\n", path);
    }

    /* 2) streamed through the callback API */
    snprintf(path, sizeof(path), "%s-stream.pdf", prefix);
    {
        struct sink s;
        memset(&s, 0, sizeof(s));
        s.f = fopen(path, "wb");
        if (!s.f) { fprintf(stderr, "cannot open %s\n", path); return 1; }
        if (pdf_doc_save_to_stream(doc, sink_write, &s) != PDF_OK)
        {
            fprintf(stderr, "save_to_stream failed: %s\n", pdflib_last_error(doc));
            fclose(s.f);
            return 1;
        }
        fclose(s.f);
        printf("%s: callback stream, %d chunk(s), %lu bytes\n", path,
               s.chunks, (unsigned long)s.total);
    }

    /* 3) serialize to memory, then write */
    snprintf(path, sizeof(path), "%s-mem.pdf", prefix);
    {
        size_t size = 0;
        unsigned char *buf;
        FILE *f;

        if (pdf_doc_save_to_memory(doc, NULL, &size) != PDF_OK)
        { fprintf(stderr, "mem size query failed\n"); return 1; }
        buf = (unsigned char *)malloc(size);
        if (!buf) return 1;
        if (pdf_doc_save_to_memory(doc, buf, &size) != PDF_OK)
        { fprintf(stderr, "save_to_memory failed\n"); free(buf); return 1; }
        f = fopen(path, "wb");
        if (!f) { free(buf); return 1; }
        fwrite(buf, 1, size, f);
        fclose(f);
        printf("%s: memory save, %lu bytes\n", path, (unsigned long)size);
        free(buf);
    }

    pdf_doc_free(doc);
    printf("done (%s backend)\n", pdflib_backend_version());
    return 0;
}
