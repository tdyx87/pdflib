/**
 * pdflib - a unified C wrapper over PDF generation backends.
 *
 * The public API is backend-agnostic: pdflib never exposes libharu or
 * PoDoFo types to callers. The concrete engine is chosen at runtime
 * through pdflib_set_backend() (see pdf_backends.h for the registry).
 *
 * Conventions:
 *  - All handle types are opaque pointers; NULL/0 is always an invalid value.
 *  - All functions returning PDFAPI status codes return PDF_OK on success.
 *  - Unless a function is documented otherwise it needs a valid handle;
 *     passing NULL makes it return PDF_E_INVALID_HANDLE.
 *  - Angle units are radians; colors are 0..1; coordinates are PDF points
 *     (1/72 inch) with the origin at the bottom-left of the page.
 *  - The caller retains ownership of all user buffers passed in.
 *
 * Threading model:
 *  - Every function is thread-safe as long as each OBJECT (document,
 *    page, font, image, annot, outline) is touched by one thread at a
 *    time. Distinct objects created by distinct documents may be used
 *    concurrently from any thread; a single document may be shared by
 *    serializing all its calls (an external lock).
 *  - pdflib_set_backend() is a process-global selector. It is safe to
 *    call concurrently (no tearing), but a racing pair of calls makes
 *    WHICH backend the next pdf_doc_create uses undefined - call it
 *    during startup, before spawning worker threads.
 *  - The lazy backend registration on the first pdflib_* call is
 *    internally synchronized; no explicit init call is required.
 *
 * Handle lifecycle:
 *  - pdf_doc owns its pages, fonts, images (in the PDF), annots and
 *    outlines. pdf_doc_free() releases everything; using any child
 *    handle after that is undefined.
 *  - Page/font/annot/outline handles may be freed EARLY (pdf_font_free,
 *    pdf_image_free, pdf_annot_free, pdf_outline_free) to release the
 *    wrapper resources; the document stays valid and the produced PDF
 *    is unaffected.
 *  - There is no pdf_page_free: page handles stay valid until
 *    pdf_doc_free. Annotations passed to pdf_annot_free must not be
 *    used afterwards; the underlying PDF annotation object remains in
 *    the document.
 *  - Streamed documents: content is pushed to the sink during the
 *    calls; pdf_doc_finish() (or free) completes the file. A sink
 *    failure poisons the document (see the streamed section below).
 */
#ifndef PDFLIB_PDF_API_H
#define PDFLIB_PDF_API_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* Export macros                                                       */
/* ------------------------------------------------------------------ */

#if defined(_WIN32) && defined(PDFLIB_SHARED)
#  ifdef PDFLIB_BUILDING
#    define PDFAPI __declspec(dllexport)
#  else
#    define PDFAPI __declspec(dllimport)
#  endif
#else
#  define PDFAPI
#endif

/* ------------------------------------------------------------------ */
/* Status codes                                                        */
/* ------------------------------------------------------------------ */

typedef enum pdf_status
{
    PDF_OK = 0,
    PDF_ERR_UNKNOWN = 1,
    PDF_ERR_INVALID_HANDLE = 2,
    PDF_ERR_INVALID_PARAM = 3,
    PDF_ERR_BACKEND_UNSUPPORTED = 4,   /* backend lacks the feature */
    PDF_ERR_BACKEND_FAILED = 5,        /* backend reported an error */
    PDF_ERR_BACKEND_NOT_FOUND = 6,     /* backend was never registered */
    PDF_ERR_OUT_OF_MEMORY = 7,
    PDF_ERR_IO = 8                     /* file could not be read/written */
} pdf_status;

typedef enum pdf_backend
{
    PDF_BACKEND_AUTO = 0,  /* pick the default registered backend */
    PDF_BACKEND_DUMMY = 1, /* in-memory validation backend (no output file) */
    PDF_BACKEND_LIBHARU = 2,
    PDF_BACKEND_PODOFO = 3
} pdf_backend;

/* ------------------------------------------------------------------ */
/* Opaque handles                                                      */
/* ------------------------------------------------------------------ */

/* Opaque handles. The tags are "*_cell" so they can never collide with
 * the typedef names in C++; the complete cell layouts live in the
 * internal header, the public API only passes the pointers around. */
typedef struct pdf_doc_cell     *pdf_doc;
typedef struct pdf_page_cell    *pdf_page;
typedef struct pdf_font_cell    *pdf_font;
typedef struct pdf_image_cell   *pdf_image;
typedef struct pdf_annot_cell   *pdf_annot;
typedef struct pdf_outline_cell *pdf_outline;

/* ------------------------------------------------------------------ */
/* Geometry / drawing types                                            */
/* ------------------------------------------------------------------ */

typedef struct pdf_rect
{
    double x, y;        /* lower-left corner */
    double w, h;
} pdf_rect;

typedef struct pdf_point
{
    double x, y;
} pdf_point;

typedef enum pdf_color_kind
{
    PDF_COLOR_GRAY = 0,
    PDF_COLOR_RGB = 1,
    PDF_COLOR_CMYK = 2
} pdf_color_kind;

typedef struct pdf_color
{
    pdf_color_kind kind;
    double c1, c2, c3, c4; /* gray | r,g,b | c,m,y,k */
    double alpha;          /* 0..1, 1 = opaque */
} pdf_color;

typedef enum pdf_page_size
{
    PDF_PAGE_A4 = 0,
    PDF_PAGE_A3,
    PDF_PAGE_A5,
    PDF_PAGE_LETTER,
    PDF_PAGE_LEGAL,
    PDF_PAGE_EXECUTIVE,
    PDF_PAGE_TABLOID
} pdf_page_size;

typedef enum pdf_page_orientation
{
    PDF_ORIENTATION_PORTRAIT = 0,
    PDF_ORIENTATION_LANDSCAPE
} pdf_page_orientation;

/* ------------------------------------------------------------------ */
/* Graphics state                                                      */
/* ------------------------------------------------------------------ */

typedef enum pdf_line_cap
{
    PDF_LINE_CAP_BUTT = 0,
    PDF_LINE_CAP_ROUND = 1,
    PDF_LINE_CAP_SQUARE = 2
} pdf_line_cap;

typedef enum pdf_line_join
{
    PDF_LINE_JOIN_MITER = 0,
    PDF_LINE_JOIN_ROUND = 1,
    PDF_LINE_JOIN_BEVEL = 2
} pdf_line_join;

typedef enum pdf_text_render_mode
{
    PDF_TEXT_RENDER_FILL = 0,
    PDF_TEXT_RENDER_STROKE = 1,
    PDF_TEXT_RENDER_FILL_STROKE = 2,
    PDF_TEXT_RENDER_INVISIBLE = 3
} pdf_text_render_mode;

typedef enum pdf_blend_mode
{
    PDF_BLEND_NORMAL = 0,
    PDF_BLEND_MULTIPLY,
    PDF_BLEND_SCREEN,
    PDF_BLEND_OVERLAY,
    PDF_BLEND_DARKEN,
    PDF_BLEND_LIGHTEN,
    PDF_BLEND_COLOR_DODGE,
    PDF_BLEND_COLOR_BURN,
    PDF_BLEND_HARD_LIGHT,
    PDF_BLEND_SOFT_LIGHT,
    PDF_BLEND_DIFFERENCE,
    PDF_BLEND_EXCLUSION
} pdf_blend_mode;

typedef enum pdf_path_fill_rule
{
    PDF_FILL_RULE_NONZERO = 0,
    PDF_FILL_RULE_EVENODD = 1
} pdf_path_fill_rule;

/* ------------------------------------------------------------------ */
/* Fonts                                                               */
/* ------------------------------------------------------------------ */

typedef enum pdf_std_font
{
    PDF_STD_FONT_TIMES_ROMAN = 0,
    PDF_STD_FONT_TIMES_ITALIC,
    PDF_STD_FONT_TIMES_BOLD,
    PDF_STD_FONT_TIMES_BOLD_ITALIC,
    PDF_STD_FONT_HELVETICA,
    PDF_STD_FONT_HELVETICA_OBLIQUE,
    PDF_STD_FONT_HELVETICA_BOLD,
    PDF_STD_FONT_HELVETICA_BOLD_OBLIQUE,
    PDF_STD_FONT_COURIER,
    PDF_STD_FONT_COURIER_OBLIQUE,
    PDF_STD_FONT_COURIER_BOLD,
    PDF_STD_FONT_COURIER_BOLD_OBLIQUE,
    PDF_STD_FONT_SYMBOL,
    PDF_STD_FONT_ZAPF_DINGBATS
} pdf_std_font;

/* ------------------------------------------------------------------ */
/* Text alignment                                                      */
/* ------------------------------------------------------------------ */

typedef enum pdf_align
{
    PDF_ALIGN_LEFT = 0,
    PDF_ALIGN_CENTER = 1,
    PDF_ALIGN_RIGHT = 2
} pdf_align;

/* ------------------------------------------------------------------ */
/* Annotations                                                         */
/* ------------------------------------------------------------------ */

typedef enum pdf_annot_type
{
    PDF_ANNOT_TEXT = 0,
    PDF_ANNOT_LINK_URI,
    PDF_ANNOT_LINK_GOTO,
    PDF_ANNOT_HIGHLIGHT,
    PDF_ANNOT_UNDERLINE,
    PDF_ANNOT_SQUIGGLY,
    PDF_ANNOT_STRIKEOUT,
    PDF_ANNOT_SQUARE,
    PDF_ANNOT_CIRCLE
} pdf_annot_type;

typedef enum pdf_annot_icon
{
    PDF_ANNOT_ICON_COMMENT = 0,
    PDF_ANNOT_ICON_KEY,
    PDF_ANNOT_ICON_NOTE,
    PDF_ANNOT_ICON_HELP,
    PDF_ANNOT_ICON_NEW_PARAGRAPH,
    PDF_ANNOT_ICON_PARAGRAPH,
    PDF_ANNOT_ICON_INSERT
} pdf_annot_icon;

/* ------------------------------------------------------------------ */
/* Document info / security                                            */
/* ------------------------------------------------------------------ */

typedef enum pdf_info_key
{
    PDF_INFO_TITLE = 0,
    PDF_INFO_AUTHOR,
    PDF_INFO_SUBJECT,
    PDF_INFO_KEYWORDS,
    PDF_INFO_CREATOR,
    PDF_INFO_PRODUCER
} pdf_info_key;

typedef enum pdf_encrypt_mode
{
    PDF_ENCRYPT_NONE = 0,
    PDF_ENCRYPT_RC4_40 = 1,
    PDF_ENCRYPT_RC4_128 = 2,
    PDF_ENCRYPT_AES_128 = 3,
    PDF_ENCRYPT_AES_256 = 4
} pdf_encrypt_mode;

typedef enum pdf_perm
{
    PDF_PERM_PRINT = 1 << 0,
    PDF_PERM_EDIT = 1 << 1,
    PDF_PERM_COPY = 1 << 2,
    PDF_PERM_ANNOTATE = 1 << 3,
    PDF_PERM_FILL_FORMS = 1 << 4,
    PDF_PERM_ACCESSIBILITY = 1 << 5,
    PDF_PERM_ASSEMBLE = 1 << 6,
    PDF_PERM_PRINT_HIGH_RES = 1 << 7,
    PDF_PERM_ALL = 0x0FF
} pdf_perm;

typedef enum pdf_compress
{
    PDF_COMPRESS_NONE = 0,
    PDF_COMPRESS_TEXT = 1 << 0,   /* compress content streams */
    PDF_COMPRESS_IMAGE = 1 << 1,  /* let backend compress image streams */
    PDF_COMPRESS_METADATA = 1 << 2,
    PDF_COMPRESS_ALL = 0xFF
} pdf_compress;

/* ------------------------------------------------------------------ */
/* Version / backend selection                                         */
/* ------------------------------------------------------------------ */

/** Compile-time pdflib version string, e.g. "1.0.0". */
PDFAPI const char *pdflib_version(void);

/** Human-readable version string of the underlying engine. */
PDFAPI const char *pdflib_backend_version(void);

/**
 * Choose the drawing backend used by subsequently created documents.
 * Backends must have been compiled in and registered (see pdf_backends.h).
 * May be called at any time; already-created documents keep their engine.
 * @return PDF_OK, PDF_ERR_INVALID_PARAM or PDF_ERR_BACKEND_NOT_FOUND
 */
PDFAPI pdf_status pdflib_set_backend(pdf_backend backend);

/** Query the currently active backend. */
PDFAPI pdf_backend pdflib_get_backend(void);

/** Check whether a backend is compiled in and registered. */
PDFAPI int pdflib_has_backend(pdf_backend backend);

/* ------------------------------------------------------------------ */
/* Document lifecycle                                                  */
/* ------------------------------------------------------------------ */

/** Release the document and every handle it created (pages, fonts,
 *  images, annots, outlines). Child handles are invalid afterwards.
 *  A streamed document that was not finished is finished first. */
PDFAPI pdf_status pdf_doc_free(pdf_doc doc);

/** Streaming output callback. Called repeatedly with successive chunks of
 *  the serialized PDF; returning a value other than PDF_OK aborts the
 *  save (that value becomes the save call's return code).
 *  @param userdata opaque pointer passed through from the save call */
typedef pdf_status (*pdf_stream_write_fn)(const unsigned char *data,
                                          size_t len, void *userdata);

/** Create a regular (memory-backed) document using the active backend.
 *  Content is accumulated in memory; serialize it with pdf_doc_save_to_*.
 *  @return PDF_OK, PDF_ERR_INVALID_PARAM, PDF_ERR_BACKEND_NOT_FOUND or
 *          PDF_ERR_OUT_OF_MEMORY. */
PDFAPI pdf_status pdf_doc_create(pdf_doc *out_doc);

/** Create a streaming document: output is written to the callback as pages
 *  complete, so total page count and content may be unknown up front
 *  (e.g. a printer driver translating a spool job). Memory use stays
 *  bounded by one page's objects, not the whole document.
 *
 *  Streaming documents are written implicitly; pdf_doc_save_to_*() is
 *  rejected on them. Call pdf_doc_finish() to flush (pending font subsets
 *  are embedded at that point); pdf_doc_free() finishes automatically.
 *
 *  @return PDF_OK, PDF_ERR_INVALID_PARAM or PDF_ERR_BACKEND_UNSUPPORTED
 *          (engine cannot stream; use pdf_doc_create + save_to_stream). */
PDFAPI pdf_status pdf_doc_create_streamed(pdf_stream_write_fn write_fn,
                                          void *userdata, pdf_doc *out_doc);

/** Create a streaming document written to an already-open FILE*
 *  (stdout, a pipe, a spooler handle...). Flushed on finish, not closed. */
PDFAPI pdf_status pdf_doc_create_streamed_to_FILE(void *file, pdf_doc *out_doc);

/** Create a streaming document written to a file path. */
PDFAPI pdf_status pdf_doc_create_streamed_to_file(const char *path,
                                                  pdf_doc *out_doc);

/** Finish a streamed document: flush pending content, embed pending font
 *  subsets and complete the output. Idempotent; also called by
 *  pdf_doc_free(). On regular (memory) documents this is a no-op. */
PDFAPI pdf_status pdf_doc_finish(pdf_doc doc);

/** Save the document to a file (always a full save). */
PDFAPI pdf_status pdf_doc_save_to_file(pdf_doc doc, const char *path);

/** Serialize the document into a caller-allocated buffer.
 *  Pass buf == NULL to only query the required size in *out_size. */
PDFAPI pdf_status pdf_doc_save_to_memory(pdf_doc doc,
                                         unsigned char *buf,
                                         size_t *inout_size);

/** Stream the document to a callback as it is serialized (no intermediate
 *  full buffer is required by the caller). Backends that support native
 *  streaming push chunks while writing; others serialize to memory first
 *  and push a single chunk.
 *  @param write_fn chunk callback (see pdf_stream_write_fn)
 *  @param userdata opaque context handed back to every callback invocation */
PDFAPI pdf_status pdf_doc_save_to_stream(pdf_doc doc,
                                         pdf_stream_write_fn write_fn,
                                         void *userdata);

/** Convenience wrapper: stream the document into an already-open FILE*
 *  (e.g. stdout, a socket-backed pipe, or a file opened by the caller).
 *  The file is flushed but not closed. */
PDFAPI pdf_status pdf_doc_save_to_FILE(pdf_doc doc, void *file);

/* ------------------------------------------------------------------ */
/* Pages                                                               */
/* ------------------------------------------------------------------ */

/** Append a page of the given standard size. */
PDFAPI pdf_status pdf_page_add(pdf_doc doc,
                               pdf_page_size size,
                               pdf_page_orientation orientation,
                               pdf_page *out_page);

/** Append a page with a custom size (width/height in PDF points). */
PDFAPI pdf_status pdf_page_add_custom(pdf_doc doc,
                                      double width,
                                      double height,
                                      pdf_page *out_page);

/** Number of pages in the document. */
PDFAPI pdf_status pdf_doc_page_count(pdf_doc doc, size_t *out_count);

PDFAPI pdf_status pdf_page_get_size(pdf_page page, double *out_w,
                                    double *out_h);

/** Rotate page content by a multiple of 90 degrees (clockwise). */
PDFAPI pdf_status pdf_page_set_rotation(pdf_page page, int degrees);

/* ------------------------------------------------------------------ */
/* Document info                                                       */
/* ------------------------------------------------------------------ */

PDFAPI pdf_status pdf_doc_set_info(pdf_doc doc, pdf_info_key key,
                                   const char *utf8_value);
PDFAPI pdf_status pdf_doc_get_info(pdf_doc doc, pdf_info_key key,
                                   char *buf, size_t *inout_len);

/* ------------------------------------------------------------------ */
/* Fonts and text                                                      */
/* ------------------------------------------------------------------ */

/** Select one of the 14 PDF standard fonts. Encoding is WinAnsi for the
 *  text fonts; Symbol/ZapfDingbats use their builtin encodings. */
PDFAPI pdf_status pdf_font_get_std(pdf_doc doc, pdf_std_font font,
                                   pdf_font *out_font);

/** Load a TrueType font from a file.
 *  @param embed non-zero to embed (subset) the font into the document. */
PDFAPI pdf_status pdf_font_load_ttf(pdf_doc doc, const char *path,
                                    int embed, pdf_font *out_font);

/** Load a TrueType font from memory. */
PDFAPI pdf_status pdf_font_load_ttf_memory(pdf_doc doc,
                                           const unsigned char *data,
                                           size_t size, int embed,
                                           pdf_font *out_font);

/** Search the system font directories for a family name. */
PDFAPI pdf_status pdf_font_find_system(pdf_doc doc, const char *family,
                                       int bold, int italic,
                                       pdf_font *out_font);

/** Release the font wrapper. The document stays valid; text already
 *  drawn with the font is unaffected. The handle is invalid after. */
PDFAPI pdf_status pdf_font_free(pdf_font font);

/** Width of the UTF-8 string rendered at the given size. */
PDFAPI pdf_status pdf_font_text_width(pdf_font font, const char *utf8_text,
                                      double size, double *out_width);

/** Ascender / descender in font units scaled by size. */
PDFAPI pdf_status pdf_font_ascent(pdf_font font, double size,
                                  double *out_ascent);
PDFAPI pdf_status pdf_font_descent(pdf_font font, double size,
                                   double *out_descent);

/** Draw a single-line UTF-8 string at (x, y) = baseline origin. */
PDFAPI pdf_status pdf_page_draw_text(pdf_page page, pdf_font font,
                                     const char *utf8_text,
                                     double x, double y, double size);

/** Draw a string aligned horizontally inside [x, x+width] at baseline y. */
PDFAPI pdf_status pdf_page_draw_text_aligned(pdf_page page, pdf_font font,
                                             const char *utf8_text,
                                             double x, double y, double size,
                                             double width, pdf_align align);

/** Word-wrap text into the rectangle; y is the TOP of the box in page
 *  coordinates (the function walks downward). */
PDFAPI pdf_status pdf_page_draw_text_wrapped(pdf_page page, pdf_font font,
                                             const char *utf8_text,
                                             double x, double y,
                                             double width, double height,
                                             double size, double line_gap,
                                             pdf_align align);

/* ------------------------------------------------------------------ */
/* Graphics state                                                      */
/* ------------------------------------------------------------------ */

/** Push the current graphics state (PDF 'q'). */
PDFAPI pdf_status pdf_gstate_save(pdf_page page);
/** Pop the graphics state (PDF 'Q'). */
PDFAPI pdf_status pdf_gstate_restore(pdf_page page);

/** Multiply the current transformation matrix (PDF 'cm'). */
PDFAPI pdf_status pdf_set_transform(pdf_page page,
                                    double a, double b,
                                    double c, double d,
                                    double e, double f);

PDFAPI pdf_status pdf_set_line_width(pdf_page page, double w);
PDFAPI pdf_status pdf_set_line_cap(pdf_page page, pdf_line_cap cap);
PDFAPI pdf_status pdf_set_line_join(pdf_page page, pdf_line_join join);
PDFAPI pdf_status pdf_set_miter_limit(pdf_page page, double limit);
/** Dash pattern in on/off units; pass n = 0 to disable dashing. */
PDFAPI pdf_status pdf_set_line_dash(pdf_page page, const double *pattern,
                                    size_t n, double phase);
PDFAPI pdf_status pdf_set_fill_color(pdf_page page, const pdf_color *c);
PDFAPI pdf_status pdf_set_stroke_color(pdf_page page, const pdf_color *c);
/** Overall opacity for filling and stroking (0..1). */
PDFAPI pdf_status pdf_set_opacity(pdf_page page, double fill_alpha,
                                  double stroke_alpha);
PDFAPI pdf_status pdf_set_blend_mode(pdf_page page, pdf_blend_mode mode);
PDFAPI pdf_status pdf_set_text_render_mode(pdf_page page,
                                           pdf_text_render_mode mode);

/* ------------------------------------------------------------------ */
/* Path construction and painting                                      */
/* ------------------------------------------------------------------ */

PDFAPI pdf_status pdf_path_move_to(pdf_page page, double x, double y);
PDFAPI pdf_status pdf_path_line_to(pdf_page page, double x, double y);
/** Cubic bezier from the current point; (x3,y3) is the end point. */
PDFAPI pdf_status pdf_path_curve_to(pdf_page page,
                                    double x1, double y1,
                                    double x2, double y2,
                                    double x3, double y3);
PDFAPI pdf_status pdf_path_close(pdf_page page);
PDFAPI pdf_status pdf_path_rectangle(pdf_page page, const pdf_rect *r);
/** Append a full circle / ellipse as a path (use pdf_path_paint after). */
PDFAPI pdf_status pdf_path_circle(pdf_page page, double cx, double cy,
                                  double radius);
PDFAPI pdf_status pdf_path_ellipse(pdf_page page, double cx, double cy,
                                   double rx, double ry);
/** Arc segment around (cx,cy) from a1 to a2 radians, ccw when ccw != 0. */
PDFAPI pdf_status pdf_path_arc(pdf_page page, double cx, double cy,
                               double radius,
                               double a1, double a2, int ccw);
/** Append a polygon with n vertices; implicitly closed when painted. */
PDFAPI pdf_status pdf_path_polygon(pdf_page page, const pdf_point *pts,
                                   size_t n);

typedef enum pdf_path_paint_mode
{
    PDF_PATH_STROKE = 0,
    PDF_PATH_FILL,
    PDF_PATH_FILL_STROKE,
    PDF_PATH_FILL_EVENODD,
    PDF_PATH_FILL_STROKE_EVENODD
} pdf_path_paint_mode;

PDFAPI pdf_status pdf_path_paint(pdf_page page, pdf_path_paint_mode mode);
/** Discard the current path without painting (PDF 'n'). */
PDFAPI pdf_status pdf_path_discard(pdf_page page);

/** Convenience: draw a rectangle in one call. */
PDFAPI pdf_status pdf_page_draw_rect(pdf_page page, const pdf_rect *r,
                                     pdf_path_paint_mode mode);
/** Convenience: draw a line in one call. */
PDFAPI pdf_status pdf_page_draw_line(pdf_page page,
                                     double x1, double y1,
                                     double x2, double y2);

/* ------------------------------------------------------------------ */
/* Images                                                              */
/* ------------------------------------------------------------------ */

typedef enum pdf_image_format
{
    PDF_IMAGE_PNG = 0,
    PDF_IMAGE_JPEG
} pdf_image_format;

PDFAPI pdf_status pdf_image_load_file(pdf_doc doc, const char *path,
                                      pdf_image *out_image);
PDFAPI pdf_status pdf_image_load_memory(pdf_doc doc,
                                        const unsigned char *data,
                                        size_t size, pdf_image *out_image);
/** Release the image wrapper (the image stays in the document).
 *  The handle is invalid after. */
PDFAPI pdf_status pdf_image_free(pdf_image image);

PDFAPI pdf_status pdf_image_get_size(pdf_image image, size_t *out_w,
                                     size_t *out_h);
/** Draw the image scaled into the rectangle. */
PDFAPI pdf_status pdf_page_draw_image(pdf_page page, pdf_image image,
                                      const pdf_rect *dest);

/* ------------------------------------------------------------------ */
/* Annotations & outlines                                              */
/* ------------------------------------------------------------------ */

PDFAPI pdf_status pdf_annot_create(pdf_page page, pdf_annot_type type,
                                   const pdf_rect *rect,
                                   const char *utf8_content,
                                   pdf_annot *out_annot);
/** For PDF_ANNOT_LINK_URI pass the URL, for PDF_ANNOT_LINK_GOTO pass
 *  NULL (the destination page is the second argument). */
PDFAPI pdf_status pdf_annot_link_to_page(pdf_annot annot, pdf_page *dest,
                                         const char *uri);
PDFAPI pdf_status pdf_annot_set_color(pdf_annot annot, const pdf_color *c);
PDFAPI pdf_status pdf_annot_set_icon(pdf_annot annot, pdf_annot_icon icon);
PDFAPI pdf_status pdf_annot_set_opacity(pdf_annot annot, double alpha);
PDFAPI pdf_status pdf_annot_set_author(pdf_annot annot, const char *author);
PDFAPI pdf_status pdf_annot_set_subject(pdf_annot annot, const char *subject);
PDFAPI pdf_status pdf_annot_set_open(pdf_annot annot, int opened);
/** Release the annot wrapper. The PDF annotation stays in the page;
 *  only the handle becomes invalid. */
PDFAPI pdf_status pdf_annot_free(pdf_annot annot);

/** Add an outline item. parent = NULL creates a top-level item. dest_page
 *  may be NULL for a title-only item (no jump target); otherwise it points
 *  to one slot holding the destination page handle. */
PDFAPI pdf_status pdf_outline_add(pdf_doc doc, pdf_outline parent,
                                  const char *title, pdf_page *dest_page,
                                  pdf_outline *out_outline);
PDFAPI pdf_status pdf_outline_set_open(pdf_outline outline, int opened);
/** Release the outline-item wrapper (the item stays in the outline
 *  tree). The handle is invalid after. */
PDFAPI pdf_status pdf_outline_free(pdf_outline outline);

/* ------------------------------------------------------------------ */
/* Security / compression / viewer preferences                         */
/* ------------------------------------------------------------------ */

PDFAPI pdf_status pdf_doc_set_encryption(pdf_doc doc, pdf_encrypt_mode mode,
                                         const char *owner_password,
                                         const char *user_password,
                                         unsigned permissions);
PDFAPI pdf_status pdf_doc_set_compression(pdf_doc doc, unsigned mode_mask);
/** Set the page mode when the document is opened (UseNone/Outlines/...). */
PDFAPI pdf_status pdf_doc_set_page_mode_use_outlines(pdf_doc doc, int enable);

/* ------------------------------------------------------------------ */
/* Error introspection                                                 */
/* ------------------------------------------------------------------ */

/** Last backend error description for this handle, or a generic message. */
PDFAPI const char *pdflib_last_error(pdf_doc doc);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* PDFLIB_PDF_API_H */
