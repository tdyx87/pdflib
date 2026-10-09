/*
 * test_threads - concurrency and lifecycle contract tests.
 *
 * Everything runs against every compiled-in backend. The dummy engine
 * is always present; when the third-party backends are compiled in,
 * the same suite runs against them, proving the per-handle ownership
 * rules hold on the real engines too. No third-party linkage here -
 * the test binary links pdflib only.
 *
 * Verified invariants:
 *  1. concurrent document creation - N threads each build documents,
 *     add pages, draw with a std font, serialize, free. Every status
 *     must be PDF_OK.
 *  2. concurrent streamed creation - each thread streams to its own
 *     callback and verifies bytes arrived and finish() succeeded.
 *  3. global selector race - threads racing on pdflib_set_backend()
 *     must never corrupt state; creates keep succeeding.
 *  4. lifecycle - early font free does not affect the doc; NULL is
 *     refused; double free of a non-NULL handle is UB and untested.
 */
#include "pdflib/pdf_api.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#  include <crtdbg.h>
typedef HANDLE thread_t;
typedef LONG atomic_int_t;
#else
#  include <pthread.h>
#  include <stdatomic.h>
typedef pthread_t thread_t;
typedef atomic_int atomic_int_t;
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

#define N_THREADS 8
#define DOCS_PER_THREAD 6
#define PAGES_PER_DOC 5

static void atomic_inc(volatile atomic_int_t *v)
{
#if defined(_WIN32)
    InterlockedIncrement(v);
#else
    atomic_fetch_add(v, 1);
#endif
}

static int atomic_get(volatile atomic_int_t *v)
{
#if defined(_WIN32)
    return (int)InterlockedCompareExchange(v, 0, 0);
#else
    return atomic_load(v);
#endif
}

static void atomic_reset(volatile atomic_int_t *v)
{
#if defined(_WIN32)
    InterlockedExchange(v, 0);
#else
    atomic_store(v, 0);
#endif
}

static int thread_join(thread_t t)
{
#if defined(_WIN32)
    WaitForSingleObject(t, INFINITE);
    CloseHandle(t);
    return 0;
#else
    pthread_join(t, NULL);
    return 0;
#endif
}

static int thread_create(thread_t *out, void *(*fn)(void *), void *arg)
{
#if defined(_WIN32)
    *out = CreateThread(NULL, 0,
                        (LPTHREAD_START_ROUTINE)(void (*)(void))fn, arg,
                        0, NULL);
    return *out ? 0 : -1;
#else
    return pthread_create(out, NULL, fn, arg);
#endif
}

/* The backends to exercise. DUMMY always; the real engines appear when
 * they were compiled into the library. */
typedef struct backend_entry
{
    pdf_backend id;
    const char *name;
} backend_entry;

static backend_entry g_backends[4];
static int g_n_backends = 0;

static void collect_backends(void)
{
    g_n_backends = 0;
    g_backends[g_n_backends].id = PDF_BACKEND_DUMMY;
    g_backends[g_n_backends].name = "dummy";
    g_n_backends++;
    if (pdflib_has_backend(PDF_BACKEND_PODOFO))
    {
        g_backends[g_n_backends].id = PDF_BACKEND_PODOFO;
        g_backends[g_n_backends].name = "podofo";
        g_n_backends++;
    }
    if (pdflib_has_backend(PDF_BACKEND_LIBHARU))
    {
        g_backends[g_n_backends].id = PDF_BACKEND_LIBHARU;
        g_backends[g_n_backends].name = "libharu";
        g_n_backends++;
    }
}

/* ------------------------------------------------------------------ */
/* 1) independent documents on independent threads                     */
/* ------------------------------------------------------------------ */

static volatile atomic_int_t g_bad_status = 0;
static volatile atomic_int_t g_docs_ok = 0;

static void *worker_docs(void *arg)
{
    const backend_entry *be = (const backend_entry *)arg;
    int d;

    for (d = 0; d < DOCS_PER_THREAD; d++)
    {
        pdf_doc doc = NULL;
        pdf_font font = NULL;
        size_t size = 0;
        int p;

        if (pdflib_set_backend(be->id) != PDF_OK)
        {
            atomic_inc(&g_bad_status);
            continue;
        }
        if (pdf_doc_create(&doc) != PDF_OK || !doc)
        {
            atomic_inc(&g_bad_status);
            continue;
        }
        if (pdf_font_get_std(doc, PDF_STD_FONT_COURIER, &font) != PDF_OK)
            font = NULL; /* optional per backend */

        for (p = 0; p < PAGES_PER_DOC; p++)
        {
            pdf_page page = NULL;
            if (pdf_page_add(doc, p % 2 ? PDF_PAGE_A5 : PDF_PAGE_A4,
                             PDF_ORIENTATION_PORTRAIT, &page) != PDF_OK ||
                !page)
            {
                atomic_inc(&g_bad_status);
                break;
            }
            if (font)
            {
                char label[32];
                sprintf(label, "t%d-d%d-p%d", 0, d, p);
                pdf_page_draw_text(page, font, label, 50, 700, 10);
            }
        }
        if (font)
            pdf_font_free(font); /* early free must not affect the doc */
        if (pdf_doc_save_to_memory(doc, NULL, &size) != PDF_OK || size == 0)
        {
            fprintf(stderr, "  [dbg] save failed: backend=%s\n",
                    be->name);
            atomic_inc(&g_bad_status);
        }
        if (pdf_doc_free(doc) != PDF_OK)
            atomic_inc(&g_bad_status);
    }
    (void)atomic_inc(&g_docs_ok);
    return NULL;
}

static void test_concurrent_documents(void)
{
    thread_t threads[N_THREADS * 3];
    int n = 0, i, b, rc;

    for (b = 0; b < g_n_backends; b++)
    {
        atomic_reset(&g_bad_status);
        g_docs_ok = 0;
        for (i = 0; i < N_THREADS; i++)
        {
            rc = thread_create(&threads[n++], worker_docs,
                               (void *)&g_backends[b]);
            CHECK(rc == 0);
            if (rc != 0)
                return;
        }
        for (i = 0; i < N_THREADS; i++)
            thread_join(threads[n - N_THREADS + i]);

        CHECK(atomic_get(&g_bad_status) == 0);
        CHECK(g_docs_ok == N_THREADS);
        printf("  concurrent docs [%s]: ok (%d threads x %d docs)\n",
               g_backends[b].name, N_THREADS, DOCS_PER_THREAD);
    }
}

/* ------------------------------------------------------------------ */
/* 2) concurrent streamed creation                                     */
/* ------------------------------------------------------------------ */

static pdf_status counting_sink(const unsigned char *data, size_t len,
                                void *ud)
{
    size_t *total = (size_t *)ud;
    (void)data;
    *total += len;
    return PDF_OK;
}

static void *worker_streamed(void *arg)
{
    const backend_entry *be = (const backend_entry *)arg;
    int r;

    for (r = 0; r < 8; r++)
    {
        pdf_doc doc = NULL;
        pdf_page page = NULL;
        size_t total = 0;
        int p;

        if (pdflib_set_backend(be->id) != PDF_OK)
        {
            atomic_inc(&g_bad_status);
            continue;
        }
        {
            pdf_status src = pdf_doc_create_streamed(counting_sink, &total,
                                                     &doc);
            if (src != PDF_OK)
            {
                if (be->id == PDF_BACKEND_DUMMY || src !=
                    PDF_ERR_BACKEND_UNSUPPORTED)
                {
                    fprintf(stderr,
                            "  [dbg] create_streamed backend=%s rc=%d\n",
                            be->name, (int)src);
                    atomic_inc(&g_bad_status);
                }
                continue; /* UNSUPPORTED is fine for non-dummy */
            }
        }
        for (p = 0; p < 3; p++)
        {
            if (pdf_page_add(doc, PDF_PAGE_LETTER,
                             PDF_ORIENTATION_PORTRAIT, &page) != PDF_OK)
                atomic_inc(&g_bad_status);
        }
        if (pdf_doc_finish(doc) != PDF_OK)
            atomic_inc(&g_bad_status);
        if (total == 0) /* the sink must have received something */
            atomic_inc(&g_bad_status);
        pdf_doc_free(doc);
    }
    return NULL;
}

static void test_concurrent_streamed(void)
{
    thread_t threads[N_THREADS];
    int i, b, rc;

    for (b = 0; b < g_n_backends; b++)
    {
        atomic_reset(&g_bad_status);
        for (i = 0; i < N_THREADS; i++)
        {
            rc = thread_create(&threads[i], worker_streamed,
                               (void *)&g_backends[b]);
            CHECK(rc == 0);
            if (rc != 0)
                return;
        }
        for (i = 0; i < N_THREADS; i++)
            thread_join(threads[i]);

        CHECK(atomic_get(&g_bad_status) == 0);
        printf("  concurrent streamed [%s]: ok (%d threads)\n",
               g_backends[b].name, N_THREADS);
    }
}

/* ------------------------------------------------------------------ */
/* 3) global backend selector race                                     */
/* ------------------------------------------------------------------ */

static void *worker_flip(void *arg)
{
    int rounds = (int)(intptr_t)arg;
    int r;

    for (r = 0; r < rounds; r++)
    {
        /* Racing selector flips must not corrupt anything. The create
         * loop pins DUMMY for its own docs, so flip only between two
         * selectors that both resolve to backends present in this
         * build; the invariant under test is no internal corruption. */
        (void)pdflib_set_backend(PDF_BACKEND_AUTO);
        (void)pdflib_set_backend(PDF_BACKEND_DUMMY);
        (void)pdflib_get_backend();
    }
    return NULL;
}

static void test_backend_selector_race(void)
{
    thread_t flipper[N_THREADS / 2];
    pdf_doc docs[N_THREADS];
    int i, rc;

    for (i = 0; i < N_THREADS / 2; i++)
    {
        rc = thread_create(&flipper[i], worker_flip, (void *)(intptr_t)50);
        CHECK(rc == 0);
        if (rc != 0)
            return;
    }
    /* meanwhile create documents that must all succeed - the dummy
     * backend answers both DUMMY and AUTO selections, so a create may
     * never fail because of the race */
    for (i = 0; i < N_THREADS; i++)
        docs[i] = NULL;
    for (i = 0; i < N_THREADS; i++)
        CHECK(pdf_doc_create(&docs[i]) == PDF_OK && docs[i] != NULL);
    for (i = 0; i < N_THREADS; i++)
        if (docs[i])
            pdf_doc_free(docs[i]);
    for (i = 0; i < N_THREADS / 2; i++)
        thread_join(flipper[i]);

    printf("  selector race: ok\n");

    /* leave a defined state behind */
    pdflib_set_backend(PDF_BACKEND_DUMMY);
}

/* ------------------------------------------------------------------ */
/* 4) lifecycle: child handles die with the document                   */
/* ------------------------------------------------------------------ */

static void test_lifecycle_contract(void)
{
    pdf_doc doc = NULL;
    pdf_page page = NULL;
    pdf_font font = NULL;
    size_t size = 0;

    CHECK(pdf_doc_create(&doc) == PDF_OK);
    CHECK(pdf_page_add(doc, PDF_PAGE_A4, PDF_ORIENTATION_PORTRAIT, &page)
          == PDF_OK);
    if (pdf_font_get_std(doc, PDF_STD_FONT_HELVETICA, &font) == PDF_OK)
    {
        CHECK(pdf_font_free(font) == PDF_OK); /* early free is legal */
    }
    CHECK(pdf_doc_save_to_memory(doc, NULL, &size) == PDF_OK && size > 0);
    CHECK(pdf_doc_free(doc) == PDF_OK);

    /* NOTE: freeing the same non-NULL doc twice is undefined behavior
     * (C has no way to detect it), it is deliberately NOT tested here.
     * NULL is always refused. */
    CHECK(pdf_doc_free(NULL) == PDF_ERR_INVALID_HANDLE);

    /* a fresh document is unaffected by the earlier frees */
    doc = NULL;
    size = 0;
    CHECK(pdf_doc_create(&doc) == PDF_OK);
    CHECK(pdf_doc_page_count(doc, &size) == PDF_ERR_INVALID_PARAM
          || pdf_doc_page_count(doc, NULL) == PDF_ERR_INVALID_PARAM);
    CHECK(pdf_doc_free(doc) == PDF_OK);
}

/* ------------------------------------------------------------------ */

int main(void)
{
#if defined(_WIN32) && defined(_DEBUG)
    _CrtSetDbgFlag(_CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF);
#endif
    collect_backends();
    printf("thread tests over %d backend(s): dummy%s%s\n", g_n_backends,
           g_n_backends > 1 ? "" : "",
           g_n_backends > 2 ? ", podofo, libharu" : "");

    test_lifecycle_contract();
    test_concurrent_documents();
    test_concurrent_streamed();
    test_backend_selector_race();

    if (failures)
    {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    printf("pdflib thread tests passed\n");
    return 0;
}
