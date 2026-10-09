/*
 * pdflibdrv.cpp - C ABI facade in front of the pdflib core.
 *
 * This translation unit is deliberately C++ even though the interface is
 * pure C: it is the single place where PoDoFo exceptions have to stop
 * before they reach the driver, so every public entry point contains a
 * catch(...) boundary. It is linked against the static pdflib core, so
 * the core's 80-odd pdf_* entry points are NOT re-exported (the .def is
 * the only export list).
 *
 * Layering:
 *
 *   driver sink  <-- pd_write_fn (stdcall, returns int)   [driver owns]
 *        ^
 *        |  sink_trampoline: maps non-zero to PD_ERR_SINK and latches it
 *        |
 *   pdflib core  <-- pdf_doc_create_streamed(pdf_stream_write_fn) [...]
 *
 * The core poisons a streamed document whose sink fails (see
 * mark_stream_broken in pdf_core.c), so a failing driver sink can never
 * silently truncate a PDF: JobFinish reports PD_ERR_SINK.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "pdflibdrv.h"

extern "C" {
#include "pdflib/pdf_api.h"
}

#include <stdlib.h>
#include <string.h>

/* --- the facade's status codes must not drift from the core's -------- */

static_assert((int)PD_OK == (int)PDF_OK, "status code drift");
static_assert((int)PD_ERR_UNKNOWN == (int)PDF_ERR_UNKNOWN, "status code drift");
static_assert((int)PD_ERR_INVALID_HANDLE == (int)PDF_ERR_INVALID_HANDLE, "status code drift");
static_assert((int)PD_ERR_INVALID_PARAM == (int)PDF_ERR_INVALID_PARAM, "status code drift");
static_assert((int)PD_ERR_BACKEND_UNSUPPORTED == (int)PDF_ERR_BACKEND_UNSUPPORTED, "status code drift");
static_assert((int)PD_ERR_BACKEND_FAILED == (int)PDF_ERR_BACKEND_FAILED, "status code drift");
static_assert((int)PD_ERR_BACKEND_NOT_FOUND == (int)PDF_ERR_BACKEND_NOT_FOUND, "status code drift");
static_assert((int)PD_ERR_OUT_OF_MEMORY == (int)PDF_ERR_OUT_OF_MEMORY, "status code drift");
static_assert((int)PD_ERR_IO == (int)PDF_ERR_IO, "status code drift");

/* standard-font indices and paint modes are part of the ABI too */
static_assert((int)PD_FONT_TIMES_ROMAN == (int)PDF_STD_FONT_TIMES_ROMAN, "font index drift");
static_assert((int)PD_FONT_ZAPF_DINGBATS == (int)PDF_STD_FONT_ZAPF_DINGBATS, "font index drift");
static_assert((int)PD_FONT_COUNT == (int)PDF_STD_FONT_ZAPF_DINGBATS + 1, "font count drift");
static_assert((int)PD_PAINT_STROKE == (int)PDF_PATH_STROKE, "paint mode drift");
static_assert((int)PD_PAINT_FILL == (int)PDF_PATH_FILL, "paint mode drift");
static_assert((int)PD_PAINT_FILL_STROKE == (int)PDF_PATH_FILL_STROKE, "paint mode drift");

namespace {

const unsigned kJobMagic  = 0x4a4f4244u; /* 'JOBD' */
const unsigned kPageMagic = 0x50414745u; /* 'PAGE' */

struct Job;

struct Page
{
    unsigned  magic;
    Page     *next;
    Job      *job;
    pdf_page  page;
};

struct Job
{
    unsigned   magic;
    Job       *next;
    pdf_doc    doc;
    pd_write_fn write;
    void      *ctx;
    int        aborted;   /* driver asked to stop; no more bytes leave */
    int        failed;    /* sink rejected data: poisoned */
    pd_status  last_err;
    /* lazily created standard fonts, owned by the document */
    pdf_font   font[PD_FONT_COUNT];
    unsigned char font_tried[PD_FONT_COUNT];
};

CRITICAL_SECTION g_lock;
/* One-time init state: 0 = untouched, 1 = being initialised, 2 = ready.
 * Deliberately Interlocked rather than InitOnceExecuteOnce: the InitOnce
 * family is Windows Vista+ and this DLL has to load on Windows XP. The
 * core uses the same pattern in pdf_ensure_backends(). */
volatile LONG    g_init_state = 0;
int              g_started = 0;
Job             *g_jobs  = NULL;
Page            *g_pages = NULL;
char             g_version[64] = "0.0.0";

void ensure_init(void)
{
    if (InterlockedCompareExchange(&g_init_state, 2, 2) == 2)
        return; /* already published */

    if (InterlockedCompareExchange(&g_init_state, 1, 0) == 0)
    {
        /* We won the race; the others wait until we publish state 2. */
        InitializeCriticalSection(&g_lock);
        const char *v = pdflib_version();
        if (v)
        {
            strncpy_s(g_version, sizeof(g_version), v, _TRUNCATE);
        }
        InterlockedExchange(&g_init_state, 2);
        return;
    }

    while (InterlockedCompareExchange(&g_init_state, 2, 2) != 2)
    {
        Sleep(0); /* initialization is short; let the winning thread run */
    }
}

pd_status to_pd(pdf_status rc)
{
    if (rc == PDF_OK)
        return PD_OK;
    return (pd_status)rc;
}

/* --- registry lookups (caller holds g_lock) -------------------------- */

Job *find_job_locked(pd_doc handle)
{
    Job *j;
    if (!handle)
        return NULL;
    for (j = g_jobs; j; j = j->next)
        if (j == (Job *)handle && j->magic == kJobMagic)
            return j;
    return NULL;
}

Page *find_page_locked(pd_page handle)
{
    Page *p;
    if (!handle)
        return NULL;
    for (p = g_pages; p; p = p->next)
        if (p == (Page *)handle && p->magic == kPageMagic)
            return p;
    return NULL;
}

void unlink_job_locked(Job *j)
{
    Job **pp = &g_jobs;
    while (*pp)
    {
        if (*pp == j)
        {
            *pp = j->next;
            j->next = NULL;
            j->magic = 0; /* any stale handle to it is now rejected */
            return;
        }
        pp = &(*pp)->next;
    }
}

void unlink_pages_of_locked(Job *j)
{
    Page **pp = &g_pages;
    while (*pp)
    {
        if ((*pp)->job == j)
        {
            Page *dead = *pp;
            *pp = dead->next;
            dead->magic = 0;
            free(dead);
        }
        else
        {
            pp = &(*pp)->next;
        }
    }
}

void destroy_job(Job *j)
{
    EnterCriticalSection(&g_lock);
    unlink_pages_of_locked(j);
    LeaveCriticalSection(&g_lock);

    if (j->doc)
    {
        /* Frees the document and every font/page it owns. */
        pdf_doc_free(j->doc);
        j->doc = NULL;
    }
    j->magic = 0;
    free(j);
}

/* --- the sink bridge -------------------------------------------------- */

pdf_status sink_trampoline(const unsigned char *data, size_t len, void *ud)
{
    Job *j = (Job *)ud;

    if (!j || j->aborted)
        return PDF_ERR_IO; /* nothing more leaves this job */
    if (!j->write)
        return PDF_ERR_INVALID_PARAM;

    if (j->write(j->ctx, data, len) != 0)
    {
        /* Latch it; the core will refuse further deliveries and the
         * failure surfaces from JobFinish as PD_ERR_SINK. */
        j->failed = 1;
        j->last_err = PD_ERR_SINK;
        return PDF_ERR_IO;
    }
    return PDF_OK;
}

pdf_font std_font(Job *j, unsigned int index)
{
    if (index >= PD_FONT_COUNT)
        return NULL;
    if (!j->font_tried[index])
    {
        j->font_tried[index] = 1;
        if (pdf_font_get_std(j->doc, (pdf_std_font)index, &j->font[index])
            != PDF_OK)
            j->font[index] = NULL;
    }
    return j->font[index];
}

} /* namespace */

/* ------------------------------------------------------------------ */
/* Module lifetime                                                     */
/* ------------------------------------------------------------------ */

extern "C" pd_status PDCALL PdflibDrvStartup(void)
{
    try
    {
        ensure_init();
        if (!pdflib_has_backend(PDF_BACKEND_PODOFO))
            return PD_ERR_BACKEND_NOT_FOUND;
        /* Serialised with job creation below; the core selector is
         * process-global, so this must not run concurrently with a
         * doc_create. */
        EnterCriticalSection(&g_lock);
        pdflib_set_backend(PDF_BACKEND_PODOFO);
        g_started = 1;
        LeaveCriticalSection(&g_lock);
        return PD_OK;
    }
    catch (...)
    {
        return PD_ERR_BACKEND_FAILED;
    }
}

extern "C" void PDCALL PdflibDrvShutdown(void)
{
    try
    {
        ensure_init();
        for (;;)
        {
            Job *j;
            EnterCriticalSection(&g_lock);
            j = g_jobs;
            if (j)
                unlink_job_locked(j);
            g_started = 0;
            LeaveCriticalSection(&g_lock);
            if (!j)
                break;
            destroy_job(j);
        }
    }
    catch (...)
    {
        /* shutdown must never propagate anything into the driver */
    }
}

extern "C" void PDCALL PdflibDrvGetAbiVersion(unsigned int *major,
                                             unsigned int *minor)
{
    if (major)
        *major = PDFLIBDRV_ABI_MAJOR;
    if (minor)
        *minor = PDFLIBDRV_ABI_MINOR;
}

extern "C" const char *PDCALL PdflibDrvVersionString(void)
{
    ensure_init();
    return g_version;
}

extern "C" const char *PDCALL PdflibDrvStatusString(pd_status status)
{
    switch (status)
    {
    case PD_OK: return "ok";
    case PD_ERR_UNKNOWN: return "unknown error";
    case PD_ERR_INVALID_HANDLE: return "invalid handle";
    case PD_ERR_INVALID_PARAM: return "invalid parameter";
    case PD_ERR_BACKEND_UNSUPPORTED: return "backend does not support this";
    case PD_ERR_BACKEND_FAILED: return "backend failure";
    case PD_ERR_BACKEND_NOT_FOUND: return "no backend compiled in";
    case PD_ERR_OUT_OF_MEMORY: return "out of memory";
    case PD_ERR_IO: return "i/o error";
    case PD_ERR_SINK: return "output sink rejected data";
    default: return "unrecognised status";
    }
}

/* ------------------------------------------------------------------ */
/* Job                                                                 */
/* ------------------------------------------------------------------ */

extern "C" pd_status PDCALL PdflibDrvJobCreate(const char *utf8_title,
                                              pd_write_fn write, void *ctx,
                                              pd_doc *out_job)
{
    Job *j;
    pdf_doc doc = NULL;
    pdf_status rc = PDF_OK;

    if (out_job)
        *out_job = NULL;
    if (!out_job || !write)
        return PD_ERR_INVALID_PARAM;

    try
    {
        ensure_init();
        j = (Job *)calloc(1, sizeof(*j));
        if (!j)
            return PD_ERR_OUT_OF_MEMORY;
        j->magic = kJobMagic;
        j->write = write;
        j->ctx = ctx;
        j->last_err = PD_OK;

        EnterCriticalSection(&g_lock);
        if (!g_started)
        {
            LeaveCriticalSection(&g_lock);
            free(j);
            return PD_ERR_INVALID_PARAM;
        }
        rc = pdf_doc_create_streamed(sink_trampoline, j, &doc);
        if (rc == PDF_OK)
        {
            if (utf8_title && *utf8_title)
                pdf_doc_set_info(doc, PDF_INFO_TITLE, utf8_title);
            j->doc = doc;
            j->next = g_jobs;
            g_jobs = j;
        }
        LeaveCriticalSection(&g_lock);

        if (rc != PDF_OK)
        {
            free(j);
            return to_pd(rc);
        }
        *out_job = (pd_doc)j;
        return PD_OK;
    }
    catch (...)
    {
        if (doc)
            pdf_doc_free(doc);
        return PD_ERR_BACKEND_FAILED;
    }
}

extern "C" pd_status PDCALL PdflibDrvJobFinish(pd_doc job)
{
    Job *j;
    pdf_status rc;

    try
    {
        EnterCriticalSection(&g_lock);
        j = find_job_locked(job);
        if (!j)
        {
            LeaveCriticalSection(&g_lock);
            return PD_ERR_INVALID_HANDLE;
        }
        /* Take sole ownership immediately so a second concurrent Finish
         * of the same handle fails instead of double-freeing. */
        unlink_job_locked(j);
        LeaveCriticalSection(&g_lock);

        rc = pdf_doc_finish(j->doc);
        {
            pd_status pd = to_pd(rc);
            if (pd == PD_OK && j->failed)
                pd = PD_ERR_SINK; /* sink tripped but backend still "ok" */
            if (pd != PD_OK && j->last_err != PD_OK)
                pd = j->last_err;
            destroy_job(j);
            return pd;
        }
    }
    catch (...)
    {
        return PD_ERR_BACKEND_FAILED;
    }
}

extern "C" pd_status PDCALL PdflibDrvJobAbort(pd_doc job)
{
    Job *j;

    try
    {
        EnterCriticalSection(&g_lock);
        j = find_job_locked(job);
        if (!j)
        {
            LeaveCriticalSection(&g_lock);
            return PD_ERR_INVALID_HANDLE;
        }
        unlink_job_locked(j);
        j->aborted = 1; /* the sink gets no more bytes from here on */
        LeaveCriticalSection(&g_lock);

        destroy_job(j);
        return PD_OK;
    }
    catch (...)
    {
        return PD_ERR_BACKEND_FAILED;
    }
}

extern "C" const char *PDCALL PdflibDrvJobLastError(pd_doc job)
{
    Job *j;
    const char *msg = "";

    try
    {
        EnterCriticalSection(&g_lock);
        j = find_job_locked(job);
        if (j)
            msg = pdflib_last_error(j->doc); /* valid while j is live */
        else if (job)
            msg = "invalid handle";
        else
            msg = "null handle";
        LeaveCriticalSection(&g_lock);
    }
    catch (...)
    {
        msg = "internal error";
    }
    return msg;
}

/* ------------------------------------------------------------------ */
/* Pages                                                               */
/* ------------------------------------------------------------------ */

extern "C" pd_status PDCALL PdflibDrvPageAdd(pd_doc job, double w, double h,
                                            pd_page *out_page)
{
    Job *j;
    Page *p;
    pdf_page page = NULL;
    pdf_status rc;

    if (out_page)
        *out_page = NULL;
    if (!out_page)
        return PD_ERR_INVALID_PARAM;

    try
    {
        EnterCriticalSection(&g_lock);
        j = find_job_locked(job);
        if (!j)
        {
            LeaveCriticalSection(&g_lock);
            return PD_ERR_INVALID_HANDLE;
        }
        rc = pdf_page_add_custom(j->doc, w, h, &page);
        if (rc == PDF_OK)
        {
            p = (Page *)calloc(1, sizeof(*p));
            if (!p)
            {
                LeaveCriticalSection(&g_lock);
                return PD_ERR_OUT_OF_MEMORY;
            }
            p->magic = kPageMagic;
            p->job = j;
            p->page = page;
            p->next = g_pages;
            g_pages = p;
            *out_page = (pd_page)p;
        }
        LeaveCriticalSection(&g_lock);
        return to_pd(rc);
    }
    catch (...)
    {
        return PD_ERR_BACKEND_FAILED;
    }
}

extern "C" pd_status PDCALL PdflibDrvPageSetRotation(pd_page page, int degrees)
{
    Page *p;
    pdf_status rc;

    try
    {
        EnterCriticalSection(&g_lock);
        p = find_page_locked(page);
        if (!p)
        {
            LeaveCriticalSection(&g_lock);
            return PD_ERR_INVALID_HANDLE;
        }
        rc = pdf_page_set_rotation(p->page, degrees);
        LeaveCriticalSection(&g_lock);
        return to_pd(rc);
    }
    catch (...)
    {
        return PD_ERR_BACKEND_FAILED;
    }
}

/* ------------------------------------------------------------------ */
/* Drawing                                                             */
/* ------------------------------------------------------------------ */

extern "C" pd_status PDCALL PdflibDrvPageDrawText(pd_page page,
                                                 unsigned int std_font_index,
                                                 const char *utf8_text,
                                                 double x, double y,
                                                 double size)
{
    Page *p;
    pdf_font f;
    pdf_status rc;

    if (!utf8_text)
        return PD_ERR_INVALID_PARAM;

    try
    {
        EnterCriticalSection(&g_lock);
        p = find_page_locked(page);
        if (!p)
        {
            LeaveCriticalSection(&g_lock);
            return PD_ERR_INVALID_HANDLE;
        }
        f = std_font(p->job, std_font_index);
        if (!f)
        {
            LeaveCriticalSection(&g_lock);
            return PD_ERR_BACKEND_FAILED;
        }
        rc = pdf_page_draw_text(p->page, f, utf8_text, x, y, size);
        LeaveCriticalSection(&g_lock);
        return to_pd(rc);
    }
    catch (...)
    {
        return PD_ERR_BACKEND_FAILED;
    }
}

extern "C" pd_status PDCALL PdflibDrvPageDrawRect(pd_page page,
                                                 double x, double y,
                                                 double w, double h,
                                                 unsigned int paint)
{
    Page *p;
    pdf_rect r;
    pdf_path_paint_mode mode;
    pdf_status rc;

    if (paint > (unsigned int)PD_PAINT_FILL_STROKE)
        return PD_ERR_INVALID_PARAM;

    try
    {
        EnterCriticalSection(&g_lock);
        p = find_page_locked(page);
        if (!p)
        {
            LeaveCriticalSection(&g_lock);
            return PD_ERR_INVALID_HANDLE;
        }
        r.x = x;
        r.y = y;
        r.w = w;
        r.h = h;
        mode = (pdf_path_paint_mode)paint;
        rc = pdf_page_draw_rect(p->page, &r, mode);
        LeaveCriticalSection(&g_lock);
        return to_pd(rc);
    }
    catch (...)
    {
        return PD_ERR_BACKEND_FAILED;
    }
}

extern "C" pd_status PDCALL PdflibDrvPageDrawLine(pd_page page,
                                                 double x1, double y1,
                                                 double x2, double y2)
{
    Page *p;
    pdf_status rc;

    try
    {
        EnterCriticalSection(&g_lock);
        p = find_page_locked(page);
        if (!p)
        {
            LeaveCriticalSection(&g_lock);
            return PD_ERR_INVALID_HANDLE;
        }
        rc = pdf_page_draw_line(p->page, x1, y1, x2, y2);
        LeaveCriticalSection(&g_lock);
        return to_pd(rc);
    }
    catch (...)
    {
        return PD_ERR_BACKEND_FAILED;
    }
}

/* ------------------------------------------------------------------ */
/* DllMain: not a place to do work.                                    */
/* ------------------------------------------------------------------ */

extern "C" BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
        DisableThreadLibraryCalls(hinst); /* our own HINSTANCE, not the EXE's */
    return TRUE;
}
