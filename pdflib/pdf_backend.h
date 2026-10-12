/**
 * pdf_backend.h - the internal backend SPI (service provider interface).
 *
 * This header is the ONLY bridge between the backend-agnostic core and the
 * concrete engines. libharu / PoDoFo specific translation units implement
 * this interface and register themselves through pdf_backend_register().
 *
 * Invariant: code outside a backend directory must never include haru or
 * podofo headers. Each backend keeps its own private state struct in its
 * .c/.cpp files so the core only ever passes opaque pointers around.
 */
#ifndef PDFLIB_PDF_BACKEND_H
#define PDFLIB_PDF_BACKEND_H

#include "pdf_api.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque per-document backend state. Each backend defines its own struct
 * (declared only inside its own translation units). */
typedef void pdf_backend_doc;

/**
 * Instance table implemented by every concrete backend.
 * A backend fills in the function pointers it supports and leaves the
 * rest NULL; the core maps NULL pointers to PDF_ERR_BACKEND_UNSUPPORTED.
 */
typedef struct pdf_backend_vtable
{
    const char *name;          /* "libharu", "podofo", "dummy" ... */
    const char *version;       /* engine version string, may be NULL */

    /* --- document --------------------------------------------------- */
    pdf_status (*doc_create)(pdf_backend_doc **out_doc);
    void       (*doc_free)(pdf_backend_doc *doc);
    pdf_status (*doc_save_to_file)(pdf_backend_doc *doc, const char *path);
    pdf_status (*doc_save_to_memory)(pdf_backend_doc *doc,
                                     unsigned char *buf, size_t *inout_size);
    const char *(*doc_last_error)(pdf_backend_doc *doc);

    /* --- info / security / options ---------------------------------- */
    pdf_status (*doc_set_info)(pdf_backend_doc *doc, pdf_info_key key,
                               const char *utf8_value);
    pdf_status (*doc_get_info)(pdf_backend_doc *doc, pdf_info_key key,
                               char *buf, size_t *inout_len);
    pdf_status (*doc_set_encryption)(pdf_backend_doc *doc,
                                     pdf_encrypt_mode mode,
                                     const char *owner_password,
                                     const char *user_password,
                                     unsigned permissions);
    pdf_status (*doc_set_compression)(pdf_backend_doc *doc,
                                      unsigned mode_mask);
    pdf_status (*doc_set_page_mode_use_outlines)(pdf_backend_doc *doc,
                                                 int enable);

    /* --- pages ------------------------------------------------------- */
    pdf_status (*page_add)(pdf_backend_doc *doc, double w, double h,
                           pdf_page *out_page);
    pdf_status (*page_count)(pdf_backend_doc *doc, size_t *out_count);
    pdf_status (*page_get_size)(pdf_page page, double *out_w, double *out_h);
    pdf_status (*page_set_rotation)(pdf_page page, int degrees);

    /* --- fonts -------------------------------------------------------- */
    pdf_status (*font_get_std)(pdf_backend_doc *doc, pdf_std_font font,
                               pdf_font *out_font);
    pdf_status (*font_load_ttf)(pdf_backend_doc *doc, const char *path,
                                int embed, pdf_font *out_font);
    pdf_status (*font_load_ttf_memory)(pdf_backend_doc *doc,
                                       const unsigned char *data, size_t size,
                                       int embed, pdf_font *out_font);
    pdf_status (*font_find_system)(pdf_backend_doc *doc, const char *family,
                                   int bold, int italic,
                                   pdf_font *out_font);
    void       (*font_free)(pdf_font font);
    pdf_status (*font_text_width)(pdf_font font, const char *utf8_text,
                                  double size, double *out_width);
    pdf_status (*font_ascent)(pdf_font font, double size, double *out);
    pdf_status (*font_descent)(pdf_font font, double size, double *out);

    /* --- text ---------------------------------------------------------- */
    pdf_status (*draw_text)(pdf_page page, pdf_font font,
                            const char *utf8_text, double x, double y,
                            double size);
    pdf_status (*draw_text_aligned)(pdf_page page, pdf_font font,
                                    const char *utf8_text, double x, double y,
                                    double size, double width, pdf_align align);
    /* Backends that implement word wrapping themselves may set this;
     * otherwise the core decomposes the request into draw_text_aligned
     * + font_text_width calls. */
    pdf_status (*draw_text_wrapped)(pdf_page page, pdf_font font,
                                    const char *utf8_text, double x, double y,
                                    double width, double height, double size,
                                    double line_gap, pdf_align align);

    /* --- graphics state ------------------------------------------------ */
    pdf_status (*gstate_save)(pdf_page page);
    pdf_status (*gstate_restore)(pdf_page page);
    pdf_status (*set_transform)(pdf_page page, double a, double b, double c,
                                double d, double e, double f);
    pdf_status (*set_line_width)(pdf_page page, double w);
    pdf_status (*set_line_cap)(pdf_page page, pdf_line_cap cap);
    pdf_status (*set_line_join)(pdf_page page, pdf_line_join join);
    pdf_status (*set_miter_limit)(pdf_page page, double limit);
    pdf_status (*set_line_dash)(pdf_page page, const double *pattern, size_t n,
                                double phase);
    pdf_status (*set_fill_color)(pdf_page page, const pdf_color *c);
    pdf_status (*set_stroke_color)(pdf_page page, const pdf_color *c);
    pdf_status (*set_opacity)(pdf_page page, double fill_alpha,
                              double stroke_alpha);
    pdf_status (*set_blend_mode)(pdf_page page, pdf_blend_mode mode);
    pdf_status (*set_text_render_mode)(pdf_page page,
                                       pdf_text_render_mode mode);

    /* --- paths ---------------------------------------------------------- */
    pdf_status (*path_move_to)(pdf_page page, double x, double y);
    pdf_status (*path_line_to)(pdf_page page, double x, double y);
    pdf_status (*path_curve_to)(pdf_page page, double x1, double y1,
                                double x2, double y2, double x3, double y3);
    pdf_status (*path_close)(pdf_page page);
    pdf_status (*path_rectangle)(pdf_page page, const pdf_rect *r);
    pdf_status (*path_paint)(pdf_page page, pdf_path_paint_mode mode);
    pdf_status (*path_discard)(pdf_page page);

    /* --- images ---------------------------------------------------------- */
    pdf_status (*image_load_file)(pdf_backend_doc *doc, const char *path,
                                  pdf_image *out_image);
    pdf_status (*image_load_memory)(pdf_backend_doc *doc,
                                    const unsigned char *data, size_t size,
                                    pdf_image *out_image);
    void       (*image_free)(pdf_image image);
    pdf_status (*image_get_size)(pdf_image image, size_t *out_w,
                                 size_t *out_h);
    pdf_status (*draw_image)(pdf_page page, pdf_image image,
                             const pdf_rect *dest);

    /* --- annotations & outlines ------------------------------------------ */
    pdf_status (*annot_create)(pdf_page page, pdf_annot_type type,
                               const pdf_rect *rect, const char *utf8_content,
                               pdf_annot *out_annot);
    pdf_status (*annot_link_to_page)(pdf_annot annot, pdf_page *dest,
                                     const char *uri);
    pdf_status (*annot_set_color)(pdf_annot annot, const pdf_color *c);
    pdf_status (*annot_set_icon)(pdf_annot annot, pdf_annot_icon icon);
    pdf_status (*annot_set_opacity)(pdf_annot annot, double alpha);
    pdf_status (*annot_set_author)(pdf_annot annot, const char *author);
    pdf_status (*annot_set_subject)(pdf_annot annot, const char *subject);
    pdf_status (*annot_set_open)(pdf_annot annot, int opened);
    void       (*annot_free)(pdf_annot annot);
    pdf_status (*outline_add)(pdf_backend_doc *doc, pdf_outline parent,
                              const char *title, pdf_page *dest_page,
                              pdf_outline *out_outline);
    pdf_status (*outline_set_open)(pdf_outline outline, int opened);
    void       (*outline_free)(pdf_outline outline);

     /* --- streaming output ----------------------------------------------- */
     /* Stream the serialized document to a caller callback in chunks.
      * Optional: when NULL the core falls back to doc_save_to_memory and
      * pushes one chunk. userdata is passed through to the callback. */
     pdf_status (*doc_save_to_stream)(pdf_backend_doc *doc,
                                      pdf_stream_write_fn write_fn,
                                      void *userdata);

     /* --- streamed (incremental) document creation ------------------------ */
     /* Create a document whose output is pushed to write_fn as content is
      * produced; total page count is unknown at creation time. Optional:
      * when NULL the core rejects pdf_doc_create_streamed* with
      * PDF_ERR_BACKEND_UNSUPPORTED. The backend must make doc_finish
      * (below) complete the output; pdf_doc_free() implies finish. */
     pdf_status (*doc_create_streamed)(pdf_stream_write_fn write_fn,
                                       void *userdata,
                                       pdf_backend_doc **out_doc);
     pdf_status (*doc_finish)(pdf_backend_doc *doc);
 } pdf_backend_vtable;

/** Register a backend implementation. Called once from each backend's
 *  pdf_backend_X_register() constructor. Later registrations with the
 *  same pdf_backend enum replace earlier ones. */
void pdf_backend_register(pdf_backend id, const pdf_backend_vtable *vtable);

/** Look up a registered backend; NULL when absent. */
const pdf_backend_vtable *pdf_backend_lookup(pdf_backend id);

/** The backend chosen by PDF_BACKEND_AUTO. */
const pdf_backend_vtable *pdf_backend_default(void);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* PDFLIB_PDF_BACKEND_H */
