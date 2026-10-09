/**
 * pdf_internal.h - internal handle layouts shared between the core and
 * the backends. Not part of the public API.
 *
 * Every public handle is a small core-owned cell that points at the
 * backend-specific object. This keeps object lifetimes uniform: the core
 * allocates/frees the cells, the backend owns the payload behind them.
 */
#ifndef PDFLIB_PDF_INTERNAL_H
#define PDFLIB_PDF_INTERNAL_H

#include "pdf_api.h"
#include "pdf_backend.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Internal cell tags. They deliberately differ from the public typedef
 * names: in C++ the typedef `pdf_doc` and the tag `struct pdf_doc` share
 * one identifier, which breaks the backend translation units. */
struct pdf_doc_cell
{
    const pdf_backend_vtable *vt;
    pdf_backend_doc *backend_doc;
    char error[256];
    int streamed;          /* created via pdf_doc_create_streamed* */
    int finished;          /* streamed doc already completed */
    pdf_status stream_error; /* sink callback failed: torn output, fail fast; 0=healthy */
    void *close_handle;    /* core-owned FILE* to close on finish (path ctor) */
    char *sink_path;       /* heap copy of the path ctor target (to unlink) */
    void *stream_sink;     /* core trampoline state handed to the backend */
};

/* Page / font / image / annot / outline cells. `tag` guards against
 * cross-type handle mixups in debug builds. */
struct pdf_page_cell
{
    const pdf_backend_vtable *vt;
    void *obj;
};

struct pdf_font_cell
{
    const pdf_backend_vtable *vt;
    void *obj;
};

struct pdf_image_cell
{
    const pdf_backend_vtable *vt;
    void *obj;
};

struct pdf_annot_cell
{
    const pdf_backend_vtable *vt;
    void *obj;
};

struct pdf_outline_cell
{
    const pdf_backend_vtable *vt;
    void *obj;
};

#ifdef __cplusplus
}
#endif

#endif /* PDFLIB_PDF_INTERNAL_H */
