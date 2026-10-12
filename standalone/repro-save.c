/* Save-path regression check: drive EVERY serializer over one document.
 *
 *   repro-save [podofo|libharu|dummy]
 *
 * History: this used to crash. pdf_doc_save_to_FILE() took a FILE* owned
 * by the caller, and pdflib.dll links the CRT statically, so the DLL's own
 * fwrite/fflush rejected that FILE* as an invalid parameter - which routes
 * to __fastfail(FAST_FAIL_INVALID_ARG) and kills the host process, with
 * nothing able to catch it (SEH and catch(...) both miss it). The two
 * FILE*-taking entry points are gone; the OS-handle ones used below are
 * what replaced them. Every call here must return PDF_OK and leave a
 * readable PDF behind.
 */
#include "pdflib.h"

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

static pdf_status sink(const unsigned char *data, size_t len, void *ud)
{
    FILE *f = (FILE *)ud;

    if (len && fwrite(data, 1, len, f) != len)
        return PDF_ERR_IO;
    return PDF_OK;
}

/* A non-zero exit is the failure signal; anything else prints the codes. */
static int failures = 0;

static void expect(const char *what, pdf_status got)
{
    printf("  %-16s rc=%d%s\n", what, got, got == PDF_OK ? "" : "   <-- FAIL");
    if (got != PDF_OK)
        failures++;
}

int main(int argc, char **argv)
{
    const char *who = argc > 1 ? argv[1] : "podofo";
    pdf_backend b;
    pdf_doc doc = NULL;
    pdf_page page = NULL;
    pdf_font font = NULL;
    void *h;
    FILE *f;
    double w, hh;
    unsigned char *buf;
    size_t size;

    setvbuf(stdout, NULL, _IONBF, 0);
    if (!strcmp(who, "libharu")) b = PDF_BACKEND_LIBHARU;
    else if (!strcmp(who, "dummy")) b = PDF_BACKEND_DUMMY;
    else b = PDF_BACKEND_PODOFO;

    printf("--- %s ---\n", who);
    printf("set_backend rc=%d\n", pdflib_set_backend(b));
    expect("doc_create", pdf_doc_create(&doc));
    expect("page_add", pdf_page_add(doc, PDF_PAGE_A4,
                                    PDF_ORIENTATION_PORTRAIT, &page));
    pdf_page_get_size(page, &w, &hh);
    expect("font_get_std", pdf_font_get_std(doc, PDF_STD_FONT_HELVETICA,
                                            &font));
    expect("draw_text", pdf_page_draw_text(page, font, "hello", 48, hh - 60, 16));

    /* 1) to a path (the DLL opens it itself) */
    expect("save_to_file", pdf_doc_save_to_file(doc, "repro-path.pdf"));

    /* 2) to a caller-owned OS handle - the replacement for the old FILE* */
    h = os_open_write("repro-handle.pdf");
    expect("save_to_handle", pdf_doc_save_to_handle(doc, h));
    os_close(h);

    /* 3) through a callback that runs in THIS module */
    f = fopen("repro-stream.pdf", "wb");
    expect("save_to_stream", pdf_doc_save_to_stream(doc, sink, f));
    fclose(f);

    /* 4) into memory, then written out here */
    size = 0;
    expect("save_to_memory", pdf_doc_save_to_memory(doc, NULL, &size));
    buf = (unsigned char *)malloc(size ? size : 1);
    expect("save_to_memory", pdf_doc_save_to_memory(doc, buf, &size));
    f = fopen("repro-mem.pdf", "wb");
    if (f)
    {
        fwrite(buf, 1, size, f);
        fclose(f);
    }
    free(buf);

    expect("doc_free", pdf_doc_free(doc));
    printf("%s: %d failure(s)\n", who, failures);
    return failures ? 1 : 0;
}
