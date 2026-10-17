/**
 * pdf_init.c - registry storage and one-time initialization.
 *
 * Backends register themselves lazily through pdf_ensure_backends(),
 * which the core calls before any document creation. Registration is
 * idempotent AND thread-safe: the guard uses an atomic compare-exchange
 * so two threads racing on the first pdf_doc_create() cannot interleave
 * the registration sequence. Threads that lose the race simply proceed;
 * the registry writes of the winning thread become visible to them
 * through the acquire barrier in the CAS loop.
 */
#include "pdf_core.h"
#include "pdf_backend.h"
#include "pdf_internal.h"

#include <string.h>

#if defined(_WIN32)
#  include <windows.h>
#elif defined(__GNUC__) || defined(__clang__)
#  include <stdatomic.h>
#endif

const pdf_backend_vtable *g_registry[PDF_BACKEND_MAX];
pdf_backend g_active_id = PDF_BACKEND_AUTO;

/** Make sure every compiled-in backend has registered itself. */
void pdf_ensure_backends(void)
{
#if defined(_WIN32)
    static volatile LONG initialized = 0;
    LONG prev = InterlockedCompareExchange(&initialized, 1, 0);

    if (prev == 2)
        return; /* fully initialized */
    if (prev == 1)
    {
        /* Another thread won the race and is still registering. Wait for
         * it to publish state 2: returning immediately (the old behavior)
         * let the loser run document creation against a half-filled
         * registry, so PDF_BACKEND_AUTO could come back NULL. The
         * compare-exchange with comparand 2 performs an acquire read. */
        while (InterlockedCompareExchange(&initialized, 2, 2) != 2)
            Sleep(0);
        return;
    }

    /* We won: register, then publish the fully-built registry. */
    pdf_backend_dummy_register();
#  ifdef PDFLIB_BACKEND_LIBHARU_ENABLED
    pdf_backend_libharu_register();
#  endif
#  ifdef PDFLIB_BACKEND_PODOFO_ENABLED
    pdf_backend_podofo_register();
#  endif
    InterlockedExchange(&initialized, 2); /* publish: done */
#elif defined(__STDC_NO_ATOMICS__)
    /* Fallback without C11 atomics: single-threaded init semantics. */
    static int initialized = 0;
    if (initialized == 2)
        return;
    initialized = 1;
    pdf_backend_dummy_register();
#  ifdef PDFLIB_BACKEND_LIBHARU_ENABLED
    pdf_backend_libharu_register();
#  endif
#  ifdef PDFLIB_BACKEND_PODOFO_ENABLED
    pdf_backend_podofo_register();
#  endif
    initialized = 2;
#elif defined(__GNUC__) || defined(__clang__)
    static atomic_int initialized = 0;
    int expected = 0;
    if (atomic_compare_exchange_strong(&initialized, &expected, 1))
    {
        /* we won the race: register everything, then release */
        pdf_backend_dummy_register();
#  ifdef PDFLIB_BACKEND_LIBHARU_ENABLED
        pdf_backend_libharu_register();
#  endif
#  ifdef PDFLIB_BACKEND_PODOFO_ENABLED
        pdf_backend_podofo_register();
#  endif
        atomic_store_explicit(&initialized, 2, memory_order_release);
        return;
    }
    /* wait until the winner has published the registry */
    while (atomic_load_explicit(&initialized, memory_order_acquire) != 2)
    {
        /* registration runs to completion; no cancellable work here */
    }
#else
#  error "no atomic primitive available for thread-safe initialization"
#endif
}
