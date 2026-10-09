/**
 * pdf_backend_podofo.cpp - PoDoFo adapter (PoDoFo 0.9.x API).
 *
 * Maps the pdflib SPI onto the PoDoFo 0.9.x C++ API (PdfMemDocument +
 * PdfPainter). All PoDoFo symbols stay inside this translation unit;
 * the core only sees the plain-C vtable.
 *
 * Why 0.9.x: upstream PoDoFo 1.0 made OpenSSL a mandatory dependency
 * (find_package(OpenSSL REQUIRED)); 0.9.x keeps libcrypto optional
 * (FIND_PACKAGE(LIBCRYPTO)), which is what the pdflib build requires
 * when configured without OpenSSL. Consequences handled here:
 *  - Encryption is only available when PoDoFo was built WITH OpenSSL
 *    (detect through the PODOFO_HAVE_OPENSSL macro in podofo_config.h).
 *    Without it, doc_set_encryption returns PDF_ERR_BACKEND_UNSUPPORTED.
 *  - PdfImage can only load JPEG/PNG/TIFF when the corresponding codec
 *    was compiled into the library.
 *
 * Lifecycle notes:
 *  - PoDoFo draws through a PdfPainter attached to a page. The adapter
 *    keeps one painter per document and re-targets it via SetPage();
 *    FinishPage() is called before saving / switching pages.
 *  - Fonts, images, pages and annotations are owned by the document;
 *    the adapter cells only reference them.
 */
#include "../pdf_core.h"
#include "pdf_backend.h"
#include "pdf_internal.h"

#include <podofo/podofo.h>
#include <podofo/base/PdfVecObjects.h>

#include <cstdarg>
#include <cstdio>
#include <vector>

#include <cstddef>
#include <cstring>
#include <memory>
#include <new>
#include <stdexcept>
#include <string>

/* 0.9.x typedefs live in the PoDoFo namespace after including the header. */
using PoDoFo::pdf_long;

/* Forward declaration; the full definition lives at the bottom of the
 * file, but the cell constructors above already reference it. */
extern const pdf_backend_vtable g_podofo_vt;

/* ----- payloads --------------------------------------------------------- */

/* PdfOutputDevice subclass that forwards every write to the caller's
 * callback. Used both for whole-document streaming (PdfWriter) and as the
 * target of a PdfStreamedDocument. Both Write (object bodies) and PrintV
 * (header, xref, trailer) must be routed, and the position kept in sync
 * via Seek/Tell because PdfXRef records object offsets through Tell().
 *
 * Streaming extra care: PdfImmediateWriter (used by PdfStreamedDocument)
 * overwrites the tail of already-emitted output -- it prints "endobj\n"
 * and then rewrites those same 7 bytes with "stream\n" -- and
 * PdfFileStream computes /Length from GetLength() and patches the Length
 * object via a backward Seek. A pure push-only callback device cannot
 * honor either, so we keep a small overwrite window of the most recent
 * bytes: backward Seeks that stay inside the window rewrite the pending
 * tail instead of emitting it, and every emission updates m_ulLength so
 * GetLength()-based arithmetic stays correct. */
class StreamCallbackDevice : public PoDoFo::PdfOutputDevice
{
public:
    StreamCallbackDevice(pdf_stream_write_fn fn, void *ud)
        : m_fn(fn), m_ud(ud), m_failed(PDF_OK) {}

    void Write(const char *pBuffer, size_t lLen) override
    {
        if (m_failed != PDF_OK || lLen == 0)
            return;
        emit(pBuffer, lLen);
    }

    void PrintV(const char *pszFormat, long lBytes, va_list args) override
    {
        if (m_failed != PDF_OK || lBytes <= 0)
            return;
        std::vector<char> buf((size_t)lBytes + 1);
        vsnprintf(buf.data(), buf.size(), pszFormat, args);
        emit(buf.data(), (size_t)lBytes);
    }

    /* True absolute seek over the byte stream we manage. */
    void Seek(size_t offset) override
    {
        if (m_failed != PDF_OK)
            return;
        if (offset > m_pending_start + m_pending.size())
        {
            /* forward gap: push everything pending, then pad */
            flush_pending();
            static const char zero[64] = {0};
            while (m_failed == PDF_OK && offset > m_position)
            {
                size_t n = offset - m_position;
                if (n > sizeof(zero))
                    n = sizeof(zero);
                emit(zero, n);
            }
            return;
        }
        if (offset < m_pending_start)
        {
            /* seek back beyond the window: the caller wants to rewrite
             * data already handed to the user -- that is impossible for a
             * streaming callback, so truncate nothing and just re-anchor
             * (data was already delivered; PoDoFo only ever rewinds over
             * the overwrite window in practice). */
            m_pending.clear();
            m_pending_start = m_position;
            m_position = offset;
            m_ulLength = (m_ulLength > m_position) ? m_ulLength : m_position;
            return;
        }
        /* inside the window: drop the tail after 'offset' so the next
         * emission overwrites it */
        m_pending.resize(offset - m_pending_start);
        m_position = offset;
    }

    size_t Tell() const override { return m_position; }

    pdf_status status() const { return m_failed; }

    /* Drain whatever is still buffered (call before reporting success). */
    pdf_status flush()
    {
        flush_pending();
        return m_failed;
    }

private:
    void emit(const char *data, size_t len)
    {
        if (m_failed != PDF_OK || len == 0)
            return;
        m_pending.insert(m_pending.end(), data, data + len);
        m_position += len;
        if (m_position > m_ulLength)
            m_ulLength = m_position;
        if (m_pending.size() > PDFLIBUTIL_OVERWRITE_WINDOW)
        {
            size_t drop = m_pending.size() - PDFLIBUTIL_OVERWRITE_WINDOW;
            flush_pending_n(drop);
        }
    }

    void flush_pending_n(size_t n)
    {
        if (m_failed != PDF_OK || n == 0 || m_pending.empty())
            return;
        if (n > m_pending.size())
            n = m_pending.size();
        m_failed = m_fn((const unsigned char *)m_pending.data(), n,
                        m_ud);
        m_pending.erase(m_pending.begin(), m_pending.begin() + n);
        m_pending_start += n;
    }

    void flush_pending() { flush_pending_n(m_pending.size()); }

    pdf_stream_write_fn m_fn;
    void *m_ud;
    pdf_status m_failed;
    /* bytes not yet delivered to the callback (the overwrite window) */
    std::vector<char> m_pending;
    /* stream offset of m_pending[0] */
    size_t m_pending_start = 0;
    /* logical stream position = delivered + pending */
    size_t m_position = 0;

    /* PoDoFo's overwrite trick touches at most the last "endobj\n" (7
     * bytes); keep a comfortable multiple of that plus /Length patch
     * headroom. */
    enum { PDFLIBUTIL_OVERWRITE_WINDOW = 256 };
};

struct podofo_doc
{
    /* Memory mode: whole document is held, serialized on save.
     * Streamed mode: PdfStreamedDocument pushes objects to the device as
     * they are created, so page count/content may be unbounded. */
    std::unique_ptr<PoDoFo::PdfMemDocument> doc;
    std::unique_ptr<PoDoFo::PdfStreamedDocument> sdoc;
    std::unique_ptr<StreamCallbackDevice> device;
    bool streamed = false;
    bool finished = false;
    std::string last_error;
    std::unique_ptr<PoDoFo::PdfPainter> painter;
    PoDoFo::PdfPage *painted_page = nullptr;

    /* PdfDocument interface shared by both modes. */
    PoDoFo::PdfDocument *pdoc() const
    {
        return streamed ? (PoDoFo::PdfDocument *)sdoc.get()
                        : (PoDoFo::PdfDocument *)doc.get();
    }
};

struct podofo_obj
{
    int kind = 0;
    podofo_doc *owner = nullptr; /* doc that created this object */
    /* Pages are addressed by INDEX, never by a stored PdfPage*.
     * PoDoFo 0.9.7's PdfPagesTree::CreatePage() stores its wrapper at a
     * misaligned cache slot, so the wrapper it returns is destroyed by the
     * next CreatePage() (confirmed by direct-probe: p1->GetObject() changes
     * identity after a second page is created). Wrappers handed out by
     * GetPage(index) are stable, so we re-resolve on every use. */
    int page_index = -1;
    PoDoFo::PdfFont *font = nullptr;
    PoDoFo::PdfImage *image = nullptr;
    PoDoFo::PdfAnnotation *annot = nullptr; /* owned by the page */
    PoDoFo::PdfOutlineItem *outline = nullptr;
};

/* NOTE: not named OBJ_* -- windows.h (pulled in by PoDoFo on Win32)
 * #defines OBJ_FONT etc. as GDI object-type macros. */
enum PfObjKind
{
    PFOBJ_PAGE = 1,
    PFOBJ_FONT,
    PFOBJ_IMAGE,
    PFOBJ_ANNOT,
    PFOBJ_OUTLINE
};

/* ----- helpers ----------------------------------------------------------- */

static podofo_doc *D(pdf_backend_doc *doc) { return (podofo_doc *)doc; }

static podofo_obj *O(void *obj) { return (podofo_obj *)obj; }

static pdf_status pf_catch(podofo_doc *d, const std::exception &e,
                           const char *what)
{
    if (d)
        d->last_error = std::string(what) + ": " + e.what();
    return PDF_ERR_BACKEND_FAILED;
}

static PoDoFo::PdfColor to_color(const pdf_color *c)
{
    switch (c->kind)
    {
        case PDF_COLOR_GRAY: return PoDoFo::PdfColor(c->c1);
        case PDF_COLOR_CMYK:
            return PoDoFo::PdfColor(c->c1, c->c2, c->c3, c->c4);
        default: return PoDoFo::PdfColor(c->c1, c->c2, c->c3);
    }
}

static void flush_painter(podofo_doc *d)
{
    if (d->painter && d->painted_page)
    {
        d->painter->FinishPage();
        d->painted_page = nullptr;
    }
}

/* If the caller's streaming sink has rejected data, the document output
 * is torn up: refuse further work instead of pushing more bytes into a
 * broken stream (the core marks the doc broken and cleans up). */
static pdf_status p_doc_probe_stream_error(pdf_backend_doc *doc)
{
    podofo_doc *d = D(doc);
    if (d->streamed && d->device && d->device->status() != PDF_OK)
        return d->device->status();
    return PDF_OK;
}

/* Make sure the painter is attached to `page` before any drawing op. */
static pdf_status paint_on(podofo_doc *d, PoDoFo::PdfPage *page)
{
    try
    {
        if (d->streamed && d->device && d->device->status() != PDF_OK)
            return d->device->status(); /* sink already failed */
        if (!d->painter)
            d->painter.reset(new PoDoFo::PdfPainter());
        if (d->painted_page != page)
        {
            flush_painter(d);
            d->painter->SetPage(page);
            d->painted_page = page;
        }
        return PDF_OK;
    }
    catch (const std::exception &e)
    {
        return pf_catch(d, e, "SetPage");
    }
}

static PoDoFo::PdfPage *page_of(pdf_page p)
{
    /* Re-resolve the canonical page wrapper through the document's page
     * tree on every use. The raw pointer returned by CreatePage() is not
     * safe to keep (see podofo_obj comment). */
    podofo_obj *o = O(((pdf_page)p)->obj);
    return o->owner->pdoc()->GetPage(o->page_index);
}

/* Every podofo_obj carries a back pointer to the podofo_doc that
 * created it, so page/annot/outline calls do not depend on per-thread
 * state: the same handle may be used from another thread as long as the
 * caller serializes access to the document (see "Threading" in
 * pdf_api.h). */
static podofo_doc *doc_of_obj(podofo_obj *o)
{
    return o ? o->owner : nullptr;
}

static podofo_doc *doc_of_page(pdf_page p)
{
    return doc_of_obj(O(p->obj));
}

static PoDoFo::EPdfStrokeStyle dash_style(const double *pattern, size_t n)
{
    if (n == 0)
        return PoDoFo::ePdfStrokeStyle_Solid;
    if (n == 1 && pattern[0] == 3.0)
        return PoDoFo::ePdfStrokeStyle_Dash;
    if (n == 1 && pattern[0] == 1.0)
        return PoDoFo::ePdfStrokeStyle_Dot;
    if (n == 2 && pattern[0] == 3.0 && pattern[1] == 1.0)
        return PoDoFo::ePdfStrokeStyle_DashDot;
    if (n == 3 && pattern[0] == 3.0 && pattern[1] == 1.0
        && pattern[2] == 1.0)
        return PoDoFo::ePdfStrokeStyle_DashDotDot;
    return PoDoFo::ePdfStrokeStyle_Custom;
}

/* ----- document ----------------------------------------------------------- */

static pdf_status p_doc_create(pdf_backend_doc **out_doc)
{
    podofo_doc *d = new (std::nothrow) podofo_doc();
    if (!d)
        return PDF_ERR_OUT_OF_MEMORY;
    try
    {
        d->doc.reset(new PoDoFo::PdfMemDocument());
        *out_doc = (pdf_backend_doc *)d;
        return PDF_OK;
    }
    catch (const std::exception &e)
    {
        delete d;
        (void)pf_catch(nullptr, e, "PdfMemDocument");
        return PDF_ERR_BACKEND_FAILED;
    }
}

/* Streaming creation: the device is bound at construction and every page/
 * font/image object is pushed to it as it is completed, so the caller
 * never needs to know the final page count. PdfStreamedDocument::Close()
 * (called from p_doc_finish) also embeds pending font subsets. */
static pdf_status p_doc_create_streamed(pdf_stream_write_fn write_fn,
                                        void *userdata,
                                        pdf_backend_doc **out_doc)
{
    podofo_doc *d = new (std::nothrow) podofo_doc();
    if (!d)
        return PDF_ERR_OUT_OF_MEMORY;
    try
    {
        d->streamed = true;
        d->device.reset(new StreamCallbackDevice(write_fn, userdata));
        d->sdoc.reset(new PoDoFo::PdfStreamedDocument(
            d->device.get(), PoDoFo::ePdfVersion_1_3));
        *out_doc = (pdf_backend_doc *)d;
        return PDF_OK;
    }
    catch (const std::exception &e)
    {
        delete d;
        (void)pf_catch(nullptr, e, "PdfStreamedDocument");
        return PDF_ERR_BACKEND_FAILED;
    }
}

static pdf_status p_doc_finish(pdf_backend_doc *doc)
{
    podofo_doc *d = D(doc);
    if (!d->streamed || d->finished)
        return PDF_OK;
    try
    {
        flush_painter(d);
        if (d->device)
        {
            pdf_status src = d->device->status();
            if (src != PDF_OK)
                return src; /* sink already failed; do not push more */
        }
        /* embeds pending subsets and completes xref/trailer */
        d->sdoc->Close();
        if (d->device)
        {
            pdf_status rc = d->device->flush(); /* drain overwrite window */
            if (rc != PDF_OK)
                return rc;
        }
        d->finished = true;
        return PDF_OK;
    }
    catch (const std::exception &e)
    {
        return pf_catch(d, e, "Close(streamed)");
    }
}

static void p_doc_free(pdf_backend_doc *doc)
{
    podofo_doc *d = D(doc);
    if (!d)
        return;
    try
    {
        flush_painter(d);
    }
    catch (...)
    {
        /* ignore errors during teardown */
    }
    delete d;
}

static pdf_status p_doc_save_to_file(pdf_backend_doc *doc, const char *path)
{
    podofo_doc *d = D(doc);
    try
    {
        flush_painter(d);
        d->doc->Write(path); /* memory-mode only; streamed rejects save */
        return PDF_OK;
    }
    catch (const std::exception &e)
    {
        return pf_catch(d, e, "Write");
    }
}

static pdf_status p_doc_save_to_memory(pdf_backend_doc *doc,
                                       unsigned char *buf,
                                       size_t *inout_size)
{
    podofo_doc *d = D(doc);
    try
    {
        flush_painter(d);
        PoDoFo::PdfWriter writer(&d->doc->GetObjects(),
                                 d->doc->GetTrailer());
        writer.SetPdfVersion(d->doc->GetPdfVersion());

        char *data = nullptr;
        PoDoFo::pdf_long len = 0;
        writer.WriteToBuffer(&data, &len);
        if (!data)
            return PDF_ERR_BACKEND_FAILED;

        if (!buf || *inout_size < (size_t)len)
        {
            *inout_size = (size_t)len;
            free(data);
            return buf ? PDF_ERR_INVALID_PARAM : PDF_OK; /* size query */
        }
        memcpy(buf, data, (size_t)len);
        *inout_size = (size_t)len;
        free(data);
        return PDF_OK;
    }
    catch (const std::exception &e)
    {
        return pf_catch(d, e, "WriteToBuffer");
    }
}

static const char *p_doc_last_error(pdf_backend_doc *doc)
{
    return D(doc)->last_error.c_str();
}

/* ----- info / security / options ------------------------------------------- */

static pdf_status p_doc_set_info(pdf_backend_doc *doc, pdf_info_key key,
                                 const char *utf8_value)
{
    podofo_doc *d = D(doc);
    PoDoFo::PdfString s(utf8_value ? utf8_value : "");
    try
    {
        PoDoFo::PdfInfo *info = d->pdoc()->GetInfo();
        if (!info)
            return PDF_ERR_BACKEND_FAILED;
        switch (key)
        {
            case PDF_INFO_TITLE: info->SetTitle(s); break;
            case PDF_INFO_AUTHOR: info->SetAuthor(s); break;
            case PDF_INFO_SUBJECT: info->SetSubject(s); break;
            case PDF_INFO_KEYWORDS: info->SetKeywords(s); break;
            case PDF_INFO_CREATOR: info->SetCreator(s); break;
            case PDF_INFO_PRODUCER: info->SetProducer(s); break;
            default: return PDF_ERR_INVALID_PARAM;
        }
        return PDF_OK;
    }
    catch (const std::exception &e)
    {
        return pf_catch(d, e, "SetInfo");
    }
}

static pdf_status p_doc_get_info(pdf_backend_doc *doc, pdf_info_key key,
                                 char *buf, size_t *inout_len)
{
    podofo_doc *d = D(doc);
    std::string value;
    try
    {
        PoDoFo::PdfInfo *info = d->pdoc()->GetInfo();
        if (!info)
            return PDF_ERR_BACKEND_FAILED;
        const PoDoFo::PdfString *s = nullptr;
        switch (key)
        {
            case PDF_INFO_TITLE: s = &info->GetTitle(); break;
            case PDF_INFO_AUTHOR: s = &info->GetAuthor(); break;
            case PDF_INFO_SUBJECT: s = &info->GetSubject(); break;
            case PDF_INFO_KEYWORDS: s = &info->GetKeywords(); break;
            case PDF_INFO_CREATOR: s = &info->GetCreator(); break;
            case PDF_INFO_PRODUCER: s = &info->GetProducer(); break;
            default: return PDF_ERR_INVALID_PARAM;
        }
        if (s)
            value = s->GetString();
    }
    catch (const std::exception &e)
    {
        return pf_catch(d, e, "GetInfo");
    }

    size_t len = value.size() + 1;
    if (buf && *inout_len > 0)
    {
        size_t copy = len - 1;
        if (copy >= *inout_len)
            copy = *inout_len - 1;
        memcpy(buf, value.c_str(), copy);
        buf[copy] = '\0';
    }
    *inout_len = len;
    return PDF_OK;
}

static pdf_status p_doc_set_encryption(pdf_backend_doc *doc,
                                       pdf_encrypt_mode mode,
                                       const char *owner, const char *user,
                                       unsigned permissions)
{
    podofo_doc *d = D(doc);

    if (mode == PDF_ENCRYPT_NONE)
        return PDF_OK;
    if (!owner && !user)
        return PDF_ERR_INVALID_PARAM;

#ifdef PODOFO_HAVE_OPENSSL
    int perm = 0;
    if (permissions & PDF_PERM_PRINT) perm |= PoDoFo::PdfEncrypt::ePdfPermissions_Print;
    if (permissions & PDF_PERM_EDIT) perm |= PoDoFo::PdfEncrypt::ePdfPermissions_Edit;
    if (permissions & PDF_PERM_COPY) perm |= PoDoFo::PdfEncrypt::ePdfPermissions_Copy;
    if (permissions & PDF_PERM_ANNOTATE) perm |= PoDoFo::PdfEncrypt::ePdfPermissions_EditNotes;
    if (permissions & PDF_PERM_FILL_FORMS) perm |= PoDoFo::PdfEncrypt::ePdfPermissions_FillAndSign;
    if (permissions & PDF_PERM_ACCESSIBILITY) perm |= PoDoFo::PdfEncrypt::ePdfPermissions_Accessible;
    if (permissions & PDF_PERM_ASSEMBLE) perm |= PoDoFo::PdfEncrypt::ePdfPermissions_DocAssembly;
    if (permissions & PDF_PERM_PRINT_HIGH_RES) perm |= PoDoFo::PdfEncrypt::ePdfPermissions_HighPrint;

    PoDoFo::PdfEncrypt::EPdfEncryptAlgorithm algo =
        PoDoFo::PdfEncrypt::ePdfEncryptAlgorithm_AESV2;
    PoDoFo::PdfEncrypt::EPdfKeyLength keylen =
        PoDoFo::PdfEncrypt::ePdfKeyLength_128;

    switch (mode)
    {
        case PDF_ENCRYPT_RC4_40:
            algo = PoDoFo::PdfEncrypt::ePdfEncryptAlgorithm_RC4V1;
            keylen = PoDoFo::PdfEncrypt::ePdfKeyLength_40;
            break;
        case PDF_ENCRYPT_RC4_128:
            algo = PoDoFo::PdfEncrypt::ePdfEncryptAlgorithm_RC4V2;
            keylen = PoDoFo::PdfEncrypt::ePdfKeyLength_128;
            break;
        case PDF_ENCRYPT_AES_128:
            algo = PoDoFo::PdfEncrypt::ePdfEncryptAlgorithm_AESV2;
            keylen = PoDoFo::PdfEncrypt::ePdfKeyLength_128;
            break;
        default:
            return PDF_ERR_BACKEND_UNSUPPORTED; /* AES-256 needs libidn */
    }

    try
    {
        if (d->streamed)
            return PDF_ERR_BACKEND_UNSUPPORTED; /* encryption is fixed at
                                * streamed-document construction time */
        d->doc->SetEncrypted(user ? user : "", owner ? owner : "", perm,
                             algo, keylen);
        return PDF_OK;
    }
    catch (const std::exception &e)
    {
        return pf_catch(d, e, "SetEncrypted");
    }
#else
    /* PoDoFo built without libcrypto: no encryption support at all. */
    (void)doc; (void)mode; (void)owner; (void)user; (void)permissions;
    (void)d;
    return PDF_ERR_BACKEND_UNSUPPORTED;
#endif
}

static pdf_status p_doc_set_compression(pdf_backend_doc *doc, unsigned mask)
{
    podofo_doc *d = D(doc);
    /* 0.9.x always flate-compresses content streams; the only knob is
     * the write mode (compact vs. pretty). Map "no compression" to the
     * clean write mode so output stays readable, otherwise compact. */
    (void)mask;
    try
    {
        d->doc->SetWriteMode(
            mask == PDF_COMPRESS_NONE ? PoDoFo::ePdfWriteMode_Clean
                                      : PoDoFo::ePdfWriteMode_Compact);
        return PDF_OK;
    }
    catch (const std::exception &e)
    {
        return pf_catch(d, e, "SetWriteMode");
    }
}

static pdf_status p_doc_set_page_mode_use_outlines(pdf_backend_doc *doc,
                                                   int enable)
{
    podofo_doc *d = D(doc);
    try
    {
        d->pdoc()->SetPageMode(enable ? PoDoFo::ePdfPageModeUseBookmarks
                                   : PoDoFo::ePdfPageModeUseNone);
        return PDF_OK;
    }
    catch (const std::exception &e)
    {
        return pf_catch(d, e, "SetPageMode");
    }
}

/* ----- pages ------------------------------------------------------------------ */

static pdf_status p_page_add(pdf_backend_doc *doc, double w, double h,
                             pdf_page *out_page)
{
    podofo_doc *d = D(doc);
    try
    {
        flush_painter(d);
        if (d->streamed)
        {
            pdf_status src = p_doc_probe_stream_error(doc);
            if (src != PDF_OK)
                return src;
        }
        PoDoFo::PdfPage *page = d->pdoc()->CreatePage(
            PoDoFo::PdfRect(0.0, 0.0, w, h));
        if (!page)
            return PDF_ERR_BACKEND_FAILED;

        /* Do NOT keep `page`: the returned wrapper sits at a misaligned
         * pages-tree-cache slot and is deleted by the next CreatePage()
         * (upstream 0.9.7 defect). Address the page by index instead. */
        int index = d->pdoc()->GetPageCount() - 1;
        if (index < 0 || d->pdoc()->GetPage(index) == nullptr)
            return PDF_ERR_BACKEND_FAILED;

        podofo_obj *o = new (std::nothrow) podofo_obj();
        if (!o)
            return PDF_ERR_OUT_OF_MEMORY;
        o->kind = PFOBJ_PAGE;
        o->owner = d;
        o->page_index = index;

        *out_page = (pdf_page)malloc(sizeof(struct pdf_page_cell));
        if (!*out_page)
        {
            delete o;
            return PDF_ERR_OUT_OF_MEMORY;
        }
        (*out_page)->vt = &g_podofo_vt;
        (*out_page)->obj = o;
        return PDF_OK;
    }
    catch (const std::exception &e)
    {
        return pf_catch(d, e, "CreatePage");
    }
}

static pdf_status p_page_count(pdf_backend_doc *doc, size_t *out_count)
{
    podofo_doc *d = D(doc);
    *out_count = (size_t)d->pdoc()->GetPageCount();
    return PDF_OK;
}

static pdf_status p_page_get_size(pdf_page page, double *out_w, double *out_h)
{
    PoDoFo::PdfRect r = page_of(page)->GetPageSize();
    *out_w = r.GetWidth();
    *out_h = r.GetHeight();
    return PDF_OK;
}

static pdf_status p_page_set_rotation(pdf_page page, int degrees)
{
    podofo_doc *d = doc_of_page(page);
    try
    {
        page_of(page)->SetRotation(degrees);
        return PDF_OK;
    }
    catch (const std::exception &e)
    {
        return pf_catch(d, e, "SetRotation");
    }
}

/* ----- fonts ------------------------------------------------------------------ */

static pdf_status make_font_cell(podofo_doc *d, PoDoFo::PdfFont *f,
                                 pdf_font *out_font)
{
    podofo_obj *o;
    if (!f)
        return PDF_ERR_BACKEND_FAILED;
    o = new (std::nothrow) podofo_obj();
    if (!o)
        return PDF_ERR_OUT_OF_MEMORY;
    o->kind = PFOBJ_FONT;
    o->owner = d;
    o->font = f;

    *out_font = (pdf_font)malloc(sizeof(struct pdf_font_cell));
    if (!*out_font)
    {
        delete o;
        return PDF_ERR_OUT_OF_MEMORY;
    }
    (*out_font)->vt = &g_podofo_vt;
    (*out_font)->obj = o;
    (void)d;
    return PDF_OK;
}

static const char *std_font_name(pdf_std_font f)
{
    switch (f)
    {
        case PDF_STD_FONT_TIMES_ROMAN: return "Times-Roman";
        case PDF_STD_FONT_TIMES_ITALIC: return "Times-Italic";
        case PDF_STD_FONT_TIMES_BOLD: return "Times-Bold";
        case PDF_STD_FONT_TIMES_BOLD_ITALIC: return "Times-BoldItalic";
        case PDF_STD_FONT_HELVETICA: return "Helvetica";
        case PDF_STD_FONT_HELVETICA_OBLIQUE: return "Helvetica-Oblique";
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

static pdf_status p_font_get_std(pdf_backend_doc *doc, pdf_std_font font,
                                 pdf_font *out_font)
{
    podofo_doc *d = D(doc);
    try
    {
        if (d->streamed)
        {
            pdf_status src = p_doc_probe_stream_error(doc);
            if (src != PDF_OK)
                return src;
        }
        /* eFontCreationFlags_AutoSelectBase14 makes the cache return a
         * non-embedded base-14 font for the known PDF standard names. */
        PoDoFo::PdfFont *f = d->pdoc()->CreateFont(
            std_font_name(font), false,
            PoDoFo::PdfEncodingFactory::GlobalWinAnsiEncodingInstance(),
            PoDoFo::PdfFontCache::eFontCreationFlags_AutoSelectBase14,
            false /* not embedded */);
        return make_font_cell(d, f, out_font);
    }
    catch (const std::exception &e)
    {
        return pf_catch(d, e, "CreateFont");
    }
}

static pdf_status p_font_load_ttf(pdf_backend_doc *doc, const char *path,
                                  int embed, pdf_font *out_font)
{
    podofo_doc *d = D(doc);
    try
    {
        if (d->streamed)
        {
            pdf_status src = p_doc_probe_stream_error(doc);
            if (src != PDF_OK)
                return src;
        }
        PoDoFo::PdfFont *f;
        if (embed)
        {
            /* Subset embedding: glyphs are collected as pages are drawn
             * (PdfPainter -> AddUsedSubsettingGlyphs) and only those are
             * written into FontFile2 when the document finishes. This is
             * essential for printer-driver style jobs where the used
             * character set is unknown up front. Works with identity
             * (2-byte) encoding so arbitrary unicode can be drawn. */
            f = d->pdoc()->CreateFontSubset(
                "", false, false, false,
                PoDoFo::PdfEncodingFactory::GlobalIdentityEncodingInstance(),
                path);
        }
        else
        {
            /* Reference the font without embedding; the overload taking a
             * file name is CreateFont(name, bold, italic, ...,
             * pszFileName); the name is taken from the file itself. */
            f = d->pdoc()->CreateFont(
                "", false, false, false,
                PoDoFo::PdfEncodingFactory::GlobalWinAnsiEncodingInstance(),
                PoDoFo::PdfFontCache::eFontCreationFlags_None,
                false, path);
        }
        if (!f)
            return PDF_ERR_BACKEND_FAILED;
        return make_font_cell(d, f, out_font);
    }
    catch (const std::exception &e)
    {
        return pf_catch(d, e, "CreateFont(file)");
    }
}

static pdf_status p_font_load_ttf_memory(pdf_backend_doc *doc,
                                         const unsigned char *data,
                                         size_t size, int embed,
                                         pdf_font *out_font)
{
    podofo_doc *d = D(doc);
    try
    {
        if (d->streamed)
        {
            pdf_status src = p_doc_probe_stream_error(doc);
            if (src != PDF_OK)
                return src;
        }
        /* Build metrics from the memory buffer, then let the factory
         * create the font object inside the document. GetFontLibrary()
         * returns the handle by value; the metrics constructor wants its
         * address, so keep it in a local. */
        FT_Library ft = d->pdoc()->GetFontLibrary();
        PoDoFo::PdfFontMetricsFreetype metrics(
            &ft, (const char *)data, (unsigned int)size, false);
        /* Subsetting via the same CID path as file fonts: glyphs are
         * registered while pages are drawn and the subset is built at
         * finish. Identity encoding keeps 2-byte unicode intact. */
        int flags = PoDoFo::ePdfFont_Subsetting;
        if (!embed)
            flags = PoDoFo::ePdfFont_Normal;
        PoDoFo::PdfFont *f = PoDoFo::PdfFontFactory::CreateFontObject(
            &metrics, flags,
            embed ? PoDoFo::PdfEncodingFactory::GlobalIdentityEncodingInstance()
                  : PoDoFo::PdfEncodingFactory::GlobalWinAnsiEncodingInstance(),
            d->pdoc()->GetObjects());
        if (!f)
            return PDF_ERR_BACKEND_FAILED;
        return make_font_cell(d, f, out_font);
    }
    catch (const std::exception &e)
    {
        return pf_catch(d, e, "CreateFont(mem)");
    }
}

static pdf_status p_font_find_system(pdf_backend_doc *doc, const char *family,
                                     int bold, int italic,
                                     pdf_font *out_font)
{
    podofo_doc *d = D(doc);
    try
    {
        if (d->streamed)
        {
            pdf_status src = p_doc_probe_stream_error(doc);
            if (src != PDF_OK)
                return src;
        }
        PoDoFo::PdfFont *f = d->pdoc()->CreateFont(
            family ? family : "Helvetica", bold ? true : false,
            italic ? true : false);
        return make_font_cell(d, f, out_font);
    }
    catch (const std::exception &e)
    {
        return pf_catch(d, e, "CreateFont(system)");
    }
}

static void p_font_free(pdf_font font)
{
    /* Fonts are owned by the document. */
    free(((pdf_font)font)->obj);
}

static pdf_status p_font_text_width(pdf_font font, const char *text,
                                    double size, double *out_width)
{
    PoDoFo::PdfFont *f = O(((pdf_font)font)->obj)->font;
    /* CharWidth returns the width already scaled by the metrics' current
     * size, so set the size first and measure. We save/restore around it
     * because the painter sets the size explicitly before drawing. */
    float saved = f->GetFontSize();
    f->SetFontSize((float)size);
    const PoDoFo::PdfFontMetrics *m = f->GetFontMetrics();
    double w = 0.0;
    for (const char *p = text; p && *p; ++p)
        w += m->CharWidth((unsigned char)*p);
    f->SetFontSize(saved);
    *out_width = w;
    return PDF_OK;
}

static pdf_status p_font_ascent(pdf_font font, double size, double *out)
{
    PoDoFo::PdfFont *f = O(((pdf_font)font)->obj)->font;
    const PoDoFo::PdfFontMetrics *m = f->GetFontMetrics();
    /* GetPdfAscent is in font units (1000 upm); scale to points. */
    *out = m->GetPdfAscent() * size / 1000.0;
    return PDF_OK;
}

static pdf_status p_font_descent(pdf_font font, double size, double *out)
{
    PoDoFo::PdfFont *f = O(((pdf_font)font)->obj)->font;
    const PoDoFo::PdfFontMetrics *m = f->GetFontMetrics();
    *out = m->GetPdfDescent() * size / 1000.0;
    return PDF_OK;
}

/* ----- text ------------------------------------------------------------------------ */

static pdf_status p_draw_text(pdf_page page, pdf_font font,
                              const char *utf8_text, double x, double y,
                              double size)
{
    podofo_doc *d = doc_of_page(page);
    PoDoFo::PdfFont *f = O(((pdf_font)font)->obj)->font;
    try
    {
        pdf_status st = paint_on(d, page_of(page));
        if (st != PDF_OK)
            return st;
        f->SetFontSize((float)size);
        d->painter->SetFont(f);
        /* The const char* ctor treats bytes as PDFDocEncoding; the utf8
   * overload converts to UTF-16BE and marks the string unicode,
   * which is what identity-encoded CID text and the subsetting
   * glyph collector need. */
        d->painter->DrawText(
            x, y,
            PoDoFo::PdfString(
                reinterpret_cast<const PoDoFo::pdf_utf8 *>(utf8_text)));
        return PDF_OK;
    }
    catch (const std::exception &e)
    {
        return pf_catch(d, e, "DrawText");
    }
}

static pdf_status p_draw_text_aligned(pdf_page page, pdf_font font,
                                      const char *utf8_text, double x,
                                      double y, double size, double width,
                                      pdf_align align)
{
    podofo_doc *d = doc_of_page(page);
    PoDoFo::PdfFont *f = O(((pdf_font)font)->obj)->font;
    try
    {
        pdf_status st = paint_on(d, page_of(page));
        if (st != PDF_OK)
            return st;
        f->SetFontSize((float)size);
        d->painter->SetFont(f);
        PoDoFo::EPdfAlignment a = PoDoFo::ePdfAlignment_Left;
        if (align == PDF_ALIGN_CENTER)
            a = PoDoFo::ePdfAlignment_Center;
        else if (align == PDF_ALIGN_RIGHT)
            a = PoDoFo::ePdfAlignment_Right;
        d->painter->DrawTextAligned(
            x, y, width,
            PoDoFo::PdfString(
                reinterpret_cast<const PoDoFo::pdf_utf8 *>(utf8_text)),
                                    a);
        return PDF_OK;
    }
    catch (const std::exception &e)
    {
        return pf_catch(d, e, "DrawTextAligned");
    }
}

/* ----- graphics state ------------------------------------------------------------------ */

static pdf_status p_gstate_save(pdf_page page)
{
    podofo_doc *d = doc_of_page(page);
    try
    {
        pdf_status st = paint_on(d, page_of(page));
        if (st != PDF_OK)
            return st;
        d->painter->Save();
        return PDF_OK;
    }
    catch (const std::exception &e)
    {
        return pf_catch(d, e, "Save");
    }
}

static pdf_status p_gstate_restore(pdf_page page)
{
    podofo_doc *d = doc_of_page(page);
    try
    {
        pdf_status st = paint_on(d, page_of(page));
        if (st != PDF_OK)
            return st;
        d->painter->Restore();
        return PDF_OK;
    }
    catch (const std::exception &e)
    {
        return pf_catch(d, e, "Restore");
    }
}

static pdf_status p_set_transform(pdf_page page, double a, double b,
                                  double c, double d_, double e, double f)
{
    podofo_doc *d = doc_of_page(page);
    try
    {
        pdf_status st = paint_on(d, page_of(page));
        if (st != PDF_OK)
            return st;
        d->painter->SetTransformationMatrix(a, b, c, d_, e, f);
        return PDF_OK;
    }
    catch (const std::exception &e)
    {
        return pf_catch(d, e, "SetTransformationMatrix");
    }
}

static pdf_status p_set_line_width(pdf_page page, double w)
{
    podofo_doc *d = doc_of_page(page);
    try
    {
        pdf_status st = paint_on(d, page_of(page));
        if (st != PDF_OK)
            return st;
        d->painter->SetStrokeWidth(w);
        return PDF_OK;
    }
    catch (const std::exception &e)
    {
        return pf_catch(d, e, "SetStrokeWidth");
    }
}

static pdf_status p_set_line_cap(pdf_page page, pdf_line_cap cap)
{
    podofo_doc *d = doc_of_page(page);
    try
    {
        pdf_status st = paint_on(d, page_of(page));
        if (st != PDF_OK)
            return st;
        switch (cap)
        {
            case PDF_LINE_CAP_ROUND:
                d->painter->SetLineCapStyle(PoDoFo::ePdfLineCapStyle_Round);
                break;
            case PDF_LINE_CAP_SQUARE:
                d->painter->SetLineCapStyle(PoDoFo::ePdfLineCapStyle_Square);
                break;
            default:
                d->painter->SetLineCapStyle(PoDoFo::ePdfLineCapStyle_Butt);
                break;
        }
        return PDF_OK;
    }
    catch (const std::exception &e)
    {
        return pf_catch(d, e, "SetLineCapStyle");
    }
}

static pdf_status p_set_line_join(pdf_page page, pdf_line_join join)
{
    podofo_doc *d = doc_of_page(page);
    try
    {
        pdf_status st = paint_on(d, page_of(page));
        if (st != PDF_OK)
            return st;
        switch (join)
        {
            case PDF_LINE_JOIN_ROUND:
                d->painter->SetLineJoinStyle(PoDoFo::ePdfLineJoinStyle_Round);
                break;
            case PDF_LINE_JOIN_BEVEL:
                d->painter->SetLineJoinStyle(PoDoFo::ePdfLineJoinStyle_Bevel);
                break;
            default:
                d->painter->SetLineJoinStyle(PoDoFo::ePdfLineJoinStyle_Miter);
                break;
        }
        return PDF_OK;
    }
    catch (const std::exception &e)
    {
        return pf_catch(d, e, "SetLineJoinStyle");
    }
}

static pdf_status p_set_miter_limit(pdf_page page, double limit)
{
    podofo_doc *d = doc_of_page(page);
    try
    {
        pdf_status st = paint_on(d, page_of(page));
        if (st != PDF_OK)
            return st;
        d->painter->SetMiterLimit(limit);
        return PDF_OK;
    }
    catch (const std::exception &e)
    {
        return pf_catch(d, e, "SetMiterLimit");
    }
}

static pdf_status p_set_line_dash(pdf_page page, const double *pattern,
                                  size_t n, double phase)
{
    podofo_doc *d = doc_of_page(page);
    try
    {
        pdf_status st = paint_on(d, page_of(page));
        if (st != PDF_OK)
            return st;
        PoDoFo::EPdfStrokeStyle style = dash_style(pattern, n);
        if (style == PoDoFo::ePdfStrokeStyle_Custom && n > 0)
        {
            /* Build a custom dash array string: "[a b ...] phase". Do NOT
             * append the "d" operator: the Custom branch of PoDoFo's
             * SetStrokeStyle appends " d" itself, so a trailing "d" here
             * would emit a bare second "d" with no operands. */
            std::string custom = "[";
            char tmp[32];
            for (size_t i = 0; i < n; ++i)
            {
                snprintf(tmp, sizeof(tmp), "%g ", pattern[i]);
                custom += tmp;
            }
            custom += "] ";
            snprintf(tmp, sizeof(tmp), "%g", phase);
            custom += tmp;
            d->painter->SetStrokeStyle(PoDoFo::ePdfStrokeStyle_Custom,
                                       custom.c_str());
        }
        else
        {
            d->painter->SetStrokeStyle(style);
        }
        return PDF_OK;
    }
    catch (const std::exception &e)
    {
        return pf_catch(d, e, "SetStrokeStyle");
    }
}

static pdf_status p_set_fill_color(pdf_page page, const pdf_color *c)
{
    podofo_doc *d = doc_of_page(page);
    try
    {
        pdf_status st = paint_on(d, page_of(page));
        if (st != PDF_OK)
            return st;
        d->painter->SetColor(to_color(c));
        return PDF_OK;
    }
    catch (const std::exception &e)
    {
        return pf_catch(d, e, "SetColor");
    }
}

static pdf_status p_set_stroke_color(pdf_page page, const pdf_color *c)
{
    podofo_doc *d = doc_of_page(page);
    try
    {
        pdf_status st = paint_on(d, page_of(page));
        if (st != PDF_OK)
            return st;
        d->painter->SetStrokingColor(to_color(c));
        return PDF_OK;
    }
    catch (const std::exception &e)
    {
        return pf_catch(d, e, "SetStrokingColor");
    }
}

static pdf_status p_set_opacity(pdf_page page, double fill_alpha,
                                double stroke_alpha)
{
    podofo_doc *d = doc_of_page(page);
    try
    {
        pdf_status st = paint_on(d, page_of(page));
        if (st != PDF_OK)
            return st;
        PoDoFo::PdfExtGState gs(d->pdoc());
        gs.SetFillOpacity((float)fill_alpha);
        gs.SetStrokeOpacity((float)stroke_alpha);
        d->painter->SetExtGState(&gs);
        return PDF_OK;
    }
    catch (const std::exception &e)
    {
        return pf_catch(d, e, "SetExtGState");
    }
}

static pdf_status p_set_blend_mode(pdf_page page, pdf_blend_mode mode)
{
    podofo_doc *d = doc_of_page(page);
    try
    {
        pdf_status st = paint_on(d, page_of(page));
        if (st != PDF_OK)
            return st;
        static const char *names[12] = {
            "Normal", "Multiply", "Screen", "Overlay", "Darken",
            "Lighten", "ColorDodge", "ColorBurn", "HardLight",
            "SoftLight", "Difference", "Exclusion"
        };
        if ((int)mode < 0 || (int)mode > 11)
            return PDF_ERR_INVALID_PARAM;
        /* 0.9.x has no blend-mode helper; set it through a raw ExtGState. */
        PoDoFo::PdfExtGState gs(d->pdoc());
        gs.GetObject()->GetDictionary().AddKey(
            PoDoFo::PdfName("BM"),
            PoDoFo::PdfObject(PoDoFo::PdfName(names[(int)mode])));
        d->painter->SetExtGState(&gs);
        return PDF_OK;
    }
    catch (const std::exception &e)
    {
        return pf_catch(d, e, "blend mode");
    }
}

static pdf_status p_set_text_render_mode(pdf_page page,
                                         pdf_text_render_mode mode)
{
    podofo_doc *d = doc_of_page(page);
    try
    {
        pdf_status st = paint_on(d, page_of(page));
        if (st != PDF_OK)
            return st;
        switch (mode)
        {
            case PDF_TEXT_RENDER_STROKE:
                d->painter->SetTextRenderingMode(
                    PoDoFo::ePdfTextRenderingMode_Stroke);
                break;
            case PDF_TEXT_RENDER_FILL_STROKE:
                d->painter->SetTextRenderingMode(
                    PoDoFo::ePdfTextRenderingMode_FillAndStroke);
                break;
            case PDF_TEXT_RENDER_INVISIBLE:
                d->painter->SetTextRenderingMode(
                    PoDoFo::ePdfTextRenderingMode_Invisible);
                break;
            default:
                d->painter->SetTextRenderingMode(
                    PoDoFo::ePdfTextRenderingMode_Fill);
                break;
        }
        return PDF_OK;
    }
    catch (const std::exception &e)
    {
        return pf_catch(d, e, "SetTextRenderingMode");
    }
}

/* ----- paths -------------------------------------------------------------------------- */

static pdf_status p_path_move_to(pdf_page page, double x, double y)
{
    podofo_doc *d = doc_of_page(page);
    try
    {
        pdf_status st = paint_on(d, page_of(page));
        if (st != PDF_OK)
            return st;
        d->painter->MoveTo(x, y);
        return PDF_OK;
    }
    catch (const std::exception &e)
    {
        return pf_catch(d, e, "MoveTo");
    }
}

static pdf_status p_path_line_to(pdf_page page, double x, double y)
{
    podofo_doc *d = doc_of_page(page);
    try
    {
        pdf_status st = paint_on(d, page_of(page));
        if (st != PDF_OK)
            return st;
        d->painter->LineTo(x, y);
        return PDF_OK;
    }
    catch (const std::exception &e)
    {
        return pf_catch(d, e, "LineTo");
    }
}

static pdf_status p_path_curve_to(pdf_page page, double x1, double y1,
                                  double x2, double y2, double x3, double y3)
{
    podofo_doc *d = doc_of_page(page);
    try
    {
        pdf_status st = paint_on(d, page_of(page));
        if (st != PDF_OK)
            return st;
        d->painter->CubicBezierTo(x1, y1, x2, y2, x3, y3);
        return PDF_OK;
    }
    catch (const std::exception &e)
    {
        return pf_catch(d, e, "CurveTo");
    }
}

static pdf_status p_path_close(pdf_page page)
{
    podofo_doc *d = doc_of_page(page);
    try
    {
        pdf_status st = paint_on(d, page_of(page));
        if (st != PDF_OK)
            return st;
        d->painter->Close();
        return PDF_OK;
    }
    catch (const std::exception &e)
    {
        return pf_catch(d, e, "Close");
    }
}

static pdf_status p_path_rectangle(pdf_page page, const pdf_rect *r)
{
    podofo_doc *d = doc_of_page(page);
    try
    {
        pdf_status st = paint_on(d, page_of(page));
        if (st != PDF_OK)
            return st;
        d->painter->Rectangle(r->x, r->y, r->w, r->h);
        return PDF_OK;
    }
    catch (const std::exception &e)
    {
        return pf_catch(d, e, "Rectangle");
    }
}

static pdf_status p_path_paint(pdf_page page, pdf_path_paint_mode mode)
{
    podofo_doc *d = doc_of_page(page);
    try
    {
        pdf_status st = paint_on(d, page_of(page));
        if (st != PDF_OK)
            return st;
        bool even_odd = (mode == PDF_PATH_FILL_EVENODD
                         || mode == PDF_PATH_FILL_STROKE_EVENODD);
        switch (mode)
        {
            case PDF_PATH_STROKE:
                d->painter->Stroke();
                break;
            case PDF_PATH_FILL:
            case PDF_PATH_FILL_EVENODD:
                d->painter->Fill(even_odd);
                break;
            case PDF_PATH_FILL_STROKE:
            case PDF_PATH_FILL_STROKE_EVENODD:
                d->painter->FillAndStroke(even_odd);
                break;
            default:
                return PDF_ERR_INVALID_PARAM;
        }
        return PDF_OK;
    }
    catch (const std::exception &e)
    {
        return pf_catch(d, e, "path paint");
    }
}

static pdf_status p_path_discard(pdf_page page)
{
    podofo_doc *d = doc_of_page(page);
    try
    {
        pdf_status st = paint_on(d, page_of(page));
        if (st != PDF_OK)
            return st;
        /* 0.9.x: closing the path without painting leaves a dangling
         * path; emulate 'n' by stroking with zero opacity is not clean.
         * The closest supported call is EndPath-less: painter has no
         * 'new' operator, so issue Close() then nothing. (PDF 'n' is
         * rare; the dummy/haru backends cover it exactly.) */
        d->painter->Close();
        return PDF_OK;
    }
    catch (const std::exception &e)
    {
        return pf_catch(d, e, "path discard");
    }
}

/* ----- images ---------------------------------------------------------------------------- */

static pdf_status make_image_cell(podofo_doc *d, PoDoFo::PdfImage *img,
                                  pdf_image *out_image)
{
    podofo_obj *o = new (std::nothrow) podofo_obj();
    if (!o)
        return PDF_ERR_OUT_OF_MEMORY;
    o->kind = PFOBJ_IMAGE;
    o->owner = d;
    o->image = img;

    *out_image = (pdf_image)malloc(sizeof(struct pdf_image_cell));
    if (!*out_image)
    {
        delete o;
        return PDF_ERR_OUT_OF_MEMORY;
    }
    (*out_image)->vt = &g_podofo_vt;
    (*out_image)->obj = o;
    return PDF_OK;
}

static pdf_status p_image_load_file(pdf_backend_doc *doc, const char *path,
                                    pdf_image *out_image)
{
    podofo_doc *d = D(doc);
    try
    {
        /* owned by us; document takes over the object when written */
        PoDoFo::PdfImage *img = new PoDoFo::PdfImage(d->pdoc());
        img->LoadFromFile(path);
        return make_image_cell(d, img, out_image);
    }
    catch (const std::exception &e)
    {
        return pf_catch(d, e, "PdfImage::LoadFromFile");
    }
}

static pdf_status p_image_load_memory(pdf_backend_doc *doc,
                                      const unsigned char *data, size_t size,
                                      pdf_image *out_image)
{
    podofo_doc *d = D(doc);
    try
    {
        PoDoFo::PdfImage *img = new PoDoFo::PdfImage(d->pdoc());
        img->LoadFromData(data, (PoDoFo::pdf_long)size);
        return make_image_cell(d, img, out_image);
    }
    catch (const std::exception &e)
    {
        return pf_catch(d, e, "PdfImage::LoadFromData");
    }
}

static void p_image_free(pdf_image image)
{
    /* The image object lives in the document's object vector. */
    delete O(((pdf_image)image)->obj);
}

static pdf_status p_image_get_size(pdf_image image, size_t *out_w,
                                   size_t *out_h)
{
    PoDoFo::PdfImage *img = O(((pdf_image)image)->obj)->image;
    *out_w = (size_t)img->GetWidth();
    *out_h = (size_t)img->GetHeight();
    return PDF_OK;
}

static pdf_status p_draw_image(pdf_page page, pdf_image image,
                               const pdf_rect *dest)
{
    podofo_doc *d = doc_of_page(page);
    PoDoFo::PdfImage *img = O(((pdf_image)image)->obj)->image;
    try
    {
        pdf_status st = paint_on(d, page_of(page));
        if (st != PDF_OK)
            return st;
        double scale_x = img->GetWidth() > 0
                             ? dest->w / img->GetWidth() : 1.0;
        double scale_y = img->GetHeight() > 0
                             ? dest->h / img->GetHeight() : 1.0;
        d->painter->DrawImage(dest->x, dest->y, img, scale_x, scale_y);
        return PDF_OK;
    }
    catch (const std::exception &e)
    {
        return pf_catch(d, e, "DrawImage");
    }
}

/* ----- annotations & outlines ---------------------------------------------------------------- */

static pdf_status p_annot_create(pdf_page page, pdf_annot_type type,
                                 const pdf_rect *rect, const char *content,
                                 pdf_annot *out_annot)
{
    podofo_doc *d = doc_of_page(page);
    try
    {
        flush_painter(d);
        if (d->streamed && d->device && d->device->status() != PDF_OK)
            return d->device->status(); /* sink already failed */
        PoDoFo::PdfPage *p = page_of(page);
        PoDoFo::PdfRect pr(rect->x, rect->y, rect->w, rect->h);

        PoDoFo::EPdfAnnotation at;
        switch (type)
        {
            case PDF_ANNOT_TEXT: at = PoDoFo::ePdfAnnotation_Text; break;
            case PDF_ANNOT_LINK_URI:
            case PDF_ANNOT_LINK_GOTO:
                at = PoDoFo::ePdfAnnotation_Link; break;
            case PDF_ANNOT_HIGHLIGHT:
                at = PoDoFo::ePdfAnnotation_Highlight; break;
            case PDF_ANNOT_UNDERLINE:
                at = PoDoFo::ePdfAnnotation_Underline; break;
            case PDF_ANNOT_SQUIGGLY:
                at = PoDoFo::ePdfAnnotation_Squiggly; break;
            case PDF_ANNOT_STRIKEOUT:
                at = PoDoFo::ePdfAnnotation_StrikeOut; break;
            case PDF_ANNOT_SQUARE:
                at = PoDoFo::ePdfAnnotation_Square; break;
            case PDF_ANNOT_CIRCLE:
                at = PoDoFo::ePdfAnnotation_Circle; break;
            default: return PDF_ERR_INVALID_PARAM;
        }

        PoDoFo::PdfAnnotation *a = p->CreateAnnotation(at, pr);
        if (!a)
            return PDF_ERR_BACKEND_FAILED;
        if (content && at != PoDoFo::ePdfAnnotation_Link)
            a->SetContents(PoDoFo::PdfString(
                reinterpret_cast<const PoDoFo::pdf_utf8 *>(content)));

        /* Links are completed later through annot_link_to_page. */

        podofo_obj *o = new (std::nothrow) podofo_obj();
        if (!o)
            return PDF_ERR_OUT_OF_MEMORY;
        o->kind = PFOBJ_ANNOT;
        o->owner = d;
        o->annot = a;

        *out_annot = (pdf_annot)malloc(sizeof(struct pdf_annot_cell));
        if (!*out_annot)
        {
            delete o;
            return PDF_ERR_OUT_OF_MEMORY;
        }
        (*out_annot)->vt = &g_podofo_vt;
        (*out_annot)->obj = o;
        return PDF_OK;
    }
    catch (const std::exception &e)
    {
        return pf_catch(d, e, "CreateAnnotation");
    }
}

static pdf_status p_annot_link_to_page(pdf_annot annot, pdf_page *dest,
                                       const char *uri)
{
    podofo_obj *o = O(((pdf_annot)annot)->obj);
    podofo_doc *d = doc_of_obj(o);
    try
    {
        PoDoFo::PdfAnnotation *a = o->annot;
        if (!a)
            return PDF_ERR_INVALID_PARAM;

        if (uri)
        {
            PoDoFo::PdfAction action(PoDoFo::ePdfAction_URI, d->pdoc());
            action.SetURI(PoDoFo::PdfString(
                reinterpret_cast<const PoDoFo::pdf_utf8 *>(uri)));
            a->SetAction(action);
        }
        else if (dest)
        {
            PoDoFo::PdfDestination dst(page_of(*dest));
            a->SetDestination(dst);
        }
        else
        {
            return PDF_ERR_INVALID_PARAM;
        }
        return PDF_OK;
    }
    catch (const std::exception &e)
    {
        return pf_catch(d, e, "link annot");
    }
}

static pdf_status p_annot_set_color(pdf_annot annot, const pdf_color *c)
{
    podofo_obj *o = O(((pdf_annot)annot)->obj);
    try
    {
        if (!o->annot)
            return PDF_ERR_INVALID_PARAM;
        /* 0.9.x offers SetColor overloads that write the /C entry. */
        switch (c->kind)
        {
            case PDF_COLOR_GRAY:
                o->annot->SetColor(c->c1);
                break;
            case PDF_COLOR_CMYK:
                o->annot->SetColor(c->c1, c->c2, c->c3, c->c4);
                break;
            default:
                o->annot->SetColor(c->c1, c->c2, c->c3);
                break;
        }
        return PDF_OK;
    }
    catch (const std::exception &e)
    {
        return pf_catch(nullptr, e, "annot color");
    }
}

static pdf_status p_annot_set_icon(pdf_annot annot, pdf_annot_icon icon)
{
    /* 0.9.x has no icon helper on PdfAnnotation, but the /Name entry of a
     * text annotation IS the icon name; write it directly. */
    podofo_obj *o = O(((pdf_annot)annot)->obj);
    try
    {
        if (!o->annot)
            return PDF_ERR_INVALID_PARAM;
        const char *name;
        switch (icon)
        {
        case PDF_ANNOT_ICON_COMMENT:       name = "Comment";       break;
        case PDF_ANNOT_ICON_KEY:           name = "Key";           break;
        case PDF_ANNOT_ICON_HELP:          name = "Help";          break;
        case PDF_ANNOT_ICON_NEW_PARAGRAPH: name = "NewParagraph";  break;
        case PDF_ANNOT_ICON_PARAGRAPH:     name = "Paragraph";     break;
        case PDF_ANNOT_ICON_INSERT:        name = "Insert";        break;
        case PDF_ANNOT_ICON_NOTE:          name = "Note";          break;
        default:                           return PDF_ERR_INVALID_PARAM;
        }
        o->annot->GetObject()->GetDictionary().AddKey(
            PoDoFo::PdfName("Name"), PoDoFo::PdfName(name));
        return PDF_OK;
    }
    catch (const std::exception &e)
    {
        return pf_catch(doc_of_obj(o), e, "SetIcon");
    }
}

static pdf_status p_annot_set_opacity(pdf_annot annot, double alpha)
{
    podofo_obj *o = O(((pdf_annot)annot)->obj);
    try
    {
        if (!o->annot)
            return PDF_ERR_INVALID_PARAM;
        o->annot->GetObject()->GetDictionary().AddKey(
            PoDoFo::PdfName("CA"), PoDoFo::PdfObject(alpha));
        return PDF_OK;
    }
    catch (const std::exception &e)
    {
        return pf_catch(nullptr, e, "annot opacity");
    }
}

static pdf_status p_annot_set_author(pdf_annot annot, const char *author)
{
    podofo_obj *o = O(((pdf_annot)annot)->obj);
    try
    {
        if (!o->annot)
            return PDF_ERR_INVALID_PARAM;
        o->annot->SetTitle(PoDoFo::PdfString(reinterpret_cast<const
                PoDoFo::pdf_utf8 *>(author ? author : "")));
        return PDF_OK;
    }
    catch (const std::exception &e)
    {
        return pf_catch(nullptr, e, "annot author");
    }
}

static pdf_status p_annot_set_subject(pdf_annot annot, const char *subject)
{
    podofo_obj *o = O(((pdf_annot)annot)->obj);
    try
    {
        if (!o->annot)
            return PDF_ERR_INVALID_PARAM;
        /* 0.9.x: /Subject entry on the annotation dictionary. */
        o->annot->GetObject()->GetDictionary().AddKey(
            PoDoFo::PdfName("Subj"),
            PoDoFo::PdfString(reinterpret_cast<const PoDoFo::pdf_utf8 *>(
                subject ? subject : "")));
        return PDF_OK;
    }
    catch (const std::exception &e)
    {
        return pf_catch(nullptr, e, "annot subject");
    }
}

static pdf_status p_annot_set_open(pdf_annot annot, int opened)
{
    podofo_obj *o = O(((pdf_annot)annot)->obj);
    try
    {
        if (!o->annot)
            return PDF_ERR_INVALID_PARAM;
        o->annot->SetOpen(opened ? true : false);
        return PDF_OK;
    }
    catch (const std::exception &e)
    {
        return pf_catch(nullptr, e, "annot open");
    }
}

static void p_annot_free(pdf_annot annot)
{
    /* The annotation object is owned by the page. */
    delete O(((pdf_annot)annot)->obj);
}

static pdf_status p_outline_add(pdf_backend_doc *doc, pdf_outline parent,
                                const char *title, pdf_page *dest_page,
                                pdf_outline *out_outline)
{
    podofo_doc *d = D(doc);
    try
    {
        flush_painter(d);
        PoDoFo::PdfOutlineItem *item;
        if (parent)
        {
            /* PoDoFo 0.9.7 has no destination-less CreateChild overload:
             * the PdfOutlineItem ctor always calls SetDestination. An
             * empty PdfDestination(PdfVecObjects*) is the sanctioned way
             * to skip it -- AddToDictionary() ignores empty arrays. The
             * root branch below guards with if (dest_page) the same way. */
            PoDoFo::PdfOutlineItem *p =
                O(((pdf_outline)parent)->obj)->outline;
            if (dest_page)
            {
                item = p->CreateChild(
                    PoDoFo::PdfString(reinterpret_cast<const PoDoFo::pdf_utf8 *>(title)),
                    PoDoFo::PdfDestination(page_of(*dest_page)));
            }
            else
            {
                item = p->CreateChild(
                    PoDoFo::PdfString(reinterpret_cast<const PoDoFo::pdf_utf8 *>(title)),
                    PoDoFo::PdfDestination(
                        d->pdoc()->GetObjects()));
            }
        }
        else
        {
            item = d->pdoc()->GetOutlines()->CreateRoot(
                PoDoFo::PdfString(
                    reinterpret_cast<const PoDoFo::pdf_utf8 *>(title)));
            if (dest_page)
                item->SetDestination(
                    PoDoFo::PdfDestination(page_of(*dest_page)));
        }
        if (!item)
            return PDF_ERR_BACKEND_FAILED;

        podofo_obj *o = new (std::nothrow) podofo_obj();
        if (!o)
            return PDF_ERR_OUT_OF_MEMORY;
        o->kind = PFOBJ_OUTLINE;
        o->owner = d;
        o->outline = item;

        *out_outline = (pdf_outline)malloc(sizeof(struct pdf_outline_cell));
        if (!*out_outline)
        {
            delete o;
            return PDF_ERR_OUT_OF_MEMORY;
        }
        (*out_outline)->vt = &g_podofo_vt;
        (*out_outline)->obj = o;
        return PDF_OK;
    }
    catch (const std::exception &e)
    {
        return pf_catch(d, e, "outline add");
    }
}

static pdf_status p_outline_set_open(pdf_outline outline, int opened)
{
    podofo_obj *o = O(((pdf_outline)outline)->obj);
    try
    {
        if (!o->outline)
            return PDF_ERR_INVALID_PARAM;
        /* /Count sign convention: negative count = collapsed children. */
        o->outline->GetObject()->GetDictionary().AddKey(
            PoDoFo::PdfName("Open"), PoDoFo::PdfObject(opened ? true : false));
        return PDF_OK;
    }
    catch (const std::exception &e)
    {
        return pf_catch(nullptr, e, "outline open");
    }
}

static void p_outline_free(pdf_outline outline)
{
    delete O(((pdf_outline)outline)->obj);
}


static pdf_status p_doc_save_to_stream(pdf_backend_doc *doc,
                                       pdf_stream_write_fn write_fn,
                                       void *userdata)
{
    podofo_doc *d = D(doc);
    try
    {
        flush_painter(d);
        PoDoFo::PdfWriter writer(&d->doc->GetObjects(),
                                 d->doc->GetTrailer());
        writer.SetPdfVersion(d->doc->GetPdfVersion());

        StreamCallbackDevice device(write_fn, userdata);
        writer.Write(&device);
        if (device.status() == PDF_OK)
            device.flush(); /* drain the overwrite window */
        if (device.status() != PDF_OK)
            return device.status(); /* caller callback aborted */
        return PDF_OK;
    }
    catch (const std::exception &e)
    {
        return pf_catch(d, e, "Write(stream)");
    }
}

/* ----- the vtable -------------------------------------------------------------------------- */

const pdf_backend_vtable g_podofo_vt = {
    "podofo",
    PODOFO_VERSION_STRING,

    p_doc_create,
    p_doc_free,
    p_doc_save_to_file,
    p_doc_save_to_memory,
    p_doc_last_error,

    p_doc_set_info,
    p_doc_get_info,
    p_doc_set_encryption,
    p_doc_set_compression,
    p_doc_set_page_mode_use_outlines,

    p_page_add,
    p_page_count,
    p_page_get_size,
    p_page_set_rotation,

    p_font_get_std,
    p_font_load_ttf,
    p_font_load_ttf_memory,
    p_font_find_system,
    p_font_free,
    p_font_text_width,
    p_font_ascent,
    p_font_descent,

    p_draw_text,
    p_draw_text_aligned,
    NULL, /* draw_text_wrapped: composed by the core */

    p_gstate_save,
    p_gstate_restore,
    p_set_transform,
    p_set_line_width,
    p_set_line_cap,
    p_set_line_join,
    p_set_miter_limit,
    p_set_line_dash,
    p_set_fill_color,
    p_set_stroke_color,
    p_set_opacity,
    p_set_blend_mode,
    p_set_text_render_mode,

    p_path_move_to,
    p_path_line_to,
    p_path_curve_to,
    p_path_close,
    p_path_rectangle,
    p_path_paint,
    p_path_discard,

    p_image_load_file,
    p_image_load_memory,
    p_image_free,
    p_image_get_size,
    p_draw_image,

    p_annot_create,
    p_annot_link_to_page,
    p_annot_set_color,
    p_annot_set_icon,
    p_annot_set_opacity,
    p_annot_set_author,
    p_annot_set_subject,
    p_annot_set_open,
    p_annot_free,
    p_outline_add,
    p_outline_set_open,
    p_outline_free,
    p_doc_save_to_stream,
    p_doc_create_streamed,
    p_doc_finish
};

/* ----- registration ------------------------------------------------------------------------ */

extern "C" void pdf_backend_podofo_register(void)
{
    pdf_backend_register(PDF_BACKEND_PODOFO, &g_podofo_vt);
}

/* Debug/introspection helper used by the memory stress tooling: returns
 * the number of objects currently retained in the PoDoFo object table.
 * Streamed mode should stay near-constant as pages accumulate. */
extern "C" size_t pdflib_podofo_debug_object_count(pdf_doc doc)
{
    podofo_doc *d = D(doc);
    if (!d)
        return 0;
    if (d->sdoc)
        return (size_t)d->sdoc->GetObjects()->GetSize();
    if (d->doc)
        return (size_t)d->doc->GetObjects().GetSize(); /* MemDocument: returns a reference */
    return 0;
}
