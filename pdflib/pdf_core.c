/**
 * pdf_core.c - backend registry and the generic, backend-independent
 * implementation of the public pdflib API.
 *
 * Everything in here is pure C and knows nothing about libharu or PoDoFo:
 * the public pdflib_* / pdf_* entry points look the active backend up in
 * the registry and forward the call through its vtable. Composite
 * operations that every backend can serve through primitives (word
 * wrapping, arcs, polygons, ellipses) are implemented here once instead
 * of duplicating the math in every backend.
 */
#include "pdf_core.h"
#include "pdf_backend.h"
#include "pdf_internal.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The *_to_handle entry points write through the OS, not through the CRT:
 * a Win32 HANDLE and a POSIX file descriptor are both process-wide, so -
 * unlike a FILE* - they are meaningful inside this DLL as well as inside
 * the caller. See handle_sink_cb below. */
#ifdef _WIN32
#include <windows.h>
#else
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#endif

/* The streamed-output helpers live with the rest of the streamed-document
 * code further down, but teardown (pdf_doc_free) has to close a handle the
 * core owns, so the closer is forward-declared here. */
static void handle_close(void *h);

/* ------------------------------------------------------------------ */
/* Registry                                                            */
/* ------------------------------------------------------------------ */

/* Declared by each backend translation unit; weak bodies below keep the
 * build green when a backend is compiled out. */
#if defined(__GNUC__) || defined(__clang__)
__attribute__((weak)) void pdf_backend_libharu_register(void) { }
__attribute__((weak)) void pdf_backend_podofo_register(void) { }
__attribute__((weak)) void pdf_backend_dummy_register(void) { }
#else
void pdf_backend_libharu_register(void);
void pdf_backend_podofo_register(void);
void pdf_backend_dummy_register(void);
#endif

void pdf_backend_register(pdf_backend id, const pdf_backend_vtable *vtable)
{
    if (!vtable)
        return;
    g_registry[(int)id] = vtable;
}

const pdf_backend_vtable *pdf_backend_lookup(pdf_backend id)
{
    if (id <= PDF_BACKEND_AUTO || id >= PDF_BACKEND_MAX)
        return NULL;
    return g_registry[(int)id];
}

const pdf_backend_vtable *pdf_backend_default(void)
{
    if (g_registry[(int)PDF_BACKEND_PODOFO])
        return g_registry[(int)PDF_BACKEND_PODOFO];
    if (g_registry[(int)PDF_BACKEND_LIBHARU])
        return g_registry[(int)PDF_BACKEND_LIBHARU];
    return g_registry[(int)PDF_BACKEND_DUMMY];
}

/* ------------------------------------------------------------------ */
/* Internal helpers                                                    */
/* ------------------------------------------------------------------ */

static const pdf_backend_vtable *active_backend(void)
{
    pdf_ensure_backends();
    if (g_active_id == PDF_BACKEND_AUTO)
        return pdf_backend_default();
    return pdf_backend_lookup(g_active_id);
}

static void record_error(pdf_doc doc, pdf_status rc)
{
    if (rc != PDF_OK && doc && doc->vt && doc->vt->doc_last_error
        && doc->backend_doc)
    {
        const char *msg = doc->vt->doc_last_error(doc->backend_doc);
        snprintf(doc->error, sizeof(doc->error), "%s", msg ? msg : "");
    }
}

static void discard_failed_stream_file(const char *path);

static pdf_status finish_handle_create(pdf_status rc,
                                       const pdf_backend_vtable *vt,
                                       pdf_backend_doc *bdoc,
                                       pdf_doc *out_doc)
{
    pdf_doc doc;

    if (rc != PDF_OK)
        return rc;

    doc = (pdf_doc)malloc(sizeof(*doc));
    if (!doc)
    {
        if (bdoc && vt && vt->doc_free)
            vt->doc_free(bdoc);
        return PDF_ERR_OUT_OF_MEMORY;
    }

    /* The caller resolved the vtable BEFORE creating the backend doc and
     * passes it in: re-reading the global selector here would race with
     * pdflib_set_backend() and could pair a vtable of backend A with a
     * backend-B payload -> type confusion on free. */
    doc->vt = vt;
    doc->backend_doc = bdoc;
    doc->error[0] = '\0';
    doc->streamed = 0;
    doc->finished = 0;
    doc->stream_error = PDF_OK;
    doc->close_handle = NULL;
    doc->sink_path = NULL;
    doc->stream_sink = NULL;
    *out_doc = doc;
    return PDF_OK;
}

static pdf_status check_doc(pdf_doc doc)
{
    if (!doc || !doc->vt || !doc->backend_doc)
        return PDF_ERR_INVALID_HANDLE;
    return PDF_OK;
}

/* A streamed doc whose sink has failed is dead: refuse new work so
 * callers get a deterministic error instead of silent data loss. */
static pdf_status check_doc_live(pdf_doc doc)
{
    pdf_status rc = check_doc(doc);
    if (rc != PDF_OK)
        return rc;
    if (doc->stream_error != PDF_OK)
        return doc->stream_error; /* torn output: report the real cause */
    return PDF_OK;
}

static pdf_status call_page_add(pdf_doc doc, double w, double h,
                                pdf_page *out_page)
{
    if (!doc->vt->page_add)
        return PDF_ERR_BACKEND_UNSUPPORTED;
    return doc->vt->page_add(doc->backend_doc, w, h, out_page);
}

/* Circle / ellipse / arc are emitted as bezier segments so that backends
 * only need the basic path primitives. */
#define PDF_PI 3.14159265358979323846

static double bezier_k(double sweep)
{
    double a = sweep / 4.0;
    if (a < 1e-6)
        a = 1e-6;
    return 4.0 / 3.0 * tan(a);
}

/* ------------------------------------------------------------------ */
/* Backend selection                                                   */
/* ------------------------------------------------------------------ */

pdf_status pdflib_set_backend(pdf_backend backend)
{
    pdf_ensure_backends();
    if (backend != PDF_BACKEND_AUTO && !pdf_backend_lookup(backend))
        return PDF_ERR_BACKEND_NOT_FOUND;
    g_active_id = backend;
    return PDF_OK;
}

pdf_backend pdflib_get_backend(void)
{
    return g_active_id;
}

int pdflib_has_backend(pdf_backend backend)
{
    pdf_ensure_backends();
    return pdf_backend_lookup(backend) != NULL;
}

const char *pdflib_version(void)
{
    return PDFLIB_VERSION_STRING;
}

const char *pdflib_backend_version(void)
{
    const pdf_backend_vtable *vt = active_backend();
    return (vt && vt->version) ? vt->version : "unknown";
}

const char *pdflib_last_error(pdf_doc doc)
{
    if (doc && doc->error[0])
        return doc->error;
    if (doc && doc->vt && doc->vt->doc_last_error && doc->backend_doc)
        return doc->vt->doc_last_error(doc->backend_doc);
    return "";
}

/* ------------------------------------------------------------------ */
/* Document lifecycle                                                  */
/* ------------------------------------------------------------------ */

pdf_status pdf_doc_create(pdf_doc *out_doc)
{
    const pdf_backend_vtable *vt;
    pdf_backend_doc *bdoc = NULL;
    pdf_status rc;

    if (!out_doc)
        return PDF_ERR_INVALID_PARAM;

    vt = active_backend();
    if (!vt || !vt->doc_create)
        return PDF_ERR_BACKEND_NOT_FOUND;

    rc = vt->doc_create(&bdoc);
    if (rc == PDF_OK && !bdoc)
        rc = PDF_ERR_BACKEND_FAILED;
    rc = finish_handle_create(rc, vt, bdoc, out_doc);
    return rc;
}

pdf_status pdf_doc_free(pdf_doc doc)
{
    if (!doc)
        return PDF_ERR_INVALID_HANDLE;
    /* finishing first is essential for streamed documents: it flushes
     * pending content and embeds pending font subsets; a broken stream
     * skips the backend finish (output is torn, nothing left to flush) */
    if (doc->streamed && !doc->finished && doc->stream_error == PDF_OK)
        pdf_doc_finish(doc);
    if (doc->stream_error != PDF_OK && doc->sink_path)
    {
        /* aborted mid-page: the half-written file is unusable trash */
        discard_failed_stream_file(doc->sink_path);
        doc->sink_path = NULL;
    }
    if (doc->close_handle)
    {
        handle_close(doc->close_handle);
        doc->close_handle = NULL;
    }
    if (doc->vt && doc->vt->doc_free)
        doc->vt->doc_free(doc->backend_doc);
    /* the sink state must outlive the backend: teardown may still push */
    if (doc->stream_sink)
    {
        free(doc->stream_sink);
        doc->stream_sink = NULL;
    }
    free(doc);
    return PDF_OK;
}

/* --- streamed document creation --------------------------------------- */

/* Core-owned trampoline around the caller's sink. It latches the first
 * failure so the document can be marked broken no matter which layer
 * (backend device, backend finish, core cleanup) observes it. */
struct core_sink_state
{
    pdf_stream_write_fn user_fn;
    void *user_ud;
    int calls;
    pdf_status failed;
    struct pdf_doc_cell *doc; /* set right after the cell exists */
};

/* Mark a streamed document as broken: the sink rejected data, so the
 * output is torn and further work is pointless. */
static void mark_stream_broken(pdf_doc doc, pdf_status rc)
{
    if (doc && rc != PDF_OK)
    {
        doc->stream_error = rc;
        record_error(doc, rc);
    }
}

static pdf_status core_sink_trampoline(const unsigned char *data, size_t len,
                                       void *userdata)
{
    struct core_sink_state *s = (struct core_sink_state *)userdata;
    pdf_status rc;

    if (!s)
        return PDF_ERR_INVALID_PARAM;
    s->calls++;
    if (s->failed != PDF_OK)
        return s->failed; /* already broken: fail every further delivery */
    rc = s->user_fn(data, len, s->user_ud);
    if (rc != PDF_OK)
    {
        s->failed = rc;
        mark_stream_broken(s->doc, rc); /* doc may still be NULL mid-create */
    }
    return rc;
}

static pdf_status doc_create_streamed_common(pdf_stream_write_fn write_fn,
                                             void *userdata, pdf_doc *out_doc)
{
    const pdf_backend_vtable *vt;
    pdf_backend_doc *bdoc = NULL;
    struct core_sink_state *state = NULL;
    pdf_doc cell = NULL;
    pdf_status rc;

    if (!out_doc || !write_fn)
        return PDF_ERR_INVALID_PARAM;

    vt = active_backend();
    if (!vt || !vt->doc_create)
        return PDF_ERR_BACKEND_NOT_FOUND;
    if (!vt->doc_create_streamed)
        return PDF_ERR_BACKEND_UNSUPPORTED;

    /* interpose our trampoline so a failing sink can mark the document
     * broken no matter which layer observes the failure */
    state = (struct core_sink_state *)calloc(1, sizeof(*state));
    if (!state)
        return PDF_ERR_OUT_OF_MEMORY;
    state->user_fn = write_fn;
    state->user_ud = userdata;
    state->failed = PDF_OK;

    rc = vt->doc_create_streamed(core_sink_trampoline, state, &bdoc);
    if (rc == PDF_OK && !bdoc)
        rc = PDF_ERR_BACKEND_FAILED;
    rc = finish_handle_create(rc, vt, bdoc, &cell);
    if (rc != PDF_OK)
    {
        free(state);
        return rc;
    }
    cell->streamed = 1;
    cell->stream_sink = state;
    state->doc = cell;
    if (state->failed != PDF_OK) /* sink tripped during creation */
        mark_stream_broken(cell, state->failed);
    *out_doc = cell;
    return PDF_OK;
}

pdf_status pdf_doc_create_streamed(pdf_stream_write_fn write_fn,
                                   void *userdata, pdf_doc *out_doc)
{
    return doc_create_streamed_common(write_fn, userdata, out_doc);
}

/* Write one chunk to a caller-owned OS handle.
 *
 * This deliberately does NOT go through stdio. The DLL links the CRT
 * statically (/MT), so it owns a private FILE table; a FILE* the caller
 * opened lives in the caller's CRT and every fwrite/fflush on it fails
 * the CRT's stream validation. In the static CRT that failure routes to
 * _invoke_watson -> __fastfail(FAST_FAIL_INVALID_ARG), which kills the
 * process on the spot and cannot be caught by SEH or catch (...).
 *
 * An OS handle has no such problem: HANDLEs and file descriptors are
 * process-wide objects, and WriteFile/write are plain kernel calls that
 * do not care which CRT opened them. */
static pdf_status handle_sink_cb(const unsigned char *data, size_t len,
                                 void *userdata)
{
    if (!userdata)
        return PDF_ERR_INVALID_PARAM;

#ifdef _WIN32
    {
        HANDLE h = (HANDLE)userdata;

        while (len > 0)
        {
            /* WriteFile caps a single write at 1 GiB-ish; chunk it. */
            DWORD want = (len > 0x40000000u) ? 0x40000000u : (DWORD)len;
            DWORD done = 0;

            if (!WriteFile(h, data, want, &done, NULL) || done == 0)
                return PDF_ERR_IO;
            data += done;
            len -= (size_t)done;
        }
    }
#else
    {
        int fd = (int)(intptr_t)userdata;

        while (len > 0)
        {
            ssize_t n = write(fd, data, len);

            if (n < 0)
            {
                if (errno == EINTR)
                    continue;
                return PDF_ERR_IO;
            }
            data += (size_t)n;
            len -= (size_t)n;
        }
    }
#endif
    return PDF_OK;
}

/* Open a path for writing as an OS handle, so the core-owned sink for
 * pdf_doc_create_streamed_to_file() speaks the same language as every
 * other handle sink. Returns NULL on failure. */
static void *handle_open_for_write(const char *path)
{
#ifdef _WIN32
    HANDLE h = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    return (h == INVALID_HANDLE_VALUE) ? NULL : (void *)h;
#else
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    return (fd < 0) ? NULL : (void *)(intptr_t)fd;
#endif
}

/* Close a handle produced by handle_open_for_write(). Safe on NULL.
 * Deliberately does NOT flush: FlushFileBuffers() blocks until a pipe
 * reader drains, and WriteFile()/write() have no user-space buffer to
 * drain anyway. */
static void handle_close(void *h)
{
    if (!h)
        return;
#ifdef _WIN32
    if (h != (void *)INVALID_HANDLE_VALUE)
        CloseHandle((HANDLE)h);
#else
    close((int)(intptr_t)h);
#endif
}

static void discard_failed_stream_file(const char *path)
{
    if (path)
    {
        remove(path);
        free((void *)path);
    }
}

/* A streamed document whose sink failed: close the sink handle (if core
 * owned), unlink the half-written target file and hand back the sink's
 * error. Idempotent-ish: repeated calls return the first error. */
static pdf_status finish_handle_broken_stream(pdf_doc doc, pdf_status rc)
{
    if (doc->close_handle)
    {
        handle_close(doc->close_handle);
        doc->close_handle = NULL;
    }
    if (doc->sink_path)
    {
        discard_failed_stream_file(doc->sink_path);
        doc->sink_path = NULL;
    }
    return rc;
}

pdf_status pdf_doc_create_streamed_to_handle(void *os_handle, pdf_doc *out_doc)
{
    if (!os_handle)
        return PDF_ERR_INVALID_PARAM;
    return doc_create_streamed_common(handle_sink_cb, os_handle, out_doc);
}

pdf_status pdf_doc_create_streamed_to_file(const char *path, pdf_doc *out_doc)
{
    void *h;
    pdf_status rc;
    char *path_copy = NULL;

    if (!out_doc || !path || !*path)
        return PDF_ERR_INVALID_PARAM;
    h = handle_open_for_write(path);
    if (!h)
        return PDF_ERR_IO;
    rc = doc_create_streamed_common(handle_sink_cb, h, out_doc);
    if (rc != PDF_OK)
    {
        handle_close(h);
        remove(path); /* failed creation must not leave an empty file */
        return rc;
    }
    /* remember the target so an aborted job can unlink the half-written
     * file; failure to track it only costs the cleanup, not the job */
    path_copy = (char *)malloc(strlen(path) + 1);
    if (path_copy)
        memcpy(path_copy, path, strlen(path) + 1);
    (*out_doc)->close_handle = h;
    (*out_doc)->sink_path = path_copy;
    return PDF_OK;
}

pdf_status pdf_doc_finish(pdf_doc doc)
{
    pdf_status rc;

    rc = check_doc(doc);
    if (rc != PDF_OK)
        return rc;
    if (!doc->streamed)
        return PDF_OK; /* no-op for regular documents */
    if (doc->finished)
        return doc->stream_error; /* keep reporting the terminal status */
    if (doc->stream_error != PDF_OK)
    {
        /* the sink rejected data earlier: the output is torn up, so do
         * not push more bytes into it; finish_handle_broken_stream
         * closes/unlinks and re-reports the failure */
        doc->finished = 1;
        return finish_handle_broken_stream(doc, doc->stream_error);
    }
    if (doc->vt->doc_finish)
    {
        rc = doc->vt->doc_finish(doc->backend_doc);
        record_error(doc, rc);
    }
    doc->finished = 1;
    if (doc->close_handle)
    {
        handle_close(doc->close_handle);
        doc->close_handle = NULL;
    }
    return rc;
}

pdf_status pdf_doc_save_to_file(pdf_doc doc, const char *path)
{
    pdf_status rc;

    rc = check_doc(doc);
    if (rc != PDF_OK)
        return rc;
    if (doc->streamed)
        return PDF_ERR_BACKEND_FAILED; /* output already bound at creation */
    if (!path || !*path)
        return PDF_ERR_INVALID_PARAM;
    if (!doc->vt->doc_save_to_file)
        return PDF_ERR_BACKEND_UNSUPPORTED;
    rc = doc->vt->doc_save_to_file(doc->backend_doc, path);
    record_error(doc, rc);
    return rc;
}

pdf_status pdf_doc_save_to_memory(pdf_doc doc, unsigned char *buf,
                                  size_t *inout_size)
{
    pdf_status rc;

    rc = check_doc(doc);
    if (rc != PDF_OK)
        return rc;
    if (doc->streamed)
        return PDF_ERR_BACKEND_FAILED; /* output already bound at creation */
    if (!inout_size)
        return PDF_ERR_INVALID_PARAM;
    if (!doc->vt->doc_save_to_memory)
        return PDF_ERR_BACKEND_UNSUPPORTED;
    rc = doc->vt->doc_save_to_memory(doc->backend_doc, buf, inout_size);
    record_error(doc, rc);
    return rc;
}

/* --- streaming output ------------------------------------------------ */

static pdf_status core_stream_from_memory(pdf_doc doc,
                                          pdf_stream_write_fn write_fn,
                                          void *userdata)
{
    unsigned char stackbuf[8192];
    unsigned char *buf = stackbuf;
    size_t size = sizeof(stackbuf);
    pdf_status rc;

    /* one call with the stack buffer; on overflow it reports the needed
     * size, then we retry from the heap */
    rc = doc->vt->doc_save_to_memory(doc->backend_doc, buf, &size);
    if (rc != PDF_OK && rc != PDF_ERR_INVALID_PARAM)
        return rc;
    if (rc == PDF_ERR_INVALID_PARAM)
    {
        buf = (unsigned char *)malloc(size);
        if (!buf)
            return PDF_ERR_OUT_OF_MEMORY;
        rc = doc->vt->doc_save_to_memory(doc->backend_doc, buf, &size);
        if (rc != PDF_OK)
        {
            free(buf);
            return rc;
        }
    }
    rc = write_fn(buf, size, userdata);
    if (buf != stackbuf)
        free(buf);
    return rc;
}

pdf_status pdf_doc_save_to_stream(pdf_doc doc, pdf_stream_write_fn write_fn,
                                  void *userdata)
{
    pdf_status rc;

    rc = check_doc(doc);
    if (rc != PDF_OK)
        return rc;
    if (doc->streamed)
        return PDF_ERR_BACKEND_FAILED; /* output already bound at creation */
    if (!write_fn)
        return PDF_ERR_INVALID_PARAM;

    if (doc->vt->doc_save_to_stream)
    {
        rc = doc->vt->doc_save_to_stream(doc->backend_doc, write_fn, userdata);
    }
    else if (doc->vt->doc_save_to_memory)
    {
        /* generic fallback: serialize to memory, push one chunk */
        rc = core_stream_from_memory(doc, write_fn, userdata);
    }
    else
    {
        return PDF_ERR_BACKEND_UNSUPPORTED;
    }
    record_error(doc, rc);
    return rc;
}

pdf_status pdf_doc_save_to_handle(pdf_doc doc, void *os_handle)
{
    pdf_status rc;

    rc = check_doc(doc);
    if (rc != PDF_OK)
        return rc;
    if (!os_handle)
        return PDF_ERR_INVALID_PARAM;
    return pdf_doc_save_to_stream(doc, handle_sink_cb, os_handle);
}

/* ------------------------------------------------------------------ */
/* Document info                                                       */
/* ------------------------------------------------------------------ */

pdf_status pdf_doc_set_info(pdf_doc doc, pdf_info_key key,
                            const char *utf8_value)
{
    pdf_status rc;

    rc = check_doc(doc);
    if (rc != PDF_OK)
        return rc;
    if (!doc->vt->doc_set_info)
        return PDF_ERR_BACKEND_UNSUPPORTED;
    rc = doc->vt->doc_set_info(doc->backend_doc, key, utf8_value);
    record_error(doc, rc);
    return rc;
}

pdf_status pdf_doc_get_info(pdf_doc doc, pdf_info_key key, char *buf,
                            size_t *inout_len)
{
    pdf_status rc;

    rc = check_doc(doc);
    if (rc != PDF_OK)
        return rc;
    if (!inout_len || (!buf && *inout_len))
        return PDF_ERR_INVALID_PARAM;
    if (!doc->vt->doc_get_info)
        return PDF_ERR_BACKEND_UNSUPPORTED;
    rc = doc->vt->doc_get_info(doc->backend_doc, key, buf, inout_len);
    record_error(doc, rc);
    return rc;
}

/* ------------------------------------------------------------------ */
/* Security / options                                                  */
/* ------------------------------------------------------------------ */

pdf_status pdf_doc_set_encryption(pdf_doc doc, pdf_encrypt_mode mode,
                                  const char *owner_password,
                                  const char *user_password,
                                  unsigned permissions)
{
    pdf_status rc;

    rc = check_doc(doc);
    if (rc != PDF_OK)
        return rc;
    if (!doc->vt->doc_set_encryption)
        return PDF_ERR_BACKEND_UNSUPPORTED;
    rc = doc->vt->doc_set_encryption(doc->backend_doc, mode, owner_password,
                                     user_password, permissions);
    record_error(doc, rc);
    return rc;
}

pdf_status pdf_doc_set_compression(pdf_doc doc, unsigned mode_mask)
{
    pdf_status rc;

    rc = check_doc(doc);
    if (rc != PDF_OK)
        return rc;
    if (!doc->vt->doc_set_compression)
        return PDF_ERR_BACKEND_UNSUPPORTED;
    return doc->vt->doc_set_compression(doc->backend_doc, mode_mask);
}

pdf_status pdf_doc_set_page_mode_use_outlines(pdf_doc doc, int enable)
{
    pdf_status rc;

    rc = check_doc(doc);
    if (rc != PDF_OK)
        return rc;
    if (!doc->vt->doc_set_page_mode_use_outlines)
        return PDF_ERR_BACKEND_UNSUPPORTED;
    return doc->vt->doc_set_page_mode_use_outlines(doc->backend_doc, enable);
}

/* ------------------------------------------------------------------ */
/* Pages                                                               */
/* ------------------------------------------------------------------ */

static const double k_page_sizes[7][2] = {
    { 595.276, 841.890 },  /* A4        */
    { 841.890, 1190.551 }, /* A3        */
    { 419.528, 595.276 },  /* A5        */
    { 612.0, 792.0 },      /* Letter    */
    { 612.0, 1008.0 },     /* Legal     */
    { 522.0, 684.0 },      /* Executive */
    { 792.0, 1224.0 }      /* Tabloid   */
};

pdf_status pdf_page_add(pdf_doc doc, pdf_page_size size,
                        pdf_page_orientation orientation, pdf_page *out_page)
{
    pdf_status rc;
    double w, h;

    rc = check_doc_live(doc);
    if (rc != PDF_OK)
        return rc;
    if ((int)size < 0 || (int)size > 6 || !out_page)
        return PDF_ERR_INVALID_PARAM;

    w = k_page_sizes[(int)size][0];
    h = k_page_sizes[(int)size][1];
    if (orientation == PDF_ORIENTATION_LANDSCAPE)
    {
        double t = w;
        w = h;
        h = t;
    }
    return call_page_add(doc, w, h, out_page);
}

pdf_status pdf_page_add_custom(pdf_doc doc, double width, double height,
                               pdf_page *out_page)
{
    pdf_status rc;

    rc = check_doc_live(doc);
    if (rc != PDF_OK)
        return rc;
    if (width <= 0 || height <= 0 || !out_page)
        return PDF_ERR_INVALID_PARAM;
    return call_page_add(doc, width, height, out_page);
}

pdf_status pdf_doc_page_count(pdf_doc doc, size_t *out_count)
{
    if (check_doc(doc) != PDF_OK)
        return PDF_ERR_INVALID_HANDLE;
    if (!out_count)
        return PDF_ERR_INVALID_PARAM;
    if (!doc->vt->page_count)
        return PDF_ERR_BACKEND_UNSUPPORTED;
    return doc->vt->page_count(doc->backend_doc, out_count);
}

pdf_status pdf_page_get_size(pdf_page page, double *out_w, double *out_h)
{
    if (!page)
        return PDF_ERR_INVALID_HANDLE;
    if (!out_w || !out_h)
        return PDF_ERR_INVALID_PARAM;
    if (!page->vt->page_get_size)
        return PDF_ERR_BACKEND_UNSUPPORTED;
    return page->vt->page_get_size(page, out_w, out_h);
}

pdf_status pdf_page_set_rotation(pdf_page page, int degrees)
{
    if (!page)
        return PDF_ERR_INVALID_HANDLE;
    if (degrees % 90 != 0)
        return PDF_ERR_INVALID_PARAM;
    if (!page->vt->page_set_rotation)
        return PDF_ERR_BACKEND_UNSUPPORTED;
    return page->vt->page_set_rotation(page, degrees);
}

/* ------------------------------------------------------------------ */
/* Fonts                                                               */
/* ------------------------------------------------------------------ */

/* ---- TrueType Collection (.ttc) support ---------------------------- *
 * Windows ships several very common CJK fonts as collections
 * (simsun.ttc, msyh.ttc, mingliu.ttc, simfang.ttc ...). FreeType can
 * open a collection, but the EMBEDDING path in PoDoFo chokes on one:
 * subsetting a face lifted out of a collection throws an exception that
 * is not derived from std::exception, so it cannot even be translated
 * into a status code - it unwinds straight through the C ABI and kills
 * the host process.
 *
 * Rather than special-casing each backend, the core lifts face 0 out of
 * a collection and hands the backend a plain single-face sfnt, which is
 * what every backend (and every other language's caller) expects. The
 * result is that .ttc fonts embed cleanly on both engines.
 *
 * Layout (all integers big-endian):
 *
 *   TTC header    : 'ttcf' | version | numFonts | offset[numFonts]
 *   sfnt offset[0]: sfntVersion | numTables | searchRange |
 *                   entrySelector | rangeShift | tableRecord[numTables]
 *   tableRecord   : tag | checkSum | offset | length   (offset is
 *                   relative to the START OF THAT FACE, not the file)
 */

#define TTC_ALIGN4(n) (((n) + 3u) & ~3u)

static unsigned ttc_get_u16(const unsigned char *p)
{
    return (unsigned)((p[0] << 8) | p[1]);
}

static unsigned long ttc_get_u32(const unsigned char *p)
{
    return ((unsigned long)p[0] << 24) | ((unsigned long)p[1] << 16) |
           ((unsigned long)p[2] << 8) | (unsigned long)p[3];
}

static void ttc_put_u16(unsigned char *p, unsigned v)
{
    p[0] = (unsigned char)(v >> 8);
    p[1] = (unsigned char)(v & 0xff);
}

static void ttc_put_u32(unsigned char *p, unsigned long v)
{
    p[0] = (unsigned char)((v >> 24) & 0xff);
    p[1] = (unsigned char)((v >> 16) & 0xff);
    p[2] = (unsigned char)((v >> 8) & 0xff);
    p[3] = (unsigned char)(v & 0xff);
}

/* Does this buffer look like a TrueType Collection? */
static int ttc_is_collection(const unsigned char *d, size_t n)
{
    return n >= 12 && d[0] == 't' && d[1] == 't' && d[2] == 'c' && d[3] == 'f';
}

/* Extract one face from a collection into a standalone sfnt.
 * Returns 1 on success (*out is malloc'd, free with free()). */
static int ttc_extract(const unsigned char *src, size_t src_size,
                       unsigned face_index,
                       unsigned char **out, size_t *out_size)
{
    unsigned num_fonts, i, num_tables, j;
    unsigned long face_off, need;
    unsigned char *dst;
    const unsigned char *sfnt, *dir;
    unsigned long *offs;

    if (!ttc_is_collection(src, src_size))
        return 0;

    num_fonts = (unsigned)ttc_get_u32(src + 8);
    if (num_fonts == 0 || num_fonts > 64)
        return 0;
    if (face_index >= num_fonts)
        return 0;
    if ((size_t)12 + (size_t)num_fonts * 4 > src_size)
        return 0;

    face_off = ttc_get_u32(src + 12 + face_index * 4);
    if ((size_t)face_off + 12 > src_size)
        return 0;
    sfnt = src + face_off;

    /* a nested collection would need recursion; bail out cleanly */
    if (sfnt[0] == 't' && sfnt[1] == 't' && sfnt[2] == 'c' && sfnt[3] == 'f')
        return 0;

    num_tables = ttc_get_u16(sfnt + 4);
    if (num_tables == 0 || num_tables > 512)
        return 0;
    if ((size_t)face_off + 12 + (size_t)num_tables * 16 > src_size)
        return 0;
    dir = sfnt + 12;

    /* figure out how big the extracted font has to be */
    need = 12 + (unsigned long)num_tables * 16;
    offs = (unsigned long *)malloc((size_t)num_tables * sizeof(unsigned long));
    if (!offs)
        return 0;
    for (i = 0; i < num_tables; i++)
    {
        const unsigned char *rec = dir + (size_t)i * 16;
        unsigned long len = ttc_get_u32(rec + 12);
        offs[i] = need;
        need += TTC_ALIGN4(len);
    }
    if (need > (unsigned long)src_size + 12u) /* sanity: cannot grow */
    {
        free(offs);
        return 0;
    }

    dst = (unsigned char *)malloc((size_t)need);
    if (!dst)
    {
        free(offs);
        return 0;
    }

    /* header */
    memcpy(dst, sfnt, 12);

    /* table directory with relocated offsets. NOTE: inside a collection
     * the TableRecord offsets are absolute - they are measured from the
     * start of the .ttc FILE, not from the face's own sfnt directory. */
    for (j = 0; j < num_tables; j++)
    {
        const unsigned char *rec = dir + (size_t)j * 16;
        unsigned char *out_rec = dst + 12 + (size_t)j * 16;
        unsigned long len = ttc_get_u32(rec + 12);
        unsigned long toff = ttc_get_u32(rec + 8);
        unsigned long copy = len;

        memcpy(out_rec, rec, 8); /* tag + checkSum */
        ttc_put_u32(out_rec + 8, offs[j]);
        ttc_put_u32(out_rec + 12, len); /* NEW offset within the face */

        if ((size_t)toff > src_size)
        {
            free(offs);
            free(dst);
            return 0;
        }
        if ((size_t)toff + copy > src_size)
            copy = (unsigned long)(src_size - (size_t)toff);
        if (copy)
            memcpy(dst + offs[j], src + toff, (size_t)copy);
        /* zero the alignment padding */
        if (len > copy)
        {
            unsigned long pad = TTC_ALIGN4(len) - copy;
            if (pad > need - offs[j] - copy) /* keep inside the buffer */
                pad = 0;
            if (pad)
                memset(dst + offs[j] + copy, 0, (size_t)pad);
        }
    }

    free(offs);
    *out = dst;
    *out_size = (size_t)need;
    return 1;
}

/* Load an in-memory font, transparently unwrapping a collection. */
static pdf_status load_font_memory(pdf_doc doc, const unsigned char *data,
                                   size_t size, int embed,
                                   pdf_font *out_font)
{
    unsigned char *face = NULL;
    size_t face_size = 0;
    pdf_status rc;

    if (!data || size == 0)
        return PDF_ERR_INVALID_PARAM;
    if (!doc->vt->font_load_ttf_memory)
        return PDF_ERR_BACKEND_UNSUPPORTED;

    if (ttc_is_collection(data, size) &&
        ttc_extract(data, size, 0, &face, &face_size))
    {
        rc = doc->vt->font_load_ttf_memory(doc->backend_doc, face, face_size,
                                           embed, out_font);
        free(face);
        return rc;
    }
    return doc->vt->font_load_ttf_memory(doc->backend_doc, data, size, embed,
                                         out_font);
}

/* Read a font file and hand the backend either the path or, when it is a
 * collection, the extracted standalone face. */
static pdf_status load_font_file(pdf_doc doc, const char *path, int embed,
                                 pdf_font *out_font)
{
    FILE *f;
    long sz;
    unsigned char *buf = NULL;
    pdf_status rc;

    if (!path || !*path)
        return PDF_ERR_INVALID_PARAM;

    /* Plain single-face fonts keep the original behaviour: hand the path
     * straight to the backend, no buffering, no temp files. */
    if (!doc->vt->font_load_ttf_memory)
    {
        if (!doc->vt->font_load_ttf)
            return PDF_ERR_BACKEND_UNSUPPORTED;
        return doc->vt->font_load_ttf(doc->backend_doc, path, embed, out_font);
    }

    f = fopen(path, "rb");
    if (!f)
        return PDF_ERR_IO;
    if (fseek(f, 0, SEEK_END) != 0 || (sz = ftell(f)) < 0 ||
        fseek(f, 0, SEEK_SET) != 0)
    {
        fclose(f);
        return PDF_ERR_IO;
    }
    if (sz == 0)
    {
        fclose(f);
        return PDF_ERR_INVALID_PARAM;
    }
    buf = (unsigned char *)malloc((size_t)sz);
    if (!buf)
    {
        fclose(f);
        return PDF_ERR_OUT_OF_MEMORY;
    }
    if (fread(buf, 1, (size_t)sz, f) != (size_t)sz)
    {
        free(buf);
        fclose(f);
        return PDF_ERR_IO;
    }
    fclose(f);

    if (!ttc_is_collection(buf, (size_t)sz))
    {
        free(buf);
        if (!doc->vt->font_load_ttf)
            return PDF_ERR_BACKEND_UNSUPPORTED;
        return doc->vt->font_load_ttf(doc->backend_doc, path, embed, out_font);
    }

    /* --- collection ------------------------------------------------ *
     * Offer the backend the unwrapped face first (that is what PoDoFo
     * needs); if the backend cannot take a memory buffer - libharu only
     * has a file-based loader - fall back to the original path call so
     * the backend's own error is what the caller sees. No behaviour
     * change for backends that never supported collections either way.
     */
    {
        unsigned char *face = NULL;
        size_t face_size = 0;
        int have_path = doc->vt->font_load_ttf != NULL;

        if (ttc_extract(buf, (size_t)sz, 0, &face, &face_size))
        {
            rc = doc->vt->font_load_ttf_memory(doc->backend_doc, face,
                                               face_size, embed, out_font);
            free(face);
            if (rc == PDF_OK || !have_path)
            {
                free(buf);
                return rc;
            }
        }
        else if (!have_path)
        {
            free(buf);
            return PDF_ERR_BACKEND_FAILED;
        }
        free(buf);
        return doc->vt->font_load_ttf(doc->backend_doc, path, embed, out_font);
    }
}

pdf_status pdf_font_get_std(pdf_doc doc, pdf_std_font font, pdf_font *out_font)
{
    pdf_status rc;

    rc = check_doc_live(doc);
    if (rc != PDF_OK)
        return rc;
    if ((int)font < 0 || (int)font > 13 || !out_font)
        return PDF_ERR_INVALID_PARAM;
    if (!doc->vt->font_get_std)
        return PDF_ERR_BACKEND_UNSUPPORTED;
    return doc->vt->font_get_std(doc->backend_doc, font, out_font);
}

pdf_status pdf_font_load_ttf(pdf_doc doc, const char *path, int embed,
                             pdf_font *out_font)
{
    pdf_status rc;

    rc = check_doc_live(doc);
    if (rc != PDF_OK)
        return rc;
    if (!path || !*path || !out_font)
        return PDF_ERR_INVALID_PARAM;
    return load_font_file(doc, path, embed, out_font);
}

pdf_status pdf_font_load_ttf_memory(pdf_doc doc, const unsigned char *data,
                                    size_t size, int embed,
                                    pdf_font *out_font)
{
    pdf_status rc;

    rc = check_doc_live(doc);
    if (rc != PDF_OK)
        return rc;
    if (!data || size == 0 || !out_font)
        return PDF_ERR_INVALID_PARAM;
    return load_font_memory(doc, data, size, embed, out_font);
}

pdf_status pdf_font_find_system(pdf_doc doc, const char *family, int bold,
                                int italic, pdf_font *out_font)
{
    pdf_status rc;

    rc = check_doc_live(doc);
    if (rc != PDF_OK)
        return rc;
    if (!family || !*family || !out_font)
        return PDF_ERR_INVALID_PARAM;
    if (!doc->vt->font_find_system)
        return PDF_ERR_BACKEND_UNSUPPORTED;
    return doc->vt->font_find_system(doc->backend_doc, family, bold, italic,
                                     out_font);
}

pdf_status pdf_font_free(pdf_font font)
{
    if (!font)
        return PDF_ERR_INVALID_HANDLE;
    if (font->vt->font_free)
        font->vt->font_free(font);
    free(font);
    return PDF_OK;
}

pdf_status pdf_font_text_width(pdf_font font, const char *utf8_text,
                               double size, double *out_width)
{
    if (!font)
        return PDF_ERR_INVALID_HANDLE;
    if (!utf8_text || !out_width)
        return PDF_ERR_INVALID_PARAM;
    if (!font->vt->font_text_width)
        return PDF_ERR_BACKEND_UNSUPPORTED;
    return font->vt->font_text_width(font, utf8_text, size, out_width);
}

pdf_status pdf_font_ascent(pdf_font font, double size, double *out_ascent)
{
    if (!font)
        return PDF_ERR_INVALID_HANDLE;
    if (!out_ascent)
        return PDF_ERR_INVALID_PARAM;
    if (!font->vt->font_ascent)
        return PDF_ERR_BACKEND_UNSUPPORTED;
    return font->vt->font_ascent(font, size, out_ascent);
}

pdf_status pdf_font_descent(pdf_font font, double size, double *out_descent)
{
    if (!font)
        return PDF_ERR_INVALID_HANDLE;
    if (!out_descent)
        return PDF_ERR_INVALID_PARAM;
    if (!font->vt->font_descent)
        return PDF_ERR_BACKEND_UNSUPPORTED;
    return font->vt->font_descent(font, size, out_descent);
}

/* ------------------------------------------------------------------ */
/* Composite text: word wrapping (backend independent)                 */
/* ------------------------------------------------------------------ */

pdf_status pdf_page_draw_text(pdf_page page, pdf_font font,
                              const char *utf8_text, double x, double y,
                              double size)
{
    if (!page)
        return PDF_ERR_INVALID_HANDLE;
    if (!utf8_text)
        return PDF_ERR_INVALID_PARAM;
    if (!page->vt->draw_text)
        return PDF_ERR_BACKEND_UNSUPPORTED;
    return page->vt->draw_text(page, font, utf8_text, x, y, size);
}

pdf_status pdf_page_draw_text_aligned(pdf_page page, pdf_font font,
                                      const char *utf8_text, double x,
                                      double y, double size, double width,
                                      pdf_align align)
{
    if (!page)
        return PDF_ERR_INVALID_HANDLE;
    if (!utf8_text)
        return PDF_ERR_INVALID_PARAM;
    if (!page->vt->draw_text_aligned)
        return PDF_ERR_BACKEND_UNSUPPORTED;
    return page->vt->draw_text_aligned(page, font, utf8_text, x, y, size,
                                       width, align);
}

pdf_status pdf_page_draw_text_wrapped(pdf_page page, pdf_font font,
                                      const char *utf8_text, double x,
                                      double y, double width, double height,
                                      double size, double line_gap,
                                      pdf_align align)
{
    const pdf_backend_vtable *vt;
    const char *p;
    char line[512];
    double cursor_y;
    double line_h;
    double ascent = 0.0;
    size_t avail;

    if (!page || !font)
        return PDF_ERR_INVALID_HANDLE;
    if (!utf8_text || width <= 0 || size <= 0)
        return PDF_ERR_INVALID_PARAM;

    vt = page->vt;
    if (!vt->font_text_width || !vt->draw_text_aligned)
        return PDF_ERR_BACKEND_UNSUPPORTED;

    if (vt->font_ascent && vt->font_ascent(font, size, &ascent) == PDF_OK
        && ascent > 0)
        line_h = ascent + size * 0.25 + line_gap;
    else
        line_h = size * 1.2 + line_gap;

    /* y is the top of the box; the first baseline sits one ascent down. */
    cursor_y = y - ascent;

    avail = sizeof(line) - 1;
    p = utf8_text;
    while (*p)
    {
        const char *nl = strchr(p, '\n');
        size_t seg_len = nl ? (size_t)(nl - p) : strlen(p);
        const char *seg = p;
        double seg_w = 0.0;
        int truncated = 0;

        if (seg_len > avail)
        {
            seg_len = avail;
            truncated = 1;
        }

        /* Fast path: whole paragraph line fits. */
        if (!truncated
            && vt->font_text_width(font, seg, size, &seg_w) == PDF_OK
            && seg_w <= width)
        {
            if (cursor_y >= y - height)
            {
                vt->draw_text_aligned(page, font, seg, x, cursor_y, size,
                                      width, align);
            }
            cursor_y -= line_h;
            p = nl ? nl + 1 : p + seg_len;
            continue;
        }

        /* Greedy wrap on spaces. */
        while (seg_len > 0)
        {
            size_t take = seg_len;

            while (take > 0)
            {
                memcpy(line, seg, take);
                line[take] = '\0';
                if (vt->font_text_width(font, line, size, &seg_w) != PDF_OK)
                    return PDF_ERR_BACKEND_FAILED;
                if (seg_w <= width)
                    break;
                if (take == 1)
                    break; /* single char too wide: hard clip */
                /* walk back to the last space */
                {
                    size_t cut = take;
                    while (cut > 1 && line[cut - 1] != ' ')
                        cut--;
                    if (cut > 1)
                        take = cut - 1; /* drop the space itself */
                    else
                        take--; /* no space found: hard break */
                }
            }

            if (cursor_y >= y - height)
            {
                memcpy(line, seg, take);
                line[take] = '\0';
                vt->draw_text_aligned(page, font, line, x, cursor_y, size,
                                      width, align);
            }
            cursor_y -= line_h;

            seg += take;
            seg_len -= take;
            while (seg_len > 0 && *seg == ' ')
            {
                seg++;
                seg_len--;
            }
        }

        /* Resume after the segment the wrap loop just consumed.
         *
         * The wrap loop walks `seg`, not `p`, and counts `seg_len` down
         * to zero as it goes - so `p` is still sitting at the head of
         * the segment and `p + seg_len` would advance it by nothing.
         * The old code did exactly that, and the same paragraph was
         * re-wrapped and re-drawn from the top of the box until
         * `cursor_y` ran past the bottom: a 3-line paragraph in a
         * 140pt box was painted three times over itself.
         *
         * `seg` is where the loop stopped, which is the newline (or the
         * end of the string), so it is the right place to resume from. */
        p = seg;
        if (nl)
            p = nl + 1;

        if (cursor_y < y - height)
            break; /* box exhausted */
    }

    return PDF_OK;
}

/* ------------------------------------------------------------------ */
/* Graphics state                                                      */
/* ------------------------------------------------------------------ */

pdf_status pdf_gstate_save(pdf_page page)
{
    if (!page)
        return PDF_ERR_INVALID_HANDLE;
    if (!page->vt->gstate_save)
        return PDF_ERR_BACKEND_UNSUPPORTED;
    return page->vt->gstate_save(page);
}

pdf_status pdf_gstate_restore(pdf_page page)
{
    if (!page)
        return PDF_ERR_INVALID_HANDLE;
    if (!page->vt->gstate_restore)
        return PDF_ERR_BACKEND_UNSUPPORTED;
    return page->vt->gstate_restore(page);
}

pdf_status pdf_set_transform(pdf_page page, double a, double b, double c,
                             double d, double e, double f)
{
    if (!page)
        return PDF_ERR_INVALID_HANDLE;
    if (!page->vt->set_transform)
        return PDF_ERR_BACKEND_UNSUPPORTED;
    return page->vt->set_transform(page, a, b, c, d, e, f);
}

pdf_status pdf_set_line_width(pdf_page page, double w)
{
    if (!page)
        return PDF_ERR_INVALID_HANDLE;
    if (w < 0)
        return PDF_ERR_INVALID_PARAM;
    if (!page->vt->set_line_width)
        return PDF_ERR_BACKEND_UNSUPPORTED;
    return page->vt->set_line_width(page, w);
}

pdf_status pdf_set_line_cap(pdf_page page, pdf_line_cap cap)
{
    if (!page)
        return PDF_ERR_INVALID_HANDLE;
    if (!page->vt->set_line_cap)
        return PDF_ERR_BACKEND_UNSUPPORTED;
    return page->vt->set_line_cap(page, cap);
}

pdf_status pdf_set_line_join(pdf_page page, pdf_line_join join)
{
    if (!page)
        return PDF_ERR_INVALID_HANDLE;
    if (!page->vt->set_line_join)
        return PDF_ERR_BACKEND_UNSUPPORTED;
    return page->vt->set_line_join(page, join);
}

pdf_status pdf_set_miter_limit(pdf_page page, double limit)
{
    if (!page)
        return PDF_ERR_INVALID_HANDLE;
    if (!page->vt->set_miter_limit)
        return PDF_ERR_BACKEND_UNSUPPORTED;
    return page->vt->set_miter_limit(page, limit);
}

pdf_status pdf_set_line_dash(pdf_page page, const double *pattern, size_t n,
                             double phase)
{
    if (!page)
        return PDF_ERR_INVALID_HANDLE;
    if (n > 0 && !pattern)
        return PDF_ERR_INVALID_PARAM;
    if (n > 8)
        n = 8;
    if (!page->vt->set_line_dash)
        return PDF_ERR_BACKEND_UNSUPPORTED;
    return page->vt->set_line_dash(page, pattern, n, phase);
}

pdf_status pdf_set_fill_color(pdf_page page, const pdf_color *c)
{
    if (!page)
        return PDF_ERR_INVALID_HANDLE;
    if (!c)
        return PDF_ERR_INVALID_PARAM;
    if (!page->vt->set_fill_color)
        return PDF_ERR_BACKEND_UNSUPPORTED;
    return page->vt->set_fill_color(page, c);
}

pdf_status pdf_set_stroke_color(pdf_page page, const pdf_color *c)
{
    if (!page)
        return PDF_ERR_INVALID_HANDLE;
    if (!c)
        return PDF_ERR_INVALID_PARAM;
    if (!page->vt->set_stroke_color)
        return PDF_ERR_BACKEND_UNSUPPORTED;
    return page->vt->set_stroke_color(page, c);
}

pdf_status pdf_set_opacity(pdf_page page, double fill_alpha,
                           double stroke_alpha)
{
    if (!page)
        return PDF_ERR_INVALID_HANDLE;
    if (!page->vt->set_opacity)
        return PDF_ERR_BACKEND_UNSUPPORTED;
    return page->vt->set_opacity(page, fill_alpha, stroke_alpha);
}

pdf_status pdf_set_blend_mode(pdf_page page, pdf_blend_mode mode)
{
    if (!page)
        return PDF_ERR_INVALID_HANDLE;
    if (!page->vt->set_blend_mode)
        return PDF_ERR_BACKEND_UNSUPPORTED;
    return page->vt->set_blend_mode(page, mode);
}

pdf_status pdf_set_text_render_mode(pdf_page page, pdf_text_render_mode mode)
{
    if (!page)
        return PDF_ERR_INVALID_HANDLE;
    if (!page->vt->set_text_render_mode)
        return PDF_ERR_BACKEND_UNSUPPORTED;
    return page->vt->set_text_render_mode(page, mode);
}

/* ------------------------------------------------------------------ */
/* Composite path primitives                                           */
/* ------------------------------------------------------------------ */

pdf_status pdf_path_move_to(pdf_page page, double x, double y)
{
    if (!page)
        return PDF_ERR_INVALID_HANDLE;
    if (!page->vt->path_move_to)
        return PDF_ERR_BACKEND_UNSUPPORTED;
    return page->vt->path_move_to(page, x, y);
}

pdf_status pdf_path_line_to(pdf_page page, double x, double y)
{
    if (!page)
        return PDF_ERR_INVALID_HANDLE;
    if (!page->vt->path_line_to)
        return PDF_ERR_BACKEND_UNSUPPORTED;
    return page->vt->path_line_to(page, x, y);
}

pdf_status pdf_path_curve_to(pdf_page page, double x1, double y1, double x2,
                             double y2, double x3, double y3)
{
    if (!page)
        return PDF_ERR_INVALID_HANDLE;
    if (!page->vt->path_curve_to)
        return PDF_ERR_BACKEND_UNSUPPORTED;
    return page->vt->path_curve_to(page, x1, y1, x2, y2, x3, y3);
}

pdf_status pdf_path_close(pdf_page page)
{
    if (!page)
        return PDF_ERR_INVALID_HANDLE;
    if (!page->vt->path_close)
        return PDF_ERR_BACKEND_UNSUPPORTED;
    return page->vt->path_close(page);
}

pdf_status pdf_path_rectangle(pdf_page page, const pdf_rect *r)
{
    if (!page)
        return PDF_ERR_INVALID_HANDLE;
    if (!r || r->w < 0 || r->h < 0)
        return PDF_ERR_INVALID_PARAM;
    if (!page->vt->path_rectangle)
        return PDF_ERR_BACKEND_UNSUPPORTED;
    return page->vt->path_rectangle(page, r);
}

pdf_status pdf_path_circle(pdf_page page, double cx, double cy, double radius)
{
    return pdf_path_ellipse(page, cx, cy, radius, radius);
}

pdf_status pdf_path_ellipse(pdf_page page, double cx, double cy, double rx,
                            double ry)
{
    static const double kappa = 0.552284749830793398400342380804583;
    double kx = rx * kappa;
    double ky = ry * kappa;
    pdf_status rc;

    if (!page)
        return PDF_ERR_INVALID_HANDLE;
    if (rx < 0 || ry < 0)
        return PDF_ERR_INVALID_PARAM;

    if ((rc = pdf_path_move_to(page, cx + rx, cy)) != PDF_OK)
        return rc;
    if ((rc = pdf_path_curve_to(page, cx + rx, cy + ky, cx + kx, cy + ry,
                                cx, cy + ry)) != PDF_OK)
        return rc;
    if ((rc = pdf_path_curve_to(page, cx - kx, cy + ry, cx - rx, cy + ky,
                                cx - rx, cy)) != PDF_OK)
        return rc;
    if ((rc = pdf_path_curve_to(page, cx - rx, cy - ky, cx - kx, cy - ry,
                                cx, cy - ry)) != PDF_OK)
        return rc;
    return pdf_path_curve_to(page, cx + kx, cy - ry, cx + rx, cy - ky,
                             cx + rx, cy);
}

pdf_status pdf_path_arc(pdf_page page, double cx, double cy, double radius,
                        double a1, double a2, int ccw)
{
    double start, end;
    pdf_status rc;
    int i, nseg;

    if (!page)
        return PDF_ERR_INVALID_HANDLE;
    if (radius <= 0)
        return PDF_ERR_INVALID_PARAM;

    start = a1;
    end = a2;
    if (ccw)
    {
        while (end <= start)
            end += 2.0 * PDF_PI;
    }
    else
    {
        while (end >= start)
            end -= 2.0 * PDF_PI;
    }

    /* Split into segments of at most 90 degrees. */
    nseg = (int)ceil(fabs(end - start) / (PDF_PI / 2.0));
    if (nseg < 1)
        nseg = 1;

    {
        double x0 = cx + radius * cos(start);
        double y0 = cy + radius * sin(start);

        /* A fresh path has no current point: engines reject LineTo there
         * (HPDF_INVALID_OPERATION from libharu). Fall back to MoveTo --
         * the arc then simply starts a new subpath. */
        rc = pdf_path_line_to(page, x0, y0);
        if (rc != PDF_OK)
            rc = pdf_path_move_to(page, x0, y0);
        if (rc != PDF_OK)
            return rc;
    }

    for (i = 1; i <= nseg; i++)
    {
        double a0 = start + (end - start) * (double)(i - 1) / nseg;
        double a3 = start + (end - start) * (double)i / nseg;
        double sweep = fabs(a3 - a0);
        double k = bezier_k(sweep);
        double dir = ccw ? 1.0 : -1.0;
        double x0 = cx + radius * cos(a0);
        double y0 = cy + radius * sin(a0);
        double x3 = cx + radius * cos(a3);
        double y3 = cy + radius * sin(a3);
        double c1x = x0 - dir * k * radius * sin(a0);
        double c1y = y0 + dir * k * radius * cos(a0);
        double c2x = x3 + dir * k * radius * sin(a3);
        double c2y = y3 - dir * k * radius * cos(a3);

        rc = pdf_path_curve_to(page, c1x, c1y, c2x, c2y, x3, y3);
        if (rc != PDF_OK)
            return rc;
    }

    return PDF_OK;
}

pdf_status pdf_path_polygon(pdf_page page, const pdf_point *pts, size_t n)
{
    size_t i;
    pdf_status rc;

    if (!page)
        return PDF_ERR_INVALID_HANDLE;
    if (!pts || n < 2)
        return PDF_ERR_INVALID_PARAM;

    if ((rc = pdf_path_move_to(page, pts[0].x, pts[0].y)) != PDF_OK)
        return rc;
    for (i = 1; i < n; i++)
    {
        rc = pdf_path_line_to(page, pts[i].x, pts[i].y);
        if (rc != PDF_OK)
            return rc;
    }
    return PDF_OK;
}

pdf_status pdf_path_paint(pdf_page page, pdf_path_paint_mode mode)
{
    if (!page)
        return PDF_ERR_INVALID_HANDLE;
    if (!page->vt->path_paint)
        return PDF_ERR_BACKEND_UNSUPPORTED;
    return page->vt->path_paint(page, mode);
}

pdf_status pdf_path_discard(pdf_page page)
{
    if (!page)
        return PDF_ERR_INVALID_HANDLE;
    if (!page->vt->path_discard)
        return PDF_ERR_BACKEND_UNSUPPORTED;
    return page->vt->path_discard(page);
}

pdf_status pdf_page_draw_rect(pdf_page page, const pdf_rect *r,
                              pdf_path_paint_mode mode)
{
    pdf_status rc;

    if ((rc = pdf_path_rectangle(page, r)) != PDF_OK)
        return rc;
    return pdf_path_paint(page, mode);
}

pdf_status pdf_page_draw_line(pdf_page page, double x1, double y1, double x2,
                              double y2)
{
    pdf_status rc;

    if (!page)
        return PDF_ERR_INVALID_HANDLE;
    if ((rc = pdf_path_move_to(page, x1, y1)) != PDF_OK)
        return rc;
    if ((rc = pdf_path_line_to(page, x2, y2)) != PDF_OK)
        return rc;
    return pdf_path_paint(page, PDF_PATH_STROKE);
}

/* ------------------------------------------------------------------ */
/* Images                                                              */
/* ------------------------------------------------------------------ */

pdf_status pdf_image_load_file(pdf_doc doc, const char *path,
                               pdf_image *out_image)
{
    pdf_status rc;

    rc = check_doc_live(doc);
    if (rc != PDF_OK)
        return rc;
    if (!path || !*path || !out_image)
        return PDF_ERR_INVALID_PARAM;
    if (!doc->vt->image_load_file)
        return PDF_ERR_BACKEND_UNSUPPORTED;
    return doc->vt->image_load_file(doc->backend_doc, path, out_image);
}

pdf_status pdf_image_load_memory(pdf_doc doc, const unsigned char *data,
                                 size_t size, pdf_image *out_image)
{
    pdf_status rc;

    rc = check_doc_live(doc);
    if (rc != PDF_OK)
        return rc;
    if (!data || size == 0 || !out_image)
        return PDF_ERR_INVALID_PARAM;
    if (!doc->vt->image_load_memory)
        return PDF_ERR_BACKEND_UNSUPPORTED;
    return doc->vt->image_load_memory(doc->backend_doc, data, size,
                                      out_image);
}

pdf_status pdf_image_free(pdf_image image)
{
    if (!image)
        return PDF_ERR_INVALID_HANDLE;
    if (image->vt->image_free)
        image->vt->image_free(image);
    free(image);
    return PDF_OK;
}

pdf_status pdf_image_get_size(pdf_image image, size_t *out_w, size_t *out_h)
{
    if (!image)
        return PDF_ERR_INVALID_HANDLE;
    if (!out_w || !out_h)
        return PDF_ERR_INVALID_PARAM;
    if (!image->vt->image_get_size)
        return PDF_ERR_BACKEND_UNSUPPORTED;
    return image->vt->image_get_size(image, out_w, out_h);
}

pdf_status pdf_page_draw_image(pdf_page page, pdf_image image,
                               const pdf_rect *dest)
{
    if (!page || !image)
        return PDF_ERR_INVALID_HANDLE;
    if (!dest || dest->w <= 0 || dest->h <= 0)
        return PDF_ERR_INVALID_PARAM;
    if (!page->vt->draw_image)
        return PDF_ERR_BACKEND_UNSUPPORTED;
    return page->vt->draw_image(page, image, dest);
}

/* ------------------------------------------------------------------ */
/* Annotations & outlines                                              */
/* ------------------------------------------------------------------ */

pdf_status pdf_annot_create(pdf_page page, pdf_annot_type type,
                            const pdf_rect *rect, const char *utf8_content,
                            pdf_annot *out_annot)
{
    if (!page || !rect || !out_annot)
        return PDF_ERR_INVALID_HANDLE;
    if (!page->vt->annot_create)
        return PDF_ERR_BACKEND_UNSUPPORTED;
    return page->vt->annot_create(page, type, rect, utf8_content, out_annot);
}

pdf_status pdf_annot_link_to_page(pdf_annot annot, pdf_page *dest,
                                  const char *uri)
{
    if (!annot)
        return PDF_ERR_INVALID_HANDLE;
    if (!annot->vt->annot_link_to_page)
        return PDF_ERR_BACKEND_UNSUPPORTED;
    return annot->vt->annot_link_to_page(annot, dest, uri);
}

pdf_status pdf_annot_set_color(pdf_annot annot, const pdf_color *c)
{
    if (!annot)
        return PDF_ERR_INVALID_HANDLE;
    if (!c)
        return PDF_ERR_INVALID_PARAM;
    if (!annot->vt->annot_set_color)
        return PDF_ERR_BACKEND_UNSUPPORTED;
    return annot->vt->annot_set_color(annot, c);
}

pdf_status pdf_annot_set_icon(pdf_annot annot, pdf_annot_icon icon)
{
    if (!annot)
        return PDF_ERR_INVALID_HANDLE;
    if (!annot->vt->annot_set_icon)
        return PDF_ERR_BACKEND_UNSUPPORTED;
    return annot->vt->annot_set_icon(annot, icon);
}

pdf_status pdf_annot_set_opacity(pdf_annot annot, double alpha)
{
    if (!annot)
        return PDF_ERR_INVALID_HANDLE;
    if (!annot->vt->annot_set_opacity)
        return PDF_ERR_BACKEND_UNSUPPORTED;
    return annot->vt->annot_set_opacity(annot, alpha);
}

pdf_status pdf_annot_set_author(pdf_annot annot, const char *author)
{
    if (!annot)
        return PDF_ERR_INVALID_HANDLE;
    if (!annot->vt->annot_set_author)
        return PDF_ERR_BACKEND_UNSUPPORTED;
    return annot->vt->annot_set_author(annot, author);
}

pdf_status pdf_annot_set_subject(pdf_annot annot, const char *subject)
{
    if (!annot)
        return PDF_ERR_INVALID_HANDLE;
    if (!annot->vt->annot_set_subject)
        return PDF_ERR_BACKEND_UNSUPPORTED;
    return annot->vt->annot_set_subject(annot, subject);
}

pdf_status pdf_annot_set_open(pdf_annot annot, int opened)
{
    if (!annot)
        return PDF_ERR_INVALID_HANDLE;
    if (!annot->vt->annot_set_open)
        return PDF_ERR_BACKEND_UNSUPPORTED;
    return annot->vt->annot_set_open(annot, opened);
}

pdf_status pdf_annot_free(pdf_annot annot)
{
    if (!annot)
        return PDF_ERR_INVALID_HANDLE;
    if (annot->vt->annot_free)
        annot->vt->annot_free(annot);
    free(annot);
    return PDF_OK;
}

pdf_status pdf_outline_add(pdf_doc doc, pdf_outline parent, const char *title,
                           pdf_page *dest_page, pdf_outline *out_outline)
{
    if (check_doc(doc) != PDF_OK)
        return PDF_ERR_INVALID_HANDLE;
    if (!title || !out_outline)
        return PDF_ERR_INVALID_PARAM;
    if (!doc->vt->outline_add)
        return PDF_ERR_BACKEND_UNSUPPORTED;
    return doc->vt->outline_add(doc->backend_doc, parent, title, dest_page,
                                out_outline);
}

pdf_status pdf_outline_set_open(pdf_outline outline, int opened)
{
    if (!outline)
        return PDF_ERR_INVALID_HANDLE;
    if (!outline->vt->outline_set_open)
        return PDF_ERR_BACKEND_UNSUPPORTED;
    return outline->vt->outline_set_open(outline, opened);
}

pdf_status pdf_outline_free(pdf_outline outline)
{
    if (!outline)
        return PDF_ERR_INVALID_HANDLE;
    if (outline->vt->outline_free)
        outline->vt->outline_free(outline);
    free(outline);
    return PDF_OK;
}
