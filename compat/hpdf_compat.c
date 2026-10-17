/**
 * hpdf_compat.c - libharu-compatible (HPDF_*) facade over the pdflib core.
 *
 * The public surface and semantics are libharu 2.4.4's where pdflib can
 * express them. The backend is selected per process with HPDF_SetBackend()
 * or PDFLIB_BACKEND (auto|dummy|libharu|podofo); everything below is plain C
 * and calls only the stable pdf_* API from pdflib/pdf_api.h.
 *
 * Deliberate differences from libharu (documented in the header):
 *  - Handles are opaque in every build (no HPDF_Doc_Rec access).
 *  - HPDF_New() materialises a real pdflib document immediately, so the
 *    page/stream APIs behave the same without an explicit HPDF_NewDoc().
 *  - Text is drawn through the engine's one-shot text primitive. The
 *    BT/ET text object state is tracked here; char/word spacing and
 *    horizontal scaling affect the advance, not every glyph shape.
 *  - Functions pdflib cannot express return HPDF_UNSUPPORTED_FUNC instead
 *    of pretending to work.
 */
#include "hpdf.h"

#include "pdflib/pdf_api.h"

#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define HPDF_MAX_GSTATE 32

/* ------------------------------------------------------------------ */
/* Internal records                                                    */
/* ------------------------------------------------------------------ */

typedef struct hpdf_font_rec
{
    pdf_font core;
    char name[128];
    char encoding[64];
    int is_standard;
    struct hpdf_font_rec *next;
} hpdf_font_rec;

typedef struct hpdf_gstate_snap
{
    pdf_text_render_mode render_mode;
    double line_width;
    pdf_line_cap line_cap;
    pdf_line_join line_join;
    double miter_limit;
    double char_space, word_space, hscale, leading, rise;
    pdf_color fill, stroke;
    double dash[8];
    size_t dash_n;
    double dash_phase;
} hpdf_gstate_snap;

typedef struct hpdf_page_rec
{
    pdf_page core;
    struct hpdf_doc_rec *doc;
    struct hpdf_page_rec *next;

    double width, height;
    HPDF_Point cur_pos;
    HPDF_Point text_pos;

    /* text object / text state */
    int in_text;
    double tm_a, tm_b, tm_c, tm_d;
    double font_size;
    hpdf_font_rec *font;
    pdf_text_render_mode render_mode;
    double char_space, word_space, hscale, leading, rise;

    /* graphics state */
    double line_width;
    pdf_line_cap line_cap;
    pdf_line_join line_join;
    double miter_limit;
    pdf_color fill, stroke;
    double dash[8];
    size_t dash_n;
    double dash_phase;
    hpdf_gstate_snap stack[HPDF_MAX_GSTATE];
    int depth;

    /* Cached accessors that pdflib does not expose. */
    double fill_alpha, stroke_alpha;
    pdf_blend_mode blend_mode;
} hpdf_page_rec;

typedef struct hpdf_dest_rec
{
    struct hpdf_doc_rec *doc;
    HPDF_Page page;
    HPDF_DestinationType type;
    HPDF_REAL left, bottom, right, top, zoom;
    struct hpdf_dest_rec *next;
} hpdf_dest_rec;

typedef struct hpdf_extg_rec
{
    double fill_alpha, stroke_alpha;
    pdf_blend_mode blend_mode;
    int has_blend;
    struct hpdf_extg_rec *next;
} hpdf_extg_rec;

typedef struct hpdf_handle_node
{
    void *handle;
    int kind; /* 1 = image, 2 = annot, 3 = outline */
    struct hpdf_handle_node *next;
} hpdf_handle_node;

typedef struct hpdf_doc_rec
{
    pdf_doc core;
    int has_doc;

    HPDF_Error_Handler error_fn;
    void *user_data;
    HPDF_STATUS error_no;
    HPDF_STATUS detail_no;

    hpdf_page_rec *pages;
    hpdf_page_rec *pages_tail;
    HPDF_Page current_page;
    hpdf_font_rec *fonts;
    hpdf_dest_rec *dests;
    hpdf_extg_rec *extgs;
    hpdf_handle_node *handles;

    /* info / viewer state */
    HPDF_PageLayout layout;
    HPDF_PageMode page_mode;
    HPDF_UINT viewer_pref;
    char info_buf[2048];

    /* memory stream */
    unsigned char *stream;
    size_t stream_size;
    size_t stream_pos;

    /* deferred encryption (libharu needs SetPassword first) */
    char owner_pass[256];
    char user_pass[256];
    int have_password;
    int encryption_mode; /* HPDF_ENCRYPT_R2/R3, 0 = default */
    unsigned long permissions;
    int encrypt_applied;

    int compression;
} hpdf_doc_rec;

/* ------------------------------------------------------------------ */
/* Small helpers                                                       */
/* ------------------------------------------------------------------ */

static void *hpdf_xmemdup(const void *p, size_t n)
{
    void *q;
    if (!p || n == 0)
        return NULL;
    q = malloc(n);
    if (q)
        memcpy(q, p, n);
    return q;
}

static char *hpdf_xstrdup(const char *s)
{
    size_t n;
    char *p;
    if (!s)
        return NULL;
    n = strlen(s) + 1;
    p = (char *)malloc(n);
    if (p)
        memcpy(p, s, n);
    return p;
}

static int hpdf_ci_equal(const char *a, const char *b)
{
    if (!a || !b)
        return a == b;
    while (*a && *b)
    {
        char ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb - 'A' + 'a');
        if (ca != cb)
            return 0;
        ++a;
        ++b;
    }
    return *a == *b;
}

static double hpdf_clamp01(double v)
{
    if (v < 0) return 0;
    if (v > 1) return 1;
    return v;
}

static double hpdf_dist(const HPDF_Point *a, const HPDF_Point *b)
{
    double dx = (double)a->x - b->x;
    double dy = (double)a->y - b->y;
    return sqrt(dx * dx + dy * dy);
}

/* Number of code points in a UTF-8 prefix. */
static size_t hpdf_utf8_count(const char *s, size_t n)
{
    size_t i = 0, count = 0;
    while (i < n)
    {
        unsigned char c = (unsigned char)s[i];
        size_t adv = 1;
        if ((c & 0xE0) == 0xC0) adv = 2;
        else if ((c & 0xF0) == 0xE0) adv = 3;
        else if ((c & 0xF8) == 0xF0) adv = 4;
        if (i + adv > n)
            adv = 1;
        i += adv;
        ++count;
    }
    return count;
}

/* Points to the next UTF-8 code point. */
static const char *hpdf_utf8_next(const char *s)
{
    unsigned char c = (unsigned char)*s;
    if (c < 0x80) return s + 1;
    if ((c & 0xE0) == 0xC0 && s[1]) return s + 2;
    if ((c & 0xF0) == 0xE0 && s[1] && s[2]) return s + 3;
    if ((c & 0xF8) == 0xF0 && s[1] && s[2] && s[3]) return s + 4;
    return s + 1;
}

static int hpdf_is_space(const char *s, size_t n)
{
    if (n == 0)
        return 0;
    return s[0] == ' ' || s[0] == '\t' || s[0] == '\n' || s[0] == '\r';
}

/* ------------------------------------------------------------------ */
/* Error handling                                                      */
/* ------------------------------------------------------------------ */

static HPDF_STATUS hpdf_map_pdf_status(pdf_status rc)
{
    switch (rc)
    {
        case PDF_OK:                      return HPDF_OK;
        case PDF_ERR_INVALID_HANDLE:      return HPDF_INVALID_DOCUMENT;
        case PDF_ERR_INVALID_PARAM:       return HPDF_INVALID_PARAMETER;
        case PDF_ERR_INVALID_STATE:       return HPDF_INVALID_DOCUMENT_STATE;
        case PDF_ERR_BACKEND_UNSUPPORTED: return HPDF_UNSUPPORTED_FUNC;
        case PDF_ERR_BACKEND_FAILED:      return HPDF_INVALID_OPERATION;
        case PDF_ERR_BACKEND_NOT_FOUND:   return HPDF_INVALID_DOCUMENT;
        case PDF_ERR_OUT_OF_MEMORY:       return HPDF_FAILD_TO_ALLOC_MEM;
        case PDF_ERR_IO:                  return HPDF_FILE_IO_ERROR;
        default:                          return HPDF_ERR_UNKNOWN_CLASS;
    }
}

static HPDF_STATUS hpdf_set_error(hpdf_doc_rec *d, HPDF_STATUS code,
                                  HPDF_STATUS detail)
{
    if (d)
    {
        d->error_no = code;
        d->detail_no = detail;
        if (d->error_fn)
            d->error_fn(code, detail, d->user_data);
    }
    return code;
}

static HPDF_STATUS hpdf_fail(hpdf_doc_rec *d, HPDF_STATUS code)
{
    return hpdf_set_error(d, code, 0);
}

static HPDF_STATUS hpdf_fail_pdf(hpdf_doc_rec *d, pdf_status rc)
{
    return hpdf_set_error(d, hpdf_map_pdf_status(rc), (HPDF_STATUS)rc);
}

static int hpdf_doc_ok(hpdf_doc_rec *d)
{
    return d && d->has_doc && d->core;
}

static hpdf_doc_rec *hpdf_doc_of_page(HPDF_Page page)
{
    return page ? ((hpdf_page_rec *)page)->doc : NULL;
}

/* ------------------------------------------------------------------ */
/* Handle tracking (cells must be released before the backend doc)     */
/* ------------------------------------------------------------------ */

static void hpdf_track(hpdf_doc_rec *d, void *handle, int kind)
{
    hpdf_handle_node *n;
    if (!d || !handle)
        return;
    n = (hpdf_handle_node *)malloc(sizeof(*n));
    if (!n)
        return;
    n->handle = handle;
    n->kind = kind;
    n->next = d->handles;
    d->handles = n;
}

static void hpdf_free_handles(hpdf_doc_rec *d)
{
    hpdf_handle_node *n = d ? d->handles : NULL;
    while (n)
    {
        hpdf_handle_node *next = n->next;
        if (n->kind == 1)
            pdf_image_free((pdf_image)n->handle);
        else if (n->kind == 2)
            pdf_annot_free((pdf_annot)n->handle);
        else if (n->kind == 3)
            pdf_outline_free((pdf_outline)n->handle);
        free(n);
        n = next;
    }
    if (d)
        d->handles = NULL;
}

static void hpdf_free_fonts(hpdf_doc_rec *d)
{
    hpdf_font_rec *f = d ? d->fonts : NULL;
    while (f)
    {
        hpdf_font_rec *next = f->next;
        if (f->core)
            pdf_font_free(f->core);
        free(f);
        f = next;
    }
    if (d)
        d->fonts = NULL;
}

static void hpdf_free_pages(hpdf_doc_rec *d)
{
    hpdf_page_rec *p = d ? d->pages : NULL;
    while (p)
    {
        hpdf_page_rec *next = p->next;
        free(p);
        p = next;
    }
    if (d)
    {
        d->pages = NULL;
        d->pages_tail = NULL;
        d->current_page = NULL;
    }
}

static void hpdf_free_dests(hpdf_doc_rec *d)
{
    hpdf_dest_rec *x = d ? d->dests : NULL;
    while (x)
    {
        hpdf_dest_rec *next = x->next;
        free(x);
        x = next;
    }
    if (d)
        d->dests = NULL;
}

static void hpdf_free_extgs(hpdf_doc_rec *d)
{
    hpdf_extg_rec *x = d ? d->extgs : NULL;
    while (x)
    {
        hpdf_extg_rec *next = x->next;
        free(x);
        x = next;
    }
    if (d)
        d->extgs = NULL;
}

static void hpdf_doc_free_content(hpdf_doc_rec *d)
{
    if (!d)
        return;
    hpdf_free_handles(d);
    hpdf_free_fonts(d);
    hpdf_free_pages(d);
    hpdf_free_dests(d);
    hpdf_free_extgs(d);
    free(d->stream);
    d->stream = NULL;
    d->stream_size = 0;
    d->stream_pos = 0;
    if (d->core)
    {
        pdf_doc_free(d->core);
        d->core = NULL;
    }
    d->has_doc = 0;
}

static pdf_status hpdf_new_core_doc(hpdf_doc_rec *d)
{
    pdf_status rc;
    d->core = NULL;
    rc = pdf_doc_create(&d->core);
    if (rc == PDF_OK)
        d->has_doc = 1;
    return rc;
}

/* ------------------------------------------------------------------ */
/* Backend selection                                                  */
/* ------------------------------------------------------------------ */

static void hpdf_apply_env_backend(void)
{
    static int done = 0;
    const char *env;
    char value[32];
    size_t n;

    if (done)
        return;
    done = 1;
    env = getenv("PDFLIB_BACKEND");
    if (!env || !*env)
        return;

    /* cmd.exe's `set VAR=value && ...` keeps the space before `&&` in the
     * value, so trim before comparing. */
    while (*env == ' ' || *env == '\t')
        ++env;
    n = strlen(env);
    while (n > 0 && (env[n - 1] == ' ' || env[n - 1] == '\t'))
        --n;
    if (n >= sizeof(value))
        n = sizeof(value) - 1;
    memcpy(value, env, n);
    value[n] = '\0';
    if (!value[0])
        return;

    if (hpdf_ci_equal(value, "libharu"))
        pdflib_set_backend(PDF_BACKEND_LIBHARU);
    else if (hpdf_ci_equal(value, "podofo"))
        pdflib_set_backend(PDF_BACKEND_PODOFO);
    else if (hpdf_ci_equal(value, "dummy"))
        pdflib_set_backend(PDF_BACKEND_DUMMY);
    else if (hpdf_ci_equal(value, "auto"))
        pdflib_set_backend(PDF_BACKEND_AUTO);
}

HPDF_STATUS HPDF_SetBackend(HPDF_UINT backend)
{
    pdf_status rc;
    /* Accept any registered backend id instead of a hard-coded 0..3. This
     * is what makes a future third/fourth engine usable from HPDF_* code
     * without editing the compatibility layer. AUTO (0) is always valid. */
    if (backend != 0 && !pdflib_has_backend((pdf_backend)backend))
        return HPDF_INVALID_PARAMETER;
    rc = pdflib_set_backend((pdf_backend)backend);
    return rc == PDF_OK ? HPDF_OK : HPDF_INVALID_PARAMETER;
}

HPDF_UINT HPDF_GetBackend(void)
{
    return (HPDF_UINT)pdflib_get_backend();
}

const char *HPDF_GetBackendName(void)
{
    switch (pdflib_get_backend())
    {
        case PDF_BACKEND_AUTO:    return "auto";
        case PDF_BACKEND_LIBHARU: return "libharu";
        case PDF_BACKEND_PODOFO:  return "podofo";
        case PDF_BACKEND_DUMMY:   return "dummy";
        default:                  return "unknown";
    }
}

const char *HPDF_GetBackendVersion(void)
{
    return pdflib_backend_version();
}

/* ------------------------------------------------------------------ */
/* Document lifecycle                                                  */
/* ------------------------------------------------------------------ */

const char *HPDF_GetVersion(void)
{
    return "2.4.4 (pdflib compat)";
}

HPDF_Doc HPDF_NewEx(HPDF_Error_Handler user_error_fn,
                    HPDF_Alloc_Func user_alloc_fn,
                    HPDF_Free_Func user_free_fn,
                    HPDF_UINT mem_pool_buf_size,
                    void *user_data)
{
    hpdf_doc_rec *d;
    pdf_status rc;

    HPDF_UNUSED(user_alloc_fn);
    HPDF_UNUSED(user_free_fn);
    HPDF_UNUSED(mem_pool_buf_size);

    hpdf_apply_env_backend();
    d = (hpdf_doc_rec *)calloc(1, sizeof(*d));
    if (!d)
        return NULL;
    d->error_fn = user_error_fn;
    d->user_data = user_data;
    d->layout = HPDF_PAGE_LAYOUT_SINGLE;
    d->page_mode = HPDF_PAGE_MODE_USE_NONE;
    d->compression = HPDF_COMP_NONE;
    d->encryption_mode = 0;

    rc = hpdf_new_core_doc(d);
    if (rc != PDF_OK)
    {
        hpdf_set_error(d, hpdf_map_pdf_status(rc), (HPDF_STATUS)rc);
        free(d);
        return NULL;
    }
    /* libharu's default compression is HPDF_COMP_NONE. PoDoFo's writer may
     * still flate-compress streams (backend limitation), but the selection
     * matches libharu as closely as the core allows. */
    pdf_doc_set_compression(d->core, PDF_COMPRESS_NONE);
    return (HPDF_Doc)d;
}

HPDF_Doc HPDF_New(HPDF_Error_Handler user_error_fn, void *user_data)
{
    return HPDF_NewEx(user_error_fn, NULL, NULL, 0, user_data);
}

HPDF_STATUS HPDF_SetErrorHandler(HPDF_Doc pdf, HPDF_Error_Handler fn)
{
    hpdf_doc_rec *d = (hpdf_doc_rec *)pdf;
    if (!d)
        return HPDF_INVALID_PARAMETER;
    d->error_fn = fn;
    return HPDF_OK;
}

void HPDF_Free(HPDF_Doc pdf)
{
    hpdf_doc_rec *d = (hpdf_doc_rec *)pdf;
    if (!d)
        return;
    hpdf_doc_free_content(d);
    free(d);
}

HPDF_MMgr HPDF_GetDocMMgr(HPDF_Doc pdf)
{
    HPDF_UNUSED(pdf);
    return NULL;
}

HPDF_STATUS HPDF_NewDoc(HPDF_Doc pdf)
{
    hpdf_doc_rec *d = (hpdf_doc_rec *)pdf;
    pdf_status rc;
    if (!d)
        return HPDF_INVALID_PARAMETER;
    hpdf_doc_free_content(d);
    rc = hpdf_new_core_doc(d);
    if (rc != PDF_OK)
        return hpdf_fail_pdf(d, rc);
    d->layout = HPDF_PAGE_LAYOUT_SINGLE;
    d->page_mode = HPDF_PAGE_MODE_USE_NONE;
    d->compression = HPDF_COMP_NONE;
    d->encrypt_applied = 0;
    pdf_doc_set_compression(d->core, PDF_COMPRESS_NONE);
    return HPDF_OK;
}

void HPDF_FreeDoc(HPDF_Doc pdf)
{
    hpdf_doc_rec *d = (hpdf_doc_rec *)pdf;
    if (d)
        hpdf_doc_free_content(d);
}

HPDF_BOOL HPDF_HasDoc(HPDF_Doc pdf)
{
    return hpdf_doc_ok((hpdf_doc_rec *)pdf) ? HPDF_TRUE : HPDF_FALSE;
}

void HPDF_FreeDocAll(HPDF_Doc pdf)
{
    HPDF_FreeDoc(pdf);
}

HPDF_STATUS HPDF_GetError(HPDF_Doc pdf)
{
    hpdf_doc_rec *d = (hpdf_doc_rec *)pdf;
    return d ? d->error_no : HPDF_INVALID_PARAMETER;
}

HPDF_STATUS HPDF_GetErrorDetail(HPDF_Doc pdf)
{
    hpdf_doc_rec *d = (hpdf_doc_rec *)pdf;
    return d ? d->detail_no : 0;
}

void HPDF_ResetError(HPDF_Doc pdf)
{
    hpdf_doc_rec *d = (hpdf_doc_rec *)pdf;
    if (d)
    {
        d->error_no = HPDF_OK;
        d->detail_no = 0;
    }
}

HPDF_STATUS HPDF_CheckError(HPDF_Error error)
{
    hpdf_doc_rec *d = (hpdf_doc_rec *)error;
    return d ? d->error_no : HPDF_OK;
}

HPDF_STATUS HPDF_SetPagesConfiguration(HPDF_Doc pdf, HPDF_UINT page_per_pages)
{
    HPDF_UNUSED(pdf);
    HPDF_UNUSED(page_per_pages);
    return HPDF_OK;
}

/* ------------------------------------------------------------------ */
/* Output                                                              */
/* ------------------------------------------------------------------ */

static HPDF_STATUS hpdf_apply_encryption(hpdf_doc_rec *d)
{
    pdf_encrypt_mode mode = PDF_ENCRYPT_RC4_40;
    unsigned perms = 0;

    if (!d || !d->core)
        return HPDF_INVALID_DOCUMENT;
    if (d->encrypt_applied)
        return HPDF_OK;
    if (!d->have_password && d->encryption_mode == 0
        && d->permissions == 0)
        return HPDF_OK;

    if (d->encryption_mode == HPDF_ENCRYPT_R3)
        mode = PDF_ENCRYPT_RC4_128;
    else
        mode = PDF_ENCRYPT_RC4_40;

    if (d->permissions & HPDF_ENABLE_PRINT)    perms |= PDF_PERM_PRINT;
    if (d->permissions & HPDF_ENABLE_EDIT)     perms |= PDF_PERM_EDIT;
    if (d->permissions & HPDF_ENABLE_COPY)     perms |= PDF_PERM_COPY;
    if (d->permissions & HPDF_ENABLE_EDIT_ALL) perms |= PDF_PERM_ANNOTATE;

    {
        pdf_status rc = pdf_doc_set_encryption(
            d->core, mode, d->owner_pass, d->user_pass, perms);
        d->encrypt_applied = 1;
        if (rc != PDF_OK)
            return hpdf_fail_pdf(d, rc);
    }
    return HPDF_OK;
}

HPDF_STATUS HPDF_SaveToFile(HPDF_Doc pdf, const char *file_name)
{
    hpdf_doc_rec *d = (hpdf_doc_rec *)pdf;
    pdf_status rc;
    if (!hpdf_doc_ok(d))
        return HPDF_INVALID_DOCUMENT;
    if (!file_name || !*file_name)
        return hpdf_fail(d, HPDF_INVALID_PARAMETER);
    {
        HPDF_STATUS st = hpdf_apply_encryption(d);
        if (st != HPDF_OK)
            return st;
    }
    rc = pdf_doc_save_to_file(d->core, file_name);
    if (rc != PDF_OK)
        return hpdf_fail_pdf(d, rc);
    return HPDF_OK;
}

HPDF_STATUS HPDF_SaveToStream(HPDF_Doc pdf)
{
    hpdf_doc_rec *d = (hpdf_doc_rec *)pdf;
    pdf_status rc;
    size_t size = 0;

    if (!hpdf_doc_ok(d))
        return HPDF_INVALID_DOCUMENT;
    {
        HPDF_STATUS st = hpdf_apply_encryption(d);
        if (st != HPDF_OK)
            return st;
    }

    free(d->stream);
    d->stream = NULL;
    d->stream_size = 0;
    d->stream_pos = 0;

    rc = pdf_doc_save_to_memory(d->core, NULL, &size);
    if (rc != PDF_OK)
        return hpdf_fail_pdf(d, rc);
    d->stream = (unsigned char *)malloc(size ? size : 1);
    if (!d->stream)
        return hpdf_fail(d, HPDF_FAILD_TO_ALLOC_MEM);
    {
        size_t want = size;
        rc = pdf_doc_save_to_memory(d->core, d->stream, &want);
        if (rc != PDF_OK)
        {
            free(d->stream);
            d->stream = NULL;
            return hpdf_fail_pdf(d, rc);
        }
        d->stream_size = want;
    }
    return HPDF_OK;
}

HPDF_UINT32 HPDF_GetStreamSize(HPDF_Doc pdf)
{
    hpdf_doc_rec *d = (hpdf_doc_rec *)pdf;
    return d ? (HPDF_UINT32)d->stream_size : 0;
}

HPDF_STATUS HPDF_GetContents(HPDF_Doc pdf, HPDF_BYTE *buf, HPDF_UINT32 *size)
{
    hpdf_doc_rec *d = (hpdf_doc_rec *)pdf;
    size_t n;
    if (!hpdf_doc_ok(d))
        return HPDF_INVALID_DOCUMENT;
    if (!buf || !size)
        return hpdf_fail(d, HPDF_INVALID_PARAMETER);
    n = *size;
    if (n > d->stream_size)
        n = d->stream_size;
    if (n)
        memcpy(buf, d->stream, n);
    *size = (HPDF_UINT32)n;
    return HPDF_OK;
}

HPDF_STATUS HPDF_ReadFromStream(HPDF_Doc pdf, HPDF_BYTE *buf,
                                HPDF_UINT32 *size)
{
    hpdf_doc_rec *d = (hpdf_doc_rec *)pdf;
    size_t n;
    if (!hpdf_doc_ok(d))
        return HPDF_INVALID_DOCUMENT;
    if (!buf || !size)
        return hpdf_fail(d, HPDF_INVALID_PARAMETER);
    if (d->stream_pos >= d->stream_size)
    {
        *size = 0;
        return HPDF_STREAM_EOF;
    }
    n = *size;
    if (n > d->stream_size - d->stream_pos)
        n = d->stream_size - d->stream_pos;
    memcpy(buf, d->stream + d->stream_pos, n);
    d->stream_pos += n;
    *size = (HPDF_UINT32)n;
    return HPDF_OK;
}

HPDF_STATUS HPDF_ResetStream(HPDF_Doc pdf)
{
    hpdf_doc_rec *d = (hpdf_doc_rec *)pdf;
    if (!d)
        return HPDF_INVALID_PARAMETER;
    d->stream_pos = 0;
    return HPDF_OK;
}

/* ------------------------------------------------------------------ */
/* Pages                                                               */
/* ------------------------------------------------------------------ */

static hpdf_page_rec *hpdf_page_new(hpdf_doc_rec *d, pdf_page core,
                                    double w, double h)
{
    hpdf_page_rec *p = (hpdf_page_rec *)calloc(1, sizeof(*p));
    if (!p)
        return NULL;
    p->core = core;
    p->doc = d;
    p->width = w;
    p->height = h;
    p->line_width = 1.0;
    p->line_cap = PDF_LINE_CAP_BUTT;
    p->line_join = PDF_LINE_JOIN_MITER;
    p->miter_limit = 10.0;
    p->render_mode = PDF_TEXT_RENDER_FILL;
    p->hscale = 100.0;
    p->font_size = 10.0;
    p->fill.kind = PDF_COLOR_GRAY;
    p->fill.c1 = 0.0;
    p->fill.alpha = 1.0;
    p->stroke = p->fill;
    p->fill_alpha = 1.0;
    p->stroke_alpha = 1.0;
    p->blend_mode = PDF_BLEND_NORMAL;
    p->tm_a = 1.0;
    p->tm_d = 1.0;
    if (d->pages_tail)
        d->pages_tail->next = p;
    else
        d->pages = p;
    d->pages_tail = p;
    d->current_page = (HPDF_Page)p;
    return p;
}

HPDF_Page HPDF_AddPage(HPDF_Doc pdf)
{
    hpdf_doc_rec *d = (hpdf_doc_rec *)pdf;
    pdf_page core = NULL;
    pdf_status rc;
    hpdf_page_rec *p;

    if (!hpdf_doc_ok(d))
    {
        hpdf_fail(d, HPDF_INVALID_DOCUMENT);
        return NULL;
    }
    rc = pdf_page_add(d->core, PDF_PAGE_A4, PDF_ORIENTATION_PORTRAIT, &core);
    if (rc != PDF_OK)
    {
        hpdf_fail_pdf(d, rc);
        return NULL;
    }
    p = hpdf_page_new(d, core, 595.276, 841.89);
    if (!p)
    {
        hpdf_fail(d, HPDF_FAILD_TO_ALLOC_MEM);
        return NULL;
    }
    return (HPDF_Page)p;
}

HPDF_Page HPDF_InsertPage(HPDF_Doc pdf, HPDF_Page page)
{
    HPDF_UNUSED(page);
    hpdf_fail((hpdf_doc_rec *)pdf, HPDF_UNSUPPORTED_FUNC);
    return NULL;
}

HPDF_Page HPDF_GetCurrentPage(HPDF_Doc pdf)
{
    hpdf_doc_rec *d = (hpdf_doc_rec *)pdf;
    return d ? d->current_page : NULL;
}

HPDF_Page HPDF_GetPageByIndex(HPDF_Doc pdf, HPDF_UINT index)
{
    hpdf_doc_rec *d = (hpdf_doc_rec *)pdf;
    hpdf_page_rec *p;
    HPDF_UINT i = 0;
    if (!d)
        return NULL;
    for (p = d->pages; p; p = p->next)
    {
        if (i == index)
            return (HPDF_Page)p;
        ++i;
    }
    hpdf_fail(d, HPDF_INVALID_PAGE_INDEX);
    return NULL;
}

static HPDF_STATUS hpdf_page_set_wh(HPDF_Page page, double w, double h)
{
    hpdf_page_rec *p = (hpdf_page_rec *)page;
    pdf_status rc;
    if (!p)
        return HPDF_INVALID_PAGE;
    if (w <= 0 || h <= 0)
        return hpdf_fail(p->doc, HPDF_INVALID_PARAMETER);
    rc = pdf_page_set_size(p->core, w, h);
    if (rc != PDF_OK)
        return hpdf_fail_pdf(p->doc, rc);
    p->width = w;
    p->height = h;
    return HPDF_OK;
}

HPDF_STATUS HPDF_Page_SetWidth(HPDF_Page page, HPDF_REAL value)
{
    hpdf_page_rec *p = (hpdf_page_rec *)page;
    if (!p)
        return HPDF_INVALID_PAGE;
    return hpdf_page_set_wh(page, (double)value, p->height);
}

HPDF_STATUS HPDF_Page_SetHeight(HPDF_Page page, HPDF_REAL value)
{
    hpdf_page_rec *p = (hpdf_page_rec *)page;
    if (!p)
        return HPDF_INVALID_PAGE;
    return hpdf_page_set_wh(page, p->width, (double)value);
}

HPDF_STATUS HPDF_Page_SetBoundary(HPDF_Page page, HPDF_PageBoundary boundary,
                                  HPDF_REAL left, HPDF_REAL bottom,
                                  HPDF_REAL right, HPDF_REAL top)
{
    hpdf_page_rec *p = (hpdf_page_rec *)page;
    if (!p)
        return HPDF_INVALID_PAGE;
    if (right <= left || top <= bottom)
        return hpdf_fail(p->doc, HPDF_INVALID_PARAMETER);
    if (boundary == HPDF_PAGE_MEDIABOX)
        return hpdf_page_set_wh(page, (double)(right - left),
                                (double)(top - bottom));
    return HPDF_OK; /* crop/bleed/trim/art boxes are not modelled */
}

HPDF_STATUS HPDF_Page_SetSize(HPDF_Page page, HPDF_PageSizes size,
                              HPDF_PageDirection direction)
{
    static const double sizes[][2] = {
        { 612.0, 792.0 },    /* LETTER */
        { 612.0, 1008.0 },   /* LEGAL */
        { 841.89, 1190.551 },/* A3 */
        { 595.276, 841.89 }, /* A4 */
        { 419.528, 595.276 },/* A5 */
        { 708.661, 1000.63 },/* B4 */
        { 498.898, 708.661 },/* B5 */
        { 522.0, 756.0 },    /* EXECUTIVE */
        { 288.0, 432.0 },    /* US4x6 */
        { 288.0, 576.0 },    /* US4x8 */
        { 360.0, 504.0 },    /* US5x7 */
        { 297.0, 684.0 }     /* COMM10 */
    };
    double w, h;
    if (!page)
        return HPDF_INVALID_PAGE;
    if ((int)size < 0 || (int)size >= (int)(sizeof(sizes) / sizeof(sizes[0])))
        return hpdf_fail(hpdf_doc_of_page(page), HPDF_PAGE_INVALID_SIZE);
    if (direction != HPDF_PAGE_PORTRAIT && direction != HPDF_PAGE_LANDSCAPE)
        return hpdf_fail(hpdf_doc_of_page(page), HPDF_PAGE_INVALID_DIRECTION);
    w = sizes[(int)size][0];
    h = sizes[(int)size][1];
    if (direction == HPDF_PAGE_LANDSCAPE)
    {
        double t = w;
        w = h;
        h = t;
    }
    return hpdf_page_set_wh(page, w, h);
}

HPDF_STATUS HPDF_Page_SetRotate(HPDF_Page page, HPDF_UINT16 angle)
{
    hpdf_page_rec *p = (hpdf_page_rec *)page;
    pdf_status rc;
    if (!p)
        return HPDF_INVALID_PAGE;
    rc = pdf_page_set_rotation(p->core, (int)angle);
    return rc == PDF_OK ? HPDF_OK : hpdf_fail_pdf(p->doc, rc);
}

HPDF_STATUS HPDF_Page_SetZoom(HPDF_Page page, HPDF_REAL zoom)
{
    HPDF_UNUSED(page);
    HPDF_UNUSED(zoom);
    return HPDF_OK;
}

HPDF_REAL HPDF_Page_GetWidth(HPDF_Page page)
{
    hpdf_page_rec *p = (hpdf_page_rec *)page;
    return p ? (HPDF_REAL)p->width : 0;
}

HPDF_REAL HPDF_Page_GetHeight(HPDF_Page page)
{
    hpdf_page_rec *p = (hpdf_page_rec *)page;
    return p ? (HPDF_REAL)p->height : 0;
}

HPDF_MMgr HPDF_GetPageMMgr(HPDF_Page page)
{
    HPDF_UNUSED(page);
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Page state accessors                                                */
/* ------------------------------------------------------------------ */

HPDF_UINT16 HPDF_Page_GetGMode(HPDF_Page page)
{
    hpdf_page_rec *p = (hpdf_page_rec *)page;
    if (!p)
        return 0;
    return p->in_text ? HPDF_GMODE_TEXT_OBJECT : HPDF_GMODE_PAGE_DESCRIPTION;
}

HPDF_Point HPDF_Page_GetCurrentPos(HPDF_Page page)
{
    hpdf_page_rec *p = (hpdf_page_rec *)page;
    HPDF_Point pos;
    if (p)
        return p->cur_pos;
    pos.x = 0;
    pos.y = 0;
    return pos;
}

HPDF_STATUS HPDF_Page_GetCurrentPos2(HPDF_Page page, HPDF_Point *pos)
{
    if (!pos)
        return HPDF_INVALID_PARAMETER;
    *pos = HPDF_Page_GetCurrentPos(page);
    return HPDF_OK;
}

HPDF_Point HPDF_Page_GetCurrentTextPos(HPDF_Page page)
{
    hpdf_page_rec *p = (hpdf_page_rec *)page;
    HPDF_Point pos;
    if (p)
        return p->text_pos;
    pos.x = 0;
    pos.y = 0;
    return pos;
}

HPDF_STATUS HPDF_Page_GetCurrentTextPos2(HPDF_Page page, HPDF_Point *pos)
{
    if (!pos)
        return HPDF_INVALID_PARAMETER;
    *pos = HPDF_Page_GetCurrentTextPos(page);
    return HPDF_OK;
}

HPDF_Font HPDF_Page_GetCurrentFont(HPDF_Page page)
{
    hpdf_page_rec *p = (hpdf_page_rec *)page;
    return p ? (HPDF_Font)p->font : NULL;
}

HPDF_REAL HPDF_Page_GetCurrentFontSize(HPDF_Page page)
{
    hpdf_page_rec *p = (hpdf_page_rec *)page;
    return p ? (HPDF_REAL)p->font_size : 0;
}

HPDF_TransMatrix HPDF_Page_GetTransMatrix(HPDF_Page page)
{
    HPDF_TransMatrix m;
    HPDF_UNUSED(page);
    m.a = 1; m.b = 0; m.c = 0; m.d = 1; m.x = 0; m.y = 0;
    return m;
}

HPDF_TransMatrix HPDF_Page_GetTextMatrix(HPDF_Page page)
{
    hpdf_page_rec *p = (hpdf_page_rec *)page;
    HPDF_TransMatrix m;
    if (!p)
    {
        m.a = 1; m.b = 0; m.c = 0; m.d = 1; m.x = 0; m.y = 0;
        return m;
    }
    m.a = (HPDF_REAL)p->tm_a;
    m.b = (HPDF_REAL)p->tm_b;
    m.c = (HPDF_REAL)p->tm_c;
    m.d = (HPDF_REAL)p->tm_d;
    m.x = (HPDF_REAL)p->text_pos.x;
    m.y = (HPDF_REAL)p->text_pos.y;
    return m;
}

HPDF_UINT HPDF_Page_GetGStateDepth(HPDF_Page page)
{
    hpdf_page_rec *p = (hpdf_page_rec *)page;
    return p ? (HPDF_UINT)p->depth : 0;
}

HPDF_REAL HPDF_Page_GetLineWidth(HPDF_Page page)
{
    hpdf_page_rec *p = (hpdf_page_rec *)page;
    return p ? (HPDF_REAL)p->line_width : 0;
}

HPDF_LineCap HPDF_Page_GetLineCap(HPDF_Page page)
{
    hpdf_page_rec *p = (hpdf_page_rec *)page;
    return p ? (HPDF_LineCap)p->line_cap : HPDF_BUTT_END;
}

HPDF_LineJoin HPDF_Page_GetLineJoin(HPDF_Page page)
{
    hpdf_page_rec *p = (hpdf_page_rec *)page;
    return p ? (HPDF_LineJoin)p->line_join : HPDF_MITER_JOIN;
}

HPDF_REAL HPDF_Page_GetMiterLimit(HPDF_Page page)
{
    hpdf_page_rec *p = (hpdf_page_rec *)page;
    return p ? (HPDF_REAL)p->miter_limit : 0;
}

HPDF_DashMode HPDF_Page_GetDash(HPDF_Page page)
{
    hpdf_page_rec *p = (hpdf_page_rec *)page;
    HPDF_DashMode d;
    size_t i;
    memset(&d, 0, sizeof(d));
    if (!p)
        return d;
    d.num_ptn = (HPDF_UINT)p->dash_n;
    d.phase = (HPDF_REAL)p->dash_phase;
    for (i = 0; i < p->dash_n && i < 8; ++i)
        d.ptn[i] = (HPDF_REAL)p->dash[i];
    return d;
}

HPDF_REAL HPDF_Page_GetFlat(HPDF_Page page)
{
    HPDF_UNUSED(page);
    return 1;
}

HPDF_REAL HPDF_Page_GetCharSpace(HPDF_Page page)
{
    hpdf_page_rec *p = (hpdf_page_rec *)page;
    return p ? (HPDF_REAL)p->char_space : 0;
}

HPDF_REAL HPDF_Page_GetWordSpace(HPDF_Page page)
{
    hpdf_page_rec *p = (hpdf_page_rec *)page;
    return p ? (HPDF_REAL)p->word_space : 0;
}

HPDF_REAL HPDF_Page_GetHorizontalScalling(HPDF_Page page)
{
    hpdf_page_rec *p = (hpdf_page_rec *)page;
    return p ? (HPDF_REAL)p->hscale : 100;
}

HPDF_REAL HPDF_Page_GetTextLeading(HPDF_Page page)
{
    hpdf_page_rec *p = (hpdf_page_rec *)page;
    return p ? (HPDF_REAL)p->leading : 0;
}

HPDF_TextRenderingMode HPDF_Page_GetTextRenderingMode(HPDF_Page page)
{
    hpdf_page_rec *p = (hpdf_page_rec *)page;
    return p ? (HPDF_TextRenderingMode)p->render_mode : HPDF_FILL;
}

HPDF_REAL HPDF_Page_GetTextRaise(HPDF_Page page)
{
    hpdf_page_rec *p = (hpdf_page_rec *)page;
    return p ? (HPDF_REAL)p->rise : 0;
}

HPDF_REAL HPDF_Page_GetTextRise(HPDF_Page page)
{
    return HPDF_Page_GetTextRaise(page);
}

static HPDF_RGBColor hpdf_to_rgb(const pdf_color *c)
{
    HPDF_RGBColor rgb;
    switch (c->kind)
    {
        case PDF_COLOR_GRAY:
            rgb.r = rgb.g = rgb.b = (HPDF_REAL)c->c1;
            break;
        case PDF_COLOR_CMYK:
            rgb.r = (HPDF_REAL)((1 - c->c1) * (1 - c->c4));
            rgb.g = (HPDF_REAL)((1 - c->c2) * (1 - c->c4));
            rgb.b = (HPDF_REAL)((1 - c->c3) * (1 - c->c4));
            break;
        default:
            rgb.r = (HPDF_REAL)c->c1;
            rgb.g = (HPDF_REAL)c->c2;
            rgb.b = (HPDF_REAL)c->c3;
            break;
    }
    return rgb;
}

HPDF_RGBColor HPDF_Page_GetRGBFill(HPDF_Page page)
{
    hpdf_page_rec *p = (hpdf_page_rec *)page;
    HPDF_RGBColor rgb;
    if (p)
        return hpdf_to_rgb(&p->fill);
    rgb.r = rgb.g = rgb.b = 0;
    return rgb;
}

HPDF_RGBColor HPDF_Page_GetRGBStroke(HPDF_Page page)
{
    hpdf_page_rec *p = (hpdf_page_rec *)page;
    HPDF_RGBColor rgb;
    if (p)
        return hpdf_to_rgb(&p->stroke);
    rgb.r = rgb.g = rgb.b = 0;
    return rgb;
}

HPDF_CMYKColor HPDF_Page_GetCMYKFill(HPDF_Page page)
{
    hpdf_page_rec *p = (hpdf_page_rec *)page;
    HPDF_CMYKColor c = { 0, 0, 0, 0 };
    if (p && p->fill.kind == PDF_COLOR_CMYK)
    {
        c.c = (HPDF_REAL)p->fill.c1;
        c.m = (HPDF_REAL)p->fill.c2;
        c.y = (HPDF_REAL)p->fill.c3;
        c.k = (HPDF_REAL)p->fill.c4;
    }
    return c;
}

HPDF_CMYKColor HPDF_Page_GetCMYKStroke(HPDF_Page page)
{
    hpdf_page_rec *p = (hpdf_page_rec *)page;
    HPDF_CMYKColor c = { 0, 0, 0, 0 };
    if (p && p->stroke.kind == PDF_COLOR_CMYK)
    {
        c.c = (HPDF_REAL)p->stroke.c1;
        c.m = (HPDF_REAL)p->stroke.c2;
        c.y = (HPDF_REAL)p->stroke.c3;
        c.k = (HPDF_REAL)p->stroke.c4;
    }
    return c;
}

HPDF_REAL HPDF_Page_GetGrayFill(HPDF_Page page)
{
    hpdf_page_rec *p = (hpdf_page_rec *)page;
    return (p && p->fill.kind == PDF_COLOR_GRAY) ? (HPDF_REAL)p->fill.c1 : 0;
}

HPDF_REAL HPDF_Page_GetGrayStroke(HPDF_Page page)
{
    hpdf_page_rec *p = (hpdf_page_rec *)page;
    return (p && p->stroke.kind == PDF_COLOR_GRAY) ? (HPDF_REAL)p->stroke.c1 : 0;
}

HPDF_ColorSpace HPDF_Page_GetStrokingColorSpace(HPDF_Page page)
{
    hpdf_page_rec *p = (hpdf_page_rec *)page;
    if (!p) return HPDF_CS_DEVICE_GRAY;
    if (p->stroke.kind == PDF_COLOR_CMYK) return HPDF_CS_DEVICE_CMYK;
    if (p->stroke.kind == PDF_COLOR_GRAY) return HPDF_CS_DEVICE_GRAY;
    return HPDF_CS_DEVICE_RGB;
}

HPDF_ColorSpace HPDF_Page_GetFillingColorSpace(HPDF_Page page)
{
    hpdf_page_rec *p = (hpdf_page_rec *)page;
    if (!p) return HPDF_CS_DEVICE_GRAY;
    if (p->fill.kind == PDF_COLOR_CMYK) return HPDF_CS_DEVICE_CMYK;
    if (p->fill.kind == PDF_COLOR_GRAY) return HPDF_CS_DEVICE_GRAY;
    return HPDF_CS_DEVICE_RGB;
}

/* ------------------------------------------------------------------ */
/* Fonts (the wrappers are needed by the text code below)              */
/* ------------------------------------------------------------------ */

static hpdf_font_rec *hpdf_find_font(hpdf_doc_rec *d, const char *name)
{
    hpdf_font_rec *f;
    for (f = d ? d->fonts : NULL; f; f = f->next)
        if (hpdf_ci_equal(f->name, name))
            return f;
    return NULL;
}

static hpdf_font_rec *hpdf_add_font(hpdf_doc_rec *d, pdf_font core,
                                    const char *name, const char *encoding,
                                    int is_standard)
{
    hpdf_font_rec *f = (hpdf_font_rec *)calloc(1, sizeof(*f));
    if (!f)
        return NULL;
    f->core = core;
    f->is_standard = is_standard;
    snprintf(f->name, sizeof(f->name), "%s", name ? name : "");
    snprintf(f->encoding, sizeof(f->encoding), "%s",
             encoding ? encoding : (is_standard ? "WinAnsiEncoding" : "UTF-8"));
    f->next = d->fonts;
    d->fonts = f;
    return f;
}

static int hpdf_std_font_from_name(const char *name, pdf_std_font *out)
{
    static const struct { const char *name; pdf_std_font id; } map[] = {
        { "Times-Roman", PDF_STD_FONT_TIMES_ROMAN },
        { "Times-Italic", PDF_STD_FONT_TIMES_ITALIC },
        { "Times-Bold", PDF_STD_FONT_TIMES_BOLD },
        { "Times-BoldItalic", PDF_STD_FONT_TIMES_BOLD_ITALIC },
        { "Helvetica", PDF_STD_FONT_HELVETICA },
        { "Helvetica-Oblique", PDF_STD_FONT_HELVETICA_OBLIQUE },
        { "Helvetica-Bold", PDF_STD_FONT_HELVETICA_BOLD },
        { "Helvetica-BoldOblique", PDF_STD_FONT_HELVETICA_BOLD_OBLIQUE },
        { "Courier", PDF_STD_FONT_COURIER },
        { "Courier-Oblique", PDF_STD_FONT_COURIER_OBLIQUE },
        { "Courier-Bold", PDF_STD_FONT_COURIER_BOLD },
        { "Courier-BoldOblique", PDF_STD_FONT_COURIER_BOLD_OBLIQUE },
        { "Symbol", PDF_STD_FONT_SYMBOL },
        { "ZapfDingbats", PDF_STD_FONT_ZAPF_DINGBATS }
    };
    size_t i;
    if (!name)
        return 0;
    for (i = 0; i < sizeof(map) / sizeof(map[0]); ++i)
        if (hpdf_ci_equal(name, map[i].name))
        {
            *out = map[i].id;
            return 1;
        }
    return 0;
}

HPDF_Font HPDF_GetFont(HPDF_Doc pdf, const char *font_name,
                       const char *encoding_name)
{
    hpdf_doc_rec *d = (hpdf_doc_rec *)pdf;
    hpdf_font_rec *f;
    pdf_std_font std;
    pdf_font core = NULL;
    pdf_status rc;

    if (!hpdf_doc_ok(d))
    {
        hpdf_fail(d, HPDF_INVALID_DOCUMENT);
        return NULL;
    }
    if (!font_name || !*font_name)
    {
        hpdf_fail(d, HPDF_INVALID_FONT_NAME);
        return NULL;
    }
    f = hpdf_find_font(d, font_name);
    if (f)
        return (HPDF_Font)f;

    if (hpdf_std_font_from_name(font_name, &std))
    {
        rc = pdf_font_get_std(d->core, std, &core);
        if (rc != PDF_OK)
        {
            hpdf_fail_pdf(d, rc);
            return NULL;
        }
        f = hpdf_add_font(d, core, font_name,
                          encoding_name ? encoding_name : "WinAnsiEncoding", 1);
        if (!f)
        {
            pdf_font_free(core);
            hpdf_fail(d, HPDF_FAILD_TO_ALLOC_MEM);
        }
        return (HPDF_Font)f;
    }

    /* Unknown name: try the system font lookup (a pdflib extension; the
     * libharu backend only knows the three common families). */
    rc = pdf_font_find_system(d->core, font_name, 0, 0, &core);
    if (rc != PDF_OK)
    {
        hpdf_fail(d, HPDF_INVALID_FONT_NAME);
        return NULL;
    }
    f = hpdf_add_font(d, core, font_name, encoding_name, 0);
    if (!f)
    {
        pdf_font_free(core);
        hpdf_fail(d, HPDF_FAILD_TO_ALLOC_MEM);
    }
    return (HPDF_Font)f;
}

const char *HPDF_LoadTTFontFromFile(HPDF_Doc pdf, const char *file_name,
                                    HPDF_BOOL embedding)
{
    hpdf_doc_rec *d = (hpdf_doc_rec *)pdf;
    pdf_font core = NULL;
    pdf_status rc;
    hpdf_font_rec *f;
    char name[128];
    const char *base;

    if (!hpdf_doc_ok(d) || !file_name)
    {
        hpdf_fail(d, HPDF_INVALID_PARAMETER);
        return NULL;
    }
    rc = pdf_font_load_ttf(d->core, file_name, embedding ? 1 : 0, &core);
    if (rc != PDF_OK)
    {
        hpdf_fail_pdf(d, rc);
        return NULL;
    }
    base = strrchr(file_name, '\\');
    if (!base) base = strrchr(file_name, '/');
    base = base ? base + 1 : file_name;
    snprintf(name, sizeof(name), "%s", base);
    {
        char *dot = strrchr(name, '.');
        if (dot && dot != name)
            *dot = '\0';
    }

    /* A second load of the same file returns the cached wrapper; the core
     * already has the font object. */
    f = hpdf_find_font(d, name);
    if (f)
    {
        pdf_font_free(core);
        return f->name;
    }
    f = hpdf_add_font(d, core, name, "UTF-8", 0);
    if (!f)
    {
        pdf_font_free(core);
        hpdf_fail(d, HPDF_FAILD_TO_ALLOC_MEM);
        return NULL;
    }
    return f->name;
}

const char *HPDF_LoadTTFontFromFile2(HPDF_Doc pdf, const char *file_name,
                                     HPDF_UINT index, HPDF_BOOL embedding)
{
    if (index != 0)
    {
        hpdf_fail((hpdf_doc_rec *)pdf, HPDF_INVALID_TTC_INDEX);
        return NULL;
    }
    return HPDF_LoadTTFontFromFile(pdf, file_name, embedding);
}

HPDF_FontDef HPDF_GetTTFontDefFromFile(HPDF_Doc pdf, const char *file_name,
                                       HPDF_BOOL embedding)
{
    HPDF_UNUSED(file_name);
    HPDF_UNUSED(embedding);
    hpdf_fail((hpdf_doc_rec *)pdf, HPDF_UNSUPPORTED_FUNC);
    return NULL;
}

const char *HPDF_LoadType1FontFromFile(HPDF_Doc pdf, const char *afm_file_name,
                                       const char *data_file_name)
{
    HPDF_UNUSED(afm_file_name);
    HPDF_UNUSED(data_file_name);
    hpdf_fail((hpdf_doc_rec *)pdf, HPDF_UNSUPPORTED_FUNC);
    return NULL;
}

HPDF_STATUS HPDF_UseJPFonts(HPDF_Doc pdf)   { HPDF_UNUSED(pdf); return HPDF_OK; }
HPDF_STATUS HPDF_UseKRFonts(HPDF_Doc pdf)   { HPDF_UNUSED(pdf); return HPDF_OK; }
HPDF_STATUS HPDF_UseCNSFonts(HPDF_Doc pdf)  { HPDF_UNUSED(pdf); return HPDF_OK; }
HPDF_STATUS HPDF_UseCNTFonts(HPDF_Doc pdf)  { HPDF_UNUSED(pdf); return HPDF_OK; }

/* ------------------------------------------------------------------ */
/* Text                                                                */
/* ------------------------------------------------------------------ */

static double hpdf_font_width_units(hpdf_font_rec *f, const char *text)
{
    double w = 0;
    if (!f || !f->core || !text)
        return 0;
    if (pdf_font_text_width(f->core, text, 1000.0, &w) != PDF_OK)
        return 0;
    return w;
}

static double hpdf_page_text_width(hpdf_page_rec *p, const char *text)
{
    if (!p || !p->font || p->font_size <= 0)
        return 0;
    return hpdf_font_width_units(p->font, text) * p->font_size / 1000.0;
}

HPDF_REAL HPDF_Page_TextWidth(HPDF_Page page, const char *text)
{
    hpdf_page_rec *p = (hpdf_page_rec *)page;
    return (HPDF_REAL)hpdf_page_text_width(p, text);
}

static double hpdf_text_advance(hpdf_page_rec *p, const char *text)
{
    double w = hpdf_page_text_width(p, text);
    size_t n = strlen(text);
    size_t chars = hpdf_utf8_count(text, n);
    size_t spaces = 0, i;
    for (i = 0; i < n; ++i)
        if (text[i] == ' ')
            ++spaces;
    w += p->char_space * (double)chars;
    w += p->word_space * (double)spaces;
    w *= p->hscale / 100.0;
    return w;
}

static HPDF_STATUS hpdf_show_text(hpdf_page_rec *p, const char *text)
{
    pdf_status rc;
    double adv;

    if (!p)
        return HPDF_INVALID_PAGE;
    if (!p->font)
        return hpdf_fail(p->doc, HPDF_PAGE_FONT_NOT_FOUND);
    if (!text || !*text)
        return HPDF_OK;
    if (p->font_size <= 0)
        return hpdf_fail(p->doc, HPDF_PAGE_INVALID_FONT_SIZE);

    {
        int transformed = fabs(p->tm_a - 1.0) > 1e-9
                       || fabs(p->tm_b) > 1e-9
                       || fabs(p->tm_c) > 1e-9
                       || fabs(p->tm_d - 1.0) > 1e-9;
        pdf_set_text_render_mode(p->core, p->render_mode);
        if (transformed)
        {
            pdf_gstate_save(p->core);
            pdf_set_transform(p->core, 1, 0, 0, 1,
                              p->text_pos.x, p->text_pos.y);
            pdf_set_transform(p->core, p->tm_a, p->tm_b, p->tm_c, p->tm_d,
                              0, 0);
            rc = pdf_page_draw_text(p->core, p->font->core, text, 0,
                                    p->rise, p->font_size);
            pdf_gstate_restore(p->core);
        }
        else
        {
            rc = pdf_page_draw_text(p->core, p->font->core, text,
                                    p->text_pos.x, p->text_pos.y + p->rise,
                                    p->font_size);
        }
    }
    if (rc != PDF_OK)
        return hpdf_fail_pdf(p->doc, rc);

    adv = hpdf_text_advance(p, text);
    p->text_pos.x += (HPDF_REAL)(adv * p->tm_a);
    p->text_pos.y += (HPDF_REAL)(adv * p->tm_b);
    return HPDF_OK;
}

HPDF_STATUS HPDF_Page_BeginText(HPDF_Page page)
{
    hpdf_page_rec *p = (hpdf_page_rec *)page;
    if (!p)
        return HPDF_INVALID_PAGE;
    p->in_text = 1;
    p->text_pos.x = 0;
    p->text_pos.y = 0;
    p->tm_a = 1; p->tm_b = 0; p->tm_c = 0; p->tm_d = 1;
    return HPDF_OK;
}

HPDF_STATUS HPDF_Page_EndText(HPDF_Page page)
{
    hpdf_page_rec *p = (hpdf_page_rec *)page;
    if (!p)
        return HPDF_INVALID_PAGE;
    p->in_text = 0;
    return HPDF_OK;
}

HPDF_STATUS HPDF_Page_SetFontAndSize(HPDF_Page page, HPDF_Font font,
                                     HPDF_REAL size)
{
    hpdf_page_rec *p = (hpdf_page_rec *)page;
    hpdf_font_rec *f = (hpdf_font_rec *)font;
    if (!p || !f || !f->core)
        return hpdf_fail(p ? p->doc : NULL, HPDF_PAGE_INVALID_FONT);
    if (size <= 0)
        return hpdf_fail(p->doc, HPDF_PAGE_INVALID_FONT_SIZE);
    p->font = f;
    p->font_size = (double)size;
    return HPDF_OK;
}

HPDF_STATUS HPDF_Page_MoveTextPos(HPDF_Page page, HPDF_REAL x, HPDF_REAL y)
{
    hpdf_page_rec *p = (hpdf_page_rec *)page;
    if (!p)
        return HPDF_INVALID_PAGE;
    p->text_pos.x += (HPDF_REAL)(x * p->tm_a + y * p->tm_c);
    p->text_pos.y += (HPDF_REAL)(y * p->tm_d + x * p->tm_b);
    return HPDF_OK;
}

HPDF_STATUS HPDF_Page_MoveTextPos2(HPDF_Page page, HPDF_REAL x, HPDF_REAL y)
{
    hpdf_page_rec *p = (hpdf_page_rec *)page;
    HPDF_STATUS st = HPDF_Page_MoveTextPos(page, x, y);
    if (st == HPDF_OK)
        p->leading = -(double)y;
    return st;
}

HPDF_STATUS HPDF_Page_SetTextMatrix(HPDF_Page page, HPDF_REAL a, HPDF_REAL b,
                                    HPDF_REAL c, HPDF_REAL d, HPDF_REAL x,
                                    HPDF_REAL y)
{
    hpdf_page_rec *p = (hpdf_page_rec *)page;
    if (!p)
        return HPDF_INVALID_PAGE;
    if ((a == 0 || d == 0) && (b == 0 || c == 0))
        return hpdf_fail(p->doc, HPDF_INVALID_PARAMETER);
    p->tm_a = (double)a;
    p->tm_b = (double)b;
    p->tm_c = (double)c;
    p->tm_d = (double)d;
    p->text_pos.x = x;
    p->text_pos.y = y;
    return HPDF_OK;
}

HPDF_STATUS HPDF_Page_MoveToNextLine(HPDF_Page page)
{
    hpdf_page_rec *p = (hpdf_page_rec *)page;
    if (!p)
        return HPDF_INVALID_PAGE;
    p->text_pos.x -= (HPDF_REAL)(p->leading * p->tm_c);
    p->text_pos.y -= (HPDF_REAL)(p->leading * p->tm_d);
    return HPDF_OK;
}

HPDF_STATUS HPDF_Page_ShowText(HPDF_Page page, const char *text)
{
    return hpdf_show_text((hpdf_page_rec *)page, text);
}

HPDF_STATUS HPDF_Page_ShowTextNextLine(HPDF_Page page, const char *text)
{
    HPDF_STATUS st = HPDF_Page_MoveToNextLine(page);
    if (st != HPDF_OK)
        return st;
    return hpdf_show_text((hpdf_page_rec *)page, text);
}

HPDF_STATUS HPDF_Page_ShowTextNextLineEx(HPDF_Page page, HPDF_REAL word_space,
                                         HPDF_REAL char_space,
                                         const char *text)
{
    hpdf_page_rec *p = (hpdf_page_rec *)page;
    HPDF_STATUS st;
    if (!p)
        return HPDF_INVALID_PAGE;
    p->word_space = (double)word_space;
    p->char_space = (double)char_space;
    st = HPDF_Page_MoveToNextLine(page);
    if (st != HPDF_OK)
        return st;
    return hpdf_show_text(p, text);
}

HPDF_STATUS HPDF_Page_TextOut(HPDF_Page page, HPDF_REAL xpos, HPDF_REAL ypos,
                              const char *text)
{
    hpdf_page_rec *p = (hpdf_page_rec *)page;
    if (!p)
        return HPDF_INVALID_PAGE;
    p->text_pos.x = xpos;
    p->text_pos.y = ypos;
    return hpdf_show_text(p, text);
}

HPDF_UINT hpdf_measure_text_impl(hpdf_font_rec *font, const char *text,
                                 size_t len, double width, double size,
                                 double char_space, double word_space,
                                 int wordwrap, HPDF_REAL *real_width)
{
    const char *p = text;
    const char *end = text + len;
    const char *last_break = NULL;
    size_t consumed = 0;
    double total = 0;

    if (!font || !text || width <= 0 || size <= 0)
    {
        if (real_width) *real_width = 0;
        return 0;
    }

    while (p < end)
    {
        const char *next = hpdf_utf8_next(p);
        size_t n = (size_t)(next - p);
        char tmp[8];
        double cw, adv;
        size_t spaces;

        if (n > sizeof(tmp) - 1)
            n = sizeof(tmp) - 1;
        memcpy(tmp, p, n);
        tmp[n] = '\0';
        cw = hpdf_font_width_units(font, tmp) * size / 1000.0;
        spaces = hpdf_is_space(p, (size_t)(end - p)) ? 1 : 0;
        adv = cw + char_space + (spaces ? word_space : 0);

        if (total + adv > width)
        {
            if (wordwrap && last_break)
                consumed = (size_t)(last_break - text);
            break;
        }
        total += adv;
        p = next;
        if (p > end)
            p = end;
        consumed = (size_t)(p - text);
        if (spaces)
            last_break = p;
    }

    if (real_width)
        *real_width = (HPDF_REAL)total;
    return (HPDF_UINT)consumed;
}

HPDF_UINT HPDF_Page_MeasureText(HPDF_Page page, const char *text,
                                HPDF_REAL width, HPDF_BOOL wordwrap,
                                HPDF_REAL *real_width)
{
    hpdf_page_rec *p = (hpdf_page_rec *)page;
    if (!p || !p->font)
        return 0;
    return hpdf_measure_text_impl(p->font, text, strlen(text), (double)width,
                                  p->font_size, p->char_space, p->word_space,
                                  wordwrap != 0, real_width);
}

HPDF_STATUS HPDF_Page_TextRect(HPDF_Page page, HPDF_REAL left, HPDF_REAL top,
                               HPDF_REAL right, HPDF_REAL bottom,
                               const char *text, HPDF_TextAlignment align,
                               HPDF_UINT *len)
{
    hpdf_page_rec *p = (hpdf_page_rec *)page;
    pdf_status rc;
    pdf_align a = PDF_ALIGN_LEFT;
    if (!p)
        return HPDF_INVALID_PAGE;
    if (!p->font)
        return hpdf_fail(p->doc, HPDF_PAGE_FONT_NOT_FOUND);
    if (!text)
        return hpdf_fail(p->doc, HPDF_INVALID_PARAMETER);
    if (right <= left || top <= bottom)
        return hpdf_fail(p->doc, HPDF_INVALID_PARAMETER);
    if (len)
        *len = (HPDF_UINT)strlen(text);
    if (align == HPDF_TALIGN_RIGHT) a = PDF_ALIGN_RIGHT;
    else if (align == HPDF_TALIGN_CENTER) a = PDF_ALIGN_CENTER;

    rc = pdf_page_draw_text_wrapped(p->core, p->font->core, text,
                                    (double)left, (double)top,
                                    (double)(right - left),
                                    (double)(top - bottom), p->font_size,
                                    0.0, a);
    return rc == PDF_OK ? HPDF_OK : hpdf_fail_pdf(p->doc, rc);
}

HPDF_STATUS HPDF_Page_SetCharSpace(HPDF_Page page, HPDF_REAL value)
{
    hpdf_page_rec *p = (hpdf_page_rec *)page;
    if (!p) return HPDF_INVALID_PAGE;
    p->char_space = (double)value;
    return HPDF_OK;
}

HPDF_STATUS HPDF_Page_SetWordSpace(HPDF_Page page, HPDF_REAL value)
{
    hpdf_page_rec *p = (hpdf_page_rec *)page;
    if (!p) return HPDF_INVALID_PAGE;
    p->word_space = (double)value;
    return HPDF_OK;
}

HPDF_STATUS HPDF_Page_SetHorizontalScalling(HPDF_Page page, HPDF_REAL value)
{
    hpdf_page_rec *p = (hpdf_page_rec *)page;
    if (!p) return HPDF_INVALID_PAGE;
    p->hscale = (double)value;
    return HPDF_OK;
}

HPDF_STATUS HPDF_Page_SetTextLeading(HPDF_Page page, HPDF_REAL value)
{
    hpdf_page_rec *p = (hpdf_page_rec *)page;
    if (!p) return HPDF_INVALID_PAGE;
    p->leading = (double)value;
    return HPDF_OK;
}

HPDF_STATUS HPDF_Page_SetTextRenderingMode(HPDF_Page page,
                                           HPDF_TextRenderingMode mode)
{
    hpdf_page_rec *p = (hpdf_page_rec *)page;
    if (!p) return HPDF_INVALID_PAGE;
    if ((int)mode < 0 || (int)mode > (int)HPDF_CLIPPING)
        return hpdf_fail(p->doc, HPDF_INVALID_PARAMETER);
    /* pdf_text_render_mode now mirrors PDF 'Tr' 0..7, including the four
     * clipping modes libharu exposes. */
    p->render_mode = (pdf_text_render_mode)mode;
    return HPDF_OK;
}

HPDF_STATUS HPDF_Page_SetTextRise(HPDF_Page page, HPDF_REAL value)
{
    hpdf_page_rec *p = (hpdf_page_rec *)page;
    if (!p) return HPDF_INVALID_PAGE;
    p->rise = (double)value;
    return HPDF_OK;
}

HPDF_STATUS HPDF_Page_SetTextRaise(HPDF_Page page, HPDF_REAL value)
{
    return HPDF_Page_SetTextRise(page, value);
}

/* ------------------------------------------------------------------ */
/* Graphics state                                                      */
/* ------------------------------------------------------------------ */

static void hpdf_save_snap(hpdf_page_rec *p)
{
    hpdf_gstate_snap *s;
    if (p->depth >= HPDF_MAX_GSTATE)
        return;
    s = &p->stack[p->depth++];
    s->render_mode = p->render_mode;
    s->line_width = p->line_width;
    s->line_cap = p->line_cap;
    s->line_join = p->line_join;
    s->miter_limit = p->miter_limit;
    s->char_space = p->char_space;
    s->word_space = p->word_space;
    s->hscale = p->hscale;
    s->leading = p->leading;
    s->rise = p->rise;
    s->fill = p->fill;
    s->stroke = p->stroke;
    memcpy(s->dash, p->dash, sizeof(s->dash));
    s->dash_n = p->dash_n;
    s->dash_phase = p->dash_phase;
}

static void hpdf_restore_snap(hpdf_page_rec *p)
{
    hpdf_gstate_snap *s;
    if (p->depth <= 0)
        return;
    s = &p->stack[--p->depth];
    p->render_mode = s->render_mode;
    p->line_width = s->line_width;
    p->line_cap = s->line_cap;
    p->line_join = s->line_join;
    p->miter_limit = s->miter_limit;
    p->char_space = s->char_space;
    p->word_space = s->word_space;
    p->hscale = s->hscale;
    p->leading = s->leading;
    p->rise = s->rise;
    p->fill = s->fill;
    p->stroke = s->stroke;
    memcpy(p->dash, s->dash, sizeof(p->dash));
    p->dash_n = s->dash_n;
    p->dash_phase = s->dash_phase;
}

HPDF_STATUS HPDF_Page_GSave(HPDF_Page page)
{
    hpdf_page_rec *p = (hpdf_page_rec *)page;
    pdf_status rc;
    if (!p) return HPDF_INVALID_PAGE;
    rc = pdf_gstate_save(p->core);
    if (rc != PDF_OK) return hpdf_fail_pdf(p->doc, rc);
    hpdf_save_snap(p);
    return HPDF_OK;
}

HPDF_STATUS HPDF_Page_GRestore(HPDF_Page page)
{
    hpdf_page_rec *p = (hpdf_page_rec *)page;
    pdf_status rc;
    if (!p) return HPDF_INVALID_PAGE;
    rc = pdf_gstate_restore(p->core);
    if (rc != PDF_OK) return hpdf_fail_pdf(p->doc, rc);
    hpdf_restore_snap(p);
    return HPDF_OK;
}

HPDF_STATUS HPDF_Page_Concat(HPDF_Page page, HPDF_REAL a, HPDF_REAL b,
                             HPDF_REAL c, HPDF_REAL d, HPDF_REAL x, HPDF_REAL y)
{
    hpdf_page_rec *p = (hpdf_page_rec *)page;
    pdf_status rc;
    if (!p) return HPDF_INVALID_PAGE;
    rc = pdf_set_transform(p->core, a, b, c, d, x, y);
    return rc == PDF_OK ? HPDF_OK : hpdf_fail_pdf(p->doc, rc);
}

HPDF_STATUS HPDF_Page_SetLineWidth(HPDF_Page page, HPDF_REAL line_width)
{
    hpdf_page_rec *p = (hpdf_page_rec *)page;
    pdf_status rc;
    if (!p) return HPDF_INVALID_PAGE;
    rc = pdf_set_line_width(p->core, (double)line_width);
    if (rc != PDF_OK) return hpdf_fail_pdf(p->doc, rc);
    p->line_width = (double)line_width;
    return HPDF_OK;
}

HPDF_STATUS HPDF_Page_SetLineCap(HPDF_Page page, HPDF_LineCap line_cap)
{
    hpdf_page_rec *p = (hpdf_page_rec *)page;
    pdf_status rc;
    if (!p) return HPDF_INVALID_PAGE;
    rc = pdf_set_line_cap(p->core, (pdf_line_cap)line_cap);
    if (rc != PDF_OK) return hpdf_fail_pdf(p->doc, rc);
    p->line_cap = (pdf_line_cap)line_cap;
    return HPDF_OK;
}

HPDF_STATUS HPDF_Page_SetLineJoin(HPDF_Page page, HPDF_LineJoin line_join)
{
    hpdf_page_rec *p = (hpdf_page_rec *)page;
    pdf_status rc;
    if (!p) return HPDF_INVALID_PAGE;
    rc = pdf_set_line_join(p->core, (pdf_line_join)line_join);
    if (rc != PDF_OK) return hpdf_fail_pdf(p->doc, rc);
    p->line_join = (pdf_line_join)line_join;
    return HPDF_OK;
}

HPDF_STATUS HPDF_Page_SetMiterLimit(HPDF_Page page, HPDF_REAL miter_limit)
{
    hpdf_page_rec *p = (hpdf_page_rec *)page;
    pdf_status rc;
    if (!p) return HPDF_INVALID_PAGE;
    rc = pdf_set_miter_limit(p->core, (double)miter_limit);
    if (rc != PDF_OK) return hpdf_fail_pdf(p->doc, rc);
    p->miter_limit = (double)miter_limit;
    return HPDF_OK;
}

HPDF_STATUS HPDF_Page_SetDash(HPDF_Page page, const HPDF_REAL *dash_ptn,
                              HPDF_UINT num_param, HPDF_REAL phase)
{
    hpdf_page_rec *p = (hpdf_page_rec *)page;
    double pattern[8];
    HPDF_UINT i;
    pdf_status rc;
    if (!p) return HPDF_INVALID_PAGE;
    if (num_param > 8) num_param = 8;
    for (i = 0; i < num_param; ++i)
        pattern[i] = (double)dash_ptn[i];
    rc = pdf_set_line_dash(p->core, num_param ? pattern : NULL,
                           (size_t)num_param, (double)phase);
    if (rc != PDF_OK) return hpdf_fail_pdf(p->doc, rc);
    memcpy(p->dash, pattern, sizeof(double) * num_param);
    p->dash_n = (size_t)num_param;
    p->dash_phase = (double)phase;
    return HPDF_OK;
}

HPDF_STATUS HPDF_Page_SetFlat(HPDF_Page page, HPDF_REAL flatness)
{
    HPDF_UNUSED(page);
    HPDF_UNUSED(flatness);
    return HPDF_OK;
}

HPDF_STATUS HPDF_Page_SetExtGState(HPDF_Page page, HPDF_ExtGState ext_gstate)
{
    hpdf_page_rec *p = (hpdf_page_rec *)page;
    hpdf_extg_rec *g = (hpdf_extg_rec *)ext_gstate;
    pdf_status rc;
    if (!p) return HPDF_INVALID_PAGE;
    if (!g) return hpdf_fail(p->doc, HPDF_INVALID_PARAMETER);
    rc = pdf_set_opacity(p->core, g->fill_alpha, g->stroke_alpha);
    if (rc != PDF_OK) return hpdf_fail_pdf(p->doc, rc);
    p->fill_alpha = g->fill_alpha;
    p->stroke_alpha = g->stroke_alpha;
    if (g->has_blend)
    {
        rc = pdf_set_blend_mode(p->core, g->blend_mode);
        if (rc != PDF_OK) return hpdf_fail_pdf(p->doc, rc);
        p->blend_mode = g->blend_mode;
    }
    return HPDF_OK;
}

static HPDF_STATUS hpdf_set_fill(hpdf_page_rec *p, pdf_color c)
{
    pdf_status rc;
    if (!p) return HPDF_INVALID_PAGE;
    c.alpha = 1.0;
    rc = pdf_set_fill_color(p->core, &c);
    if (rc != PDF_OK) return hpdf_fail_pdf(p->doc, rc);
    p->fill = c;
    return HPDF_OK;
}

static HPDF_STATUS hpdf_set_stroke(hpdf_page_rec *p, pdf_color c)
{
    pdf_status rc;
    if (!p) return HPDF_INVALID_PAGE;
    c.alpha = 1.0;
    rc = pdf_set_stroke_color(p->core, &c);
    if (rc != PDF_OK) return hpdf_fail_pdf(p->doc, rc);
    p->stroke = c;
    return HPDF_OK;
}

HPDF_STATUS HPDF_Page_SetGrayFill(HPDF_Page page, HPDF_REAL gray)
{
    pdf_color c;
    memset(&c, 0, sizeof(c));
    c.kind = PDF_COLOR_GRAY;
    c.c1 = hpdf_clamp01((double)gray);
    c.alpha = 1.0;
    return hpdf_set_fill((hpdf_page_rec *)page, c);
}

HPDF_STATUS HPDF_Page_SetGrayStroke(HPDF_Page page, HPDF_REAL gray)
{
    pdf_color c;
    memset(&c, 0, sizeof(c));
    c.kind = PDF_COLOR_GRAY;
    c.c1 = hpdf_clamp01((double)gray);
    c.alpha = 1.0;
    return hpdf_set_stroke((hpdf_page_rec *)page, c);
}

HPDF_STATUS HPDF_Page_SetRGBFill(HPDF_Page page, HPDF_REAL r, HPDF_REAL g,
                                 HPDF_REAL b)
{
    pdf_color c;
    memset(&c, 0, sizeof(c));
    c.kind = PDF_COLOR_RGB;
    c.c1 = hpdf_clamp01((double)r);
    c.c2 = hpdf_clamp01((double)g);
    c.c3 = hpdf_clamp01((double)b);
    c.alpha = 1.0;
    return hpdf_set_fill((hpdf_page_rec *)page, c);
}

HPDF_STATUS HPDF_Page_SetRGBStroke(HPDF_Page page, HPDF_REAL r, HPDF_REAL g,
                                   HPDF_REAL b)
{
    pdf_color c;
    memset(&c, 0, sizeof(c));
    c.kind = PDF_COLOR_RGB;
    c.c1 = hpdf_clamp01((double)r);
    c.c2 = hpdf_clamp01((double)g);
    c.c3 = hpdf_clamp01((double)b);
    c.alpha = 1.0;
    return hpdf_set_stroke((hpdf_page_rec *)page, c);
}

HPDF_STATUS HPDF_Page_SetCMYKFill(HPDF_Page page, HPDF_REAL c, HPDF_REAL m,
                                  HPDF_REAL y, HPDF_REAL k)
{
    pdf_color col;
    memset(&col, 0, sizeof(col));
    col.kind = PDF_COLOR_CMYK;
    col.c1 = hpdf_clamp01((double)c);
    col.c2 = hpdf_clamp01((double)m);
    col.c3 = hpdf_clamp01((double)y);
    col.c4 = hpdf_clamp01((double)k);
    col.alpha = 1.0;
    return hpdf_set_fill((hpdf_page_rec *)page, col);
}

HPDF_STATUS HPDF_Page_SetCMYKStroke(HPDF_Page page, HPDF_REAL c, HPDF_REAL m,
                                    HPDF_REAL y, HPDF_REAL k)
{
    pdf_color col;
    memset(&col, 0, sizeof(col));
    col.kind = PDF_COLOR_CMYK;
    col.c1 = hpdf_clamp01((double)c);
    col.c2 = hpdf_clamp01((double)m);
    col.c3 = hpdf_clamp01((double)y);
    col.c4 = hpdf_clamp01((double)k);
    col.alpha = 1.0;
    return hpdf_set_stroke((hpdf_page_rec *)page, col);
}

/* ------------------------------------------------------------------ */
/* Path construction / painting                                        */
/* ------------------------------------------------------------------ */

static HPDF_STATUS hpdf_path_rc(hpdf_page_rec *p, pdf_status rc)
{
    return rc == PDF_OK ? HPDF_OK : hpdf_fail_pdf(p->doc, rc);
}

HPDF_STATUS HPDF_Page_MoveTo(HPDF_Page page, HPDF_REAL x, HPDF_REAL y)
{
    hpdf_page_rec *p = (hpdf_page_rec *)page;
    HPDF_STATUS st;
    if (!p) return HPDF_INVALID_PAGE;
    st = hpdf_path_rc(p, pdf_path_move_to(p->core, x, y));
    if (st == HPDF_OK)
    {
        p->cur_pos.x = x;
        p->cur_pos.y = y;
    }
    return st;
}

HPDF_STATUS HPDF_Page_LineTo(HPDF_Page page, HPDF_REAL x, HPDF_REAL y)
{
    hpdf_page_rec *p = (hpdf_page_rec *)page;
    HPDF_STATUS st;
    if (!p) return HPDF_INVALID_PAGE;
    st = hpdf_path_rc(p, pdf_path_line_to(p->core, x, y));
    if (st == HPDF_OK)
    {
        p->cur_pos.x = x;
        p->cur_pos.y = y;
    }
    return st;
}

HPDF_STATUS HPDF_Page_CurveTo(HPDF_Page page, HPDF_REAL x1, HPDF_REAL y1,
                              HPDF_REAL x2, HPDF_REAL y2, HPDF_REAL x3,
                              HPDF_REAL y3)
{
    hpdf_page_rec *p = (hpdf_page_rec *)page;
    HPDF_STATUS st;
    if (!p) return HPDF_INVALID_PAGE;
    st = hpdf_path_rc(p, pdf_path_curve_to(p->core, x1, y1, x2, y2, x3, y3));
    if (st == HPDF_OK)
    {
        p->cur_pos.x = x3;
        p->cur_pos.y = y3;
    }
    return st;
}

HPDF_STATUS HPDF_Page_CurveTo2(HPDF_Page page, HPDF_REAL x2, HPDF_REAL y2,
                               HPDF_REAL x3, HPDF_REAL y3)
{
    hpdf_page_rec *p = (hpdf_page_rec *)page;
    if (!p) return HPDF_INVALID_PAGE;
    return HPDF_Page_CurveTo(page, p->cur_pos.x, p->cur_pos.y, x2, y2, x3, y3);
}

HPDF_STATUS HPDF_Page_CurveTo3(HPDF_Page page, HPDF_REAL x1, HPDF_REAL y1,
                               HPDF_REAL x3, HPDF_REAL y3)
{
    return HPDF_Page_CurveTo(page, x1, y1, x3, y3, x3, y3);
}

HPDF_STATUS HPDF_Page_ClosePath(HPDF_Page page)
{
    hpdf_page_rec *p = (hpdf_page_rec *)page;
    if (!p) return HPDF_INVALID_PAGE;
    return hpdf_path_rc(p, pdf_path_close(p->core));
}

HPDF_STATUS HPDF_Page_Rectangle(HPDF_Page page, HPDF_REAL x, HPDF_REAL y,
                                HPDF_REAL width, HPDF_REAL height)
{
    hpdf_page_rec *p = (hpdf_page_rec *)page;
    pdf_rect r;
    HPDF_STATUS st;
    if (!p) return HPDF_INVALID_PAGE;
    r.x = (double)x; r.y = (double)y;
    r.w = (double)width; r.h = (double)height;
    st = hpdf_path_rc(p, pdf_path_rectangle(p->core, &r));
    if (st == HPDF_OK)
    {
        p->cur_pos.x = x;
        p->cur_pos.y = y;
    }
    return st;
}

HPDF_STATUS HPDF_Page_Circle(HPDF_Page page, HPDF_REAL x, HPDF_REAL y,
                             HPDF_REAL ray)
{
    hpdf_page_rec *p = (hpdf_page_rec *)page;
    HPDF_STATUS st;
    if (!p) return HPDF_INVALID_PAGE;
    st = hpdf_path_rc(p, pdf_path_circle(p->core, x, y, ray));
    if (st == HPDF_OK)
    {
        p->cur_pos.x = x - ray;
        p->cur_pos.y = y;
    }
    return st;
}

HPDF_STATUS HPDF_Page_Ellipse(HPDF_Page page, HPDF_REAL x, HPDF_REAL y,
                              HPDF_REAL xray, HPDF_REAL yray)
{
    hpdf_page_rec *p = (hpdf_page_rec *)page;
    HPDF_STATUS st;
    if (!p) return HPDF_INVALID_PAGE;
    st = hpdf_path_rc(p, pdf_path_ellipse(p->core, x, y, xray, yray));
    if (st == HPDF_OK)
    {
        p->cur_pos.x = x - xray;
        p->cur_pos.y = y;
    }
    return st;
}

HPDF_STATUS HPDF_Page_Arc(HPDF_Page page, HPDF_REAL x, HPDF_REAL y,
                          HPDF_REAL ray, HPDF_REAL ang1, HPDF_REAL ang2)
{
    hpdf_page_rec *p = (hpdf_page_rec *)page;
    HPDF_STATUS st;
    if (!p) return HPDF_INVALID_PAGE;
    st = hpdf_path_rc(p, pdf_path_arc(p->core, x, y, ray, ang1, ang2, 1));
    if (st == HPDF_OK)
    {
        p->cur_pos.x = (HPDF_REAL)(x + ray * cos(ang1));
        p->cur_pos.y = (HPDF_REAL)(y + ray * sin(ang1));
    }
    return st;
}

HPDF_STATUS HPDF_Page_Stroke(HPDF_Page page)
{
    hpdf_page_rec *p = (hpdf_page_rec *)page;
    if (!p) return HPDF_INVALID_PAGE;
    return hpdf_path_rc(p, pdf_path_paint(p->core, PDF_PATH_STROKE));
}

HPDF_STATUS HPDF_Page_ClosePathStroke(HPDF_Page page)
{
    HPDF_STATUS st = HPDF_Page_ClosePath(page);
    return st == HPDF_OK ? HPDF_Page_Stroke(page) : st;
}

HPDF_STATUS HPDF_Page_Fill(HPDF_Page page)
{
    hpdf_page_rec *p = (hpdf_page_rec *)page;
    if (!p) return HPDF_INVALID_PAGE;
    return hpdf_path_rc(p, pdf_path_paint(p->core, PDF_PATH_FILL));
}

HPDF_STATUS HPDF_Page_Eofill(HPDF_Page page)
{
    hpdf_page_rec *p = (hpdf_page_rec *)page;
    if (!p) return HPDF_INVALID_PAGE;
    return hpdf_path_rc(p, pdf_path_paint(p->core, PDF_PATH_FILL_EVENODD));
}

HPDF_STATUS HPDF_Page_FillStroke(HPDF_Page page)
{
    hpdf_page_rec *p = (hpdf_page_rec *)page;
    if (!p) return HPDF_INVALID_PAGE;
    return hpdf_path_rc(p, pdf_path_paint(p->core, PDF_PATH_FILL_STROKE));
}

HPDF_STATUS HPDF_Page_EofillStroke(HPDF_Page page)
{
    hpdf_page_rec *p = (hpdf_page_rec *)page;
    if (!p) return HPDF_INVALID_PAGE;
    return hpdf_path_rc(p,
                        pdf_path_paint(p->core, PDF_PATH_FILL_STROKE_EVENODD));
}

HPDF_STATUS HPDF_Page_ClosePathFillStroke(HPDF_Page page)
{
    HPDF_STATUS st = HPDF_Page_ClosePath(page);
    return st == HPDF_OK ? HPDF_Page_FillStroke(page) : st;
}

HPDF_STATUS HPDF_Page_ClosePathEofillStroke(HPDF_Page page)
{
    HPDF_STATUS st = HPDF_Page_ClosePath(page);
    return st == HPDF_OK ? HPDF_Page_EofillStroke(page) : st;
}

HPDF_STATUS HPDF_Page_EndPath(HPDF_Page page)
{
    hpdf_page_rec *p = (hpdf_page_rec *)page;
    if (!p) return HPDF_INVALID_PAGE;
    return hpdf_path_rc(p, pdf_path_discard(p->core));
}

HPDF_STATUS HPDF_Page_Clip(HPDF_Page page)
{
    hpdf_page_rec *p = (hpdf_page_rec *)page;
    if (!p) return HPDF_INVALID_PAGE;
    return hpdf_path_rc(p, pdf_path_clip(p->core, 0));
}

HPDF_STATUS HPDF_Page_Eoclip(HPDF_Page page)
{
    hpdf_page_rec *p = (hpdf_page_rec *)page;
    if (!p) return HPDF_INVALID_PAGE;
    return hpdf_path_rc(p, pdf_path_clip(p->core, 1));
}

/* ------------------------------------------------------------------ */
/* ExtGState                                                           */
/* ------------------------------------------------------------------ */

HPDF_ExtGState HPDF_CreateExtGState(HPDF_Doc pdf)
{
    hpdf_doc_rec *d = (hpdf_doc_rec *)pdf;
    hpdf_extg_rec *g;
    if (!d)
        return NULL;
    g = (hpdf_extg_rec *)calloc(1, sizeof(*g));
    if (!g)
    {
        hpdf_fail(d, HPDF_FAILD_TO_ALLOC_MEM);
        return NULL;
    }
    g->fill_alpha = 1.0;
    g->stroke_alpha = 1.0;
    g->next = d->extgs;
    d->extgs = g;
    return (HPDF_ExtGState)g;
}

HPDF_STATUS HPDF_ExtGState_SetAlphaStroke(HPDF_ExtGState ext_gstate,
                                          HPDF_REAL value)
{
    hpdf_extg_rec *g = (hpdf_extg_rec *)ext_gstate;
    if (!g) return HPDF_INVALID_PARAMETER;
    g->stroke_alpha = hpdf_clamp01((double)value);
    return HPDF_OK;
}

HPDF_STATUS HPDF_ExtGState_SetAlphaFill(HPDF_ExtGState ext_gstate,
                                        HPDF_REAL value)
{
    hpdf_extg_rec *g = (hpdf_extg_rec *)ext_gstate;
    if (!g) return HPDF_INVALID_PARAMETER;
    g->fill_alpha = hpdf_clamp01((double)value);
    return HPDF_OK;
}

HPDF_STATUS HPDF_ExtGState_SetBlendMode(HPDF_ExtGState ext_gstate,
                                        HPDF_BlendMode mode)
{
    hpdf_extg_rec *g = (hpdf_extg_rec *)ext_gstate;
    if (!g) return HPDF_INVALID_PARAMETER;
    g->blend_mode = (pdf_blend_mode)mode;
    g->has_blend = 1;
    return HPDF_OK;
}

/* ------------------------------------------------------------------ */
/* Images                                                              */
/* ------------------------------------------------------------------ */

static pdf_status hpdf_image_file(hpdf_doc_rec *d, const char *path,
                                  pdf_image *out)
{
    if (!d || !d->core || !path || !*path)
        return PDF_ERR_INVALID_PARAM;
    return pdf_image_load_file(d->core, path, out);
}

HPDF_Image HPDF_LoadPngImageFromFile(HPDF_Doc pdf, const char *filename)
{
    hpdf_doc_rec *d = (hpdf_doc_rec *)pdf;
    pdf_image img = NULL;
    pdf_status rc;
    if (!hpdf_doc_ok(d)) { hpdf_fail(d, HPDF_INVALID_DOCUMENT); return NULL; }
    rc = hpdf_image_file(d, filename, &img);
    if (rc != PDF_OK) { hpdf_fail_pdf(d, rc); return NULL; }
    hpdf_track(d, img, 1);
    return (HPDF_Image)img;
}

HPDF_Image HPDF_LoadPngImageFromFile2(HPDF_Doc pdf, const char *filename)
{
    return HPDF_LoadPngImageFromFile(pdf, filename);
}

HPDF_Image HPDF_LoadJpegImageFromFile(HPDF_Doc pdf, const char *filename)
{
    return HPDF_LoadPngImageFromFile(pdf, filename);
}

HPDF_Image HPDF_LoadPngImageFromMem(HPDF_Doc pdf, const HPDF_BYTE *buffer,
                                    HPDF_UINT size)
{
    hpdf_doc_rec *d = (hpdf_doc_rec *)pdf;
    pdf_image img = NULL;
    pdf_status rc;
    if (!hpdf_doc_ok(d)) { hpdf_fail(d, HPDF_INVALID_DOCUMENT); return NULL; }
    rc = pdf_image_load_memory(d->core, buffer, (size_t)size, &img);
    if (rc != PDF_OK) { hpdf_fail_pdf(d, rc); return NULL; }
    hpdf_track(d, img, 1);
    return (HPDF_Image)img;
}

HPDF_Image HPDF_LoadJpegImageFromMem(HPDF_Doc pdf, const HPDF_BYTE *buffer,
                                     HPDF_UINT size)
{
    return HPDF_LoadPngImageFromMem(pdf, buffer, size);
}

HPDF_UINT HPDF_Image_GetWidth(HPDF_Image image)
{
    size_t w = 0, h = 0;
    if (!image) return 0;
    pdf_image_get_size((pdf_image)image, &w, &h);
    return (HPDF_UINT)w;
}

HPDF_UINT HPDF_Image_GetHeight(HPDF_Image image)
{
    size_t w = 0, h = 0;
    if (!image) return 0;
    pdf_image_get_size((pdf_image)image, &w, &h);
    return (HPDF_UINT)h;
}

HPDF_Point HPDF_Image_GetSize(HPDF_Image image)
{
    HPDF_Point p = { 0, 0 };
    p.x = (HPDF_REAL)HPDF_Image_GetWidth(image);
    p.y = (HPDF_REAL)HPDF_Image_GetHeight(image);
    return p;
}

HPDF_STATUS HPDF_Image_GetSize2(HPDF_Image image, HPDF_Point *size)
{
    if (!size) return HPDF_INVALID_PARAMETER;
    *size = HPDF_Image_GetSize(image);
    return HPDF_OK;
}

HPDF_UINT HPDF_Image_GetBitsPerComponent(HPDF_Image image)
{
    HPDF_UNUSED(image);
    return 8;
}

const char *HPDF_Image_GetColorSpace(HPDF_Image image)
{
    HPDF_UNUSED(image);
    return "DeviceRGB";
}

HPDF_STATUS HPDF_Page_DrawImage(HPDF_Page page, HPDF_Image image,
                                HPDF_REAL x, HPDF_REAL y, HPDF_REAL width,
                                HPDF_REAL height)
{
    hpdf_page_rec *p = (hpdf_page_rec *)page;
    pdf_rect r;
    pdf_status rc;
    if (!p) return HPDF_INVALID_PAGE;
    r.x = (double)x; r.y = (double)y;
    r.w = (double)width; r.h = (double)height;
    rc = pdf_page_draw_image(p->core, (pdf_image)image, &r);
    return rc == PDF_OK ? HPDF_OK : hpdf_fail_pdf(p->doc, rc);
}

/* ------------------------------------------------------------------ */
/* Info / security / options                                           */
/* ------------------------------------------------------------------ */

static int hpdf_info_key(HPDF_InfoType type, pdf_info_key *out)
{
    switch (type)
    {
        case HPDF_INFO_TITLE:    *out = PDF_INFO_TITLE; return 1;
        case HPDF_INFO_AUTHOR:   *out = PDF_INFO_AUTHOR; return 1;
        case HPDF_INFO_SUBJECT:  *out = PDF_INFO_SUBJECT; return 1;
        case HPDF_INFO_KEYWORDS: *out = PDF_INFO_KEYWORDS; return 1;
        case HPDF_INFO_CREATOR:  *out = PDF_INFO_CREATOR; return 1;
        case HPDF_INFO_PRODUCER: *out = PDF_INFO_PRODUCER; return 1;
        default: return 0;
    }
}

HPDF_STATUS HPDF_SetInfoAttr(HPDF_Doc pdf, HPDF_InfoType type,
                             const char *value)
{
    hpdf_doc_rec *d = (hpdf_doc_rec *)pdf;
    pdf_info_key key;
    pdf_status rc;
    if (!hpdf_doc_ok(d)) return HPDF_INVALID_DOCUMENT;
    if (!hpdf_info_key(type, &key))
        return HPDF_OK; /* date/trapped entries are not modelled */
    rc = pdf_doc_set_info(d->core, key, value ? value : "");
    return rc == PDF_OK ? HPDF_OK : hpdf_fail_pdf(d, rc);
}

const char *HPDF_GetInfoAttr(HPDF_Doc pdf, HPDF_InfoType type)
{
    hpdf_doc_rec *d = (hpdf_doc_rec *)pdf;
    pdf_info_key key;
    size_t len = sizeof(d->info_buf);
    pdf_status rc;
    if (!hpdf_doc_ok(d)) return "";
    if (!hpdf_info_key(type, &key))
        return "";
    rc = pdf_doc_get_info(d->core, key, d->info_buf, &len);
    if (rc != PDF_OK)
    {
        d->info_buf[0] = '\0';
        return d->info_buf;
    }
    d->info_buf[sizeof(d->info_buf) - 1] = '\0';
    return d->info_buf;
}

HPDF_STATUS HPDF_SetInfoDateAttr(HPDF_Doc pdf, HPDF_InfoType type,
                                 HPDF_Date value)
{
    HPDF_UNUSED(value);
    HPDF_UNUSED(type);
    if (!pdf)
        return HPDF_INVALID_PARAMETER;
    return HPDF_OK;
}

HPDF_STATUS HPDF_SetCompressionMode(HPDF_Doc pdf, HPDF_UINT mode)
{
    hpdf_doc_rec *d = (hpdf_doc_rec *)pdf;
    unsigned mask = 0;
    pdf_status rc;
    if (!hpdf_doc_ok(d)) return HPDF_INVALID_DOCUMENT;
    if (mode & HPDF_COMP_TEXT)     mask |= PDF_COMPRESS_TEXT;
    if (mode & HPDF_COMP_IMAGE)    mask |= PDF_COMPRESS_IMAGE;
    if (mode & HPDF_COMP_METADATA) mask |= PDF_COMPRESS_METADATA;
    if (mode == HPDF_COMP_ALL)     mask = PDF_COMPRESS_ALL;
    rc = pdf_doc_set_compression(d->core, mask);
    if (rc == PDF_OK)
        d->compression = (int)mode;
    return rc == PDF_OK ? HPDF_OK : hpdf_fail_pdf(d, rc);
}

HPDF_STATUS HPDF_SetPassword(HPDF_Doc pdf, const char *owner_passwd,
                             const char *user_passwd)
{
    hpdf_doc_rec *d = (hpdf_doc_rec *)pdf;
    if (!d) return HPDF_INVALID_PARAMETER;
    snprintf(d->owner_pass, sizeof(d->owner_pass), "%s",
             owner_passwd ? owner_passwd : "");
    snprintf(d->user_pass, sizeof(d->user_pass), "%s",
             user_passwd ? user_passwd : "");
    d->have_password = 1;
    d->encrypt_applied = 0;
    return HPDF_OK;
}

HPDF_STATUS HPDF_SetPermission(HPDF_Doc pdf, HPDF_UINT permission)
{
    hpdf_doc_rec *d = (hpdf_doc_rec *)pdf;
    if (!d) return HPDF_INVALID_PARAMETER;
    d->permissions = permission;
    d->encrypt_applied = 0;
    return HPDF_OK;
}

HPDF_STATUS HPDF_SetEncryptionMode(HPDF_Doc pdf, HPDF_EncryptMode mode,
                                   HPDF_UINT key_len)
{
    hpdf_doc_rec *d = (hpdf_doc_rec *)pdf;
    if (!d) return HPDF_INVALID_PARAMETER;
    if (mode != HPDF_ENCRYPT_R2 && mode != HPDF_ENCRYPT_R3)
        return hpdf_fail(d, HPDF_INVALID_ENCRYPT_KEY_LEN);
    HPDF_UNUSED(key_len);
    d->encryption_mode = (int)mode;
    d->encrypt_applied = 0;
    return HPDF_OK;
}

HPDF_PageLayout HPDF_GetPageLayout(HPDF_Doc pdf)
{
    hpdf_doc_rec *d = (hpdf_doc_rec *)pdf;
    return d ? d->layout : HPDF_PAGE_LAYOUT_SINGLE;
}

HPDF_STATUS HPDF_SetPageLayout(HPDF_Doc pdf, HPDF_PageLayout layout)
{
    hpdf_doc_rec *d = (hpdf_doc_rec *)pdf;
    if (!d) return HPDF_INVALID_PARAMETER;
    d->layout = layout;
    return HPDF_OK;
}

HPDF_PageMode HPDF_GetPageMode(HPDF_Doc pdf)
{
    hpdf_doc_rec *d = (hpdf_doc_rec *)pdf;
    return d ? d->page_mode : HPDF_PAGE_MODE_USE_NONE;
}

HPDF_STATUS HPDF_SetPageMode(HPDF_Doc pdf, HPDF_PageMode mode)
{
    hpdf_doc_rec *d = (hpdf_doc_rec *)pdf;
    pdf_status rc;
    if (!hpdf_doc_ok(d)) return HPDF_INVALID_DOCUMENT;
    d->page_mode = mode;
    rc = pdf_doc_set_page_mode_use_outlines(
        d->core, mode == HPDF_PAGE_MODE_USE_OUTLINE);
    return rc == PDF_OK ? HPDF_OK : hpdf_fail_pdf(d, rc);
}

HPDF_UINT HPDF_GetViewerPreference(HPDF_Doc pdf)
{
    hpdf_doc_rec *d = (hpdf_doc_rec *)pdf;
    return d ? d->viewer_pref : 0;
}

HPDF_STATUS HPDF_SetViewerPreference(HPDF_Doc pdf, HPDF_UINT value)
{
    hpdf_doc_rec *d = (hpdf_doc_rec *)pdf;
    if (!d) return HPDF_INVALID_PARAMETER;
    d->viewer_pref = value;
    return HPDF_OK;
}

HPDF_STATUS HPDF_SetOpenAction(HPDF_Doc pdf, HPDF_Destination open_action)
{
    HPDF_UNUSED(open_action);
    if (!pdf) return HPDF_INVALID_PARAMETER;
    return HPDF_UNSUPPORTED_FUNC;
}

/* ------------------------------------------------------------------ */
/* Destinations / outlines                                             */
/* ------------------------------------------------------------------ */

HPDF_Destination HPDF_Page_CreateDestination(HPDF_Page page)
{
    hpdf_page_rec *p = (hpdf_page_rec *)page;
    hpdf_dest_rec *dst;
    if (!p) return NULL;
    dst = (hpdf_dest_rec *)calloc(1, sizeof(*dst));
    if (!dst)
    {
        hpdf_fail(p->doc, HPDF_FAILD_TO_ALLOC_MEM);
        return NULL;
    }
    dst->doc = p->doc;
    dst->page = page;
    dst->type = HPDF_XYZ;
    dst->zoom = 0;
    dst->next = p->doc->dests;
    p->doc->dests = dst;
    return (HPDF_Destination)dst;
}

HPDF_STATUS HPDF_Destination_SetXYZ(HPDF_Destination dst, HPDF_REAL left,
                                    HPDF_REAL top, HPDF_REAL zoom)
{
    hpdf_dest_rec *x = (hpdf_dest_rec *)dst;
    if (!x) return HPDF_INVALID_DESTINATION;
    x->type = HPDF_XYZ;
    x->left = left; x->top = top; x->zoom = zoom;
    return HPDF_OK;
}

HPDF_STATUS HPDF_Destination_SetFit(HPDF_Destination dst)
{
    hpdf_dest_rec *x = (hpdf_dest_rec *)dst;
    if (!x) return HPDF_INVALID_DESTINATION;
    x->type = HPDF_FIT;
    return HPDF_OK;
}

HPDF_STATUS HPDF_Destination_SetFitH(HPDF_Destination dst, HPDF_REAL top)
{
    hpdf_dest_rec *x = (hpdf_dest_rec *)dst;
    if (!x) return HPDF_INVALID_DESTINATION;
    x->type = HPDF_FIT_H; x->top = top;
    return HPDF_OK;
}

HPDF_STATUS HPDF_Destination_SetFitV(HPDF_Destination dst, HPDF_REAL left)
{
    hpdf_dest_rec *x = (hpdf_dest_rec *)dst;
    if (!x) return HPDF_INVALID_DESTINATION;
    x->type = HPDF_FIT_V; x->left = left;
    return HPDF_OK;
}

HPDF_STATUS HPDF_Destination_SetFitR(HPDF_Destination dst, HPDF_REAL left,
                                     HPDF_REAL bottom, HPDF_REAL right,
                                     HPDF_REAL top)
{
    hpdf_dest_rec *x = (hpdf_dest_rec *)dst;
    if (!x) return HPDF_INVALID_DESTINATION;
    x->type = HPDF_FIT_R;
    x->left = left; x->bottom = bottom; x->right = right; x->top = top;
    return HPDF_OK;
}

HPDF_STATUS HPDF_Destination_SetFitB(HPDF_Destination dst)
{
    hpdf_dest_rec *x = (hpdf_dest_rec *)dst;
    if (!x) return HPDF_INVALID_DESTINATION;
    x->type = HPDF_FIT_B;
    return HPDF_OK;
}

HPDF_STATUS HPDF_Destination_SetFitBH(HPDF_Destination dst, HPDF_REAL top)
{
    hpdf_dest_rec *x = (hpdf_dest_rec *)dst;
    if (!x) return HPDF_INVALID_DESTINATION;
    x->type = HPDF_FIT_BH; x->top = top;
    return HPDF_OK;
}

HPDF_STATUS HPDF_Destination_SetFitBV(HPDF_Destination dst, HPDF_REAL left)
{
    hpdf_dest_rec *x = (hpdf_dest_rec *)dst;
    if (!x) return HPDF_INVALID_DESTINATION;
    x->type = HPDF_FIT_BV; x->left = left;
    return HPDF_OK;
}

HPDF_Outline HPDF_CreateOutline(HPDF_Doc pdf, HPDF_Outline parent,
                                const char *title, HPDF_Encoder encoder)
{
    hpdf_doc_rec *d = (hpdf_doc_rec *)pdf;
    pdf_outline out = NULL;
    pdf_status rc;
    HPDF_UNUSED(encoder);
    if (!hpdf_doc_ok(d))
    {
        hpdf_fail(d, HPDF_INVALID_DOCUMENT);
        return NULL;
    }
    rc = pdf_outline_add(d->core, (pdf_outline)parent,
                         title ? title : "", NULL, &out);
    if (rc != PDF_OK)
    {
        hpdf_fail_pdf(d, rc);
        return NULL;
    }
    hpdf_track(d, out, 3);
    return (HPDF_Outline)out;
}

HPDF_STATUS HPDF_Outline_SetOpened(HPDF_Outline outline, HPDF_BOOL opened)
{
    pdf_status rc;
    if (!outline) return HPDF_INVALID_OUTLINE;
    rc = pdf_outline_set_open((pdf_outline)outline, opened ? 1 : 0);
    return rc == PDF_OK ? HPDF_OK : HPDF_INVALID_OUTLINE;
}

HPDF_STATUS HPDF_Outline_SetDestination(HPDF_Outline outline,
                                        HPDF_Destination dst)
{
    hpdf_dest_rec *x = (hpdf_dest_rec *)dst;
    pdf_page page;
    pdf_status rc;
    if (!outline || !x || !x->page)
        return HPDF_INVALID_DESTINATION;
    page = ((hpdf_page_rec *)x->page)->core;
    rc = pdf_outline_set_destination((pdf_outline)outline, &page);
    return rc == PDF_OK ? HPDF_OK : HPDF_UNSUPPORTED_FUNC;
}

/* ------------------------------------------------------------------ */
/* Annotations                                                         */
/* ------------------------------------------------------------------ */

static HPDF_Annotation hpdf_create_annot(hpdf_page_rec *p, pdf_annot_type type,
                                         const HPDF_Rect *rect,
                                         const char *content)
{
    pdf_rect r;
    pdf_annot a = NULL;
    pdf_status rc;
    if (!p) return NULL;
    r.x = rect->left;
    r.y = rect->bottom;
    r.w = (double)(rect->right - rect->left);
    r.h = (double)(rect->top - rect->bottom);
    rc = pdf_annot_create(p->core, type, &r, content, &a);
    if (rc != PDF_OK)
    {
        hpdf_fail_pdf(p->doc, rc);
        return NULL;
    }
    hpdf_track(p->doc, a, 2);
    return (HPDF_Annotation)a;
}

HPDF_Annotation HPDF_Page_CreateTextAnnot(HPDF_Page page, HPDF_Rect rect,
                                          const char *text,
                                          HPDF_Encoder encoder)
{
    HPDF_UNUSED(encoder);
    return hpdf_create_annot((hpdf_page_rec *)page, PDF_ANNOT_TEXT, &rect,
                             text ? text : "");
}

HPDF_Annotation HPDF_Page_CreateURILinkAnnot(HPDF_Page page, HPDF_Rect rect,
                                             const char *uri)
{
    if (!uri || !*uri)
    {
        hpdf_fail(hpdf_doc_of_page(page), HPDF_INVALID_URI);
        return NULL;
    }
    return hpdf_create_annot((hpdf_page_rec *)page, PDF_ANNOT_LINK_URI, &rect,
                             uri);
}

HPDF_Annotation HPDF_Page_CreateLinkAnnot(HPDF_Page page, HPDF_Rect rect,
                                          HPDF_Destination dst)
{
    hpdf_dest_rec *x = (hpdf_dest_rec *)dst;
    HPDF_Annotation a = hpdf_create_annot((hpdf_page_rec *)page,
                                          PDF_ANNOT_LINK_GOTO, &rect, NULL);
    if (!a)
        return NULL;
    if (!x || !x->page)
    {
        hpdf_fail(hpdf_doc_of_page(page), HPDF_INVALID_DESTINATION);
        return NULL;
    }
    {
        pdf_page dest = ((hpdf_page_rec *)x->page)->core;
        pdf_status rc = pdf_annot_link_to_page((pdf_annot)a, &dest, NULL);
        if (rc != PDF_OK)
        {
            hpdf_fail_pdf(hpdf_doc_of_page(page), rc);
            return NULL;
        }
    }
    return a;
}

HPDF_Annotation HPDF_Page_CreateHighlightAnnot(HPDF_Page page, HPDF_Rect rect,
                                               const char *text,
                                               HPDF_Encoder encoder)
{
    HPDF_UNUSED(encoder);
    return hpdf_create_annot((hpdf_page_rec *)page, PDF_ANNOT_HIGHLIGHT,
                             &rect, text ? text : "");
}

HPDF_Annotation HPDF_Page_CreateUnderlineAnnot(HPDF_Page page, HPDF_Rect rect,
                                               const char *text,
                                               HPDF_Encoder encoder)
{
    HPDF_UNUSED(encoder);
    return hpdf_create_annot((hpdf_page_rec *)page, PDF_ANNOT_UNDERLINE,
                             &rect, text ? text : "");
}

HPDF_Annotation HPDF_Page_CreateSquigglyAnnot(HPDF_Page page, HPDF_Rect rect,
                                              const char *text,
                                              HPDF_Encoder encoder)
{
    HPDF_UNUSED(encoder);
    return hpdf_create_annot((hpdf_page_rec *)page, PDF_ANNOT_SQUIGGLY,
                             &rect, text ? text : "");
}

HPDF_Annotation HPDF_Page_CreateStrikeOutAnnot(HPDF_Page page, HPDF_Rect rect,
                                               const char *text,
                                               HPDF_Encoder encoder)
{
    HPDF_UNUSED(encoder);
    return hpdf_create_annot((hpdf_page_rec *)page, PDF_ANNOT_STRIKEOUT,
                             &rect, text ? text : "");
}

HPDF_Annotation HPDF_Page_CreateSquareAnnot(HPDF_Page page, HPDF_Rect rect,
                                            const char *text,
                                            HPDF_Encoder encoder)
{
    HPDF_UNUSED(encoder);
    return hpdf_create_annot((hpdf_page_rec *)page, PDF_ANNOT_SQUARE, &rect,
                             text ? text : "");
}

HPDF_Annotation HPDF_Page_CreateCircleAnnot(HPDF_Page page, HPDF_Rect rect,
                                            const char *text,
                                            HPDF_Encoder encoder)
{
    HPDF_UNUSED(encoder);
    return hpdf_create_annot((hpdf_page_rec *)page, PDF_ANNOT_CIRCLE, &rect,
                             text ? text : "");
}

HPDF_STATUS HPDF_TextAnnot_SetIcon(HPDF_Annotation annot, HPDF_AnnotIcon icon)
{
    pdf_status rc;
    if (!annot) return HPDF_INVALID_ANNOTATION;
    rc = pdf_annot_set_icon((pdf_annot)annot, (pdf_annot_icon)icon);
    return rc == PDF_OK ? HPDF_OK : hpdf_map_pdf_status(rc);
}

HPDF_STATUS HPDF_TextAnnot_SetOpened(HPDF_Annotation annot, HPDF_BOOL opened)
{
    pdf_status rc;
    if (!annot) return HPDF_INVALID_ANNOTATION;
    rc = pdf_annot_set_open((pdf_annot)annot, opened ? 1 : 0);
    return rc == PDF_OK ? HPDF_OK : hpdf_map_pdf_status(rc);
}

static HPDF_STATUS hpdf_annot_color(HPDF_Annotation annot, const pdf_color *c)
{
    pdf_status rc;
    if (!annot) return HPDF_INVALID_ANNOTATION;
    rc = pdf_annot_set_color((pdf_annot)annot, c);
    return rc == PDF_OK ? HPDF_OK : hpdf_map_pdf_status(rc);
}

HPDF_STATUS HPDF_Annot_SetRGBColor(HPDF_Annotation annot, HPDF_RGBColor color)
{
    pdf_color c;
    memset(&c, 0, sizeof(c));
    c.kind = PDF_COLOR_RGB;
    c.c1 = color.r; c.c2 = color.g; c.c3 = color.b;
    return hpdf_annot_color(annot, &c);
}

HPDF_STATUS HPDF_Annot_SetCMYKColor(HPDF_Annotation annot,
                                    HPDF_CMYKColor color)
{
    pdf_color c;
    memset(&c, 0, sizeof(c));
    c.kind = PDF_COLOR_CMYK;
    c.c1 = color.c; c.c2 = color.m; c.c3 = color.y; c.c4 = color.k;
    return hpdf_annot_color(annot, &c);
}

HPDF_STATUS HPDF_Annot_SetGrayColor(HPDF_Annotation annot, HPDF_REAL color)
{
    pdf_color c;
    memset(&c, 0, sizeof(c));
    c.kind = PDF_COLOR_GRAY;
    c.c1 = color;
    return hpdf_annot_color(annot, &c);
}

HPDF_STATUS HPDF_Annot_SetNoColor(HPDF_Annotation annot)
{
    if (!annot) return HPDF_INVALID_ANNOTATION;
    return HPDF_OK;
}

HPDF_STATUS HPDF_MarkupAnnot_SetTitle(HPDF_Annotation annot, const char *name)
{
    pdf_status rc;
    if (!annot) return HPDF_INVALID_ANNOTATION;
    rc = pdf_annot_set_author((pdf_annot)annot, name);
    return rc == PDF_OK ? HPDF_OK : hpdf_map_pdf_status(rc);
}

HPDF_STATUS HPDF_MarkupAnnot_SetSubject(HPDF_Annotation annot,
                                        const char *name)
{
    pdf_status rc;
    if (!annot) return HPDF_INVALID_ANNOTATION;
    rc = pdf_annot_set_subject((pdf_annot)annot, name);
    return rc == PDF_OK ? HPDF_OK : hpdf_map_pdf_status(rc);
}

HPDF_STATUS HPDF_MarkupAnnot_SetTransparency(HPDF_Annotation annot,
                                             HPDF_REAL value)
{
    pdf_status rc;
    if (!annot) return HPDF_INVALID_ANNOTATION;
    rc = pdf_annot_set_opacity((pdf_annot)annot, value);
    return rc == PDF_OK ? HPDF_OK : hpdf_map_pdf_status(rc);
}

HPDF_STATUS HPDF_MarkupAnnot_SetCreationDate(HPDF_Annotation annot,
                                             HPDF_Date value)
{
    HPDF_UNUSED(annot);
    HPDF_UNUSED(value);
    return HPDF_OK;
}

HPDF_STATUS HPDF_MarkupAnnot_SetIntent(HPDF_Annotation annot,
                                        HPDF_AnnotIntent intent)
{
    HPDF_UNUSED(annot);
    HPDF_UNUSED(intent);
    return HPDF_OK;
}

HPDF_STATUS HPDF_MarkupAnnot_SetPopup(HPDF_Annotation annot,
                                      HPDF_Annotation popup)
{
    HPDF_UNUSED(annot);
    HPDF_UNUSED(popup);
    return HPDF_UNSUPPORTED_FUNC;
}

HPDF_STATUS HPDF_MarkupAnnot_SetRectDiff(HPDF_Annotation annot, HPDF_Rect rect)
{
    HPDF_UNUSED(annot);
    HPDF_UNUSED(rect);
    return HPDF_UNSUPPORTED_FUNC;
}

HPDF_STATUS HPDF_MarkupAnnot_SetCloudEffect(HPDF_Annotation annot,
                                            HPDF_INT cloudIntensity)
{
    HPDF_UNUSED(annot);
    HPDF_UNUSED(cloudIntensity);
    return HPDF_UNSUPPORTED_FUNC;
}

HPDF_STATUS HPDF_MarkupAnnot_SetInteriorRGBColor(HPDF_Annotation annot,
                                                 HPDF_RGBColor color)
{
    HPDF_UNUSED(annot);
    HPDF_UNUSED(color);
    return HPDF_UNSUPPORTED_FUNC;
}

HPDF_STATUS HPDF_MarkupAnnot_SetInteriorCMYKColor(HPDF_Annotation annot,
                                                  HPDF_CMYKColor color)
{
    HPDF_UNUSED(annot);
    HPDF_UNUSED(color);
    return HPDF_UNSUPPORTED_FUNC;
}

HPDF_STATUS HPDF_MarkupAnnot_SetInteriorGrayColor(HPDF_Annotation annot,
                                                  HPDF_REAL color)
{
    HPDF_UNUSED(annot);
    HPDF_UNUSED(color);
    return HPDF_UNSUPPORTED_FUNC;
}

HPDF_STATUS HPDF_MarkupAnnot_SetInteriorTransparent(HPDF_Annotation annot)
{
    HPDF_UNUSED(annot);
    return HPDF_UNSUPPORTED_FUNC;
}

HPDF_STATUS HPDF_TextMarkupAnnot_SetQuadPoints(HPDF_Annotation annot,
                                               HPDF_Point lb, HPDF_Point rb,
                                               HPDF_Point rt, HPDF_Point lt)
{
    HPDF_UNUSED(annot);
    HPDF_UNUSED(lb); HPDF_UNUSED(rb); HPDF_UNUSED(rt); HPDF_UNUSED(lt);
    return HPDF_OK;
}

HPDF_STATUS HPDF_LinkAnnot_SetHighlightMode(HPDF_Annotation annot,
                                            HPDF_AnnotHighlightMode mode)
{
    HPDF_UNUSED(annot);
    HPDF_UNUSED(mode);
    return HPDF_OK;
}

HPDF_STATUS HPDF_LinkAnnot_SetJavaScript(HPDF_Annotation annot,
                                         HPDF_JavaScript javascript)
{
    HPDF_UNUSED(annot);
    HPDF_UNUSED(javascript);
    return HPDF_UNSUPPORTED_FUNC;
}

HPDF_STATUS HPDF_LinkAnnot_SetBorderStyle(HPDF_Annotation annot,
                                          HPDF_REAL width, HPDF_UINT16 dash_on,
                                          HPDF_UINT16 dash_off)
{
    HPDF_UNUSED(annot);
    HPDF_UNUSED(width);
    HPDF_UNUSED(dash_on);
    HPDF_UNUSED(dash_off);
    return HPDF_OK;
}

HPDF_STATUS HPDF_Annotation_SetBorderStyle(HPDF_Annotation annot,
                                           HPDF_BSSubtype subtype,
                                           HPDF_REAL width, HPDF_UINT16 dash_on,
                                           HPDF_UINT16 dash_off,
                                           HPDF_UINT16 dash_phase)
{
    HPDF_UNUSED(annot);
    HPDF_UNUSED(subtype);
    HPDF_UNUSED(width);
    HPDF_UNUSED(dash_on);
    HPDF_UNUSED(dash_off);
    HPDF_UNUSED(dash_phase);
    return HPDF_OK;
}

HPDF_STATUS HPDF_PopupAnnot_SetOpened(HPDF_Annotation annot, HPDF_BOOL opened)
{
    HPDF_UNUSED(annot);
    HPDF_UNUSED(opened);
    return HPDF_OK;
}

HPDF_STATUS HPDF_FreeTextAnnot_SetLineEndingStyle(
    HPDF_Annotation annot, HPDF_LineAnnotEndingStyle startStyle,
    HPDF_LineAnnotEndingStyle endStyle)
{
    HPDF_UNUSED(annot); HPDF_UNUSED(startStyle); HPDF_UNUSED(endStyle);
    return HPDF_UNSUPPORTED_FUNC;
}

HPDF_STATUS HPDF_FreeTextAnnot_Set3PointCalloutLine(
    HPDF_Annotation annot, HPDF_Point startPoint, HPDF_Point kneePoint,
    HPDF_Point endPoint)
{
    HPDF_UNUSED(annot); HPDF_UNUSED(startPoint);
    HPDF_UNUSED(kneePoint); HPDF_UNUSED(endPoint);
    return HPDF_UNSUPPORTED_FUNC;
}

HPDF_STATUS HPDF_FreeTextAnnot_Set2PointCalloutLine(
    HPDF_Annotation annot, HPDF_Point startPoint, HPDF_Point endPoint)
{
    HPDF_UNUSED(annot); HPDF_UNUSED(startPoint); HPDF_UNUSED(endPoint);
    return HPDF_UNSUPPORTED_FUNC;
}

HPDF_STATUS HPDF_FreeTextAnnot_SetDefaultStyle(HPDF_Annotation annot,
                                               const char *style)
{
    HPDF_UNUSED(annot); HPDF_UNUSED(style);
    return HPDF_UNSUPPORTED_FUNC;
}

HPDF_STATUS HPDF_LineAnnot_SetPosition(
    HPDF_Annotation annot, HPDF_Point startPoint,
    HPDF_LineAnnotEndingStyle startStyle, HPDF_Point endPoint,
    HPDF_LineAnnotEndingStyle endStyle)
{
    HPDF_UNUSED(annot); HPDF_UNUSED(startPoint); HPDF_UNUSED(startStyle);
    HPDF_UNUSED(endPoint); HPDF_UNUSED(endStyle);
    return HPDF_UNSUPPORTED_FUNC;
}

HPDF_STATUS HPDF_LineAnnot_SetLeader(HPDF_Annotation annot,
                                     HPDF_INT leaderLen,
                                     HPDF_INT leaderExtLen,
                                     HPDF_INT leaderOffsetLen)
{
    HPDF_UNUSED(annot); HPDF_UNUSED(leaderLen);
    HPDF_UNUSED(leaderExtLen); HPDF_UNUSED(leaderOffsetLen);
    return HPDF_UNSUPPORTED_FUNC;
}

HPDF_STATUS HPDF_LineAnnot_SetCaption(HPDF_Annotation annot,
                                       HPDF_BOOL showCaption,
                                       HPDF_LineAnnotCapPosition position,
                                       HPDF_INT horzOffset,
                                       HPDF_INT vertOffset)
{
    HPDF_UNUSED(annot); HPDF_UNUSED(showCaption); HPDF_UNUSED(position);
    HPDF_UNUSED(horzOffset); HPDF_UNUSED(vertOffset);
    return HPDF_UNSUPPORTED_FUNC;
}

HPDF_STATUS HPDF_ProjectionAnnot_SetExData(HPDF_Annotation annot,
                                           HPDF_ExData exdata)
{
    HPDF_UNUSED(annot); HPDF_UNUSED(exdata);
    return HPDF_UNSUPPORTED_FUNC;
}

/* ------------------------------------------------------------------ */
/* Font metrics                                                        */
/* ------------------------------------------------------------------ */

const char *HPDF_Font_GetFontName(HPDF_Font font)
{
    hpdf_font_rec *f = (hpdf_font_rec *)font;
    return f ? f->name : "";
}

const char *HPDF_Font_GetEncodingName(HPDF_Font font)
{
    hpdf_font_rec *f = (hpdf_font_rec *)font;
    return f ? f->encoding : "";
}

HPDF_INT HPDF_Font_GetAscent(HPDF_Font font)
{
    hpdf_font_rec *f = (hpdf_font_rec *)font;
    double v = 0;
    if (!f || !f->core) return 0;
    if (pdf_font_ascent(f->core, 1000.0, &v) != PDF_OK)
        return 0;
    return (HPDF_INT)(v + (v >= 0 ? 0.5 : -0.5));
}

HPDF_INT HPDF_Font_GetDescent(HPDF_Font font)
{
    hpdf_font_rec *f = (hpdf_font_rec *)font;
    double v = 0;
    if (!f || !f->core) return 0;
    if (pdf_font_descent(f->core, 1000.0, &v) != PDF_OK)
        return 0;
    return (HPDF_INT)(v + (v >= 0 ? 0.5 : -0.5));
}

HPDF_UINT HPDF_Font_GetXHeight(HPDF_Font font)
{
    HPDF_INT a = HPDF_Font_GetAscent(font);
    return (HPDF_UINT)(a > 0 ? a / 2 : 0);
}

HPDF_UINT HPDF_Font_GetCapHeight(HPDF_Font font)
{
    HPDF_INT a = HPDF_Font_GetAscent(font);
    return (HPDF_UINT)(a > 0 ? a * 7 / 10 : 0);
}

HPDF_Box HPDF_Font_GetBBox(HPDF_Font font)
{
    HPDF_Box b;
    b.left = 0;
    b.right = 1000;
    b.top = (HPDF_REAL)HPDF_Font_GetAscent(font);
    b.bottom = (HPDF_REAL)HPDF_Font_GetDescent(font);
    return b;
}

HPDF_INT HPDF_Font_GetUnicodeWidth(HPDF_Font font, HPDF_UNICODE code)
{
    hpdf_font_rec *f = (hpdf_font_rec *)font;
    char utf8[8];
    int n = 0;
    if (!f || !f->core) return 0;
    if (code < 0x80)
    {
        utf8[n++] = (char)code;
    }
    else if (code < 0x800)
    {
        utf8[n++] = (char)(0xC0 | (code >> 6));
        utf8[n++] = (char)(0x80 | (code & 0x3F));
    }
    else
    {
        utf8[n++] = (char)(0xE0 | (code >> 12));
        utf8[n++] = (char)(0x80 | ((code >> 6) & 0x3F));
        utf8[n++] = (char)(0x80 | (code & 0x3F));
    }
    utf8[n] = '\0';
    return (HPDF_INT)(hpdf_font_width_units(f, utf8) + 0.5);
}

HPDF_TextWidth HPDF_Font_TextWidth(HPDF_Font font, const HPDF_BYTE *text,
                                   HPDF_UINT len)
{
    hpdf_font_rec *f = (hpdf_font_rec *)font;
    HPDF_TextWidth tw;
    char *tmp;
    size_t n;
    memset(&tw, 0, sizeof(tw));
    if (!f || !text || len == 0)
        return tw;
    tmp = (char *)malloc((size_t)len + 1);
    if (!tmp)
        return tw;
    memcpy(tmp, text, len);
    tmp[len] = '\0';
    n = strlen(tmp);
    tw.numchars = (HPDF_UINT)hpdf_utf8_count(tmp, n);
    {
        HPDF_UINT i;
        for (i = 0; i < n; ++i)
            if (tmp[i] == ' ')
                ++tw.numspace;
    }
    tw.numwords = tw.numchars - tw.numspace + (tw.numspace ? 1 : 0);
    tw.width = (HPDF_UINT)(hpdf_font_width_units(f, tmp) + 0.5);
    free(tmp);
    return tw;
}

HPDF_UINT HPDF_Font_MeasureText(HPDF_Font font, const HPDF_BYTE *text,
                                HPDF_UINT len, HPDF_REAL width,
                                HPDF_REAL font_size, HPDF_REAL char_space,
                                HPDF_REAL word_space, HPDF_BOOL wordwrap,
                                HPDF_REAL *real_width)
{
    return hpdf_measure_text_impl((hpdf_font_rec *)font, (const char *)text,
                                  (size_t)len, (double)width,
                                  (double)font_size, (double)char_space,
                                  (double)word_space, wordwrap != 0,
                                  real_width);
}

/* ------------------------------------------------------------------ */
/* Encoders (opaque stubs)                                             */
/* ------------------------------------------------------------------ */

static int g_hpdf_encoder_stub;

HPDF_Encoder HPDF_GetEncoder(HPDF_Doc pdf, const char *encoding_name)
{
    HPDF_UNUSED(pdf);
    HPDF_UNUSED(encoding_name);
    /* The pdflib core handles UTF-8 / WinAnsi internally; callers only pass
     * this handle back to APIs that ignore it. A stable non-NULL value keeps
     * code such as `if (!(enc = HPDF_GetEncoder(...)))` on the happy path. */
    return (HPDF_Encoder)&g_hpdf_encoder_stub;
}

HPDF_Encoder HPDF_GetCurrentEncoder(HPDF_Doc pdf)
{
    return HPDF_GetEncoder(pdf, "WinAnsiEncoding");
}

HPDF_STATUS HPDF_SetCurrentEncoder(HPDF_Doc pdf, const char *encoding_name)
{
    HPDF_UNUSED(pdf);
    HPDF_UNUSED(encoding_name);
    return HPDF_OK;
}

HPDF_EncoderType HPDF_Encoder_GetType(HPDF_Encoder encoder)
{
    HPDF_UNUSED(encoder);
    return HPDF_ENCODER_TYPE_SINGLE_BYTE;
}

HPDF_ByteType HPDF_Encoder_GetByteType(HPDF_Encoder encoder, const char *text,
                                       HPDF_UINT index)
{
    HPDF_UNUSED(encoder);
    HPDF_UNUSED(index);
    if (text && ((unsigned char)text[0] & 0x80))
        return HPDF_BYTE_TYPE_LEAD;
    return HPDF_BYTE_TYPE_SINGLE;
}

HPDF_UNICODE HPDF_Encoder_GetUnicode(HPDF_Encoder encoder, HPDF_UINT16 code)
{
    HPDF_UNUSED(encoder);
    return (HPDF_UNICODE)code;
}

HPDF_WritingMode HPDF_Encoder_GetWritingMode(HPDF_Encoder encoder)
{
    HPDF_UNUSED(encoder);
    return HPDF_WMODE_HORIZONTAL;
}

HPDF_STATUS HPDF_UseJPEncodings(HPDF_Doc pdf)  { HPDF_UNUSED(pdf); return HPDF_OK; }
HPDF_STATUS HPDF_UseKREncodings(HPDF_Doc pdf)  { HPDF_UNUSED(pdf); return HPDF_OK; }
HPDF_STATUS HPDF_UseCNSEncodings(HPDF_Doc pdf) { HPDF_UNUSED(pdf); return HPDF_OK; }
HPDF_STATUS HPDF_UseCNTEncodings(HPDF_Doc pdf) { HPDF_UNUSED(pdf); return HPDF_OK; }
HPDF_STATUS HPDF_UseUTFEncodings(HPDF_Doc pdf) { HPDF_UNUSED(pdf); return HPDF_OK; }

/* ------------------------------------------------------------------ */
/* Unsupported / not-modelled libharu surface                          */
/* ------------------------------------------------------------------ */

/* Constructors that cannot be expressed by the pdflib core. They report
 * HPDF_UNSUPPORTED_FUNC (never HPDF_OK silently). */
static HPDF_Annotation hpdf_unsupported_annot(HPDF_Doc d, HPDF_STATUS code)
{
    hpdf_fail((hpdf_doc_rec *)d, code);
    return NULL;
}

HPDF_Annotation HPDF_Page_Create3DAnnot(HPDF_Page page, HPDF_Rect rect,
                                        HPDF_BOOL tb, HPDF_BOOL np,
                                        HPDF_U3D u3d, HPDF_Image ap)
{
    HPDF_UNUSED(rect); HPDF_UNUSED(tb); HPDF_UNUSED(np);
    HPDF_UNUSED(u3d); HPDF_UNUSED(ap);
    return hpdf_unsupported_annot(hpdf_doc_of_page(page), HPDF_UNSUPPORTED_FUNC);
}

HPDF_Annotation HPDF_Page_CreateFreeTextAnnot(HPDF_Page page, HPDF_Rect rect,
                                              const char *text,
                                              HPDF_Encoder encoder)
{
    HPDF_UNUSED(rect); HPDF_UNUSED(text); HPDF_UNUSED(encoder);
    return hpdf_unsupported_annot(hpdf_doc_of_page(page), HPDF_UNSUPPORTED_FUNC);
}

HPDF_Annotation HPDF_Page_CreateLineAnnot(HPDF_Page page, const char *text,
                                          HPDF_Encoder encoder)
{
    HPDF_UNUSED(text); HPDF_UNUSED(encoder);
    return hpdf_unsupported_annot(hpdf_doc_of_page(page), HPDF_UNSUPPORTED_FUNC);
}

HPDF_Annotation HPDF_Page_CreateWidgetAnnot_WhiteOnlyWhilePrint(
    HPDF_Doc pdf, HPDF_Page page, HPDF_Rect rect)
{
    HPDF_UNUSED(page); HPDF_UNUSED(rect);
    return hpdf_unsupported_annot(pdf, HPDF_UNSUPPORTED_FUNC);
}

HPDF_Annotation HPDF_Page_CreateWidgetAnnot(HPDF_Page page, HPDF_Rect rect)
{
    HPDF_UNUSED(rect);
    return hpdf_unsupported_annot(hpdf_doc_of_page(page), HPDF_UNSUPPORTED_FUNC);
}

HPDF_Annotation HPDF_Page_CreateTextMarkupAnnot(HPDF_Page page,
                                                HPDF_Rect rect,
                                                const char *text,
                                                HPDF_Encoder encoder,
                                                HPDF_AnnotType subType)
{
    HPDF_UNUSED(rect); HPDF_UNUSED(text); HPDF_UNUSED(encoder);
    HPDF_UNUSED(subType);
    return hpdf_unsupported_annot(hpdf_doc_of_page(page), HPDF_UNSUPPORTED_FUNC);
}

HPDF_Annotation HPDF_Page_CreatePopupAnnot(HPDF_Page page, HPDF_Rect rect,
                                           HPDF_Annotation parent)
{
    HPDF_UNUSED(rect); HPDF_UNUSED(parent);
    return hpdf_unsupported_annot(hpdf_doc_of_page(page), HPDF_UNSUPPORTED_FUNC);
}

HPDF_Annotation HPDF_Page_CreateStampAnnot(HPDF_Page page, HPDF_Rect rect,
                                           HPDF_StampAnnotName name,
                                           const char *text,
                                           HPDF_Encoder encoder)
{
    HPDF_UNUSED(rect); HPDF_UNUSED(name); HPDF_UNUSED(text); HPDF_UNUSED(encoder);
    return hpdf_unsupported_annot(hpdf_doc_of_page(page), HPDF_UNSUPPORTED_FUNC);
}

HPDF_Annotation HPDF_Page_CreateProjectionAnnot(HPDF_Page page,
                                                HPDF_Rect rect,
                                                const char *text,
                                                HPDF_Encoder encoder)
{
    HPDF_UNUSED(rect); HPDF_UNUSED(text); HPDF_UNUSED(encoder);
    return hpdf_unsupported_annot(hpdf_doc_of_page(page), HPDF_UNSUPPORTED_FUNC);
}

HPDF_XObject HPDF_Page_CreateXObjectFromImage(HPDF_Doc pdf, HPDF_Page page,
                                              HPDF_Rect rect, HPDF_Image image,
                                              HPDF_BOOL zoom)
{
    HPDF_UNUSED(page); HPDF_UNUSED(rect); HPDF_UNUSED(image); HPDF_UNUSED(zoom);
    hpdf_fail((hpdf_doc_rec *)pdf, HPDF_UNSUPPORTED_FUNC);
    return NULL;
}

HPDF_XObject HPDF_Page_CreateXObjectAsWhiteRect(HPDF_Doc pdf, HPDF_Page page,
                                                HPDF_Rect rect)
{
    HPDF_UNUSED(page); HPDF_UNUSED(rect);
    hpdf_fail((hpdf_doc_rec *)pdf, HPDF_UNSUPPORTED_FUNC);
    return NULL;
}

HPDF_STATUS HPDF_Page_ExecuteXObject(HPDF_Page page, HPDF_XObject obj)
{
    HPDF_UNUSED(obj);
    hpdf_fail(hpdf_doc_of_page(page), HPDF_UNSUPPORTED_FUNC);
    return HPDF_UNSUPPORTED_FUNC;
}

HPDF_STATUS HPDF_Image_AddSMask(HPDF_Image image, HPDF_Image smask)
{
    HPDF_UNUSED(image); HPDF_UNUSED(smask);
    return HPDF_UNSUPPORTED_FUNC;
}

HPDF_STATUS HPDF_Image_SetColorMask(HPDF_Image image, HPDF_UINT rmin,
                                    HPDF_UINT rmax, HPDF_UINT gmin,
                                    HPDF_UINT gmax, HPDF_UINT bmin,
                                    HPDF_UINT bmax)
{
    HPDF_UNUSED(image); HPDF_UNUSED(rmin); HPDF_UNUSED(rmax);
    HPDF_UNUSED(gmin); HPDF_UNUSED(gmax); HPDF_UNUSED(bmin); HPDF_UNUSED(bmax);
    return HPDF_UNSUPPORTED_FUNC;
}

HPDF_STATUS HPDF_Image_SetMaskImage(HPDF_Image image, HPDF_Image mask_image)
{
    HPDF_UNUSED(image); HPDF_UNUSED(mask_image);
    return HPDF_UNSUPPORTED_FUNC;
}

HPDF_Image HPDF_Image_LoadRaw1BitImageFromMem(HPDF_Doc pdf,
                                              const HPDF_BYTE *buf,
                                              HPDF_UINT width, HPDF_UINT height,
                                              HPDF_UINT line_width,
                                              HPDF_BOOL black_is1,
                                              HPDF_BOOL top_is_first)
{
    HPDF_UNUSED(buf); HPDF_UNUSED(width); HPDF_UNUSED(height);
    HPDF_UNUSED(line_width); HPDF_UNUSED(black_is1); HPDF_UNUSED(top_is_first);
    hpdf_fail((hpdf_doc_rec *)pdf, HPDF_UNSUPPORTED_FUNC);
    return NULL;
}

HPDF_Image HPDF_LoadRawImageFromFile(HPDF_Doc pdf, const char *filename,
                                     HPDF_UINT width, HPDF_UINT height,
                                     HPDF_ColorSpace color_space)
{
    HPDF_UNUSED(filename); HPDF_UNUSED(width); HPDF_UNUSED(height);
    HPDF_UNUSED(color_space);
    hpdf_fail((hpdf_doc_rec *)pdf, HPDF_UNSUPPORTED_FUNC);
    return NULL;
}

HPDF_Image HPDF_LoadRawImageFromMem(HPDF_Doc pdf, const HPDF_BYTE *buf,
                                    HPDF_UINT width, HPDF_UINT height,
                                    HPDF_ColorSpace color_space,
                                    HPDF_UINT bits_per_component)
{
    HPDF_UNUSED(buf); HPDF_UNUSED(width); HPDF_UNUSED(height);
    HPDF_UNUSED(color_space); HPDF_UNUSED(bits_per_component);
    hpdf_fail((hpdf_doc_rec *)pdf, HPDF_UNSUPPORTED_FUNC);
    return NULL;
}

HPDF_Image HPDF_LoadU3DFromFile(HPDF_Doc pdf, const char *filename)
{
    HPDF_UNUSED(filename);
    hpdf_fail((hpdf_doc_rec *)pdf, HPDF_UNSUPPORTED_FUNC);
    return NULL;
}

HPDF_Image HPDF_LoadU3DFromMem(HPDF_Doc pdf, const HPDF_BYTE *buffer,
                               HPDF_UINT size)
{
    HPDF_UNUSED(buffer); HPDF_UNUSED(size);
    hpdf_fail((hpdf_doc_rec *)pdf, HPDF_UNSUPPORTED_FUNC);
    return NULL;
}

HPDF_EmbeddedFile HPDF_AttachFile(HPDF_Doc pdf, const char *file)
{
    HPDF_UNUSED(file);
    hpdf_fail((hpdf_doc_rec *)pdf, HPDF_UNSUPPORTED_FUNC);
    return NULL;
}

HPDF_STATUS HPDF_AddPageLabel(HPDF_Doc pdf, HPDF_UINT page_num,
                              HPDF_PageNumStyle style,
                              HPDF_UINT first_page, const char *prefix)
{
    HPDF_UNUSED(page_num); HPDF_UNUSED(style);
    HPDF_UNUSED(first_page); HPDF_UNUSED(prefix);
    hpdf_fail((hpdf_doc_rec *)pdf, HPDF_UNSUPPORTED_FUNC);
    return HPDF_UNSUPPORTED_FUNC;
}

HPDF_STATUS HPDF_Page_SetSlideShow(HPDF_Page page, HPDF_TransitionStyle type,
                                   HPDF_REAL disp_time,
                                   HPDF_REAL trans_time)
{
    HPDF_UNUSED(type); HPDF_UNUSED(disp_time); HPDF_UNUSED(trans_time);
    hpdf_fail(hpdf_doc_of_page(page), HPDF_UNSUPPORTED_FUNC);
    return HPDF_UNSUPPORTED_FUNC;
}

HPDF_STATUS HPDF_Page_SetShading(HPDF_Page page, HPDF_Shading shading)
{
    HPDF_UNUSED(shading);
    hpdf_fail(hpdf_doc_of_page(page), HPDF_UNSUPPORTED_FUNC);
    return HPDF_UNSUPPORTED_FUNC;
}

HPDF_Shading HPDF_Shading_New(HPDF_Doc pdf, HPDF_ShadingType type,
                              HPDF_ColorSpace colorSpace, HPDF_REAL xMin,
                              HPDF_REAL xMax, HPDF_REAL yMin, HPDF_REAL yMax)
{
    HPDF_UNUSED(type); HPDF_UNUSED(colorSpace); HPDF_UNUSED(xMin);
    HPDF_UNUSED(xMax); HPDF_UNUSED(yMin); HPDF_UNUSED(yMax);
    hpdf_fail((hpdf_doc_rec *)pdf, HPDF_UNSUPPORTED_FUNC);
    return NULL;
}

HPDF_STATUS HPDF_Shading_AddVertexRGB(
    HPDF_Shading shading, HPDF_Shading_FreeFormTriangleMeshEdgeFlag edgeFlag,
    HPDF_REAL x, HPDF_REAL y, HPDF_UINT8 r, HPDF_UINT8 g, HPDF_UINT8 b)
{
    HPDF_UNUSED(shading); HPDF_UNUSED(edgeFlag); HPDF_UNUSED(x);
    HPDF_UNUSED(y); HPDF_UNUSED(r); HPDF_UNUSED(g); HPDF_UNUSED(b);
    return HPDF_UNSUPPORTED_FUNC;
}

HPDF_STATUS HPDF_Page_New_Content_Stream(HPDF_Page page, HPDF_Dict *new_stream)
{
    HPDF_UNUSED(new_stream);
    hpdf_fail(hpdf_doc_of_page(page), HPDF_UNSUPPORTED_FUNC);
    return HPDF_UNSUPPORTED_FUNC;
}

HPDF_STATUS HPDF_Page_Insert_Shared_Content_Stream(HPDF_Page page,
                                                   HPDF_Dict shared_stream)
{
    HPDF_UNUSED(shared_stream);
    hpdf_fail(hpdf_doc_of_page(page), HPDF_UNSUPPORTED_FUNC);
    return HPDF_UNSUPPORTED_FUNC;
}

/* 3D measure / exdata / view: not modelled. */
HPDF_3DMeasure HPDF_Page_Create3DC3DMeasure(HPDF_Page page,
                                            HPDF_Point3D firstanchorpoint,
                                            HPDF_Point3D textanchorpoint)
{
    HPDF_UNUSED(firstanchorpoint); HPDF_UNUSED(textanchorpoint);
    hpdf_fail(hpdf_doc_of_page(page), HPDF_UNSUPPORTED_FUNC);
    return NULL;
}

HPDF_3DMeasure HPDF_Page_CreatePD33DMeasure(
    HPDF_Page page, HPDF_Point3D annotationPlaneNormal,
    HPDF_Point3D firstAnchorPoint, HPDF_Point3D secondAnchorPoint,
    HPDF_Point3D leaderLinesDirection, HPDF_Point3D measurementValuePoint,
    HPDF_Point3D textYDirection, HPDF_REAL value, const char *unitsString)
{
    HPDF_UNUSED(annotationPlaneNormal); HPDF_UNUSED(firstAnchorPoint);
    HPDF_UNUSED(secondAnchorPoint); HPDF_UNUSED(leaderLinesDirection);
    HPDF_UNUSED(measurementValuePoint); HPDF_UNUSED(textYDirection);
    HPDF_UNUSED(value); HPDF_UNUSED(unitsString);
    hpdf_fail(hpdf_doc_of_page(page), HPDF_UNSUPPORTED_FUNC);
    return NULL;
}

HPDF_STATUS HPDF_3DMeasure_SetName(HPDF_3DMeasure measure, const char *name)
{
    HPDF_UNUSED(measure); HPDF_UNUSED(name);
    return HPDF_UNSUPPORTED_FUNC;
}

HPDF_STATUS HPDF_3DMeasure_SetColor(HPDF_3DMeasure measure,
                                    HPDF_RGBColor color)
{
    HPDF_UNUSED(measure); HPDF_UNUSED(color);
    return HPDF_UNSUPPORTED_FUNC;
}

HPDF_STATUS HPDF_3DMeasure_SetTextSize(HPDF_3DMeasure measure,
                                       HPDF_REAL textsize)
{
    HPDF_UNUSED(measure); HPDF_UNUSED(textsize);
    return HPDF_UNSUPPORTED_FUNC;
}

HPDF_STATUS HPDF_3DC3DMeasure_SetTextBoxSize(HPDF_3DMeasure measure,
                                             HPDF_INT32 x, HPDF_INT32 y)
{
    HPDF_UNUSED(measure); HPDF_UNUSED(x); HPDF_UNUSED(y);
    return HPDF_UNSUPPORTED_FUNC;
}

HPDF_STATUS HPDF_3DC3DMeasure_SetText(HPDF_3DMeasure measure, const char *text,
                                      HPDF_Encoder encoder)
{
    HPDF_UNUSED(measure); HPDF_UNUSED(text); HPDF_UNUSED(encoder);
    return HPDF_UNSUPPORTED_FUNC;
}

HPDF_STATUS HPDF_3DC3DMeasure_SetProjectionAnotation(
    HPDF_3DMeasure measure, HPDF_Annotation projectionanotation)
{
    HPDF_UNUSED(measure); HPDF_UNUSED(projectionanotation);
    return HPDF_UNSUPPORTED_FUNC;
}

HPDF_ExData HPDF_Page_Create3DAnnotExData(HPDF_Page page)
{
    hpdf_fail(hpdf_doc_of_page(page), HPDF_UNSUPPORTED_FUNC);
    return NULL;
}

HPDF_STATUS HPDF_3DAnnotExData_Set3DMeasurement(HPDF_ExData exdata,
                                                HPDF_3DMeasure measure)
{
    HPDF_UNUSED(exdata); HPDF_UNUSED(measure);
    return HPDF_UNSUPPORTED_FUNC;
}

HPDF_Dict HPDF_Page_Create3DView(HPDF_Page page, HPDF_U3D u3d,
                                 HPDF_Annotation annot3d, const char *name)
{
    HPDF_UNUSED(u3d); HPDF_UNUSED(annot3d); HPDF_UNUSED(name);
    hpdf_fail(hpdf_doc_of_page(page), HPDF_UNSUPPORTED_FUNC);
    return NULL;
}

HPDF_STATUS HPDF_3DView_Add3DC3DMeasure(HPDF_Dict view,
                                        HPDF_3DMeasure measure)
{
    HPDF_UNUSED(view); HPDF_UNUSED(measure);
    return HPDF_UNSUPPORTED_FUNC;
}

HPDF_STATUS HPDF_Annot_Set3DView(HPDF_MMgr mmgr, HPDF_Annotation annot,
                                 HPDF_Annotation annot3d, HPDF_Dict view)
{
    HPDF_UNUSED(mmgr); HPDF_UNUSED(annot); HPDF_UNUSED(annot3d);
    HPDF_UNUSED(view);
    return HPDF_UNSUPPORTED_FUNC;
}

HPDF_OutputIntent HPDF_ICC_LoadIccFromMem(HPDF_Doc pdf, HPDF_MMgr mmgr,
                                          HPDF_Stream iccdata, HPDF_Xref xref,
                                          int numcomponent)
{
    HPDF_UNUSED(mmgr); HPDF_UNUSED(iccdata); HPDF_UNUSED(xref);
    HPDF_UNUSED(numcomponent);
    hpdf_fail((hpdf_doc_rec *)pdf, HPDF_UNSUPPORTED_FUNC);
    return NULL;
}

HPDF_OutputIntent HPDF_LoadIccProfileFromFile(HPDF_Doc pdf,
                                              const char *icc_file_name,
                                              int numcomponent)
{
    HPDF_UNUSED(icc_file_name); HPDF_UNUSED(numcomponent);
    hpdf_fail((hpdf_doc_rec *)pdf, HPDF_UNSUPPORTED_FUNC);
    return NULL;
}
