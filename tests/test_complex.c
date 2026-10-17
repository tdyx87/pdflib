/*
 * test_complex.c - complex-document automated tests for pdflib.
 *
 * Unlike test_pdflib.c (smoke level), every case here builds a full
 * feature-rich multi-page document and byte-verifies the produced PDF:
 * header, xref/EOF trailer, content-stream compression, page attributes
 * (size, rotation), resources (fonts, XObjects, ExtGState), link and
 * outline structures, the encryption dictionary and info fields.
 *
 * The suite runs against every backend registered at runtime and decides
 * via pdflib_has_backend() whether byte checks make sense: the dummy
 * backend never emits real PDF bytes, so structural checks run only for
 * real engines (libharu / PoDoFo).
 *
 * No third-party headers are needed: the suite links pdflib only and
 * therefore also builds in the core-only configuration.
 */
#include "pdflib/pdf_api.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#else
#  include <pthread.h>
#endif

#define CHECK(cond)                                                          \
    do                                                                       \
    {                                                                        \
        if (!(cond))                                                         \
        {                                                                    \
            fprintf(stderr, "  FAILED: %s at line %d\n", #cond, __LINE__);   \
            failures++;                                                      \
        }                                                                    \
    } while (0)

static int failures = 0;
static int skipped = 0;
static int real_engine = 0; /* 1 when the active backend emits real PDFs */
static const char *cur_backend = "dummy"; /* name of the active backend */

/* ------------------------------------------------------------------ */
/* Growable byte buffer + streamed sink                                 */
/* ------------------------------------------------------------------ */

struct membuf
{
    unsigned char *data;
    size_t len;
    size_t cap;
};

static void buf_init(struct membuf *b)
{
    b->cap = 1u << 16;
    b->len = 0;
    b->data = (unsigned char *)malloc(b->cap);
}

static void buf_free(struct membuf *b)
{
    free(b->data);
    b->data = NULL;
    b->len = b->cap = 0;
}

static pdf_status sink_write(const unsigned char *data, size_t len, void *ud)
{
    struct membuf *b = (struct membuf *)ud;
    if (b->len + len + 1 > b->cap)
    {
        size_t cap = b->cap * 2;
        unsigned char *p;
        while (cap < b->len + len + 1)
            cap *= 2;
        p = (unsigned char *)realloc(b->data, cap);
        if (!p)
            return PDF_ERR_OUT_OF_MEMORY;
        b->data = p;
        b->cap = cap;
    }
    memcpy(b->data + b->len, data, len);
    b->len += len;
    return PDF_OK;
}

/* Fetch the whole document via the two-call save_to_memory protocol. */
static int fetch_bytes(pdf_doc doc, struct membuf *b)
{
    size_t size = 0;
    if (pdf_doc_save_to_memory(doc, NULL, &size) != PDF_OK || size == 0)
        return 0;
    if (b->cap < size + 1)
    {
        unsigned char *p = (unsigned char *)realloc(b->data, size + 1);
        if (!p)
            return 0;
        b->data = p;
        b->cap = size + 1;
    }
    if (pdf_doc_save_to_memory(doc, b->data, &size) != PDF_OK)
        return 0;
    b->len = size;
    b->data[size] = 0;
    return 1;
}

/* ------------------------------------------------------------------ */
/* Tolerant byte-level PDF matchers                                     */
/*                                                                      */
/* Engines differ in whitespace ("[/Type /Page]" vs "/Type /Page"), so  */
/* the matchers skip whitespace between tokens and verify token         */
/* boundaries (the value /Page must not match /Pages or /PageMode).     */
/* ------------------------------------------------------------------ */

static const unsigned char *mem_find(const unsigned char *hay, size_t n,
                                     const char *needle)
{
    size_t m = strlen(needle);
    if (m == 0 || n < m)
        return NULL;
    for (size_t i = 0; i + m <= n; i++)
        if (hay[i] == (unsigned char)needle[0] &&
            memcmp(hay + i, needle, m) == 0)
            return hay + i;
    return NULL;
}

static const unsigned char *mem_rfind(const unsigned char *hay, size_t n,
                                      const char *needle)
{
    size_t m = strlen(needle);
    if (m == 0 || n < m)
        return NULL;
    for (size_t i = n - m + 1; i-- > 0;)
        if (hay[i] == (unsigned char)needle[0] &&
            memcmp(hay + i, needle, m) == 0)
            return hay + i;
    return NULL;
}

static int is_ws(unsigned char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

static int is_token_end(unsigned char c)
{
    return isalnum(c) || c == '_';
}

/* Count "<key> <ws>* <value>" occurrences with token boundaries. */
static size_t count_pair(const struct membuf *b, const char *key,
                         const char *value)
{
    size_t count = 0;
    size_t klen = strlen(key), vlen = strlen(value);
    const unsigned char *p = b->data;
    const unsigned char *end = b->data + b->len;

    while (p < end && (p = mem_find(p, (size_t)(end - p), key)) != NULL)
    {
        const unsigned char *q = p + klen;
        while (q < end && is_ws(*q))
            q++;
        if ((size_t)(end - q) >= vlen && memcmp(q, value, vlen) == 0 &&
            (q + vlen == end || !is_token_end(q[vlen])))
        {
            count++;
            p = q + vlen;
        }
        else
        {
            p = p + 1;
        }
    }
    return count;
}

static int has_pair(const struct membuf *b, const char *key, const char *value)
{
    return count_pair(b, key, value) > 0;
}

/* Count occurrences of a bare key (used when the value shape varies). */
static int count_key(const struct membuf *b, const char *key)
{
    size_t count = 0;
    const unsigned char *p = b->data;
    const unsigned char *end = b->data + b->len;
    while (p < end && (p = mem_find(p, (size_t)(end - p), key)) != NULL)
    {
        count++;
        p++;
    }
    return (int)count;
}

static int has_key(const struct membuf *b, const char *key)
{
    return mem_find(b->data, b->len, key) != NULL;
}

/* Parse "[a b w h]" of one /MediaBox entry; returns 1 with w/h set. */
static int parse_mediabox_at(const unsigned char *p, size_t avail,
                             double *w, double *h)
{
    char tmp[128];
    size_t i = 0;
    char *endp = NULL;
    double x1, y1, wv, hv;

    while (avail > 0 && is_ws(*p))
    {
        p++;
        avail--;
    }
    if (avail == 0 || *p != '[')
        return 0;
    p++;
    avail--;
    while (avail > 0 && *p != ']' && i + 1 < sizeof(tmp))
    {
        tmp[i++] = (char)*p++;
        avail--;
    }
    if (i == 0 || i >= sizeof(tmp))
        return 0;
    tmp[i] = 0;
    x1 = strtod(tmp, &endp);
    if (endp == tmp)
        return 0;
    y1 = strtod(endp, &endp);
    wv = strtod(endp, &endp);
    hv = strtod(endp, &endp);
    if (endp == tmp)
        return 0;
    (void)x1;
    (void)y1;
    *w = wv;
    *h = hv;
    return wv > 0.0 && hv > 0.0;
}

/* Number of /MediaBox entries whose size is within 1.5pt of (w, h). */
static int count_mediabox(const struct membuf *b, double w, double h)
{
    int count = 0;
    const unsigned char *p = b->data;
    const unsigned char *end = b->data + b->len;

    while (p < end && (p = mem_find(p, (size_t)(end - p), "/MediaBox")) != NULL)
    {
        double mw, mh;
        if (parse_mediabox_at(p + 9, (size_t)(end - (p + 9)), &mw, &mh) &&
            mw - w < 1.5 && w - mw < 1.5 && mh - h < 1.5 && h - mh < 1.5)
            count++;
        p += 9;
    }
    return count;
}

/*
 * The last /startxref offset must point at either a classic "xref"
 * keyword or a cross-reference-stream object header ("N 0 obj").
 */
static int check_xref_tail(const struct membuf *b)
{
    const unsigned char *p = mem_rfind(b->data, b->len, "startxref");
    long long off;
    char *endp;
    if (!p)
        return 0;
    p += 9;
    while (p < b->data + b->len && is_ws(*p))
        p++;
    if (p >= b->data + b->len)
        return 0;
    off = strtoll((const char *)p, &endp, 10);
    if (endp == (const char *)p || off <= 0 || (size_t)off >= b->len)
        return 0;
    if (b->len - (size_t)off >= 4 &&
        memcmp(b->data + off, "xref", 4) == 0)
        return 1;
    return b->len - (size_t)off >= 4 && isdigit(b->data[off]) != 0;
}

static int check_eof(const struct membuf *b)
{
    size_t n = b->len;
    while (n > 0 && is_ws(b->data[n - 1]))
        n--;
    return n >= 5 && memcmp(b->data + n - 5, "%%EOF", 5) == 0;
}

/* Structural checks that hold for every real backend. */
static void check_pdf_skeleton(const struct membuf *b, size_t want_pages)
{
    CHECK(b->len > 9 && memcmp(b->data, "%PDF-1.", 7) == 0);
    CHECK(check_eof(b));
    CHECK(has_key(b, "startxref"));
    CHECK(check_xref_tail(b));
    CHECK(has_key(b, "/Root"));
    CHECK(has_pair(b, "/Type", "/Catalog"));
    CHECK(has_pair(b, "/Type", "/Pages"));
    CHECK(count_pair(b, "/Type", "/Page") == want_pages);
}

/* ------------------------------------------------------------------ */
/* Shared content builder: a genuinely rich page                        */
/*                                                                      */
/* Real engines execute every call. The dummy content-validation        */
/* backend rejects some optional steps (it has no image decoder, no     */
/* opacity, no blend modes), so those are treated as "best effort":    */
/* PDF_ERR_BACKEND_UNSUPPORTED keeps the page build going, any other    */
/* error still fails the test.                                          */
/* ------------------------------------------------------------------ */

static int build_rich_page(pdf_doc doc, int index, double custom_w,
                           double custom_h)
{
#define FAIL_AT(what)                                                      \
    do                                                                     \
    {                                                                      \
        fprintf(stderr, "  rich_page %d FAILED at %s\n", index, what);     \
        return 1;                                                          \
    } while (0)
    pdf_page page;
    pdf_font helv, times, courier;
    pdf_color fill, stroke;
    pdf_rect r;
    pdf_point poly[4];
    double dash[2];
    double w, h;

    fill.kind = PDF_COLOR_RGB;
    fill.c1 = 0.9;
    fill.c2 = 0.3;
    fill.c3 = 0.1;
    fill.c4 = 0;
    fill.alpha = 1.0;
    stroke = fill;
    stroke.c1 = 0.1;
    stroke.c2 = 0.2;
    stroke.c3 = 0.6;

    if (index == 4)
    {
        pdf_status rc = pdf_page_add_custom(doc, custom_w, custom_h, &page);
        if (rc == PDF_ERR_BACKEND_UNSUPPORTED)
            rc = pdf_page_add(doc, PDF_PAGE_A4, PDF_ORIENTATION_PORTRAIT,
                              &page);
        if (rc != PDF_OK)
            FAIL_AT("page_add_custom");
    }
    else if (pdf_page_add(doc, index % 2 ? PDF_PAGE_A5 : PDF_PAGE_A4,
                          PDF_ORIENTATION_PORTRAIT, &page) != PDF_OK)
        FAIL_AT("page_add");

    w = 595.0; /* fallback for marker backends without geometry */
    h = 842.0;
    {
        double gw = 0.0, gh = 0.0;
        pdf_status rc = pdf_page_get_size(page, &gw, &gh);
        if (rc == PDF_OK && gw > 0.0 && gh > 0.0)
        {
            w = gw;
            h = gh;
        }
        else if (rc != PDF_OK && rc != PDF_ERR_BACKEND_UNSUPPORTED)
            FAIL_AT("get_size");
    }

/* Best-effort helper: UNSUPPORTED never fails the page build. */
#define TRY(call)                                                          \
    do                                                                     \
    {                                                                      \
        pdf_status _rc = (call);                                           \
        if (_rc != PDF_OK && _rc != PDF_ERR_BACKEND_UNSUPPORTED)           \
        {                                                                  \
            fprintf(stderr, "  TRY failed: %s -> %d\n", #call,             \
                    (int)_rc);                                             \
            return 1;                                                      \
        }                                                                  \
    } while (0)

    if (pdf_font_get_std(doc, PDF_STD_FONT_HELVETICA, &helv) != PDF_OK ||
        pdf_font_get_std(doc, PDF_STD_FONT_TIMES_BOLD, &times) != PDF_OK ||
        pdf_font_get_std(doc, PDF_STD_FONT_COURIER, &courier) != PDF_OK)
        FAIL_AT("font_get_std");

    if (pdf_page_draw_text(page, helv, "complex automated suite", 40, h - 50,
                           18) != PDF_OK)
        FAIL_AT("draw_text");
    TRY(pdf_page_draw_text_aligned(page, times, "centered caption", 40,
                                   h - 80, 12, w - 80, PDF_ALIGN_CENTER));
    TRY(pdf_page_draw_text_wrapped(
        page, courier,
        "wrapped body text that must flow across "
        "several lines inside the box",
        40, h - 110, w - 80, 60, 10, 2, PDF_ALIGN_LEFT));

    /* filled + stroked rect with opacity and blend mode */
    r.x = 40;
    r.y = h - 220;
    r.w = 120;
    r.h = 60;
    if (pdf_gstate_save(page) != PDF_OK)
        FAIL_AT("gstate_save");
    TRY(pdf_set_fill_color(page, &fill));
    TRY(pdf_set_stroke_color(page, &stroke));
    TRY(pdf_set_opacity(page, 0.5, 0.8));
    TRY(pdf_set_blend_mode(page, PDF_BLEND_MULTIPLY));
    TRY(pdf_set_line_width(page, 2.0));
    TRY(pdf_set_line_cap(page, PDF_LINE_CAP_ROUND));
    TRY(pdf_set_line_join(page, PDF_LINE_JOIN_BEVEL));
    TRY(pdf_page_draw_rect(page, &r, PDF_PATH_FILL_STROKE));
    if (pdf_gstate_restore(page) != PDF_OK)
        FAIL_AT("gstate_restore");

    /* dashed polyline */
    dash[0] = 4;
    dash[1] = 2;
    TRY(pdf_set_line_dash(page, dash, 2, 0));
    if (pdf_path_move_to(page, 40, h - 260) != PDF_OK ||
        pdf_path_line_to(page, 200, h - 300) != PDF_OK ||
        pdf_path_line_to(page, 360, h - 260) != PDF_OK ||
        pdf_path_paint(page, PDF_PATH_STROKE) != PDF_OK)
        FAIL_AT("polyline");
    TRY(pdf_set_line_dash(page, NULL, 0, 0));

    /* bezier, circle, arc, polygon */
    if (pdf_path_move_to(page, 40, h - 340) != PDF_OK ||
        pdf_path_curve_to(page, 120, h - 280, 240, h - 400, 320, h - 340) !=
            PDF_OK ||
        pdf_path_paint(page, PDF_PATH_STROKE) != PDF_OK)
        FAIL_AT("bezier");
    {
        pdf_status rc = pdf_path_circle(page, 120, 120, 40);
        if (rc == PDF_OK)
        {
            if (pdf_path_paint(page, PDF_PATH_FILL) != PDF_OK)
                FAIL_AT("circle paint");
        }
        else if (rc != PDF_ERR_BACKEND_UNSUPPORTED)
            FAIL_AT("circle");
    }
    {
        pdf_status rc = pdf_path_arc(page, 260, 120, 40, 0, 3.14159, 0);
        if (rc == PDF_OK)
        {
            if (pdf_path_paint(page, PDF_PATH_STROKE) != PDF_OK)
                FAIL_AT("arc paint");
        }
        else if (rc != PDF_ERR_BACKEND_UNSUPPORTED)
            FAIL_AT("arc");
    }
    poly[0].x = 320;
    poly[0].y = 100;
    poly[1].x = 380;
    poly[1].y = 160;
    poly[2].x = 440;
    poly[2].y = 100;
    poly[3].x = 380;
    poly[3].y = 60;
    {
        pdf_status rc = pdf_path_polygon(page, poly, 4);
        if (rc == PDF_OK)
        {
            if (pdf_path_paint(page, PDF_PATH_FILL_EVENODD) != PDF_OK)
                FAIL_AT("polygon paint");
        }
        else if (rc != PDF_ERR_BACKEND_UNSUPPORTED)
            FAIL_AT("polygon");
    }

    /* transform + invisible-mode text (real accessibility probe) */
    if (pdf_gstate_save(page) != PDF_OK)
        return 1;
    TRY(pdf_set_transform(page, 1.0, 0.0, 0.0, 1.0, 30.0, 40.0));
    TRY(pdf_set_text_render_mode(page, PDF_TEXT_RENDER_INVISIBLE));
    TRY(pdf_page_draw_text(page, helv, "hidden probe text", 0, 0, 10));
    if (pdf_gstate_restore(page) != PDF_OK)
        FAIL_AT("gstate_restore2");

    pdf_font_free(helv);
    pdf_font_free(times);
    pdf_font_free(courier);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Test 1: multi-page rich document (memory mode)                       */
/* ------------------------------------------------------------------ */

static void test_complex_basic(void)
{
    pdf_doc doc = NULL;
    struct membuf b;
    char info[128];
    size_t count = 0;

    buf_init(&b);
    CHECK(pdf_doc_create(&doc) == PDF_OK);
    CHECK(pdf_doc_set_info(doc, PDF_INFO_TITLE, "pdflib complex suite") ==
          PDF_OK);
    CHECK(pdf_doc_set_info(doc, PDF_INFO_AUTHOR, "test_complex") == PDF_OK);
    {
        pdf_status rc = pdf_doc_set_compression(doc, PDF_COMPRESS_ALL);
        CHECK(rc == PDF_OK || rc == PDF_ERR_BACKEND_UNSUPPORTED);
    }

    for (int i = 0; i < 5; i++)
        CHECK(build_rich_page(doc, i, 300, 200) == 0);

    CHECK(pdf_doc_page_count(doc, &count) == PDF_OK && count == 5);
    CHECK(fetch_bytes(doc, &b));
    CHECK(b.len > (size_t)(real_engine ? 2000 : 0));

    if (real_engine)
    {
        check_pdf_skeleton(&b, 5);
        CHECK(count_pair(&b, "/Type", "/Font") >= 3);
        /* 2x A4 portrait + 2x A5 portrait + 1x 300x200 custom */
        CHECK(count_mediabox(&b, 595, 842) >= 2);
        CHECK(count_mediabox(&b, 420, 595) >= 2);
        CHECK(count_mediabox(&b, 300, 200) == 1);
        CHECK(has_key(&b, "/FlateDecode"));
        CHECK(has_key(&b, "/ExtGState"));
        CHECK(has_key(&b, "/Producer"));
        if (strcmp(cur_backend, "libharu") == 0)
            printf("  NOTE: libharu writes no trailer /ID (upstream "
                   "limitation, no trailer API)\n");
        else
            CHECK(has_key(&b, "/ID"));
    }
    else
    {
        CHECK(b.len > 0);
    }

    {
        size_t ilen = sizeof(info);
        CHECK(pdf_doc_get_info(doc, PDF_INFO_TITLE, info, &ilen) == PDF_OK);
        CHECK(strcmp(info, "pdflib complex suite") == 0);
    }

    pdf_doc_free(doc);
    buf_free(&b);
}

/* ------------------------------------------------------------------ */
/* Test 2: page rotation + landscape geometry + page mode               */
/* ------------------------------------------------------------------ */

static void test_rotation_and_page_mode(void)
{
    pdf_doc doc = NULL;
    pdf_page page = NULL;
    struct membuf b;

    buf_init(&b);
    CHECK(pdf_doc_create(&doc) == PDF_OK);
    CHECK(pdf_page_add(doc, PDF_PAGE_A4, PDF_ORIENTATION_LANDSCAPE,
                       &page) == PDF_OK);
    CHECK(pdf_page_set_rotation(page, 90) == PDF_OK);
    CHECK(pdf_doc_set_page_mode_use_outlines(doc, 1) == PDF_OK);
    CHECK(fetch_bytes(doc, &b));
    CHECK(b.len > (size_t)(real_engine ? 100 : 0));

    if (real_engine)
    {
        check_pdf_skeleton(&b, 1);
        CHECK(has_pair(&b, "/Rotate", "90"));
        CHECK(has_pair(&b, "/PageMode", "/UseOutlines"));
        CHECK(count_mediabox(&b, 842, 595) == 1); /* A4 landscape */
    }
    pdf_doc_free(doc);
    buf_free(&b);
}

/* ------------------------------------------------------------------ */
/* Test 3: link annotations (goto + URI), text note, outline tree       */
/* ------------------------------------------------------------------ */

static void test_links_and_outlines(void)
{
    pdf_doc doc = NULL;
    pdf_page p1 = NULL, p2 = NULL;
    pdf_font helv = NULL;
    pdf_annot link = NULL, note = NULL;
    pdf_outline top = NULL, child = NULL;
    pdf_rect r;
    struct membuf b;

    if (!real_engine)
    {
        printf("  SKIP: annot/outline byte checks (marker backend)\n");
        skipped++;
        return;
    }

    buf_init(&b);
    r.x = 40;
    r.y = 700;
    r.w = 200;
    r.h = 30;

    CHECK(pdf_doc_create(&doc) == PDF_OK);
    CHECK(pdf_page_add(doc, PDF_PAGE_A4, PDF_ORIENTATION_PORTRAIT, &p1) ==
          PDF_OK);
    CHECK(pdf_page_add(doc, PDF_PAGE_A4, PDF_ORIENTATION_PORTRAIT, &p2) ==
          PDF_OK);
    CHECK(pdf_font_get_std(doc, PDF_STD_FONT_HELVETICA, &helv) == PDF_OK);
    CHECK(pdf_page_draw_text(p1, helv, "go to page two", 40, 760, 14) ==
          PDF_OK);

    /* internal goto link */
    CHECK(pdf_annot_create(p1, PDF_ANNOT_LINK_GOTO, &r, NULL, &link) ==
          PDF_OK);
    CHECK(pdf_annot_link_to_page(link, &p2, NULL) == PDF_OK);
    /* external uri link */
    r.y = 600;
    CHECK(pdf_annot_create(p1, PDF_ANNOT_LINK_URI, &r, NULL, &link) == PDF_OK);
    CHECK(pdf_annot_link_to_page(link, NULL, "https://example.com/doc") ==
          PDF_OK);
    /* text note with icon + open flag; early-free the handle afterwards */
    r.y = 500;
    CHECK(pdf_annot_create(p1, PDF_ANNOT_TEXT, &r, "note on page one",
                           &note) == PDF_OK);
    CHECK(pdf_annot_set_icon(note, PDF_ANNOT_ICON_NOTE) == PDF_OK);
    CHECK(pdf_annot_set_open(note, 1) == PDF_OK);
    CHECK(pdf_annot_free(note) == PDF_OK);

    /* outline tree: one open parent with a child */
    CHECK(pdf_outline_add(doc, NULL, "Top level", &p2, &top) == PDF_OK);
    CHECK(pdf_outline_add(doc, top, "Child item", &p1, &child) == PDF_OK);
    CHECK(pdf_outline_set_open(top, 1) == PDF_OK);
    CHECK(pdf_outline_free(child) == PDF_OK);
    /* title-only child (dest_page = NULL): regression for the PoDoFo
     * backend which used to dereference the NULL page slot here */
    child = NULL;
    CHECK(pdf_outline_add(doc, top, "Child without target", NULL, &child)
          == PDF_OK);
    CHECK(pdf_outline_free(child) == PDF_OK);
    CHECK(pdf_outline_free(top) == PDF_OK);
    pdf_font_free(helv);

    CHECK(fetch_bytes(doc, &b));
    check_pdf_skeleton(&b, 2);
    CHECK(has_pair(&b, "/Type", "/Annot"));
    CHECK(has_key(&b, "/URI"));                       /* uri link action */
    /* Internal links: either a /GoTo action (libharu) or a /Dest array
     * (PoDoFo) -- both are valid PDF for an internal jump. */
    CHECK(has_pair(&b, "/S", "/GoTo") || has_key(&b, "/Dest"));
    CHECK(has_key(&b, "/Dests") || has_key(&b, "/Dest"));
    CHECK(has_pair(&b, "/Type", "/Outlines"));
    /* /Type /Outline is optional in PDF 1.x (implied by /Title in the
     * outline dict); PoDoFo omits it. Count raw /Title keys instead. */
    CHECK(count_pair(&b, "/Type", "/Outline") >= 2 ||
          count_key(&b, "/Title") >= 3);
    CHECK(has_key(&b, "/First") && has_key(&b, "/Last"));
    CHECK(has_key(&b, "/Note"));                      /* text annot icon */

    pdf_doc_free(doc);
    buf_free(&b);
}

/* ------------------------------------------------------------------ */
/* Test 4: streamed document with an in-memory image                    */
/* ------------------------------------------------------------------ */

static void test_streamed_with_images(void)
{
    /* A real 8x8 grayscale PNG (generated offline, valid CRCs) so the
     * engines' true decoders (libpng) accept it. */
    static const unsigned char png[] = {
        0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00, 0x00,
        0x0D, 0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00,
        0x00, 0x08, 0x08, 0x00, 0x00, 0x00, 0x00, 0xE1, 0x64, 0xE1, 0x57,
        0x00, 0x00, 0x00, 0x14, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9C, 0x63,
        0x60, 0x50, 0x70, 0x48, 0x68, 0x58, 0x70, 0xE0, 0x01, 0x03, 0x65,
        0x0C, 0x00, 0xC6, 0x75, 0x1C, 0x01, 0x5A, 0x64, 0xB6, 0xFA, 0x00,
        0x00, 0x00, 0x00, 0x49, 0x45, 0x4E, 0x44, 0xAE, 0x42, 0x60, 0x82
    };
    pdf_doc doc = NULL;
    pdf_page page = NULL;
    pdf_font helv = NULL;
    pdf_image img = NULL;
    pdf_rect dest;
    struct membuf b;

    buf_init(&b);
    if (pdf_doc_create_streamed(sink_write, &b, &doc) != PDF_OK)
    {
        /* Both real engines stream; only the marker backend has no writer.
         * apitest and the showcase assert this hard - this file just skips
         * so it can also run against the dummy backend. */
        printf("  SKIP: no streamed writer on this backend\n");
        skipped++;
        return;
    }
    CHECK(pdf_page_add(doc, PDF_PAGE_A4, PDF_ORIENTATION_PORTRAIT, &page) ==
          PDF_OK);
    CHECK(pdf_font_get_std(doc, PDF_STD_FONT_HELVETICA, &helv) == PDF_OK);
    CHECK(pdf_page_draw_text(page, helv, "streamed page with image", 60, 760,
                             14) == PDF_OK);

    {
        pdf_status rc = pdf_image_load_memory(doc, png, sizeof(png), &img);
        CHECK(rc == PDF_OK || rc == PDF_ERR_BACKEND_UNSUPPORTED);
        if (rc == PDF_OK)
        {
            size_t wpx = 0, hpx = 0;
            CHECK(pdf_image_get_size(img, &wpx, &hpx) == PDF_OK);
            dest.x = 60;
            dest.y = 600;
            dest.w = 120;
            dest.h = 80;
            CHECK(pdf_page_draw_image(page, img, &dest) == PDF_OK);
            CHECK(pdf_image_free(img) == PDF_OK);
        }
    }
    pdf_font_free(helv);

    CHECK(pdf_doc_finish(doc) == PDF_OK);
    CHECK(pdf_doc_finish(doc) == PDF_OK); /* idempotent */
    CHECK(b.len > (size_t)(real_engine ? 500 : 0));

    if (real_engine)
    {
        check_pdf_skeleton(&b, 1);
        CHECK(has_key(&b, "/XObject"));
        CHECK(has_pair(&b, "/Subtype", "/Image"));
        CHECK(has_key(&b, "/DCTDecode") || has_key(&b, "/FlateDecode"));
    }
    pdf_doc_free(doc);
    buf_free(&b);
}

/* ------------------------------------------------------------------ */
/* Test 5: encrypted output                                             */
/* ------------------------------------------------------------------ */

static void test_encrypted_output(void)
{
    pdf_doc doc = NULL;
    pdf_page page = NULL;
    struct membuf b;
    pdf_status rc;

    if (!real_engine)
    {
        printf("  SKIP: encryption byte checks (marker backend)\n");
        skipped++;
        return;
    }

    buf_init(&b);
    CHECK(pdf_doc_create(&doc) == PDF_OK);
    CHECK(pdf_page_add(doc, PDF_PAGE_A4, PDF_ORIENTATION_PORTRAIT, &page) ==
          PDF_OK);

    rc = pdf_doc_set_encryption(doc, PDF_ENCRYPT_RC4_128, "owner-secret",
                                "user-secret", PDF_PERM_PRINT);
    if (rc == PDF_ERR_BACKEND_UNSUPPORTED)
    {
        printf("  SKIP: backend has no encryption support\n");
        skipped++;
        pdf_doc_free(doc);
        buf_free(&b);
        return;
    }
    CHECK(rc == PDF_OK);

    rc = fetch_bytes(doc, &b);
    CHECK(rc);
    check_pdf_skeleton(&b, 1);
    CHECK(has_key(&b, "/Encrypt"));
    CHECK(has_pair(&b, "/Filter", "/Standard"));
    CHECK(has_key(&b, "/O") && has_key(&b, "/U"));

    pdf_doc_free(doc);
    buf_free(&b);
}

/* ------------------------------------------------------------------ */
/* Test 6: concurrent complex documents on one real engine              */
/* ------------------------------------------------------------------ */

struct thread_ctx
{
    unsigned seed;
    int failed;
};

static unsigned char rnd_byte(struct thread_ctx *c)
{
    c->seed = c->seed * 1103515245u + 12345u;
    return (unsigned char)(c->seed >> 16);
}

#if defined(_WIN32)
static unsigned __stdcall
#else
static void *
#endif
complex_worker(void *arg)
{
    struct thread_ctx *c = (struct thread_ctx *)arg;
    pdf_doc doc = NULL;
    struct membuf b;
    int i;

    buf_init(&b);
    if (pdf_doc_create(&doc) != PDF_OK)
    {
        c->failed++;
        buf_free(&b);
#if defined(_WIN32)
        _endthreadex(0);
        return 0;
#else
        return NULL;
#endif
    }

    for (i = 0; i < 8; i++)
    {
        pdf_page page = NULL;
        pdf_font font = NULL;
        pdf_color col;
        pdf_rect r;
        char title[32];
        double w, h;

        if (pdf_page_add(doc, i % 2 ? PDF_PAGE_LETTER : PDF_PAGE_A4,
                         i % 3 ? PDF_ORIENTATION_PORTRAIT
                               : PDF_ORIENTATION_LANDSCAPE,
                         &page) != PDF_OK ||
            pdf_page_get_size(page, &w, &h) != PDF_OK)
        {
            c->failed++;
            continue;
        }

        {
            pdf_std_font pick = (pdf_std_font)(rnd_byte(c) % 3 == 0
                                                   ? PDF_STD_FONT_TIMES_BOLD
                                                   : PDF_STD_FONT_HELVETICA);
            if (pdf_font_get_std(doc, pick, &font) != PDF_OK)
            {
                c->failed++;
                continue;
            }
            snprintf(title, sizeof(title), "worker doc page %d", i);
            if (pdf_page_draw_text(page, font, title, 40, h - 40, 12) !=
                PDF_OK)
                c->failed++;
            pdf_font_free(font);
        }

        col.kind = PDF_COLOR_RGB;
        col.c1 = rnd_byte(c) / 255.0;
        col.c2 = rnd_byte(c) / 255.0;
        col.c3 = rnd_byte(c) / 255.0;
        col.c4 = 0;
        col.alpha = 1.0;
        r.x = 40 + (rnd_byte(c) % 100);
        r.y = 40 + (rnd_byte(c) % 100);
        r.w = 120;
        r.h = 60;
        if (pdf_set_fill_color(page, &col) != PDF_OK ||
            pdf_page_draw_rect(page, &r, PDF_PATH_FILL) != PDF_OK ||
            pdf_path_circle(page, 200, 200, 30 + rnd_byte(c) % 50) != PDF_OK ||
            pdf_path_paint(page, PDF_PATH_STROKE) != PDF_OK)
            c->failed++;

        snprintf(title, sizeof(title), "doc-%u-page%d",
                 (unsigned)(size_t)c, i);
        pdf_doc_set_info(doc, PDF_INFO_TITLE, title);
    }

    if (fetch_bytes(doc, &b))
    {
        if (real_engine)
        {
            if (b.len < 1000 || memcmp(b.data, "%PDF-", 5) != 0 ||
                count_pair(&b, "/Type", "/Page") != 8)
                c->failed++;
        }
        else if (b.len == 0)
        {
            c->failed++;
        }
    }
    else
    {
        c->failed++;
    }

    if (pdf_doc_free(doc) != PDF_OK)
        c->failed++;
    buf_free(&b);
#if defined(_WIN32)
    _endthreadex(0);
    return 0;
#else
    return NULL;
#endif
}

static void run_complex_concurrency(void)
{
    pdf_backend bks[2];
    int nbk = 0;

    if (pdflib_has_backend(PDF_BACKEND_PODOFO))
        bks[nbk++] = PDF_BACKEND_PODOFO;
    if (pdflib_has_backend(PDF_BACKEND_LIBHARU))
        bks[nbk++] = PDF_BACKEND_LIBHARU;
    if (nbk == 0)
    {
        printf("  SKIP: concurrency (no real engine registered)\n");
        skipped++;
        return;
    }

    for (int bk = 0; bk < nbk; bk++)
    {
        struct thread_ctx ctx[8];
#if defined(_WIN32)
        HANDLE th[8];
#else
        pthread_t th[8];
#endif
        int t;

        /* Selector set once; workers inherit it (see the threading
         * contract in pdf_api.h). */
        if (pdflib_set_backend(bks[bk]) != PDF_OK)
        {
            CHECK(0);
            continue;
        }
        printf("  concurrency on backend %d (8 threads x 8 pages)\n",
               (int)bks[bk]);

        for (t = 0; t < 8; t++)
        {
            ctx[t].seed = 0x9E3779B9u * (unsigned)(t + 1) + 7u;
            ctx[t].failed = 0;
        }
        for (t = 0; t < 8; t++)
        {
#if defined(_WIN32)
            unsigned tid;
            th[t] = (HANDLE)_beginthreadex(NULL, 0, complex_worker, &ctx[t],
                                           0, &tid);
#else
            pthread_create(&th[t], NULL, complex_worker, &ctx[t]);
#endif
        }
        for (t = 0; t < 8; t++)
        {
#if defined(_WIN32)
            WaitForSingleObject(th[t], INFINITE);
            CloseHandle(th[t]);
#else
            pthread_join(th[t], NULL);
#endif
        }
        for (t = 0; t < 8; t++)
            CHECK(ctx[t].failed == 0);
    }
}

/* ------------------------------------------------------------------ */
/* Driver                                                               */
/* ------------------------------------------------------------------ */

typedef void (*test_fn)(void);

static void run_all(void)
{
    /* dummy always exists: exercise the core plumbing in marker mode */
    printf("-- backend: dummy --\n");
    real_engine = 0;
    cur_backend = "dummy";
    if (pdflib_set_backend(PDF_BACKEND_DUMMY) != PDF_OK)
    {
        CHECK(0);
        return;
    }
    test_complex_basic();
    test_rotation_and_page_mode();
    test_streamed_with_images();
    test_links_and_outlines();
    test_encrypted_output();
    run_complex_concurrency();
}

int main(int argc, char **argv)
{
    /* Optional argv[1] filter: run only one backend ("dummy", "libharu",
     * "podofo") -- handy for bisecting failures per engine. */
    const char *only = argc > 1 ? argv[1] : NULL;
    struct
    {
        pdf_backend bk;
        const char *name;
    } engines[2];
    int neng = 0;

    printf("== pdflib complex-document tests ==\n");
    printf("backends: dummy=%d libharu=%d podofo=%d\n",
           pdflib_has_backend(PDF_BACKEND_DUMMY),
           pdflib_has_backend(PDF_BACKEND_LIBHARU),
           pdflib_has_backend(PDF_BACKEND_PODOFO));

    if ((!only || strcmp(only, "podofo") == 0)
        && pdflib_has_backend(PDF_BACKEND_PODOFO))
        engines[neng].bk = PDF_BACKEND_PODOFO, engines[neng++].name = "podofo";
    if ((!only || strcmp(only, "libharu") == 0)
        && pdflib_has_backend(PDF_BACKEND_LIBHARU))
        engines[neng].bk = PDF_BACKEND_LIBHARU,
        engines[neng++].name = "libharu";

    run_all();
    for (int i = 0; i < neng; i++)
    {
        printf("-- backend: %s --\n", engines[i].name);
        real_engine = 1;
        cur_backend = engines[i].name;
        if (pdflib_set_backend(engines[i].bk) != PDF_OK)
        {
            CHECK(0);
            continue;
        }
        test_complex_basic();
        test_rotation_and_page_mode();
        test_streamed_with_images();
        test_links_and_outlines();
        test_encrypted_output();
        run_complex_concurrency();
    }

    printf("== complex tests: %d failure(s), %d skip(s) ==\n", failures,
           skipped);
    return failures ? 1 : 0;
}
