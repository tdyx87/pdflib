#include "pdflib/pdf_api.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

/*
 * test_pdflib - exercises the core, the registry and the dummy backend.
 * It links no third-party library, so it always builds (core-only CI).
 */

/* --- minimal OS-handle shim ------------------------------------------
 * pdflib takes OS handles rather than FILE* wherever an "already open"
 * target is involved: the DLL links the CRT statically, so a FILE* opened
 * here belongs to this program's CRT and the DLL's stdio rejects it as an
 * invalid parameter (which aborts the process outright). An OS handle is
 * process-wide and means the same thing on both sides. */
#ifdef _WIN32
#include <windows.h>
static void *test_open_write(const char *path)
{
    HANDLE h = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    return (h == INVALID_HANDLE_VALUE) ? NULL : (void *)h;
}
static void *test_open_read(const char *path)
{
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    return (h == INVALID_HANDLE_VALUE) ? NULL : (void *)h;
}
static void test_close(void *h)
{
    if (h)
        CloseHandle((HANDLE)h);
}
#else
#include <fcntl.h>
#include <stdint.h>
#include <unistd.h>
static void *test_open_write(const char *path)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    return (fd < 0) ? NULL : (void *)(intptr_t)fd;
}
static void *test_open_read(const char *path)
{
    int fd = open(path, O_RDONLY);
    return (fd < 0) ? NULL : (void *)(intptr_t)fd;
}
static void test_close(void *h)
{
    if (h)
        close((int)(intptr_t)h);
}
#endif

#define CHECK(cond)                                                    \
    do                                                                 \
    {                                                                  \
        if (!(cond))                                                   \
        {                                                              \
            fprintf(stderr, "FAILED: %s at line %d\n", #cond,          \
                    __LINE__);                                         \
            failures++;                                                \
        }                                                              \
    } while (0)

static int failures = 0;

static void test_backend_selection(void)
{
    CHECK(pdflib_has_backend(PDF_BACKEND_DUMMY));
    /* dummy must be selectable even when no third-party backend exists */
    CHECK(pdflib_set_backend(PDF_BACKEND_DUMMY) == PDF_OK);
    CHECK(pdflib_get_backend() == PDF_BACKEND_DUMMY);
    CHECK(pdflib_set_backend((pdf_backend)42) == PDF_ERR_BACKEND_NOT_FOUND);
}

static void test_lifecycle(void)
{
    pdf_doc doc = NULL;
    pdf_page page = NULL;
    double w = 0, h = 0;
    size_t count = 0;
    size_t size = 0;

    CHECK(pdf_doc_create(&doc) == PDF_OK);
    CHECK(pdf_page_add(doc, PDF_PAGE_A4, PDF_ORIENTATION_PORTRAIT, &page)
          == PDF_OK);
    CHECK(pdf_doc_page_count(doc, &count) == PDF_OK && count == 1);
    /* dummy reports no geometry; the API surface must still work */
    CHECK(pdf_page_get_size(page, &w, &h) == PDF_OK
          || pdf_page_get_size(page, &w, &h) == PDF_ERR_BACKEND_UNSUPPORTED);
    CHECK(pdf_doc_save_to_memory(doc, NULL, &size) == PDF_OK && size > 0);
    CHECK(pdf_doc_free(doc) == PDF_OK);
    CHECK(pdf_doc_free(NULL) == PDF_ERR_INVALID_HANDLE);
}

static void test_handles_and_params(void)
{
    pdf_doc doc = NULL;
    pdf_page page = NULL;
    pdf_font font = NULL;
    pdf_annot annot = NULL;
    pdf_outline outline = NULL;
    pdf_rect rect = { 10, 10, 100, 40 };
    pdf_color c;
    double width = 0;

    c.kind = PDF_COLOR_RGB;
    c.c1 = 0.2;
    c.c2 = 0.4;
    c.c3 = 0.6;
    c.c4 = 0;
    c.alpha = 1.0;

    CHECK(pdf_doc_create(&doc) == PDF_OK);
    CHECK(pdf_page_add(doc, PDF_PAGE_LETTER, PDF_ORIENTATION_LANDSCAPE, &page)
          == PDF_OK);

    CHECK(pdf_font_get_std(doc, PDF_STD_FONT_COURIER_BOLD, &font) == PDF_OK);
    CHECK(pdf_font_text_width(font, "Hello", 12, &width) == PDF_OK);
    CHECK(width > 0);
    pdf_font_free(font);

    /* drawing calls must be accepted and must not crash */
    CHECK(pdf_gstate_save(page) == PDF_OK);
    CHECK(pdf_set_line_width(page, 1.5) == PDF_OK);
    CHECK(pdf_set_fill_color(page, &c) == PDF_OK);
    CHECK(pdf_set_stroke_color(page, &c) == PDF_OK);
    CHECK(pdf_page_draw_rect(page, &rect, PDF_PATH_FILL_STROKE) == PDF_OK);
    CHECK(pdf_gstate_restore(page) == PDF_OK);

    CHECK(pdf_annot_create(page, PDF_ANNOT_TEXT, &rect, "note", &annot)
          == PDF_OK);
    CHECK(pdf_annot_set_color(annot, &c) == PDF_OK);
    pdf_annot_free(annot);

    CHECK(pdf_outline_add(doc, NULL, "root", &page, &outline) == PDF_OK);
    pdf_outline_free(outline);

    /* invalid params */
    CHECK(pdf_page_add(doc, PDF_PAGE_A4, PDF_ORIENTATION_PORTRAIT, NULL)
          == PDF_ERR_INVALID_PARAM);
    CHECK(pdf_page_set_rotation(page, 45) == PDF_ERR_INVALID_PARAM);
    CHECK(pdf_page_set_rotation(page, 90) == PDF_OK);

    pdf_doc_free(doc);
}

static void test_word_wrap(void)
{
    pdf_doc doc = NULL;
    pdf_page page = NULL;
    pdf_font font = NULL;

    CHECK(pdf_doc_create(&doc) == PDF_OK);
    CHECK(pdf_page_add(doc, PDF_PAGE_A5, PDF_ORIENTATION_PORTRAIT, &page)
          == PDF_OK);
    CHECK(pdf_font_get_std(doc, PDF_STD_FONT_HELVETICA, &font) == PDF_OK);

    CHECK(pdf_page_draw_text_wrapped(page, font,
                                     "The quick brown fox jumps over "
                                     "the lazy dog again and again.",
                                     40, 700, 200, 300, 12, 2,
                                     PDF_ALIGN_LEFT) == PDF_OK);
    pdf_font_free(font);
    pdf_doc_free(doc);
}

/* --- streaming output ------------------------------------------------- */

struct stream_sink {
    unsigned char buf[4096];
    size_t len;
    int calls;
    pdf_status fail_on_call; /* 0 = never fail */
};

static pdf_status stream_sink_cb(const unsigned char *data, size_t len,
                                 void *userdata)
{
    struct stream_sink *s = (struct stream_sink *)userdata;

    s->calls++;
    if (s->fail_on_call && s->calls == s->fail_on_call)
        return PDF_ERR_IO; /* simulated consumer abort */
    if (s->len + len > sizeof(s->buf))
        return PDF_ERR_IO;
    memcpy(s->buf + s->len, data, len);
    s->len += len;
    return PDF_OK;
}

static void test_stream_output(void)
{
    pdf_doc doc = NULL;
    struct stream_sink sink;
    size_t mem_size = 0;

    memset(&sink, 0, sizeof(sink));
    CHECK(pdf_doc_create(&doc) == PDF_OK);
    CHECK(pdf_doc_save_to_memory(doc, NULL, &mem_size) == PDF_OK);
    CHECK(mem_size > 0);

    /* streaming produces the same bytes as save_to_memory, in chunks */
    CHECK(pdf_doc_save_to_stream(doc, stream_sink_cb, &sink) == PDF_OK);
    CHECK(sink.calls >= 1);
    CHECK(sink.len == mem_size);

    /* callback abort propagates */
    memset(&sink, 0, sizeof(sink));
    sink.fail_on_call = 1;
    CHECK(pdf_doc_save_to_stream(doc, stream_sink_cb, &sink) == PDF_ERR_IO);

    /* invalid params */
    CHECK(pdf_doc_save_to_stream(doc, NULL, &sink) == PDF_ERR_INVALID_PARAM);

    /* save_to_handle round trip through a real file */
    {
        void *h = test_open_write("test-stream-out.pdf");
        CHECK(h != NULL);
        if (h)
        {
            CHECK(pdf_doc_save_to_handle(doc, h) == PDF_OK);
            test_close(h);

            {
                FILE *f = fopen("test-stream-out.pdf", "rb");
                CHECK(f != NULL);
                if (f)
                {
                    unsigned char head[5] = {0};
                    CHECK(fread(head, 1, 5, f) == 5);
                    CHECK(memcmp(head, "%PDF-", 5) == 0);
                    fclose(f);
                    remove("test-stream-out.pdf");
                }
            }
        }
    }

    pdf_doc_free(doc);
}

/* --- streamed documents: mid-page abort & recovery -------------------- */

/* counting sink that can fail on the Nth call (1-based, 0 = never) */
struct abort_sink
{
    unsigned char last[16];
    size_t last_len;
    int calls;
    int fail_on_call;
    pdf_status injected; /* error to return when failing */
};

static pdf_status abort_sink_cb(const unsigned char *data, size_t len,
                                void *userdata)
{
    struct abort_sink *s = (struct abort_sink *)userdata;

    s->calls++;
    if (s->fail_on_call && s->calls == s->fail_on_call)
        return s->injected;
    if (len > sizeof(s->last))
        len = sizeof(s->last);
    memcpy(s->last, data, len);
    s->last_len = len;
    return PDF_OK;
}

static void test_streamed_abort(void)
{
    struct abort_sink sink;
    pdf_doc doc = NULL;
    pdf_page page = NULL;
    pdf_status rc;

    /* 1) callback failure during page N is reported and later work fails */
    memset(&sink, 0, sizeof(sink));
    sink.fail_on_call = 2; /* page 2 marker */
    sink.injected = PDF_ERR_IO;
    CHECK(pdf_doc_create_streamed(abort_sink_cb, &sink, &doc) == PDF_OK);
    CHECK(pdf_page_add(doc, PDF_PAGE_A4, PDF_ORIENTATION_PORTRAIT, &page)
          == PDF_OK); /* page 1 delivered fine */
    rc = pdf_page_add(doc, PDF_PAGE_A4, PDF_ORIENTATION_PORTRAIT, &page);
    CHECK(rc == PDF_ERR_IO); /* page 2 marker rejected mid-stream */
    CHECK(pdf_page_add(doc, PDF_PAGE_A4, PDF_ORIENTATION_PORTRAIT, &page)
          == PDF_ERR_IO); /* doc is broken: fast fail, no further deliveries */
    CHECK(pdf_font_get_std(doc, PDF_STD_FONT_HELVETICA, NULL)
          == PDF_ERR_IO); /* broken doc refuses new work with the real cause */
    CHECK(pdf_doc_finish(doc) == PDF_ERR_IO); /* no flush into broken sink */
    CHECK(sink.calls == 2); /* nothing pushed after the failure */
    pdf_doc_free(doc); /* teardown must not push either */
    CHECK(sink.calls == 2);

    /* 2) a caller-owned sink handle that hard-fails on write aborts
     *    mid-stream. The handle stays the caller's - pdflib must never
     *    close it - and the failure has to surface as a status code, not
     *    as a crash. A read-only handle gives a genuine I/O error on the
     *    very first page. */
    {
        void *ro;
        FILE *w = fopen("ro-probe.bin", "wb");
        CHECK(w != NULL);
        if (w)
            fclose(w);
        ro = test_open_read("ro-probe.bin");
        CHECK(ro != NULL);
        CHECK(pdf_doc_create_streamed_to_handle(ro, &doc) == PDF_OK);
        CHECK(pdf_page_add(doc, PDF_PAGE_A4, PDF_ORIENTATION_PORTRAIT, &page)
              == PDF_ERR_IO); /* write to a read-only handle fails */
        CHECK(pdf_page_add(doc, PDF_PAGE_A4, PDF_ORIENTATION_PORTRAIT, &page)
              == PDF_ERR_IO); /* broken: fast fail, no further deliveries */
        CHECK(pdf_doc_finish(doc) == PDF_ERR_IO);
        pdf_doc_free(doc);
        test_close(ro); /* still ours to close */
        remove("ro-probe.bin");
    }

    /* 3) healthy path still works after the abort tests (fresh doc) */
    memset(&sink, 0, sizeof(sink));
    CHECK(pdf_doc_create_streamed(abort_sink_cb, &sink, &doc) == PDF_OK);
    CHECK(pdf_page_add(doc, PDF_PAGE_A4, PDF_ORIENTATION_PORTRAIT, &page)
          == PDF_OK);
    CHECK(pdf_page_add(doc, PDF_PAGE_A5, PDF_ORIENTATION_PORTRAIT, &page)
          == PDF_OK);
    CHECK(pdf_doc_finish(doc) == PDF_OK);
    CHECK(memcmp(sink.last, "FINISHED", 8) == 0); /* finish marker seen */
    pdf_doc_free(doc);
}

int main(void)
{
    test_backend_selection();
    test_lifecycle();
    test_handles_and_params();
    test_word_wrap();
    test_stream_output();
    test_streamed_abort();

    if (failures)
    {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    printf("pdflib tests passed\n");
    return 0;
}
