/**
 * pdf_backend_libharu.c - libharu adapter.
 *
 * Maps the pdflib SPI onto the libharu C API (hpdf.h). All haru symbols
 * stay inside this translation unit; the core only sees the vtable.
 */
#include "../pdf_core.h"
#include "pdf_backend.h"
#include "pdf_internal.h"

#include <hpdf.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The vtable is defined at the bottom of this file; constructors above
 * reference it, so the declaration must precede every use. */
extern const pdf_backend_vtable g_libharu_vt;

/* ----- payloads --------------------------------------------------------- */

typedef struct haru_doc
{
    HPDF_Doc pdf;
    char error[128];
    /* Pages can only enter the document through h_page_add, so the count is
     * authoritative here. HPDF_GetPageByIndex probing is NOT safe for
     * counting: the first out-of-range index raises HPDF_INVALID_PAGE_INDEX
     * and libharu's error state is sticky -- every later call that routes
     * through HPDF_HasDoc fails with HPDF_INVALID_DOCUMENT. */
    size_t page_count;
} haru_doc;

typedef struct haru_page
{
    HPDF_Page page;
    /* back-pointer to the owning haru_doc wrapper; page-level error paths
     * need HPDF_GetError(doc) and the raw HPDF_Doc is not the wrapper */
    struct haru_doc *doc;
} haru_page;

typedef struct haru_annot
{
    HPDF_Page page;       /* owning page */
    HPDF_Doc pdf;
    HPDF_Annotation annot;
    pdf_annot_type type;  /* needed to gate subtype-only setters */
    pdf_rect rect;        /* kept for deferred link creation */
    int link_created;
} haru_annot;

typedef struct haru_font
{
    HPDF_Font font;
} haru_font;

typedef struct haru_image
{
    HPDF_Image image;
} haru_image;

typedef struct haru_outline
{
    HPDF_Doc pdf;
    HPDF_Outline outline;
} haru_outline;

/* ----- small helpers ----------------------------------------------------- */

/* Fail a call AND clear libharu's latched document error.
 *
 * libharu latches the first error on the document: while it is set,
 * HPDF_CheckError() turns every subsequent API call into a silent no-op
 * that still reports success. A function that returns NULL and leaves the
 * latch set therefore does not merely report one failure - it disables the
 * whole document from that point on.
 *
 * That is not hypothetical. libharu cannot read a .ttc (TrueType
 * collection); HPDF_LoadTTFontFromFile fails with HPDF_INVALID_TTC_FILE,
 * and the early return here used to leave that error latched. Every later
 * page_add, draw and image call then did nothing at all while returning
 * PDF_OK, so the document silently came out empty. PoDoFo has no such
 * latch and recovered normally, which made the two engines behave
 * completely differently after one failed call - the exact class of
 * inconsistency this DLL exists to remove.
 *
 * Clearing it restores the contract the rest of pdflib relies on: a failed
 * call reports a status and leaves the document usable. */
static pdf_status haru_fail(haru_doc *d, const char *what)
{
    if (d)
    {
        HPDF_STATUS code = HPDF_GetError(d->pdf);
        HPDF_STATUS detail = HPDF_GetErrorDetail(d->pdf);

        if (code != HPDF_OK || detail != HPDF_OK)
            snprintf(d->error, sizeof(d->error),
                     "libharu: %s failed (error 0x%lx, detail 0x%lx)", what,
                     (unsigned long)code, (unsigned long)detail);
        else
            snprintf(d->error, sizeof(d->error), "libharu: %s failed", what);
        HPDF_ResetError(d->pdf);
    }
    return PDF_ERR_BACKEND_FAILED;
}

static pdf_status haru_rc(HPDF_STATUS st, haru_doc *d)
{
    if (st == HPDF_OK)
        return PDF_OK;
    return haru_fail(d, "operation");
}

static pdf_color normalize_color(const pdf_color *c)
{
    pdf_color out = *c;
    if (out.alpha < 0)
        out.alpha = 0;
    if (out.alpha > 1)
        out.alpha = 1;
    return out;
}

static void fill_rgb(const pdf_color *c, double *r, double *g, double *b)
{
    switch (c->kind)
    {
        case PDF_COLOR_GRAY:
            *r = *g = *b = c->c1;
            break;
        case PDF_COLOR_CMYK:
            /* cheap CMYK -> RGB */
            *r = (1 - c->c1) * (1 - c->c4);
            *g = (1 - c->c2) * (1 - c->c4);
            *b = (1 - c->c3) * (1 - c->c4);
            break;
        default:
            *r = c->c1;
            *g = c->c2;
            *b = c->c3;
            break;
    }
    if (*r < 0) *r = 0;
    if (*r > 1) *r = 1;
    if (*g < 0) *g = 0;
    if (*g > 1) *g = 1;
    if (*b < 0) *b = 0;
    if (*b > 1) *b = 1;
}

static const char *std_font_name(pdf_std_font f)
{
    /* libharu's base-14 font defs are keyed by these exact names. */
    switch (f)
    {
        case PDF_STD_FONT_TIMES_ROMAN: return "Times-Roman";
        case PDF_STD_FONT_TIMES_ITALIC: return "Times-Italic";
        case PDF_STD_FONT_TIMES_BOLD: return "Times-Bold";
        case PDF_STD_FONT_TIMES_BOLD_ITALIC:
            return "Times-BoldItalic";
        case PDF_STD_FONT_HELVETICA: return "Helvetica";
        case PDF_STD_FONT_HELVETICA_OBLIQUE:
            return "Helvetica-Oblique";
        case PDF_STD_FONT_HELVETICA_BOLD: return "Helvetica-Bold";
        case PDF_STD_FONT_HELVETICA_BOLD_OBLIQUE:
            return "Helvetica-BoldOblique";
        case PDF_STD_FONT_COURIER: return "Courier";
        case PDF_STD_FONT_COURIER_OBLIQUE: return "Courier-Oblique";
        case PDF_STD_FONT_COURIER_BOLD: return "Courier-Bold";
        case PDF_STD_FONT_COURIER_BOLD_OBLIQUE:
            return "Courier-BoldOblique";
        case PDF_STD_FONT_SYMBOL: return "Symbol";
        case PDF_STD_FONT_ZAPF_DINGBATS: return "ZapfDingbats";
    }
    return "Helvetica";
}

static HPDF_LineCap map_cap(pdf_line_cap cap)
{
    switch (cap)
    {
        case PDF_LINE_CAP_ROUND: return HPDF_BUTT_END + 1; /* round */
        case PDF_LINE_CAP_SQUARE: return HPDF_BUTT_END + 2; /* square */
        default: return HPDF_BUTT_END;
    }
}

static HPDF_LineJoin map_join(pdf_line_join join)
{
    switch (join)
    {
        case PDF_LINE_JOIN_ROUND: return HPDF_MITER_JOIN + 1;
        case PDF_LINE_JOIN_BEVEL: return HPDF_MITER_JOIN + 2;
        default: return HPDF_MITER_JOIN;
    }
}

static HPDF_TextRenderingMode map_text_mode(pdf_text_render_mode mode)
{
    switch (mode)
    {
        case PDF_TEXT_RENDER_STROKE: return HPDF_STROKE;
        case PDF_TEXT_RENDER_FILL_STROKE: return HPDF_FILL_THEN_STROKE;
        case PDF_TEXT_RENDER_INVISIBLE: return HPDF_INVISIBLE;
        default: return HPDF_FILL;
    }
}

static HPDF_AnnotIcon map_icon(pdf_annot_icon icon)
{
    switch (icon)
    {
        case PDF_ANNOT_ICON_KEY: return HPDF_ANNOT_ICON_KEY;
        case PDF_ANNOT_ICON_NOTE: return HPDF_ANNOT_ICON_NOTE;
        case PDF_ANNOT_ICON_HELP: return HPDF_ANNOT_ICON_HELP;
        case PDF_ANNOT_ICON_NEW_PARAGRAPH:
            return HPDF_ANNOT_ICON_NEW_PARAGRAPH;
        case PDF_ANNOT_ICON_PARAGRAPH: return HPDF_ANNOT_ICON_PARAGRAPH;
        case PDF_ANNOT_ICON_INSERT: return HPDF_ANNOT_ICON_INSERT;
        default: return HPDF_ANNOT_ICON_COMMENT;
    }
}

static HPDF_Page page_of(pdf_page p)
{
    return ((haru_page *)((pdf_page)p)->obj)->page;
}

/* the owning haru_doc wrapper (NOT the raw HPDF_Doc) */
static haru_doc *doc_of_page(pdf_page p)
{
    return ((haru_page *)((pdf_page)p)->obj)->doc;
}

static int ci_equal(const char *a, const char *b)
{
    while (*a && *b)
    {
        char ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb - 'A' + 'a');
        if (ca != cb)
            return 0;
        a++;
        b++;
    }
    return *a == *b;
}

/* ----- document ----------------------------------------------------------- */

static pdf_status h_doc_create(pdf_backend_doc **out_doc)
{
    haru_doc *d = (haru_doc *)calloc(1, sizeof(*d));

    if (!d)
        return PDF_ERR_OUT_OF_MEMORY;
    d->pdf = HPDF_New(NULL, NULL);
    if (!d->pdf)
    {
        free(d);
        return PDF_ERR_OUT_OF_MEMORY;
    }
    /* We poll errors after each call instead of using the callback. */
    HPDF_UseUTFEncodings(d->pdf);
    HPDF_SetCompressionMode(d->pdf, HPDF_COMP_ALL);
    *out_doc = (pdf_backend_doc *)d;
    return PDF_OK;
}

static void h_doc_free(pdf_backend_doc *doc)
{
    haru_doc *d = (haru_doc *)doc;

    if (!d)
        return;
    if (d->pdf)
        HPDF_Free(d->pdf);
    free(d);
}

static pdf_status h_doc_save_to_file(pdf_backend_doc *doc, const char *path)
{
    haru_doc *d = (haru_doc *)doc;

    if (haru_rc(HPDF_SaveToFile(d->pdf, path), d) != PDF_OK)
        return PDF_ERR_BACKEND_FAILED;
    return PDF_OK;
}

static pdf_status h_doc_save_to_memory(pdf_backend_doc *doc,
                                       unsigned char *buf, size_t *inout_size)
{
    haru_doc *d = (haru_doc *)doc;
    HPDF_UINT32 size = 0;
    HPDF_STATUS st;

    if (!inout_size)
        return PDF_ERR_INVALID_PARAM;

    /* HPDF_SaveToStream creates the internal stream on first use and
     * frees+rewrites it on later calls; calling HPDF_ResetStream first
     * would fail with HPDF_INVALID_OPERATION when no stream exists yet
     * (fresh document). So: SaveToStream, then query/copy. */
    if (haru_rc(HPDF_SaveToStream(d->pdf), d) != PDF_OK)
        return PDF_ERR_BACKEND_FAILED;

    size = HPDF_GetStreamSize(d->pdf);
    if (!buf || *inout_size < (size_t)size)
    {
        *inout_size = (size_t)size;
        return buf ? PDF_ERR_INVALID_PARAM : PDF_OK; /* size query */
    }

    st = HPDF_GetContents(d->pdf, buf, &size);
    if (haru_rc(st, d) != PDF_OK)
        return PDF_ERR_BACKEND_FAILED;
    *inout_size = (size_t)size;
    return PDF_OK;
}

static const char *h_doc_last_error(pdf_backend_doc *doc)
{
    return ((haru_doc *)doc)->error;
}

/* libharu has no "save to arbitrary stream" entry point, but it can
 * serialize into its internal memory stream and then hand that out in
 * chunks via HPDF_ReadFromStream; that keeps peak memory to one internal
 * copy instead of a caller-visible full buffer. */
static pdf_status h_doc_save_to_stream(pdf_backend_doc *doc,
                                       pdf_stream_write_fn write_fn,
                                       void *userdata)
{
    haru_doc *d = (haru_doc *)doc;
    unsigned char chunk[16384];
    HPDF_UINT32 want;
    HPDF_STATUS st;

    /* SaveToStream creates the internal memory stream on first use; calling
     * HPDF_ResetStream before it exists yields HPDF_INVALID_OPERATION. */
    if (haru_rc(HPDF_SaveToStream(d->pdf), d) != PDF_OK)
        return PDF_ERR_BACKEND_FAILED;
    if (haru_rc(HPDF_ResetStream(d->pdf), d) != PDF_OK)
        return PDF_ERR_BACKEND_FAILED;

    want = (HPDF_UINT32)sizeof(chunk);
    st = HPDF_ReadFromStream(d->pdf, chunk, &want);
    while (st == HPDF_OK)
    {
        pdf_status rc;
        if (want == 0) /* defensive: EOF without error status */
            break;
        rc = write_fn(chunk, (size_t)want, userdata);
        if (rc != PDF_OK)
            return rc; /* caller aborted */
        want = (HPDF_UINT32)sizeof(chunk);
        st = HPDF_ReadFromStream(d->pdf, chunk, &want);
    }
    /* HPDF_STREAM_EOF terminates the internal memstream, but the final
     * read may still have delivered a partial chunk alongside it. */
    if (st == HPDF_STREAM_EOF && want > 0)
    {
        pdf_status rc = write_fn(chunk, (size_t)want, userdata);
        if (rc != PDF_OK)
            return rc;
        st = HPDF_OK;
    }
    if (st != HPDF_OK)
        return PDF_ERR_BACKEND_FAILED;
    return PDF_OK;
}

/* ----- info / security / options ------------------------------------------- */

static pdf_status h_doc_set_info(pdf_backend_doc *doc, pdf_info_key key,
                                 const char *value)
{
    static const HPDF_InfoType map[6] = {
        HPDF_INFO_TITLE, HPDF_INFO_AUTHOR, HPDF_INFO_SUBJECT,
        HPDF_INFO_KEYWORDS, HPDF_INFO_CREATOR, HPDF_INFO_PRODUCER
    };
    haru_doc *d = (haru_doc *)doc;

    if ((int)key < 0 || (int)key > 5)
        return PDF_ERR_INVALID_PARAM;
    if (haru_rc(HPDF_SetInfoAttr(d->pdf, map[(int)key], value ? value : ""), d)
        != PDF_OK)
        return PDF_ERR_BACKEND_FAILED;
    return PDF_OK;
}

static pdf_status h_doc_get_info(pdf_backend_doc *doc, pdf_info_key key,
                                 char *buf, size_t *inout_len)
{
    static const HPDF_InfoType map[6] = {
        HPDF_INFO_TITLE, HPDF_INFO_AUTHOR, HPDF_INFO_SUBJECT,
        HPDF_INFO_KEYWORDS, HPDF_INFO_CREATOR, HPDF_INFO_PRODUCER
    };
    haru_doc *d = (haru_doc *)doc;
    const char *src;
    size_t len;

    if ((int)key < 0 || (int)key > 5)
        return PDF_ERR_INVALID_PARAM;
    src = HPDF_GetInfoAttr(d->pdf, map[(int)key]);
    if (!src)
        src = "";
    len = strlen(src) + 1;
    if (buf && *inout_len > 0)
    {
        size_t copy = len - 1;
        if (copy >= *inout_len)
            copy = *inout_len - 1;
        memcpy(buf, src, copy);
        buf[copy] = '\0';
    }
    *inout_len = len;
    return PDF_OK;
}

static pdf_status h_doc_set_encryption(pdf_backend_doc *doc,
                                       pdf_encrypt_mode mode,
                                       const char *owner, const char *user,
                                       unsigned permissions)
{
    haru_doc *d = (haru_doc *)doc;
    HPDF_EncryptMode emode;
    HPDF_UINT keylen = 0;
    HPDF_UINT perm = 0;

    if (mode == PDF_ENCRYPT_NONE)
        return PDF_OK;
    if (!owner && !user)
        return PDF_ERR_INVALID_PARAM;

    switch (mode)
    {
        case PDF_ENCRYPT_RC4_40:
            emode = HPDF_ENCRYPT_R2;
            keylen = 5; /* libharu wants BYTES (5..16); R2 forces 5 */
            break;
        case PDF_ENCRYPT_RC4_128:
            emode = HPDF_ENCRYPT_R3;
            keylen = 16; /* 128-bit = 16 bytes (libharu accepts 5..16) */
            break;
        default:
            /* libharu 2.4 only implements RC4 (revision 2/3). */
            return PDF_ERR_BACKEND_UNSUPPORTED;
    }

    if (permissions & PDF_PERM_PRINT) perm |= HPDF_ENABLE_PRINT;
    if (permissions & PDF_PERM_EDIT) perm |= HPDF_ENABLE_EDIT;
    if (permissions & PDF_PERM_COPY) perm |= HPDF_ENABLE_COPY;
    if (permissions & PDF_PERM_ANNOTATE) perm |= HPDF_ENABLE_EDIT_ALL;
    /* HPDF_ENABLE_FORM_FILLING / _ACCESSIBILITY / _ASSEMBLY /
     * _PRINT_HIGH_RES only exist in newer forks; R3 does not carry them. */

    /* libharu creates the encrypt dict only inside HPDF_SetPassword, and
     * SetEncryptionMode / SetPermission both fail with
     * HPDF_DOC_ENCRYPTDICT_NOT_FOUND (0x100B) until it exists -- so the
     * password MUST be set first. */
    if (haru_rc(HPDF_SetPassword(d->pdf, owner ? owner : "",
                                 user ? user : ""),
                d) != PDF_OK)
        return PDF_ERR_BACKEND_FAILED;
    if (haru_rc(HPDF_SetEncryptionMode(d->pdf, emode, keylen), d) != PDF_OK)
        return PDF_ERR_BACKEND_FAILED;
    if (haru_rc(HPDF_SetPermission(d->pdf, perm), d) != PDF_OK)
        return PDF_ERR_BACKEND_FAILED;
    return PDF_OK;
}

static pdf_status h_doc_set_compression(pdf_backend_doc *doc, unsigned mask)
{
    haru_doc *d = (haru_doc *)doc;
    HPDF_UINT mode = 0;

    if (mask & PDF_COMPRESS_TEXT) mode |= HPDF_COMP_TEXT;
    if (mask & PDF_COMPRESS_IMAGE) mode |= HPDF_COMP_IMAGE;
    if (mask & PDF_COMPRESS_METADATA) mode |= HPDF_COMP_METADATA;
    if (mask == PDF_COMPRESS_ALL) mode = HPDF_COMP_ALL;
    return haru_rc(HPDF_SetCompressionMode(d->pdf, mode), d) == PDF_OK
               ? PDF_OK
               : PDF_ERR_BACKEND_FAILED;
}

static pdf_status h_doc_set_page_mode_use_outlines(pdf_backend_doc *doc,
                                                   int enable)
{
    haru_doc *d = (haru_doc *)doc;
    return haru_rc(HPDF_SetPageMode(
                       d->pdf, enable ? HPDF_PAGE_MODE_USE_OUTLINE
                                      : HPDF_PAGE_MODE_USE_NONE),
                   d)
               == PDF_OK
               ? PDF_OK
               : PDF_ERR_BACKEND_FAILED;
}

/* ----- pages ------------------------------------------------------------------ */

static pdf_status h_page_add(pdf_backend_doc *doc, double w, double h,
                             pdf_page *out_page)
{
    haru_doc *d = (haru_doc *)doc;
    HPDF_Page page = HPDF_AddPage(d->pdf);
    haru_page *payload;

    if (!page)
        return haru_fail(d, "HPDF_AddPage");

    if (haru_rc(HPDF_Page_SetWidth(page, (HPDF_REAL)w), d) != PDF_OK
        || haru_rc(HPDF_Page_SetHeight(page, (HPDF_REAL)h), d) != PDF_OK)
        return PDF_ERR_BACKEND_FAILED;

    payload = (haru_page *)calloc(1, sizeof(*payload));
    if (!payload)
        return PDF_ERR_OUT_OF_MEMORY;
    payload->page = page;
    payload->doc = d;

    *out_page = (pdf_page)malloc(sizeof(struct pdf_page_cell));
    if (!*out_page)
    {
        free(payload);
        return PDF_ERR_OUT_OF_MEMORY;
    }
    (*out_page)->vt = &g_libharu_vt;
    (*out_page)->obj = payload;
    d->page_count++;
    return PDF_OK;
}

static pdf_status h_page_count(pdf_backend_doc *doc, size_t *out_count)
{
    haru_doc *d = (haru_doc *)doc;

    /* O(1), side-effect free -- do not probe HPDF_GetPageByIndex (see
     * haru_doc::page_count for the sticky-error trap). */
    *out_count = d->page_count;
    return PDF_OK;
}

static pdf_status h_page_get_size(pdf_page page, double *out_w, double *out_h)
{
    HPDF_Page p = page_of(page);

    *out_w = (double)HPDF_Page_GetWidth(p);
    *out_h = (double)HPDF_Page_GetHeight(p);
    return PDF_OK;
}

static pdf_status h_page_set_rotation(pdf_page page, int degrees)
{
    haru_doc *d = (haru_doc *)doc_of_page(page);

    return haru_rc(HPDF_Page_SetRotate(page_of(page), (HPDF_UINT16)degrees), d)
               == PDF_OK
               ? PDF_OK
               : PDF_ERR_BACKEND_FAILED;
}

/* ----- fonts ------------------------------------------------------------------- */

static pdf_status make_font_cell(HPDF_Font font, pdf_font *out_font)
{
    haru_font *payload;
    const pdf_backend_vtable *vt = &g_libharu_vt;

    /* Every caller hands over the result of an HPDF_* font call, and all
     * of them can return NULL. Wrapping NULL in a "successful" cell would
     * only move the failure to the first draw call, where it is a crash
     * instead of an error code. */
    if (!font)
        return PDF_ERR_BACKEND_FAILED;

    payload = (haru_font *)calloc(1, sizeof(*payload));
    if (!payload)
        return PDF_ERR_OUT_OF_MEMORY;
    payload->font = font;

    *out_font = (pdf_font)malloc(sizeof(struct pdf_font_cell));
    if (!*out_font)
    {
        free(payload);
        return PDF_ERR_OUT_OF_MEMORY;
    }
    (*out_font)->vt = vt;
    (*out_font)->obj = payload;
    return PDF_OK;
}

static pdf_status h_font_get_std(pdf_backend_doc *doc, pdf_std_font font,
                                 pdf_font *out_font)
{
    haru_doc *d = (haru_doc *)doc;
    HPDF_Font f;
    const char *name = std_font_name(font);
    const char *encoding = "StandardEncoding";

    if (font == PDF_STD_FONT_SYMBOL || font == PDF_STD_FONT_ZAPF_DINGBATS)
        encoding = "FontSpecific";
    f = HPDF_GetFont(d->pdf, name, encoding);
    if (!f)
        return haru_fail(d, "HPDF_GetFont");
    return make_font_cell(f, out_font);
}

static pdf_status h_font_load_ttf(pdf_backend_doc *doc, const char *path,
                                  int embed, pdf_font *out_font)
{
    haru_doc *d = (haru_doc *)doc;
    const char *name;

    if (!embed)
        return PDF_ERR_BACKEND_UNSUPPORTED; /* haru always embeds TTF
                                             * (embedding=HPDF_TRUE is baked
                                             * into HPDF_LoadTTFontFromFile) */
    name = HPDF_LoadTTFontFromFile(d->pdf, path, HPDF_TRUE);
    if (!name)
        return haru_fail(d, "HPDF_LoadTTFontFromFile");
    {
        /* HPDF_GetFont can fail too (e.g. an encoding libharu does not
         * know), and make_font_cell has no doc to clear the latch with. */
        HPDF_Font f = HPDF_GetFont(d->pdf, name, "UTF-8");

        if (!f)
            return haru_fail(d, "HPDF_GetFont(loaded TTF)");
        return make_font_cell(f, out_font);
    }
}

static pdf_status h_font_load_ttf_memory(pdf_backend_doc *doc,
                                         const unsigned char *data, size_t size,
                                         int embed, pdf_font *out_font)
{
    (void)data;
    (void)size;
    (void)embed;
    (void)out_font;
    /* libharu has no in-memory TTF loader (only HPDF_LoadTTFontFromFile);
     * callers must fall back to h_font_load_ttf / h_font_find_system. */
    return PDF_ERR_BACKEND_UNSUPPORTED;
}

static pdf_status h_font_find_system(pdf_backend_doc *doc, const char *family,
                                     int bold, int italic,
                                     pdf_font *out_font)
{
    /* libharu has no system font search; resolve common families by name. */
    haru_doc *d = (haru_doc *)doc;
    const char *name = NULL;

    (void)bold;
    (void)italic;
    if (family)
    {
        if (ci_equal(family, "arial") || ci_equal(family, "helvetica"))
            name = "Helvetica";
        else if (ci_equal(family, "times")
                 || ci_equal(family, "times new roman"))
            name = "Times-Roman";
        else if (ci_equal(family, "courier")
                 || ci_equal(family, "courier new"))
            name = "Courier";
    }
    if (!name)
        return PDF_ERR_BACKEND_UNSUPPORTED;
    return make_font_cell(
        HPDF_GetFont(d->pdf, name, "StandardEncoding"), out_font);
}

static void h_font_free(pdf_font font)
{
    free(((pdf_font)font)->obj);
}

static pdf_status h_font_text_width(pdf_font font, const char *text,
                                    double size, double *out_width)
{
    HPDF_Font f = ((haru_font *)((pdf_font)font)->obj)->font;
    HPDF_TextWidth tw;

    tw = HPDF_Font_TextWidth(f, (const HPDF_BYTE *)text,
                             (HPDF_UINT)strlen(text));
    *out_width = (double)tw.width * size / 1000.0;
    return PDF_OK;
}

static pdf_status h_font_ascent(pdf_font font, double size, double *out)
{
    HPDF_Font f = ((haru_font *)((pdf_font)font)->obj)->font;

    *out = (double)HPDF_Font_GetAscent(f) * size / 1000.0;
    return PDF_OK;
}

static pdf_status h_font_descent(pdf_font font, double size, double *out)
{
    HPDF_Font f = ((haru_font *)((pdf_font)font)->obj)->font;

    *out = (double)HPDF_Font_GetDescent(f) * size / 1000.0;
    return PDF_OK;
}

/* ----- text ------------------------------------------------------------------------ */

static pdf_status h_draw_text(pdf_page page, pdf_font font,
                              const char *utf8_text, double x, double y,
                              double size)
{
    haru_doc *d = (haru_doc *)doc_of_page(page);
    HPDF_Page p = page_of(page);
    HPDF_Font f = ((haru_font *)((pdf_font)font)->obj)->font;

    if (haru_rc(HPDF_Page_BeginText(p), d) != PDF_OK)
        return PDF_ERR_BACKEND_FAILED;
    if (haru_rc(HPDF_Page_SetFontAndSize(p, f, (HPDF_REAL)size), d)
        != PDF_OK)
        return PDF_ERR_BACKEND_FAILED;
    if (haru_rc(HPDF_Page_TextOut(p, (HPDF_REAL)x, (HPDF_REAL)y, utf8_text),
                d)
        != PDF_OK)
        return PDF_ERR_BACKEND_FAILED;
    if (haru_rc(HPDF_Page_EndText(p), d) != PDF_OK)
        return PDF_ERR_BACKEND_FAILED;
    return PDF_OK;
}

static pdf_status h_draw_text_aligned(pdf_page page, pdf_font font,
                                      const char *utf8_text, double x, double y,
                                      double size, double width,
                                      pdf_align align)
{
    HPDF_Page p = page_of(page);
    HPDF_Font f = ((haru_font *)((pdf_font)font)->obj)->font;
    haru_doc *d = (haru_doc *)doc_of_page(page);
    double text_w = 0;
    double draw_x = x;

    if (h_font_text_width(font, utf8_text, size, &text_w) != PDF_OK)
        return PDF_ERR_BACKEND_FAILED;

    if (align == PDF_ALIGN_CENTER)
        draw_x = x + (width - text_w) / 2.0;
    else if (align == PDF_ALIGN_RIGHT)
        draw_x = x + width - text_w;

    if (haru_rc(HPDF_Page_BeginText(p), d) != PDF_OK)
        return PDF_ERR_BACKEND_FAILED;
    if (haru_rc(HPDF_Page_SetFontAndSize(p, f, (HPDF_REAL)size), d)
        != PDF_OK)
        return PDF_ERR_BACKEND_FAILED;
    if (haru_rc(HPDF_Page_TextOut(p, (HPDF_REAL)draw_x, (HPDF_REAL)y,
                                  utf8_text),
                d)
        != PDF_OK)
        return PDF_ERR_BACKEND_FAILED;
    if (haru_rc(HPDF_Page_EndText(p), d) != PDF_OK)
        return PDF_ERR_BACKEND_FAILED;
    return PDF_OK;
}

/* ----- graphics state ------------------------------------------------------------------ */

static pdf_status h_gstate_save(pdf_page page)
{
    haru_doc *d = (haru_doc *)doc_of_page(page);
    return haru_rc(HPDF_Page_GSave(page_of(page)), d) == PDF_OK
               ? PDF_OK
               : PDF_ERR_BACKEND_FAILED;
}

static pdf_status h_gstate_restore(pdf_page page)
{
    haru_doc *d = (haru_doc *)doc_of_page(page);
    return haru_rc(HPDF_Page_GRestore(page_of(page)), d) == PDF_OK
               ? PDF_OK
               : PDF_ERR_BACKEND_FAILED;
}

static pdf_status h_set_transform(pdf_page page, double a, double b, double c,
                                  double d_, double e, double f)
{
    haru_doc *d = (haru_doc *)doc_of_page(page);
    return haru_rc(HPDF_Page_Concat(page_of(page), (HPDF_REAL)a, (HPDF_REAL)b,
                                    (HPDF_REAL)c, (HPDF_REAL)d_, (HPDF_REAL)e,
                                    (HPDF_REAL)f),
                   d)
               == PDF_OK
               ? PDF_OK
               : PDF_ERR_BACKEND_FAILED;
}

static pdf_status h_set_line_width(pdf_page page, double w)
{
    haru_doc *d = (haru_doc *)doc_of_page(page);
    return haru_rc(HPDF_Page_SetLineWidth(page_of(page), (HPDF_REAL)w), d)
               == PDF_OK
               ? PDF_OK
               : PDF_ERR_BACKEND_FAILED;
}

static pdf_status h_set_line_cap(pdf_page page, pdf_line_cap cap)
{
    haru_doc *d = (haru_doc *)doc_of_page(page);
    return haru_rc(HPDF_Page_SetLineCap(page_of(page), map_cap(cap)), d)
               == PDF_OK
               ? PDF_OK
               : PDF_ERR_BACKEND_FAILED;
}

static pdf_status h_set_line_join(pdf_page page, pdf_line_join join)
{
    haru_doc *d = (haru_doc *)doc_of_page(page);
    return haru_rc(HPDF_Page_SetLineJoin(page_of(page), map_join(join)), d)
               == PDF_OK
               ? PDF_OK
               : PDF_ERR_BACKEND_FAILED;
}

static pdf_status h_set_miter_limit(pdf_page page, double limit)
{
    haru_doc *d = (haru_doc *)doc_of_page(page);
    return haru_rc(HPDF_Page_SetMiterLimit(page_of(page), (HPDF_REAL)limit), d)
               == PDF_OK
               ? PDF_OK
               : PDF_ERR_BACKEND_FAILED;
}

static pdf_status h_set_line_dash(pdf_page page, const double *pattern,
                                  size_t n, double phase)
{
    haru_doc *d = (haru_doc *)doc_of_page(page);
    HPDF_REAL dash[8];
    HPDF_UINT i;

    if (n == 0)
        return haru_rc(HPDF_Page_SetDash(page_of(page), NULL, 0, 0), d)
                       == PDF_OK
                   ? PDF_OK
                   : PDF_ERR_BACKEND_FAILED;
    for (i = 0; i < n && i < 8; i++)
        dash[i] = (HPDF_REAL)pattern[i];
    return haru_rc(HPDF_Page_SetDash(page_of(page), dash, (HPDF_UINT)n,
                                     (HPDF_REAL)phase),
                   d)
               == PDF_OK
               ? PDF_OK
               : PDF_ERR_BACKEND_FAILED;
}

static pdf_status h_set_fill_color(pdf_page page, const pdf_color *c)
{
    haru_doc *d = (haru_doc *)doc_of_page(page);
    pdf_color nc = normalize_color(c);
    HPDF_STATUS st;

    if (nc.alpha < 1.0)
    {
        HPDF_ExtGState gs = HPDF_CreateExtGState(d->pdf);
        if (gs)
        {
            HPDF_ExtGState_SetAlphaFill(gs, (HPDF_REAL)nc.alpha);
            HPDF_ExtGState_SetAlphaStroke(gs, (HPDF_REAL)nc.alpha);
            HPDF_Page_SetExtGState(page_of(page), gs);
        }
    }

    if (nc.kind == PDF_COLOR_CMYK)
        st = HPDF_Page_SetCMYKFill(page_of(page), (HPDF_REAL)nc.c1,
                                   (HPDF_REAL)nc.c2, (HPDF_REAL)nc.c3,
                                   (HPDF_REAL)nc.c4);
    else if (nc.kind == PDF_COLOR_GRAY)
        st = HPDF_Page_SetGrayFill(page_of(page), (HPDF_REAL)nc.c1);
    else
    {
        double r, g, b;
        fill_rgb(&nc, &r, &g, &b);
        st = HPDF_Page_SetRGBFill(page_of(page), (HPDF_REAL)r, (HPDF_REAL)g,
                                  (HPDF_REAL)b);
    }
    return haru_rc(st, d) == PDF_OK ? PDF_OK : PDF_ERR_BACKEND_FAILED;
}

static pdf_status h_set_stroke_color(pdf_page page, const pdf_color *c)
{
    haru_doc *d = (haru_doc *)doc_of_page(page);
    pdf_color nc = normalize_color(c);
    HPDF_STATUS st;

    if (nc.kind == PDF_COLOR_CMYK)
        st = HPDF_Page_SetCMYKStroke(page_of(page), (HPDF_REAL)nc.c1,
                                     (HPDF_REAL)nc.c2, (HPDF_REAL)nc.c3,
                                     (HPDF_REAL)nc.c4);
    else if (nc.kind == PDF_COLOR_GRAY)
        st = HPDF_Page_SetGrayStroke(page_of(page), (HPDF_REAL)nc.c1);
    else
    {
        double r, g, b;
        fill_rgb(&nc, &r, &g, &b);
        st = HPDF_Page_SetRGBStroke(page_of(page), (HPDF_REAL)r, (HPDF_REAL)g,
                                    (HPDF_REAL)b);
    }
    return haru_rc(st, d) == PDF_OK ? PDF_OK : PDF_ERR_BACKEND_FAILED;
}

static pdf_status h_set_opacity(pdf_page page, double fill_alpha,
                                double stroke_alpha)
{
    haru_doc *d = (haru_doc *)doc_of_page(page);
    HPDF_ExtGState gs = HPDF_CreateExtGState(d->pdf);

    if (!gs)
        return haru_fail(d, "HPDF_CreateExtGState");
    HPDF_ExtGState_SetAlphaFill(gs, (HPDF_REAL)fill_alpha);
    HPDF_ExtGState_SetAlphaStroke(gs, (HPDF_REAL)stroke_alpha);
    return haru_rc(HPDF_Page_SetExtGState(page_of(page), gs), d) == PDF_OK
               ? PDF_OK
               : PDF_ERR_BACKEND_FAILED;
}

static pdf_status h_set_blend_mode(pdf_page page, pdf_blend_mode mode)
{
    static const HPDF_BlendMode map[12] = {
        HPDF_BM_NORMAL,     HPDF_BM_MULTIPLY,   HPDF_BM_SCREEN,
        HPDF_BM_OVERLAY,    HPDF_BM_DARKEN,     HPDF_BM_LIGHTEN,
        HPDF_BM_COLOR_DODGE, HPDF_BM_COLOR_BUM, HPDF_BM_HARD_LIGHT,
        HPDF_BM_SOFT_LIGHT, HPDF_BM_DIFFERENCE, HPDF_BM_EXCLUSHON
    };
    haru_doc *d = (haru_doc *)doc_of_page(page);
    HPDF_ExtGState gs;

    if ((int)mode < 0 || (int)mode > 11)
        return PDF_ERR_INVALID_PARAM;
    gs = HPDF_CreateExtGState(d->pdf);
    if (!gs)
        return haru_fail(d, "HPDF_CreateExtGState");
    HPDF_ExtGState_SetBlendMode(gs, map[(int)mode]);
    return haru_rc(HPDF_Page_SetExtGState(page_of(page), gs), d) == PDF_OK
               ? PDF_OK
               : PDF_ERR_BACKEND_FAILED;
}

static pdf_status h_set_text_render_mode(pdf_page page,
                                         pdf_text_render_mode mode)
{
    haru_doc *d = (haru_doc *)doc_of_page(page);
    return haru_rc(HPDF_Page_SetTextRenderingMode(page_of(page),
                                                  map_text_mode(mode)),
                   d)
               == PDF_OK
               ? PDF_OK
               : PDF_ERR_BACKEND_FAILED;
}

/* ----- paths -------------------------------------------------------------------------- */

static pdf_status h_path_move_to(pdf_page page, double x, double y)
{
    haru_doc *d = (haru_doc *)doc_of_page(page);
    return haru_rc(HPDF_Page_MoveTo(page_of(page), (HPDF_REAL)x, (HPDF_REAL)y),
                   d)
               == PDF_OK
               ? PDF_OK
               : PDF_ERR_BACKEND_FAILED;
}

static pdf_status h_path_line_to(pdf_page page, double x, double y)
{
    haru_doc *d = (haru_doc *)doc_of_page(page);
    return haru_rc(HPDF_Page_LineTo(page_of(page), (HPDF_REAL)x, (HPDF_REAL)y),
                   d)
               == PDF_OK
               ? PDF_OK
               : PDF_ERR_BACKEND_FAILED;
}

static pdf_status h_path_curve_to(pdf_page page, double x1, double y1,
                                  double x2, double y2, double x3, double y3)
{
    haru_doc *d = (haru_doc *)doc_of_page(page);
    return haru_rc(HPDF_Page_CurveTo(page_of(page), (HPDF_REAL)x1,
                                     (HPDF_REAL)y1, (HPDF_REAL)x2,
                                     (HPDF_REAL)y2, (HPDF_REAL)x3,
                                     (HPDF_REAL)y3),
                   d)
               == PDF_OK
               ? PDF_OK
               : PDF_ERR_BACKEND_FAILED;
}

static pdf_status h_path_close(pdf_page page)
{
    haru_doc *d = (haru_doc *)doc_of_page(page);
    return haru_rc(HPDF_Page_ClosePath(page_of(page)), d) == PDF_OK
               ? PDF_OK
               : PDF_ERR_BACKEND_FAILED;
}

static pdf_status h_path_rectangle(pdf_page page, const pdf_rect *r)
{
    haru_doc *d = (haru_doc *)doc_of_page(page);
    return haru_rc(HPDF_Page_Rectangle(page_of(page), (HPDF_REAL)r->x,
                                       (HPDF_REAL)r->y, (HPDF_REAL)r->w,
                                       (HPDF_REAL)r->h),
                   d)
               == PDF_OK
               ? PDF_OK
               : PDF_ERR_BACKEND_FAILED;
}

static pdf_status h_path_paint(pdf_page page, pdf_path_paint_mode mode)
{
    haru_doc *d = (haru_doc *)doc_of_page(page);
    HPDF_Page p = page_of(page);
    HPDF_STATUS st;

    switch (mode)
    {
        case PDF_PATH_STROKE: st = HPDF_Page_Stroke(p); break;
        case PDF_PATH_FILL: st = HPDF_Page_Fill(p); break;
        case PDF_PATH_FILL_STROKE: st = HPDF_Page_FillStroke(p); break;
        case PDF_PATH_FILL_EVENODD: st = HPDF_Page_Eofill(p); break;
        case PDF_PATH_FILL_STROKE_EVENODD:
            st = HPDF_Page_EofillStroke(p);
            break;
        default: return PDF_ERR_INVALID_PARAM;
    }
    return haru_rc(st, d) == PDF_OK ? PDF_OK : PDF_ERR_BACKEND_FAILED;
}

static pdf_status h_path_discard(pdf_page page)
{
    haru_doc *d = (haru_doc *)doc_of_page(page);
    return haru_rc(HPDF_Page_EndPath(page_of(page)), d) == PDF_OK
               ? PDF_OK
               : PDF_ERR_BACKEND_FAILED;
}

/* ----- images ---------------------------------------------------------------------------- */

static pdf_status h_image_load_file(pdf_backend_doc *doc, const char *path,
                                    pdf_image *out_image)
{
    haru_doc *d = (haru_doc *)doc;
    HPDF_Image img = HPDF_LoadPngImageFromFile(d->pdf, path);
    haru_image *payload;

    if (!img)
    {
        HPDF_ResetError(d->pdf);
        img = HPDF_LoadJpegImageFromFile(d->pdf, path);
    }
    if (!img)
    {
        snprintf(d->error, sizeof(d->error), "cannot load image %s", path);
        return PDF_ERR_BACKEND_FAILED;
    }

    payload = (haru_image *)calloc(1, sizeof(*payload));
    if (!payload)
        return PDF_ERR_OUT_OF_MEMORY;
    payload->image = img;

    *out_image = (pdf_image)malloc(sizeof(struct pdf_image_cell));
    if (!*out_image)
    {
        free(payload);
        return PDF_ERR_OUT_OF_MEMORY;
    }
    (*out_image)->vt = &g_libharu_vt;
    (*out_image)->obj = payload;
    return PDF_OK;
}

static pdf_status h_image_load_memory(pdf_backend_doc *doc,
                                      const unsigned char *data, size_t size,
                                      pdf_image *out_image)
{
    haru_doc *d = (haru_doc *)doc;
    HPDF_Image img = HPDF_LoadPngImageFromMem(d->pdf, data,
                                              (HPDF_UINT)size);
    haru_image *payload;

    if (!img)
    {
        HPDF_ResetError(d->pdf);
        img = HPDF_LoadJpegImageFromMem(d->pdf, data, (HPDF_UINT)size);
    }
    if (!img)
        return haru_fail(d, "image decode (neither PNG nor JPEG)");

    payload = (haru_image *)calloc(1, sizeof(*payload));
    if (!payload)
        return PDF_ERR_OUT_OF_MEMORY;
    payload->image = img;

    *out_image = (pdf_image)malloc(sizeof(struct pdf_image_cell));
    if (!*out_image)
    {
        free(payload);
        return PDF_ERR_OUT_OF_MEMORY;
    }
    (*out_image)->vt = &g_libharu_vt;
    (*out_image)->obj = payload;
    return PDF_OK;
}

static void h_image_free(pdf_image image)
{
    /* HPDF images are owned by the document; just drop the cell. */
    free(((pdf_image)image)->obj);
}

static pdf_status h_image_get_size(pdf_image image, size_t *out_w,
                                   size_t *out_h)
{
    HPDF_Image img = ((haru_image *)((pdf_image)image)->obj)->image;

    *out_w = (size_t)HPDF_Image_GetWidth(img);
    *out_h = (size_t)HPDF_Image_GetHeight(img);
    return PDF_OK;
}

static pdf_status h_draw_image(pdf_page page, pdf_image image,
                               const pdf_rect *dest)
{
    haru_doc *d = (haru_doc *)doc_of_page(page);
    HPDF_Image img = ((haru_image *)((pdf_image)image)->obj)->image;

    return haru_rc(HPDF_Page_DrawImage(page_of(page), img, (HPDF_REAL)dest->x,
                                       (HPDF_REAL)dest->y, (HPDF_REAL)dest->w,
                                       (HPDF_REAL)dest->h),
                   d)
               == PDF_OK
               ? PDF_OK
               : PDF_ERR_BACKEND_FAILED;
}

/* ----- annotations & outlines ---------------------------------------------------------------- */

static pdf_status h_annot_create(pdf_page page, pdf_annot_type type,
                                 const pdf_rect *rect, const char *content,
                                 pdf_annot *out_annot)
{
    haru_doc *d = (haru_doc *)doc_of_page(page);
    HPDF_Page p = page_of(page);
    HPDF_Rect hr;
    HPDF_Annotation a = NULL;
    haru_annot *payload;

    hr.left = (HPDF_REAL)rect->x;
    hr.bottom = (HPDF_REAL)rect->y;
    hr.right = (HPDF_REAL)(rect->x + rect->w);
    hr.top = (HPDF_REAL)(rect->y + rect->h);

    switch (type)
    {
        case PDF_ANNOT_TEXT:
            a = HPDF_Page_CreateTextAnnot(p, hr, content ? content : "", NULL);
            break;
        case PDF_ANNOT_HIGHLIGHT:
            a = HPDF_Page_CreateHighlightAnnot(p, hr, content ? content : "",
                                               NULL);
            break;
        case PDF_ANNOT_UNDERLINE:
            a = HPDF_Page_CreateUnderlineAnnot(p, hr, content ? content : "",
                                               NULL);
            break;
        case PDF_ANNOT_SQUIGGLY:
            a = HPDF_Page_CreateSquigglyAnnot(p, hr, content ? content : "",
                                              NULL);
            break;
        case PDF_ANNOT_STRIKEOUT:
            a = HPDF_Page_CreateStrikeOutAnnot(p, hr, content ? content : "",
                                               NULL);
            break;
        case PDF_ANNOT_SQUARE:
            a = HPDF_Page_CreateSquareAnnot(p, hr, content ? content : "",
                                            NULL);
            break;
        case PDF_ANNOT_CIRCLE:
            a = HPDF_Page_CreateCircleAnnot(p, hr, content ? content : "",
                                            NULL);
            break;
        case PDF_ANNOT_LINK_URI:
            /* The API contract is that the URL arrives in `content`, and
             * the PoDoFo backend materialises the link right here. Doing
             * the same keeps set_color/author/subject usable straight away
             * - formerly the deferred path left payload->annot NULL, so
             * the annotation silently never reached the output PDF unless
             * the caller also called annot_link_to_page. */
            if (content && *content)
            {
                a = HPDF_Page_CreateURILinkAnnot(p, hr, content);
                if (!a)
                    return haru_fail(d, "HPDF_Page_CreateURILinkAnnot");
                break; /* fall through to the shared payload setup */
            }
            /* no URL supplied: fall through to the deferred path */
            /* fallthrough */
        case PDF_ANNOT_LINK_GOTO:
        {
            /* libharu creates links in one shot; defer to the
             * annot_link_to_page call by remembering the rect. */
            payload = (haru_annot *)calloc(1, sizeof(*payload));
            if (!payload)
                return PDF_ERR_OUT_OF_MEMORY;
            payload->page = p;
            payload->pdf = d->pdf;
            payload->type = type;
            payload->rect = *rect;
            payload->link_created = 0;
            payload->annot = NULL;

            *out_annot = (pdf_annot)malloc(sizeof(struct pdf_annot_cell));
            if (!*out_annot)
            {
                free(payload);
                return PDF_ERR_OUT_OF_MEMORY;
            }
            (*out_annot)->vt = &g_libharu_vt;
            (*out_annot)->obj = payload;
            return PDF_OK;
        }
        default:
            return PDF_ERR_INVALID_PARAM;
    }

    if (!a)
        return haru_fail(d, "HPDF_Page_Create*Annot");

    /* Highlight / Underline / Squiggly / StrikeOut are *text markup*
     * annotations, and the PDF spec makes /QuadPoints REQUIRED on them:
     * the quadrilateral is what tells the viewer which region to mark up.
     * libharu's HPDF_Page_Create*Annot helpers build the dictionary and
     * the /Rect but never write /QuadPoints, so a markup annotation made
     * without this call is one a viewer cannot place at all - MuPDF, for
     * one, falls back to a rectangle of INT_MIN..INT_MAX.
     *
     * Mind the argument order. The parameters are NAMED lb, rb, rt, lt
     * (left-bottom, right-bottom, right-top, left-top), but libharu
     * writes the four points into the array in the order they are passed,
     * without reordering. The spec wants (upper-left, upper-right,
     * lower-left, lower-right), so the arguments below are passed in THAT
     * order and the names are simply wrong for this call. Passing them in
     * the order the names suggest produces a crossed quadrilateral: the
     * annotation is then placed at a bogus rectangle, which is exactly
     * what the showcase's independent cross-check caught. */
    if (type == PDF_ANNOT_HIGHLIGHT || type == PDF_ANNOT_UNDERLINE ||
        type == PDF_ANNOT_SQUIGGLY  || type == PDF_ANNOT_STRIKEOUT)
    {
        HPDF_Point ul, ur, ll, lr;

        ul.x = hr.left;  ul.y = hr.top;
        ur.x = hr.right; ur.y = hr.top;
        ll.x = hr.left;  ll.y = hr.bottom;
        lr.x = hr.right; lr.y = hr.bottom;

        if (HPDF_TextMarkupAnnot_SetQuadPoints(a, ul, ur, ll, lr) != HPDF_OK)
            return haru_fail(d, "HPDF_TextMarkupAnnot_SetQuadPoints");
    }

    payload = (haru_annot *)calloc(1, sizeof(*payload));
    if (!payload)
        return PDF_ERR_OUT_OF_MEMORY;
    payload->page = p;
    payload->pdf = d->pdf;
    payload->annot = a;
    payload->type = type;
    payload->rect = *rect;
    payload->link_created = 0;

    *out_annot = (pdf_annot)malloc(sizeof(struct pdf_annot_cell));
    if (!*out_annot)
    {
        free(payload);
        return PDF_ERR_OUT_OF_MEMORY;
    }
    (*out_annot)->vt = &g_libharu_vt;
    (*out_annot)->obj = payload;
    return PDF_OK;
}

static pdf_status h_annot_link_to_page(pdf_annot annot, pdf_page *dest,
                                       const char *uri)
{
    haru_annot *a = (haru_annot *)((pdf_annot)annot)->obj;
    haru_doc *d = (haru_doc *)a->pdf;
    HPDF_Rect hr;
    HPDF_Annotation created;

    if (a->link_created || a->annot)
        return PDF_ERR_INVALID_PARAM; /* one link per annot cell */
    hr.left = (HPDF_REAL)a->rect.x;
    hr.bottom = (HPDF_REAL)a->rect.y;
    hr.right = (HPDF_REAL)(a->rect.x + a->rect.w);
    hr.top = (HPDF_REAL)(a->rect.y + a->rect.h);

    if (uri)
    {
        created = HPDF_Page_CreateURILinkAnnot(a->page, hr, uri);
        if (!created)
            return haru_fail(d, "HPDF_Page_CreateURILinkAnnot");
    }
    else if (dest)
    {
        HPDF_Destination dst = HPDF_Page_CreateDestination(page_of(*dest));

        if (haru_rc(HPDF_Destination_SetFit(dst), d) != PDF_OK)
            return PDF_ERR_BACKEND_FAILED;
        created = HPDF_Page_CreateLinkAnnot(a->page, hr, dst);
        if (!created)
            return haru_fail(d, "HPDF_Page_CreateLinkAnnot");
    }
    else
    {
        return PDF_ERR_INVALID_PARAM;
    }

    a->annot = created;
    a->link_created = 1;
    return PDF_OK;
}

/* Every dict-backed setter needs a materialised annotation. A GOTO link is
 * deferred until h_annot_link_to_page() supplies the destination, so the
 * cell legitimately holds no annotation before that call - and libharu's
 * HPDF_MarkupAnnot_* / HPDF_TextAnnot_* helpers do not NULL-check their
 * argument, they fault the process. */
static int annot_ready(const haru_annot *a)
{
    return a->annot != NULL;
}

static pdf_status h_annot_set_color(pdf_annot annot, const pdf_color *c)
{
    haru_annot *a = (haru_annot *)((pdf_annot)annot)->obj;
    haru_doc *d = (haru_doc *)a->pdf;
    pdf_color nc = normalize_color(c);
    HPDF_STATUS st;

    if (!annot_ready(a))
        return PDF_ERR_INVALID_PARAM;

    if (nc.kind == PDF_COLOR_CMYK)
    {
        HPDF_CMYKColor k;
        k.c = (HPDF_REAL)nc.c1;
        k.m = (HPDF_REAL)nc.c2;
        k.y = (HPDF_REAL)nc.c3;
        k.k = (HPDF_REAL)nc.c4;
        st = HPDF_Annot_SetCMYKColor(a->annot, k);
    }
    else if (nc.kind == PDF_COLOR_GRAY)
    {
        st = HPDF_Annot_SetGrayColor(a->annot, (HPDF_REAL)nc.c1);
    }
    else
    {
        HPDF_RGBColor rgb;
        double r, g, b;
        fill_rgb(&nc, &r, &g, &b);
        rgb.r = (HPDF_REAL)r;
        rgb.g = (HPDF_REAL)g;
        rgb.b = (HPDF_REAL)b;
        st = HPDF_Annot_SetRGBColor(a->annot, rgb);
    }
    return haru_rc(st, d) == PDF_OK ? PDF_OK : PDF_ERR_BACKEND_FAILED;
}

static pdf_status h_annot_set_icon(pdf_annot annot, pdf_annot_icon icon)
{
    haru_annot *a = (haru_annot *)((pdf_annot)annot)->obj;
    haru_doc *d = (haru_doc *)a->pdf;

    /* /Name only exists on a text annotation. libharu's
     * HPDF_TextAnnot_SetIcon does not check the subtype and reads
     * type-specific state, so calling it on a markup or link annotation
     * faults the process instead of returning an error. Decline. */
    if (!annot_ready(a) || a->type != PDF_ANNOT_TEXT)
        return PDF_ERR_BACKEND_UNSUPPORTED;

    return haru_rc(HPDF_TextAnnot_SetIcon(a->annot, map_icon(icon)), d)
               == PDF_OK
               ? PDF_OK
               : PDF_ERR_BACKEND_FAILED;
}

static pdf_status h_annot_set_opacity(pdf_annot annot, double alpha)
{
    haru_annot *a = (haru_annot *)((pdf_annot)annot)->obj;
    haru_doc *d = (haru_doc *)a->pdf;

    if (!annot_ready(a))
        return PDF_ERR_INVALID_PARAM;
    return haru_rc(HPDF_MarkupAnnot_SetTransparency(a->annot,
                                                    (HPDF_REAL)alpha),
                   d)
               == PDF_OK
               ? PDF_OK
               : PDF_ERR_BACKEND_FAILED;
}

static pdf_status h_annot_set_author(pdf_annot annot, const char *author)
{
    haru_annot *a = (haru_annot *)((pdf_annot)annot)->obj;
    haru_doc *d = (haru_doc *)a->pdf;

    if (!annot_ready(a))
        return PDF_ERR_INVALID_PARAM;
    return haru_rc(HPDF_MarkupAnnot_SetTitle(a->annot, author ? author : ""), d)
               == PDF_OK
               ? PDF_OK
               : PDF_ERR_BACKEND_FAILED;
}

static pdf_status h_annot_set_subject(pdf_annot annot, const char *subject)
{
    haru_annot *a = (haru_annot *)((pdf_annot)annot)->obj;
    haru_doc *d = (haru_doc *)a->pdf;

    if (!annot_ready(a))
        return PDF_ERR_INVALID_PARAM;
    return haru_rc(HPDF_MarkupAnnot_SetSubject(a->annot,
                                               subject ? subject : ""),
                   d)
               == PDF_OK
               ? PDF_OK
               : PDF_ERR_BACKEND_FAILED;
}

static pdf_status h_annot_set_open(pdf_annot annot, int opened)
{
    haru_annot *a = (haru_annot *)((pdf_annot)annot)->obj;
    haru_doc *d = (haru_doc *)a->pdf;

    /* Same reasoning as h_annot_set_icon: /Open is a text annotation key. */
    if (!annot_ready(a) || a->type != PDF_ANNOT_TEXT)
        return PDF_ERR_BACKEND_UNSUPPORTED;

    return haru_rc(HPDF_TextAnnot_SetOpened(a->annot, opened ? HPDF_TRUE
                                                             : HPDF_FALSE),
                   d)
               == PDF_OK
               ? PDF_OK
               : PDF_ERR_BACKEND_FAILED;
}

static void h_annot_free(pdf_annot annot)
{
    free(((pdf_annot)annot)->obj);
}

static pdf_status h_outline_add(pdf_backend_doc *doc, pdf_outline parent,
                                const char *title, pdf_page *dest_page,
                                pdf_outline *out_outline)
{
    haru_doc *d = (haru_doc *)doc;
    HPDF_Outline parent_outline = NULL;
    HPDF_Outline outline;
    haru_outline *payload;

    if (parent)
        parent_outline = ((haru_outline *)((pdf_outline)parent)->obj)->outline;

    outline = HPDF_CreateOutline(d->pdf, parent_outline,
                                 title ? title : "", NULL);
    if (!outline)
        return haru_fail(d, "HPDF_CreateOutline");

    if (dest_page)
    {
        HPDF_Destination dst = HPDF_Page_CreateDestination(page_of(*dest_page));
        if (haru_rc(HPDF_Destination_SetFit(dst), d) != PDF_OK)
            return PDF_ERR_BACKEND_FAILED;
        if (haru_rc(HPDF_Outline_SetDestination(outline, dst), d) != PDF_OK)
            return PDF_ERR_BACKEND_FAILED;
    }

    payload = (haru_outline *)calloc(1, sizeof(*payload));
    if (!payload)
        return PDF_ERR_OUT_OF_MEMORY;
    payload->pdf = d->pdf;
    payload->outline = outline;

    *out_outline = (pdf_outline)malloc(sizeof(struct pdf_outline_cell));
    if (!*out_outline)
    {
        free(payload);
        return PDF_ERR_OUT_OF_MEMORY;
    }
    (*out_outline)->vt = &g_libharu_vt;
    (*out_outline)->obj = payload;
    return PDF_OK;
}

static pdf_status h_outline_set_open(pdf_outline outline, int opened)
{
    haru_outline *o = (haru_outline *)((pdf_outline)outline)->obj;
    haru_doc *d = (haru_doc *)o->pdf;
    return haru_rc(HPDF_Outline_SetOpened(o->outline,
                                          opened ? HPDF_TRUE : HPDF_FALSE),
                   d)
               == PDF_OK
               ? PDF_OK
               : PDF_ERR_BACKEND_FAILED;
}

static void h_outline_free(pdf_outline outline)
{
    free(((pdf_outline)outline)->obj);
}

/* ----- the vtable -------------------------------------------------------------------------- */

const pdf_backend_vtable g_libharu_vt = {
    "libharu",
    HPDF_VERSION_TEXT,

    h_doc_create,
    h_doc_free,
    h_doc_save_to_file,
    h_doc_save_to_memory,
    h_doc_last_error,

    h_doc_set_info,
    h_doc_get_info,
    h_doc_set_encryption,
    h_doc_set_compression,
    h_doc_set_page_mode_use_outlines,

    h_page_add,
    h_page_count,
    h_page_get_size,
    h_page_set_rotation,

    h_font_get_std,
    h_font_load_ttf,
    h_font_load_ttf_memory,
    h_font_find_system,
    h_font_free,
    h_font_text_width,
    h_font_ascent,
    h_font_descent,

    h_draw_text,
    h_draw_text_aligned,
    NULL, /* draw_text_wrapped: composed by the core */

    h_gstate_save,
    h_gstate_restore,
    h_set_transform,
    h_set_line_width,
    h_set_line_cap,
    h_set_line_join,
    h_set_miter_limit,
    h_set_line_dash,
    h_set_fill_color,
    h_set_stroke_color,
    h_set_opacity,
    h_set_blend_mode,
    h_set_text_render_mode,

    h_path_move_to,
    h_path_line_to,
    h_path_curve_to,
    h_path_close,
    h_path_rectangle,
    h_path_paint,
    h_path_discard,

    h_image_load_file,
    h_image_load_memory,
    h_image_free,
    h_image_get_size,
    h_draw_image,

    h_annot_create,
    h_annot_link_to_page, /* links: annot_create defers, this completes */
    h_annot_set_color,
    h_annot_set_icon,
    h_annot_set_opacity,
    h_annot_set_author,
    h_annot_set_subject,
    h_annot_set_open,
    h_annot_free,
    h_outline_add,
    h_outline_set_open,
    h_outline_free,
    h_doc_save_to_stream,
    NULL, /* doc_create_streamed: libharu cannot push pages to a device
           * incrementally; the core falls back to create + save_to_stream,
           * which buffers in memory (acceptable for bounded jobs). */
    NULL  /* doc_finish: memory-mode documents have nothing to finish */
};

void pdf_backend_libharu_register(void)
{
    static int registered = 0;

    if (registered)
        return;
    registered = 1;
    pdf_backend_register(PDF_BACKEND_LIBHARU, &g_libharu_vt);
}
