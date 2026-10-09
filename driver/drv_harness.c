/*
 * drv_harness.c - consumer-side contract test for pdflibdrv.dll.
 *
 * This is deliberately NOT linked against the DLL: it resolves every
 * export through GetProcAddress, exactly like a print driver would. That
 * makes the export names in pdflibdrv.def part of what is being tested.
 *
 * It exercises the parts of the ABI that would crash a print service
 * process if they were wrong:
 *   - every advertised export exists under its undecorated name;
 *   - a healthy streamed job produces a structurally complete PDF;
 *   - a sink that rejects data poisons the job, and the DLL stops
 *     delivering bytes (no silent truncation);
 *   - abort stops delivery and the handle is really invalidated;
 *   - forged / NULL / already-freed handles are rejected, not dereferenced;
 *   - zero-length sink calls are accepted;
 *   - Shutdown releases jobs that were never finished.
 *
 * Usage: drv_harness [path-to-pdflibdrv.dll]
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "pdflibdrv.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_checks;
static int g_failures;

static void check(int ok, const char *what)
{
    g_checks++;
    if (ok)
    {
        printf("  ok    %s\n", what);
    }
    else
    {
        g_failures++;
        printf("  FAIL  %s\n", what);
    }
}

/* ------------------------------------------------------------------ */
/* A collecting / fault-injecting sink                                 */
/* ------------------------------------------------------------------ */

typedef struct collector
{
    unsigned char *buf;
    size_t len;
    size_t cap;
    int calls;              /* total calls into the sink */
    int zero_calls;         /* calls with len == 0 */
    int fail_at;            /* fail on the Nth call (0 = never) */
    int failed;             /* the sink rejected data */
    int after_failure;      /* calls observed *after* it failed (must be 0) */
} collector;

static int PDCALL sink_cb(void *ctx, const unsigned char *data, size_t len)
{
    collector *c = (collector *)ctx;

    c->calls++;
    if (c->failed)
    {
        /* The DLL must never reach this: a failed sink is latched. */
        c->after_failure++;
        return 1;
    }
    if (c->fail_at && c->calls == c->fail_at)
    {
        c->failed = 1;
        return 7; /* any non-zero = permanent failure */
    }
    if (len == 0)
    {
        c->zero_calls++;
        return 0; /* zero-length must be accepted */
    }
    if (c->len + len > c->cap)
    {
        size_t ncap = c->cap ? c->cap * 2 : 4096;
        unsigned char *nb;
        while (ncap < c->len + len)
            ncap *= 2;
        nb = (unsigned char *)realloc(c->buf, ncap);
        if (!nb)
            return 1;
        c->buf = nb;
        c->cap = ncap;
    }
    memcpy(c->buf + c->len, data, len);
    c->len += len;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Small byte-search helpers (the buffer is binary, may contain NULs)  */
/* ------------------------------------------------------------------ */

static long find_bytes(const unsigned char *hay, size_t hay_len,
                       const char *needle)
{
    size_t nlen = strlen(needle);
    size_t i;

    if (nlen == 0 || hay_len < nlen)
        return -1;
    for (i = 0; i + nlen <= hay_len; i++)
    {
        if (memcmp(hay + i, needle, nlen) == 0)
            return (long)i;
    }
    return -1;
}

/* Parse the numeric argument of the last "startxref" and confirm it
 * points at an "xref" keyword - the cheapest proof the trailer is real. */
static int trailer_is_consistent(const unsigned char *buf, size_t len)
{
    long at = find_bytes(buf, len, "startxref");
    size_t p;
    unsigned long off = 0;

    if (at < 0)
        return 0;
    p = (size_t)at + strlen("startxref");
    while (p < len && (buf[p] == '\r' || buf[p] == '\n' || buf[p] == ' '))
        p++;
    while (p < len && buf[p] >= '0' && buf[p] <= '9')
    {
        off = off * 10 + (unsigned long)(buf[p] - '0');
        p++;
    }
    if (off + 4 > len)
        return 0;
    return memcmp(buf + off, "xref", 4) == 0;
}

static void collector_free(collector *c)
{
    free(c->buf);
    c->buf = NULL;
    c->len = c->cap = 0;
}

/* ------------------------------------------------------------------ */
/* API function pointers, resolved by name                             */
/* ------------------------------------------------------------------ */

typedef struct api
{
    pd_status (PDCALL *Startup)(void);
    void      (PDCALL *Shutdown)(void);
    void      (PDCALL *GetAbiVersion)(unsigned int *, unsigned int *);
    const char *(PDCALL *VersionString)(void);
    const char *(PDCALL *StatusString)(pd_status);
    pd_status (PDCALL *JobCreate)(const char *, pd_write_fn, void *, pd_doc *);
    pd_status (PDCALL *JobFinish)(pd_doc);
    pd_status (PDCALL *JobAbort)(pd_doc);
    const char *(PDCALL *JobLastError)(pd_doc);
    pd_status (PDCALL *PageAdd)(pd_doc, double, double, pd_page *);
    pd_status (PDCALL *PageSetRotation)(pd_page, int);
    pd_status (PDCALL *PageDrawText)(pd_page, unsigned int, const char *,
                                     double, double, double);
    pd_status (PDCALL *PageDrawRect)(pd_page, double, double, double, double,
                                     unsigned int);
    pd_status (PDCALL *PageDrawLine)(pd_page, double, double, double, double);
} api;

static const char *k_names[] = {
    "PdflibDrvStartup",       "PdflibDrvShutdown",
    "PdflibDrvGetAbiVersion", "PdflibDrvVersionString",
    "PdflibDrvStatusString",  "PdflibDrvJobCreate",
    "PdflibDrvJobFinish",     "PdflibDrvJobAbort",
    "PdflibDrvJobLastError",  "PdflibDrvPageAdd",
    "PdflibDrvPageSetRotation", "PdflibDrvPageDrawText",
    "PdflibDrvPageDrawRect",  "PdflibDrvPageDrawLine"
};

static int resolve_all(HMODULE h, api *a, void **raw, int n)
{
    int i;
    memset(a, 0, sizeof(*a));
    for (i = 0; i < n; i++)
    {
        raw[i] = (void *)GetProcAddress(h, k_names[i]);
        printf("  %-26s %s\n", k_names[i],
               raw[i] ? "found" : "MISSING");
    }
    a->Startup = (pd_status (PDCALL *)(void))raw[0];
    a->Shutdown = (void (PDCALL *)(void))raw[1];
    a->GetAbiVersion = (void (PDCALL *)(unsigned int *, unsigned int *))raw[2];
    a->VersionString = (const char *(PDCALL *)(void))raw[3];
    a->StatusString = (const char *(PDCALL *)(pd_status))raw[4];
    a->JobCreate = (pd_status (PDCALL *)(const char *, pd_write_fn, void *,
                                         pd_doc *))raw[5];
    a->JobFinish = (pd_status (PDCALL *)(pd_doc))raw[6];
    a->JobAbort = (pd_status (PDCALL *)(pd_doc))raw[7];
    a->JobLastError = (const char *(PDCALL *)(pd_doc))raw[8];
    a->PageAdd = (pd_status (PDCALL *)(pd_doc, double, double,
                                       pd_page *))raw[9];
    a->PageSetRotation = (pd_status (PDCALL *)(pd_page, int))raw[10];
    a->PageDrawText = (pd_status (PDCALL *)(pd_page, unsigned int,
                                            const char *, double, double,
                                            double))raw[11];
    a->PageDrawRect = (pd_status (PDCALL *)(pd_page, double, double, double,
                                            double, unsigned int))raw[12];
    a->PageDrawLine = (pd_status (PDCALL *)(pd_page, double, double, double,
                                            double))raw[13];
    for (i = 0; i < n; i++)
        if (!raw[i])
            return 0;
    return 1;
}

/* Draw a page worth of content so the streamed writer emits real bytes. */
static void draw_page(const api *a, pd_page page, int index)
{
    char label[64];
    snprintf(label, sizeof(label), "harness page %d", index);
    a->PageDrawRect(page, 36, 36, 523, 770, PD_PAINT_STROKE);
    a->PageDrawText(page, PD_FONT_HELVETICA, label, 72, 760, 24);
    a->PageDrawLine(page, 72, 700, 500, 700);
}

/* ------------------------------------------------------------------ */

int main(int argc, char **argv)
{
    const char *dll_path = argc > 1 ? argv[1] : "pdflibdrv.dll";
    HMODULE h;
    api a;
    void *raw[14];
    unsigned int abi_major = 0, abi_minor = 0;

    printf("pdflibdrv harness - loading %s\n", dll_path);

    h = LoadLibraryA(dll_path);
    if (!h)
    {
        printf("FATAL: LoadLibrary failed (%lu)\n", GetLastError());
        return 2;
    }

    printf("\n[1] export surface\n");
    check(resolve_all(h, &a, raw, 14), "all 14 exports resolved by name");
#if defined(_M_IX86)
    check(GetProcAddress(h, "_PdflibDrvJobCreate@16") == NULL,
          "no stdcall-decorated duplicate name (x86)");
#endif

    printf("\n[2] startup + ABI handshake\n");
    a.GetAbiVersion(&abi_major, &abi_minor);
    check(abi_major == PDFLIBDRV_ABI_MAJOR && abi_minor == PDFLIBDRV_ABI_MINOR,
          "ABI version matches the header");
    check(a.VersionString() != NULL, "version string available");
    check(a.Startup() == PD_OK, "Startup succeeds (PoDoFo present)");
    check(a.Startup() == PD_OK, "Startup is idempotent");

    printf("  info  pdflib %s / abi %u.%u\n", a.VersionString(), abi_major,
           abi_minor);

    printf("\n[3] healthy streamed job\n");
    {
        collector c;
        pd_doc job = NULL;
        pd_page page = NULL;
        pd_status rc;
        int i;

        memset(&c, 0, sizeof(c));
        rc = a.JobCreate("harness/healthy", sink_cb, &c, &job);
        check(rc == PD_OK, "JobCreate");

        for (i = 1; i <= 3; i++)
        {
            page = NULL;
            rc = a.PageAdd(job, 595.276, 841.89, &page);
            if (rc != PD_OK)
                break;
            draw_page(&a, page, i);
        }
        check(rc == PD_OK, "PageAdd + drawing");
        check(a.PageSetRotation(page, 90) == PD_OK, "PageSetRotation");

        rc = a.JobFinish(job);
        check(rc == PD_OK, "JobFinish");
        check(c.failed == 0, "sink never failed");
        check(c.len > 64, "bytes were streamed");
        check(c.buf && memcmp(c.buf, "%PDF-", 5) == 0, "output starts with %PDF-");
        check(find_bytes(c.buf, c.len, "%%EOF") >= 0, "output contains %%EOF");
        check(trailer_is_consistent(c.buf, c.len),
              "startxref points at an xref table");
        printf("  info  %d sink call(s), %d zero-length, %lu byte(s)\n",
               c.calls, c.zero_calls, (unsigned long)c.len);
        collector_free(&c);
    }

    printf("\n[4] failing sink poisons the job\n");
    {
        collector c;
        pd_doc job = NULL;
        pd_page page = NULL;
        pd_status rc;
        int i;

        memset(&c, 0, sizeof(c));
        c.fail_at = 3;
        rc = a.JobCreate("harness/fail", sink_cb, &c, &job);
        check(rc == PD_OK, "JobCreate still succeeds");

        for (i = 1; i <= 3; i++)
        {
            pd_status pr = a.PageAdd(job, 595.276, 841.89, &page);
            if (pr != PD_OK)
                break;
            draw_page(&a, page, i);
        }

        rc = a.JobFinish(job);
        check(c.failed == 1, "the sink failure actually triggered");
        check(rc == PD_ERR_SINK, "JobFinish reports PD_ERR_SINK");
        check(c.after_failure == 0,
              "no further delivery after the sink failed");
        printf("  info  %d sink call(s) total, rc=%u (%s)\n", c.calls, rc,
               a.StatusString(rc));
        collector_free(&c);
    }

    printf("\n[5] abort stops delivery\n");
    {
        collector c;
        pd_doc job = NULL;
        pd_page page = NULL;
        int calls_at_abort;
        int i;

        memset(&c, 0, sizeof(c));
        check(a.JobCreate("harness/abort", sink_cb, &c, &job) == PD_OK,
              "JobCreate");
        for (i = 1; i <= 3; i++)
        {
            if (a.PageAdd(job, 595.276, 841.89, &page) != PD_OK)
                break;
            draw_page(&a, page, i);
        }
        check(i > 1, "PageAdd");

        calls_at_abort = c.calls;
        check(calls_at_abort > 0, "bytes had already been delivered");
        check(a.JobAbort(job) == PD_OK, "JobAbort");
        check(c.calls == calls_at_abort, "no delivery during/after abort");
        check(a.JobAbort(job) == PD_ERR_INVALID_HANDLE,
              "second JobAbort rejected");
        printf("  info  %d sink call(s) before abort\n", calls_at_abort);
        collector_free(&c);
    }

    printf("\n[6] forged / invalid handles are errors, not crashes\n");
    check(a.JobFinish(NULL) == PD_ERR_INVALID_HANDLE, "JobFinish(NULL)");
    check(a.JobFinish((pd_doc)(void *)(size_t)0x1234) == PD_ERR_INVALID_HANDLE,
          "JobFinish(0x1234)");
    check(a.JobAbort((pd_doc)(void *)(size_t)0xdeadbeef)
              == PD_ERR_INVALID_HANDLE,
          "JobAbort(0xdeadbeef)");
    check(a.PageDrawText((pd_page)(void *)(size_t)0x20, PD_FONT_HELVETICA,
                         "x", 0, 0, 12) == PD_ERR_INVALID_HANDLE,
          "PageDrawText(forged page)");
    check(a.JobCreate("bad", NULL, NULL, NULL) == PD_ERR_INVALID_PARAM,
          "JobCreate(NULL sink)");
    {
        collector c;
        pd_doc job = NULL;
        pd_page page = NULL;
        memset(&c, 0, sizeof(c));
        check(a.JobCreate("harness/params", sink_cb, &c, &job) == PD_OK,
              "JobCreate for param checks");
        check(a.PageAdd(job, 10, 10, NULL) == PD_ERR_INVALID_PARAM,
              "PageAdd(NULL out_page)");
        check(a.PageAdd(job, 10, 10, &page) == PD_OK, "PageAdd okay");
        draw_page(&a, page, 1);
        check(a.JobFinish(job) == PD_OK, "cleanup");
        collector_free(&c);
    }

    printf("\n[7] finish/abort invalidate the handle\n");
    {
        collector c;
        pd_doc job = NULL;
        pd_page page = NULL;
        memset(&c, 0, sizeof(c));
        check(a.JobCreate("harness/double", sink_cb, &c, &job) == PD_OK,
              "JobCreate");
        check(a.PageAdd(job, 200, 200, &page) == PD_OK, "PageAdd");
        draw_page(&a, page, 1);
        check(a.JobFinish(job) == PD_OK, "first JobFinish");
        check(a.JobFinish(job) == PD_ERR_INVALID_HANDLE,
              "second JobFinish rejected");
        check(a.JobAbort(job) == PD_ERR_INVALID_HANDLE,
              "JobAbort after finish rejected");
        collector_free(&c);
    }

    printf("\n[8] Shutdown releases unfinished jobs\n");
    {
        collector c;
        pd_doc job = NULL;
        pd_page page = NULL;
        memset(&c, 0, sizeof(c));
        check(a.JobCreate("harness/leak", sink_cb, &c, &job) == PD_OK,
              "JobCreate");
        check(a.PageAdd(job, 200, 200, &page) == PD_OK, "PageAdd");
        draw_page(&a, page, 1);
        a.Shutdown();
        check(a.JobFinish(job) == PD_ERR_INVALID_HANDLE,
              "unfinished job is gone after Shutdown");
        check(a.Startup() == PD_OK, "Startup works again after Shutdown");
        collector_free(&c);
    }

    printf("\n%d check(s), %d failure(s)\n", g_checks, g_failures);
    FreeLibrary(h);
    return g_failures == 0 ? 0 : 1;
}
