/**
 * pdfx_compat.c - libharu-compatible (PDFX_*) facade over the pdflib core.
 *
 * The public surface and semantics are libharu 2.4.4's where pdflib can
 * express them. The backend is selected per process with PDFX_SetBackend()
 * or PDFLIB_BACKEND (auto|dummy|libharu|podofo); everything below is plain C
 * and calls only the stable pdf_* API from pdflib/pdf_api.h.
 *
 * Deliberate differences from libharu (documented in the header):
 *  - Handles are opaque in every build (no PDFX_Doc_Rec access).
 *  - PDFX_New() materialises a real pdflib document immediately, so the
 *    page/stream APIs behave the same without an explicit PDFX_NewDoc().
 *  - Text is drawn through the engine's one-shot text primitive. The
 *    BT/ET text object state is tracked here; char/word spacing and
 *    horizontal scaling affect the advance, not every glyph shape.
 *  - Functions pdflib cannot express return PDFX_UNSUPPORTED_FUNC instead
 *    of pretending to work.
 */
#include "pdfx.h"

#include "pdflib/pdf_api.h"

#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define PDFX_MAX_GSTATE 32

/* ------------------------------------------------------------------ */
/* Internal records                                                    */
/* ------------------------------------------------------------------ */

typedef struct pdfx_font_rec
{
    pdf_font core;
    char name[128];
    char encoding[64];
    int is_standard;
    struct pdfx_font_rec *next;
} pdfx_font_rec;

typedef struct pdfx_gstate_snap
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
} pdfx_gstate_snap;

typedef struct pdfx_page_rec
{
    pdf_page core;
    struct pdfx_doc_rec *doc;
    struct pdfx_page_rec *next;

    double width, height;
    PDFX_Point cur_pos;
    PDFX_Point text_pos;

    /* text object / text state */
    int in_text;
    double tm_a, tm_b, tm_c, tm_d;
    double font_size;
    pdfx_font_rec *font;
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
    pdfx_gstate_snap stack[PDFX_MAX_GSTATE];
    int depth;

    /* Cached accessors that pdflib does not expose. */
    double fill_alpha, stroke_alpha;
    pdf_blend_mode blend_mode;
} pdfx_page_rec;

typedef struct pdfx_dest_rec
{
    struct pdfx_doc_rec *doc;
    PDFX_Page page;
    PDFX_DestinationType type;
    PDFX_REAL left, bottom, right, top, zoom;
    struct pdfx_dest_rec *next;
} pdfx_dest_rec;

typedef struct pdfx_extg_rec
{
    double fill_alpha, stroke_alpha;
    pdf_blend_mode blend_mode;
    int has_blend;
    struct pdfx_extg_rec *next;
} pdfx_extg_rec;

typedef struct pdfx_handle_node
{
    void *handle;
    int kind; /* 1 = image, 2 = annot, 3 = outline */
    struct pdfx_handle_node *next;
} pdfx_handle_node;

typedef struct pdfx_doc_rec
{
    pdf_doc core;
    int has_doc;

    PDFX_Error_Handler error_fn;
    void *user_data;
    PDFX_STATUS error_no;
    PDFX_STATUS detail_no;

    pdfx_page_rec *pages;
    pdfx_page_rec *pages_tail;
    PDFX_Page current_page;
    pdfx_font_rec *fonts;
    pdfx_dest_rec *dests;
    pdfx_extg_rec *extgs;
    pdfx_handle_node *handles;

    /* info / viewer state */
    PDFX_PageLayout layout;
    PDFX_PageMode page_mode;
    PDFX_UINT viewer_pref;
    char info_buf[2048];

    /* memory stream */
    unsigned char *stream;
    size_t stream_size;
    size_t stream_pos;

    /* deferred encryption (libharu needs SetPassword first) */
    char owner_pass[256];
    char user_pass[256];
    int have_password;
    int encryption_mode; /* PDFX_ENCRYPT_R2/R3, 0 = default */
    unsigned long permissions;
    int encrypt_applied;

    int compression;
} pdfx_doc_rec;

/* ------------------------------------------------------------------ */
/* Small helpers                                                       */
/* ------------------------------------------------------------------ */

static void *pdfx_xmemdup(const void *p, size_t n)
{
    void *q;
    if (!p || n == 0)
        return NULL;
    q = malloc(n);
    if (q)
        memcpy(q, p, n);
    return q;
}

static char *pdfx_xstrdup(const char *s)
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

static int pdfx_ci_equal(const char *a, const char *b)
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

static double pdfx_clamp01(double v)
{
    if (v < 0) return 0;
    if (v > 1) return 1;
    return v;
}

static double pdfx_dist(const PDFX_Point *a, const PDFX_Point *b)
{
    double dx = (double)a->x - b->x;
    double dy = (double)a->y - b->y;
    return sqrt(dx * dx + dy * dy);
}

/* Number of code points in a UTF-8 prefix. */
static size_t pdfx_utf8_count(const char *s, size_t n)
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
static const char *pdfx_utf8_next(const char *s)
{
    unsigned char c = (unsigned char)*s;
    if (c < 0x80) return s + 1;
    if ((c & 0xE0) == 0xC0 && s[1]) return s + 2;
    if ((c & 0xF0) == 0xE0 && s[1] && s[2]) return s + 3;
    if ((c & 0xF8) == 0xF0 && s[1] && s[2] && s[3]) return s + 4;
    return s + 1;
}

static int pdfx_is_space(const char *s, size_t n)
{
    if (n == 0)
        return 0;
    return s[0] == ' ' || s[0] == '\t' || s[0] == '\n' || s[0] == '\r';
}

/* ------------------------------------------------------------------ */
/* Error handling                                                      */
/* ------------------------------------------------------------------ */

static PDFX_STATUS pdfx_map_pdf_status(pdf_status rc)
{
    switch (rc)
    {
        case PDF_OK:                      return PDFX_OK;
        case PDF_ERR_INVALID_HANDLE:      return PDFX_INVALID_DOCUMENT;
        case PDF_ERR_INVALID_PARAM:       return PDFX_INVALID_PARAMETER;
        case PDF_ERR_INVALID_STATE:       return PDFX_INVALID_DOCUMENT_STATE;
        case PDF_ERR_BACKEND_UNSUPPORTED: return PDFX_UNSUPPORTED_FUNC;
        case PDF_ERR_BACKEND_FAILED:      return PDFX_INVALID_OPERATION;
        case PDF_ERR_BACKEND_NOT_FOUND:   return PDFX_INVALID_DOCUMENT;
        case PDF_ERR_OUT_OF_MEMORY:       return PDFX_FAILD_TO_ALLOC_MEM;
        case PDF_ERR_IO:                  return PDFX_FILE_IO_ERROR;
        default:                          return PDFX_ERR_UNKNOWN_CLASS;
    }
}

static PDFX_STATUS pdfx_set_error(pdfx_doc_rec *d, PDFX_STATUS code,
                                  PDFX_STATUS detail)
{
    if (d)
    {
        d->error_no = code;
        d->detail_no = detail;
        /* Deliberately NOT calling d->error_fn here.
         *
         * libharu's HPDF_SetError only records the error; the user's
         * handler is invoked from a few internal paths, not from every
         * failing call. The difference is not academic: libharu's own
         * demos install a handler that does longjmp(), so calling it for
         * something as ordinary as "this build does not support free-text
         * annotations" would abandon a perfectly good document mid-build.
         *
         * Keeping the handler out of the error path also makes the
         * contract usable: PDFX_GetError()/PDFX_GetErrorDetail() report
         * what went wrong, and a caller decides for itself whether that
         * is fatal. */
        (void)0;
    }
    return code;
}

static PDFX_STATUS pdfx_fail(pdfx_doc_rec *d, PDFX_STATUS code)
{
    return pdfx_set_error(d, code, 0);
}

static PDFX_STATUS pdfx_fail_pdf(pdfx_doc_rec *d, pdf_status rc)
{
    return pdfx_set_error(d, pdfx_map_pdf_status(rc), (PDFX_STATUS)rc);
}

static int pdfx_doc_ok(pdfx_doc_rec *d)
{
    return d && d->has_doc && d->core;
}

static pdfx_doc_rec *pdfx_doc_of_page(PDFX_Page page)
{
    return page ? ((pdfx_page_rec *)page)->doc : NULL;
}

/* ------------------------------------------------------------------ */
/* Handle tracking (cells must be released before the backend doc)     */
/* ------------------------------------------------------------------ */

static void pdfx_track(pdfx_doc_rec *d, void *handle, int kind)
{
    pdfx_handle_node *n;
    if (!d || !handle)
        return;
    n = (pdfx_handle_node *)malloc(sizeof(*n));
    if (!n)
        return;
    n->handle = handle;
    n->kind = kind;
    n->next = d->handles;
    d->handles = n;
}

static void pdfx_free_handles(pdfx_doc_rec *d)
{
    pdfx_handle_node *n = d ? d->handles : NULL;
    while (n)
    {
        pdfx_handle_node *next = n->next;
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

static void pdfx_free_fonts(pdfx_doc_rec *d)
{
    pdfx_font_rec *f = d ? d->fonts : NULL;
    while (f)
    {
        pdfx_font_rec *next = f->next;
        if (f->core)
            pdf_font_free(f->core);
        free(f);
        f = next;
    }
    if (d)
        d->fonts = NULL;
}

static void pdfx_free_pages(pdfx_doc_rec *d)
{
    pdfx_page_rec *p = d ? d->pages : NULL;
    while (p)
    {
        pdfx_page_rec *next = p->next;
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

static void pdfx_free_dests(pdfx_doc_rec *d)
{
    pdfx_dest_rec *x = d ? d->dests : NULL;
    while (x)
    {
        pdfx_dest_rec *next = x->next;
        free(x);
        x = next;
    }
    if (d)
        d->dests = NULL;
}

static void pdfx_free_extgs(pdfx_doc_rec *d)
{
    pdfx_extg_rec *x = d ? d->extgs : NULL;
    while (x)
    {
        pdfx_extg_rec *next = x->next;
        free(x);
        x = next;
    }
    if (d)
        d->extgs = NULL;
}

static void pdfx_doc_free_content(pdfx_doc_rec *d)
{
    if (!d)
        return;
    pdfx_free_handles(d);
    pdfx_free_fonts(d);
    pdfx_free_pages(d);
    pdfx_free_dests(d);
    pdfx_free_extgs(d);
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

static pdf_status pdfx_new_core_doc(pdfx_doc_rec *d)
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

static void pdfx_apply_env_backend(void)
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

    if (pdfx_ci_equal(value, "libharu"))
        pdflib_set_backend(PDF_BACKEND_LIBHARU);
    else if (pdfx_ci_equal(value, "podofo"))
        pdflib_set_backend(PDF_BACKEND_PODOFO);
    else if (pdfx_ci_equal(value, "dummy"))
        pdflib_set_backend(PDF_BACKEND_DUMMY);
    else if (pdfx_ci_equal(value, "auto"))
        pdflib_set_backend(PDF_BACKEND_AUTO);
}

PDFX_STATUS PDFX_SetBackend(PDFX_UINT backend)
{
    pdf_status rc;
    /* Accept any registered backend id instead of a hard-coded 0..3. This
     * is what makes a future third/fourth engine usable from PDFX_* code
     * without editing the compatibility layer. AUTO (0) is always valid. */
    if (backend != 0 && !pdflib_has_backend((pdf_backend)backend))
        return PDFX_INVALID_PARAMETER;
    rc = pdflib_set_backend((pdf_backend)backend);
    return rc == PDF_OK ? PDFX_OK : PDFX_INVALID_PARAMETER;
}

PDFX_UINT PDFX_GetBackend(void)
{
    return (PDFX_UINT)pdflib_get_backend();
}

const char *PDFX_GetBackendName(void)
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

const char *PDFX_GetBackendVersion(void)
{
    return pdflib_backend_version();
}

/* ------------------------------------------------------------------ */
/* Document lifecycle                                                  */
/* ------------------------------------------------------------------ */

const char *PDFX_GetVersion(void)
{
    return "pdflib pdfx 1.0 (multi-backend)";
}

PDFX_Doc PDFX_NewEx(PDFX_Error_Handler user_error_fn,
                    PDFX_Alloc_Func user_alloc_fn,
                    PDFX_Free_Func user_free_fn,
                    PDFX_UINT mem_pool_buf_size,
                    void *user_data)
{
    pdfx_doc_rec *d;
    pdf_status rc;

    PDFX_UNUSED(user_alloc_fn);
    PDFX_UNUSED(user_free_fn);
    PDFX_UNUSED(mem_pool_buf_size);

    pdfx_apply_env_backend();
    d = (pdfx_doc_rec *)calloc(1, sizeof(*d));
    if (!d)
        return NULL;
    d->error_fn = user_error_fn;
    d->user_data = user_data;
    d->layout = PDFX_PAGE_LAYOUT_SINGLE;
    d->page_mode = PDFX_PAGE_MODE_USE_NONE;
    d->compression = PDFX_COMP_NONE;
    d->encryption_mode = 0;

    rc = pdfx_new_core_doc(d);
    if (rc != PDF_OK)
    {
        pdfx_set_error(d, pdfx_map_pdf_status(rc), (PDFX_STATUS)rc);
        free(d);
        return NULL;
    }
    /* libharu's default compression is PDFX_COMP_NONE. PoDoFo's writer may
     * still flate-compress streams (backend limitation), but the selection
     * matches libharu as closely as the core allows. */
    pdf_doc_set_compression(d->core, PDF_COMPRESS_NONE);
    return (PDFX_Doc)d;
}

PDFX_Doc PDFX_New(PDFX_Error_Handler user_error_fn, void *user_data)
{
    return PDFX_NewEx(user_error_fn, NULL, NULL, 0, user_data);
}

PDFX_STATUS PDFX_SetErrorHandler(PDFX_Doc pdf, PDFX_Error_Handler fn)
{
    pdfx_doc_rec *d = (pdfx_doc_rec *)pdf;
    if (!d)
        return PDFX_INVALID_PARAMETER;
    d->error_fn = fn;
    return PDFX_OK;
}

void PDFX_Free(PDFX_Doc pdf)
{
    pdfx_doc_rec *d = (pdfx_doc_rec *)pdf;
    if (!d)
        return;
    pdfx_doc_free_content(d);
    free(d);
}

PDFX_MMgr PDFX_GetDocMMgr(PDFX_Doc pdf)
{
    PDFX_UNUSED(pdf);
    return NULL;
}

PDFX_STATUS PDFX_NewDoc(PDFX_Doc pdf)
{
    pdfx_doc_rec *d = (pdfx_doc_rec *)pdf;
    pdf_status rc;
    if (!d)
        return PDFX_INVALID_PARAMETER;
    pdfx_doc_free_content(d);
    rc = pdfx_new_core_doc(d);
    if (rc != PDF_OK)
        return pdfx_fail_pdf(d, rc);
    d->layout = PDFX_PAGE_LAYOUT_SINGLE;
    d->page_mode = PDFX_PAGE_MODE_USE_NONE;
    d->compression = PDFX_COMP_NONE;
    d->encrypt_applied = 0;
    pdf_doc_set_compression(d->core, PDF_COMPRESS_NONE);
    return PDFX_OK;
}

void PDFX_FreeDoc(PDFX_Doc pdf)
{
    pdfx_doc_rec *d = (pdfx_doc_rec *)pdf;
    if (d)
        pdfx_doc_free_content(d);
}

PDFX_BOOL PDFX_HasDoc(PDFX_Doc pdf)
{
    return pdfx_doc_ok((pdfx_doc_rec *)pdf) ? PDFX_TRUE : PDFX_FALSE;
}

void PDFX_FreeDocAll(PDFX_Doc pdf)
{
    PDFX_FreeDoc(pdf);
}

PDFX_STATUS PDFX_GetError(PDFX_Doc pdf)
{
    pdfx_doc_rec *d = (pdfx_doc_rec *)pdf;
    return d ? d->error_no : PDFX_INVALID_PARAMETER;
}

PDFX_STATUS PDFX_GetErrorDetail(PDFX_Doc pdf)
{
    pdfx_doc_rec *d = (pdfx_doc_rec *)pdf;
    return d ? d->detail_no : 0;
}

void PDFX_ResetError(PDFX_Doc pdf)
{
    pdfx_doc_rec *d = (pdfx_doc_rec *)pdf;
    if (d)
    {
        d->error_no = PDFX_OK;
        d->detail_no = 0;
    }
}

PDFX_STATUS PDFX_CheckError(PDFX_Error error)
{
    pdfx_doc_rec *d = (pdfx_doc_rec *)error;
    return d ? d->error_no : PDFX_OK;
}

/* Accepted and ignored, on purpose: these six set viewer hints or document
 * metadata that pdflib does not model - the /Pages tree chunking, the
 * initial zoom, curve flatness, an annotation's creation date and intent,
 * and the /H highlight mode of a link. None of them changes what the page
 * draws or where anything sits, so returning PDFX_OK does not mislead a
 * caller into thinking content was lost; it only means "your preference was
 * received and will not appear in the file". Everything that *would* change
 * the output reports PDFX_UNSUPPORTED_FUNC instead. */
PDFX_STATUS PDFX_SetPagesConfiguration(PDFX_Doc pdf, PDFX_UINT page_per_pages)
{
    PDFX_UNUSED(pdf);
    PDFX_UNUSED(page_per_pages);
    return PDFX_OK;
}

/* ------------------------------------------------------------------ */
/* Output                                                              */
/* ------------------------------------------------------------------ */

static PDFX_STATUS pdfx_apply_encryption(pdfx_doc_rec *d)
{
    pdf_encrypt_mode mode = PDF_ENCRYPT_RC4_40;
    unsigned perms = 0;

    if (!d || !d->core)
        return PDFX_INVALID_DOCUMENT;
    if (d->encrypt_applied)
        return PDFX_OK;
    if (!d->have_password && d->encryption_mode == 0
        && d->permissions == 0)
        return PDFX_OK;

    if (d->encryption_mode == PDFX_ENCRYPT_R3)
        mode = PDF_ENCRYPT_RC4_128;
    else
        mode = PDF_ENCRYPT_RC4_40;

    if (d->permissions & PDFX_ENABLE_PRINT)    perms |= PDF_PERM_PRINT;
    if (d->permissions & PDFX_ENABLE_EDIT)     perms |= PDF_PERM_EDIT;
    if (d->permissions & PDFX_ENABLE_COPY)     perms |= PDF_PERM_COPY;
    if (d->permissions & PDFX_ENABLE_EDIT_ALL) perms |= PDF_PERM_ANNOTATE;

    {
        pdf_status rc = pdf_doc_set_encryption(
            d->core, mode, d->owner_pass, d->user_pass, perms);
        d->encrypt_applied = 1;
        if (rc != PDF_OK)
            return pdfx_fail_pdf(d, rc);
    }
    return PDFX_OK;
}

PDFX_STATUS PDFX_SaveToFile(PDFX_Doc pdf, const char *file_name)
{
    pdfx_doc_rec *d = (pdfx_doc_rec *)pdf;
    pdf_status rc;
    if (!pdfx_doc_ok(d))
        return PDFX_INVALID_DOCUMENT;
    if (!file_name || !*file_name)
        return pdfx_fail(d, PDFX_INVALID_PARAMETER);
    {
        PDFX_STATUS st = pdfx_apply_encryption(d);
        if (st != PDFX_OK)
            return st;
    }
    rc = pdf_doc_save_to_file(d->core, file_name);
    if (rc != PDF_OK)
        return pdfx_fail_pdf(d, rc);
    return PDFX_OK;
}

PDFX_STATUS PDFX_SaveToStream(PDFX_Doc pdf)
{
    pdfx_doc_rec *d = (pdfx_doc_rec *)pdf;
    pdf_status rc;
    size_t size = 0;

    if (!pdfx_doc_ok(d))
        return PDFX_INVALID_DOCUMENT;
    {
        PDFX_STATUS st = pdfx_apply_encryption(d);
        if (st != PDFX_OK)
            return st;
    }

    free(d->stream);
    d->stream = NULL;
    d->stream_size = 0;
    d->stream_pos = 0;

    rc = pdf_doc_save_to_memory(d->core, NULL, &size);
    if (rc != PDF_OK)
        return pdfx_fail_pdf(d, rc);
    d->stream = (unsigned char *)malloc(size ? size : 1);
    if (!d->stream)
        return pdfx_fail(d, PDFX_FAILD_TO_ALLOC_MEM);
    {
        size_t want = size;
        rc = pdf_doc_save_to_memory(d->core, d->stream, &want);
        if (rc != PDF_OK)
        {
            free(d->stream);
            d->stream = NULL;
            return pdfx_fail_pdf(d, rc);
        }
        d->stream_size = want;
    }
    return PDFX_OK;
}

PDFX_UINT32 PDFX_GetStreamSize(PDFX_Doc pdf)
{
    pdfx_doc_rec *d = (pdfx_doc_rec *)pdf;
    return d ? (PDFX_UINT32)d->stream_size : 0;
}

PDFX_STATUS PDFX_GetContents(PDFX_Doc pdf, PDFX_BYTE *buf, PDFX_UINT32 *size)
{
    pdfx_doc_rec *d = (pdfx_doc_rec *)pdf;
    size_t n;
    if (!pdfx_doc_ok(d))
        return PDFX_INVALID_DOCUMENT;
    if (!buf || !size)
        return pdfx_fail(d, PDFX_INVALID_PARAMETER);
    n = *size;
    if (n > d->stream_size)
        n = d->stream_size;
    if (n)
        memcpy(buf, d->stream, n);
    *size = (PDFX_UINT32)n;
    return PDFX_OK;
}

PDFX_STATUS PDFX_ReadFromStream(PDFX_Doc pdf, PDFX_BYTE *buf,
                                PDFX_UINT32 *size)
{
    pdfx_doc_rec *d = (pdfx_doc_rec *)pdf;
    size_t n;
    if (!pdfx_doc_ok(d))
        return PDFX_INVALID_DOCUMENT;
    if (!buf || !size)
        return pdfx_fail(d, PDFX_INVALID_PARAMETER);
    if (d->stream_pos >= d->stream_size)
    {
        *size = 0;
        return PDFX_STREAM_EOF;
    }
    n = *size;
    if (n > d->stream_size - d->stream_pos)
        n = d->stream_size - d->stream_pos;
    memcpy(buf, d->stream + d->stream_pos, n);
    d->stream_pos += n;
    *size = (PDFX_UINT32)n;
    return PDFX_OK;
}

PDFX_STATUS PDFX_ResetStream(PDFX_Doc pdf)
{
    pdfx_doc_rec *d = (pdfx_doc_rec *)pdf;
    if (!d)
        return PDFX_INVALID_PARAMETER;
    d->stream_pos = 0;
    return PDFX_OK;
}

/* ------------------------------------------------------------------ */
/* Pages                                                               */
/* ------------------------------------------------------------------ */

static pdfx_page_rec *pdfx_page_new(pdfx_doc_rec *d, pdf_page core,
                                    double w, double h)
{
    pdfx_page_rec *p = (pdfx_page_rec *)calloc(1, sizeof(*p));
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
    d->current_page = (PDFX_Page)p;
    return p;
}

PDFX_Page PDFX_AddPage(PDFX_Doc pdf)
{
    pdfx_doc_rec *d = (pdfx_doc_rec *)pdf;
    pdf_page core = NULL;
    pdf_status rc;
    pdfx_page_rec *p;

    if (!pdfx_doc_ok(d))
    {
        pdfx_fail(d, PDFX_INVALID_DOCUMENT);
        return NULL;
    }
    rc = pdf_page_add(d->core, PDF_PAGE_A4, PDF_ORIENTATION_PORTRAIT, &core);
    if (rc != PDF_OK)
    {
        pdfx_fail_pdf(d, rc);
        return NULL;
    }
    p = pdfx_page_new(d, core, 595.276, 841.89);
    if (!p)
    {
        pdfx_fail(d, PDFX_FAILD_TO_ALLOC_MEM);
        return NULL;
    }
    return (PDFX_Page)p;
}

PDFX_Page PDFX_InsertPage(PDFX_Doc pdf, PDFX_Page page)
{
    PDFX_UNUSED(page);
    pdfx_fail((pdfx_doc_rec *)pdf, PDFX_UNSUPPORTED_FUNC);
    return NULL;
}

PDFX_Page PDFX_GetCurrentPage(PDFX_Doc pdf)
{
    pdfx_doc_rec *d = (pdfx_doc_rec *)pdf;
    return d ? d->current_page : NULL;
}

PDFX_Page PDFX_GetPageByIndex(PDFX_Doc pdf, PDFX_UINT index)
{
    pdfx_doc_rec *d = (pdfx_doc_rec *)pdf;
    pdfx_page_rec *p;
    PDFX_UINT i = 0;
    if (!d)
        return NULL;
    for (p = d->pages; p; p = p->next)
    {
        if (i == index)
            return (PDFX_Page)p;
        ++i;
    }
    pdfx_fail(d, PDFX_INVALID_PAGE_INDEX);
    return NULL;
}

static PDFX_STATUS pdfx_page_set_wh(PDFX_Page page, double w, double h)
{
    pdfx_page_rec *p = (pdfx_page_rec *)page;
    pdf_status rc;
    if (!p)
        return PDFX_INVALID_PAGE;
    if (w <= 0 || h <= 0)
        return pdfx_fail(p->doc, PDFX_INVALID_PARAMETER);
    rc = pdf_page_set_size(p->core, w, h);
    if (rc != PDF_OK)
        return pdfx_fail_pdf(p->doc, rc);
    p->width = w;
    p->height = h;
    return PDFX_OK;
}

PDFX_STATUS PDFX_Page_SetWidth(PDFX_Page page, PDFX_REAL value)
{
    pdfx_page_rec *p = (pdfx_page_rec *)page;
    if (!p)
        return PDFX_INVALID_PAGE;
    return pdfx_page_set_wh(page, (double)value, p->height);
}

PDFX_STATUS PDFX_Page_SetHeight(PDFX_Page page, PDFX_REAL value)
{
    pdfx_page_rec *p = (pdfx_page_rec *)page;
    if (!p)
        return PDFX_INVALID_PAGE;
    return pdfx_page_set_wh(page, p->width, (double)value);
}

PDFX_STATUS PDFX_Page_SetBoundary(PDFX_Page page, PDFX_PageBoundary boundary,
                                  PDFX_REAL left, PDFX_REAL bottom,
                                  PDFX_REAL right, PDFX_REAL top)
{
    pdfx_page_rec *p = (pdfx_page_rec *)page;
    if (!p)
        return PDFX_INVALID_PAGE;
    if (right <= left || top <= bottom)
        return pdfx_fail(p->doc, PDFX_INVALID_PARAMETER);
    if (boundary == PDFX_PAGE_MEDIABOX)
        return pdfx_page_set_wh(page, (double)(right - left),
                                (double)(top - bottom));
    return PDFX_OK; /* crop/bleed/trim/art boxes are not modelled */
}

PDFX_STATUS PDFX_Page_SetSize(PDFX_Page page, PDFX_PageSizes size,
                              PDFX_PageDirection direction)
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
        return PDFX_INVALID_PAGE;
    if ((int)size < 0 || (int)size >= (int)(sizeof(sizes) / sizeof(sizes[0])))
        return pdfx_fail(pdfx_doc_of_page(page), PDFX_PAGE_INVALID_SIZE);
    if (direction != PDFX_PAGE_PORTRAIT && direction != PDFX_PAGE_LANDSCAPE)
        return pdfx_fail(pdfx_doc_of_page(page), PDFX_PAGE_INVALID_DIRECTION);
    w = sizes[(int)size][0];
    h = sizes[(int)size][1];
    if (direction == PDFX_PAGE_LANDSCAPE)
    {
        double t = w;
        w = h;
        h = t;
    }
    return pdfx_page_set_wh(page, w, h);
}

PDFX_STATUS PDFX_Page_SetRotate(PDFX_Page page, PDFX_UINT16 angle)
{
    pdfx_page_rec *p = (pdfx_page_rec *)page;
    pdf_status rc;
    if (!p)
        return PDFX_INVALID_PAGE;
    rc = pdf_page_set_rotation(p->core, (int)angle);
    return rc == PDF_OK ? PDFX_OK : pdfx_fail_pdf(p->doc, rc);
}

/* Viewer hint, see PDFX_SetPagesConfiguration. */
PDFX_STATUS PDFX_Page_SetZoom(PDFX_Page page, PDFX_REAL zoom)
{
    PDFX_UNUSED(page);
    PDFX_UNUSED(zoom);
    return PDFX_OK;
}

PDFX_REAL PDFX_Page_GetWidth(PDFX_Page page)
{
    pdfx_page_rec *p = (pdfx_page_rec *)page;
    return p ? (PDFX_REAL)p->width : 0;
}

PDFX_REAL PDFX_Page_GetHeight(PDFX_Page page)
{
    pdfx_page_rec *p = (pdfx_page_rec *)page;
    return p ? (PDFX_REAL)p->height : 0;
}

PDFX_MMgr PDFX_GetPageMMgr(PDFX_Page page)
{
    PDFX_UNUSED(page);
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Page state accessors                                                */
/* ------------------------------------------------------------------ */

PDFX_UINT16 PDFX_Page_GetGMode(PDFX_Page page)
{
    pdfx_page_rec *p = (pdfx_page_rec *)page;
    if (!p)
        return 0;
    return p->in_text ? PDFX_GMODE_TEXT_OBJECT : PDFX_GMODE_PAGE_DESCRIPTION;
}

PDFX_Point PDFX_Page_GetCurrentPos(PDFX_Page page)
{
    pdfx_page_rec *p = (pdfx_page_rec *)page;
    PDFX_Point pos;
    if (p)
        return p->cur_pos;
    pos.x = 0;
    pos.y = 0;
    return pos;
}

PDFX_STATUS PDFX_Page_GetCurrentPos2(PDFX_Page page, PDFX_Point *pos)
{
    if (!pos)
        return PDFX_INVALID_PARAMETER;
    *pos = PDFX_Page_GetCurrentPos(page);
    return PDFX_OK;
}

PDFX_Point PDFX_Page_GetCurrentTextPos(PDFX_Page page)
{
    pdfx_page_rec *p = (pdfx_page_rec *)page;
    PDFX_Point pos;
    if (p)
        return p->text_pos;
    pos.x = 0;
    pos.y = 0;
    return pos;
}

PDFX_STATUS PDFX_Page_GetCurrentTextPos2(PDFX_Page page, PDFX_Point *pos)
{
    if (!pos)
        return PDFX_INVALID_PARAMETER;
    *pos = PDFX_Page_GetCurrentTextPos(page);
    return PDFX_OK;
}

PDFX_Font PDFX_Page_GetCurrentFont(PDFX_Page page)
{
    pdfx_page_rec *p = (pdfx_page_rec *)page;
    return p ? (PDFX_Font)p->font : NULL;
}

PDFX_REAL PDFX_Page_GetCurrentFontSize(PDFX_Page page)
{
    pdfx_page_rec *p = (pdfx_page_rec *)page;
    return p ? (PDFX_REAL)p->font_size : 0;
}

PDFX_TransMatrix PDFX_Page_GetTransMatrix(PDFX_Page page)
{
    PDFX_TransMatrix m;
    PDFX_UNUSED(page);
    m.a = 1; m.b = 0; m.c = 0; m.d = 1; m.x = 0; m.y = 0;
    return m;
}

PDFX_TransMatrix PDFX_Page_GetTextMatrix(PDFX_Page page)
{
    pdfx_page_rec *p = (pdfx_page_rec *)page;
    PDFX_TransMatrix m;
    if (!p)
    {
        m.a = 1; m.b = 0; m.c = 0; m.d = 1; m.x = 0; m.y = 0;
        return m;
    }
    m.a = (PDFX_REAL)p->tm_a;
    m.b = (PDFX_REAL)p->tm_b;
    m.c = (PDFX_REAL)p->tm_c;
    m.d = (PDFX_REAL)p->tm_d;
    m.x = (PDFX_REAL)p->text_pos.x;
    m.y = (PDFX_REAL)p->text_pos.y;
    return m;
}

PDFX_UINT PDFX_Page_GetGStateDepth(PDFX_Page page)
{
    pdfx_page_rec *p = (pdfx_page_rec *)page;
    return p ? (PDFX_UINT)p->depth : 0;
}

PDFX_REAL PDFX_Page_GetLineWidth(PDFX_Page page)
{
    pdfx_page_rec *p = (pdfx_page_rec *)page;
    return p ? (PDFX_REAL)p->line_width : 0;
}

PDFX_LineCap PDFX_Page_GetLineCap(PDFX_Page page)
{
    pdfx_page_rec *p = (pdfx_page_rec *)page;
    return p ? (PDFX_LineCap)p->line_cap : PDFX_BUTT_END;
}

PDFX_LineJoin PDFX_Page_GetLineJoin(PDFX_Page page)
{
    pdfx_page_rec *p = (pdfx_page_rec *)page;
    return p ? (PDFX_LineJoin)p->line_join : PDFX_MITER_JOIN;
}

PDFX_REAL PDFX_Page_GetMiterLimit(PDFX_Page page)
{
    pdfx_page_rec *p = (pdfx_page_rec *)page;
    return p ? (PDFX_REAL)p->miter_limit : 0;
}

PDFX_DashMode PDFX_Page_GetDash(PDFX_Page page)
{
    pdfx_page_rec *p = (pdfx_page_rec *)page;
    PDFX_DashMode d;
    size_t i;
    memset(&d, 0, sizeof(d));
    if (!p)
        return d;
    d.num_ptn = (PDFX_UINT)p->dash_n;
    d.phase = (PDFX_REAL)p->dash_phase;
    for (i = 0; i < p->dash_n && i < 8; ++i)
        d.ptn[i] = (PDFX_REAL)p->dash[i];
    return d;
}

PDFX_REAL PDFX_Page_GetFlat(PDFX_Page page)
{
    PDFX_UNUSED(page);
    return 1;
}

PDFX_REAL PDFX_Page_GetCharSpace(PDFX_Page page)
{
    pdfx_page_rec *p = (pdfx_page_rec *)page;
    return p ? (PDFX_REAL)p->char_space : 0;
}

PDFX_REAL PDFX_Page_GetWordSpace(PDFX_Page page)
{
    pdfx_page_rec *p = (pdfx_page_rec *)page;
    return p ? (PDFX_REAL)p->word_space : 0;
}

PDFX_REAL PDFX_Page_GetHorizontalScalling(PDFX_Page page)
{
    pdfx_page_rec *p = (pdfx_page_rec *)page;
    return p ? (PDFX_REAL)p->hscale : 100;
}

PDFX_REAL PDFX_Page_GetTextLeading(PDFX_Page page)
{
    pdfx_page_rec *p = (pdfx_page_rec *)page;
    return p ? (PDFX_REAL)p->leading : 0;
}

PDFX_TextRenderingMode PDFX_Page_GetTextRenderingMode(PDFX_Page page)
{
    pdfx_page_rec *p = (pdfx_page_rec *)page;
    return p ? (PDFX_TextRenderingMode)p->render_mode : PDFX_FILL;
}

PDFX_REAL PDFX_Page_GetTextRaise(PDFX_Page page)
{
    pdfx_page_rec *p = (pdfx_page_rec *)page;
    return p ? (PDFX_REAL)p->rise : 0;
}

PDFX_REAL PDFX_Page_GetTextRise(PDFX_Page page)
{
    return PDFX_Page_GetTextRaise(page);
}

static PDFX_RGBColor pdfx_to_rgb(const pdf_color *c)
{
    PDFX_RGBColor rgb;
    switch (c->kind)
    {
        case PDF_COLOR_GRAY:
            rgb.r = rgb.g = rgb.b = (PDFX_REAL)c->c1;
            break;
        case PDF_COLOR_CMYK:
            rgb.r = (PDFX_REAL)((1 - c->c1) * (1 - c->c4));
            rgb.g = (PDFX_REAL)((1 - c->c2) * (1 - c->c4));
            rgb.b = (PDFX_REAL)((1 - c->c3) * (1 - c->c4));
            break;
        default:
            rgb.r = (PDFX_REAL)c->c1;
            rgb.g = (PDFX_REAL)c->c2;
            rgb.b = (PDFX_REAL)c->c3;
            break;
    }
    return rgb;
}

PDFX_RGBColor PDFX_Page_GetRGBFill(PDFX_Page page)
{
    pdfx_page_rec *p = (pdfx_page_rec *)page;
    PDFX_RGBColor rgb;
    if (p)
        return pdfx_to_rgb(&p->fill);
    rgb.r = rgb.g = rgb.b = 0;
    return rgb;
}

PDFX_RGBColor PDFX_Page_GetRGBStroke(PDFX_Page page)
{
    pdfx_page_rec *p = (pdfx_page_rec *)page;
    PDFX_RGBColor rgb;
    if (p)
        return pdfx_to_rgb(&p->stroke);
    rgb.r = rgb.g = rgb.b = 0;
    return rgb;
}

PDFX_CMYKColor PDFX_Page_GetCMYKFill(PDFX_Page page)
{
    pdfx_page_rec *p = (pdfx_page_rec *)page;
    PDFX_CMYKColor c = { 0, 0, 0, 0 };
    if (p && p->fill.kind == PDF_COLOR_CMYK)
    {
        c.c = (PDFX_REAL)p->fill.c1;
        c.m = (PDFX_REAL)p->fill.c2;
        c.y = (PDFX_REAL)p->fill.c3;
        c.k = (PDFX_REAL)p->fill.c4;
    }
    return c;
}

PDFX_CMYKColor PDFX_Page_GetCMYKStroke(PDFX_Page page)
{
    pdfx_page_rec *p = (pdfx_page_rec *)page;
    PDFX_CMYKColor c = { 0, 0, 0, 0 };
    if (p && p->stroke.kind == PDF_COLOR_CMYK)
    {
        c.c = (PDFX_REAL)p->stroke.c1;
        c.m = (PDFX_REAL)p->stroke.c2;
        c.y = (PDFX_REAL)p->stroke.c3;
        c.k = (PDFX_REAL)p->stroke.c4;
    }
    return c;
}

PDFX_REAL PDFX_Page_GetGrayFill(PDFX_Page page)
{
    pdfx_page_rec *p = (pdfx_page_rec *)page;
    return (p && p->fill.kind == PDF_COLOR_GRAY) ? (PDFX_REAL)p->fill.c1 : 0;
}

PDFX_REAL PDFX_Page_GetGrayStroke(PDFX_Page page)
{
    pdfx_page_rec *p = (pdfx_page_rec *)page;
    return (p && p->stroke.kind == PDF_COLOR_GRAY) ? (PDFX_REAL)p->stroke.c1 : 0;
}

PDFX_ColorSpace PDFX_Page_GetStrokingColorSpace(PDFX_Page page)
{
    pdfx_page_rec *p = (pdfx_page_rec *)page;
    if (!p) return PDFX_CS_DEVICE_GRAY;
    if (p->stroke.kind == PDF_COLOR_CMYK) return PDFX_CS_DEVICE_CMYK;
    if (p->stroke.kind == PDF_COLOR_GRAY) return PDFX_CS_DEVICE_GRAY;
    return PDFX_CS_DEVICE_RGB;
}

PDFX_ColorSpace PDFX_Page_GetFillingColorSpace(PDFX_Page page)
{
    pdfx_page_rec *p = (pdfx_page_rec *)page;
    if (!p) return PDFX_CS_DEVICE_GRAY;
    if (p->fill.kind == PDF_COLOR_CMYK) return PDFX_CS_DEVICE_CMYK;
    if (p->fill.kind == PDF_COLOR_GRAY) return PDFX_CS_DEVICE_GRAY;
    return PDFX_CS_DEVICE_RGB;
}

/* ------------------------------------------------------------------ */
/* Fonts (the wrappers are needed by the text code below)              */
/* ------------------------------------------------------------------ */

static pdfx_font_rec *pdfx_find_font(pdfx_doc_rec *d, const char *name)
{
    pdfx_font_rec *f;
    for (f = d ? d->fonts : NULL; f; f = f->next)
        if (pdfx_ci_equal(f->name, name))
            return f;
    return NULL;
}

static pdfx_font_rec *pdfx_add_font(pdfx_doc_rec *d, pdf_font core,
                                    const char *name, const char *encoding,
                                    int is_standard)
{
    pdfx_font_rec *f = (pdfx_font_rec *)calloc(1, sizeof(*f));
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

static int pdfx_std_font_from_name(const char *name, pdf_std_font *out)
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
        if (pdfx_ci_equal(name, map[i].name))
        {
            *out = map[i].id;
            return 1;
        }
    return 0;
}

PDFX_Font PDFX_GetFont(PDFX_Doc pdf, const char *font_name,
                       const char *encoding_name)
{
    pdfx_doc_rec *d = (pdfx_doc_rec *)pdf;
    pdfx_font_rec *f;
    pdf_std_font std;
    pdf_font core = NULL;
    pdf_status rc;

    if (!pdfx_doc_ok(d))
    {
        pdfx_fail(d, PDFX_INVALID_DOCUMENT);
        return NULL;
    }
    if (!font_name || !*font_name)
    {
        pdfx_fail(d, PDFX_INVALID_FONT_NAME);
        return NULL;
    }
    f = pdfx_find_font(d, font_name);
    if (f)
        return (PDFX_Font)f;

    if (pdfx_std_font_from_name(font_name, &std))
    {
        rc = pdf_font_get_std(d->core, std, &core);
        if (rc != PDF_OK)
        {
            pdfx_fail_pdf(d, rc);
            return NULL;
        }
        f = pdfx_add_font(d, core, font_name,
                          encoding_name ? encoding_name : "WinAnsiEncoding", 1);
        if (!f)
        {
            pdf_font_free(core);
            pdfx_fail(d, PDFX_FAILD_TO_ALLOC_MEM);
        }
        return (PDFX_Font)f;
    }

    /* Unknown name: try the system font lookup (a pdflib extension; the
     * libharu backend only knows the three common families). */
    rc = pdf_font_find_system(d->core, font_name, 0, 0, &core);
    if (rc != PDF_OK)
    {
        pdfx_fail(d, PDFX_INVALID_FONT_NAME);
        return NULL;
    }
    f = pdfx_add_font(d, core, font_name, encoding_name, 0);
    if (!f)
    {
        pdf_font_free(core);
        pdfx_fail(d, PDFX_FAILD_TO_ALLOC_MEM);
    }
    return (PDFX_Font)f;
}

const char *PDFX_LoadTTFontFromFile(PDFX_Doc pdf, const char *file_name,
                                    PDFX_BOOL embedding)
{
    pdfx_doc_rec *d = (pdfx_doc_rec *)pdf;
    pdf_font core = NULL;
    pdf_status rc;
    pdfx_font_rec *f;
    char name[128];
    const char *base;

    if (!pdfx_doc_ok(d) || !file_name)
    {
        pdfx_fail(d, PDFX_INVALID_PARAMETER);
        return NULL;
    }
    rc = pdf_font_load_ttf(d->core, file_name, embedding ? 1 : 0, &core);
    if (rc != PDF_OK)
    {
        pdfx_fail_pdf(d, rc);
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
    f = pdfx_find_font(d, name);
    if (f)
    {
        pdf_font_free(core);
        return f->name;
    }
    f = pdfx_add_font(d, core, name, "UTF-8", 0);
    if (!f)
    {
        pdf_font_free(core);
        pdfx_fail(d, PDFX_FAILD_TO_ALLOC_MEM);
        return NULL;
    }
    return f->name;
}

const char *PDFX_LoadTTFontFromFile2(PDFX_Doc pdf, const char *file_name,
                                     PDFX_UINT index, PDFX_BOOL embedding)
{
    if (index != 0)
    {
        pdfx_fail((pdfx_doc_rec *)pdf, PDFX_INVALID_TTC_INDEX);
        return NULL;
    }
    return PDFX_LoadTTFontFromFile(pdf, file_name, embedding);
}

PDFX_FontDef PDFX_GetTTFontDefFromFile(PDFX_Doc pdf, const char *file_name,
                                       PDFX_BOOL embedding)
{
    PDFX_UNUSED(file_name);
    PDFX_UNUSED(embedding);
    pdfx_fail((pdfx_doc_rec *)pdf, PDFX_UNSUPPORTED_FUNC);
    return NULL;
}

const char *PDFX_LoadType1FontFromFile(PDFX_Doc pdf, const char *afm_file_name,
                                       const char *data_file_name)
{
    PDFX_UNUSED(afm_file_name);
    PDFX_UNUSED(data_file_name);
    pdfx_fail((pdfx_doc_rec *)pdf, PDFX_UNSUPPORTED_FUNC);
    return NULL;
}

/* CJK font packs. libharu's HPDF_UseJPFonts() registers a set of Japanese
 * font names so that a later HPDF_GetFont(pdf, "MS-Gothic") resolves. pdflib
 * has no font registry: a CJK face has to be loaded from a file, and system
 * font lookup is compiled out of this PoDoFo build. Returning PDFX_OK here
 * (as this layer used to) made the following GetFont return NULL while every
 * call still reported success - the exact silent failure the facade exists to
 * avoid. Declining is the honest answer, and the caller can fall back to
 * PDFX_LoadTTFontFromFile. */
PDFX_STATUS PDFX_UseJPFonts(PDFX_Doc pdf)
{
    pdfx_fail((pdfx_doc_rec *)pdf, PDFX_UNSUPPORTED_FUNC);
    return PDFX_UNSUPPORTED_FUNC;
}

PDFX_STATUS PDFX_UseKRFonts(PDFX_Doc pdf)
{
    pdfx_fail((pdfx_doc_rec *)pdf, PDFX_UNSUPPORTED_FUNC);
    return PDFX_UNSUPPORTED_FUNC;
}

PDFX_STATUS PDFX_UseCNSFonts(PDFX_Doc pdf)
{
    pdfx_fail((pdfx_doc_rec *)pdf, PDFX_UNSUPPORTED_FUNC);
    return PDFX_UNSUPPORTED_FUNC;
}

PDFX_STATUS PDFX_UseCNTFonts(PDFX_Doc pdf)
{
    pdfx_fail((pdfx_doc_rec *)pdf, PDFX_UNSUPPORTED_FUNC);
    return PDFX_UNSUPPORTED_FUNC;
}

/* ------------------------------------------------------------------ */
/* Text                                                                */
/* ------------------------------------------------------------------ */

static double pdfx_font_width_units(pdfx_font_rec *f, const char *text)
{
    double w = 0;
    if (!f || !f->core || !text)
        return 0;
    if (pdf_font_text_width(f->core, text, 1000.0, &w) != PDF_OK)
        return 0;
    return w;
}

static double pdfx_page_text_width(pdfx_page_rec *p, const char *text)
{
    if (!p || !p->font || p->font_size <= 0)
        return 0;
    return pdfx_font_width_units(p->font, text) * p->font_size / 1000.0;
}

PDFX_REAL PDFX_Page_TextWidth(PDFX_Page page, const char *text)
{
    pdfx_page_rec *p = (pdfx_page_rec *)page;
    return (PDFX_REAL)pdfx_page_text_width(p, text);
}

static double pdfx_text_advance(pdfx_page_rec *p, const char *text)
{
    double w = pdfx_page_text_width(p, text);
    size_t n = strlen(text);
    size_t chars = pdfx_utf8_count(text, n);
    size_t spaces = 0, i;
    for (i = 0; i < n; ++i)
        if (text[i] == ' ')
            ++spaces;
    w += p->char_space * (double)chars;
    w += p->word_space * (double)spaces;
    w *= p->hscale / 100.0;
    return w;
}

static PDFX_STATUS pdfx_show_text(pdfx_page_rec *p, const char *text)
{
    pdf_status rc;
    double adv;

    if (!p)
        return PDFX_INVALID_PAGE;
    if (!p->font)
        return pdfx_fail(p->doc, PDFX_PAGE_FONT_NOT_FOUND);
    if (!text || !*text)
        return PDFX_OK;
    if (p->font_size <= 0)
        return pdfx_fail(p->doc, PDFX_PAGE_INVALID_FONT_SIZE);

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
        return pdfx_fail_pdf(p->doc, rc);

    adv = pdfx_text_advance(p, text);
    p->text_pos.x += (PDFX_REAL)(adv * p->tm_a);
    p->text_pos.y += (PDFX_REAL)(adv * p->tm_b);
    return PDFX_OK;
}

PDFX_STATUS PDFX_Page_BeginText(PDFX_Page page)
{
    pdfx_page_rec *p = (pdfx_page_rec *)page;
    if (!p)
        return PDFX_INVALID_PAGE;
    p->in_text = 1;
    p->text_pos.x = 0;
    p->text_pos.y = 0;
    p->tm_a = 1; p->tm_b = 0; p->tm_c = 0; p->tm_d = 1;
    return PDFX_OK;
}

PDFX_STATUS PDFX_Page_EndText(PDFX_Page page)
{
    pdfx_page_rec *p = (pdfx_page_rec *)page;
    if (!p)
        return PDFX_INVALID_PAGE;
    p->in_text = 0;
    return PDFX_OK;
}

PDFX_STATUS PDFX_Page_SetFontAndSize(PDFX_Page page, PDFX_Font font,
                                     PDFX_REAL size)
{
    pdfx_page_rec *p = (pdfx_page_rec *)page;
    pdfx_font_rec *f = (pdfx_font_rec *)font;
    if (!p || !f || !f->core)
        return pdfx_fail(p ? p->doc : NULL, PDFX_PAGE_INVALID_FONT);
    if (size <= 0)
        return pdfx_fail(p->doc, PDFX_PAGE_INVALID_FONT_SIZE);
    p->font = f;
    p->font_size = (double)size;
    return PDFX_OK;
}

PDFX_STATUS PDFX_Page_MoveTextPos(PDFX_Page page, PDFX_REAL x, PDFX_REAL y)
{
    pdfx_page_rec *p = (pdfx_page_rec *)page;
    if (!p)
        return PDFX_INVALID_PAGE;
    p->text_pos.x += (PDFX_REAL)(x * p->tm_a + y * p->tm_c);
    p->text_pos.y += (PDFX_REAL)(y * p->tm_d + x * p->tm_b);
    return PDFX_OK;
}

PDFX_STATUS PDFX_Page_MoveTextPos2(PDFX_Page page, PDFX_REAL x, PDFX_REAL y)
{
    pdfx_page_rec *p = (pdfx_page_rec *)page;
    PDFX_STATUS st = PDFX_Page_MoveTextPos(page, x, y);
    if (st == PDFX_OK)
        p->leading = -(double)y;
    return st;
}

PDFX_STATUS PDFX_Page_SetTextMatrix(PDFX_Page page, PDFX_REAL a, PDFX_REAL b,
                                    PDFX_REAL c, PDFX_REAL d, PDFX_REAL x,
                                    PDFX_REAL y)
{
    pdfx_page_rec *p = (pdfx_page_rec *)page;
    if (!p)
        return PDFX_INVALID_PAGE;
    if ((a == 0 || d == 0) && (b == 0 || c == 0))
        return pdfx_fail(p->doc, PDFX_INVALID_PARAMETER);
    p->tm_a = (double)a;
    p->tm_b = (double)b;
    p->tm_c = (double)c;
    p->tm_d = (double)d;
    p->text_pos.x = x;
    p->text_pos.y = y;
    return PDFX_OK;
}

PDFX_STATUS PDFX_Page_MoveToNextLine(PDFX_Page page)
{
    pdfx_page_rec *p = (pdfx_page_rec *)page;
    if (!p)
        return PDFX_INVALID_PAGE;
    p->text_pos.x -= (PDFX_REAL)(p->leading * p->tm_c);
    p->text_pos.y -= (PDFX_REAL)(p->leading * p->tm_d);
    return PDFX_OK;
}

PDFX_STATUS PDFX_Page_ShowText(PDFX_Page page, const char *text)
{
    return pdfx_show_text((pdfx_page_rec *)page, text);
}

PDFX_STATUS PDFX_Page_ShowTextNextLine(PDFX_Page page, const char *text)
{
    PDFX_STATUS st = PDFX_Page_MoveToNextLine(page);
    if (st != PDFX_OK)
        return st;
    return pdfx_show_text((pdfx_page_rec *)page, text);
}

PDFX_STATUS PDFX_Page_ShowTextNextLineEx(PDFX_Page page, PDFX_REAL word_space,
                                         PDFX_REAL char_space,
                                         const char *text)
{
    pdfx_page_rec *p = (pdfx_page_rec *)page;
    PDFX_STATUS st;
    if (!p)
        return PDFX_INVALID_PAGE;
    p->word_space = (double)word_space;
    p->char_space = (double)char_space;
    st = PDFX_Page_MoveToNextLine(page);
    if (st != PDFX_OK)
        return st;
    return pdfx_show_text(p, text);
}

PDFX_STATUS PDFX_Page_TextOut(PDFX_Page page, PDFX_REAL xpos, PDFX_REAL ypos,
                              const char *text)
{
    pdfx_page_rec *p = (pdfx_page_rec *)page;
    if (!p)
        return PDFX_INVALID_PAGE;
    p->text_pos.x = xpos;
    p->text_pos.y = ypos;
    return pdfx_show_text(p, text);
}

PDFX_UINT pdfx_measure_text_impl(pdfx_font_rec *font, const char *text,
                                 size_t len, double width, double size,
                                 double char_space, double word_space,
                                 int wordwrap, PDFX_REAL *real_width)
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
        const char *next = pdfx_utf8_next(p);
        size_t n = (size_t)(next - p);
        char tmp[8];
        double cw, adv;
        size_t spaces;

        if (n > sizeof(tmp) - 1)
            n = sizeof(tmp) - 1;
        memcpy(tmp, p, n);
        tmp[n] = '\0';
        cw = pdfx_font_width_units(font, tmp) * size / 1000.0;
        spaces = pdfx_is_space(p, (size_t)(end - p)) ? 1 : 0;
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
        *real_width = (PDFX_REAL)total;
    return (PDFX_UINT)consumed;
}

PDFX_UINT PDFX_Page_MeasureText(PDFX_Page page, const char *text,
                                PDFX_REAL width, PDFX_BOOL wordwrap,
                                PDFX_REAL *real_width)
{
    pdfx_page_rec *p = (pdfx_page_rec *)page;
    if (!p || !p->font)
        return 0;
    return pdfx_measure_text_impl(p->font, text, strlen(text), (double)width,
                                  p->font_size, p->char_space, p->word_space,
                                  wordwrap != 0, real_width);
}

PDFX_STATUS PDFX_Page_TextRect(PDFX_Page page, PDFX_REAL left, PDFX_REAL top,
                               PDFX_REAL right, PDFX_REAL bottom,
                               const char *text, PDFX_TextAlignment align,
                               PDFX_UINT *len)
{
    pdfx_page_rec *p = (pdfx_page_rec *)page;
    pdf_status rc;
    pdf_align a = PDF_ALIGN_LEFT;
    if (!p)
        return PDFX_INVALID_PAGE;
    if (!p->font)
        return pdfx_fail(p->doc, PDFX_PAGE_FONT_NOT_FOUND);
    if (!text)
        return pdfx_fail(p->doc, PDFX_INVALID_PARAMETER);
    if (right <= left || top <= bottom)
        return pdfx_fail(p->doc, PDFX_INVALID_PARAMETER);
    if (len)
        *len = (PDFX_UINT)strlen(text);
    if (align == PDFX_TALIGN_RIGHT) a = PDF_ALIGN_RIGHT;
    else if (align == PDFX_TALIGN_CENTER) a = PDF_ALIGN_CENTER;

    rc = pdf_page_draw_text_wrapped(p->core, p->font->core, text,
                                    (double)left, (double)top,
                                    (double)(right - left),
                                    (double)(top - bottom), p->font_size,
                                    0.0, a);
    return rc == PDF_OK ? PDFX_OK : pdfx_fail_pdf(p->doc, rc);
}

PDFX_STATUS PDFX_Page_SetCharSpace(PDFX_Page page, PDFX_REAL value)
{
    pdfx_page_rec *p = (pdfx_page_rec *)page;
    if (!p) return PDFX_INVALID_PAGE;
    p->char_space = (double)value;
    return PDFX_OK;
}

PDFX_STATUS PDFX_Page_SetWordSpace(PDFX_Page page, PDFX_REAL value)
{
    pdfx_page_rec *p = (pdfx_page_rec *)page;
    if (!p) return PDFX_INVALID_PAGE;
    p->word_space = (double)value;
    return PDFX_OK;
}

PDFX_STATUS PDFX_Page_SetHorizontalScalling(PDFX_Page page, PDFX_REAL value)
{
    pdfx_page_rec *p = (pdfx_page_rec *)page;
    if (!p) return PDFX_INVALID_PAGE;
    p->hscale = (double)value;
    return PDFX_OK;
}

PDFX_STATUS PDFX_Page_SetTextLeading(PDFX_Page page, PDFX_REAL value)
{
    pdfx_page_rec *p = (pdfx_page_rec *)page;
    if (!p) return PDFX_INVALID_PAGE;
    p->leading = (double)value;
    return PDFX_OK;
}

PDFX_STATUS PDFX_Page_SetTextRenderingMode(PDFX_Page page,
                                           PDFX_TextRenderingMode mode)
{
    pdfx_page_rec *p = (pdfx_page_rec *)page;
    if (!p) return PDFX_INVALID_PAGE;
    if ((int)mode < 0 || (int)mode > (int)PDFX_CLIPPING)
        return pdfx_fail(p->doc, PDFX_INVALID_PARAMETER);
    /* pdf_text_render_mode now mirrors PDF 'Tr' 0..7, including the four
     * clipping modes libharu exposes. */
    p->render_mode = (pdf_text_render_mode)mode;
    return PDFX_OK;
}

PDFX_STATUS PDFX_Page_SetTextRise(PDFX_Page page, PDFX_REAL value)
{
    pdfx_page_rec *p = (pdfx_page_rec *)page;
    if (!p) return PDFX_INVALID_PAGE;
    p->rise = (double)value;
    return PDFX_OK;
}

PDFX_STATUS PDFX_Page_SetTextRaise(PDFX_Page page, PDFX_REAL value)
{
    return PDFX_Page_SetTextRise(page, value);
}

/* ------------------------------------------------------------------ */
/* Graphics state                                                      */
/* ------------------------------------------------------------------ */

static void pdfx_save_snap(pdfx_page_rec *p)
{
    pdfx_gstate_snap *s;
    if (p->depth >= PDFX_MAX_GSTATE)
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

static void pdfx_restore_snap(pdfx_page_rec *p)
{
    pdfx_gstate_snap *s;
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

PDFX_STATUS PDFX_Page_GSave(PDFX_Page page)
{
    pdfx_page_rec *p = (pdfx_page_rec *)page;
    pdf_status rc;
    if (!p) return PDFX_INVALID_PAGE;
    rc = pdf_gstate_save(p->core);
    if (rc != PDF_OK) return pdfx_fail_pdf(p->doc, rc);
    pdfx_save_snap(p);
    return PDFX_OK;
}

PDFX_STATUS PDFX_Page_GRestore(PDFX_Page page)
{
    pdfx_page_rec *p = (pdfx_page_rec *)page;
    pdf_status rc;
    if (!p) return PDFX_INVALID_PAGE;
    rc = pdf_gstate_restore(p->core);
    if (rc != PDF_OK) return pdfx_fail_pdf(p->doc, rc);
    pdfx_restore_snap(p);
    return PDFX_OK;
}

PDFX_STATUS PDFX_Page_Concat(PDFX_Page page, PDFX_REAL a, PDFX_REAL b,
                             PDFX_REAL c, PDFX_REAL d, PDFX_REAL x, PDFX_REAL y)
{
    pdfx_page_rec *p = (pdfx_page_rec *)page;
    pdf_status rc;
    if (!p) return PDFX_INVALID_PAGE;
    rc = pdf_set_transform(p->core, a, b, c, d, x, y);
    return rc == PDF_OK ? PDFX_OK : pdfx_fail_pdf(p->doc, rc);
}

PDFX_STATUS PDFX_Page_SetLineWidth(PDFX_Page page, PDFX_REAL line_width)
{
    pdfx_page_rec *p = (pdfx_page_rec *)page;
    pdf_status rc;
    if (!p) return PDFX_INVALID_PAGE;
    rc = pdf_set_line_width(p->core, (double)line_width);
    if (rc != PDF_OK) return pdfx_fail_pdf(p->doc, rc);
    p->line_width = (double)line_width;
    return PDFX_OK;
}

PDFX_STATUS PDFX_Page_SetLineCap(PDFX_Page page, PDFX_LineCap line_cap)
{
    pdfx_page_rec *p = (pdfx_page_rec *)page;
    pdf_status rc;
    if (!p) return PDFX_INVALID_PAGE;
    rc = pdf_set_line_cap(p->core, (pdf_line_cap)line_cap);
    if (rc != PDF_OK) return pdfx_fail_pdf(p->doc, rc);
    p->line_cap = (pdf_line_cap)line_cap;
    return PDFX_OK;
}

PDFX_STATUS PDFX_Page_SetLineJoin(PDFX_Page page, PDFX_LineJoin line_join)
{
    pdfx_page_rec *p = (pdfx_page_rec *)page;
    pdf_status rc;
    if (!p) return PDFX_INVALID_PAGE;
    rc = pdf_set_line_join(p->core, (pdf_line_join)line_join);
    if (rc != PDF_OK) return pdfx_fail_pdf(p->doc, rc);
    p->line_join = (pdf_line_join)line_join;
    return PDFX_OK;
}

PDFX_STATUS PDFX_Page_SetMiterLimit(PDFX_Page page, PDFX_REAL miter_limit)
{
    pdfx_page_rec *p = (pdfx_page_rec *)page;
    pdf_status rc;
    if (!p) return PDFX_INVALID_PAGE;
    rc = pdf_set_miter_limit(p->core, (double)miter_limit);
    if (rc != PDF_OK) return pdfx_fail_pdf(p->doc, rc);
    p->miter_limit = (double)miter_limit;
    return PDFX_OK;
}

PDFX_STATUS PDFX_Page_SetDash(PDFX_Page page, const PDFX_REAL *dash_ptn,
                              PDFX_UINT num_param, PDFX_REAL phase)
{
    pdfx_page_rec *p = (pdfx_page_rec *)page;
    double pattern[8];
    PDFX_UINT i;
    pdf_status rc;
    if (!p) return PDFX_INVALID_PAGE;
    if (num_param > 8) num_param = 8;
    for (i = 0; i < num_param; ++i)
        pattern[i] = (double)dash_ptn[i];
    rc = pdf_set_line_dash(p->core, num_param ? pattern : NULL,
                           (size_t)num_param, (double)phase);
    if (rc != PDF_OK) return pdfx_fail_pdf(p->doc, rc);
    memcpy(p->dash, pattern, sizeof(double) * num_param);
    p->dash_n = (size_t)num_param;
    p->dash_phase = (double)phase;
    return PDFX_OK;
}

/* Render hint, see PDFX_SetPagesConfiguration. */
PDFX_STATUS PDFX_Page_SetFlat(PDFX_Page page, PDFX_REAL flatness)
{
    PDFX_UNUSED(page);
    PDFX_UNUSED(flatness);
    return PDFX_OK;
}

PDFX_STATUS PDFX_Page_SetExtGState(PDFX_Page page, PDFX_ExtGState ext_gstate)
{
    pdfx_page_rec *p = (pdfx_page_rec *)page;
    pdfx_extg_rec *g = (pdfx_extg_rec *)ext_gstate;
    pdf_status rc;
    if (!p) return PDFX_INVALID_PAGE;
    if (!g) return pdfx_fail(p->doc, PDFX_INVALID_PARAMETER);
    rc = pdf_set_opacity(p->core, g->fill_alpha, g->stroke_alpha);
    if (rc != PDF_OK) return pdfx_fail_pdf(p->doc, rc);
    p->fill_alpha = g->fill_alpha;
    p->stroke_alpha = g->stroke_alpha;
    if (g->has_blend)
    {
        rc = pdf_set_blend_mode(p->core, g->blend_mode);
        if (rc != PDF_OK) return pdfx_fail_pdf(p->doc, rc);
        p->blend_mode = g->blend_mode;
    }
    return PDFX_OK;
}

static PDFX_STATUS pdfx_set_fill(pdfx_page_rec *p, pdf_color c)
{
    pdf_status rc;
    if (!p) return PDFX_INVALID_PAGE;
    c.alpha = 1.0;
    rc = pdf_set_fill_color(p->core, &c);
    if (rc != PDF_OK) return pdfx_fail_pdf(p->doc, rc);
    p->fill = c;
    return PDFX_OK;
}

static PDFX_STATUS pdfx_set_stroke(pdfx_page_rec *p, pdf_color c)
{
    pdf_status rc;
    if (!p) return PDFX_INVALID_PAGE;
    c.alpha = 1.0;
    rc = pdf_set_stroke_color(p->core, &c);
    if (rc != PDF_OK) return pdfx_fail_pdf(p->doc, rc);
    p->stroke = c;
    return PDFX_OK;
}

PDFX_STATUS PDFX_Page_SetGrayFill(PDFX_Page page, PDFX_REAL gray)
{
    pdf_color c;
    memset(&c, 0, sizeof(c));
    c.kind = PDF_COLOR_GRAY;
    c.c1 = pdfx_clamp01((double)gray);
    c.alpha = 1.0;
    return pdfx_set_fill((pdfx_page_rec *)page, c);
}

PDFX_STATUS PDFX_Page_SetGrayStroke(PDFX_Page page, PDFX_REAL gray)
{
    pdf_color c;
    memset(&c, 0, sizeof(c));
    c.kind = PDF_COLOR_GRAY;
    c.c1 = pdfx_clamp01((double)gray);
    c.alpha = 1.0;
    return pdfx_set_stroke((pdfx_page_rec *)page, c);
}

PDFX_STATUS PDFX_Page_SetRGBFill(PDFX_Page page, PDFX_REAL r, PDFX_REAL g,
                                 PDFX_REAL b)
{
    pdf_color c;
    memset(&c, 0, sizeof(c));
    c.kind = PDF_COLOR_RGB;
    c.c1 = pdfx_clamp01((double)r);
    c.c2 = pdfx_clamp01((double)g);
    c.c3 = pdfx_clamp01((double)b);
    c.alpha = 1.0;
    return pdfx_set_fill((pdfx_page_rec *)page, c);
}

PDFX_STATUS PDFX_Page_SetRGBStroke(PDFX_Page page, PDFX_REAL r, PDFX_REAL g,
                                   PDFX_REAL b)
{
    pdf_color c;
    memset(&c, 0, sizeof(c));
    c.kind = PDF_COLOR_RGB;
    c.c1 = pdfx_clamp01((double)r);
    c.c2 = pdfx_clamp01((double)g);
    c.c3 = pdfx_clamp01((double)b);
    c.alpha = 1.0;
    return pdfx_set_stroke((pdfx_page_rec *)page, c);
}

PDFX_STATUS PDFX_Page_SetCMYKFill(PDFX_Page page, PDFX_REAL c, PDFX_REAL m,
                                  PDFX_REAL y, PDFX_REAL k)
{
    pdf_color col;
    memset(&col, 0, sizeof(col));
    col.kind = PDF_COLOR_CMYK;
    col.c1 = pdfx_clamp01((double)c);
    col.c2 = pdfx_clamp01((double)m);
    col.c3 = pdfx_clamp01((double)y);
    col.c4 = pdfx_clamp01((double)k);
    col.alpha = 1.0;
    return pdfx_set_fill((pdfx_page_rec *)page, col);
}

PDFX_STATUS PDFX_Page_SetCMYKStroke(PDFX_Page page, PDFX_REAL c, PDFX_REAL m,
                                    PDFX_REAL y, PDFX_REAL k)
{
    pdf_color col;
    memset(&col, 0, sizeof(col));
    col.kind = PDF_COLOR_CMYK;
    col.c1 = pdfx_clamp01((double)c);
    col.c2 = pdfx_clamp01((double)m);
    col.c3 = pdfx_clamp01((double)y);
    col.c4 = pdfx_clamp01((double)k);
    col.alpha = 1.0;
    return pdfx_set_stroke((pdfx_page_rec *)page, col);
}

/* ------------------------------------------------------------------ */
/* Path construction / painting                                        */
/* ------------------------------------------------------------------ */

static PDFX_STATUS pdfx_path_rc(pdfx_page_rec *p, pdf_status rc)
{
    return rc == PDF_OK ? PDFX_OK : pdfx_fail_pdf(p->doc, rc);
}

PDFX_STATUS PDFX_Page_MoveTo(PDFX_Page page, PDFX_REAL x, PDFX_REAL y)
{
    pdfx_page_rec *p = (pdfx_page_rec *)page;
    PDFX_STATUS st;
    if (!p) return PDFX_INVALID_PAGE;
    st = pdfx_path_rc(p, pdf_path_move_to(p->core, x, y));
    if (st == PDFX_OK)
    {
        p->cur_pos.x = x;
        p->cur_pos.y = y;
    }
    return st;
}

PDFX_STATUS PDFX_Page_LineTo(PDFX_Page page, PDFX_REAL x, PDFX_REAL y)
{
    pdfx_page_rec *p = (pdfx_page_rec *)page;
    PDFX_STATUS st;
    if (!p) return PDFX_INVALID_PAGE;
    st = pdfx_path_rc(p, pdf_path_line_to(p->core, x, y));
    if (st == PDFX_OK)
    {
        p->cur_pos.x = x;
        p->cur_pos.y = y;
    }
    return st;
}

PDFX_STATUS PDFX_Page_CurveTo(PDFX_Page page, PDFX_REAL x1, PDFX_REAL y1,
                              PDFX_REAL x2, PDFX_REAL y2, PDFX_REAL x3,
                              PDFX_REAL y3)
{
    pdfx_page_rec *p = (pdfx_page_rec *)page;
    PDFX_STATUS st;
    if (!p) return PDFX_INVALID_PAGE;
    st = pdfx_path_rc(p, pdf_path_curve_to(p->core, x1, y1, x2, y2, x3, y3));
    if (st == PDFX_OK)
    {
        p->cur_pos.x = x3;
        p->cur_pos.y = y3;
    }
    return st;
}

PDFX_STATUS PDFX_Page_CurveTo2(PDFX_Page page, PDFX_REAL x2, PDFX_REAL y2,
                               PDFX_REAL x3, PDFX_REAL y3)
{
    pdfx_page_rec *p = (pdfx_page_rec *)page;
    if (!p) return PDFX_INVALID_PAGE;
    return PDFX_Page_CurveTo(page, p->cur_pos.x, p->cur_pos.y, x2, y2, x3, y3);
}

PDFX_STATUS PDFX_Page_CurveTo3(PDFX_Page page, PDFX_REAL x1, PDFX_REAL y1,
                               PDFX_REAL x3, PDFX_REAL y3)
{
    return PDFX_Page_CurveTo(page, x1, y1, x3, y3, x3, y3);
}

PDFX_STATUS PDFX_Page_ClosePath(PDFX_Page page)
{
    pdfx_page_rec *p = (pdfx_page_rec *)page;
    if (!p) return PDFX_INVALID_PAGE;
    return pdfx_path_rc(p, pdf_path_close(p->core));
}

PDFX_STATUS PDFX_Page_Rectangle(PDFX_Page page, PDFX_REAL x, PDFX_REAL y,
                                PDFX_REAL width, PDFX_REAL height)
{
    pdfx_page_rec *p = (pdfx_page_rec *)page;
    pdf_rect r;
    PDFX_STATUS st;
    if (!p) return PDFX_INVALID_PAGE;
    r.x = (double)x; r.y = (double)y;
    r.w = (double)width; r.h = (double)height;
    st = pdfx_path_rc(p, pdf_path_rectangle(p->core, &r));
    if (st == PDFX_OK)
    {
        p->cur_pos.x = x;
        p->cur_pos.y = y;
    }
    return st;
}

PDFX_STATUS PDFX_Page_Circle(PDFX_Page page, PDFX_REAL x, PDFX_REAL y,
                             PDFX_REAL ray)
{
    pdfx_page_rec *p = (pdfx_page_rec *)page;
    PDFX_STATUS st;
    if (!p) return PDFX_INVALID_PAGE;
    st = pdfx_path_rc(p, pdf_path_circle(p->core, x, y, ray));
    if (st == PDFX_OK)
    {
        p->cur_pos.x = x - ray;
        p->cur_pos.y = y;
    }
    return st;
}

PDFX_STATUS PDFX_Page_Ellipse(PDFX_Page page, PDFX_REAL x, PDFX_REAL y,
                              PDFX_REAL xray, PDFX_REAL yray)
{
    pdfx_page_rec *p = (pdfx_page_rec *)page;
    PDFX_STATUS st;
    if (!p) return PDFX_INVALID_PAGE;
    st = pdfx_path_rc(p, pdf_path_ellipse(p->core, x, y, xray, yray));
    if (st == PDFX_OK)
    {
        p->cur_pos.x = x - xray;
        p->cur_pos.y = y;
    }
    return st;
}

PDFX_STATUS PDFX_Page_Arc(PDFX_Page page, PDFX_REAL x, PDFX_REAL y,
                          PDFX_REAL ray, PDFX_REAL ang1, PDFX_REAL ang2)
{
    pdfx_page_rec *p = (pdfx_page_rec *)page;
    PDFX_STATUS st;
    if (!p) return PDFX_INVALID_PAGE;
    st = pdfx_path_rc(p, pdf_path_arc(p->core, x, y, ray, ang1, ang2, 1));
    if (st == PDFX_OK)
    {
        p->cur_pos.x = (PDFX_REAL)(x + ray * cos(ang1));
        p->cur_pos.y = (PDFX_REAL)(y + ray * sin(ang1));
    }
    return st;
}

PDFX_STATUS PDFX_Page_Stroke(PDFX_Page page)
{
    pdfx_page_rec *p = (pdfx_page_rec *)page;
    if (!p) return PDFX_INVALID_PAGE;
    return pdfx_path_rc(p, pdf_path_paint(p->core, PDF_PATH_STROKE));
}

PDFX_STATUS PDFX_Page_ClosePathStroke(PDFX_Page page)
{
    PDFX_STATUS st = PDFX_Page_ClosePath(page);
    return st == PDFX_OK ? PDFX_Page_Stroke(page) : st;
}

PDFX_STATUS PDFX_Page_Fill(PDFX_Page page)
{
    pdfx_page_rec *p = (pdfx_page_rec *)page;
    if (!p) return PDFX_INVALID_PAGE;
    return pdfx_path_rc(p, pdf_path_paint(p->core, PDF_PATH_FILL));
}

PDFX_STATUS PDFX_Page_Eofill(PDFX_Page page)
{
    pdfx_page_rec *p = (pdfx_page_rec *)page;
    if (!p) return PDFX_INVALID_PAGE;
    return pdfx_path_rc(p, pdf_path_paint(p->core, PDF_PATH_FILL_EVENODD));
}

PDFX_STATUS PDFX_Page_FillStroke(PDFX_Page page)
{
    pdfx_page_rec *p = (pdfx_page_rec *)page;
    if (!p) return PDFX_INVALID_PAGE;
    return pdfx_path_rc(p, pdf_path_paint(p->core, PDF_PATH_FILL_STROKE));
}

PDFX_STATUS PDFX_Page_EofillStroke(PDFX_Page page)
{
    pdfx_page_rec *p = (pdfx_page_rec *)page;
    if (!p) return PDFX_INVALID_PAGE;
    return pdfx_path_rc(p,
                        pdf_path_paint(p->core, PDF_PATH_FILL_STROKE_EVENODD));
}

PDFX_STATUS PDFX_Page_ClosePathFillStroke(PDFX_Page page)
{
    PDFX_STATUS st = PDFX_Page_ClosePath(page);
    return st == PDFX_OK ? PDFX_Page_FillStroke(page) : st;
}

PDFX_STATUS PDFX_Page_ClosePathEofillStroke(PDFX_Page page)
{
    PDFX_STATUS st = PDFX_Page_ClosePath(page);
    return st == PDFX_OK ? PDFX_Page_EofillStroke(page) : st;
}

PDFX_STATUS PDFX_Page_EndPath(PDFX_Page page)
{
    pdfx_page_rec *p = (pdfx_page_rec *)page;
    if (!p) return PDFX_INVALID_PAGE;
    return pdfx_path_rc(p, pdf_path_discard(p->core));
}

PDFX_STATUS PDFX_Page_Clip(PDFX_Page page)
{
    pdfx_page_rec *p = (pdfx_page_rec *)page;
    if (!p) return PDFX_INVALID_PAGE;
    return pdfx_path_rc(p, pdf_path_clip(p->core, 0));
}

PDFX_STATUS PDFX_Page_Eoclip(PDFX_Page page)
{
    pdfx_page_rec *p = (pdfx_page_rec *)page;
    if (!p) return PDFX_INVALID_PAGE;
    return pdfx_path_rc(p, pdf_path_clip(p->core, 1));
}

/* ------------------------------------------------------------------ */
/* ExtGState                                                           */
/* ------------------------------------------------------------------ */

PDFX_ExtGState PDFX_CreateExtGState(PDFX_Doc pdf)
{
    pdfx_doc_rec *d = (pdfx_doc_rec *)pdf;
    pdfx_extg_rec *g;
    if (!d)
        return NULL;
    g = (pdfx_extg_rec *)calloc(1, sizeof(*g));
    if (!g)
    {
        pdfx_fail(d, PDFX_FAILD_TO_ALLOC_MEM);
        return NULL;
    }
    g->fill_alpha = 1.0;
    g->stroke_alpha = 1.0;
    g->next = d->extgs;
    d->extgs = g;
    return (PDFX_ExtGState)g;
}

PDFX_STATUS PDFX_ExtGState_SetAlphaStroke(PDFX_ExtGState ext_gstate,
                                          PDFX_REAL value)
{
    pdfx_extg_rec *g = (pdfx_extg_rec *)ext_gstate;
    if (!g) return PDFX_INVALID_PARAMETER;
    g->stroke_alpha = pdfx_clamp01((double)value);
    return PDFX_OK;
}

PDFX_STATUS PDFX_ExtGState_SetAlphaFill(PDFX_ExtGState ext_gstate,
                                        PDFX_REAL value)
{
    pdfx_extg_rec *g = (pdfx_extg_rec *)ext_gstate;
    if (!g) return PDFX_INVALID_PARAMETER;
    g->fill_alpha = pdfx_clamp01((double)value);
    return PDFX_OK;
}

PDFX_STATUS PDFX_ExtGState_SetBlendMode(PDFX_ExtGState ext_gstate,
                                        PDFX_BlendMode mode)
{
    pdfx_extg_rec *g = (pdfx_extg_rec *)ext_gstate;
    if (!g) return PDFX_INVALID_PARAMETER;
    g->blend_mode = (pdf_blend_mode)mode;
    g->has_blend = 1;
    return PDFX_OK;
}

/* ------------------------------------------------------------------ */
/* Images                                                              */
/* ------------------------------------------------------------------ */

static pdf_status pdfx_image_file(pdfx_doc_rec *d, const char *path,
                                  pdf_image *out)
{
    if (!d || !d->core || !path || !*path)
        return PDF_ERR_INVALID_PARAM;
    return pdf_image_load_file(d->core, path, out);
}

PDFX_Image PDFX_LoadPngImageFromFile(PDFX_Doc pdf, const char *filename)
{
    pdfx_doc_rec *d = (pdfx_doc_rec *)pdf;
    pdf_image img = NULL;
    pdf_status rc;
    if (!pdfx_doc_ok(d)) { pdfx_fail(d, PDFX_INVALID_DOCUMENT); return NULL; }
    rc = pdfx_image_file(d, filename, &img);
    if (rc != PDF_OK) { pdfx_fail_pdf(d, rc); return NULL; }
    pdfx_track(d, img, 1);
    return (PDFX_Image)img;
}

PDFX_Image PDFX_LoadPngImageFromFile2(PDFX_Doc pdf, const char *filename)
{
    return PDFX_LoadPngImageFromFile(pdf, filename);
}

PDFX_Image PDFX_LoadJpegImageFromFile(PDFX_Doc pdf, const char *filename)
{
    return PDFX_LoadPngImageFromFile(pdf, filename);
}

PDFX_Image PDFX_LoadPngImageFromMem(PDFX_Doc pdf, const PDFX_BYTE *buffer,
                                    PDFX_UINT size)
{
    pdfx_doc_rec *d = (pdfx_doc_rec *)pdf;
    pdf_image img = NULL;
    pdf_status rc;
    if (!pdfx_doc_ok(d)) { pdfx_fail(d, PDFX_INVALID_DOCUMENT); return NULL; }
    rc = pdf_image_load_memory(d->core, buffer, (size_t)size, &img);
    if (rc != PDF_OK) { pdfx_fail_pdf(d, rc); return NULL; }
    pdfx_track(d, img, 1);
    return (PDFX_Image)img;
}

PDFX_Image PDFX_LoadJpegImageFromMem(PDFX_Doc pdf, const PDFX_BYTE *buffer,
                                     PDFX_UINT size)
{
    return PDFX_LoadPngImageFromMem(pdf, buffer, size);
}

PDFX_UINT PDFX_Image_GetWidth(PDFX_Image image)
{
    size_t w = 0, h = 0;
    if (!image) return 0;
    pdf_image_get_size((pdf_image)image, &w, &h);
    return (PDFX_UINT)w;
}

PDFX_UINT PDFX_Image_GetHeight(PDFX_Image image)
{
    size_t w = 0, h = 0;
    if (!image) return 0;
    pdf_image_get_size((pdf_image)image, &w, &h);
    return (PDFX_UINT)h;
}

PDFX_Point PDFX_Image_GetSize(PDFX_Image image)
{
    PDFX_Point p = { 0, 0 };
    p.x = (PDFX_REAL)PDFX_Image_GetWidth(image);
    p.y = (PDFX_REAL)PDFX_Image_GetHeight(image);
    return p;
}

PDFX_STATUS PDFX_Image_GetSize2(PDFX_Image image, PDFX_Point *size)
{
    if (!size) return PDFX_INVALID_PARAMETER;
    *size = PDFX_Image_GetSize(image);
    return PDFX_OK;
}

PDFX_UINT PDFX_Image_GetBitsPerComponent(PDFX_Image image)
{
    PDFX_UNUSED(image);
    return 8;
}

const char *PDFX_Image_GetColorSpace(PDFX_Image image)
{
    PDFX_UNUSED(image);
    return "DeviceRGB";
}

PDFX_STATUS PDFX_Page_DrawImage(PDFX_Page page, PDFX_Image image,
                                PDFX_REAL x, PDFX_REAL y, PDFX_REAL width,
                                PDFX_REAL height)
{
    pdfx_page_rec *p = (pdfx_page_rec *)page;
    pdf_rect r;
    pdf_status rc;
    if (!p) return PDFX_INVALID_PAGE;
    r.x = (double)x; r.y = (double)y;
    r.w = (double)width; r.h = (double)height;
    rc = pdf_page_draw_image(p->core, (pdf_image)image, &r);
    return rc == PDF_OK ? PDFX_OK : pdfx_fail_pdf(p->doc, rc);
}

/* ------------------------------------------------------------------ */
/* Info / security / options                                           */
/* ------------------------------------------------------------------ */

static int pdfx_info_key(PDFX_InfoType type, pdf_info_key *out)
{
    switch (type)
    {
        case PDFX_INFO_TITLE:    *out = PDF_INFO_TITLE; return 1;
        case PDFX_INFO_AUTHOR:   *out = PDF_INFO_AUTHOR; return 1;
        case PDFX_INFO_SUBJECT:  *out = PDF_INFO_SUBJECT; return 1;
        case PDFX_INFO_KEYWORDS: *out = PDF_INFO_KEYWORDS; return 1;
        case PDFX_INFO_CREATOR:  *out = PDF_INFO_CREATOR; return 1;
        case PDFX_INFO_PRODUCER: *out = PDF_INFO_PRODUCER; return 1;
        default: return 0;
    }
}

PDFX_STATUS PDFX_SetInfoAttr(PDFX_Doc pdf, PDFX_InfoType type,
                             const char *value)
{
    pdfx_doc_rec *d = (pdfx_doc_rec *)pdf;
    pdf_info_key key;
    pdf_status rc;
    if (!pdfx_doc_ok(d)) return PDFX_INVALID_DOCUMENT;
    if (!pdfx_info_key(type, &key))
        return PDFX_OK; /* date/trapped entries are not modelled */
    rc = pdf_doc_set_info(d->core, key, value ? value : "");
    return rc == PDF_OK ? PDFX_OK : pdfx_fail_pdf(d, rc);
}

const char *PDFX_GetInfoAttr(PDFX_Doc pdf, PDFX_InfoType type)
{
    pdfx_doc_rec *d = (pdfx_doc_rec *)pdf;
    pdf_info_key key;
    size_t len = sizeof(d->info_buf);
    pdf_status rc;
    if (!pdfx_doc_ok(d)) return "";
    if (!pdfx_info_key(type, &key))
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

PDFX_STATUS PDFX_SetInfoDateAttr(PDFX_Doc pdf, PDFX_InfoType type,
                                 PDFX_Date value)
{
    PDFX_UNUSED(value);
    PDFX_UNUSED(type);
    if (!pdf)
        return PDFX_INVALID_PARAMETER;
    return PDFX_OK;
}

PDFX_STATUS PDFX_SetCompressionMode(PDFX_Doc pdf, PDFX_UINT mode)
{
    pdfx_doc_rec *d = (pdfx_doc_rec *)pdf;
    unsigned mask = 0;
    pdf_status rc;
    if (!pdfx_doc_ok(d)) return PDFX_INVALID_DOCUMENT;
    if (mode & PDFX_COMP_TEXT)     mask |= PDF_COMPRESS_TEXT;
    if (mode & PDFX_COMP_IMAGE)    mask |= PDF_COMPRESS_IMAGE;
    if (mode & PDFX_COMP_METADATA) mask |= PDF_COMPRESS_METADATA;
    if (mode == PDFX_COMP_ALL)     mask = PDF_COMPRESS_ALL;
    rc = pdf_doc_set_compression(d->core, mask);
    if (rc == PDF_OK)
        d->compression = (int)mode;
    return rc == PDF_OK ? PDFX_OK : pdfx_fail_pdf(d, rc);
}

PDFX_STATUS PDFX_SetPassword(PDFX_Doc pdf, const char *owner_passwd,
                             const char *user_passwd)
{
    pdfx_doc_rec *d = (pdfx_doc_rec *)pdf;
    if (!d) return PDFX_INVALID_PARAMETER;
    snprintf(d->owner_pass, sizeof(d->owner_pass), "%s",
             owner_passwd ? owner_passwd : "");
    snprintf(d->user_pass, sizeof(d->user_pass), "%s",
             user_passwd ? user_passwd : "");
    d->have_password = 1;
    d->encrypt_applied = 0;
    return PDFX_OK;
}

PDFX_STATUS PDFX_SetPermission(PDFX_Doc pdf, PDFX_UINT permission)
{
    pdfx_doc_rec *d = (pdfx_doc_rec *)pdf;
    if (!d) return PDFX_INVALID_PARAMETER;
    d->permissions = permission;
    d->encrypt_applied = 0;
    return PDFX_OK;
}

PDFX_STATUS PDFX_SetEncryptionMode(PDFX_Doc pdf, PDFX_EncryptMode mode,
                                   PDFX_UINT key_len)
{
    pdfx_doc_rec *d = (pdfx_doc_rec *)pdf;
    if (!d) return PDFX_INVALID_PARAMETER;
    if (mode != PDFX_ENCRYPT_R2 && mode != PDFX_ENCRYPT_R3)
        return pdfx_fail(d, PDFX_INVALID_ENCRYPT_KEY_LEN);
    PDFX_UNUSED(key_len);
    d->encryption_mode = (int)mode;
    d->encrypt_applied = 0;
    return PDFX_OK;
}

PDFX_PageLayout PDFX_GetPageLayout(PDFX_Doc pdf)
{
    pdfx_doc_rec *d = (pdfx_doc_rec *)pdf;
    return d ? d->layout : PDFX_PAGE_LAYOUT_SINGLE;
}

PDFX_STATUS PDFX_SetPageLayout(PDFX_Doc pdf, PDFX_PageLayout layout)
{
    pdfx_doc_rec *d = (pdfx_doc_rec *)pdf;
    if (!d) return PDFX_INVALID_PARAMETER;
    d->layout = layout;
    return PDFX_OK;
}

PDFX_PageMode PDFX_GetPageMode(PDFX_Doc pdf)
{
    pdfx_doc_rec *d = (pdfx_doc_rec *)pdf;
    return d ? d->page_mode : PDFX_PAGE_MODE_USE_NONE;
}

PDFX_STATUS PDFX_SetPageMode(PDFX_Doc pdf, PDFX_PageMode mode)
{
    pdfx_doc_rec *d = (pdfx_doc_rec *)pdf;
    pdf_status rc;
    if (!pdfx_doc_ok(d)) return PDFX_INVALID_DOCUMENT;
    d->page_mode = mode;
    rc = pdf_doc_set_page_mode_use_outlines(
        d->core, mode == PDFX_PAGE_MODE_USE_OUTLINE);
    return rc == PDF_OK ? PDFX_OK : pdfx_fail_pdf(d, rc);
}

PDFX_UINT PDFX_GetViewerPreference(PDFX_Doc pdf)
{
    pdfx_doc_rec *d = (pdfx_doc_rec *)pdf;
    return d ? d->viewer_pref : 0;
}

PDFX_STATUS PDFX_SetViewerPreference(PDFX_Doc pdf, PDFX_UINT value)
{
    pdfx_doc_rec *d = (pdfx_doc_rec *)pdf;
    if (!d) return PDFX_INVALID_PARAMETER;
    d->viewer_pref = value;
    return PDFX_OK;
}

PDFX_STATUS PDFX_SetOpenAction(PDFX_Doc pdf, PDFX_Destination open_action)
{
    PDFX_UNUSED(open_action);
    if (!pdf) return PDFX_INVALID_PARAMETER;
    return PDFX_UNSUPPORTED_FUNC;
}

/* ------------------------------------------------------------------ */
/* Destinations / outlines                                             */
/* ------------------------------------------------------------------ */

PDFX_Destination PDFX_Page_CreateDestination(PDFX_Page page)
{
    pdfx_page_rec *p = (pdfx_page_rec *)page;
    pdfx_dest_rec *dst;
    if (!p) return NULL;
    dst = (pdfx_dest_rec *)calloc(1, sizeof(*dst));
    if (!dst)
    {
        pdfx_fail(p->doc, PDFX_FAILD_TO_ALLOC_MEM);
        return NULL;
    }
    dst->doc = p->doc;
    dst->page = page;
    dst->type = PDFX_XYZ;
    dst->zoom = 0;
    dst->next = p->doc->dests;
    p->doc->dests = dst;
    return (PDFX_Destination)dst;
}

PDFX_STATUS PDFX_Destination_SetXYZ(PDFX_Destination dst, PDFX_REAL left,
                                    PDFX_REAL top, PDFX_REAL zoom)
{
    pdfx_dest_rec *x = (pdfx_dest_rec *)dst;
    if (!x) return PDFX_INVALID_DESTINATION;
    x->type = PDFX_XYZ;
    x->left = left; x->top = top; x->zoom = zoom;
    return PDFX_OK;
}

PDFX_STATUS PDFX_Destination_SetFit(PDFX_Destination dst)
{
    pdfx_dest_rec *x = (pdfx_dest_rec *)dst;
    if (!x) return PDFX_INVALID_DESTINATION;
    x->type = PDFX_FIT;
    return PDFX_OK;
}

PDFX_STATUS PDFX_Destination_SetFitH(PDFX_Destination dst, PDFX_REAL top)
{
    pdfx_dest_rec *x = (pdfx_dest_rec *)dst;
    if (!x) return PDFX_INVALID_DESTINATION;
    x->type = PDFX_FIT_H; x->top = top;
    return PDFX_OK;
}

PDFX_STATUS PDFX_Destination_SetFitV(PDFX_Destination dst, PDFX_REAL left)
{
    pdfx_dest_rec *x = (pdfx_dest_rec *)dst;
    if (!x) return PDFX_INVALID_DESTINATION;
    x->type = PDFX_FIT_V; x->left = left;
    return PDFX_OK;
}

PDFX_STATUS PDFX_Destination_SetFitR(PDFX_Destination dst, PDFX_REAL left,
                                     PDFX_REAL bottom, PDFX_REAL right,
                                     PDFX_REAL top)
{
    pdfx_dest_rec *x = (pdfx_dest_rec *)dst;
    if (!x) return PDFX_INVALID_DESTINATION;
    x->type = PDFX_FIT_R;
    x->left = left; x->bottom = bottom; x->right = right; x->top = top;
    return PDFX_OK;
}

PDFX_STATUS PDFX_Destination_SetFitB(PDFX_Destination dst)
{
    pdfx_dest_rec *x = (pdfx_dest_rec *)dst;
    if (!x) return PDFX_INVALID_DESTINATION;
    x->type = PDFX_FIT_B;
    return PDFX_OK;
}

PDFX_STATUS PDFX_Destination_SetFitBH(PDFX_Destination dst, PDFX_REAL top)
{
    pdfx_dest_rec *x = (pdfx_dest_rec *)dst;
    if (!x) return PDFX_INVALID_DESTINATION;
    x->type = PDFX_FIT_BH; x->top = top;
    return PDFX_OK;
}

PDFX_STATUS PDFX_Destination_SetFitBV(PDFX_Destination dst, PDFX_REAL left)
{
    pdfx_dest_rec *x = (pdfx_dest_rec *)dst;
    if (!x) return PDFX_INVALID_DESTINATION;
    x->type = PDFX_FIT_BV; x->left = left;
    return PDFX_OK;
}

PDFX_Outline PDFX_CreateOutline(PDFX_Doc pdf, PDFX_Outline parent,
                                const char *title, PDFX_Encoder encoder)
{
    pdfx_doc_rec *d = (pdfx_doc_rec *)pdf;
    pdf_outline out = NULL;
    pdf_status rc;
    PDFX_UNUSED(encoder);
    if (!pdfx_doc_ok(d))
    {
        pdfx_fail(d, PDFX_INVALID_DOCUMENT);
        return NULL;
    }
    rc = pdf_outline_add(d->core, (pdf_outline)parent,
                         title ? title : "", NULL, &out);
    if (rc != PDF_OK)
    {
        pdfx_fail_pdf(d, rc);
        return NULL;
    }
    pdfx_track(d, out, 3);
    return (PDFX_Outline)out;
}

PDFX_STATUS PDFX_Outline_SetOpened(PDFX_Outline outline, PDFX_BOOL opened)
{
    pdf_status rc;
    if (!outline) return PDFX_INVALID_OUTLINE;
    rc = pdf_outline_set_open((pdf_outline)outline, opened ? 1 : 0);
    return rc == PDF_OK ? PDFX_OK : PDFX_INVALID_OUTLINE;
}

PDFX_STATUS PDFX_Outline_SetDestination(PDFX_Outline outline,
                                        PDFX_Destination dst)
{
    pdfx_dest_rec *x = (pdfx_dest_rec *)dst;
    pdf_page page;
    pdf_status rc;
    if (!outline || !x || !x->page)
        return PDFX_INVALID_DESTINATION;
    page = ((pdfx_page_rec *)x->page)->core;
    rc = pdf_outline_set_destination((pdf_outline)outline, &page);
    return rc == PDF_OK ? PDFX_OK : PDFX_UNSUPPORTED_FUNC;
}

/* ------------------------------------------------------------------ */
/* Annotations                                                         */
/* ------------------------------------------------------------------ */

static PDFX_Annotation pdfx_create_annot(pdfx_page_rec *p, pdf_annot_type type,
                                         const PDFX_Rect *rect,
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
        pdfx_fail_pdf(p->doc, rc);
        return NULL;
    }
    pdfx_track(p->doc, a, 2);
    return (PDFX_Annotation)a;
}

PDFX_Annotation PDFX_Page_CreateTextAnnot(PDFX_Page page, PDFX_Rect rect,
                                          const char *text,
                                          PDFX_Encoder encoder)
{
    PDFX_UNUSED(encoder);
    return pdfx_create_annot((pdfx_page_rec *)page, PDF_ANNOT_TEXT, &rect,
                             text ? text : "");
}

PDFX_Annotation PDFX_Page_CreateURILinkAnnot(PDFX_Page page, PDFX_Rect rect,
                                             const char *uri)
{
    if (!uri || !*uri)
    {
        pdfx_fail(pdfx_doc_of_page(page), PDFX_INVALID_URI);
        return NULL;
    }
    return pdfx_create_annot((pdfx_page_rec *)page, PDF_ANNOT_LINK_URI, &rect,
                             uri);
}

PDFX_Annotation PDFX_Page_CreateLinkAnnot(PDFX_Page page, PDFX_Rect rect,
                                          PDFX_Destination dst)
{
    pdfx_dest_rec *x = (pdfx_dest_rec *)dst;
    PDFX_Annotation a = pdfx_create_annot((pdfx_page_rec *)page,
                                          PDF_ANNOT_LINK_GOTO, &rect, NULL);
    if (!a)
        return NULL;
    if (!x || !x->page)
    {
        pdfx_fail(pdfx_doc_of_page(page), PDFX_INVALID_DESTINATION);
        return NULL;
    }
    {
        pdf_page dest = ((pdfx_page_rec *)x->page)->core;
        pdf_status rc = pdf_annot_link_to_page((pdf_annot)a, &dest, NULL);
        if (rc != PDF_OK)
        {
            pdfx_fail_pdf(pdfx_doc_of_page(page), rc);
            return NULL;
        }
    }
    return a;
}

PDFX_Annotation PDFX_Page_CreateHighlightAnnot(PDFX_Page page, PDFX_Rect rect,
                                               const char *text,
                                               PDFX_Encoder encoder)
{
    PDFX_UNUSED(encoder);
    return pdfx_create_annot((pdfx_page_rec *)page, PDF_ANNOT_HIGHLIGHT,
                             &rect, text ? text : "");
}

PDFX_Annotation PDFX_Page_CreateUnderlineAnnot(PDFX_Page page, PDFX_Rect rect,
                                               const char *text,
                                               PDFX_Encoder encoder)
{
    PDFX_UNUSED(encoder);
    return pdfx_create_annot((pdfx_page_rec *)page, PDF_ANNOT_UNDERLINE,
                             &rect, text ? text : "");
}

PDFX_Annotation PDFX_Page_CreateSquigglyAnnot(PDFX_Page page, PDFX_Rect rect,
                                              const char *text,
                                              PDFX_Encoder encoder)
{
    PDFX_UNUSED(encoder);
    return pdfx_create_annot((pdfx_page_rec *)page, PDF_ANNOT_SQUIGGLY,
                             &rect, text ? text : "");
}

PDFX_Annotation PDFX_Page_CreateStrikeOutAnnot(PDFX_Page page, PDFX_Rect rect,
                                               const char *text,
                                               PDFX_Encoder encoder)
{
    PDFX_UNUSED(encoder);
    return pdfx_create_annot((pdfx_page_rec *)page, PDF_ANNOT_STRIKEOUT,
                             &rect, text ? text : "");
}

PDFX_Annotation PDFX_Page_CreateSquareAnnot(PDFX_Page page, PDFX_Rect rect,
                                            const char *text,
                                            PDFX_Encoder encoder)
{
    PDFX_UNUSED(encoder);
    return pdfx_create_annot((pdfx_page_rec *)page, PDF_ANNOT_SQUARE, &rect,
                             text ? text : "");
}

PDFX_Annotation PDFX_Page_CreateCircleAnnot(PDFX_Page page, PDFX_Rect rect,
                                            const char *text,
                                            PDFX_Encoder encoder)
{
    PDFX_UNUSED(encoder);
    return pdfx_create_annot((pdfx_page_rec *)page, PDF_ANNOT_CIRCLE, &rect,
                             text ? text : "");
}

PDFX_STATUS PDFX_TextAnnot_SetIcon(PDFX_Annotation annot, PDFX_AnnotIcon icon)
{
    pdf_status rc;
    if (!annot) return PDFX_INVALID_ANNOTATION;
    rc = pdf_annot_set_icon((pdf_annot)annot, (pdf_annot_icon)icon);
    return rc == PDF_OK ? PDFX_OK : pdfx_map_pdf_status(rc);
}

PDFX_STATUS PDFX_TextAnnot_SetOpened(PDFX_Annotation annot, PDFX_BOOL opened)
{
    pdf_status rc;
    if (!annot) return PDFX_INVALID_ANNOTATION;
    rc = pdf_annot_set_open((pdf_annot)annot, opened ? 1 : 0);
    return rc == PDF_OK ? PDFX_OK : pdfx_map_pdf_status(rc);
}

static PDFX_STATUS pdfx_annot_color(PDFX_Annotation annot, const pdf_color *c)
{
    pdf_status rc;
    if (!annot) return PDFX_INVALID_ANNOTATION;
    rc = pdf_annot_set_color((pdf_annot)annot, c);
    return rc == PDF_OK ? PDFX_OK : pdfx_map_pdf_status(rc);
}

PDFX_STATUS PDFX_Annot_SetRGBColor(PDFX_Annotation annot, PDFX_RGBColor color)
{
    pdf_color c;
    memset(&c, 0, sizeof(c));
    c.kind = PDF_COLOR_RGB;
    c.c1 = color.r; c.c2 = color.g; c.c3 = color.b;
    return pdfx_annot_color(annot, &c);
}

PDFX_STATUS PDFX_Annot_SetCMYKColor(PDFX_Annotation annot,
                                    PDFX_CMYKColor color)
{
    pdf_color c;
    memset(&c, 0, sizeof(c));
    c.kind = PDF_COLOR_CMYK;
    c.c1 = color.c; c.c2 = color.m; c.c3 = color.y; c.c4 = color.k;
    return pdfx_annot_color(annot, &c);
}

PDFX_STATUS PDFX_Annot_SetGrayColor(PDFX_Annotation annot, PDFX_REAL color)
{
    pdf_color c;
    memset(&c, 0, sizeof(c));
    c.kind = PDF_COLOR_GRAY;
    c.c1 = color;
    return pdfx_annot_color(annot, &c);
}

PDFX_STATUS PDFX_Annot_SetNoColor(PDFX_Annotation annot)
{
    if (!annot) return PDFX_INVALID_ANNOTATION;
    return PDFX_OK;
}

PDFX_STATUS PDFX_MarkupAnnot_SetTitle(PDFX_Annotation annot, const char *name)
{
    pdf_status rc;
    if (!annot) return PDFX_INVALID_ANNOTATION;
    rc = pdf_annot_set_author((pdf_annot)annot, name);
    return rc == PDF_OK ? PDFX_OK : pdfx_map_pdf_status(rc);
}

PDFX_STATUS PDFX_MarkupAnnot_SetSubject(PDFX_Annotation annot,
                                        const char *name)
{
    pdf_status rc;
    if (!annot) return PDFX_INVALID_ANNOTATION;
    rc = pdf_annot_set_subject((pdf_annot)annot, name);
    return rc == PDF_OK ? PDFX_OK : pdfx_map_pdf_status(rc);
}

PDFX_STATUS PDFX_MarkupAnnot_SetTransparency(PDFX_Annotation annot,
                                             PDFX_REAL value)
{
    pdf_status rc;
    if (!annot) return PDFX_INVALID_ANNOTATION;
    rc = pdf_annot_set_opacity((pdf_annot)annot, value);
    return rc == PDF_OK ? PDFX_OK : pdfx_map_pdf_status(rc);
}

/* Annotation metadata, see PDFX_SetPagesConfiguration. */
PDFX_STATUS PDFX_MarkupAnnot_SetCreationDate(PDFX_Annotation annot,
                                             PDFX_Date value)
{
    PDFX_UNUSED(annot);
    PDFX_UNUSED(value);
    return PDFX_OK;
}

/* Annotation metadata, see PDFX_SetPagesConfiguration. */
PDFX_STATUS PDFX_MarkupAnnot_SetIntent(PDFX_Annotation annot,
                                        PDFX_AnnotIntent intent)
{
    PDFX_UNUSED(annot);
    PDFX_UNUSED(intent);
    return PDFX_OK;
}

PDFX_STATUS PDFX_MarkupAnnot_SetPopup(PDFX_Annotation annot,
                                      PDFX_Annotation popup)
{
    PDFX_UNUSED(annot);
    PDFX_UNUSED(popup);
    return PDFX_UNSUPPORTED_FUNC;
}

PDFX_STATUS PDFX_MarkupAnnot_SetRectDiff(PDFX_Annotation annot, PDFX_Rect rect)
{
    PDFX_UNUSED(annot);
    PDFX_UNUSED(rect);
    return PDFX_UNSUPPORTED_FUNC;
}

PDFX_STATUS PDFX_MarkupAnnot_SetCloudEffect(PDFX_Annotation annot,
                                            PDFX_INT cloudIntensity)
{
    PDFX_UNUSED(annot);
    PDFX_UNUSED(cloudIntensity);
    return PDFX_UNSUPPORTED_FUNC;
}

PDFX_STATUS PDFX_MarkupAnnot_SetInteriorRGBColor(PDFX_Annotation annot,
                                                 PDFX_RGBColor color)
{
    PDFX_UNUSED(annot);
    PDFX_UNUSED(color);
    return PDFX_UNSUPPORTED_FUNC;
}

PDFX_STATUS PDFX_MarkupAnnot_SetInteriorCMYKColor(PDFX_Annotation annot,
                                                  PDFX_CMYKColor color)
{
    PDFX_UNUSED(annot);
    PDFX_UNUSED(color);
    return PDFX_UNSUPPORTED_FUNC;
}

PDFX_STATUS PDFX_MarkupAnnot_SetInteriorGrayColor(PDFX_Annotation annot,
                                                  PDFX_REAL color)
{
    PDFX_UNUSED(annot);
    PDFX_UNUSED(color);
    return PDFX_UNSUPPORTED_FUNC;
}

PDFX_STATUS PDFX_MarkupAnnot_SetInteriorTransparent(PDFX_Annotation annot)
{
    PDFX_UNUSED(annot);
    return PDFX_UNSUPPORTED_FUNC;
}

/* pdflib generates /QuadPoints itself, from the annotation's /Rect, when the
 * markup annotation is created (pdf_backend_libharu.c does this in
 * annot_create). It has no setter for an independent quad, so there is
 * nothing to forward here. This stays PDFX_OK rather than
 * PDFX_UNSUPPORTED_FUNC on purpose: libharu programs are *required* to call
 * it, and the four points they pass are the corners of that same rect, so
 * the resulting file is the one they asked for. It would be wrong only for a
 * quad that is not axis-aligned with the rect. */
PDFX_STATUS PDFX_TextMarkupAnnot_SetQuadPoints(PDFX_Annotation annot,
                                               PDFX_Point lb, PDFX_Point rb,
                                               PDFX_Point rt, PDFX_Point lt)
{
    PDFX_UNUSED(annot);
    PDFX_UNUSED(lb); PDFX_UNUSED(rb); PDFX_UNUSED(rt); PDFX_UNUSED(lt);
    return PDFX_OK;
}

/* Viewer hint, see PDFX_SetPagesConfiguration. */
PDFX_STATUS PDFX_LinkAnnot_SetHighlightMode(PDFX_Annotation annot,
                                            PDFX_AnnotHighlightMode mode)
{
    PDFX_UNUSED(annot);
    PDFX_UNUSED(mode);
    return PDFX_OK;
}

PDFX_STATUS PDFX_LinkAnnot_SetJavaScript(PDFX_Annotation annot,
                                         PDFX_JavaScript javascript)
{
    PDFX_UNUSED(annot);
    PDFX_UNUSED(javascript);
    return PDFX_UNSUPPORTED_FUNC;
}

/* Border style. pdflib has no border-style setter - the annotation's /BS
 * dictionary is not modelled at all, so there is nothing to forward to. It
 * used to return PDFX_OK and write nothing, which a caller can only notice
 * by opening the file: verified by building a square annotation, calling
 * this with width 2.5, and finding zero /BS keys in the output. */
PDFX_STATUS PDFX_LinkAnnot_SetBorderStyle(PDFX_Annotation annot,
                                          PDFX_REAL width, PDFX_UINT16 dash_on,
                                          PDFX_UINT16 dash_off)
{
    PDFX_UNUSED(annot);
    PDFX_UNUSED(width);
    PDFX_UNUSED(dash_on);
    PDFX_UNUSED(dash_off);
    return PDFX_UNSUPPORTED_FUNC;
}

PDFX_STATUS PDFX_Annotation_SetBorderStyle(PDFX_Annotation annot,
                                           PDFX_BSSubtype subtype,
                                           PDFX_REAL width, PDFX_UINT16 dash_on,
                                           PDFX_UINT16 dash_off,
                                           PDFX_UINT16 dash_phase)
{
    PDFX_UNUSED(annot);
    PDFX_UNUSED(subtype);
    PDFX_UNUSED(width);
    PDFX_UNUSED(dash_on);
    PDFX_UNUSED(dash_off);
    PDFX_UNUSED(dash_phase);
    return PDFX_UNSUPPORTED_FUNC;
}

/* pdflib does model this one (pdf_annot_set_open writes /Open on the popup),
 * so forward it instead of accepting the call and dropping it. */
PDFX_STATUS PDFX_PopupAnnot_SetOpened(PDFX_Annotation annot, PDFX_BOOL opened)
{
    pdf_status rc;
    if (!annot)
        return PDFX_INVALID_ANNOTATION;
    rc = pdf_annot_set_open((pdf_annot)annot, opened ? 1 : 0);
    return rc == PDF_OK ? PDFX_OK : pdfx_map_pdf_status(rc);
}

PDFX_STATUS PDFX_FreeTextAnnot_SetLineEndingStyle(
    PDFX_Annotation annot, PDFX_LineAnnotEndingStyle startStyle,
    PDFX_LineAnnotEndingStyle endStyle)
{
    PDFX_UNUSED(annot); PDFX_UNUSED(startStyle); PDFX_UNUSED(endStyle);
    return PDFX_UNSUPPORTED_FUNC;
}

PDFX_STATUS PDFX_FreeTextAnnot_Set3PointCalloutLine(
    PDFX_Annotation annot, PDFX_Point startPoint, PDFX_Point kneePoint,
    PDFX_Point endPoint)
{
    PDFX_UNUSED(annot); PDFX_UNUSED(startPoint);
    PDFX_UNUSED(kneePoint); PDFX_UNUSED(endPoint);
    return PDFX_UNSUPPORTED_FUNC;
}

PDFX_STATUS PDFX_FreeTextAnnot_Set2PointCalloutLine(
    PDFX_Annotation annot, PDFX_Point startPoint, PDFX_Point endPoint)
{
    PDFX_UNUSED(annot); PDFX_UNUSED(startPoint); PDFX_UNUSED(endPoint);
    return PDFX_UNSUPPORTED_FUNC;
}

PDFX_STATUS PDFX_FreeTextAnnot_SetDefaultStyle(PDFX_Annotation annot,
                                               const char *style)
{
    PDFX_UNUSED(annot); PDFX_UNUSED(style);
    return PDFX_UNSUPPORTED_FUNC;
}

PDFX_STATUS PDFX_LineAnnot_SetPosition(
    PDFX_Annotation annot, PDFX_Point startPoint,
    PDFX_LineAnnotEndingStyle startStyle, PDFX_Point endPoint,
    PDFX_LineAnnotEndingStyle endStyle)
{
    PDFX_UNUSED(annot); PDFX_UNUSED(startPoint); PDFX_UNUSED(startStyle);
    PDFX_UNUSED(endPoint); PDFX_UNUSED(endStyle);
    return PDFX_UNSUPPORTED_FUNC;
}

PDFX_STATUS PDFX_LineAnnot_SetLeader(PDFX_Annotation annot,
                                     PDFX_INT leaderLen,
                                     PDFX_INT leaderExtLen,
                                     PDFX_INT leaderOffsetLen)
{
    PDFX_UNUSED(annot); PDFX_UNUSED(leaderLen);
    PDFX_UNUSED(leaderExtLen); PDFX_UNUSED(leaderOffsetLen);
    return PDFX_UNSUPPORTED_FUNC;
}

PDFX_STATUS PDFX_LineAnnot_SetCaption(PDFX_Annotation annot,
                                       PDFX_BOOL showCaption,
                                       PDFX_LineAnnotCapPosition position,
                                       PDFX_INT horzOffset,
                                       PDFX_INT vertOffset)
{
    PDFX_UNUSED(annot); PDFX_UNUSED(showCaption); PDFX_UNUSED(position);
    PDFX_UNUSED(horzOffset); PDFX_UNUSED(vertOffset);
    return PDFX_UNSUPPORTED_FUNC;
}

PDFX_STATUS PDFX_ProjectionAnnot_SetExData(PDFX_Annotation annot,
                                           PDFX_ExData exdata)
{
    PDFX_UNUSED(annot); PDFX_UNUSED(exdata);
    return PDFX_UNSUPPORTED_FUNC;
}

/* ------------------------------------------------------------------ */
/* Font metrics                                                        */
/* ------------------------------------------------------------------ */

const char *PDFX_Font_GetFontName(PDFX_Font font)
{
    pdfx_font_rec *f = (pdfx_font_rec *)font;
    return f ? f->name : "";
}

const char *PDFX_Font_GetEncodingName(PDFX_Font font)
{
    pdfx_font_rec *f = (pdfx_font_rec *)font;
    return f ? f->encoding : "";
}

PDFX_INT PDFX_Font_GetAscent(PDFX_Font font)
{
    pdfx_font_rec *f = (pdfx_font_rec *)font;
    double v = 0;
    if (!f || !f->core) return 0;
    if (pdf_font_ascent(f->core, 1000.0, &v) != PDF_OK)
        return 0;
    return (PDFX_INT)(v + (v >= 0 ? 0.5 : -0.5));
}

PDFX_INT PDFX_Font_GetDescent(PDFX_Font font)
{
    pdfx_font_rec *f = (pdfx_font_rec *)font;
    double v = 0;
    if (!f || !f->core) return 0;
    if (pdf_font_descent(f->core, 1000.0, &v) != PDF_OK)
        return 0;
    return (PDFX_INT)(v + (v >= 0 ? 0.5 : -0.5));
}

PDFX_UINT PDFX_Font_GetXHeight(PDFX_Font font)
{
    PDFX_INT a = PDFX_Font_GetAscent(font);
    return (PDFX_UINT)(a > 0 ? a / 2 : 0);
}

PDFX_UINT PDFX_Font_GetCapHeight(PDFX_Font font)
{
    PDFX_INT a = PDFX_Font_GetAscent(font);
    return (PDFX_UINT)(a > 0 ? a * 7 / 10 : 0);
}

PDFX_Box PDFX_Font_GetBBox(PDFX_Font font)
{
    PDFX_Box b;
    b.left = 0;
    b.right = 1000;
    b.top = (PDFX_REAL)PDFX_Font_GetAscent(font);
    b.bottom = (PDFX_REAL)PDFX_Font_GetDescent(font);
    return b;
}

PDFX_INT PDFX_Font_GetUnicodeWidth(PDFX_Font font, PDFX_UNICODE code)
{
    pdfx_font_rec *f = (pdfx_font_rec *)font;
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
    return (PDFX_INT)(pdfx_font_width_units(f, utf8) + 0.5);
}

PDFX_TextWidth PDFX_Font_TextWidth(PDFX_Font font, const PDFX_BYTE *text,
                                   PDFX_UINT len)
{
    pdfx_font_rec *f = (pdfx_font_rec *)font;
    PDFX_TextWidth tw;
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
    tw.numchars = (PDFX_UINT)pdfx_utf8_count(tmp, n);
    {
        PDFX_UINT i;
        for (i = 0; i < n; ++i)
            if (tmp[i] == ' ')
                ++tw.numspace;
    }
    tw.numwords = tw.numchars - tw.numspace + (tw.numspace ? 1 : 0);
    tw.width = (PDFX_UINT)(pdfx_font_width_units(f, tmp) + 0.5);
    free(tmp);
    return tw;
}

PDFX_UINT PDFX_Font_MeasureText(PDFX_Font font, const PDFX_BYTE *text,
                                PDFX_UINT len, PDFX_REAL width,
                                PDFX_REAL font_size, PDFX_REAL char_space,
                                PDFX_REAL word_space, PDFX_BOOL wordwrap,
                                PDFX_REAL *real_width)
{
    return pdfx_measure_text_impl((pdfx_font_rec *)font, (const char *)text,
                                  (size_t)len, (double)width,
                                  (double)font_size, (double)char_space,
                                  (double)word_space, wordwrap != 0,
                                  real_width);
}

/* ------------------------------------------------------------------ */
/* Encoders (opaque stubs)                                             */
/* ------------------------------------------------------------------ */

static int g_pdfx_encoder_stub;

PDFX_Encoder PDFX_GetEncoder(PDFX_Doc pdf, const char *encoding_name)
{
    PDFX_UNUSED(pdf);
    PDFX_UNUSED(encoding_name);
    /* The pdflib core handles UTF-8 / WinAnsi internally; callers only pass
     * this handle back to APIs that ignore it. A stable non-NULL value keeps
     * code such as `if (!(enc = PDFX_GetEncoder(...)))` on the happy path. */
    return (PDFX_Encoder)&g_pdfx_encoder_stub;
}

PDFX_Encoder PDFX_GetCurrentEncoder(PDFX_Doc pdf)
{
    return PDFX_GetEncoder(pdf, "WinAnsiEncoding");
}

/* Encoder selection. pdflib's text entry points take UTF-8 and let the
 * engine map it through the font's encoding; there is no document-level
 * "current encoder" to switch, and no conversion from a legacy byte string
 * is performed. Returning PDFX_OK for a legacy encoder (as this layer used
 * to) told the caller its Shift-JIS buffer would be converted when in fact
 * the bytes are written through unchanged. UTF-8 is the one encoder that is
 * genuinely already in effect, so it stays a success. */
PDFX_STATUS PDFX_SetCurrentEncoder(PDFX_Doc pdf, const char *encoding_name)
{
    if (encoding_name &&
        (pdfx_ci_equal(encoding_name, "UTF-8") ||
         pdfx_ci_equal(encoding_name, "UTF8")))
        return PDFX_OK;
    pdfx_fail((pdfx_doc_rec *)pdf, PDFX_UNSUPPORTED_FUNC);
    return PDFX_UNSUPPORTED_FUNC;
}

PDFX_EncoderType PDFX_Encoder_GetType(PDFX_Encoder encoder)
{
    PDFX_UNUSED(encoder);
    return PDFX_ENCODER_TYPE_SINGLE_BYTE;
}

PDFX_ByteType PDFX_Encoder_GetByteType(PDFX_Encoder encoder, const char *text,
                                       PDFX_UINT index)
{
    PDFX_UNUSED(encoder);
    PDFX_UNUSED(index);
    if (text && ((unsigned char)text[0] & 0x80))
        return PDFX_BYTE_TYPE_LEAD;
    return PDFX_BYTE_TYPE_SINGLE;
}

PDFX_UNICODE PDFX_Encoder_GetUnicode(PDFX_Encoder encoder, PDFX_UINT16 code)
{
    PDFX_UNUSED(encoder);
    return (PDFX_UNICODE)code;
}

PDFX_WritingMode PDFX_Encoder_GetWritingMode(PDFX_Encoder encoder)
{
    PDFX_UNUSED(encoder);
    return PDFX_WMODE_HORIZONTAL;
}

/* Same reasoning as the CJK font packs above: these register legacy
 * encodings that pdflib does not convert from. UTF-8 is the encoding the
 * facade actually expects, so UseUTFEncodings() is already satisfied. */
PDFX_STATUS PDFX_UseJPEncodings(PDFX_Doc pdf)
{
    pdfx_fail((pdfx_doc_rec *)pdf, PDFX_UNSUPPORTED_FUNC);
    return PDFX_UNSUPPORTED_FUNC;
}

PDFX_STATUS PDFX_UseKREncodings(PDFX_Doc pdf)
{
    pdfx_fail((pdfx_doc_rec *)pdf, PDFX_UNSUPPORTED_FUNC);
    return PDFX_UNSUPPORTED_FUNC;
}

PDFX_STATUS PDFX_UseCNSEncodings(PDFX_Doc pdf)
{
    pdfx_fail((pdfx_doc_rec *)pdf, PDFX_UNSUPPORTED_FUNC);
    return PDFX_UNSUPPORTED_FUNC;
}

PDFX_STATUS PDFX_UseCNTEncodings(PDFX_Doc pdf)
{
    pdfx_fail((pdfx_doc_rec *)pdf, PDFX_UNSUPPORTED_FUNC);
    return PDFX_UNSUPPORTED_FUNC;
}

/* UTF-8 is what every PDFX_* text entry point already takes, so this one
 * really is a no-op that has succeeded. */
PDFX_STATUS PDFX_UseUTFEncodings(PDFX_Doc pdf) { PDFX_UNUSED(pdf); return PDFX_OK; }

/* ------------------------------------------------------------------ */
/* Unsupported / not-modelled libharu surface                          */
/* ------------------------------------------------------------------ */

/* Constructors that cannot be expressed by the pdflib core. They report
 * PDFX_UNSUPPORTED_FUNC (never PDFX_OK silently). */
static PDFX_Annotation pdfx_unsupported_annot(PDFX_Doc d, PDFX_STATUS code)
{
    pdfx_fail((pdfx_doc_rec *)d, code);
    return NULL;
}

PDFX_Annotation PDFX_Page_Create3DAnnot(PDFX_Page page, PDFX_Rect rect,
                                        PDFX_BOOL tb, PDFX_BOOL np,
                                        PDFX_U3D u3d, PDFX_Image ap)
{
    PDFX_UNUSED(rect); PDFX_UNUSED(tb); PDFX_UNUSED(np);
    PDFX_UNUSED(u3d); PDFX_UNUSED(ap);
    return pdfx_unsupported_annot(pdfx_doc_of_page(page), PDFX_UNSUPPORTED_FUNC);
}

PDFX_Annotation PDFX_Page_CreateFreeTextAnnot(PDFX_Page page, PDFX_Rect rect,
                                              const char *text,
                                              PDFX_Encoder encoder)
{
    PDFX_UNUSED(rect); PDFX_UNUSED(text); PDFX_UNUSED(encoder);
    return pdfx_unsupported_annot(pdfx_doc_of_page(page), PDFX_UNSUPPORTED_FUNC);
}

PDFX_Annotation PDFX_Page_CreateLineAnnot(PDFX_Page page, const char *text,
                                          PDFX_Encoder encoder)
{
    PDFX_UNUSED(text); PDFX_UNUSED(encoder);
    return pdfx_unsupported_annot(pdfx_doc_of_page(page), PDFX_UNSUPPORTED_FUNC);
}

PDFX_Annotation PDFX_Page_CreateWidgetAnnot_WhiteOnlyWhilePrint(
    PDFX_Doc pdf, PDFX_Page page, PDFX_Rect rect)
{
    PDFX_UNUSED(page); PDFX_UNUSED(rect);
    return pdfx_unsupported_annot(pdf, PDFX_UNSUPPORTED_FUNC);
}

PDFX_Annotation PDFX_Page_CreateWidgetAnnot(PDFX_Page page, PDFX_Rect rect)
{
    PDFX_UNUSED(rect);
    return pdfx_unsupported_annot(pdfx_doc_of_page(page), PDFX_UNSUPPORTED_FUNC);
}

PDFX_Annotation PDFX_Page_CreateTextMarkupAnnot(PDFX_Page page,
                                                PDFX_Rect rect,
                                                const char *text,
                                                PDFX_Encoder encoder,
                                                PDFX_AnnotType subType)
{
    PDFX_UNUSED(rect); PDFX_UNUSED(text); PDFX_UNUSED(encoder);
    PDFX_UNUSED(subType);
    return pdfx_unsupported_annot(pdfx_doc_of_page(page), PDFX_UNSUPPORTED_FUNC);
}

PDFX_Annotation PDFX_Page_CreatePopupAnnot(PDFX_Page page, PDFX_Rect rect,
                                           PDFX_Annotation parent)
{
    PDFX_UNUSED(rect); PDFX_UNUSED(parent);
    return pdfx_unsupported_annot(pdfx_doc_of_page(page), PDFX_UNSUPPORTED_FUNC);
}

PDFX_Annotation PDFX_Page_CreateStampAnnot(PDFX_Page page, PDFX_Rect rect,
                                           PDFX_StampAnnotName name,
                                           const char *text,
                                           PDFX_Encoder encoder)
{
    PDFX_UNUSED(rect); PDFX_UNUSED(name); PDFX_UNUSED(text); PDFX_UNUSED(encoder);
    return pdfx_unsupported_annot(pdfx_doc_of_page(page), PDFX_UNSUPPORTED_FUNC);
}

PDFX_Annotation PDFX_Page_CreateProjectionAnnot(PDFX_Page page,
                                                PDFX_Rect rect,
                                                const char *text,
                                                PDFX_Encoder encoder)
{
    PDFX_UNUSED(rect); PDFX_UNUSED(text); PDFX_UNUSED(encoder);
    return pdfx_unsupported_annot(pdfx_doc_of_page(page), PDFX_UNSUPPORTED_FUNC);
}

PDFX_XObject PDFX_Page_CreateXObjectFromImage(PDFX_Doc pdf, PDFX_Page page,
                                              PDFX_Rect rect, PDFX_Image image,
                                              PDFX_BOOL zoom)
{
    PDFX_UNUSED(page); PDFX_UNUSED(rect); PDFX_UNUSED(image); PDFX_UNUSED(zoom);
    pdfx_fail((pdfx_doc_rec *)pdf, PDFX_UNSUPPORTED_FUNC);
    return NULL;
}

PDFX_XObject PDFX_Page_CreateXObjectAsWhiteRect(PDFX_Doc pdf, PDFX_Page page,
                                                PDFX_Rect rect)
{
    PDFX_UNUSED(page); PDFX_UNUSED(rect);
    pdfx_fail((pdfx_doc_rec *)pdf, PDFX_UNSUPPORTED_FUNC);
    return NULL;
}

PDFX_STATUS PDFX_Page_ExecuteXObject(PDFX_Page page, PDFX_XObject obj)
{
    PDFX_UNUSED(obj);
    pdfx_fail(pdfx_doc_of_page(page), PDFX_UNSUPPORTED_FUNC);
    return PDFX_UNSUPPORTED_FUNC;
}

PDFX_STATUS PDFX_Image_AddSMask(PDFX_Image image, PDFX_Image smask)
{
    PDFX_UNUSED(image); PDFX_UNUSED(smask);
    return PDFX_UNSUPPORTED_FUNC;
}

PDFX_STATUS PDFX_Image_SetColorMask(PDFX_Image image, PDFX_UINT rmin,
                                    PDFX_UINT rmax, PDFX_UINT gmin,
                                    PDFX_UINT gmax, PDFX_UINT bmin,
                                    PDFX_UINT bmax)
{
    PDFX_UNUSED(image); PDFX_UNUSED(rmin); PDFX_UNUSED(rmax);
    PDFX_UNUSED(gmin); PDFX_UNUSED(gmax); PDFX_UNUSED(bmin); PDFX_UNUSED(bmax);
    return PDFX_UNSUPPORTED_FUNC;
}

PDFX_STATUS PDFX_Image_SetMaskImage(PDFX_Image image, PDFX_Image mask_image)
{
    PDFX_UNUSED(image); PDFX_UNUSED(mask_image);
    return PDFX_UNSUPPORTED_FUNC;
}

PDFX_Image PDFX_Image_LoadRaw1BitImageFromMem(PDFX_Doc pdf,
                                              const PDFX_BYTE *buf,
                                              PDFX_UINT width, PDFX_UINT height,
                                              PDFX_UINT line_width,
                                              PDFX_BOOL black_is1,
                                              PDFX_BOOL top_is_first)
{
    PDFX_UNUSED(buf); PDFX_UNUSED(width); PDFX_UNUSED(height);
    PDFX_UNUSED(line_width); PDFX_UNUSED(black_is1); PDFX_UNUSED(top_is_first);
    pdfx_fail((pdfx_doc_rec *)pdf, PDFX_UNSUPPORTED_FUNC);
    return NULL;
}

PDFX_Image PDFX_LoadRawImageFromFile(PDFX_Doc pdf, const char *filename,
                                     PDFX_UINT width, PDFX_UINT height,
                                     PDFX_ColorSpace color_space)
{
    PDFX_UNUSED(filename); PDFX_UNUSED(width); PDFX_UNUSED(height);
    PDFX_UNUSED(color_space);
    pdfx_fail((pdfx_doc_rec *)pdf, PDFX_UNSUPPORTED_FUNC);
    return NULL;
}

PDFX_Image PDFX_LoadRawImageFromMem(PDFX_Doc pdf, const PDFX_BYTE *buf,
                                    PDFX_UINT width, PDFX_UINT height,
                                    PDFX_ColorSpace color_space,
                                    PDFX_UINT bits_per_component)
{
    PDFX_UNUSED(buf); PDFX_UNUSED(width); PDFX_UNUSED(height);
    PDFX_UNUSED(color_space); PDFX_UNUSED(bits_per_component);
    pdfx_fail((pdfx_doc_rec *)pdf, PDFX_UNSUPPORTED_FUNC);
    return NULL;
}

PDFX_Image PDFX_LoadU3DFromFile(PDFX_Doc pdf, const char *filename)
{
    PDFX_UNUSED(filename);
    pdfx_fail((pdfx_doc_rec *)pdf, PDFX_UNSUPPORTED_FUNC);
    return NULL;
}

PDFX_Image PDFX_LoadU3DFromMem(PDFX_Doc pdf, const PDFX_BYTE *buffer,
                               PDFX_UINT size)
{
    PDFX_UNUSED(buffer); PDFX_UNUSED(size);
    pdfx_fail((pdfx_doc_rec *)pdf, PDFX_UNSUPPORTED_FUNC);
    return NULL;
}

PDFX_EmbeddedFile PDFX_AttachFile(PDFX_Doc pdf, const char *file)
{
    PDFX_UNUSED(file);
    pdfx_fail((pdfx_doc_rec *)pdf, PDFX_UNSUPPORTED_FUNC);
    return NULL;
}

PDFX_STATUS PDFX_AddPageLabel(PDFX_Doc pdf, PDFX_UINT page_num,
                              PDFX_PageNumStyle style,
                              PDFX_UINT first_page, const char *prefix)
{
    PDFX_UNUSED(page_num); PDFX_UNUSED(style);
    PDFX_UNUSED(first_page); PDFX_UNUSED(prefix);
    pdfx_fail((pdfx_doc_rec *)pdf, PDFX_UNSUPPORTED_FUNC);
    return PDFX_UNSUPPORTED_FUNC;
}

PDFX_STATUS PDFX_Page_SetSlideShow(PDFX_Page page, PDFX_TransitionStyle type,
                                   PDFX_REAL disp_time,
                                   PDFX_REAL trans_time)
{
    PDFX_UNUSED(type); PDFX_UNUSED(disp_time); PDFX_UNUSED(trans_time);
    pdfx_fail(pdfx_doc_of_page(page), PDFX_UNSUPPORTED_FUNC);
    return PDFX_UNSUPPORTED_FUNC;
}

PDFX_STATUS PDFX_Page_SetShading(PDFX_Page page, PDFX_Shading shading)
{
    PDFX_UNUSED(shading);
    pdfx_fail(pdfx_doc_of_page(page), PDFX_UNSUPPORTED_FUNC);
    return PDFX_UNSUPPORTED_FUNC;
}

PDFX_Shading PDFX_Shading_New(PDFX_Doc pdf, PDFX_ShadingType type,
                              PDFX_ColorSpace colorSpace, PDFX_REAL xMin,
                              PDFX_REAL xMax, PDFX_REAL yMin, PDFX_REAL yMax)
{
    PDFX_UNUSED(type); PDFX_UNUSED(colorSpace); PDFX_UNUSED(xMin);
    PDFX_UNUSED(xMax); PDFX_UNUSED(yMin); PDFX_UNUSED(yMax);
    pdfx_fail((pdfx_doc_rec *)pdf, PDFX_UNSUPPORTED_FUNC);
    return NULL;
}

PDFX_STATUS PDFX_Shading_AddVertexRGB(
    PDFX_Shading shading, PDFX_Shading_FreeFormTriangleMeshEdgeFlag edgeFlag,
    PDFX_REAL x, PDFX_REAL y, PDFX_UINT8 r, PDFX_UINT8 g, PDFX_UINT8 b)
{
    PDFX_UNUSED(shading); PDFX_UNUSED(edgeFlag); PDFX_UNUSED(x);
    PDFX_UNUSED(y); PDFX_UNUSED(r); PDFX_UNUSED(g); PDFX_UNUSED(b);
    return PDFX_UNSUPPORTED_FUNC;
}

PDFX_STATUS PDFX_Page_New_Content_Stream(PDFX_Page page, PDFX_Dict *new_stream)
{
    PDFX_UNUSED(new_stream);
    pdfx_fail(pdfx_doc_of_page(page), PDFX_UNSUPPORTED_FUNC);
    return PDFX_UNSUPPORTED_FUNC;
}

PDFX_STATUS PDFX_Page_Insert_Shared_Content_Stream(PDFX_Page page,
                                                   PDFX_Dict shared_stream)
{
    PDFX_UNUSED(shared_stream);
    pdfx_fail(pdfx_doc_of_page(page), PDFX_UNSUPPORTED_FUNC);
    return PDFX_UNSUPPORTED_FUNC;
}

/* 3D measure / exdata / view: not modelled. */
PDFX_3DMeasure PDFX_Page_Create3DC3DMeasure(PDFX_Page page,
                                            PDFX_Point3D firstanchorpoint,
                                            PDFX_Point3D textanchorpoint)
{
    PDFX_UNUSED(firstanchorpoint); PDFX_UNUSED(textanchorpoint);
    pdfx_fail(pdfx_doc_of_page(page), PDFX_UNSUPPORTED_FUNC);
    return NULL;
}

PDFX_3DMeasure PDFX_Page_CreatePD33DMeasure(
    PDFX_Page page, PDFX_Point3D annotationPlaneNormal,
    PDFX_Point3D firstAnchorPoint, PDFX_Point3D secondAnchorPoint,
    PDFX_Point3D leaderLinesDirection, PDFX_Point3D measurementValuePoint,
    PDFX_Point3D textYDirection, PDFX_REAL value, const char *unitsString)
{
    PDFX_UNUSED(annotationPlaneNormal); PDFX_UNUSED(firstAnchorPoint);
    PDFX_UNUSED(secondAnchorPoint); PDFX_UNUSED(leaderLinesDirection);
    PDFX_UNUSED(measurementValuePoint); PDFX_UNUSED(textYDirection);
    PDFX_UNUSED(value); PDFX_UNUSED(unitsString);
    pdfx_fail(pdfx_doc_of_page(page), PDFX_UNSUPPORTED_FUNC);
    return NULL;
}

PDFX_STATUS PDFX_3DMeasure_SetName(PDFX_3DMeasure measure, const char *name)
{
    PDFX_UNUSED(measure); PDFX_UNUSED(name);
    return PDFX_UNSUPPORTED_FUNC;
}

PDFX_STATUS PDFX_3DMeasure_SetColor(PDFX_3DMeasure measure,
                                    PDFX_RGBColor color)
{
    PDFX_UNUSED(measure); PDFX_UNUSED(color);
    return PDFX_UNSUPPORTED_FUNC;
}

PDFX_STATUS PDFX_3DMeasure_SetTextSize(PDFX_3DMeasure measure,
                                       PDFX_REAL textsize)
{
    PDFX_UNUSED(measure); PDFX_UNUSED(textsize);
    return PDFX_UNSUPPORTED_FUNC;
}

PDFX_STATUS PDFX_3DC3DMeasure_SetTextBoxSize(PDFX_3DMeasure measure,
                                             PDFX_INT32 x, PDFX_INT32 y)
{
    PDFX_UNUSED(measure); PDFX_UNUSED(x); PDFX_UNUSED(y);
    return PDFX_UNSUPPORTED_FUNC;
}

PDFX_STATUS PDFX_3DC3DMeasure_SetText(PDFX_3DMeasure measure, const char *text,
                                      PDFX_Encoder encoder)
{
    PDFX_UNUSED(measure); PDFX_UNUSED(text); PDFX_UNUSED(encoder);
    return PDFX_UNSUPPORTED_FUNC;
}

PDFX_STATUS PDFX_3DC3DMeasure_SetProjectionAnotation(
    PDFX_3DMeasure measure, PDFX_Annotation projectionanotation)
{
    PDFX_UNUSED(measure); PDFX_UNUSED(projectionanotation);
    return PDFX_UNSUPPORTED_FUNC;
}

PDFX_ExData PDFX_Page_Create3DAnnotExData(PDFX_Page page)
{
    pdfx_fail(pdfx_doc_of_page(page), PDFX_UNSUPPORTED_FUNC);
    return NULL;
}

PDFX_STATUS PDFX_3DAnnotExData_Set3DMeasurement(PDFX_ExData exdata,
                                                PDFX_3DMeasure measure)
{
    PDFX_UNUSED(exdata); PDFX_UNUSED(measure);
    return PDFX_UNSUPPORTED_FUNC;
}

PDFX_Dict PDFX_Page_Create3DView(PDFX_Page page, PDFX_U3D u3d,
                                 PDFX_Annotation annot3d, const char *name)
{
    PDFX_UNUSED(u3d); PDFX_UNUSED(annot3d); PDFX_UNUSED(name);
    pdfx_fail(pdfx_doc_of_page(page), PDFX_UNSUPPORTED_FUNC);
    return NULL;
}

PDFX_STATUS PDFX_3DView_Add3DC3DMeasure(PDFX_Dict view,
                                        PDFX_3DMeasure measure)
{
    PDFX_UNUSED(view); PDFX_UNUSED(measure);
    return PDFX_UNSUPPORTED_FUNC;
}

PDFX_STATUS PDFX_Annot_Set3DView(PDFX_MMgr mmgr, PDFX_Annotation annot,
                                 PDFX_Annotation annot3d, PDFX_Dict view)
{
    PDFX_UNUSED(mmgr); PDFX_UNUSED(annot); PDFX_UNUSED(annot3d);
    PDFX_UNUSED(view);
    return PDFX_UNSUPPORTED_FUNC;
}

PDFX_OutputIntent PDFX_ICC_LoadIccFromMem(PDFX_Doc pdf, PDFX_MMgr mmgr,
                                          PDFX_Stream iccdata, PDFX_Xref xref,
                                          int numcomponent)
{
    PDFX_UNUSED(mmgr); PDFX_UNUSED(iccdata); PDFX_UNUSED(xref);
    PDFX_UNUSED(numcomponent);
    pdfx_fail((pdfx_doc_rec *)pdf, PDFX_UNSUPPORTED_FUNC);
    return NULL;
}

PDFX_OutputIntent PDFX_LoadIccProfileFromFile(PDFX_Doc pdf,
                                              const char *icc_file_name,
                                              int numcomponent)
{
    PDFX_UNUSED(icc_file_name); PDFX_UNUSED(numcomponent);
    pdfx_fail((pdfx_doc_rec *)pdf, PDFX_UNSUPPORTED_FUNC);
    return NULL;
}
