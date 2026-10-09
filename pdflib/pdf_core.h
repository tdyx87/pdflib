/**
 * pdf_core.h - declarations shared between the core translation units.
 * Not part of the public API.
 */
#ifndef PDFLIB_PDF_CORE_H
#define PDFLIB_PDF_CORE_H

#include "pdf_api.h"
#include "pdf_backend.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PDFLIB_VERSION_STRING "1.0.0"

#define PDF_BACKEND_MAX 8

/* The registry lives in pdf_core.c; backends write into it through
 * pdf_backend_register(). */
extern const pdf_backend_vtable *g_registry[PDF_BACKEND_MAX];
extern pdf_backend g_active_id;

/* Registration entry points (weak in pdf_core.c when a backend is
 * compiled out of the build). */
void pdf_backend_libharu_register(void);
void pdf_backend_podofo_register(void);
void pdf_backend_dummy_register(void);

/** Ensure all compiled-in backends have registered (idempotent). */
void pdf_ensure_backends(void);

#ifdef __cplusplus
}
#endif

#endif /* PDFLIB_PDF_CORE_H */
