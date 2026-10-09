/**
 * pdf_backend_dummy.c - a trivial in-memory backend.
 *
 * It implements the full SPI without producing real PDF bytes; the save
 * functions emit a tiny placeholder payload. Its purpose is twofold:
 *   - keep pdflib linkable and testable when neither libharu nor PoDoFo
 *     is available (conan options pdf_backend_libharu/podofo = False);
 *   - serve as the reference implementation showing how a backend wires
 *     into the registry.
 */
#include "pdf_backend.h"
#include "pdf_internal.h"
#include "../pdf_core.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The vtable is defined at the bottom of this file; constructors above
 * reference it, so the declaration must precede every use. */
extern const pdf_backend_vtable g_dummy_vt;

/* ----- object payloads ------------------------------------------------ */

typedef struct dummy_doc
{
    unsigned page_count;
    double last_page_w, last_page_h;
    char info[6][128];
    char last_error[128];
    unsigned char *blob;
    size_t blob_size;
    int streamed;
    int finished;
    pdf_stream_write_fn stream_write;
    void *stream_ud;
    pdf_status stream_failed; /* latched sink failure */
} dummy_doc;

typedef struct dummy_obj
{
    int kind;
} dummy_obj;

enum
{
    OBJ_PAGE = 1,
    OBJ_FONT,
    OBJ_IMAGE,
    OBJ_ANNOT,
    OBJ_OUTLINE
};

static dummy_obj *obj_new(int kind)
{
    dummy_obj *o = (dummy_obj *)calloc(1, sizeof(*o));
    if (o)
        o->kind = kind;
    return o;
}

/* ----- document -------------------------------------------------------- */

static pdf_status d_doc_create(pdf_backend_doc **out_doc)
{
    dummy_doc *d = (dummy_doc *)calloc(1, sizeof(*d));
    if (!d)
        return PDF_ERR_OUT_OF_MEMORY;
    *out_doc = (pdf_backend_doc *)d;
    return PDF_OK;
}

static void d_doc_free(pdf_backend_doc *doc)
{
    dummy_doc *d = (dummy_doc *)doc;
    if (!d)
        return;
    free(d->blob);
    free(d);
}

static pdf_status d_save_blob(pdf_backend_doc *doc, unsigned char *buf,
                              size_t *inout_size)
{
    dummy_doc *d = (dummy_doc *)doc;
    static const char payload[] = "%PDF-1.4 dummy backend\n";
    const size_t need = sizeof(payload) - 1 + d->page_count * 8;

    if (!inout_size)
        return PDF_ERR_INVALID_PARAM;
    if (!buf || *inout_size < need)
    {
        *inout_size = need;
        return buf ? PDF_ERR_INVALID_PARAM : PDF_OK; /* size query */
    }
    memcpy(buf, payload, sizeof(payload) - 1);
    {
        char trailer[64];
        int n = snprintf(trailer, sizeof(trailer), "pages: %u\n",
                         d->page_count);
        memcpy(buf + sizeof(payload) - 1, trailer, (size_t)n);
    }
    *inout_size = need;
    return PDF_OK;
}

static pdf_status d_doc_save_to_file(pdf_backend_doc *doc, const char *path)
{
    dummy_doc *d = (dummy_doc *)doc;
    unsigned char stack[256];
    size_t size = sizeof(stack);
    FILE *f;

    if (d_save_blob(doc, stack, &size) != PDF_OK)
    {
        /* size query first */
        unsigned char *heap = (unsigned char *)malloc(size);
        pdf_status rc;
        if (!heap)
            return PDF_ERR_OUT_OF_MEMORY;
        rc = d_save_blob(doc, heap, &size);
        if (rc == PDF_OK)
        {
            f = fopen(path, "wb");
            if (!f)
            {
                free(heap);
                snprintf(d->last_error, sizeof(d->last_error),
                         "cannot open %s", path);
                return PDF_ERR_IO;
            }
            fwrite(heap, 1, size, f);
            fclose(f);
        }
        free(heap);
        return rc;
    }

    f = fopen(path, "wb");
    if (!f)
    {
        snprintf(d->last_error, sizeof(d->last_error), "cannot open %s",
                 path);
        return PDF_ERR_IO;
    }
    fwrite(stack, 1, size, f);
    fclose(f);
    return PDF_OK;
}

static pdf_status d_doc_save_to_memory(pdf_backend_doc *doc, unsigned char *buf,
                                       size_t *inout_size)
{
    return d_save_blob(doc, buf, inout_size);
}

static pdf_status d_doc_save_to_stream(pdf_backend_doc *doc,
                                       pdf_stream_write_fn write_fn,
                                       void *userdata)
{
    /* serialize the same blob as save_to_memory, then push it in two
     * chunks so tests can verify chunked delivery */
    unsigned char stack[256];
    size_t size = sizeof(stack);
    unsigned char *heap = NULL;
    pdf_status rc;

    if (d_save_blob(doc, stack, &size) != PDF_OK)
    {
        heap = (unsigned char *)malloc(size);
        if (!heap)
            return PDF_ERR_OUT_OF_MEMORY;
        rc = d_save_blob(doc, heap, &size);
        if (rc != PDF_OK)
        {
            free(heap);
            return rc;
        }
    }
    {
        const unsigned char *blob = heap ? heap : stack;
        size_t half = size / 2;
        rc = write_fn(blob, half, userdata);
        if (rc == PDF_OK && half < size)
            rc = write_fn(blob + half, size - half, userdata);
    }
    free(heap);
    return rc;
}

static pdf_status d_doc_create_streamed(pdf_stream_write_fn write_fn,
                                        void *userdata,
                                        pdf_backend_doc **out_doc)
{
    dummy_doc *d = (dummy_doc *)calloc(1, sizeof(dummy_doc));
    pdf_status rc;

    if (!d)
        return PDF_ERR_OUT_OF_MEMORY;
    d->stream_write = write_fn;
    d->stream_ud = userdata;
    d->streamed = 1;
    d->stream_failed = PDF_OK;
    *out_doc = (pdf_backend_doc *)d;
    (void)rc;
    return PDF_OK;
}

/* Push a per-page marker while streaming so tests can fail the sink at
 * an exact page and verify that later work is refused. */
static pdf_status d_stream_page_marker(dummy_doc *d, unsigned page_no)
{
    char marker[32];
    int n;

    if (!d->stream_write || d->stream_failed != PDF_OK)
        return d->stream_failed;
    n = snprintf(marker, sizeof(marker), "[page %u]", page_no);
    d->stream_failed = d->stream_write((const unsigned char *)marker,
                                       (size_t)n, d->stream_ud);
    return d->stream_failed;
}

static pdf_status d_doc_finish(pdf_backend_doc *doc)
{
    dummy_doc *d = (dummy_doc *)doc;

    if (!d->streamed || d->finished)
        return PDF_OK;
    if (d->stream_failed != PDF_OK)
        return d->stream_failed; /* broken: do not push more bytes */
    d->finished = 1;
    /* push a deterministic marker so tests can observe finish() */
    return d->stream_write
               ? d->stream_write((const unsigned char *)"FINISHED", 8,
                                 d->stream_ud)
               : PDF_OK;
}

static const char *d_doc_last_error(pdf_backend_doc *doc)
{
    return ((dummy_doc *)doc)->last_error;
}

/* ----- info / options --------------------------------------------------- */

static pdf_status d_doc_set_info(pdf_backend_doc *doc, pdf_info_key key,
                                 const char *value)
{
    dummy_doc *d = (dummy_doc *)doc;
    if ((int)key < 0 || (int)key > 5)
        return PDF_ERR_INVALID_PARAM;
    snprintf(d->info[(int)key], sizeof(d->info[0]), "%s", value ? value : "");
    return PDF_OK;
}

static pdf_status d_doc_get_info(pdf_backend_doc *doc, pdf_info_key key,
                                 char *buf, size_t *inout_len)
{
    dummy_doc *d = (dummy_doc *)doc;
    const char *src;
    size_t len;

    if ((int)key < 0 || (int)key > 5)
        return PDF_ERR_INVALID_PARAM;
    src = d->info[(int)key];
    len = strlen(src);
    if (buf && *inout_len > 0)
    {
        if (len >= *inout_len)
            len = *inout_len - 1;
        memcpy(buf, src, len);
        buf[len] = '\0';
    }
    *inout_len = strlen(src) + 1;
    return PDF_OK;
}

static pdf_status d_doc_set_encryption(pdf_backend_doc *doc,
                                       pdf_encrypt_mode mode,
                                       const char *owner,
                                       const char *user,
                                       unsigned permissions)
{
    (void)doc;
    (void)owner;
    (void)user;
    (void)permissions;
    if (mode == PDF_ENCRYPT_NONE)
        return PDF_OK;
    return PDF_ERR_BACKEND_UNSUPPORTED;
}

static pdf_status d_doc_set_compression(pdf_backend_doc *doc, unsigned mask)
{
    (void)doc;
    (void)mask;
    return PDF_OK;
}

static pdf_status d_doc_set_page_mode_use_outlines(pdf_backend_doc *doc,
                                                   int enable)
{
    (void)doc;
    (void)enable;
    return PDF_OK;
}

/* ----- pages ------------------------------------------------------------ */

static pdf_status d_page_add(pdf_backend_doc *doc, double w, double h,
                             pdf_page *out_page)
{
    dummy_doc *d = (dummy_doc *)doc;
    dummy_obj *o = obj_new(OBJ_PAGE);

    if (!o)
        return PDF_ERR_OUT_OF_MEMORY;
    if (d->streamed)
    {
        pdf_status rc = d_stream_page_marker(d, d->page_count + 1);
        if (rc != PDF_OK)
        {
            free(o);
            return rc; /* sink refused the page marker: abort mid-page */
        }
    }
    d->page_count++;
    d->last_page_w = w;
    d->last_page_h = h;

    *out_page = (pdf_page)malloc(sizeof(struct pdf_page_cell));
    if (!*out_page)
    {
        free(o);
        return PDF_ERR_OUT_OF_MEMORY;
    }
    (*out_page)->vt = &g_dummy_vt;
    (*out_page)->obj = o;
    return PDF_OK;
}

static pdf_status d_page_count(pdf_backend_doc *doc, size_t *out_count)
{
    *out_count = ((dummy_doc *)doc)->page_count;
    return PDF_OK;
}

static pdf_status d_page_get_size(pdf_page page, double *out_w, double *out_h)
{
    (void)page;
    *out_w = 0;
    *out_h = 0;
    return PDF_ERR_BACKEND_UNSUPPORTED;
}

static pdf_status d_page_set_rotation(pdf_page page, int degrees)
{
    (void)page;
    (void)degrees;
    return PDF_OK;
}

/* ----- fonts ------------------------------------------------------------ */

static pdf_status d_font_get_std(pdf_backend_doc *doc, pdf_std_font font,
                                 pdf_font *out_font)
{
    dummy_obj *o;
    (void)doc;
    (void)font;
    o = obj_new(OBJ_FONT);
    if (!o)
        return PDF_ERR_OUT_OF_MEMORY;
    *out_font = (pdf_font)malloc(sizeof(struct pdf_font_cell));
    if (!*out_font)
    {
        free(o);
        return PDF_ERR_OUT_OF_MEMORY;
    }
    (*out_font)->vt = &g_dummy_vt;
    (*out_font)->obj = o;
    return PDF_OK;
}

static void d_font_free(pdf_font font)
{
    free(((pdf_font)font)->obj);
}

static pdf_status d_font_text_width(pdf_font font, const char *text,
                                    double size, double *out_width)
{
    (void)font;
    *out_width = (double)strlen(text) * size * 0.5;
    return PDF_OK;
}

static pdf_status d_font_ascent(pdf_font font, double size, double *out)
{
    (void)font;
    *out = size * 0.8;
    return PDF_OK;
}

static pdf_status d_font_descent(pdf_font font, double size, double *out)
{
    (void)font;
    *out = -size * 0.2;
    return PDF_OK;
}

/* ----- drawing: accepted and ignored ------------------------------------ */

static pdf_status d_draw_text(pdf_page page, pdf_font font,
                              const char *text, double x, double y,
                              double size)
{
    (void)page;
    (void)font;
    (void)text;
    (void)x;
    (void)y;
    (void)size;
    return PDF_OK;
}

static pdf_status d_draw_text_aligned(pdf_page page, pdf_font font,
                                      const char *text, double x, double y,
                                      double size, double width,
                                      pdf_align align)
{
    (void)page;
    (void)font;
    (void)text;
    (void)x;
    (void)y;
    (void)size;
    (void)width;
    (void)align;
    return PDF_OK;
}

static pdf_status d_gstate_save(pdf_page page)
{
    (void)page;
    return PDF_OK;
}

static pdf_status d_gstate_restore(pdf_page page)
{
    (void)page;
    return PDF_OK;
}

static pdf_status d_one(pdf_page page)
{
    (void)page;
    return PDF_OK;
}

static pdf_status d_line_width(pdf_page page, double w)
{
    (void)page;
    (void)w;
    return PDF_OK;
}

static pdf_status d_cap(pdf_page page, pdf_line_cap cap)
{
    (void)page;
    (void)cap;
    return PDF_OK;
}

static pdf_status d_join(pdf_page page, pdf_line_join join)
{
    (void)page;
    (void)join;
    return PDF_OK;
}

static pdf_status d_miter(pdf_page page, double limit)
{
    (void)page;
    (void)limit;
    return PDF_OK;
}

static pdf_status d_dash(pdf_page page, const double *p, size_t n, double ph)
{
    (void)page;
    (void)p;
    (void)n;
    (void)ph;
    return PDF_OK;
}

static pdf_status d_color(pdf_page page, const pdf_color *c)
{
    (void)page;
    (void)c;
    return PDF_OK;
}

static pdf_status d_opacity(pdf_page page, double fa, double sa)
{
    (void)page;
    (void)fa;
    (void)sa;
    return PDF_OK;
}

static pdf_status d_blend(pdf_page page, pdf_blend_mode m)
{
    (void)page;
    (void)m;
    return PDF_OK;
}

static pdf_status d_text_mode(pdf_page page, pdf_text_render_mode m)
{
    (void)page;
    (void)m;
    return PDF_OK;
}

static pdf_status d_path_move_to(pdf_page page, double x, double y)
{
    (void)page;
    (void)x;
    (void)y;
    return PDF_OK;
}

static pdf_status d_path_line_to(pdf_page page, double x, double y)
{
    (void)page;
    (void)x;
    (void)y;
    return PDF_OK;
}

static pdf_status d_path_curve_to(pdf_page page, double x1, double y1,
                                  double x2, double y2, double x3, double y3)
{
    (void)page;
    (void)x1;
    (void)y1;
    (void)x2;
    (void)y2;
    (void)x3;
    (void)y3;
    return PDF_OK;
}

static pdf_status d_path_rect(pdf_page page, const pdf_rect *r)
{
    (void)page;
    (void)r;
    return PDF_OK;
}

static pdf_status d_paint(pdf_page page, pdf_path_paint_mode m)
{
    (void)page;
    (void)m;
    return PDF_OK;
}

static pdf_status d_image_get_size(pdf_image image, size_t *out_w,
                                   size_t *out_h)
{
    (void)image;
    *out_w = 0;
    *out_h = 0;
    return PDF_ERR_BACKEND_UNSUPPORTED;
}

static pdf_status d_draw_image(pdf_page page, pdf_image image,
                               const pdf_rect *dest)
{
    (void)page;
    (void)image;
    (void)dest;
    return PDF_OK;
}

/* ----- annotations & outlines ------------------------------------------- */

static pdf_status d_annot_create(pdf_page page, pdf_annot_type type,
                                 const pdf_rect *rect, const char *content,
                                 pdf_annot *out_annot)
{
    dummy_obj *o;
    (void)page;
    (void)type;
    (void)rect;
    (void)content;
    o = obj_new(OBJ_ANNOT);
    if (!o)
        return PDF_ERR_OUT_OF_MEMORY;
    *out_annot = (pdf_annot)malloc(sizeof(struct pdf_annot_cell));
    if (!*out_annot)
    {
        free(o);
        return PDF_ERR_OUT_OF_MEMORY;
    }
    (*out_annot)->vt = &g_dummy_vt;
    (*out_annot)->obj = o;
    return PDF_OK;
}

static pdf_status d_annot_link(pdf_annot annot, pdf_page *dest,
                               const char *uri)
{
    (void)annot;
    (void)dest;
    (void)uri;
    return PDF_OK;
}

static void d_annot_free(pdf_annot annot)
{
    free(((pdf_annot)annot)->obj);
}

static pdf_status d_outline_add(pdf_backend_doc *doc, pdf_outline parent,
                                const char *title, pdf_page *dest,
                                pdf_outline *out)
{
    dummy_obj *o;
    (void)doc;
    (void)parent;
    (void)title;
    (void)dest;
    o = obj_new(OBJ_OUTLINE);
    if (!o)
        return PDF_ERR_OUT_OF_MEMORY;
    *out = (pdf_outline)malloc(sizeof(struct pdf_outline_cell));
    if (!*out)
    {
        free(o);
        return PDF_ERR_OUT_OF_MEMORY;
    }
    (*out)->vt = &g_dummy_vt;
    (*out)->obj = o;
    return PDF_OK;
}

static void d_outline_free(pdf_outline outline)
{
    free(((pdf_outline)outline)->obj);
}

/* ----- the vtable -------------------------------------------------------- */

const pdf_backend_vtable g_dummy_vt = {
    "dummy",
    "builtin",

    d_doc_create,
    d_doc_free,
    d_doc_save_to_file,
    d_doc_save_to_memory,
    d_doc_last_error,

    d_doc_set_info,
    d_doc_get_info,
    d_doc_set_encryption,
    d_doc_set_compression,
    d_doc_set_page_mode_use_outlines,

    d_page_add,
    d_page_count,
    d_page_get_size,
    d_page_set_rotation,

    d_font_get_std,
    NULL, /* font_load_ttf */
    NULL, /* font_load_ttf_memory */
    NULL, /* font_find_system */
    d_font_free,
    d_font_text_width,
    d_font_ascent,
    d_font_descent,

    d_draw_text,
    d_draw_text_aligned,
    NULL, /* draw_text_wrapped: core implements it from primitives */

    d_gstate_save,
    d_gstate_restore,
    NULL, /* set_transform */
    d_line_width,
    d_cap,
    d_join,
    d_miter,
    d_dash,
    d_color, /* fill */
    d_color, /* stroke */
    d_opacity,
    d_blend,
    d_text_mode,

    d_path_move_to,
    d_path_line_to,
    d_path_curve_to,
    d_one, /* path_close */
    d_path_rect,
    d_paint,
    d_one, /* path_discard */

    NULL, /* image_load_file */
    NULL, /* image_load_memory */
    NULL, /* image_free */
    d_image_get_size,
    d_draw_image,

    d_annot_create,
    d_annot_link,
    d_color, /* annot_set_color */
    NULL,    /* annot_set_icon */
    NULL,    /* annot_set_opacity */
    NULL,    /* annot_set_author */
    NULL,    /* annot_set_subject */
    NULL,    /* annot_set_open */
    d_annot_free,
    d_outline_add,
    NULL, /* outline_set_open */
    d_outline_free,
    d_doc_save_to_stream,
    d_doc_create_streamed,
    d_doc_finish
};

void pdf_backend_dummy_register(void)
{
    static int registered = 0;

    if (registered)
        return;
    registered = 1;
    pdf_backend_register(PDF_BACKEND_DUMMY, &g_dummy_vt);
}
