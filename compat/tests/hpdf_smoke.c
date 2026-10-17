/*
 * hpdf_smoke.c - self-checking consumer of the libharu-compatible layer.
 *
 * Builds the same document through libharu and PoDoFo by selecting the
 * backend with HPDF_SetBackend(), exercises the libharu text object API,
 * paths, colour, outlines and the memory stream, and verifies the bytes that
 * came out. This is deliberately written as a libharu program: nothing here
 * includes pdflib headers.
 */
#include "hpdf.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures = 0;

#define CHECK(cond, what)                                                    \
    do {                                                                     \
        if (!(cond)) {                                                       \
            printf("  [FAIL] %s\n", what);                                   \
            ++g_failures;                                                    \
        } else {                                                             \
            printf("  [ok]   %s\n", what);                                   \
        }                                                                    \
    } while (0)

static long file_size(const char *path)
{
    FILE *f = fopen(path, "rb");
    long n;
    if (!f)
        return -1;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fclose(f);
    return n;
}

static int starts_with_pdf(const char *path)
{
    char buf[5];
    FILE *f = fopen(path, "rb");
    size_t n;
    if (!f)
        return 0;
    n = fread(buf, 1, 4, f);
    fclose(f);
    return n == 4 && memcmp(buf, "%PDF", 4) == 0;
}

static int make_document(int backend, const char *path)
{
    HPDF_Doc doc;
    HPDF_Page page;
    HPDF_Font font;
    HPDF_Outline root;
    HPDF_Destination dst;
    HPDF_RGBColor rgb;
    HPDF_STATUS st;
    HPDF_BYTE *stream = NULL;
    HPDF_UINT32 stream_size = 0;

    st = HPDF_SetBackend((HPDF_UINT)backend);
    printf("[backend %d: %s]\n", backend, HPDF_GetBackendName());
    CHECK(st == HPDF_OK, "HPDF_SetBackend");

    doc = HPDF_New(NULL, NULL);
    CHECK(doc != NULL, "HPDF_New");
    if (!doc)
        return 1;

    CHECK(HPDF_SetCompressionMode(doc, HPDF_COMP_ALL) == HPDF_OK,
          "HPDF_SetCompressionMode");
    CHECK(HPDF_SetInfoAttr(doc, HPDF_INFO_TITLE, "hpdf compat smoke") == HPDF_OK,
          "HPDF_SetInfoAttr(title)");
    CHECK(strcmp(HPDF_GetInfoAttr(doc, HPDF_INFO_TITLE),
                 "hpdf compat smoke") == 0,
          "HPDF_GetInfoAttr(title)");

    page = HPDF_AddPage(doc);
    CHECK(page != NULL, "HPDF_AddPage");
    if (!page)
        return 1;
    CHECK(HPDF_Page_SetSize(page, HPDF_PAGE_SIZE_A4,
                            HPDF_PAGE_PORTRAIT) == HPDF_OK,
          "HPDF_Page_SetSize(A4)");
    CHECK(HPDF_Page_GetWidth(page) > 595.0
          && HPDF_Page_GetWidth(page) < 596.0,
          "HPDF_Page_GetWidth(A4)");

    font = HPDF_GetFont(doc, "Helvetica", "WinAnsiEncoding");
    CHECK(font != NULL, "HPDF_GetFont(Helvetica)");
    if (!font)
        return 1;
    CHECK(HPDF_Font_GetAscent(font) > 0, "HPDF_Font_GetAscent");
    CHECK(HPDF_Font_GetDescent(font) <= 0, "HPDF_Font_GetDescent");

    CHECK(HPDF_Page_SetRGBFill(page, 0.1f, 0.2f, 0.6f) == HPDF_OK,
          "HPDF_Page_SetRGBFill");
    rgb = HPDF_Page_GetRGBFill(page);
    CHECK(rgb.r > 0.09f && rgb.r < 0.11f, "HPDF_Page_GetRGBFill");

    CHECK(HPDF_Page_BeginText(page) == HPDF_OK, "HPDF_Page_BeginText");
    CHECK(HPDF_Page_SetFontAndSize(page, font, 24) == HPDF_OK,
          "HPDF_Page_SetFontAndSize");
    CHECK(HPDF_Page_TextOut(page, 60, 760, "libharu-compatible pdflib") == HPDF_OK,
          "HPDF_Page_TextOut");
    CHECK(HPDF_Page_MoveTextPos(page, 0, -32) == HPDF_OK,
          "HPDF_Page_MoveTextPos");
    CHECK(HPDF_Page_ShowText(page, "same source, two engines") == HPDF_OK,
          "HPDF_Page_ShowText");
    CHECK(HPDF_Page_EndText(page) == HPDF_OK, "HPDF_Page_EndText");

    CHECK(HPDF_Page_SetRGBStroke(page, 0.8f, 0.1f, 0.1f) == HPDF_OK,
          "HPDF_Page_SetRGBStroke");
    CHECK(HPDF_Page_SetLineWidth(page, 2) == HPDF_OK, "HPDF_Page_SetLineWidth");
    CHECK(HPDF_Page_MoveTo(page, 60, 700) == HPDF_OK, "HPDF_Page_MoveTo");
    CHECK(HPDF_Page_LineTo(page, 500, 700) == HPDF_OK, "HPDF_Page_LineTo");
    CHECK(HPDF_Page_Stroke(page) == HPDF_OK, "HPDF_Page_Stroke");

    CHECK(HPDF_Page_SetRGBFill(page, 0.95f, 0.85f, 0.2f) == HPDF_OK,
          "HPDF_Page_SetRGBFill(rect)");
    CHECK(HPDF_Page_Rectangle(page, 60, 620, 200, 50) == HPDF_OK,
          "HPDF_Page_Rectangle");
    CHECK(HPDF_Page_FillStroke(page) == HPDF_OK, "HPDF_Page_FillStroke");

    /* TextRect is the libharu convenience that wraps text into a box. */
    CHECK(HPDF_Page_BeginText(page) == HPDF_OK, "HPDF_Page_BeginText(rect)");
    CHECK(HPDF_Page_SetFontAndSize(page, font, 11) == HPDF_OK,
          "HPDF_Page_SetFontAndSize(rect)");
    {
        HPDF_UINT used = 0;
        CHECK(HPDF_Page_TextRect(page, 60, 580, 500, 500,
                                 "The quick brown fox jumps over the lazy dog. "
                                 "This run is long enough to wrap onto a second "
                                 "line inside the box.",
                                 HPDF_TALIGN_LEFT, &used) == HPDF_OK,
              "HPDF_Page_TextRect");
        CHECK(used > 20, "HPDF_Page_TextRect reports consumed length");
    }
    CHECK(HPDF_Page_EndText(page) == HPDF_OK, "HPDF_Page_EndText(rect)");

    root = HPDF_CreateOutline(doc, NULL, "Chapter 1", NULL);
    CHECK(root != NULL, "HPDF_CreateOutline");
    if (root)
    {
        dst = HPDF_Page_CreateDestination(page);
        CHECK(dst != NULL, "HPDF_Page_CreateDestination");
        if (dst)
        {
            CHECK(HPDF_Destination_SetXYZ(dst, 0, HPDF_Page_GetHeight(page), 1)
                  == HPDF_OK, "HPDF_Destination_SetXYZ");
            CHECK(HPDF_Outline_SetDestination(root, dst) == HPDF_OK,
                  "HPDF_Outline_SetDestination");
            CHECK(HPDF_Outline_SetOpened(root, HPDF_TRUE) == HPDF_OK,
                  "HPDF_Outline_SetOpened");
        }
    }
    CHECK(HPDF_SetPageMode(doc, HPDF_PAGE_MODE_USE_OUTLINE) == HPDF_OK,
          "HPDF_SetPageMode(outline)");

    CHECK(HPDF_SaveToFile(doc, path) == HPDF_OK, "HPDF_SaveToFile");
    CHECK(HPDF_SaveToStream(doc) == HPDF_OK, "HPDF_SaveToStream");
    CHECK(HPDF_GetStreamSize(doc) > 0, "HPDF_GetStreamSize");
    stream_size = HPDF_GetStreamSize(doc);
    stream = (HPDF_BYTE *)malloc(stream_size ? stream_size : 1);
    if (stream)
    {
        HPDF_UINT32 got = stream_size;
        CHECK(HPDF_GetContents(doc, stream, &got) == HPDF_OK,
              "HPDF_GetContents");
        CHECK(got == stream_size && memcmp(stream, "%PDF", 4) == 0,
              "stream starts with %PDF");
        free(stream);
    }
    else
    {
        CHECK(0, "malloc stream");
    }

    HPDF_Free(doc);
    return 0;
}

int main(void)
{
    printf("=== libharu compatibility smoke ===\n");

    make_document(PDFLIB_HPDF_BACKEND_LIBHARU, "hpdf-smoke-libharu.pdf");
    CHECK(file_size("hpdf-smoke-libharu.pdf") > 500,
          "libharu output has content");
    CHECK(starts_with_pdf("hpdf-smoke-libharu.pdf"),
          "libharu output starts with %PDF");

    make_document(PDFLIB_HPDF_BACKEND_PODOFO, "hpdf-smoke-podofo.pdf");
    CHECK(file_size("hpdf-smoke-podofo.pdf") > 500,
          "podofo output has content");
    CHECK(starts_with_pdf("hpdf-smoke-podofo.pdf"),
          "podofo output starts with %PDF");

    if (g_failures)
    {
        printf("=== FAILED: %d check(s) ===\n", g_failures);
        return 1;
    }
    printf("=== PASSED ===\n");
    return 0;
}
