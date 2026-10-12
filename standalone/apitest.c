/**
 * apitest.c - exhaustive consumer test for pdflib.dll.
 *
 * smoke.c proves the DLL works; this proves the DLL is COMPLETE. Every one
 * of the 84 exported entry points is called at least once, with a real
 * assertion on the result, and the documents it produces are parsed back
 * and checked. tools/check_api_coverage.py enforces that mechanically by
 * diffing src/pdflib.def against this file, so a newly exported function
 * cannot slip in untested.
 *
 *   apitest <podofo|libharu|dummy|all> [output-prefix]
 *
 * "all" (the default) runs the whole suite on every compiled-in engine.
 *
 * Coverage map, by area:
 *   version / backend     pdflib_version, pdflib_backend_version,
 *                         pdflib_set_backend, pdflib_get_backend,
 *                         pdflib_has_backend, pdf_dll_version,
 *                         pdf_dll_backends, pdf_dll_has_backend,
 *                         pdf_dll_status_string, pdflib_last_error
 *   lifecycle             pdf_doc_create, pdf_doc_create_streamed,
 *                         pdf_doc_create_streamed_to_file,
 *                         pdf_doc_create_streamed_to_handle,
 *                         pdf_doc_finish, pdf_doc_free
 *   output                pdf_doc_save_to_file, pdf_doc_save_to_memory,
 *                         pdf_doc_save_to_stream, pdf_doc_save_to_handle
 *   info / security       pdf_doc_set_info, pdf_doc_get_info,
 *                         pdf_doc_set_encryption, pdf_doc_set_compression,
 *                         pdf_doc_set_page_mode_use_outlines
 *   pages                 pdf_page_add, pdf_page_add_custom,
 *                         pdf_doc_page_count, pdf_page_get_size,
 *                         pdf_page_set_rotation
 *   fonts                 pdf_font_get_std, pdf_font_load_ttf,
 *                         pdf_font_load_ttf_memory, pdf_font_find_system,
 *                         pdf_font_free, pdf_font_text_width,
 *                         pdf_font_ascent, pdf_font_descent
 *   text                  pdf_page_draw_text, pdf_page_draw_text_aligned,
 *                         pdf_page_draw_text_wrapped
 *   graphics state        pdf_gstate_save, pdf_gstate_restore,
 *                         pdf_set_transform, pdf_set_line_width,
 *                         pdf_set_line_cap, pdf_set_line_join,
 *                         pdf_set_miter_limit, pdf_set_line_dash,
 *                         pdf_set_fill_color, pdf_set_stroke_color,
 *                         pdf_set_opacity, pdf_set_blend_mode,
 *                         pdf_set_text_render_mode
 *   paths                 pdf_path_move_to, pdf_path_line_to,
 *                         pdf_path_curve_to, pdf_path_close,
 *                         pdf_path_rectangle, pdf_path_circle,
 *                         pdf_path_ellipse, pdf_path_arc,
 *                         pdf_path_polygon, pdf_path_paint,
 *                         pdf_path_discard, pdf_page_draw_rect,
 *                         pdf_page_draw_line
 *   images                pdf_image_load_file, pdf_image_load_memory,
 *                         pdf_image_get_size, pdf_page_draw_image,
 *                         pdf_image_free
 *   annotations/outlines  pdf_annot_create, pdf_annot_link_to_page,
 *                         pdf_annot_set_color, pdf_annot_set_icon,
 *                         pdf_annot_set_opacity, pdf_annot_set_author,
 *                         pdf_annot_set_subject, pdf_annot_set_open,
 *                         pdf_annot_free, pdf_outline_add,
 *                         pdf_outline_set_open, pdf_outline_free
 *
 * Exit code 0 == everything passed.
 */
#include "pdflib.h"
#include "image_samples.h"
/* Read-back inspector: lets the tests assert on what was actually written
 * rather than on the fact that a call returned PDF_OK. */
#include "pdf_inspect.h"

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

/* ------------------------------------------------------------------ */
/* Harness                                                             */
/* ------------------------------------------------------------------ */

static int g_fail = 0;
static int g_checks = 0;

#define CHECK(cond, msg)                                                  \
    do {                                                                  \
        g_checks++;                                                       \
        if (!(cond)) {                                                    \
            printf("  [FAIL] %s (%s:%d)\n", msg, __FILE__, __LINE__);     \
            g_fail++;                                                     \
        } else {                                                          \
            printf("  [ok]   %s\n", msg);                                 \
        }                                                                 \
    } while (0)

/* Engine-dependent feature: a clean "this engine cannot do that" is a
 * pass. Only a code that is neither OK nor UNSUPPORTED is a failure. */
#define CHECK_MAYBE(rc, msg)                                              \
    do {                                                                  \
        pdf_status rc_ = (rc);                                            \
        CHECK(rc_ == PDF_OK || rc_ == PDF_ERR_BACKEND_UNSUPPORTED, msg);  \
        if (rc_ != PDF_OK)                                                \
            printf("  [i]    %s: engine declined (%s)\n", msg,            \
                   pdf_dll_status_string(rc_));                           \
    } while (0)

/* A status code must always be one of the defined ones - never a garbage
 * value from an unhandled path. */
static int status_is_valid(pdf_status rc)
{
    return rc >= PDF_OK && rc <= PDF_ERR_IO;
}

/* The two engines legitimately differ on operations that only make sense
 * once an annotation has been materialised:
 *   - PoDoFo creates the annotation object up front even for a GOTO link,
 *     libharu defers it until annot_link_to_page() supplies the
 *     destination, so before that call libharu has no dictionary to write
 *     into and declines with PDF_ERR_INVALID_PARAM.
 *   - /Name and /Open are text-annotation keys; libharu refuses them on
 *     other subtypes (PDF_ERR_BACKEND_UNSUPPORTED) where PoDoFo just
 *     writes the key.
 * Accept "done" or "cleanly declined", and say which happened. Anything
 * else - a garbage code, or a crash - is a failure. */
static void check_declined_ok(pdf_status rc, const char *msg, int line)
{
    g_checks++;
    if (rc == PDF_OK || rc == PDF_ERR_INVALID_PARAM ||
        rc == PDF_ERR_BACKEND_UNSUPPORTED)
    {
        printf("  [ok]   %s\n", msg);
        if (rc != PDF_OK)
            printf("  [i]    %s: engine declined (%s)\n", msg,
                   pdf_dll_status_string(rc));
    }
    else
    {
        printf("  [FAIL] %s -> %s (%s:%d)\n", msg, pdf_dll_status_string(rc),
               __FILE__, line);
        g_fail++;
    }
}

#define CHECK_DECLINED(rc, msg) check_declined_ok((rc), (msg), __LINE__)

static const char *g_prefix = "apitest";

static void path_for(char *out, size_t n, const char *suffix)
{
    snprintf(out, n, "%s-%s", g_prefix, suffix);
}

/* --- OS handles ------------------------------------------------------
 * pdflib takes OS handles, not FILE*, for already-open targets: the DLL
 * links the CRT statically, so a FILE* from this module is foreign to it
 * and its stdio would abort the process. See STANDALONE-DLL.md. */
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

/* --- file helpers ---------------------------------------------------- */

static long slurp(const char *path, unsigned char **out)
{
    FILE *f = fopen(path, "rb");
    long n;

    *out = NULL;
    if (!f)
        return -1;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0) { fclose(f); return -1; }
    *out = (unsigned char *)malloc((size_t)n + 1);
    if (!*out) { fclose(f); return -1; }
    if (n > 0 && fread(*out, 1, (size_t)n, f) != (size_t)n)
    {
        free(*out);
        *out = NULL;
        fclose(f);
        return -1;
    }
    (*out)[n] = 0;
    fclose(f);
    return n;
}

/* memmem() is a GNU extension that MSVC does not have. */
static int contains(const unsigned char *buf, long n, const char *needle)
{
    size_t m = strlen(needle);
    long i;

    if (m == 0 || n < (long)m)
        return 0;
    for (i = 0; i + (long)m <= n; i++)
        if (memcmp(buf + i, needle, m) == 0)
            return 1;
    return 0;
}

/* Count page objects in a serialized PDF.
 *
 * The two engines do not agree on spacing: PoDoFo writes "/Type/Page"
 * while libharu writes "/Type /Page", and both write "/Type/Pages" for
 * the tree nodes. A plain substring search for either spelling is
 * therefore wrong. Walk "/Type", skip whitespace, then require "/Page"
 * not followed by the 's' of "/Pages".
 *
 * If the writer folded the page dictionaries into object streams the
 * dictionaries are not visible in the raw bytes at all; validate_pdf()
 * detects that case before calling this. */
static int count_page_objects(const unsigned char *buf, long n)
{
    static const char key[] = "/Type";
    const long klen = (long)sizeof(key) - 1;
    int count = 0;
    long i;

    for (i = 0; i + klen <= n; i++)
    {
        long j;

        if (memcmp(buf + i, key, (size_t)klen) != 0)
            continue;

        j = i + klen;
        while (j < n && (buf[j] == ' ' || buf[j] == '\t' || buf[j] == '\r'
                         || buf[j] == '\n' || buf[j] == '\f' || buf[j] == 0))
            j++;

        if (j + 5 > n || memcmp(buf + j, "/Page", 5) != 0)
            continue;
        j += 5;
        if (j < n && buf[j] == 's')   /* "/Pages" is a tree node */
            continue;
        count++;
    }
    return count;
}

/* Every produced file must be a structurally complete PDF. */
static void validate_pdf(const char *path, int min_pages, const char *label)
{
    unsigned char *buf = NULL;
    long n = slurp(path, &buf);
    char msg[160];

    snprintf(msg, sizeof(msg), "%s: readable", label);
    if (n <= 8 || !buf)
    {
        CHECK(0, msg);
        free(buf);
        return;
    }
    CHECK(1, msg);

    snprintf(msg, sizeof(msg), "%s: %%PDF- header", label);
    CHECK(memcmp(buf, "%PDF-", 5) == 0, msg);

    snprintf(msg, sizeof(msg), "%s: %%EOF trailer", label);
    CHECK(contains(buf, n, "%%EOF"), msg);

    snprintf(msg, sizeof(msg), "%s: xref table present", label);
    CHECK(contains(buf, n, "xref") || contains(buf, n, "/XRef"), msg);

    if (min_pages > 0)
    {
        if (contains(buf, n, "/ObjStm"))
        {
            /* Page dictionaries live inside compressed object streams and
             * cannot be seen in the raw bytes. The page count is verified
             * through the API instead (pdf_doc_page_count), and the
             * structural checks above still apply to this artefact. */
            printf("  [i]    %s: object streams present - raw page count "
                   "not applicable\n", label);
        }
        else
        {
            int pages = count_page_objects(buf, n);
            snprintf(msg, sizeof(msg), "%s: >= %d page object(s) (found %d)",
                     label, min_pages, pages);
            CHECK(pages >= min_pages, msg);
        }
    }

    printf("  [i]    %s: %ld bytes written\n", label, n);
    free(buf);
}

static int files_identical(const char *a, const char *b)
{
    unsigned char *x = NULL, *y = NULL;
    long nx = slurp(a, &x), ny = slurp(b, &y);
    int same = (nx >= 0 && nx == ny && memcmp(x, y, (size_t)nx) == 0);

    free(x);
    free(y);
    return same;
}

/* ------------------------------------------------------------------ */
/* Read-back assertions                                                */
/*                                                                     */
/* A status code only says the library ACCEPTED a call. Every helper   */
/* below instead opens the artefact the call produced, decodes it, and */
/* asserts on what is actually inside. That is the only way to catch a  */
/* function that returns PDF_OK and draws nothing, or that writes the   */
/* right number of pages with the wrong content.                        */
/*                                                                     */
/* `rb` owns the file bytes and a heap-allocated object table (the      */
/* table alone is ~320 KB, too big to put on the stack).               */
/* ------------------------------------------------------------------ */

typedef struct rb
{
    char           label[80];
    unsigned char *buf;
    long           n;
    pdfi_doc      *d;
    int            ok;
} rb;

static int rb_open(rb *r, const char *path, const char *label)
{
    memset(r, 0, sizeof(*r));
    snprintf(r->label, sizeof(r->label), "%s", label);

    r->n = slurp(path, &r->buf);
    if (r->n <= 8 || !r->buf)
    {
        g_checks++;
        printf("  [FAIL] %s: cannot read back '%s'\n", label, path);
        g_fail++;
        return 0;
    }
    r->d = (pdfi_doc *)malloc(sizeof(pdfi_doc));
    if (!r->d)
    {
        g_checks++;
        printf("  [FAIL] %s: out of memory\n", label);
        g_fail++;
        free(r->buf);
        r->buf = NULL;
        return 0;
    }
    pdfi_scan(r->buf, r->n, r->d);
    r->ok = 1;
    return 1;
}

static void rb_close(rb *r)
{
    free(r->d);
    free(r->buf);
    r->d = NULL;
    r->buf = NULL;
}

/* Report one read-back assertion. Keeping the label prefix uniform makes
 * a failure identify the artefact and the property in one line. */
static void rb_report(const rb *r, int cond, const char *what)
{
    g_checks++;
    if (cond)
        printf("  [ok]   %s: %s\n", r->label, what);
    else
    {
        printf("  [FAIL] %s: %s\n", r->label, what);
        g_fail++;
    }
}

/* An artefact that could not be opened is already counted as one failure;
 * skipping the follow-ups keeps a single broken file from producing a
 * cascade of noise that hides the real error. */
#define RB_GUARD(r) do { if (!(r)->ok) return; } while (0)

static void rb_info(const rb *r, const char *what)
{
    printf("  [i]    %s: %s\n", r->label, what);
}

/* --- structure ------------------------------------------------------ */

static int rb_page_count(const rb *r)
{
    long nums[PDFI_MAX_PAGES];

    return pdfi_pages(r->d, nums, PDFI_MAX_PAGES);
}

static void rb_check_pages(rb *r, int expect, const char *what)
{
    char msg[160];
    int got;

    RB_GUARD(r);
    got = rb_page_count(r);
    snprintf(msg, sizeof(msg), "%s (expected %d, found %d)", what, expect, got);
    rb_report(r, got == expect, msg);
}

/* Every stream in the file must decode. This is the single most valuable
 * read-back check: a truncated or mis-sized stream (the failure mode that
 * a wrong /Length produces) shows up here and nowhere else, because the
 * xref and the page tree still look perfect. */
static void rb_check_streams_decode(rb *r, const char *what)
{
    int i, bad = 0, checked = 0, skipped = 0;
    char msg[160];

    RB_GUARD(r);
    for (i = 0; i < r->d->nobj; i++)
    {
        const pdfi_obj *o = &r->d->objs[i];
        unsigned char *decoded = NULL;
        long len;

        if (o->stream_off < 0 || o->stream_len <= 0)
            continue;
        /* Only Flate-coded and unfiltered streams are decodable here; a
         * DCTDecode/JPXDecode image is opaque on purpose. Note the test
         * must ask about the FILTER CHAIN: libharu writes
         * "/Filter [ /FlateDecode ]", so a name-only check would classify
         * every compressed stream as unfiltered. */
        if (!pdfi_stream_is_flate(r->d, o))
        {
            char filters[4][48];
            int nf = pdfi_stream_filters(r->d, o, filters, 4);

            if (nf > 0)
            {
                skipped++;
                continue;
            }
        }
        checked++;
        len = pdfi_stream(r->d, o, &decoded);
        if (len < 0)
            bad++;
        free(decoded);
    }
    snprintf(msg, sizeof(msg),
             "%s: every decodable stream inflates (%d checked, %d failed, "
             "%d non-Flate skipped)", what, checked, bad, skipped);
    rb_report(r, bad == 0 && checked > 0, msg);
}

/* Require exactly `expect_nonempty` pages to carry non-empty content.
 *
 * This is the direct test for "the API said PDF_OK but drew nothing": a page
 * that silently ends up blank still has a valid page tree, a valid /MediaBox
 * and a decodable (empty) stream, so nothing else here would notice. The
 * per-operator checks above only catch it on the pages they name.
 *
 * The count - not a per-page flag - is the right shape, because the two
 * engines represent "a page nothing was drawn on" differently and both are
 * legal: PoDoFo drops the /Contents key altogether, libharu emits a
 * zero-length content stream. Asserting "every page has content" therefore
 * fails on a perfectly correct file; asserting "N pages have content" does
 * not, while still catching a page that should have been drawn but was not. */
static void rb_check_pages_nonempty(rb *r, const char *what, int expect_nonempty)
{
    long nums[PDFI_MAX_PAGES];
    int np, i, blank = 0, empty = 0, have = 0;
    long total = 0;
    char msg[240];

    RB_GUARD(r);
    np = pdfi_pages(r->d, nums, PDFI_MAX_PAGES);
    for (i = 0; i < np; i++)
    {
        unsigned char *c = NULL;
        long n;

        if (!pdfi_page_has_contents(r->d, nums[i]))
        {
            blank++;                    /* no /Contents key at all (PoDoFo) */
            continue;
        }
        n = pdfi_page_content(r->d, nums[i], &c);
        if (n <= 0)
            empty++;                    /* /Contents present but empty (libharu) */
        else
        {
            have++;
            total += n;
        }
        free(c);
    }
    snprintf(msg, sizeof(msg),
             "%s: %d of %d page(s) carry non-empty content (want %d; %d had no "
             "/Contents, %d had an empty stream, %ld bytes total)",
             what, have, np, expect_nonempty, blank, empty, total);
    rb_report(r, have == expect_nonempty, msg);
}

/* A specific page must carry no content at all. The count check above would
 * still pass if the wrong page were blank, so name the page.
 *
 * Both representations are accepted and the message says which one was seen,
 * because the engines differ: PoDoFo omits /Contents, libharu writes an
 * empty stream. That difference is worth seeing in the output. */
static void rb_check_page_blank(rb *r, int page_idx, const char *what)
{
    long nums[PDFI_MAX_PAGES];
    unsigned char *c = NULL;
    long n;
    int np, has;
    char msg[200];

    RB_GUARD(r);
    np = pdfi_pages(r->d, nums, PDFI_MAX_PAGES);
    if (page_idx >= np)
    {
        snprintf(msg, sizeof(msg), "%s: page %d does not exist", what,
                 page_idx + 1);
        rb_report(r, 0, msg);
        return;
    }
    has = pdfi_page_has_contents(r->d, nums[page_idx]);
    if (!has)
    {
        snprintf(msg, sizeof(msg),
                 "%s: page %d has no /Contents (PoDoFo drops the key)", what,
                 page_idx + 1);
        rb_report(r, 1, msg);
        return;
    }
    n = pdfi_page_content(r->d, nums[page_idx], &c);
    free(c);
    snprintf(msg, sizeof(msg),
             "%s: page %d has a zero-length /Contents (libharu writes an empty "
             "stream)", what, page_idx + 1);
    rb_report(r, n == 0, msg);
}

/* --- pages ---------------------------------------------------------- */

/* /MediaBox in points, to within half a point (the writers round the
 * conversion from mm and the two engines round differently). */
static void rb_check_page_size(rb *r, int page_idx, double w, double h,
                               const char *what)
{
    long nums[PDFI_MAX_PAGES];
    double box[4];
    int np;
    char msg[200];

    RB_GUARD(r);
    np = pdfi_pages(r->d, nums, PDFI_MAX_PAGES);
    if (page_idx >= np)
    {
        snprintf(msg, sizeof(msg), "%s: page %d missing (only %d pages)",
                 what, page_idx + 1, np);
        rb_report(r, 0, msg);
        return;
    }
    if (pdfi_dict_array_nums(r->d, pdfi_find(r->d, nums[page_idx]), "MediaBox",
                             box, 4) != 4)
    {
        snprintf(msg, sizeof(msg), "%s: /MediaBox missing on page %d",
                 what, page_idx + 1);
        rb_report(r, 0, msg);
        return;
    }
    {
        double gw = box[2] - box[0];
        double gh = box[3] - box[1];
        int ok = (gw - w < 0.5 && w - gw < 0.5) &&
                 (gh - h < 0.5 && h - gh < 0.5);

        snprintf(msg, sizeof(msg), "%s: page %d is %.2fx%.2f pt "
                 "(wanted %.2fx%.2f)", what, page_idx + 1, gw, gh, w, h);
        rb_report(r, ok, msg);
    }
}

static void rb_check_page_rotate(rb *r, int page_idx, long deg,
                                 const char *what)
{
    long nums[PDFI_MAX_PAGES];
    long got = 0;
    int np;
    char msg[160];

    RB_GUARD(r);
    np = pdfi_pages(r->d, nums, PDFI_MAX_PAGES);
    if (page_idx >= np)
    {
        snprintf(msg, sizeof(msg), "%s: page %d missing", what, page_idx + 1);
        rb_report(r, 0, msg);
        return;
    }
    pdfi_dict_int(r->d, pdfi_find(r->d, nums[page_idx]), "Rotate", &got);
    snprintf(msg, sizeof(msg), "%s: page %d /Rotate = %ld (wanted %ld)",
             what, page_idx + 1, got, deg);
    rb_report(r, got == deg, msg);
}

/* --- content streams ------------------------------------------------ */

/* Decoded content of a page, or NULL. Caller frees. pdfi_page_content
 * hands back the length as its return value and the buffer through an
 * out-parameter, so wrap both here rather than at every call site. */
static unsigned char *rb_page_content(const rb *r, int page_idx, long *len)
{
    long nums[PDFI_MAX_PAGES];
    int np;
    unsigned char *out = NULL;

    *len = 0;
    if (!r->ok)
        return NULL;
    np = pdfi_pages(r->d, nums, PDFI_MAX_PAGES);
    if (page_idx >= np)
        return NULL;
    *len = pdfi_page_content(r->d, nums[page_idx], &out);
    if (*len < 0)
    {
        *len = 0;
        return NULL;
    }
    return out;
}

/* Why a page produced no decodable content. A page that carries no /Contents
 * at all is merely blank (PoDoFo drops the key when nothing was drawn on the
 * page), while a /Contents that exists but will not inflate is a defect. A
 * single "no decodable content" message would send you hunting for a
 * compression bug in a document that is simply empty. */
static const char *rb_why_no_content(const rb *r, int page_idx)
{
    long nums[PDFI_MAX_PAGES];
    int np;

    if (!r->ok)
        return "document did not open";
    np = pdfi_pages(r->d, nums, PDFI_MAX_PAGES);
    if (page_idx >= np)
        return "no such page";
    if (!pdfi_page_has_contents(r->d, nums[page_idx]))
        return "page has no /Contents - nothing was drawn on it";
    return "stream present but will not inflate";
}

static void rb_check_op(rb *r, int page_idx, const char *op, int min_count,
                        const char *what)
{
    unsigned char *c;
    long n;
    int got;
    char msg[200];

    RB_GUARD(r);
    c = rb_page_content(r, page_idx, &n);
    if (!c)
    {
        snprintf(msg, sizeof(msg), "%s: page %d has no decodable content (%s)",
                 what, page_idx + 1, rb_why_no_content(r, page_idx));
        rb_report(r, 0, msg);
        return;
    }
    got = pdfi_count_op(c, n, op);
    snprintf(msg, sizeof(msg), "%s: page %d has '%s' x%d (want >= %d)",
             what, page_idx + 1, op, got, min_count);
    rb_report(r, got >= min_count, msg);
    free(c);
}

static void rb_check_no_op(rb *r, int page_idx, const char *op,
                           const char *what)
{
    unsigned char *c;
    long n;
    int got;
    char msg[200];

    RB_GUARD(r);
    c = rb_page_content(r, page_idx, &n);
    if (!c)
    {
        snprintf(msg, sizeof(msg), "%s: page %d has no decodable content (%s)",
                 what, page_idx + 1, rb_why_no_content(r, page_idx));
        rb_report(r, 0, msg);
        return;
    }
    got = pdfi_count_op(c, n, op);
    snprintf(msg, sizeof(msg), "%s: page %d has no '%s' (found %d)",
             what, page_idx + 1, op, got);
    rb_report(r, got == 0, msg);
    free(c);
}

/* Is the text actually in the page's content stream? */
static void rb_check_text(rb *r, int page_idx, const char *text,
                          const char *what)
{
    unsigned char *c;
    long n;
    int got;
    char msg[220];

    RB_GUARD(r);
    c = rb_page_content(r, page_idx, &n);
    if (!c)
    {
        snprintf(msg, sizeof(msg), "%s: page %d has no decodable content (%s)",
                 what, page_idx + 1, rb_why_no_content(r, page_idx));
        rb_report(r, 0, msg);
        return;
    }
    got = pdfi_content_has_text(c, n, text);
    snprintf(msg, sizeof(msg), "%s: page %d carries the text \"%s\"",
             what, page_idx + 1, text);
    rb_report(r, got, msg);
    free(c);
}

/* The text must be painted exactly `expect` times.
 *
 * Presence is blind to a duplicated run: pdf_page_draw_text_wrapped used
 * to re-wrap and re-draw the whole paragraph on every pass through its
 * box-filling loop, so a paragraph that wrapped to fewer lines than its
 * box could hold was painted two or three times over itself. "The text
 * is in the file" was true the whole time; only a count sees it. */
static void rb_check_text_count(rb *r, int page_idx, const char *text,
                                int expect, const char *what)
{
    unsigned char *c;
    long n;
    int got;
    char msg[240];

    RB_GUARD(r);
    c = rb_page_content(r, page_idx, &n);
    if (!c)
    {
        snprintf(msg, sizeof(msg), "%s: page %d has no decodable content (%s)",
                 what, page_idx + 1, rb_why_no_content(r, page_idx));
        rb_report(r, 0, msg);
        return;
    }
    got = pdfi_count_text(c, n, text);
    snprintf(msg, sizeof(msg),
             "%s: page %d paints \"%s\" exactly %d time(s) (got %d)",
             what, page_idx + 1, text, expect, got);
    rb_report(r, got == expect, msg);
    free(c);
}

/* The operand of the last occurrence of `op` on a page must be `expect`.
 *
 * For /Tr this is the whole question of whether the page finished in
 * fill mode. /Tr is text state, so it carries into every later text
 * object; PoDoFo's painter writes it only when the mode is not Fill,
 * which means a page that used "3 Tr" and never wrote "0 Tr" afterwards
 * has silently lost every word drawn after that point. Counting "Tr"
 * operators cannot see it - the operator is there either way. */
static void rb_check_last_op_operand(rb *r, int page_idx, const char *op,
                                     long expect, const char *what)
{
    unsigned char *c;
    long n;
    long got;
    char msg[240];

    RB_GUARD(r);
    c = rb_page_content(r, page_idx, &n);
    if (!c)
    {
        snprintf(msg, sizeof(msg), "%s: page %d has no decodable content (%s)",
                 what, page_idx + 1, rb_why_no_content(r, page_idx));
        rb_report(r, 0, msg);
        return;
    }
    got = pdfi_last_op_operand(c, n, op);
    snprintf(msg, sizeof(msg),
             "%s: page %d ends with '%s %ld' (want %ld)",
             what, page_idx + 1, op, got, expect);
    rb_report(r, got == expect, msg);
    free(c);
}

/* The drawing operators must be balanced: an unbalanced q/Q is the classic
 * symptom of a gstate leak, and it silently corrupts every later page. */
static void rb_check_q_balance(rb *r, int page_idx, const char *what)
{
    unsigned char *c;
    long n;
    int q, Q;
    char msg[200];

    RB_GUARD(r);
    c = rb_page_content(r, page_idx, &n);
    if (!c)
    {
        snprintf(msg, sizeof(msg), "%s: page %d has no decodable content",
                 what, page_idx + 1);
        rb_report(r, 0, msg);
        return;
    }
    q = pdfi_count_op(c, n, "q");
    Q = pdfi_count_op(c, n, "Q");
    snprintf(msg, sizeof(msg), "%s: page %d q/Q balanced (q=%d Q=%d)",
             what, page_idx + 1, q, Q);
    rb_report(r, q == Q, msg);
    free(c);
}

/* --- fonts ---------------------------------------------------------- */

/* Locate a font object by /BaseFont substring. Returns the object index
 * in r->d->objs, or -1. */
static int rb_find_font(const rb *r, const char *basefont)
{
    int i;

    for (i = 0; i < r->d->nobj; i++)
    {
        char base[128] = "", sub[48] = "";

        pdfi_dict_name(r->d, &r->d->objs[i], "BaseFont", base, sizeof(base));
        pdfi_dict_name(r->d, &r->d->objs[i], "Subtype", sub, sizeof(sub));
        if (!base[0])
            continue;
        if (strcmp(sub, "Type1") != 0 && strcmp(sub, "TrueType") != 0 &&
            strcmp(sub, "Type0") != 0 && strcmp(sub, "Type3") != 0 &&
            strcmp(sub, "CIDFontType0") != 0 && strcmp(sub, "CIDFontType2") != 0)
            continue;
        if (contains((const unsigned char *)base, (long)strlen(base), basefont))
            return i;
    }
    return -1;
}

static void rb_check_font_present(rb *r, const char *basefont,
                                  const char *what)
{
    char msg[200];
    int idx;

    RB_GUARD(r);
    idx = rb_find_font(r, basefont);
    snprintf(msg, sizeof(msg), "%s: font '%s' object present", what, basefont);
    rb_report(r, idx >= 0, msg);
}

/* The standard 14 fonts are the ones a reader is expected to supply, so a
 * file that embeds them is bloated and one that claims to embed them is
 * lying. */
static void rb_check_font_not_embedded(rb *r, const char *basefont,
                                       const char *what)
{
    char msg[220];
    int idx;
    int embedded = 0;

    RB_GUARD(r);
    idx = rb_find_font(r, basefont);
    if (idx < 0)
    {
        snprintf(msg, sizeof(msg), "%s: font '%s' not found", what, basefont);
        rb_report(r, 0, msg);
        return;
    }
    if (pdfi_dict_has(r->d, &r->d->objs[idx], "FontFile") ||
        pdfi_dict_has(r->d, &r->d->objs[idx], "FontFile2") ||
        pdfi_dict_has(r->d, &r->d->objs[idx], "FontFile3") ||
        pdfi_dict_has(r->d, &r->d->objs[idx], "FontDescriptor"))
        embedded = 1;

    snprintf(msg, sizeof(msg),
             "%s: standard font '%s' is NOT embedded", what, basefont);
    rb_report(r, !embedded, msg);
}

/* A Type0 (composite) font must reach its glyph data through
 * /DescendantFonts and declare a /CIDSystemInfo; that chain is what makes
 * CJK text renderable. A Type0 font without it renders as nothing in a
 * real reader while still looking fine to a status-code test.
 *
 * Naming the font is not portable - which CJK face exists depends on the
 * machine - so this asserts the STRUCTURE, which is what actually has to
 * be right. */
static void rb_check_any_cid_font(rb *r, const char *what)
{
    int i;
    int found = 0;
    char msg[200];

    RB_GUARD(r);
    for (i = 0; i < r->d->nobj && !found; i++)
    {
        char sub[48] = "";
        long cid;

        pdfi_dict_name(r->d, &r->d->objs[i], "Subtype", sub, sizeof(sub));
        if (strcmp(sub, "Type0") != 0)
            continue;

        /* /DescendantFonts may be an inline array or a reference to an
         * array object; pdfi_dict_first_ref handles both. */
        cid = pdfi_dict_first_ref(r->d, &r->d->objs[i], "DescendantFonts");
        if (cid >= 0)
        {
            const pdfi_obj *o = pdfi_find(r->d, cid);

            if (o && pdfi_dict_has(r->d, o, "CIDSystemInfo"))
                found = 1;
        }
    }

    snprintf(msg, sizeof(msg),
             "%s: a /Type0 font has a /DescendantFonts chain with "
             "/CIDSystemInfo", what);
    rb_report(r, found, msg);
}

/* --- images --------------------------------------------------------- */

/* Assert an image XObject with the given pixel dimensions exists AND is
 * actually placed on the page with a Do operator. Both halves matter: an
 * XObject nobody draws is dead weight, and a Do with no XObject is a
 * broken page. */
static void rb_check_image(rb *r, int page_idx, long w, long h,
                           const char *what)
{
    char msg[220];
    int i, found = 0;
    long fw = 0, fh = 0;

    RB_GUARD(r);
    for (i = 0; i < r->d->nobj; i++)
    {
        const pdfi_obj *o = &r->d->objs[i];
        char sub[48] = "";
        long iw = 0, ih = 0;

        if (o->stream_off < 0)
            continue;
        pdfi_dict_name(r->d, o, "Subtype", sub, sizeof(sub));
        if (strcmp(sub, "Image") != 0)
            continue;
        if (!pdfi_dict_int(r->d, o, "Width", &iw) ||
            !pdfi_dict_int(r->d, o, "Height", &ih))
            continue;
        if (iw == w && ih == h)
        {
            found = 1;
            fw = iw;
            fh = ih;
            break;
        }
    }
    snprintf(msg, sizeof(msg), "%s: image XObject %ldx%ld present", what, w, h);
    rb_report(r, found, msg);
    if (found)
        rb_check_op(r, page_idx, "Do", 1, what);
}

/* At least one font in the file must carry a real font program.
 *
 * Naming the font is not possible here: which system TTF gets loaded
 * depends on the machine, and a subsetted font's /BaseFont carries a
 * random six-letter tag prefix. What is both portable and meaningful is
 * "the embed flag produced an actual embedded program somewhere", which
 * paired with the standard-font check proves the flag is honoured in both
 * directions. */
static void rb_check_any_font_embedded(rb *r, const char *what)
{
    int i;
    int embedded = 0;
    char msg[200];

    RB_GUARD(r);
    for (i = 0; i < r->d->nobj && !embedded; i++)
    {
        const pdfi_obj *o = &r->d->objs[i];
        long fd;
        int k;

        if (o->stream_off >= 0)
            continue;               /* a font dictionary is not a stream */
        if (!pdfi_dict_has(r->d, o, "BaseFont"))
            continue;

        for (k = 0; k < 3 && !embedded; k++)
        {
            static const char *keys[3] = { "FontFile", "FontFile2",
                                           "FontFile3" };
            long sref;

            /* Either the descriptor is referenced from the font, or the
             * font object IS the descriptor (PoDoFo inlines it). */
            fd = pdfi_dict_ref(r->d, o, "FontDescriptor");
            if (fd >= 0)
            {
                const pdfi_obj *desc = pdfi_find(r->d, fd);

                if (desc)
                {
                    sref = pdfi_dict_ref(r->d, desc, keys[k]);
                    if (sref >= 0)
                    {
                        const pdfi_obj *fs = pdfi_find(r->d, sref);

                        if (fs && fs->stream_len > 0)
                            embedded = 1;
                    }
                }
            }
            sref = pdfi_dict_ref(r->d, o, keys[k]);
            if (sref >= 0)
            {
                const pdfi_obj *fs = pdfi_find(r->d, sref);

                if (fs && fs->stream_len > 0)
                    embedded = 1;
            }
        }
    }

    snprintf(msg, sizeof(msg),
             "%s: at least one font carries an embedded font program", what);
    rb_report(r, embedded, msg);
}

/* --- annotations ---------------------------------------------------- */

/* Count annotations of a subtype on a page, walking /Annots. */
static int rb_count_annots(const rb *r, int page_idx, const char *subtype)
{
    long nums[PDFI_MAX_PAGES];
    int np = pdfi_pages(r->d, nums, PDFI_MAX_PAGES);
    long p, end;
    int count = 0;

    if (page_idx >= np)
        return 0;
    if (!pdfi_dict_array_span(r->d, pdfi_find(r->d, nums[page_idx]),
                              "Annots", &p, &end))
        return 0;

    while (p < end)
    {
        pdfi_val e;
        const pdfi_obj *a;
        char sub[48] = "";

        while (p < end && pdfi_is_ws(r->d->buf[p]))
            p++;
        if (p >= end || r->d->buf[p] == ']')
            break;
        pdfi_read_value(r->d->buf, end, &p, &e);
        if (e.kind != PDFI_V_REF)
            continue;
        a = pdfi_find(r->d, e.ival);
        if (!a)
            continue;
        pdfi_dict_name(r->d, a, "Subtype", sub, sizeof(sub));
        if (subtype == NULL || strcmp(sub, subtype) == 0)
            count++;
    }
    return count;
}

static void rb_check_annot(rb *r, int page_idx, const char *subtype,
                           int min_count, const char *what)
{
    char msg[200];
    int got;

    RB_GUARD(r);
    got = rb_count_annots(r, page_idx, subtype);
    snprintf(msg, sizeof(msg), "%s: page %d has >= %d /%s annotation(s) "
             "(found %d)", what, page_idx + 1, min_count, subtype, got);
    rb_report(r, got >= min_count, msg);
}

static void rb_check_annot_total(rb *r, int page_idx, int expect,
                                 const char *what)
{
    char msg[200];
    int got;

    RB_GUARD(r);
    got = rb_count_annots(r, page_idx, NULL);
    snprintf(msg, sizeof(msg), "%s: page %d /Annots has %d entries "
             "(want %d)", what, page_idx + 1, got, expect);
    rb_report(r, got == expect, msg);
}

/* A URI link must carry the action dictionary, not just the rectangle.
 *
 * The action may be inline (/A << ... >>, libharu) or a reference
 * (/A 11 0 R, PoDoFo); pdfi_dict_subdict accepts both. */
static void rb_check_link_uri(rb *r, int page_idx, const char *uri,
                              const char *what)
{
    long nums[PDFI_MAX_PAGES];
    int np;
    long p, end;
    int found = 0;
    char msg[240];

    RB_GUARD(r);
    np = pdfi_pages(r->d, nums, PDFI_MAX_PAGES);
    if (page_idx >= np)
    {
        snprintf(msg, sizeof(msg), "%s: page %d missing", what, page_idx + 1);
        rb_report(r, 0, msg);
        return;
    }
    if (!pdfi_dict_array_span(r->d, pdfi_find(r->d, nums[page_idx]),
                              "Annots", &p, &end))
    {
        snprintf(msg, sizeof(msg), "%s: page %d has no /Annots", what,
                 page_idx + 1);
        rb_report(r, 0, msg);
        return;
    }

    while (p < end && !found)
    {
        pdfi_val e;
        const pdfi_obj *a;
        char sub[48] = "";
        pdfi_obj act;
        char got[256] = "";

        while (p < end && pdfi_is_ws(r->d->buf[p]))
            p++;
        if (p >= end || r->d->buf[p] == ']')
            break;
        pdfi_read_value(r->d->buf, end, &p, &e);
        if (e.kind != PDFI_V_REF)
            continue;
        a = pdfi_find(r->d, e.ival);
        if (!a)
            continue;
        pdfi_dict_name(r->d, a, "Subtype", sub, sizeof(sub));
        if (strcmp(sub, "Link") != 0)
            continue;
        if (!pdfi_dict_subdict(r->d, a, "A", &act))
            continue;
        if (!pdfi_dict_text(r->d, &act, "URI", got, sizeof(got)))
            continue;
        if (strcmp(got, uri) == 0)
            found = 1;
    }

    snprintf(msg, sizeof(msg), "%s: page %d has a /Link with /A /URI \"%s\"",
             what, page_idx + 1, uri);
    rb_report(r, found, msg);
}

/* --- outlines ------------------------------------------------------- */

/* The outline tree.
 *
 * Two engine differences have to be absorbed here:
 *   - the root is found through the catalog's /Outlines reference, not by
 *     looking for /Type /Outlines. libharu tags every outline ITEM with
 *     /Type /Outlines as well (the spec reserves that type for the root),
 *     so a type-based search picks an item and then finds no /First on it;
 *   - /Count is not portable on the root either: the spec requires it, but
 *     PoDoFo omits it and writes only /First and /Last.
 * What must hold on both engines is that the catalog reaches an outline
 * root with a /First, and that the items really exist - so count the item
 * objects (they carry /Title and /Parent, which no other object type does)
 * and require the root to have a /First. */
static void rb_check_outlines(rb *r, int min_items, const char *what)
{
    int i;
    long root = -1;
    int items = 0;
    char msg[200];

    RB_GUARD(r);

    /* Catalog -> /Outlines. */
    {
        long cat = pdfi_trailer_ref(r->d, "Root");

        if (cat >= 0)
        {
            const pdfi_obj *c = pdfi_find(r->d, cat);

            if (c)
                root = pdfi_dict_ref(r->d, c, "Outlines");
        }
    }

    for (i = 0; i < r->d->nobj; i++)
    {
        /* An outline item: has /Title and /Parent. An annotation has /P,
         * not /Parent, so there is no overlap. */
        if (pdfi_dict_has(r->d, &r->d->objs[i], "Title") &&
            pdfi_dict_has(r->d, &r->d->objs[i], "Parent"))
            items++;
    }

    if (root < 0 || !pdfi_find(r->d, root))
    {
        snprintf(msg, sizeof(msg), "%s: catalog has no reachable /Outlines",
                 what);
        rb_report(r, 0, msg);
        return;
    }
    snprintf(msg, sizeof(msg), "%s: /Outlines root has /First", what);
    rb_report(r, pdfi_dict_has(r->d, pdfi_find(r->d, root), "First"), msg);

    snprintf(msg, sizeof(msg), "%s: %d outline item(s) present (want >= %d)",
             what, items, min_items);
    rb_report(r, items >= min_items, msg);
}

/* --- info / security ------------------------------------------------ */

static void rb_check_info(rb *r, const char *key, const char *value,
                          const char *what)
{
    char got[256] = "";
    char msg[320];
    long info;

    RB_GUARD(r);
    /* /Info is reached through the trailer, which the object scan does not
     * follow. Resolve the reference, then read the key from that object. */
    info = pdfi_trailer_ref(r->d, "Info");
    if (info < 0)
    {
        snprintf(msg, sizeof(msg), "%s: trailer has no /Info reference", what);
        rb_report(r, 0, msg);
        return;
    }
    {
        const pdfi_obj *o = pdfi_find(r->d, info);
        int ok = o && pdfi_dict_text(r->d, o, key, got, sizeof(got));

        snprintf(msg, sizeof(msg), "%s: /Info /%s = \"%s\" (want \"%s\")",
                 what, key, ok ? got : "<missing>", value);
        rb_report(r, ok && strcmp(got, value) == 0, msg);
    }
}

static void rb_check_encrypt(rb *r, int expect, const char *what)
{
    char msg[160];
    int got = pdfi_contains(r->buf, r->n, "/Encrypt");

    RB_GUARD(r);
    snprintf(msg, sizeof(msg), "%s: /Encrypt %s", what,
             expect ? "present" : "absent");
    rb_report(r, got == expect, msg);
}

/* A compressed file must actually contain Flate streams: a "compression
 * on" document that stored everything raw means the flag was ignored. */
static void rb_check_has_flate(rb *r, const char *what)
{
    int i, flate = 0;
    char msg[160];

    RB_GUARD(r);
    for (i = 0; i < r->d->nobj; i++)
    {
        if (r->d->objs[i].stream_off < 0)
            continue;
        /* Ask about the filter CHAIN, not a /Filter name: libharu writes
         * "/Filter [ /FlateDecode ]" and a name-only reader counts zero. */
        if (pdfi_stream_is_flate(r->d, &r->d->objs[i]))
            flate++;
    }
    snprintf(msg, sizeof(msg), "%s: %d FlateDecode stream(s) present", what,
             flate);
    rb_report(r, flate > 0, msg);
}

/* Sink used by pdf_doc_save_to_stream. It lives in this module on
 * purpose: the callback runs inside the caller, so the caller's CRT is
 * the one touching the FILE* - the safe direction. */
static pdf_status apitest_sink(const unsigned char *data, size_t len,
                               void *userdata);

/* ------------------------------------------------------------------ */
/* Version / backend introspection                                     */
/* ------------------------------------------------------------------ */

static void test_introspection(void)
{
    int i;
    int all_text = 1;

    printf("[test] version & backend introspection\n");

    CHECK(pdflib_version() && *pdflib_version(), "pdflib_version");
    CHECK(pdflib_backend_version() && *pdflib_backend_version(),
          "pdflib_backend_version");
    CHECK(pdf_dll_version() && *pdf_dll_version(), "pdf_dll_version");
    CHECK(pdf_dll_backends() && *pdf_dll_backends(), "pdf_dll_backends");
    printf("  [i]    %s\n", pdf_dll_version());
    printf("  [i]    engines: %s\n", pdf_dll_backends());

    CHECK(pdf_dll_has_backend(PDF_BACKEND_LIBHARU) == PDF_OK,
          "pdf_dll_has_backend(libharu)");
    CHECK(pdf_dll_has_backend(PDF_BACKEND_PODOFO) == PDF_OK,
          "pdf_dll_has_backend(podofo)");
    CHECK(pdf_dll_has_backend(999) == PDF_ERR_BACKEND_NOT_FOUND,
          "pdf_dll_has_backend(999) -> NOT_FOUND");

    CHECK(pdflib_has_backend(PDF_BACKEND_LIBHARU) != 0,
          "pdflib_has_backend(libharu)");
    CHECK(pdflib_has_backend(PDF_BACKEND_PODOFO) != 0,
          "pdflib_has_backend(podofo)");
    CHECK(pdflib_has_backend((pdf_backend)999) == 0,
          "pdflib_has_backend(999) == 0");

    /* set/get must round-trip, and a bogus id must be rejected */
    CHECK(pdflib_set_backend(PDF_BACKEND_LIBHARU) == PDF_OK,
          "set_backend(libharu)");
    CHECK(pdflib_get_backend() == PDF_BACKEND_LIBHARU,
          "get_backend echoes libharu");
    CHECK(pdflib_set_backend(PDF_BACKEND_PODOFO) == PDF_OK,
          "set_backend(podofo)");
    CHECK(pdflib_get_backend() == PDF_BACKEND_PODOFO,
          "get_backend echoes podofo");
    CHECK(pdflib_set_backend(PDF_BACKEND_AUTO) == PDF_OK, "set_backend(auto)");
    CHECK(pdflib_set_backend((pdf_backend)999) == PDF_ERR_BACKEND_NOT_FOUND,
          "set_backend(999) -> NOT_FOUND");

    /* every defined status code must carry a non-empty description */
    for (i = PDF_OK; i <= PDF_ERR_IO; i++)
    {
        const char *s = pdf_dll_status_string((pdf_status)i);
        if (!s || !*s)
            all_text = 0;
    }
    CHECK(all_text, "pdf_dll_status_string covers all 9 codes");

    /* an unknown code must still return something printable */
    CHECK(pdf_dll_status_string((pdf_status)1234) != NULL,
          "pdf_dll_status_string(unknown) still returns text");

    /* pdflib_last_error on a NULL handle must not crash */
    CHECK(pdflib_last_error(NULL) != NULL, "pdflib_last_error(NULL) safe");
}

/* ------------------------------------------------------------------ */
/* Error paths: bad input must produce a status code, never a crash    */
/* ------------------------------------------------------------------ */

static void test_error_paths(void)
{
    pdf_doc doc = NULL;
    pdf_page page = NULL;
    pdf_font font = NULL;
    pdf_image image = NULL;
    pdf_annot annot = NULL;
    pdf_outline outline = NULL;
    size_t count = 0;
    unsigned char *buf = NULL;
    size_t len = 0;
    double d = 0;
    pdf_color c;

    memset(&c, 0, sizeof(c));
    c.kind = PDF_COLOR_RGB;
    c.c1 = 1; c.c2 = 0; c.c3 = 0; c.alpha = 1;

    printf("[test] error paths (NULL / bad arguments)\n");

    /* NULL document handles */
    CHECK(pdf_doc_free(NULL) == PDF_ERR_INVALID_HANDLE, "doc_free(NULL)");
    CHECK(pdf_doc_finish(NULL) == PDF_ERR_INVALID_HANDLE, "doc_finish(NULL)");
    CHECK(pdf_doc_page_count(NULL, &count) == PDF_ERR_INVALID_HANDLE,
          "doc_page_count(NULL)");
    CHECK(pdf_doc_page_count(NULL, NULL) == PDF_ERR_INVALID_HANDLE,
          "doc_page_count(NULL, NULL)");
    CHECK(pdf_page_add(NULL, PDF_PAGE_A4, PDF_ORIENTATION_PORTRAIT, &page)
          == PDF_ERR_INVALID_HANDLE, "page_add(NULL)");
    CHECK(pdf_page_add_custom(NULL, 100, 100, &page) == PDF_ERR_INVALID_HANDLE,
          "page_add_custom(NULL)");
    CHECK(pdf_font_get_std(NULL, PDF_STD_FONT_HELVETICA, &font)
          == PDF_ERR_INVALID_HANDLE, "font_get_std(NULL)");
    CHECK(pdf_font_load_ttf(NULL, "x.ttf", 1, &font)
          == PDF_ERR_INVALID_HANDLE, "font_load_ttf(NULL)");
    CHECK(pdf_font_load_ttf_memory(NULL, sample_png, 4, 1, &font)
          == PDF_ERR_INVALID_HANDLE, "font_load_ttf_memory(NULL)");
    CHECK(pdf_font_find_system(NULL, "Arial", 0, 0, &font)
          == PDF_ERR_INVALID_HANDLE, "font_find_system(NULL)");
    CHECK(pdf_doc_save_to_file(NULL, "x.pdf") == PDF_ERR_INVALID_HANDLE,
          "save_to_file(NULL)");
    CHECK(pdf_doc_save_to_memory(NULL, NULL, &len) == PDF_ERR_INVALID_HANDLE,
          "save_to_memory(NULL)");
    CHECK(pdf_doc_save_to_stream(NULL, NULL, NULL) == PDF_ERR_INVALID_HANDLE,
          "save_to_stream(NULL)");
    CHECK(pdf_doc_save_to_handle(NULL, (void *)1) == PDF_ERR_INVALID_HANDLE,
          "save_to_handle(NULL)");
    CHECK(pdf_doc_set_info(NULL, PDF_INFO_TITLE, "x")
          == PDF_ERR_INVALID_HANDLE, "set_info(NULL)");
    CHECK(pdf_doc_get_info(NULL, PDF_INFO_TITLE, NULL, &len)
          == PDF_ERR_INVALID_HANDLE, "get_info(NULL)");
    CHECK(pdf_doc_set_compression(NULL, PDF_COMPRESS_ALL)
          == PDF_ERR_INVALID_HANDLE, "set_compression(NULL)");
    CHECK(pdf_doc_set_encryption(NULL, PDF_ENCRYPT_NONE, NULL, NULL, 0)
          == PDF_ERR_INVALID_HANDLE, "set_encryption(NULL)");
    CHECK(pdf_doc_set_page_mode_use_outlines(NULL, 1)
          == PDF_ERR_INVALID_HANDLE, "set_page_mode_use_outlines(NULL)");
    CHECK(pdf_image_load_file(NULL, "x.png", &image)
          == PDF_ERR_INVALID_HANDLE, "image_load_file(NULL)");
    CHECK(pdf_image_load_memory(NULL, sample_png, SAMPLE_PNG_LEN, &image)
          == PDF_ERR_INVALID_HANDLE, "image_load_memory(NULL)");
    CHECK(pdf_outline_add(NULL, NULL, "x", NULL, &outline)
          == PDF_ERR_INVALID_HANDLE, "outline_add(NULL)");

    /* NULL child handles */
    CHECK(pdf_font_free(NULL) == PDF_ERR_INVALID_HANDLE, "font_free(NULL)");
    CHECK(pdf_image_free(NULL) == PDF_ERR_INVALID_HANDLE, "image_free(NULL)");
    CHECK(pdf_annot_free(NULL) == PDF_ERR_INVALID_HANDLE, "annot_free(NULL)");
    CHECK(pdf_outline_free(NULL) == PDF_ERR_INVALID_HANDLE, "outline_free(NULL)");
    CHECK(pdf_outline_set_open(NULL, 1) == PDF_ERR_INVALID_HANDLE,
          "outline_set_open(NULL)");
    CHECK(pdf_page_get_size(NULL, &d, &d) == PDF_ERR_INVALID_HANDLE,
          "page_get_size(NULL)");
    CHECK(pdf_page_set_rotation(NULL, 90) == PDF_ERR_INVALID_HANDLE,
          "page_set_rotation(NULL)");
    CHECK(pdf_image_get_size(NULL, &count, &count) == PDF_ERR_INVALID_HANDLE,
          "image_get_size(NULL)");
    CHECK(pdf_gstate_save(NULL) == PDF_ERR_INVALID_HANDLE, "gstate_save(NULL)");
    CHECK(pdf_gstate_restore(NULL) == PDF_ERR_INVALID_HANDLE,
          "gstate_restore(NULL)");
    CHECK(pdf_set_transform(NULL, 1, 0, 0, 1, 0, 0) == PDF_ERR_INVALID_HANDLE,
          "set_transform(NULL)");
    CHECK(pdf_set_line_width(NULL, 1) == PDF_ERR_INVALID_HANDLE,
          "set_line_width(NULL)");
    CHECK(pdf_set_line_cap(NULL, PDF_LINE_CAP_BUTT) == PDF_ERR_INVALID_HANDLE,
          "set_line_cap(NULL)");
    CHECK(pdf_set_line_join(NULL, PDF_LINE_JOIN_MITER) == PDF_ERR_INVALID_HANDLE,
          "set_line_join(NULL)");
    CHECK(pdf_set_miter_limit(NULL, 1) == PDF_ERR_INVALID_HANDLE,
          "set_miter_limit(NULL)");
    CHECK(pdf_set_line_dash(NULL, NULL, 0, 0) == PDF_ERR_INVALID_HANDLE,
          "set_line_dash(NULL)");
    CHECK(pdf_set_fill_color(NULL, &c) == PDF_ERR_INVALID_HANDLE,
          "set_fill_color(NULL)");
    CHECK(pdf_set_stroke_color(NULL, &c) == PDF_ERR_INVALID_HANDLE,
          "set_stroke_color(NULL)");
    CHECK(pdf_set_opacity(NULL, 1, 1) == PDF_ERR_INVALID_HANDLE,
          "set_opacity(NULL)");
    CHECK(pdf_set_blend_mode(NULL, PDF_BLEND_NORMAL) == PDF_ERR_INVALID_HANDLE,
          "set_blend_mode(NULL)");
    CHECK(pdf_set_text_render_mode(NULL, PDF_TEXT_RENDER_FILL)
          == PDF_ERR_INVALID_HANDLE, "set_text_render_mode(NULL)");
    CHECK(pdf_path_move_to(NULL, 0, 0) == PDF_ERR_INVALID_HANDLE,
          "path_move_to(NULL)");
    CHECK(pdf_path_line_to(NULL, 0, 0) == PDF_ERR_INVALID_HANDLE,
          "path_line_to(NULL)");
    CHECK(pdf_path_curve_to(NULL, 0, 0, 0, 0, 0, 0) == PDF_ERR_INVALID_HANDLE,
          "path_curve_to(NULL)");
    CHECK(pdf_path_close(NULL) == PDF_ERR_INVALID_HANDLE, "path_close(NULL)");
    CHECK(pdf_path_rectangle(NULL, NULL) == PDF_ERR_INVALID_HANDLE,
          "path_rectangle(NULL)");
    CHECK(pdf_path_circle(NULL, 0, 0, 1) == PDF_ERR_INVALID_HANDLE,
          "path_circle(NULL)");
    CHECK(pdf_path_ellipse(NULL, 0, 0, 1, 1) == PDF_ERR_INVALID_HANDLE,
          "path_ellipse(NULL)");
    CHECK(pdf_path_arc(NULL, 0, 0, 1, 0, 1, 0) == PDF_ERR_INVALID_HANDLE,
          "path_arc(NULL)");
    CHECK(pdf_path_polygon(NULL, NULL, 0) == PDF_ERR_INVALID_HANDLE,
          "path_polygon(NULL)");
    CHECK(pdf_path_paint(NULL, PDF_PATH_FILL) == PDF_ERR_INVALID_HANDLE,
          "path_paint(NULL)");
    CHECK(pdf_path_discard(NULL) == PDF_ERR_INVALID_HANDLE,
          "path_discard(NULL)");
    CHECK(pdf_page_draw_rect(NULL, NULL, PDF_PATH_FILL)
          == PDF_ERR_INVALID_HANDLE, "page_draw_rect(NULL)");
    CHECK(pdf_page_draw_line(NULL, 0, 0, 1, 1) == PDF_ERR_INVALID_HANDLE,
          "page_draw_line(NULL)");
    CHECK(pdf_page_draw_text(NULL, NULL, "x", 0, 0, 10)
          == PDF_ERR_INVALID_HANDLE, "page_draw_text(NULL)");
    CHECK(pdf_page_draw_text_aligned(NULL, NULL, "x", 0, 0, 10, 100,
                                     PDF_ALIGN_LEFT)
          == PDF_ERR_INVALID_HANDLE, "page_draw_text_aligned(NULL)");
    CHECK(pdf_page_draw_text_wrapped(NULL, NULL, "x", 0, 0, 100, 100, 10, 2,
                                     PDF_ALIGN_LEFT)
          == PDF_ERR_INVALID_HANDLE, "page_draw_text_wrapped(NULL)");
    CHECK(pdf_page_draw_image(NULL, NULL, NULL) == PDF_ERR_INVALID_HANDLE,
          "page_draw_image(NULL)");
    CHECK(pdf_font_text_width(NULL, "x", 10, &d) == PDF_ERR_INVALID_HANDLE,
          "font_text_width(NULL)");
    CHECK(pdf_font_ascent(NULL, 10, &d) == PDF_ERR_INVALID_HANDLE,
          "font_ascent(NULL)");
    CHECK(pdf_font_descent(NULL, 10, &d) == PDF_ERR_INVALID_HANDLE,
          "font_descent(NULL)");
    CHECK(pdf_annot_create(NULL, PDF_ANNOT_TEXT, NULL, NULL, &annot)
          == PDF_ERR_INVALID_HANDLE, "annot_create(NULL)");
    CHECK(pdf_annot_link_to_page(NULL, NULL, "x") == PDF_ERR_INVALID_HANDLE,
          "annot_link_to_page(NULL)");
    CHECK(pdf_annot_set_color(NULL, &c) == PDF_ERR_INVALID_HANDLE,
          "annot_set_color(NULL)");
    CHECK(pdf_annot_set_icon(NULL, PDF_ANNOT_ICON_NOTE)
          == PDF_ERR_INVALID_HANDLE, "annot_set_icon(NULL)");
    CHECK(pdf_annot_set_opacity(NULL, 0.5) == PDF_ERR_INVALID_HANDLE,
          "annot_set_opacity(NULL)");
    CHECK(pdf_annot_set_author(NULL, "x") == PDF_ERR_INVALID_HANDLE,
          "annot_set_author(NULL)");
    CHECK(pdf_annot_set_subject(NULL, "x") == PDF_ERR_INVALID_HANDLE,
          "annot_set_subject(NULL)");
    CHECK(pdf_annot_set_open(NULL, 1) == PDF_ERR_INVALID_HANDLE,
          "annot_set_open(NULL)");
    CHECK(pdf_doc_create_streamed(NULL, NULL, &doc) == PDF_ERR_INVALID_PARAM,
          "create_streamed(NULL cb)");
    CHECK(pdf_doc_create_streamed_to_handle(NULL, &doc)
          == PDF_ERR_INVALID_PARAM, "create_streamed_to_handle(NULL)");
    CHECK(pdf_doc_create_streamed_to_file(NULL, &doc)
          == PDF_ERR_INVALID_PARAM, "create_streamed_to_file(NULL)");
    CHECK(pdf_doc_create(NULL) == PDF_ERR_INVALID_PARAM,
          "doc_create(NULL out)");

    /* bad arguments on a live document */
    CHECK(pdf_doc_create(&doc) == PDF_OK, "doc_create for arg tests");
    CHECK(pdf_doc_save_to_file(doc, NULL) == PDF_ERR_INVALID_PARAM,
          "save_to_file(doc, NULL)");
    CHECK(pdf_doc_save_to_file(doc, "") == PDF_ERR_INVALID_PARAM,
          "save_to_file(doc, \"\")");
    CHECK(pdf_doc_save_to_memory(doc, NULL, NULL) == PDF_ERR_INVALID_PARAM,
          "save_to_memory(NULL size)");
    CHECK(pdf_doc_save_to_stream(doc, NULL, NULL) == PDF_ERR_INVALID_PARAM,
          "save_to_stream(NULL cb)");
    CHECK(pdf_doc_save_to_handle(doc, NULL) == PDF_ERR_INVALID_PARAM,
          "save_to_handle(doc, NULL)");
    CHECK(pdf_doc_get_info(doc, PDF_INFO_TITLE, NULL, NULL)
          == PDF_ERR_INVALID_PARAM, "get_info(NULL len)");

    /* A NULL value clears the entry rather than being rejected - both
     * engines coerce it to the empty string. Assert the observable
     * behaviour, not just a status code. */
    {
        char cleared[16];
        size_t clen = sizeof(cleared);

        CHECK(pdf_doc_set_info(doc, PDF_INFO_TITLE, NULL) == PDF_OK,
              "set_info(NULL) accepted");
        memset(cleared, 0x7f, sizeof(cleared));
        CHECK(pdf_doc_get_info(doc, PDF_INFO_TITLE, cleared, &clen) == PDF_OK
              && cleared[0] == '\0',
              "set_info(NULL) cleared the entry");
        CHECK(pdf_doc_set_info(doc, PDF_INFO_TITLE, "restored") == PDF_OK,
              "set_info(title) restored");
    }

    /* A missing file has to fail loudly. Which code the engine reports is
     * engine-specific (PoDoFo maps the loader exception, libharu its own
     * error), so assert "not OK" plus a usable message. */
    {
        pdf_status rc = pdf_image_load_file(doc, "no-such-file.png", &image);
        const char *why = pdflib_last_error(doc);
        CHECK(rc != PDF_OK, "image_load_file(missing) fails");
        printf("  [i]    image_load_file(missing) -> %s (%s)\n",
               pdf_dll_status_string(rc), why ? why : "-");
    }
    CHECK(pdf_font_load_ttf(doc, "no-such-font.ttf", 1, &font) != PDF_OK,
          "font_load_ttf(missing) fails cleanly");
    CHECK(pdf_doc_set_encryption(doc, PDF_ENCRYPT_RC4_128, NULL, NULL, 0)
          == PDF_ERR_INVALID_PARAM, "set_encryption without passwords");

    /* saving a streamed document must be refused, not silently ignored */
    {
        pdf_doc sdoc = NULL;
        pdf_status rc = pdf_doc_create_streamed_to_file(
            "apitest-streamed-reject.pdf", &sdoc);
        if (rc == PDF_OK)
        {
            CHECK(pdf_doc_save_to_file(sdoc, "apitest-nope.pdf")
                  == PDF_ERR_BACKEND_FAILED,
                  "save_to_file on a streamed doc is refused");
            CHECK(pdf_doc_save_to_handle(sdoc, (void *)1)
                  == PDF_ERR_BACKEND_FAILED,
                  "save_to_handle on a streamed doc is refused");
            pdf_doc_free(sdoc);
        }
        else
        {
            printf("  [i]    no streamed writer on this engine; "
                   "streamed-reject checks skipped\n");
        }
        remove("apitest-streamed-reject.pdf");
        remove("apitest-nope.pdf");
    }

    /* a document with no pages still has to serialize */
    {
        pdf_doc empty = NULL;
        CHECK(pdf_doc_create(&empty) == PDF_OK, "empty doc create");
        len = 0;
        CHECK(pdf_doc_save_to_memory(empty, NULL, &len) == PDF_OK && len > 100,
              "empty doc serializes");
        buf = (unsigned char *)malloc(len ? len : 1);
        CHECK(pdf_doc_save_to_memory(empty, buf, &len) == PDF_OK,
              "empty doc memory fill");
        free(buf);
        CHECK(pdf_doc_page_count(empty, &count) == PDF_OK && count == 0,
              "empty doc page count == 0");
        pdf_doc_free(empty);
    }
}

/* ------------------------------------------------------------------ */
/* Full document: every drawing feature, then every save mode          */
/* ------------------------------------------------------------------ */

static int find_system_ttf(char *out, size_t n)
{
    static const char *candidates[] = {
        "C:/Windows/Fonts/arial.ttf",
        "C:/Windows/Fonts/segoeui.ttf",
        "C:/Windows/Fonts/times.ttf",
        "C:/Windows/Fonts/cour.ttf",
        NULL
    };
    int i;

    for (i = 0; candidates[i]; i++)
    {
        FILE *f = fopen(candidates[i], "rb");
        if (f) { fclose(f); snprintf(out, n, "%s", candidates[i]); return 1; }
    }
    return 0;
}

/* A CJK-capable face, so the Type0 / CIDFont path gets exercised. Only a
 * composite font can carry the glyph repertoire, and Windows ships these
 * as both .ttc collections and .ttf singles. */
static int find_cjk_ttf(char *out, size_t n)
{
    static const char *candidates[] = {
        "C:/Windows/Fonts/msyh.ttc",
        "C:/Windows/Fonts/simsun.ttc",
        "C:/Windows/Fonts/simhei.ttf",
        "C:/Windows/Fonts/msyh.ttf",
        "/System/Library/Fonts/PingFang.ttc",
        "/usr/share/fonts/truetype/noto/NotoSansCJK-Regular.ttc",
        NULL
    };
    int i;

    for (i = 0; candidates[i]; i++)
    {
        FILE *f = fopen(candidates[i], "rb");
        if (f) { fclose(f); snprintf(out, n, "%s", candidates[i]); return 1; }
    }
    return 0;
}

static void test_images(pdf_doc doc, pdf_page page)
{
    pdf_image img = NULL;
    size_t w = 0, h = 0;
    pdf_rect dest;
    FILE *f;

    /* materialise the two samples so the file loader has something real */
    f = fopen("apitest-sample.png", "wb");
    CHECK(f != NULL, "write PNG sample");
    if (f) { fwrite(sample_png, 1, SAMPLE_PNG_LEN, f); fclose(f); }
    f = fopen("apitest-sample.jpg", "wb");
    CHECK(f != NULL, "write JPEG sample");
    if (f) { fwrite(sample_jpeg, 1, SAMPLE_JPEG_LEN, f); fclose(f); }

    /* --- PNG: supported by both engines --- */
    CHECK(pdf_image_load_file(doc, "apitest-sample.png", &img) == PDF_OK,
          "image_load_file(png)");
    if (img)
    {
        CHECK(pdf_image_get_size(img, &w, &h) == PDF_OK && w == 16 && h == 16,
              "image_get_size(png) == 16x16");
        dest.x = 60; dest.y = 300; dest.w = 64; dest.h = 64;
        CHECK(pdf_page_draw_image(page, img, &dest) == PDF_OK,
              "page_draw_image(png)");
        CHECK(pdf_image_free(img) == PDF_OK, "image_free(png)");
    }

    img = NULL;
    CHECK(pdf_image_load_memory(doc, sample_png, SAMPLE_PNG_LEN, &img) == PDF_OK,
          "image_load_memory(png)");
    if (img)
    {
        CHECK(pdf_image_get_size(img, &w, &h) == PDF_OK && w == 16 && h == 16,
              "image_get_size(memory png) == 16x16");
        dest.x = 140; dest.y = 300; dest.w = 48; dest.h = 48;
        CHECK(pdf_page_draw_image(page, img, &dest) == PDF_OK,
              "page_draw_image(memory png)");
        CHECK(pdf_image_free(img) == PDF_OK, "image_free(memory png)");
    }

    /* --- JPEG: this build ships PoDoFo with libjpeg deliberately off
     * (see conanfile.py), so a clean refusal is the expected answer. --- */
    img = NULL;
    {
        pdf_status rc = pdf_image_load_file(doc, "apitest-sample.jpg", &img);
        CHECK(rc == PDF_OK || rc == PDF_ERR_BACKEND_UNSUPPORTED ||
              rc == PDF_ERR_BACKEND_FAILED,
              "image_load_file(jpeg) handled");
        if (rc == PDF_OK && img)
        {
            dest.x = 200; dest.y = 300; dest.w = 48; dest.h = 48;
            CHECK(pdf_image_get_size(img, &w, &h) == PDF_OK,
                  "image_get_size(jpeg)");
            CHECK(pdf_page_draw_image(page, img, &dest) == PDF_OK,
                  "page_draw_image(jpeg)");
            CHECK(pdf_image_free(img) == PDF_OK, "image_free(jpeg)");
        }
        else
        {
            printf("  [i]    jpeg: no codec in this build (libjpeg off) - "
                   "path exercised, %s\n", pdf_dll_status_string(rc));
        }
    }

    img = NULL;
    {
        pdf_status rc = pdf_image_load_memory(doc, sample_jpeg,
                                              SAMPLE_JPEG_LEN, &img);
        CHECK(rc == PDF_OK || rc == PDF_ERR_BACKEND_UNSUPPORTED ||
              rc == PDF_ERR_BACKEND_FAILED,
              "image_load_memory(jpeg) handled");
        if (rc == PDF_OK && img)
            CHECK(pdf_image_free(img) == PDF_OK, "image_free(memory jpeg)");
    }
}

static void test_annotations(pdf_page page, pdf_page other)
{
    static const pdf_annot_type types[] = {
        PDF_ANNOT_TEXT, PDF_ANNOT_HIGHLIGHT, PDF_ANNOT_UNDERLINE,
        PDF_ANNOT_SQUIGGLY, PDF_ANNOT_STRIKEOUT, PDF_ANNOT_SQUARE,
        PDF_ANNOT_CIRCLE, PDF_ANNOT_LINK_URI, PDF_ANNOT_LINK_GOTO
    };
    pdf_rect r;
    pdf_color c;
    int i;

    memset(&c, 0, sizeof(c));
    c.kind = PDF_COLOR_RGB;
    c.c1 = 0.9; c.c2 = 0.2; c.c3 = 0.2; c.alpha = 1;

    for (i = 0; i < (int)(sizeof(types) / sizeof(types[0])); i++)
    {
        pdf_annot a = NULL;
        pdf_status rc;
        int deferred;   /* GOTO link: no dictionary until link_to_page */

        r.x = 60; r.y = 200 + i * 12; r.w = 120; r.h = 10;
        deferred = (types[i] == PDF_ANNOT_LINK_GOTO);

        if (types[i] == PDF_ANNOT_LINK_URI)
            rc = pdf_annot_create(page, types[i], &r, "https://example.com", &a);
        else if (deferred)
            rc = pdf_annot_create(page, types[i], &r, NULL, &a);
        else
            rc = pdf_annot_create(page, types[i], &r, "apitest note", &a);

        CHECK(rc == PDF_OK, "annot_create");
        if (rc != PDF_OK || !a)
            continue;

        /* Properties that land in the annotation dictionary. A deferred
         * GOTO link has none yet, so libharu declines and PoDoFo accepts. */
        if (deferred)
        {
            CHECK_DECLINED(pdf_annot_set_color(a, &c), "set_color(deferred)");
            CHECK_DECLINED(pdf_annot_set_opacity(a, 0.75),
                           "set_opacity(deferred)");
            CHECK_DECLINED(pdf_annot_set_author(a, "pdflib apitest"),
                           "set_author(deferred)");
            CHECK_DECLINED(pdf_annot_set_subject(a, "coverage"),
                           "set_subject(deferred)");
        }
        else
        {
            CHECK(pdf_annot_set_color(a, &c) == PDF_OK, "annot_set_color");
            CHECK(pdf_annot_set_opacity(a, 0.75) == PDF_OK,
                  "annot_set_opacity");
            CHECK(pdf_annot_set_author(a, "pdflib apitest") == PDF_OK,
                  "annot_set_author");
            CHECK(pdf_annot_set_subject(a, "coverage") == PDF_OK,
                  "annot_set_subject");
        }

        /* /Name and /Open only exist on a text annotation. */
        if (types[i] == PDF_ANNOT_TEXT)
        {
            CHECK(pdf_annot_set_icon(a, PDF_ANNOT_ICON_NOTE) == PDF_OK,
                  "annot_set_icon(text)");
            CHECK(pdf_annot_set_open(a, 1) == PDF_OK, "annot_set_open(text)");
        }
        else
        {
            CHECK_DECLINED(pdf_annot_set_icon(a, PDF_ANNOT_ICON_NOTE),
                           "annot_set_icon(non-text)");
            CHECK_DECLINED(pdf_annot_set_open(a, 1), "annot_set_open(non-text)");
        }

        if (types[i] == PDF_ANNOT_LINK_URI)
        {
            /* annot_create already materialised the URI action, so a
             * second call is a no-op on PoDoFo and refused by libharu. */
            CHECK_DECLINED(pdf_annot_link_to_page(a, NULL,
                                                  "https://example.com"),
                           "annot_link_to_page(uri)");
        }
        else if (deferred)
        {
            CHECK(pdf_annot_link_to_page(a, &other, NULL) == PDF_OK,
                  "annot_link_to_page(goto)");
            /* The link now exists, so the dictionary-backed setters have
             * something to write into on both engines. */
            CHECK(pdf_annot_set_color(a, &c) == PDF_OK,
                  "set_color after link_to_page");
        }

        CHECK(pdf_annot_free(a) == PDF_OK, "annot_free");
    }

    /* every icon value must be accepted */
    {
        pdf_annot a = NULL;
        r.x = 200; r.y = 200; r.w = 60; r.h = 12;
        if (pdf_annot_create(page, PDF_ANNOT_TEXT, &r, "icons", &a) == PDF_OK)
        {
            for (i = PDF_ANNOT_ICON_COMMENT; i <= PDF_ANNOT_ICON_INSERT; i++)
                CHECK(pdf_annot_set_icon(a, (pdf_annot_icon)i) == PDF_OK,
                      "annot_set_icon(each)");
            pdf_annot_free(a);
        }
    }
}

static void test_outlines(pdf_doc doc, pdf_page page)
{
    pdf_outline top = NULL, child = NULL;

    CHECK(pdf_outline_add(doc, NULL, "Chapter 1", &page, &top) == PDF_OK,
          "outline_add(top level)");
    if (top)
    {
        CHECK(pdf_outline_set_open(top, 1) == PDF_OK, "outline_set_open");
        CHECK(pdf_outline_add(doc, top, "Section 1.1", &page, &child) == PDF_OK,
              "outline_add(nested)");
        if (child)
            CHECK(pdf_outline_free(child) == PDF_OK, "outline_free(nested)");
        CHECK(pdf_outline_free(top) == PDF_OK, "outline_free(top)");
    }

    /* a title-only item (no jump target) is legal too */
    {
        pdf_outline only = NULL;
        CHECK(pdf_outline_add(doc, NULL, "Appendix (no target)", NULL, &only)
              == PDF_OK, "outline_add(no destination)");
        if (only)
            CHECK(pdf_outline_free(only) == PDF_OK, "outline_free(title only)");
    }
}

static void test_document(int backend, const char *name)
{
    pdf_doc doc = NULL;
    pdf_page page = NULL, page2 = NULL;
    pdf_font font = NULL, ttf = NULL;
    pdf_color c;
    pdf_rect r;
    pdf_point poly[5];
    double dash[2];
    char path[128], label[64];
    size_t count = 0, len = 0;
    unsigned char *buf = NULL;
    double d = 0;
    char ttf_path[260];
    int i;

    printf("[test] full document (%s)\n", name);

    if (pdflib_set_backend((pdf_backend)backend) != PDF_OK)
    {
        printf("  [i]    backend %s unavailable, skipping\n", name);
        return;
    }

    CHECK(pdf_doc_create(&doc) == PDF_OK, "doc_create");

    /* --- info: write all six keys, read one back --- */
    CHECK(pdf_doc_set_info(doc, PDF_INFO_TITLE, "pdflib apitest") == PDF_OK,
          "set_info(title)");
    CHECK(pdf_doc_set_info(doc, PDF_INFO_AUTHOR, "apitest") == PDF_OK,
          "set_info(author)");
    CHECK(pdf_doc_set_info(doc, PDF_INFO_SUBJECT, "full API coverage") == PDF_OK,
          "set_info(subject)");
    CHECK(pdf_doc_set_info(doc, PDF_INFO_KEYWORDS, "pdflib,test,coverage")
          == PDF_OK, "set_info(keywords)");
    CHECK(pdf_doc_set_info(doc, PDF_INFO_CREATOR, "apitest.c") == PDF_OK,
          "set_info(creator)");
    CHECK(pdf_doc_set_info(doc, PDF_INFO_PRODUCER, "pdflib") == PDF_OK,
          "set_info(producer)");

    len = 0;
    CHECK(pdf_doc_get_info(doc, PDF_INFO_TITLE, NULL, &len) == PDF_OK &&
          len > 1, "get_info size query");
    if (len > 1)
    {
        char *title = (char *)malloc(len);
        CHECK(pdf_doc_get_info(doc, PDF_INFO_TITLE, title, &len) == PDF_OK &&
              strcmp(title, "pdflib apitest") == 0, "get_info round trip");
        free(title);
    }

    /* --- security / viewer preferences --- */
    CHECK(pdf_doc_set_compression(doc, PDF_COMPRESS_ALL) == PDF_OK,
          "set_compression(ALL)");
    CHECK(pdf_doc_set_compression(doc, PDF_COMPRESS_NONE) == PDF_OK,
          "set_compression(NONE)");
    CHECK(pdf_doc_set_page_mode_use_outlines(doc, 1) == PDF_OK,
          "set_page_mode_use_outlines(on)");
    CHECK(pdf_doc_set_page_mode_use_outlines(doc, 0) == PDF_OK,
          "set_page_mode_use_outlines(off)");
    CHECK(pdf_doc_set_encryption(doc, PDF_ENCRYPT_NONE, NULL, NULL, 0) == PDF_OK,
          "set_encryption(NONE)");

    /* --- pages --- */
    CHECK(pdf_page_add(doc, PDF_PAGE_A4, PDF_ORIENTATION_PORTRAIT, &page)
          == PDF_OK, "page_add(A4 portrait)");
    CHECK(pdf_page_add(doc, PDF_PAGE_LETTER, PDF_ORIENTATION_LANDSCAPE, &page2)
          == PDF_OK, "page_add(Letter landscape)");
    {
        pdf_page custom = NULL;
        CHECK(pdf_page_add_custom(doc, 300, 400, &custom) == PDF_OK,
              "page_add_custom(300x400)");
        if (custom)
        {
            double w = 0, h = 0;
            CHECK(pdf_page_get_size(custom, &w, &h) == PDF_OK &&
                  w == 300 && h == 400, "page_get_size(custom)");
            CHECK(pdf_page_set_rotation(custom, 90) == PDF_OK,
                  "page_set_rotation(90)");
            CHECK(pdf_page_set_rotation(custom, 0) == PDF_OK,
                  "page_set_rotation(0)");
        }
    }
    CHECK(pdf_doc_page_count(doc, &count) == PDF_OK && count == 3,
          "doc_page_count == 3");
    {
        double w = 0, h = 0;
        CHECK(pdf_page_get_size(page, &w, &h) == PDF_OK && w > 500 && h > 800,
              "page_get_size(A4)");
    }

    /* --- fonts --- */
    CHECK(pdf_font_get_std(doc, PDF_STD_FONT_HELVETICA, &font) == PDF_OK,
          "font_get_std(Helvetica)");
    if (font)
    {
        CHECK(pdf_font_text_width(font, "Hello", 16, &d) == PDF_OK && d > 0,
              "font_text_width");
        CHECK(pdf_font_ascent(font, 16, &d) == PDF_OK && d > 0,
              "font_ascent");
        CHECK(pdf_font_descent(font, 16, &d) == PDF_OK && d != 0,
              "font_descent");
    }

    /* a second standard font that exercises the Symbol encoding path */
    {
        pdf_font sym = NULL;
        CHECK(pdf_font_get_std(doc, PDF_STD_FONT_TIMES_BOLD, &sym) == PDF_OK,
              "font_get_std(Times-Bold)");
        if (sym)
            CHECK(pdf_font_free(sym) == PDF_OK, "font_free(std font)");
    }

    if (find_system_ttf(ttf_path, sizeof(ttf_path)))
    {
        printf("  [i]    system TTF: %s\n", ttf_path);

        CHECK(pdf_font_load_ttf(doc, ttf_path, 1, &ttf) == PDF_OK,
              "font_load_ttf(file)");
        if (ttf)
        {
            CHECK(pdf_page_draw_text(page, ttf, "embedded TTF", 60, 680, 14)
                  == PDF_OK, "draw text with file TTF");
            CHECK(pdf_font_free(ttf) == PDF_OK, "font_free(file TTF)");
        }

        /* the same font again, this time straight from memory */
        {
            unsigned char *blob = NULL;
            long n = slurp(ttf_path, &blob);
            pdf_font memfont = NULL;
            CHECK(n > 0, "read TTF into memory");
            if (n > 0)
            {
                pdf_status rc = pdf_font_load_ttf_memory(doc, blob,
                                                         (size_t)n, 1, &memfont);
                CHECK_MAYBE(rc, "font_load_ttf_memory");
                if (rc == PDF_OK && memfont)
                {
                    CHECK(pdf_page_draw_text(page, memfont,
                                             "in-memory TTF", 60, 660, 14)
                          == PDF_OK, "draw text with memory TTF");
                    CHECK(pdf_font_free(memfont) == PDF_OK, "font_free(mem TTF)");
                }
            }
            free(blob);
        }

        {
            pdf_font sysfont = NULL;
            pdf_status rc = pdf_font_find_system(doc, "Arial", 0, 0, &sysfont);
            CHECK(status_is_valid(rc), "font_find_system returns a status");
            if (rc == PDF_OK && sysfont)
            {
                CHECK(pdf_page_draw_text(page, sysfont, "system font",
                                         60, 640, 14) == PDF_OK,
                      "draw text with system font");
                CHECK(pdf_font_free(sysfont) == PDF_OK,
                      "font_free(system font)");
            }
            else
            {
                /* PoDoFo resolves system families through fontconfig, which
                 * this build does not link (PODOFO_HAVE_FONTCONFIG is
                 * undefined - see podofo_config.h); libharu maps the common
                 * families onto the base-14 fonts and does resolve. Both
                 * outcomes are correct, so only the status is asserted. */
                printf("  [i]    find_system(\"Arial\") -> %s (acceptable)\n",
                       pdf_dll_status_string(rc));
            }
        }
    }
    else
    {
        printf("  [i]    no system TTF found; TTF paths not exercised\n");
    }

    /* --- text --- */
    if (font)
    {
        CHECK(pdf_page_draw_text(page, font, "draw_text", 60, 780, 16) == PDF_OK,
              "page_draw_text");
        CHECK(pdf_page_draw_text_aligned(page, font, "aligned centre",
                                         60, 760, 14, 400, PDF_ALIGN_CENTER)
              == PDF_OK, "page_draw_text_aligned");
        CHECK(pdf_page_draw_text_aligned(page, font, "aligned right",
                                         60, 740, 14, 400, PDF_ALIGN_RIGHT)
              == PDF_OK, "page_draw_text_aligned(right)");
        CHECK(pdf_page_draw_text_wrapped(page, font,
                                         "word wrapped paragraph that has to "
                                         "flow onto several lines inside the "
                                         "box because it is long enough",
                                         60, 700, 300, 80, 12, 3,
                                         PDF_ALIGN_LEFT) == PDF_OK,
              "page_draw_text_wrapped");
    }

    /* --- graphics state --- */
    CHECK(pdf_gstate_save(page) == PDF_OK, "gstate_save");
    CHECK(pdf_set_transform(page, 1, 0.1, -0.1, 1, 10, 10) == PDF_OK,
          "set_transform");
    CHECK(pdf_set_line_width(page, 1.5) == PDF_OK, "set_line_width");
    for (i = PDF_LINE_CAP_BUTT; i <= PDF_LINE_CAP_SQUARE; i++)
        CHECK(pdf_set_line_cap(page, (pdf_line_cap)i) == PDF_OK, "set_line_cap");
    for (i = PDF_LINE_JOIN_MITER; i <= PDF_LINE_JOIN_BEVEL; i++)
        CHECK(pdf_set_line_join(page, (pdf_line_join)i) == PDF_OK,
              "set_line_join");
    CHECK(pdf_set_miter_limit(page, 4) == PDF_OK, "set_miter_limit");
    dash[0] = 6; dash[1] = 3;
    CHECK(pdf_set_line_dash(page, dash, 2, 0) == PDF_OK, "set_line_dash(on)");
    CHECK(pdf_set_line_dash(page, NULL, 0, 0) == PDF_OK, "set_line_dash(off)");
    CHECK(pdf_set_opacity(page, 0.8, 0.6) == PDF_OK, "set_opacity");
    for (i = PDF_BLEND_NORMAL; i <= PDF_BLEND_EXCLUSION; i++)
        CHECK(pdf_set_blend_mode(page, (pdf_blend_mode)i) == PDF_OK,
              "set_blend_mode");
    CHECK(pdf_set_blend_mode(page, PDF_BLEND_NORMAL) == PDF_OK,
          "set_blend_mode(back to normal)");
    for (i = PDF_TEXT_RENDER_FILL; i <= PDF_TEXT_RENDER_INVISIBLE; i++)
        CHECK(pdf_set_text_render_mode(page, (pdf_text_render_mode)i) == PDF_OK,
              "set_text_render_mode");
    CHECK(pdf_set_text_render_mode(page, PDF_TEXT_RENDER_FILL) == PDF_OK,
          "set_text_render_mode(fill)");
    CHECK(pdf_gstate_restore(page) == PDF_OK, "gstate_restore");

    /* --- colours --- */
    c.kind = PDF_COLOR_GRAY; c.c1 = 0.2; c.alpha = 1;
    CHECK(pdf_set_fill_color(page, &c) == PDF_OK, "set_fill_color(gray)");
    CHECK(pdf_set_stroke_color(page, &c) == PDF_OK, "set_stroke_color(gray)");
    c.kind = PDF_COLOR_RGB; c.c1 = 0.1; c.c2 = 0.4; c.c3 = 0.9; c.alpha = 1;
    CHECK(pdf_set_fill_color(page, &c) == PDF_OK, "set_fill_color(rgb)");
    c.kind = PDF_COLOR_CMYK; c.c1 = 0.1; c.c2 = 0.7; c.c3 = 0.2; c.c4 = 0.0;
    CHECK(pdf_set_fill_color(page, &c) == PDF_OK, "set_fill_color(cmyk)");
    c.kind = PDF_COLOR_RGB; c.c1 = 0.9; c.c2 = 0.2; c.c3 = 0.2; c.alpha = 0.5;
    CHECK(pdf_set_stroke_color(page, &c) == PDF_OK, "set_stroke_color(rgb+alpha)");

    /* --- paths, all construction primitives and all paint modes --- */
    CHECK(pdf_path_move_to(page, 60, 560) == PDF_OK, "path_move_to");
    CHECK(pdf_path_line_to(page, 200, 560) == PDF_OK, "path_line_to");
    CHECK(pdf_path_curve_to(page, 220, 600, 260, 520, 300, 560) == PDF_OK,
          "path_curve_to");
    CHECK(pdf_path_line_to(page, 340, 560) == PDF_OK, "path_line_to again");
    CHECK(pdf_path_close(page) == PDF_OK, "path_close");
    CHECK(pdf_path_paint(page, PDF_PATH_STROKE) == PDF_OK,
          "path_paint(stroke)");

    r.x = 60; r.y = 480; r.w = 100; r.h = 50;
    CHECK(pdf_path_rectangle(page, &r) == PDF_OK, "path_rectangle");
    CHECK(pdf_path_paint(page, PDF_PATH_FILL) == PDF_OK, "path_paint(fill)");

    /* Painting consumes the current path (PDF 'f'/'S'/'B' all clear it),
     * so every paint mode needs its own rectangle. Reusing one rectangle
     * for four paints happens to work on PoDoFo but is refused by
     * libharu - and the second behaviour is the correct one. */
    CHECK(pdf_path_rectangle(page, &r) == PDF_OK, "path_rectangle (again)");
    CHECK(pdf_path_paint(page, PDF_PATH_FILL_STROKE) == PDF_OK,
          "path_paint(fill+stroke)");

    CHECK(pdf_path_rectangle(page, &r) == PDF_OK, "path_rectangle (3rd)");
    CHECK(pdf_path_paint(page, PDF_PATH_FILL_EVENODD) == PDF_OK,
          "path_paint(even-odd fill)");

    CHECK(pdf_path_rectangle(page, &r) == PDF_OK, "path_rectangle (4th)");
    CHECK(pdf_path_paint(page, PDF_PATH_FILL_STROKE_EVENODD) == PDF_OK,
          "path_paint(even-odd fill+stroke)");

    CHECK(pdf_path_circle(page, 220, 500, 30) == PDF_OK, "path_circle");
    CHECK(pdf_path_paint(page, PDF_PATH_FILL_STROKE) == PDF_OK,
          "paint circle");
    CHECK(pdf_path_ellipse(page, 320, 500, 45, 25) == PDF_OK, "path_ellipse");
    CHECK(pdf_path_paint(page, PDF_PATH_FILL) == PDF_OK, "paint ellipse");
    CHECK(pdf_path_arc(page, 420, 500, 40, 0.0, 3.14159, 0) == PDF_OK,
          "path_arc");
    CHECK(pdf_path_paint(page, PDF_PATH_STROKE) == PDF_OK, "paint arc");

    poly[0].x = 60;  poly[0].y = 400;
    poly[1].x = 110; poly[1].y = 440;
    poly[2].x = 160; poly[2].y = 400;
    poly[3].x = 135; poly[3].y = 350;
    poly[4].x = 85;  poly[4].y = 350;
    CHECK(pdf_path_polygon(page, poly, 5) == PDF_OK, "path_polygon");
    CHECK(pdf_path_paint(page, PDF_PATH_FILL_STROKE) == PDF_OK,
          "paint polygon");

    CHECK(pdf_path_move_to(page, 60, 320) == PDF_OK, "path_move_to (discard)");
    CHECK(pdf_path_line_to(page, 200, 320) == PDF_OK, "path_line_to (discard)");
    CHECK(pdf_path_discard(page) == PDF_OK, "path_discard");

    r.x = 220; r.y = 400; r.w = 80; r.h = 40;
    CHECK(pdf_page_draw_rect(page, &r, PDF_PATH_FILL_STROKE) == PDF_OK,
          "page_draw_rect");
    CHECK(pdf_page_draw_line(page, 60, 240, 460, 240) == PDF_OK,
          "page_draw_line");

    /* --- images --- */
    test_images(doc, page);

    /* --- annotations & outlines --- */
    test_annotations(page, page2);
    test_outlines(doc, page);

    /* --- save: all four ways, then check every artefact --- */
    path_for(path, sizeof(path), name);   /* apitest-<engine> */
    {
        char p_file[160], p_handle[160], p_stream[160], p_mem[160];
        void *h;
        FILE *f;

        snprintf(p_file, sizeof(p_file), "%s-out.pdf", path);
        snprintf(p_handle, sizeof(p_handle), "%s-handle.pdf", path);
        snprintf(p_stream, sizeof(p_stream), "%s-stream.pdf", path);
        snprintf(p_mem, sizeof(p_mem), "%s-mem.pdf", path);

        CHECK(pdf_doc_save_to_file(doc, p_file) == PDF_OK, "save_to_file");

        h = os_open_write(p_handle);
        CHECK(h != NULL, "open handle for save_to_handle");
        CHECK(pdf_doc_save_to_handle(doc, h) == PDF_OK, "save_to_handle");
        os_close(h);

        f = fopen(p_stream, "wb");
        CHECK(f != NULL, "open file for save_to_stream");
        if (f)
        {
            CHECK(pdf_doc_save_to_stream(doc, apitest_sink, f) == PDF_OK,
                  "save_to_stream");
            fclose(f);
        }

        len = 0;
        CHECK(pdf_doc_save_to_memory(doc, NULL, &len) == PDF_OK && len > 100,
              "save_to_memory size query");
        buf = (unsigned char *)malloc(len ? len : 1);
        CHECK(pdf_doc_save_to_memory(doc, buf, &len) == PDF_OK,
              "save_to_memory fill");
        f = fopen(p_mem, "wb");
        if (f)
        {
            if (buf && len)
                fwrite(buf, 1, len, f);
            fclose(f);
        }
        free(buf);
        buf = NULL;

        snprintf(label, sizeof(label), "%s/out", name);
        validate_pdf(p_file, 3, label);
        snprintf(label, sizeof(label), "%s/handle", name);
        validate_pdf(p_handle, 3, label);
        snprintf(label, sizeof(label), "%s/stream", name);
        validate_pdf(p_stream, 3, label);
        snprintf(label, sizeof(label), "%s/memory", name);
        validate_pdf(p_mem, 3, label);

        /* the four writers must agree byte for byte */
        CHECK(files_identical(p_mem, p_stream),
              "memory and stream output are byte-identical");
        CHECK(files_identical(p_mem, p_handle),
              "memory and handle output are byte-identical");

        remove(p_file);
        remove(p_handle);
        remove(p_stream);
        remove(p_mem);
    }

    CHECK(pdf_doc_free(doc) == PDF_OK, "doc_free");
    (void)font;
}

/* sink used by save_to_stream - lives in this module, so it is the
 * caller's CRT that touches the FILE*, which is the safe direction. */
static pdf_status apitest_sink(const unsigned char *data, size_t len,
                               void *userdata)
{
    FILE *f = (FILE *)userdata;

    if (len && fwrite(data, 1, len, f) != len)
        return PDF_ERR_IO;
    return PDF_OK;
}

/* ------------------------------------------------------------------ */
/* Streamed documents: the three construction entry points             */
/* ------------------------------------------------------------------ */

typedef struct sink_state
{
    FILE *f;
    size_t total;
    int calls;
    int fail_on_call;   /* 1-based; 0 = never */
} sink_state;

static pdf_status counting_sink(const unsigned char *data, size_t len, void *ud)
{
    sink_state *s = (sink_state *)ud;

    s->calls++;
    if (s->fail_on_call && s->calls >= s->fail_on_call)
        return PDF_ERR_IO;
    if (len && fwrite(data, 1, len, s->f) != len)
        return PDF_ERR_IO;
    s->total += len;
    return PDF_OK;
}

static void test_streamed(int backend, const char *name)
{
    const int pages = 9;
    char path[160], label[64];
    sink_state s;
    pdf_doc doc = NULL;
    rb rd;
    int i;

    printf("[test] streamed documents (%s)\n", name);

    if (pdflib_set_backend((pdf_backend)backend) != PDF_OK)
    {
        printf("  [i]    backend %s unavailable, skipping\n", name);
        return;
    }

    /* --- 1) callback sink --- */
    snprintf(path, sizeof(path), "apitest-streamed-%s.pdf", name);
    memset(&s, 0, sizeof(s));
    s.f = fopen(path, "wb");
    CHECK(s.f != NULL, "open streamed output");

    if (pdf_doc_create_streamed(counting_sink, &s, &doc) != PDF_OK)
    {
        printf("  [i]    %s has no native streamed writer "
               "(OK: caller falls back to create + save_to_stream)\n", name);
        fclose(s.f);
        remove(path);
    }
    else
    {
        for (i = 0; i < pages; i++)
        {
            pdf_page page = NULL;
            pdf_font font = NULL;
            char line[80];

            CHECK(pdf_page_add_custom(doc, 595, 842, &page) == PDF_OK,
                  "streamed page_add_custom");
            pdf_font_get_std(doc, PDF_STD_FONT_HELVETICA, &font);
            snprintf(line, sizeof(line), "sheet %d/%d", i + 1, pages);
            pdf_page_draw_text(page, font, line, 60, 780, 14);
        }
        CHECK(pdf_doc_finish(doc) == PDF_OK, "streamed doc_finish");
        CHECK(s.calls >= pages, "sink saw >= one chunk per page");
        CHECK(pdf_doc_free(doc) == PDF_OK, "streamed doc_free");
        fclose(s.f);

        snprintf(label, sizeof(label), "%s/streamed-callback", name);
        validate_pdf(path, pages, label);
        /* The streamed writer is a completely different code path from the
         * batch saver, so it gets its own content check rather than
         * inheriting confidence from test_readback(). */
        if (rb_open(&rd, path, label))
        {
            char first[64], last[64];

            rb_check_pages(&rd, pages, "streamed pages");
            rb_check_streams_decode(&rd, "streamed integrity");
            rb_check_pages_nonempty(&rd, "every streamed sheet drawn", pages);
            rb_check_op(&rd, 0, "Tj", 1, "streamed text op");
            rb_check_q_balance(&rd, 0, "streamed gstate");
            snprintf(first, sizeof(first), "sheet 1/%d", pages);
            snprintf(last, sizeof(last), "sheet %d/%d", pages, pages);
            rb_check_text(&rd, 0, first, "streamed page 1 text");
            rb_check_text(&rd, pages - 1, last, "streamed last page text");
            rb_close(&rd);
        }
        remove(path);
    }

    /* --- 2) core-owned file sink --- */
    snprintf(path, sizeof(path), "apitest-streamedfile-%s.pdf", name);
    doc = NULL;
    {
        pdf_status rc = pdf_doc_create_streamed_to_file(path, &doc);
        if (rc == PDF_ERR_BACKEND_UNSUPPORTED)
        {
            printf("  [i]    %s: create_streamed_to_file unsupported\n", name);
            remove(path);
        }
        else
        {
            CHECK(rc == PDF_OK, "create_streamed_to_file");
            if (rc == PDF_OK)
            {
                for (i = 0; i < pages; i++)
                {
                    pdf_page page = NULL;
                    pdf_font font = NULL;
                    CHECK(pdf_page_add_custom(doc, 595, 842, &page) == PDF_OK,
                          "streamed-to-file page add");
                    pdf_font_get_std(doc, PDF_STD_FONT_HELVETICA, &font);
                    pdf_page_draw_text(page, font, "streamed to a file",
                                       60, 780, 14);
                }
                CHECK(pdf_doc_finish(doc) == PDF_OK, "streamed-to-file finish");
                CHECK(pdf_doc_free(doc) == PDF_OK, "streamed-to-file free");
                snprintf(label, sizeof(label), "%s/streamed-file", name);
                validate_pdf(path, pages, label);
                if (rb_open(&rd, path, label))
                {
                    rb_check_pages(&rd, pages, "streamed-to-file pages");
                    rb_check_streams_decode(&rd, "streamed-to-file integrity");
                    rb_check_text(&rd, 0, "streamed to a file",
                                  "streamed-to-file text");
                    rb_close(&rd);
                }
            }
            remove(path);
        }
    }

    /* --- 3) caller-owned OS handle sink --- */
    snprintf(path, sizeof(path), "apitest-streamedhandle-%s.pdf", name);
    doc = NULL;
    {
        void *h = os_open_write(path);
        pdf_status rc;

        CHECK(h != NULL, "open handle for streamed output");
        rc = pdf_doc_create_streamed_to_handle(h, &doc);
        if (rc == PDF_ERR_BACKEND_UNSUPPORTED)
        {
            printf("  [i]    %s: create_streamed_to_handle unsupported\n", name);
            os_close(h);
            remove(path);
        }
        else
        {
            CHECK(rc == PDF_OK, "create_streamed_to_handle");
            for (i = 0; i < pages; i++)
            {
                pdf_page page = NULL;
                pdf_font font = NULL;
                CHECK(pdf_page_add_custom(doc, 595, 842, &page) == PDF_OK,
                      "streamed-to-handle page add");
                pdf_font_get_std(doc, PDF_STD_FONT_HELVETICA, &font);
                pdf_page_draw_text(page, font, "streamed to a handle",
                                   60, 780, 14);
            }
            CHECK(pdf_doc_finish(doc) == PDF_OK, "streamed-to-handle finish");
            CHECK(pdf_doc_free(doc) == PDF_OK, "streamed-to-handle free");
            os_close(h);   /* the handle is the caller's, pdflib never closes */
            snprintf(label, sizeof(label), "%s/streamed-handle", name);
            validate_pdf(path, pages, label);
            /* Read back through the caller-owned handle path: the sink is
             * the one this API redesign exists for, so its output is
             * verified rather than assumed. */
            if (rb_open(&rd, path, label))
            {
                rb_check_pages(&rd, pages, "streamed-to-handle pages");
                rb_check_streams_decode(&rd, "streamed-to-handle integrity");
                rb_check_text(&rd, 0, "streamed to a handle",
                              "streamed-to-handle text");
                rb_close(&rd);
            }
            remove(path);
        }
    }

    /* --- sink failure contract --- */
    snprintf(path, sizeof(path), "apitest-abort-%s.pdf", name);
    doc = NULL;
    memset(&s, 0, sizeof(s));
    s.f = fopen(path, "wb");
    if (s.f && pdf_doc_create_streamed(counting_sink, &s, &doc) == PDF_OK)
    {
        s.fail_on_call = 3;   /* let a couple of chunks through, then fail */
        {
            pdf_page page = NULL;
            pdf_font font = NULL;
            pdf_status rc;
            int got_error = 0;

            pdf_page_add_custom(doc, 595, 842, &page);
            pdf_font_get_std(doc, PDF_STD_FONT_HELVETICA, &font);
            for (i = 0; i < 40 && !got_error; i++)
            {
                pdf_page_draw_text(page, font, "boom", 60, 700, 14);
                rc = pdf_page_add_custom(doc, 595, 842, &page);
                if (rc == PDF_ERR_IO)
                    got_error = 1;
            }
            CHECK(got_error, "failing sink propagates PDF_ERR_IO");
            CHECK(pdf_doc_finish(doc) == PDF_ERR_IO,
                  "finish keeps reporting the sink failure");
        }
        pdf_doc_free(doc);
    }
    else
    {
        printf("  [i]    %s: no native streamed writer, so the sink-failure "
               "contract is exercised on a regular document below\n", name);
    }
    /* Close before unlinking: Windows refuses to delete a file that still
     * has an open handle, which used to leave a stray .pdf behind. */
    if (s.f)
        fclose(s.f);
    remove(path);

    /* --- a failing callback on a regular document --- */
    doc = NULL;
    memset(&s, 0, sizeof(s));
    s.f = fopen("apitest-abort-regular.bin", "wb");
    s.fail_on_call = 1;
    if (s.f && pdf_doc_create(&doc) == PDF_OK)
    {
        pdf_page page = NULL;
        pdf_font font = NULL;

        pdf_page_add(doc, PDF_PAGE_A4, PDF_ORIENTATION_PORTRAIT, &page);
        pdf_font_get_std(doc, PDF_STD_FONT_HELVETICA, &font);
        pdf_page_draw_text(page, font, "abort", 60, 700, 14);
        CHECK(pdf_doc_save_to_stream(doc, counting_sink, &s) == PDF_ERR_IO,
              "save_to_stream propagates a failing sink");
        pdf_doc_free(doc);
    }
    if (s.f)
        fclose(s.f);
    remove("apitest-abort-regular.bin");
}

/* ------------------------------------------------------------------ */
/* Encryption: RC4 needs passwords, and the engine may refuse          */
/* ------------------------------------------------------------------ */

static void test_encryption(int backend, const char *name)
{
    pdf_doc doc = NULL;
    pdf_page page = NULL;
    pdf_font font = NULL;
    char path[160];
    pdf_status rc;

    printf("[test] encryption (%s)\n", name);
    if (pdflib_set_backend((pdf_backend)backend) != PDF_OK)
        return;

    snprintf(path, sizeof(path), "apitest-encrypted-%s.pdf", name);

    CHECK(pdf_doc_create(&doc) == PDF_OK, "encrypted doc create");
    pdf_page_add(doc, PDF_PAGE_A4, PDF_ORIENTATION_PORTRAIT, &page);
    pdf_font_get_std(doc, PDF_STD_FONT_HELVETICA, &font);
    pdf_page_draw_text(page, font, "confidential", 60, 700, 16);

    rc = pdf_doc_set_encryption(doc, PDF_ENCRYPT_RC4_128, "owner-pw",
                                "user-pw", PDF_PERM_PRINT | PDF_PERM_COPY);
    CHECK_MAYBE(rc, "set_encryption(RC4_128)");

    if (rc == PDF_OK)
    {
        CHECK(pdf_doc_save_to_file(doc, path) == PDF_OK,
              "save encrypted document");
        validate_pdf(path, 1, "encrypted");
        {
            unsigned char *buf = NULL;
            long n = slurp(path, &buf);
            int has_encrypt = (n > 0 && contains(buf, n, "/Encrypt"));
            CHECK(has_encrypt, "encrypted output carries /Encrypt");
            free(buf);
        }
        remove(path);
    }
    pdf_doc_free(doc);
}

/* ------------------------------------------------------------------ */
/* Read-back: build a document, then prove the CONTENT is in the file  */
/*                                                                     */
/* Every assertion here is on the decoded artefact, never on a status  */
/* code. The document is deliberately not a toy: several page sizes,   */
/* a rotated page, both colour spaces that map to distinct operators,  */
/* every paint mode, an embedded font, an embedded image, annotations  */
/* and a nested outline. A call that returns PDF_OK and writes nothing */
/* passes every status-code test ever written and fails this one.      */
/* ------------------------------------------------------------------ */

static void test_readback(int backend, const char *name)
{
    pdf_doc doc = NULL;
    pdf_page p_gfx = NULL, p_text = NULL, p_img = NULL, p_annot = NULL;
    pdf_font helv = NULL, ttf = NULL;
    pdf_color col;
    pdf_rect r;
    pdf_point poly[5];
    double dash[2];
    char path[160], label[96], ttf_path[260], cjk_path[260];
    int have_ttf, have_cjk;
    rb rd;

    printf("[test] read-back content verification (%s)\n", name);

    if (pdflib_set_backend((pdf_backend)backend) != PDF_OK)
    {
        printf("  [i]    backend %s unavailable, skipping\n", name);
        return;
    }

    /* --- build ------------------------------------------------------- */
    CHECK(pdf_doc_create(&doc) == PDF_OK, "readback: doc_create");
    if (!doc)
        return;

    CHECK(pdf_doc_set_info(doc, PDF_INFO_TITLE, "pdflib readback") == PDF_OK,
          "readback: set_info(title)");
    CHECK(pdf_doc_set_info(doc, PDF_INFO_AUTHOR, "apitest") == PDF_OK,
          "readback: set_info(author)");
    /* Compression on: content streams then have to be Flate-decoded before
     * anything can be asserted, which also exercises the reader. */
    CHECK(pdf_doc_set_compression(doc, PDF_COMPRESS_ALL) == PDF_OK,
          "readback: set_compression(ALL)");

    /* ---- page 1: graphics ---- */
    CHECK(pdf_page_add(doc, PDF_PAGE_A4, PDF_ORIENTATION_PORTRAIT, &p_gfx)
          == PDF_OK, "readback: page_add(A4)");

    /* A CMYK fill is the sharpest colour test available: CMYK must map to
     * the lowercase 'k' operator, and a K/k mix-up (a real hazard, since
     * they are one case apart) shows up as the wrong operator rather than
     * as a wrong-looking colour. */
    memset(&col, 0, sizeof(col));
    col.kind = PDF_COLOR_CMYK;
    col.c1 = 0.1; col.c2 = 0.7; col.c3 = 0.2; col.c4 = 0.0;
    col.alpha = 1;
    CHECK(pdf_set_fill_color(p_gfx, &col) == PDF_OK,
          "readback: set_fill_color(CMYK)");
    r.x = 60; r.y = 700; r.w = 120; r.h = 60;
    CHECK(pdf_path_rectangle(p_gfx, &r) == PDF_OK, "readback: rectangle");
    CHECK(pdf_path_paint(p_gfx, PDF_PATH_FILL) == PDF_OK,
          "readback: paint(fill)");

    /* Each paint mode needs its own path: painting consumes it.
     *
     * Note the case: a fill colour is the lowercase operator ('rg') and a
     * stroke colour is the uppercase one ('RG'), likewise 'g'/'G' and
     * 'k'/'K'. Asserting the wrong case is a mistake this test made on
     * its first run, which is exactly why the operators are checked here
     * rather than the colours being eyeballed. */
    memset(&col, 0, sizeof(col));
    col.kind = PDF_COLOR_RGB;
    col.c1 = 0.8; col.c2 = 0.1; col.c3 = 0.1; col.alpha = 1;
    CHECK(pdf_set_stroke_color(p_gfx, &col) == PDF_OK,
          "readback: set_stroke_color(RGB)");
    CHECK(pdf_set_line_width(p_gfx, 3) == PDF_OK, "readback: set_line_width");
    dash[0] = 6; dash[1] = 3;
    CHECK(pdf_set_line_dash(p_gfx, dash, 2, 0) == PDF_OK,
          "readback: set_line_dash");
    r.x = 200; r.y = 700; r.w = 100; r.h = 60;
    CHECK(pdf_path_rectangle(p_gfx, &r) == PDF_OK, "readback: rectangle 2");
    CHECK(pdf_path_paint(p_gfx, PDF_PATH_FILL_STROKE) == PDF_OK,
          "readback: paint(fill+stroke)");

    /* An RGB FILL as well, so both halves of the case pair are covered. */
    col.c1 = 0.2; col.c2 = 0.4; col.c3 = 0.9;
    CHECK(pdf_set_fill_color(p_gfx, &col) == PDF_OK,
          "readback: set_fill_color(RGB)");
    r.x = 330; r.y = 700; r.w = 90; r.h = 60;
    CHECK(pdf_path_rectangle(p_gfx, &r) == PDF_OK, "readback: rectangle 5");
    CHECK(pdf_path_paint(p_gfx, PDF_PATH_FILL) == PDF_OK,
          "readback: paint(rgb fill)");

    /* Wrap a section in save/restore: that is what emits q/Q, so the
     * balance check below has something to be balanced about instead of
     * comparing zero with zero. */
    CHECK(pdf_gstate_save(p_gfx) == PDF_OK, "readback: gstate_save");
    CHECK(pdf_set_line_width(p_gfx, 9) == PDF_OK, "readback: width in gstate");
    r.x = 60; r.y = 640; r.w = 40; r.h = 20;
    CHECK(pdf_path_rectangle(p_gfx, &r) == PDF_OK, "readback: rectangle 6");
    CHECK(pdf_path_paint(p_gfx, PDF_PATH_STROKE) == PDF_OK,
          "readback: paint(stroke in gstate)");
    CHECK(pdf_gstate_restore(p_gfx) == PDF_OK, "readback: gstate_restore");

    memset(&col, 0, sizeof(col));
    col.kind = PDF_COLOR_GRAY;
    col.c1 = 0.35; col.alpha = 1;
    CHECK(pdf_set_fill_color(p_gfx, &col) == PDF_OK,
          "readback: set_fill_color(gray)");
    r.x = 60; r.y = 600; r.w = 100; r.h = 40;
    CHECK(pdf_path_rectangle(p_gfx, &r) == PDF_OK, "readback: rectangle 3");
    CHECK(pdf_path_paint(p_gfx, PDF_PATH_FILL_EVENODD) == PDF_OK,
          "readback: paint(fill even-odd)");
    r.x = 180; r.y = 600; r.w = 100; r.h = 40;
    CHECK(pdf_path_rectangle(p_gfx, &r) == PDF_OK, "readback: rectangle 4");
    CHECK(pdf_path_paint(p_gfx, PDF_PATH_FILL_STROKE_EVENODD) == PDF_OK,
          "readback: paint(fill+stroke even-odd)");

    /* A non-identity transform must reach the stream as a cm operand. */
    CHECK(pdf_set_transform(p_gfx, 1.5, 0, 0, 1.5, 20, 20) == PDF_OK,
          "readback: set_transform");
    CHECK(pdf_set_opacity(p_gfx, 0.5, 0.5) == PDF_OK, "readback: set_opacity");

    CHECK(pdf_font_get_std(doc, PDF_STD_FONT_HELVETICA, &helv) == PDF_OK,
          "readback: font_get_std(Helvetica)");
    CHECK(pdf_page_draw_text(p_gfx, helv, "readback page one", 60, 540, 18)
          == PDF_OK, "readback: draw_text");

    /* Text render mode 3 is invisible text: it must still be emitted, as
     * a "3 Tr" operand, even though nothing shows. */
    CHECK(pdf_set_text_render_mode(p_gfx, PDF_TEXT_RENDER_INVISIBLE) == PDF_OK,
          "readback: set_text_render_mode(3)");
    CHECK(pdf_page_draw_text(p_gfx, helv, "hidden", 60, 500, 12) == PDF_OK,
          "readback: draw_text(invisible)");
    CHECK(pdf_set_text_render_mode(p_gfx, PDF_TEXT_RENDER_FILL) == PDF_OK,
          "readback: set_text_render_mode(0)");
    /* Draw again after returning to fill. PoDoFo's PdfPainter emits the
     * "n Tr" operator only when the mode is NOT fill, so without the
     * adapter's repair this run inherits the "3 Tr" above and disappears
     * from the page while every call still returns PDF_OK. */
    CHECK(pdf_page_draw_text(p_gfx, helv, "visible again", 60, 470, 12)
          == PDF_OK, "readback: draw_text(after fill reset)");

    /* ---- page 2: text in several shapes ---- */
    CHECK(pdf_page_add(doc, PDF_PAGE_LETTER, PDF_ORIENTATION_LANDSCAPE,
                       &p_text) == PDF_OK, "readback: page_add(Letter land)");
    CHECK(pdf_page_draw_text(p_text, helv, "left aligned", 60, 500, 14)
          == PDF_OK, "readback: draw_text(left)");
    CHECK(pdf_page_draw_text_aligned(p_text, helv, "centred", 60, 460, 14,
                                     400, PDF_ALIGN_CENTER) == PDF_OK,
          "readback: draw_text_aligned(center)");
    CHECK(pdf_page_draw_text_aligned(p_text, helv, "right aligned", 60, 420,
                                     14, 400, PDF_ALIGN_RIGHT) == PDF_OK,
          "readback: draw_text_aligned(right)");
    CHECK(pdf_page_draw_text_wrapped(p_text, helv,
                                     "wrapped text that has to be broken "
                                     "across several lines by the library",
                                     60, 380, 300, 120, 12, 4,
                                     PDF_ALIGN_LEFT) == PDF_OK,
          "readback: draw_text_wrapped");
    memset(poly, 0, sizeof(poly));
    poly[0].x = 60;  poly[0].y = 200;
    poly[1].x = 160; poly[1].y = 240;
    poly[2].x = 120; poly[2].y = 300;
    poly[3].x = 20;  poly[3].y = 260;
    poly[4].x = 60;  poly[4].y = 200;
    CHECK(pdf_gstate_save(p_text) == PDF_OK, "readback: p2 gstate_save");
    CHECK(pdf_path_polygon(p_text, poly, 5) == PDF_OK, "readback: polygon");
    CHECK(pdf_path_paint(p_text, PDF_PATH_STROKE) == PDF_OK,
          "readback: paint(polygon)");
    CHECK(pdf_gstate_restore(p_text) == PDF_OK, "readback: p2 gstate_restore");

    have_ttf = find_system_ttf(ttf_path, sizeof(ttf_path));
    if (have_ttf)
    {
        printf("  [i]    readback: system TTF %s\n", ttf_path);
        if (pdf_font_load_ttf(doc, ttf_path, 1, &ttf) == PDF_OK && ttf)
            CHECK(pdf_page_draw_text(p_text, ttf, "embedded face", 60, 160, 16)
                  == PDF_OK, "readback: draw_text(TTF)");
        else
            ttf = NULL;
    }

    /* CJK text goes through the composite-font path, which is a different
     * code path in both engines and the only one that produces a
     * /DescendantFonts chain.
     *
     * libharu cannot read a .ttc (TrueType collection) - it fails with
     * HPDF_INVALID_TTC_FILE - so a clean refusal is a pass here. What must
     * NOT happen is the failure poisoning the document: libharu latches
     * the first error and then makes every later call a silent no-op, so
     * the pages added below are the regression test for that. */
    have_cjk = find_cjk_ttf(cjk_path, sizeof(cjk_path));
    if (have_cjk)
    {
        pdf_font cjk = NULL;
        pdf_status rc;

        printf("  [i]    readback: CJK font %s\n", cjk_path);
        rc = pdf_font_load_ttf(doc, cjk_path, 1, &cjk);
        CHECK(rc == PDF_OK || rc == PDF_ERR_BACKEND_FAILED ||
              rc == PDF_ERR_BACKEND_UNSUPPORTED,
              "readback: CJK font load handled cleanly");
        if (rc == PDF_OK && cjk)
        {
            CHECK(pdf_page_draw_text(p_text, cjk,
                                     "\xE4\xB8\xAD\xE6\x96\x87\xE6\xB5\x8B\xE8\xAF\x95",
                                     60, 120, 18) == PDF_OK,
                  "readback: draw_text(CJK)");
            pdf_font_free(cjk);
        }
        else
        {
            have_cjk = 0;
            printf("  [i]    readback: CJK font declined (%s); the document "
                   "must still be usable\n", pdf_dll_status_string(rc));
        }
    }

    /* ---- page 3: an image, on a rotated custom page ----
     *
     * This page is added immediately after the CJK font attempt, so it
     * doubles as the check that a refused font load did not disable the
     * document. */
    CHECK(pdf_page_add_custom(doc, 300, 400, &p_img) == PDF_OK,
          "readback: page_add_custom after font load (document still usable)");
    CHECK(pdf_page_set_rotation(p_img, 90) == PDF_OK,
          "readback: page_set_rotation(90)");
    {
        pdf_image img = NULL;
        pdf_status rc = pdf_image_load_memory(doc, sample_png, SAMPLE_PNG_LEN,
                                              &img);
        CHECK(rc == PDF_OK, "readback: image_load_memory(png)");
        if (rc == PDF_OK && img)
        {
            size_t iw = 0, ih = 0;
            CHECK(pdf_image_get_size(img, &iw, &ih) == PDF_OK && iw && ih,
                  "readback: image_get_size");
            r.x = 40; r.y = 200; r.w = 160; r.h = 160;
            CHECK(pdf_gstate_save(p_img) == PDF_OK, "readback: p3 gstate_save");
            CHECK(pdf_page_draw_image(p_img, img, &r) == PDF_OK,
                  "readback: draw_image");
            CHECK(pdf_gstate_restore(p_img) == PDF_OK,
                  "readback: p3 gstate_restore");
            pdf_image_free(img);
        }
    }

    /* ---- page 4: annotations and an outline ---- */
    CHECK(pdf_page_add(doc, PDF_PAGE_A5, PDF_ORIENTATION_PORTRAIT, &p_annot)
          == PDF_OK, "readback: page_add(A5)");
    {
        pdf_annot a = NULL;
        pdf_rect ar;

        ar.x = 60; ar.y = 400; ar.w = 140; ar.h = 24;
        CHECK(pdf_annot_create(p_annot, PDF_ANNOT_TEXT, &ar, "readback note",
                               &a) == PDF_OK, "readback: annot_create(text)");
        if (a)
        {
            memset(&col, 0, sizeof(col));
            col.kind = PDF_COLOR_RGB;
            col.c1 = 0.9; col.c2 = 0.2; col.c3 = 0.2; col.alpha = 1;
            pdf_annot_set_color(a, &col);
            pdf_annot_set_opacity(a, 0.75);
            pdf_annot_set_author(a, "apitest");
            pdf_annot_set_subject(a, "readback");
            pdf_annot_set_icon(a, PDF_ANNOT_ICON_NOTE);
            pdf_annot_set_open(a, 1);
            pdf_annot_free(a);
        }

        a = NULL;
        ar.x = 60; ar.y = 360; ar.w = 180; ar.h = 16;
        CHECK(pdf_annot_create(p_annot, PDF_ANNOT_LINK_URI, &ar,
                               "https://example.com/readback", &a) == PDF_OK,
              "readback: annot_create(link uri)");
        if (a)
            pdf_annot_free(a);

        a = NULL;
        ar.x = 60; ar.y = 320; ar.w = 120; ar.h = 16;
        if (pdf_annot_create(p_annot, PDF_ANNOT_HIGHLIGHT, &ar, "highlighted",
                             &a) == PDF_OK && a)
        {
            pdf_annot_link_to_page(a, NULL, "https://example.com/readback");
            pdf_annot_free(a);
        }
    }
    {
        pdf_outline top = NULL, child = NULL;

        CHECK(pdf_outline_add(doc, NULL, "Chapter one", &p_gfx, &top) == PDF_OK,
              "readback: outline_add(top)");
        if (top)
        {
            CHECK(pdf_outline_add(doc, top, "Section 1.1", &p_text, &child)
                  == PDF_OK, "readback: outline_add(child)");
            if (child)
            {
                pdf_outline_set_open(child, 1);
                pdf_outline_free(child);
            }
            pdf_outline_set_open(top, 1);
            pdf_outline_free(top);
        }
        {
            pdf_outline second = NULL;
            pdf_outline_add(doc, NULL, "Chapter two", &p_img, &second);
            if (second)
                pdf_outline_free(second);
        }
    }

    snprintf(path, sizeof(path), "%s-readback.pdf", g_prefix);
    CHECK(pdf_doc_save_to_file(doc, path) == PDF_OK, "readback: save_to_file");
    pdf_doc_free(doc);

    /* --- verify ------------------------------------------------------ */
    snprintf(label, sizeof(label), "readback(%s)", name);
    validate_pdf(path, 4, label);

    if (!rb_open(&rd, path, label))
        return;

    rb_check_pages(&rd, 4, "page tree");
    rb_check_streams_decode(&rd, "stream integrity");
    /* Pages 1-3 are drawn on; page 4 carries only annotations and an outline,
     * so it legitimately has no content. Expect exactly three. */
    rb_check_pages_nonempty(&rd, "pages 1-3 drawn (page 4 is annot-only)", 3);
    rb_check_page_blank(&rd, 3, "page 4 annot-only");
    rb_check_has_flate(&rd, "compression");

    /* Page geometry, including the reals that a naive integer read would
     * round away (A4 is 595.28x841.89 pt, not 595x841). */
    rb_check_page_size(&rd, 0, 595.28, 841.89, "page 1 size");
    rb_check_page_size(&rd, 1, 792.0, 612.0, "page 2 size");
    rb_check_page_size(&rd, 2, 300.0, 400.0, "page 3 size");
    rb_check_page_size(&rd, 3, 419.53, 595.28, "page 4 size");
    rb_check_page_rotate(&rd, 2, 90, "page 3 rotation");

    /* Page 1: the drawing operators, including the CMYK 'k' and the
     * text render mode 'Tr'. */
    rb_check_op(&rd, 0, "k", 1, "page 1 CMYK fill");
    rb_check_op(&rd, 0, "RG", 1, "page 1 RGB stroke");
    rb_check_op(&rd, 0, "rg", 1, "page 1 RGB fill");
    rb_check_op(&rd, 0, "g", 1, "page 1 gray fill");
    rb_check_op(&rd, 0, "re", 5, "page 1 rectangles");
    rb_check_op(&rd, 0, "f", 2, "page 1 fill");
    rb_check_op(&rd, 0, "B", 1, "page 1 fill+stroke");
    rb_check_op(&rd, 0, "f*", 1, "page 1 even-odd fill");
    rb_check_op(&rd, 0, "B*", 1, "page 1 even-odd fill+stroke");
    rb_check_op(&rd, 0, "cm", 1, "page 1 transform");
    rb_check_op(&rd, 0, "Tr", 1, "page 1 text render mode");
    rb_check_op(&rd, 0, "d", 1, "page 1 dash");
    rb_check_op(&rd, 0, "q", 1, "page 1 gstate save");
    rb_check_q_balance(&rd, 0, "page 1 gstate");
    rb_check_text(&rd, 0, "readback page one", "page 1 text");
    rb_check_text(&rd, 0, "visible again", "page 1 text after fill reset");
    /* The page used "3 Tr" for the invisible run; it must have written
     * "0 Tr" afterwards, or everything after it is invisible too. */
    rb_check_last_op_operand(&rd, 0, "Tr", 0, "page 1 ends in fill mode");

    /* Page 2: the text operators. */
    rb_check_op(&rd, 1, "Tj", 1, "page 2 show text");
    rb_check_op(&rd, 1, "BT", 1, "page 2 text object");
    rb_check_text(&rd, 1, "left aligned", "page 2 left text");
    rb_check_text(&rd, 1, "centred", "page 2 centred text");
    rb_check_text(&rd, 1, "right aligned", "page 2 right text");
    /* Two lines of text in a 120pt box: painted once, not once per pass
     * through the box-filling loop. */
    rb_check_text_count(&rd, 1, "wrapped text that has to be", 1,
                        "page 2 wrapped paragraph");
    rb_check_op(&rd, 1, "m", 1, "page 2 polygon path");
    rb_check_op(&rd, 1, "q", 1, "page 2 gstate save");
    rb_check_q_balance(&rd, 1, "page 2 gstate");

    /* Page 3: the image must be a real XObject that the page draws. */
    rb_check_image(&rd, 2, 16, 16, "page 3 image");
    rb_check_op(&rd, 2, "q", 1, "page 3 gstate save");
    rb_check_q_balance(&rd, 2, "page 3 gstate");

    /* Page 4: annotations and the outline tree. */
    rb_check_annot(&rd, 3, "Text", 1, "page 4 text annot");
    rb_check_annot(&rd, 3, "Link", 1, "page 4 link annot");
    rb_check_annot(&rd, 3, "Highlight", 1, "page 4 highlight annot");
    rb_check_link_uri(&rd, 3, "https://example.com/readback",
                      "page 4 link target");
    rb_check_outlines(&rd, 3, "outline tree");

    /* Metadata has to survive the round trip. */
    rb_check_info(&rd, "Title", "pdflib readback", "metadata");
    rb_check_info(&rd, "Author", "apitest", "metadata");

    /* Fonts. The standard font must NOT be embedded; the TTF asked for
     * with embed=1 must contribute a real font program. */
    rb_check_font_present(&rd, "Helvetica", "standard font");
    rb_check_font_not_embedded(&rd, "Helvetica", "standard font");
    if (have_ttf && ttf)
        rb_check_any_font_embedded(&rd, "TTF font");
    if (have_cjk)
        rb_check_any_cid_font(&rd, "CJK font");

    rb_check_encrypt(&rd, 0, "no encryption requested");

    rb_close(&rd);

    /* The same document without compression: the content must decode to
     * the same operators, which also proves the compressor is lossless
     * rather than merely present. */
    {
        pdf_doc doc2 = NULL;
        char path2[160];

        if (pdf_doc_create(&doc2) == PDF_OK)
        {
            pdf_page pg = NULL;

            pdf_doc_set_compression(doc2, PDF_COMPRESS_NONE);
            if (pdf_page_add(doc2, PDF_PAGE_A4, PDF_ORIENTATION_PORTRAIT,
                             &pg) == PDF_OK)
            {
                pdf_font f = NULL;
                pdf_color cc;

                pdf_font_get_std(doc2, PDF_STD_FONT_HELVETICA, &f);
                memset(&cc, 0, sizeof(cc));
                cc.kind = PDF_COLOR_CMYK;
                cc.c1 = 0.25; cc.c2 = 0.5; cc.c3 = 0.75; cc.c4 = 0.1;
                cc.alpha = 1;
                pdf_set_fill_color(pg, &cc);
                r.x = 10; r.y = 10; r.w = 50; r.h = 50;
                pdf_path_rectangle(pg, &r);
                pdf_path_paint(pg, PDF_PATH_FILL);
                pdf_page_draw_text(pg, f, "uncompressed marker", 40, 400, 12);
            }
            snprintf(path2, sizeof(path2), "%s-readback-plain.pdf", g_prefix);
            CHECK(pdf_doc_save_to_file(doc2, path2) == PDF_OK,
                  "readback: save uncompressed");
            pdf_doc_free(doc2);

            {
                rb rd2;

                if (rb_open(&rd2, path2, "readback-plain"))
                {
                    rb_check_pages(&rd2, 1, "uncompressed page tree");
                    rb_check_streams_decode(&rd2, "uncompressed integrity");
                    rb_check_pages_nonempty(&rd2, "uncompressed page drawn", 1);
                    rb_check_op(&rd2, 0, "k", 1, "uncompressed CMYK");
                    rb_check_text(&rd2, 0, "uncompressed marker",
                                  "uncompressed text");
                    rb_close(&rd2);
                }
            }
            remove(path2);
        }
    }

    remove(path);
}

/* ------------------------------------------------------------------ */
/* --inspect: read a PDF back and report what is really in it          */
/*                                                                     */
/* Useful on its own (debugging a file pdflib produced) and as the      */
/* sanity check for pdf_inspect.h before the assertions trust it.       */
/* ------------------------------------------------------------------ */

static void report_ops(const unsigned char *c, long n)
{
    static const char *ops[] = {
        "q", "Q", "cm", "w", "J", "j", "M", "d", "gs", "ri", "i",
        "rg", "RG", "g", "G", "k", "K", "cs", "CS", "sc", "SC",
        "m", "l", "c", "v", "y", "h", "re",
        "S", "s", "f", "F", "f*", "B", "B*", "n", "W", "W*",
        "BT", "ET", "Tf", "Td", "TD", "Tm", "T*", "Tj", "TJ", "Tr",
        "Ts", "Tc", "Tw", "Tz", "TL", "Do", "sh", "BI", "EI",
        NULL
    };
    int i;
    int shown = 0;

    for (i = 0; ops[i]; i++)
    {
        int k = pdfi_count_op(c, n, ops[i]);
        if (k)
        {
            printf("        %-4s x%d", ops[i], k);
            if (++shown % 6 == 0)
                printf("\n");
        }
    }
    if (shown % 6)
        printf("\n");
    if (!shown)
        printf("        (no operators)\n");
}

static int inspect_file(const char *path)
{
    unsigned char *buf = NULL;
    long n = slurp(path, &buf);
    pdfi_doc d;
    long pages[PDFI_MAX_PAGES];
    int np, i;

    if (n < 0 || !buf)
    {
        printf("cannot read %s\n", path);
        return 2;
    }

    printf("=== %s (%ld bytes) ===\n", path, n);
    printf("  header      : %.8s\n", (const char *)buf);
    printf("  %%EOF        : %s\n", pdfi_contains(buf, n, "%%EOF") ? "yes" : "NO");
    printf("  xref        : %s\n",
           pdfi_contains(buf, n, "\nxref") ? "table"
                                           : (pdfi_contains(buf, n, "/XRef")
                                                  ? "stream" : "MISSING"));

    pdfi_scan(buf, n, &d);
    printf("  objects     : %d\n", d.nobj);
    if (pdfi_has_object_streams(&d))
        printf("  [warn] /ObjStm present: page dictionaries are inside\n"
               "         object streams and are NOT visible to this scanner\n");

    np = pdfi_pages(&d, pages, PDFI_MAX_PAGES);
    printf("  pages       : %d\n", np);

    for (i = 0; i < np; i++)
    {
        double box[4] = { 0, 0, 0, 0 };
        long rot = 0;
        unsigned char *c = NULL;
        long cn;

        printf("  page %d (obj %ld)", i + 1, pages[i]);
        /* Reals, not integers: A4 is 595.276x841.89 pt and printing "595x841"
         * hides exactly the precision a page-size bug would show up in. */
        if (pdfi_dict_array_nums(&d, pdfi_find(&d, pages[i]), "MediaBox",
                                 box, 4) == 4)
            printf("  MediaBox=[%.2f %.2f %.2f %.2f]", box[0], box[1], box[2],
                   box[3]);
        if (pdfi_dict_int(&d, pdfi_find(&d, pages[i]), "Rotate", &rot))
            printf("  Rotate=%ld", rot);
        printf("\n");

        cn = pdfi_page_content(&d, pages[i], &c);
        if (cn < 0)
        {
            /* "blank" and "broken" are different diagnoses: PoDoFo omits
             * /Contents altogether for a page nothing was drawn on, which is
             * perfectly legal, while a present-but-uninflatable stream is a
             * real defect. Do not report them the same way. */
            if (!pdfi_page_has_contents(&d, pages[i]))
                printf("      content: (none - page has no /Contents)\n");
            else
                printf("      content: UNDECODABLE (stream present, will not inflate)\n");
            continue;
        }
        printf("      content: %ld bytes decoded\n", cn);
        report_ops(c, cn);
        free(c);
    }

    /* Fonts. */
    for (i = 0; i < d.nobj; i++)
    {
        char sub[48] = "", base[80] = "", type[32] = "";
        const pdfi_obj *o = &d.objs[i];

        pdfi_dict_name(&d, o, "Type", type, sizeof(type));
        pdfi_dict_name(&d, o, "Subtype", sub, sizeof(sub));
        pdfi_dict_name(&d, o, "BaseFont", base, sizeof(base));

        if (strcmp(sub, "Type1") == 0 || strcmp(sub, "TrueType") == 0 ||
            strcmp(sub, "Type0") == 0 || strcmp(sub, "CIDFontType0") == 0 ||
            strcmp(sub, "CIDFontType2") == 0 || strcmp(sub, "Type3") == 0)
        {
            printf("  font obj %ld : %s %s", o->num, sub, base);
            if (pdfi_dict_has(&d, o, "FontFile"))
                printf(" [FontFile]");
            if (pdfi_dict_has(&d, o, "FontFile2"))
                printf(" [FontFile2]");
            if (pdfi_dict_has(&d, o, "FontFile3"))
                printf(" [FontFile3]");
            if (pdfi_dict_has(&d, o, "ToUnicode"))
                printf(" [ToUnicode]");
            if (pdfi_dict_has(&d, o, "W"))
                printf(" [W]");
            if (pdfi_dict_has(&d, o, "Widths"))
                printf(" [Widths]");
            printf("\n");
        }
    }

    /* Images. */
    for (i = 0; i < d.nobj; i++)
    {
        char sub[48] = "";
        const pdfi_obj *o = &d.objs[i];
        long w = 0, h = 0, bpc = 0;

        if (!pdfi_dict_name(&d, o, "Subtype", sub, sizeof(sub)) ||
            strcmp(sub, "Image") != 0)
            continue;

        pdfi_dict_int(&d, o, "Width", &w);
        pdfi_dict_int(&d, o, "Height", &h);
        pdfi_dict_int(&d, o, "BitsPerComponent", &bpc);
        printf("  image obj %ld: %ldx%ld bpc=%ld encoded=%ld bytes",
               o->num, w, h, bpc, o->stream_len);
        if (pdfi_dict_has(&d, o, "SMask"))
            printf(" [SMask]");
        {
            char filters[4][48];
            int nf = pdfi_stream_filters(&d, o, filters, 4);
            int k;

            for (k = 0; k < nf; k++)
                printf(" [%s]", filters[k]);
        }
        {
            unsigned char *px = NULL;
            long pn = pdfi_stream(&d, o, &px);
            if (pn > 0)
                printf(" decoded=%ld", pn);
            free(px);
        }
        printf("\n");
    }

    /* Annotations per page. */
    for (i = 0; i < np; i++)
    {
        const pdfi_obj *p = pdfi_find(&d, pages[i]);
        pdfi_val a;

        if (pdfi_dict_get(&d, p, "Annots", &a) && a.kind == PDFI_V_ARRAY)
        {
            long q = a.off + 1, end = a.off + a.len;
            int k = 0;

            printf("  page %d annots:", i + 1);
            while (q < end && k < 40)
            {
                pdfi_val e;
                char sub[48] = "";

                while (q < end && pdfi_is_ws(d.buf[q]))
                    q++;
                if (q >= end || d.buf[q] == ']')
                    break;
                pdfi_read_value(d.buf, end, &q, &e);
                if (e.kind == PDFI_V_REF)
                {
                    const pdfi_obj *ao = pdfi_find(&d, e.ival);
                    if (ao && pdfi_dict_name(&d, ao, "Subtype", sub, sizeof(sub)))
                    {
                        printf(" %s", sub);
                        k++;
                    }
                }
            }
            printf("\n");
        }
    }

    /* Outlines. The root is the catalog's /Outlines target; items are the
     * objects carrying /Title and /Parent. (libharu tags items with
     * /Type /Outlines as well, so a type scan lists the items as roots.) */
    {
        long cat = pdfi_trailer_ref(&d, "Root");
        long root = -1;
        int items = 0;
        long count = 0;

        if (cat >= 0)
        {
            const pdfi_obj *c = pdfi_find(&d, cat);

            if (c)
                root = pdfi_dict_ref(&d, c, "Outlines");
        }
        for (i = 0; i < d.nobj; i++)
        {
            if (pdfi_dict_has(&d, &d.objs[i], "Title") &&
                pdfi_dict_has(&d, &d.objs[i], "Parent"))
                items++;
        }
        if (root >= 0)
        {
            const pdfi_obj *r = pdfi_find(&d, root);

            if (r)
                pdfi_dict_int(&d, r, "Count", &count);
            printf("  outlines    : root obj %ld, /Count %ld, %d item(s)\n",
                   root, count, items);
        }
        else if (items)
            printf("  outlines    : %d item(s), no root reachable\n", items);
    }

    /* Info. Reached through the trailer, not by scanning every object for
     * a /Title: an outline item carries /Title too, and a scan prints the
     * chapter names as though they were document metadata. */
    {
        long info = pdfi_trailer_ref(&d, "Info");
        const pdfi_obj *o = info >= 0 ? pdfi_find(&d, info) : NULL;

        if (!o)
            printf("  info        : (no /Info in the trailer)\n");
        else
        {
            static const char *keys[] = { "Title", "Author", "Subject",
                                          "Keywords", "Creator", "Producer",
                                          NULL };
            int j;

            printf("  info obj %ld:\n", o->num);
            for (j = 0; keys[j]; j++)
            {
                char txt[256];

                if (pdfi_dict_text(&d, o, keys[j], txt, sizeof(txt)))
                    printf("      /%s = \"%s\"\n", keys[j], txt);
            }
        }
    }

    if (pdfi_contains(buf, n, "/Encrypt"))
        printf("  encrypted   : yes\n");

    free(buf);
    return 0;
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */

typedef struct engine
{
    int id;
    const char *name;
} engine;

int main(int argc, char **argv)
{
    const char *which = argc > 1 ? argv[1] : "all";
    static const engine engines[] = {
        { PDF_BACKEND_PODOFO,  "podofo"  },
        { PDF_BACKEND_LIBHARU, "libharu" }
    };
    int i;
    int ran = 0;

    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc > 2)
        g_prefix = argv[2];

    /* Read-back mode: report what is actually inside a produced file. */
    if (argc > 1 && strcmp(argv[1], "--inspect") == 0)
    {
        if (argc < 3)
        {
            printf("usage: apitest --inspect <file.pdf>\n");
            return 2;
        }
        return inspect_file(argv[2]);
    }

    printf("=== pdflib API coverage test ===\n");
    printf("DLL      : %s\n", pdf_dll_version());
    printf("backends : %s\n", pdf_dll_backends());
    printf("mode     : %s\n\n", which);

    test_introspection();
    test_error_paths();

    for (i = 0; i < (int)(sizeof(engines) / sizeof(engines[0])); i++)
    {
        if (strcmp(which, "all") != 0 && strcmp(which, engines[i].name) != 0)
            continue;
        if (!pdflib_has_backend((pdf_backend)engines[i].id))
            continue;
        test_document(engines[i].id, engines[i].name);
        test_readback(engines[i].id, engines[i].name);
        test_streamed(engines[i].id, engines[i].name);
        test_encryption(engines[i].id, engines[i].name);
        ran++;
    }

    if (!ran)
    {
        printf("no backend matched \"%s\"\n", which);
        return 2;
    }

    /* test_images() materialises the embedded samples so the file loader
     * has real input; they are regenerated on every run, so drop them. */
    remove("apitest-sample.png");
    remove("apitest-sample.jpg");

    printf("\n=== %s: %d check(s), %d failure(s) ===\n",
           g_fail ? "FAILED" : "PASSED", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
