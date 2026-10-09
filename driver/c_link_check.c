/*
 * c_link_check.c - proof that pdflibdrv is a C interface, not just a
 * DLL that happens to be reachable from C.
 *
 * drv_harness.c resolves everything with GetProcAddress, so it cannot
 * tell a C ABI from a mangled one. This program is the other half:
 *
 *   - it is compiled as plain C (this is a .c file, /TC on MSVC);
 *   - it includes ONLY pdflibdrv.h and calls the functions by name;
 *   - it is LINKED against pdflibdrv.lib (the .def import library);
 *   - no GetProcAddress, no C++, no dllexport/dllimport in the header.
 *
 * If any export were C++-mangled, __stdcall-decorated, or missing from
 * the import library, this would fail at link time or at run time.
 *
 * Exit code 0 = linked and ran; non-zero identifies the failing step.
 */
#include "pdflibdrv.h"

#include <stdio.h>
#include <string.h>

static int    g_calls;
static size_t g_bytes;

static int PDCALL sink(void *ctx, const unsigned char *data, size_t len)
{
    (void)ctx;
    (void)data;
    g_calls++;
    g_bytes += len;
    return 0;
}

int main(void)
{
    unsigned int major = 0, minor = 0;
    pd_doc job = NULL;
    pd_page page = NULL;
    pd_status rc;

    if (PdflibDrvStartup() != PD_OK)
    {
        printf("FAIL: PdflibDrvStartup\n");
        return 1;
    }

    PdflibDrvGetAbiVersion(&major, &minor);
    if (major != PDFLIBDRV_ABI_MAJOR || minor != PDFLIBDRV_ABI_MINOR)
    {
        printf("FAIL: abi %u.%u\n", major, minor);
        return 2;
    }

    if (PdflibDrvJobCreate("c-link-check", sink, NULL, &job) != PD_OK)
    {
        printf("FAIL: PdflibDrvJobCreate\n");
        return 3;
    }

    rc = PdflibDrvPageAdd(job, 595.276, 841.89, &page);
    if (rc != PD_OK)
    {
        printf("FAIL: PdflibDrvPageAdd (%u)\n", rc);
        return 4;
    }

    rc = PdflibDrvPageDrawText(page, PD_FONT_HELVETICA, "linked from C",
                               72, 760, 24);
    if (rc != PD_OK)
    {
        printf("FAIL: PdflibDrvPageDrawText (%u)\n", rc);
        return 5;
    }

    rc = PdflibDrvPageDrawRect(page, 36, 36, 523, 770, PD_PAINT_STROKE);
    if (rc != PD_OK)
    {
        printf("FAIL: PdflibDrvPageDrawRect (%u)\n", rc);
        return 6;
    }

    if (PdflibDrvJobFinish(job) != PD_OK)
    {
        printf("FAIL: PdflibDrvJobFinish\n");
        return 7;
    }

    PdflibDrvShutdown();

    if (g_calls <= 0 || g_bytes == 0)
    {
        printf("FAIL: nothing was streamed\n");
        return 8;
    }

    printf("ok: linked against pdflibdrv.lib from C, abi %u.%u, "
           "pdflib %s, %d sink call(s), %lu byte(s)\n",
           major, minor, PdflibDrvVersionString(), g_calls,
           (unsigned long)g_bytes);
    return 0;
}
