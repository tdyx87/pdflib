/**
 * pdflib_export.c - DLL-specific helpers exported by pdflib.dll.
 *
 * The bulk of the exported surface is the pdflib core itself
 * (pdflib/pdf_core.c): it is already a pure-C, backend-agnostic API, so the
 * DLL is produced by compiling the core with PDFLIB_SHARED + PDFLIB_BUILDING
 * (which turns PDFAPI into __declspec(dllexport)) plus pdflib.def, which
 * pins every exported name to its undecorated form on x86 and x64.
 *
 * Re-declaring the core functions here would be a C2375 "redefinition with
 * different linkage" error, so this file adds ONLY what the core does not:
 * a few introspection helpers a dynamically-loaded consumer needs to
 * identify the DLL and its engines.
 *
 * The exception barrier: the PoDoFo backend is C++ and may throw
 * (std::bad_alloc, PdfError). The core is plain C and does not catch, so a
 * throw would unwind out of the DLL into a caller that may have no C++
 * unwinding at all (ctypes, JNA, VB). pdflib_dll_guard_install() is a no-op
 * placeholder documenting that contract; the real protection is that the
 * core's few C++-touching entry points are wrapped inside the backend
 * adapter itself (see pdf_backend_podofo.cpp), and every entry point is
 * declared __cdecl so no callee-cleanup mismatch is possible.
 */

#include "pdf_api.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Export macro: same switch the core uses, so this TU's symbols land in
 * the DLL export table too. */
#if defined(_WIN32) && defined(PDFLIB_SHARED) && defined(PDFLIB_BUILDING)
#  define PDFDLL_API __declspec(dllexport)
#else
#  define PDFDLL_API
#endif

/** Identifies this DLL build. Static storage; do not free. */
PDFDLL_API const char *pdf_dll_version(void)
{
    return "pdflib.dll 1.0.0 (libharu + PoDoFo)";
}

/** Comma-separated list of the engines compiled into this DLL. */
PDFDLL_API const char *pdf_dll_backends(void)
{
    return "libharu,podofo";
}

/** PDF_OK when `backend` (a pdf_backend value) is compiled in and
 *  registered, PDF_ERR_BACKEND_NOT_FOUND otherwise. */
PDFDLL_API pdf_status pdf_dll_has_backend(int backend)
{
    return pdflib_has_backend((pdf_backend)backend) ? PDF_OK
                                                    : PDF_ERR_BACKEND_NOT_FOUND;
}

/** Human-readable text for a pdf_status value. Static storage. */
PDFDLL_API const char *pdf_dll_status_string(pdf_status s)
{
    switch (s) {
    case PDF_OK:                      return "ok";
    case PDF_ERR_UNKNOWN:             return "unknown error";
    case PDF_ERR_INVALID_HANDLE:      return "invalid handle";
    case PDF_ERR_INVALID_PARAM:       return "invalid parameter";
    case PDF_ERR_BACKEND_UNSUPPORTED: return "backend does not support this";
    case PDF_ERR_BACKEND_FAILED:      return "backend reported a failure";
    case PDF_ERR_BACKEND_NOT_FOUND:   return "no such backend registered";
    case PDF_ERR_OUT_OF_MEMORY:       return "out of memory";
    case PDF_ERR_IO:                  return "I/O error";
    default:                          return "unrecognized status";
    }
}

#ifdef __cplusplus
} /* extern "C" */
#endif
