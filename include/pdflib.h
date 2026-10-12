/**
 * pdflib.h - the ONE header a consumer of pdflib.dll includes.
 *
 * pdflib.dll wraps two PDF engines - libharu and PoDoFo - behind a single,
 * stable, pure-C ABI. It is designed to be called from ANY language:
 *
 *   Python   ctypes / cffi        (see examples/python/pdflib_demo.py)
 *   .NET     P/Invoke (DllImport)
 *   Java     JNA / JNI
 *   Go       cgo
 *   Rust     bindgen / extern "C"
 *   Node.js  ffi-napi / koffi
 *   Delphi   external 'pdflib.dll'
 *   VBA      Declare Function ... Lib "pdflib.dll"
 *   C / C++  link pdflib.lib (or LoadLibrary + GetProcAddress)
 *
 * Key properties:
 *   - Names are exported UNDECORATED on both x86 and x64 (via pdflib.def).
 *   - Calling convention is __cdecl (the x86 default); x64 has one
 *     convention only. There is no __stdcall name decoration to strip.
 *   - No C++ runtime, no libharu/PoDoFo headers, no other DLLs are needed
 *     at run time (both engines are linked statically into pdflib.dll).
 *   - Handles are opaque pointers; NULL is always an invalid handle and is
 *     rejected with PDF_ERR_INVALID_HANDLE rather than crashing.
 *   - Every function returns pdf_status (PDF_OK == 0) except the pure
 *     getters that return a value directly.
 *
 * STREAMING (the reason this DLL exists)
 * --------------------------------------
 * For a print-driver-style job the page count is unknown until the source
 * commands stop, and buffering every page in RAM is not an option. Two
 * mechanisms are provided:
 *
 *   1. Native streamed document (bounded memory, O(1) per page):
 *        pdf_doc_create_streamed(write_cb, ctx, &doc);  // sink bound up front
 *        for each sheet:
 *            pdf_page_add_custom(doc, w, h, &page);
 *            ... draw ...
 *        pdf_doc_finish(doc);       // flush subsets + xref/trailer
 *        pdf_doc_free(doc);
 *      Pages are pushed to the sink as they complete. PoDoFo implements
 *      this natively; backends without support return
 *      PDF_ERR_BACKEND_UNSUPPORTED so the caller can fall back to (2).
 *
 *   2. Save-to-stream on a regular document (the whole doc is buffered,
 *      then serialized to the sink in chunks):
 *        pdf_doc_create(&doc);
 *        ... draw everything ...
 *        pdf_doc_save_to_stream(doc, write_cb, ctx);
 *        pdf_doc_free(doc);
 *
 * A sink callback is `pdf_status (*)(const unsigned char *data, size_t len,
 * void *ctx)`. Return PDF_OK to accept bytes; any other value aborts and
 * poisons the document (no further bytes are delivered). The callback is
 * invoked on the calling thread, never concurrently.
 */

#ifndef PDFLIB_H
#define PDFLIB_H

/* The generated header carries every type and function declaration. It is
 * the very same header the core was built from, so the ABI can never drift
 * from what the caller compiles against. */
#include "pdflib/pdf_api.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* DLL-specific conveniences                                           */
/*                                                                     */
/* These three are NOT part of the engine API; they let a caller that    */
/* loaded the DLL dynamically verify what it got.                       */
/* ------------------------------------------------------------------ */

/** Identifies this DLL build, e.g. "pdflib.dll 1.0.0 (libharu + PoDoFo)".
 *  Static storage; do not free. */
const char *pdf_dll_version(void);

/** Comma-separated list of compiled-in engines, e.g. "libharu,podofo".
 *  Static storage; do not free. */
const char *pdf_dll_backends(void);

/** Returns PDF_OK when `backend` (a pdf_backend value) is compiled in and
 *  registered, PDF_ERR_BACKEND_NOT_FOUND otherwise. */
pdf_status pdf_dll_has_backend(int backend);

/** Human-readable text for a pdf_status value. Static storage; do not
 *  free. */
const char *pdf_dll_status_string(pdf_status status);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* PDFLIB_H */
