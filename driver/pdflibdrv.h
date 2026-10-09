/*
 * pdflibdrv.h - the ONLY ABI surface a print driver / XPSDrv filter sees.
 *
 * This facade sits in front of the pdflib core so that a WDK user-mode
 * print driver can be written in plain C against a frozen, tiny,
 * stdcall-exported interface. The driver never sees libharu, PoDoFo or
 * any pdf_* symbol and never touches a FILE* or COM object: all output
 * goes through a caller-supplied sink callback.
 *
 * Rules for callers (a print pipeline filter in particular):
 *   - Call PdflibDrvStartup() once from InitializeFilter (or your
 *     equivalent), never from DllMain.
 *   - Every job is created with a sink, fed pages, then either
 *     PdflibDrvJobFinish()d (completes the PDF) or PdflibDrvJobAbort()ed.
 *     Ownership of the handle transfers: the handle is invalid after
 *     either call, and the document object is freed for you.
 *   - All handles are opaque. Passing a handle that is not a live one
 *     (NULL, forged, already finished) is a handled error, not a crash:
 *     the facade keeps its own registry and rejects unknown pointers.
 *   - The sink callback must not throw and must not call back into this
 *     DLL. Returning non-zero poisons the job permanently: the DLL stops
 *     delivering bytes and JobFinish reports the sink failure.
 *
 * Linking: the driver must NOT link an import library for names that it
 * also declares with its own prototype. Either use the .def-based
 * import library produced next to the DLL, or LoadLibrary/GetProcAddress.
 * The export names are exactly the PdflibDrv* identifiers below, with
 * no decoration (guaranteed by pdflibdrv.def, not by __declspec).
 *
 * Status codes mirror the core pdf_status values 0..8 so the two layers
 * cannot drift silently (checked at compile time inside the DLL).
 */
#ifndef PDFLIBDRV_H
#define PDFLIBDRV_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* Calling convention / ABI version                                    */
/* ------------------------------------------------------------------ */

#if defined(_MSC_VER)
#  define PDCALL __stdcall
#else
#  define PDCALL
#endif

/* Exports are driven purely by pdflibdrv.def; the header deliberately
 * defines no dllexport/dllimport. A consumer that wants an import lib
 * should link the one generated from the .def. */
#define PDFLIBDRV_ABI_MAJOR 1u
#define PDFLIBDRV_ABI_MINOR 0u

typedef unsigned int pd_status;
typedef void        *pd_doc;   /* opaque job handle */
typedef void        *pd_page;  /* opaque page handle, valid while its job lives */

/* ------------------------------------------------------------------ */
/* Status codes (numerically equal to the core pdf_status)             */
/* ------------------------------------------------------------------ */

enum
{
    PD_OK = 0,
    PD_ERR_UNKNOWN = 1,
    PD_ERR_INVALID_HANDLE = 2,
    PD_ERR_INVALID_PARAM = 3,
    PD_ERR_BACKEND_UNSUPPORTED = 4,
    PD_ERR_BACKEND_FAILED = 5,
    PD_ERR_BACKEND_NOT_FOUND = 6,
    PD_ERR_OUT_OF_MEMORY = 7,
    PD_ERR_IO = 8,
    /* facade-only: the driver's sink callback rejected data */
    PD_ERR_SINK = 100
};

/* Standard fonts by index; the numbering is a stable part of the ABI. */
enum
{
    PD_FONT_TIMES_ROMAN = 0,
    PD_FONT_TIMES_ITALIC,
    PD_FONT_TIMES_BOLD,
    PD_FONT_TIMES_BOLD_ITALIC,
    PD_FONT_HELVETICA,
    PD_FONT_HELVETICA_OBLIQUE,
    PD_FONT_HELVETICA_BOLD,
    PD_FONT_HELVETICA_BOLD_OBLIQUE,
    PD_FONT_COURIER,
    PD_FONT_COURIER_OBLIQUE,
    PD_FONT_COURIER_BOLD,
    PD_FONT_COURIER_BOLD_OBLIQUE,
    PD_FONT_SYMBOL,
    PD_FONT_ZAPF_DINGBATS,
    PD_FONT_COUNT
};

/* Path painting modes for PdflibDrvPageDrawRect. */
enum
{
    PD_PAINT_STROKE = 0,
    PD_PAINT_FILL = 1,
    PD_PAINT_FILL_STROKE = 2
};

/* ------------------------------------------------------------------ */
/* Sink: implemented by the DRIVER, called by the DLL                  */
/* ------------------------------------------------------------------ */

/*
 * Deliver `len` bytes to the driver's output. Contract:
 *   - append-only, in order, and called with len == 0 at least once is
 *     allowed (treat it as a no-op, do not fail it);
 *   - returning 0 accepts the bytes; any non-zero value is a PERMANENT
 *     failure -> the job is poisoned and no further bytes are sent;
 *   - must not throw and must not re-enter the DLL;
 *   - `ctx` is the pointer passed to PdflibDrvJobCreate.
 */
typedef int (PDCALL *pd_write_fn)(void *ctx, const unsigned char *data,
                                  size_t len);

/* ------------------------------------------------------------------ */
/* Module lifetime                                                     */
/* ------------------------------------------------------------------ */

/* One-time init. Idempotent. Returns PD_OK, or PD_ERR_BACKEND_NOT_FOUND
 * when no PDF engine was compiled into this DLL. Call from your filter's
 * InitializeFilter, NOT from DllMain. */
pd_status PDCALL PdflibDrvStartup(void);

/* Releases every job that was never finished/aborted. After this the
 * handle space is empty and Startup must be called again to create jobs. */
void PDCALL PdflibDrvShutdown(void);

/* Reports the ABI the DLL was built against. Pass either pointer as NULL
 * to skip it. */
void PDCALL PdflibDrvGetAbiVersion(unsigned int *major, unsigned int *minor);

/* pdflib core version string (static storage; do not free). */
const char *PDCALL PdflibDrvVersionString(void);

/* Human-readable text for a pd_status value (static storage). */
const char *PDCALL PdflibDrvStatusString(pd_status status);

/* ------------------------------------------------------------------ */
/* Job                                                                 */
/* ------------------------------------------------------------------ */

/* Create a streamed job. Output is pushed to `write` as pages complete,
 * so peak memory is bounded by one page, not the whole document.
 * `utf8_title` may be NULL. On success *out_job receives the handle. */
pd_status PDCALL PdflibDrvJobCreate(const char *utf8_title,
                                    pd_write_fn write, void *ctx,
                                    pd_doc *out_job);

/* Complete the document (xref/trailer + pending font subsets) and free
 * the job. The handle is invalid afterwards. Returns PD_OK, or a backend
 * error, or PD_ERR_SINK if the sink failed earlier. */
pd_status PDCALL PdflibDrvJobFinish(pd_doc job);

/* Discard the job without completing the file and free the handle. No
 * further bytes are delivered to the sink. (Tearing down anything the
 * sink already buffered is the driver's business.) */
pd_status PDCALL PdflibDrvJobAbort(pd_doc job);

/* Last error text for a still-live job. Valid until finish/abort; do not
 * free. Returns "" for an unknown handle or no error. */
const char *PDCALL PdflibDrvJobLastError(pd_doc job);

/* ------------------------------------------------------------------ */
/* Pages                                                               */
/* ------------------------------------------------------------------ */

/* Append a page of `w` x `h` PDF points (1/72 inch, origin bottom-left). */
pd_status PDCALL PdflibDrvPageAdd(pd_doc job, double w, double h,
                                  pd_page *out_page);

/* Rotate the page content clockwise by a multiple of 90. */
pd_status PDCALL PdflibDrvPageSetRotation(pd_page page, int degrees);

/* ------------------------------------------------------------------ */
/* Drawing                                                             */
/* ------------------------------------------------------------------ */

/* Draw a single-line UTF-8 string; (x, y) is the baseline origin. */
pd_status PDCALL PdflibDrvPageDrawText(pd_page page, unsigned int std_font,
                                       const char *utf8_text,
                                       double x, double y, double size);

/* Draw an axis-aligned rectangle. */
pd_status PDCALL PdflibDrvPageDrawRect(pd_page page,
                                       double x, double y,
                                       double w, double h,
                                       unsigned int paint);

/* Draw a straight line. */
pd_status PDCALL PdflibDrvPageDrawLine(pd_page page,
                                       double x1, double y1,
                                       double x2, double y2);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* PDFLIBDRV_H */
