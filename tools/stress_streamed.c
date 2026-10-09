/* stress_streamed - batch stress test for the streamed-document path.
 *
 * Generates documents with a large number of pages (default 1000) of
 * pseudo-random content (rects, polygons, bezier curves, text, gstate
 * save/restore, custom page sizes) while sampling the process working
 * set. After the run it reports wall time, throughput and peak memory,
 * and verifies the streamed-mode guarantees:
 *
 *   1. memory is O(1) per page: the growth rate (RSS bytes per page,
 *      measured inside a single run between page checkpoints) stays
 *      within a constant budget as the page count grows;
 *   2. the growth rate does not depend on per-page content volume
 *      (content is flushed, not buffered).
 *
 * Measurement notes:
 *   - The slope is taken WITHIN one run (RSS at page 250 vs page 1000)
 *     so allocator high-water from previous runs cannot pollute it.
 *   - Peak RSS is still reported, but only as an informational figure;
 *     Windows' working set never shrinks back after transient spikes,
 *     so it overstates retained memory.
 *   - The PoDoFo engine keeps a per-page cache entry
 *     (PdfPagesTreeCache) and page dictionary alive in streamed mode,
 *     costing a few KB per page. That is constant per-page overhead,
 *     not content buffering - content streams are serialized and
 *     released as each page completes (the object table drops to a
 *     handful of entries after finish).
 *
 * Usage: stress_streamed [pages] [backend]
 *   pages   page count for a single rich-content run (default 1000);
 *           0 = full verification suite (growth check + 1000-page run)
 *   backend "podofo" | "libharu" | "dummy" (default podofo)
 *
 * Memory measurement is Windows-only (GetProcessMemoryInfo); on other
 * platforms it reports 0 and the memory checks are skipped.
 */
#include "pdflib/pdf_api.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

#if defined(_WIN32)
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#  include <psapi.h>
static size_t process_rss(void)
{
    PROCESS_MEMORY_COUNTERS pmc;
    memset(&pmc, 0, sizeof(pmc));
    pmc.cb = sizeof(pmc);
    if (!GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc)))
        return 0;
    return (size_t)pmc.WorkingSetSize;
}
#else
static size_t process_rss(void)
{
    return 0; /* not implemented on this platform */
}
#endif

static double mb(size_t bytes)
{
    return (double)bytes / (1024.0 * 1024.0);
}

#define OUT_FILE "stress-out.pdf"

/* ------------------------------------------------------------------ */
/* Deterministic pseudo-random content                                 */
/* ------------------------------------------------------------------ */

static unsigned long rng_state = 0x12345678UL;

static void rng_reset(void)
{
    rng_state = 0x12345678UL;
}

static unsigned long rng_next(void)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}

static double rng_range(double lo, double hi)
{
    return lo + (hi - lo) * ((double)(rng_next() % 1000001u) / 1000000.0);
}

static const char *WORDS[] = {
    "print", "driver", "stream", "page", "query", "job", "sheet",
    "spool", "vector", "glyph", "subset", "xref", "trailer", "flush",
};
#define N_WORDS ((int)(sizeof(WORDS) / sizeof(WORDS[0])))

static void fill_page(pdf_page page, const pdf_font *fonts, int n_fonts,
                      int shapes_min, int shapes_var)
{
    pdf_rect r;
    pdf_point poly[5];
    pdf_color c;
    int i, shapes = shapes_min + (int)(rng_next() % (unsigned)shapes_var);

    for (i = 0; i < shapes; i++)
    {
        switch (rng_next() % 5u)
        {
        case 0: /* filled rect */
            r.x = rng_range(0, 500); r.y = rng_range(0, 700);
            r.w = rng_range(4, 90);  r.h = rng_range(4, 90);
            c.kind = PDF_COLOR_RGB;
            c.c1 = rng_range(0, 1); c.c2 = rng_range(0, 1);
            c.c3 = rng_range(0, 1); c.c4 = 0;
            c.alpha = 1.0;
            pdf_set_fill_color(page, &c);
            pdf_page_draw_rect(page, &r, PDF_PATH_FILL);
            break;

        case 1: /* stroked polygon */
            {
                double cx = rng_range(60, 540), cy = rng_range(60, 740);
                double rad = rng_range(10, 50);
                int v, nv = 3 + (int)(rng_next() % 3u);
                for (v = 0; v < nv; v++)
                {
                    double a = 6.28318530718 * (double)v / (double)nv
                             + rng_range(-0.2, 0.2);
                    double rr = rad * (0.7 + rng_range(-0.3, 0.3));
                    poly[v].x = cx + rr * cos(a);
                    poly[v].y = cy + rr * sin(a);
                }
                c.kind = PDF_COLOR_GRAY;
                c.c1 = rng_range(0.1, 0.9); c.c2 = c.c3 = c.c4 = 0;
                c.alpha = 1.0;
                pdf_set_stroke_color(page, &c);
                pdf_path_polygon(page, poly, (size_t)nv);
                pdf_path_paint(page, PDF_PATH_STROKE);
            }
            break;

        case 2: /* bezier curve */
            pdf_path_move_to(page, rng_range(0, 600), rng_range(0, 800));
            pdf_path_curve_to(page,
                              rng_range(0, 600), rng_range(0, 800),
                              rng_range(0, 600), rng_range(0, 800),
                              rng_range(0, 600), rng_range(0, 800));
            pdf_path_paint(page, PDF_PATH_STROKE);
            break;

        case 3: /* text line */
            if (n_fonts > 0)
            {
                char text[64];
                int w, nw = 2 + (int)(rng_next() % 6u);
                int pos = 0;
                for (w = 0; w < nw; w++)
                {
                    const char *word = WORDS[rng_next() % (unsigned)N_WORDS];
                    pos += sprintf(text + pos, "%s%s", w ? " " : "", word);
                }
                pdf_page_draw_text(page,
                                   fonts[rng_next() % (unsigned)n_fonts],
                                   text, rng_range(20, 400),
                                   rng_range(40, 760), rng_range(7, 13));
            }
            break;

        default: /* gstate round-trip with per-call opacity (ExtGState) */
            pdf_gstate_save(page);
            r.x = rng_range(0, 500); r.y = rng_range(0, 700);
            r.w = rng_range(10, 60); r.h = rng_range(10, 60);
            c.kind = PDF_COLOR_CMYK;
            c.c1 = rng_range(0, 1); c.c2 = rng_range(0, 1);
            c.c3 = rng_range(0, 1); c.c4 = rng_range(0, 1);
            c.alpha = rng_range(0.3, 1.0);
            pdf_set_fill_color(page, &c);
            pdf_set_opacity(page, c.alpha, c.alpha);
            pdf_page_draw_rect(page, &r, PDF_PATH_FILL);
            pdf_gstate_restore(page);
            break;
        }
    }
}

/* ------------------------------------------------------------------ */
/* The run                                                             */
/* ------------------------------------------------------------------ */

typedef struct run_result
{
    int      pages;
    int      shapes_per_page;
    double   seconds;
    size_t   peak_rss;
    size_t   file_bytes;
    /* within-run checkpoints for the growth slope */
    size_t   rss_at_250;
    size_t   rss_at_1000;
    int      rc; /* first PDF error encountered, PDF_OK on success */
} run_result;

static int run_batch(int pages, int shapes_min, int shapes_var,
                     run_result *out)
{
    pdf_doc doc = NULL;
    pdf_font fonts[4];
    int n_fonts = 0, i;
    int rc;
    size_t base_rss = process_rss();

    memset(fonts, 0, sizeof(fonts));
    memset(out, 0, sizeof(*out));
    out->pages = pages;
    out->shapes_per_page = shapes_min + shapes_var / 2;
    rng_reset();

    rc = pdf_doc_create_streamed_to_file(OUT_FILE, &doc);
    if (rc != PDF_OK)
    {
        fprintf(stderr, "create_streamed_to_file failed rc=%d\n", rc);
        out->rc = rc;
        return rc;
    }

    {
        static const pdf_std_font kinds[4] = {
            PDF_STD_FONT_HELVETICA, PDF_STD_FONT_COURIER,
            PDF_STD_FONT_TIMES_ROMAN, PDF_STD_FONT_HELVETICA_BOLD
        };
        for (i = 0; i < 4; i++)
        {
            if (pdf_font_get_std(doc, kinds[i], &fonts[n_fonts]) == PDF_OK)
                n_fonts++;
        }
    }

    for (i = 0; i < pages; i++)
    {
        pdf_page page = NULL;
        double w = (i % 7 == 0) ? 612.0 : 595.0; /* mix custom sizes */
        double h = (i % 7 == 0) ? 792.0 : 842.0;

        rc = pdf_page_add_custom(doc, w, h, &page);
        if (rc != PDF_OK)
        {
            fprintf(stderr, "page %d: page_add failed rc=%d (%s)\n",
                    i, rc, pdflib_last_error(doc));
            pdf_doc_finish(doc);
            pdf_doc_free(doc);
            out->rc = rc;
            return rc;
        }
        fill_page(page, fonts, n_fonts, shapes_min, shapes_var);

        if (i == 249)
            out->rss_at_250 = process_rss();
        if (i == 999 && pages >= 1000)
            out->rss_at_1000 = process_rss();
        if ((i & 63) == 0)
        {
            size_t rss = process_rss();
            if (rss > out->peak_rss)
                out->peak_rss = rss;
        }
    }
    if (out->rss_at_1000 == 0)
        out->rss_at_1000 = process_rss();

    rc = pdf_doc_finish(doc);
    if (rc != PDF_OK)
    {
        fprintf(stderr, "finish failed rc=%d (%s)\n", rc,
                pdflib_last_error(doc));
        pdf_doc_free(doc);
        out->rc = rc;
        return rc;
    }
    pdf_doc_free(doc);

    out->seconds = (double)clock() / CLOCKS_PER_SEC;
    if (out->peak_rss < process_rss())
        out->peak_rss = process_rss();
    if (out->peak_rss < base_rss)
        out->peak_rss = base_rss;
    out->rc = PDF_OK;

    {
        FILE *f = fopen(OUT_FILE, "rb");
        if (f)
        {
            char sig[5] = {0};
            if (fseek(f, 0, SEEK_END) == 0)
                out->file_bytes = (size_t)ftell(f);
            rewind(f);
            if (fread(sig, 1, 5, f) != 5 || memcmp(sig, "%PDF-", 5) != 0)
                fprintf(stderr, "warning: output lacks %%PDF signature\n");
            fclose(f);
        }
    }
    return PDF_OK;
}

/* ------------------------------------------------------------------ */
/* Verification: constant per-page budget + content independence       */
/* ------------------------------------------------------------------ */

/* Fail when the within-run RSS growth per page exceeds this budget.
 * The PoDoFo page cache costs a few KB per page even with trivial
 * content; growth beyond ~12 KB/page means content is accumulating. */
#define RSS_PER_PAGE_BUDGET (12.0 * 1024.0)

static int check_growth(void)
{
    run_result small, big;
    double slope_small, slope_big;
    int have_slope = process_rss() != 0;

    printf("== memory growth check (within-run slopes) ==\n");

    /* light content first, then heavy: each slope is measured inside
     * its own run, so run order cannot pollute the comparison */
    if (run_batch(1000, 2, 4, &small) != PDF_OK)
        return small.rc ? small.rc : 1;
    printf("  light : shapes~%2d time=%5.2fs peak=%7.2fMB file=%6.2fMB\n",
           small.shapes_per_page, small.seconds, mb(small.peak_rss),
           mb(small.file_bytes));
    remove(OUT_FILE);

    if (run_batch(1000, 8, 16, &big) != PDF_OK)
        return big.rc ? big.rc : 1;
    printf("  heavy : shapes~%2d time=%5.2fs peak=%7.2fMB file=%6.2fMB\n",
           big.shapes_per_page, big.seconds, mb(big.peak_rss),
           mb(big.file_bytes));
    remove(OUT_FILE);

    if (!have_slope)
    {
        printf("  (no memory data on this platform; checks skipped)\n");
        return 0;
    }

    slope_small = (double)(small.rss_at_1000 - small.rss_at_250) / 750.0;
    slope_big = (double)(big.rss_at_1000 - big.rss_at_250) / 750.0;
    printf("  growth: light=%.2f KB/page  heavy=%.2f KB/page "
           "(page 250 -> 1000)\n",
           slope_small / 1024.0, slope_big / 1024.0);

    if (slope_small <= 0.0 || slope_small > RSS_PER_PAGE_BUDGET)
    {
        printf("  FAIL: light-content growth %.2f KB/page exceeds the "
               "%.2f KB/page budget\n",
               slope_small / 1024.0, RSS_PER_PAGE_BUDGET / 1024.0);
        return 1;
    }
    /* content independence: buffering would make the heavy slope grow
     * with the content volume (~4x here); allow modest overhead for the
     * extra per-page resource entries heavy content creates */
    if (slope_big > slope_small * 2.5 + 2.0 * 1024.0)
    {
        printf("  FAIL: heavy content grows %.1fx faster per page - "
               "content is being buffered\n",
               slope_big / (slope_small > 0 ? slope_small : 1));
        return 1;
    }
    printf("  PASS: memory is O(1) per page and independent of content "
           "volume\n");
    return 0;
}

int main(int argc, char **argv)
{
    int pages = argc > 1 ? atoi(argv[1]) : 1000;
    const char *backend = argc > 2 ? argv[2] : "podofo";
    run_result r;
    int rc;

    if (strcmp(backend, "podofo") == 0)
        pdflib_set_backend(PDF_BACKEND_PODOFO);
    else if (strcmp(backend, "libharu") == 0)
        pdflib_set_backend(PDF_BACKEND_LIBHARU);
    else
        pdflib_set_backend(PDF_BACKEND_DUMMY);

    printf("stress_streamed: backend=%s (%s)\n", backend,
           pdflib_backend_version());

    if (pages <= 0)
    {
        rc = check_growth();
        if (rc == PDF_ERR_BACKEND_UNSUPPORTED)
        {
            printf("  SKIP: backend has no streamed-document support\n");
            return 0;
        }
        if (rc == 0)
        {
            if (run_batch(1000, 20, 60, &r) != PDF_OK)
                return 1;
            printf("== final 1000-page rich-content run ==\n");
            printf("  time=%.2fs (%.0f pages/s)  peak_rss=%.2f MB  "
                   "file=%.2f MB\n",
                   r.seconds, (double)r.pages / (r.seconds > 0 ? r.seconds
                                                              : 1),
                   mb(r.peak_rss), mb(r.file_bytes));
            remove(OUT_FILE);
        }
        return rc;
    }

    rc = run_batch(pages, 20, 60, &r);
    if (rc != PDF_OK)
    {
        if (rc == PDF_ERR_BACKEND_UNSUPPORTED)
            printf("  SKIP: backend has no streamed-document support\n");
        return rc == PDF_ERR_BACKEND_UNSUPPORTED ? 0 : 1;
    }
    printf("  time=%.2fs (%.0f pages/s)  peak_rss=%.2f MB  file=%.2f MB\n",
           r.seconds, (double)r.pages / (r.seconds > 0 ? r.seconds : 1),
           mb(r.peak_rss), mb(r.file_bytes));
    remove(OUT_FILE);
    return 0;
}
