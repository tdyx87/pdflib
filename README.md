# pdflib

A backend-agnostic **C** wrapper library for generating PDF documents.
The public API (`pdflib/pdf_api.h`) never exposes the underlying engines;
the concrete implementation — **libharu** or **PoDoFo** — is selected at
runtime with `pdflib_set_backend()`, and both engines are provisioned
through **Conan**.

```
        +---------------------------+
        |   your application        |
        |   #include <pdflib/pdf_api.h>
        +------------+--------------+
                     |
             pdflib_* / pdf_*  (pure C API)
                     |
        +------------v--------------+
        |   pdflib core (pdf_core.c)|   registry + dispatch +
        |   knows nothing about the |   composite drawing (wrap,
        |   engines                 |   arcs, polygons, ellipses)
        +---+-------------------+---+
            | vtable (pdf_backend.h)
   +--------v--------+  +---------v-------+  +---------v------+
   | libharu backend |  | PoDoFo backend  |  | dummy backend  |
   | (C, hpdf.h)     |  | (C++, podofo.h) |  | (always built) |
   +-----------------+  +-----------------+  +----------------+
```

## Features

- Documents: create, save to file or memory, stream to a callback or to
  an OS handle (`pdf_doc_save_to_stream` / `pdf_doc_save_to_handle`),
  metadata (title/author/...)
- Pages: ISO/US standard sizes + custom sizes, rotation
- Text: 14 standard fonts, TrueType from file/memory/system, widths,
  ascender/descender, aligned and word-wrapped paragraphs
- Graphics: colors (gray/RGB/CMYK + alpha), line width/cap/join/dash,
  opacity, blend modes, text render modes, transforms, full path model
  (move/line/curve/rect/circle/ellipse/arc/polygon) with the five PDF
  paint modes
- Images: PNG/JPEG from file or memory, scaled drawing
- Annotations: text, highlight, underline, squiggly, strikeout, square,
  circle, URI links, internal links
- Outlines (bookmarks) with hierarchy and destinations
- Security: RC4-40/128, AES-128/256 with fine-grained permissions
  (where supported by the engine)
- Compression control, viewer page mode

Anything a backend cannot do returns `PDF_ERR_BACKEND_UNSUPPORTED`
instead of failing silently; the capability table is the vtable in
`pdflib/pdf_backend.h`.

## libharu-compatible API (`HPDF_*`)

The native `pdflib.dll` exports **87** `pdf_*` / `pdflib_*` functions. That is
**not** the libharu API: libharu 2.4.4 declares **281** public `HPDF_*`
functions, and the two sets share no names. If you want to keep using
libharu-style code, the `compat/` directory adds a source/link-compatible
facade:

- `compat/include/hpdf.h` - libharu 2.4.4 public declarations (opaque handles);
- `compat/hpdf_compat.c` - `HPDF_*` mapped onto the stable `pdf_*` core;
- `hpdf.lib` - static library, can replace libharu's `hpdf.lib`;
- `pdflib_hpdf.dll` - cdecl DLL facade (links `pdflib.dll`).

```c
#include <hpdf.h>                       /* compat/include */

HPDF_Doc  pdf  = HPDF_New(NULL, NULL);
HPDF_Page page = HPDF_AddPage(pdf);
HPDF_Page_SetSize(page, HPDF_PAGE_SIZE_A4, HPDF_PAGE_PORTRAIT);
HPDF_Font font = HPDF_GetFont(pdf, "Helvetica", NULL);
HPDF_Page_BeginText(page);
HPDF_Page_SetFontAndSize(page, font, 18);
HPDF_Page_TextOut(page, 60, 760, "same source, two engines");
HPDF_Page_EndText(page);
HPDF_SaveToFile(pdf, "out.pdf");
HPDF_Free(pdf);
```

Both engines are available at runtime:

```c
HPDF_SetBackend(PDFLIB_HPDF_BACKEND_PODOFO);   /* or LIBHARU / DUMMY / AUTO */
```

or `set PDFLIB_BACKEND=podofo`. **219** of the 281 public entries are real
implementations; **65** are cold entries (3D, ICC, shading, raw images, page
labels, attachments...) that return `HPDF_UNSUPPORTED_FUNC` instead of
pretending to work. See [docs/libharu-compatibility.md](docs/libharu-compatibility.md)
for the exact mapping, semantics and limitations.

## Building

### 1) With conan (both engines)

```bash
# One-time: export the bundled podofo recipe (see below), then:
conan install . -s build_type=Release -o "podofo-local/*:with_jpeg=False" --build=missing -of build/all-backends -c tools.cmake.cmaketoolchain:generator=Ninja -c tools.files.download:verify=False

cmake --preset all-backends        # run from a vcvars/Ninja environment on Windows
cmake --build --preset all-backends
ctest --test-dir build/all-backends --output-on-failure
./build/all-backends/pdf_hello hello.pdf podofo
./build/all-backends/pdf_hello hello.pdf libharu
```

`conanfile.py` pins the engines:

- `libharu/2.4.4` (option `with_libharu`)
- `podofo-local/0.9.7` (option `with_podofo`) — a local recipe in
  `recipes/podofo/all/`, exported with `conan export recipes/podofo/all`
  (the recipe itself carries `name`/`version`; the `conan export <path>
  <ref>@` form is Conan 1 syntax and is rejected by Conan 2)

Disable either one to ship a lighter binary — the registry simply won't
contain that engine, and `pdflib_set_backend()` reports
`PDF_ERR_BACKEND_NOT_FOUND`.

### Why podofo 0.9.7 (and not 1.0.x)

Upstream PoDoFo 1.0 made OpenSSL a **mandatory** build dependency
(`find_package(OpenSSL REQUIRED)`; the encryption/CMS code unconditionally
includes `<openssl/*.h>`). PoDoFo 0.9.x keeps libcrypto optional
(`FIND_PACKAGE(LIBCRYPTO)`), so the bundled recipe exposes
`podofo-local/*:with_openssl=False` (the default) and produces a
crypto-free library. The pdflib PoDoFo adapter targets the 0.9.x API and
reports `PDF_ERR_BACKEND_UNSUPPORTED` from `pdf_doc_set_encryption()`
when the engine was built without crypto. AES-256 additionally needs
`with_libidn=True` (0.9.x gates AES-256 behind libidn).

Optional codecs (`with_jpeg`, `with_png`, `with_tiff`) are also switchable;
without a codec `pdf_image_load_*` fails at runtime with
`PDF_ERR_BACKEND_FAILED`.

### 2) Core only (no third-party dependencies)

```bash
cmake --preset core-only
cmake --build --preset core-only
ctest --preset core-only
```

This builds the core plus the `dummy` backend, which accepts every call
and is used by the unit tests. Useful for CI, fuzzing the API surface,
and as a template for new backends.

### 3) Continuous integration

[.github/workflows/ci.yml](.github/workflows/ci.yml) runs the three
pdflibdrv configurations on a Windows runner - x64 `/MD`, x64 `/MT` and
x86 + Windows XP - each through the shared, path-agnostic
[ci/build-windows.ps1](ci/build-windows.ps1). That script locates Visual
Studio itself, so it also runs locally:

```powershell
# cmake and ninja must be on PATH
./ci/build-windows.ps1 -Variant md|mt|x86xp
```

See [docs/pdflibdrv-abi.md](docs/pdflibdrv-abi.md) for what each job
checks and how a hosted runner gets the VS2017 / `v141_xp` toolchain.

## Output modes & font embedding

Documents can be serialized three ways, all available on every backend:

```c
/* 1. full save to a file */
pdf_doc_save_to_file(doc, "out.pdf");

/* 2. full save to a caller buffer (NULL buf = size query) */
size_t size;
pdf_doc_save_to_memory(doc, NULL, &size);
unsigned char *buf = malloc(size);
pdf_doc_save_to_memory(doc, buf, &size);

/* 3. streaming: chunks are pushed to your callback while the engine
 *    serializes - no full-document buffer needed on the caller side.
 *    Return != PDF_OK from the callback to abort. */
pdf_status my_writer(const unsigned char *data, size_t len, void *ud);
pdf_doc_save_to_stream(doc, my_writer, userdata);

/* convenience: stream into an already-open OS handle - a Win32 HANDLE
 * (stdout, a pipe, a socket, a spooler) or a POSIX file descriptor; the
 * handle is written to, never closed */
pdf_doc_save_to_handle(doc, (void *)GetStdHandle(STD_OUTPUT_HANDLE));
```

Why a handle and not a `FILE*`: the DLL links the CRT statically, so its
`FILE` table is private to it. A `FILE*` opened by the caller belongs to
the caller's CRT, and the DLL's own `fwrite`/`fflush` reject it as an
invalid parameter - which aborts the process on the spot, uncatchably.
An OS handle is process-wide, so it means the same thing on both sides.

PoDoFo streams natively (a `PdfOutputDevice` subclass forwards bytes and
keeps xref offsets correct); libharu serializes into its internal stream
and pushes 16 KiB chunks. Both produce byte-identical output compared to
`pdf_doc_save_to_memory`.

TrueType fonts: `pdf_font_load_ttf` (file), `pdf_font_load_ttf_memory`
(memory buffer) and `pdf_font_find_system` (system font directories).
The `embed` flag controls subsetting/embedding; PoDoFo honours both
values, libharu always embeds TTF fonts (it returns
`PDF_ERR_BACKEND_UNSUPPORTED` for `embed = 0`), which yields a
`/FontFile2` entry in the output. On PoDoFo, `embed = 1` creates a true
glyph subset (only the shapes actually drawn are embedded): a 5-page
document drawn from `arial.ttf` (378 KB) ships a ~20 KB `/FontFile2`.

## Streamed documents (unknown page count)

For printer-driver style jobs the number of pages is not known until the
source commands stop arriving, and holding every page in memory is not
an option. Use streamed creation: the document is bound to a sink at
construction, each page is pushed out as soon as it is finished, and
`pdf_doc_finish` embeds pending font subsets and writes the xref/trailer.

```c
pdf_doc doc;
pdf_doc_create_streamed_to_file("out.pdf", &doc);  /* or ..._to_stream */

for (;;) {
    pdf_page page;
    /* pages may be preset or fully custom sized */
    pdf_page_add(doc, PDF_PAGE_A4, PDF_ORIENTATION_PORTRAIT, &page);
    /* pdf_page_add_custom(doc, 612.0, 792.0, &page); */
    ... /* draw the commands of this sheet */
}

pdf_doc_finish(doc);   /* required: flushes subsets + xref/trailer */
pdf_doc_free(doc);
```

- Pages, fonts and images are streamed out as they complete; memory use
  does not grow with page count.
- `pdf_doc_save_to_file/memory/stream` are rejected on a streamed
  document (`PDF_ERR_INVALID_STATE`) - output is already flowing.
- PoDoFo implements this natively (`PdfStreamedDocument`); backends
  without support return `PDF_ERR_BACKEND_UNSUPPORTED` from the create
  call, so callers can fall back to the buffered API.

### Sink failures mid-document (abort contract)

A streaming sink (callback, OS handle, disk) may fail at any moment - disk
full, connection dropped, consumer gone. The failure contract is:

- The sink's error code propagates verbatim (e.g. `PDF_ERR_IO`); it is
  not masked by generic handle errors.
- Once a sink reports failure the document is poisoned: every further
  page/font/image call fails fast with the same code and **no further
  bytes are pushed to the sink**.
- `pdf_doc_finish` re-reports the failure (idempotently) and a
  half-written target file from `pdf_doc_create_streamed_to_file` is
  removed, so no torn PDF is ever left behind.
- Backend internals are also guarded: a page-level engine error returns
  `PDF_ERR_BACKEND_FAILED` instead of crashing.

```c
rc = pdf_page_add(doc, PDF_PAGE_A4, PDF_ORIENTATION_PORTRAIT, &page);
if (rc != PDF_OK) {
    pdf_doc_finish(doc);      /* returns the original sink error */
    pdf_doc_free(doc);        /* releases everything, file already removed */
}
```

## Threading and handle ownership

**Threading.** Every call is thread-safe under one rule: a given object
(document, page, font, image, annot, outline) must be touched by only
one thread at a time. In particular:

- Documents are independent - N threads can each create pages, draw,
  serialize and free their own document with no locking. The PoDoFo
  adapter resolves each handle back to its owning document, so handles
  do not depend on which thread created them.
- Sharing ONE document across threads is the caller's job: serialize
  all calls on that document (and its children) behind a lock.
- `pdflib_set_backend()` is a process-global selector. Concurrent calls
  cannot corrupt state, but a racing pair makes which backend the next
  `pdf_doc_create()` picks undefined - set it once at startup. The core
  resolves the selector once per create call and pairs the chosen
  vtable with the backend payload atomically, so a race can never mix
  two backends inside one handle (that used to be a real
  type-confusion crash; the thread suite's selector-race stage guards
  the fix).
- The one-time lazy backend registration is internally synchronized
  (atomic compare-exchange); no init call is needed.

`tests/test_threads.c` (run via ctest as `pdflib_thread_tests`) runs
against EVERY compiled-in backend, not just the dummy engine:
concurrent document creation (8 threads x 6 docs each, with std-font
text), concurrent streamed creation, the global selector race, and
lifecycle contracts. It is randomized - a CI loop or ASan/TSan run
over many repetitions is the intended hardening step.

**Known third-party engine limits.** The per-handle ownership model is
enforced by pdflib itself, but the engines have their own limits:
PoDoFo 0.9.7 uses lazily-initialized process-global singletons (e.g.
`PdfEncodingFactory` instances, protected by its own mutex) and
libharu allocates all objects through a per-document memory manager,
so the contracts above hold for these versions - but re-audit any
engine upgrade against this test suite.

**Handle ownership.** A `pdf_doc` owns the handles it created. When it
is freed, all page/font/image/annot/outline handles become invalid.
Font, image, annot and outline handles may be freed early
(`pdf_font_free` etc.) to release their wrappers - the document stays
valid and the produced PDF is unchanged. There is no `pdf_page_free`;
page handles stay valid until `pdf_doc_free`. Freeing the same non-NULL
handle twice is undefined behavior (plain C, by design); NULL is always
refused with `PDF_ERR_INVALID_HANDLE`.

## Choosing a backend at runtime

```c
#include <pdflib/pdf_api.h>

pdflib_set_backend(PDF_BACKEND_PODOFO);   /* or LIBHARU / DUMMY / AUTO */

pdf_doc doc;
pdf_doc_create(&doc);
pdf_page page;
pdf_page_add(doc, PDF_PAGE_A4, PDF_ORIENTATION_PORTRAIT, &page);
...
pdf_doc_save_to_file(doc, "out.pdf");
pdf_doc_free(doc);
```

- `PDF_BACKEND_AUTO` (the default) prefers PoDoFo, then libharu,
  then dummy.
- Documents capture the active backend at creation time; switching the
  backend later does not affect already-created documents.

## Driver facade (pdflibdrv.dll)

A user-mode print driver should not link the core directly: the core
exposes every `pdf_*` entry point and hands out raw heap pointers. The
`driver/` directory contains a thin C-ABI facade instead -
`pdflibdrv.h` is the only header a driver includes, and
`pdflibdrv.def` fixes the exported names. Build it with
`-DPDFLIB_BUILD_DRIVER=ON` (off by default). Three scripts:

- `build-driver.cmd` - x64, prebuilt /MD backends (fast, dev only);
- `build-mt.cmd` - x64 /MT, rebuilds the whole dependency chain with
  `compiler.runtime=static` so the DLL imports no CRT DLL at all;
- `build-x86-xp.cmd` - 32-bit with the `v141_xp` toolset, producing an
  XP-targeted image (`subsystem 5.01`, 14 undecorated exports) and
  auditing its import table for post-XP APIs.

See [docs/pdflibdrv-abi.md](docs/pdflibdrv-abi.md) for the verified
contract and the remaining gaps.

## Adding a new backend

`pdflib` is a multi-backend core, not a libharu-only library. libharu and
PoDoFo are simply the two backends compiled in today; the native `pdf_*`
API dispatches through `pdf_backend_vtable`, so a third engine plugs in
below the API. The `HPDF_*` facade in `compat/` is another consumer of
that same core: it does **not** know which engine is active, so a new
backend automatically becomes available to both APIs.

1. Create `pdflib/backends/pdf_backend_<name>.c`.
2. Implement the functions of `pdf_backend_vtable` you support and
   assemble the table; unimplemented slots stay `NULL` and the core
   maps them to `PDF_ERR_BACKEND_UNSUPPORTED`.
3. Add a `pdf_backend_<name>_register()` that calls
   `pdf_backend_register(PDF_BACKEND_<NAME>, &vtable)`.
4. Add the enum value to `pdf_backend` in `pdflib/pdf_api.h` (append it
   before `PDF_BACKEND_MAX`, which currently gives eight slots), hook it
   into `pdf_init.c` and CMake, and (optionally) add it to
   `src/pdflib.def`/`apitest.c` only if you want a new exported helper.
5. The `HPDF_*` facade needs no change: `HPDF_SetBackend((HPDF_UINT)id)`
   and `pdf_doc_create()` resolve it through the same registry.

Two known limits for future engines:

- `pdf_backend` is a fixed enum and `PDF_BACKEND_MAX` is 8. A name-based
  selector (`pdflib_set_backend_by_name` / `HPDF_SetBackendByName`) is the
  recommended next API addition so a backend can be added without an ABI
  visible enum value; until then, use the enum value.
- `HPDF_SetBackend()` accepts any **registered** backend id, so a backend
  appended under the same enum becomes selectable without editing the
  compatibility layer. Only `HPDF_GetBackendName()` has a small switch for
  friendly names; unknown ids report `"unknown"`. A name-based selector is
  the planned API so even that switch disappears.

Nothing else in the tree changes — the public API, the core, the tests
and the whole `HPDF_*` surface stay engine-agnostic.

## Tests

Three suites are wired into CTest (`pdflib_tests`,
`pdflib_thread_tests`, `pdflib_complex_tests`):

- `tests/test_pdflib.c` — backend-independent unit tests; they run
  against the dummy backend, so they build and run with no third-party
  dependencies (`build-core.cmd`).
- `tests/test_threads.c` — concurrency: many threads each building and
  serializing documents on every registered backend, plus the global
  backend-selector race. Randomized; run it in a loop (or under ASan)
  for stronger guarantees.
- `tests/test_complex.c` — **complex automated documents**: multi-page
  rich content (standard + embedded TTF fonts, wrapped and aligned
  text, shapes/arcs/polygons, opacity/blend modes, dashes, transforms,
  a real PNG image, links, outlines, document info, rotation,
  encryption, streamed mode) with **byte-level output validation**:
  header/`%%EOF`, `xref`, `/Type /Page` counts, `MediaBox` dimensions
  per page size, `/Type /Font`, `/FlateDecode`, `/ExtGState`,
  `/Producer`, `/Encrypt` + `/Filter /Standard` + `/O`/`/U`.

The complex suite runs first on the dummy backend (core plumbing only,
content byte checks skipped) and then on every registered real engine.
It takes an optional backend filter for bisecting:

```
./build/all-backends/pdflib_complex_tests.exe podofo
./build/all-backends/pdflib_complex_tests.exe libharu
```

Byte-level assertions adapt to each engine's documented output shape:

- PoDoFo 0.9.7 writes GoTo links as `/Dest` arrays (no `/S /GoTo`
  action) and omits the optional `/Type /Outline` entry; both forms are
  accepted. The adapter also works around an upstream defect where the
  `PdfPage` wrapper returned by `CreatePage` becomes dangling once
  another page is created — the backend resolves pages by index through
  `GetPage()` instead.
- libharu streams through a callback-backed `HPDF_Stream` rather than an
  incremental writer, so its bytes reach the sink at `pdf_doc_finish`
  instead of per page. It requires the encrypt-dict to be created via
  `HPDF_SetPassword` before
  mode/permission calls (the adapter orders them correctly), and has no
  public API to write a trailer `/ID` — the suite notes this upstream
  limitation instead of failing.

## Design notes

- **Pure C core.** `pdf_core.c` has no engine includes; it resolves
  every public call through the vtable and implements composite
  operations (word wrapping, ellipse/arc beziers, polygons) once,
  on top of the primitive SPI, so every backend gets them for free.
- **Handle cells.** Public handles are small core-owned structs
  (`pdf_internal.h`) that pair a vtable pointer with a backend-private
  payload; backends own their payloads and stay opaque.
- **PoDoFo painter lifecycle.** PoDoFo draws through a `PdfPainter`
  attached to a canvas. The adapter keeps one painter per document,
  re-attaches it when drawing switches pages, and flushes it before
  saves — so the plain-C API stays stateless from the caller's view.

## Layout

```
pdflib/                 core + public API
  pdf_api.h             the only header applications include
  pdf_backend.h         backend SPI (vtable) - internal
  pdf_core.c/.h         registry, dispatch, composite drawing
  pdf_init.c            lazy backend registration
  pdf_internal.h        handle cells
  backends/
    pdf_backend_dummy.c     always built; reference implementation
    pdf_backend_libharu.c   libharu adapter
    pdf_backend_podofo.cpp  PoDoFo adapter
examples/hello_pdf.c    minimal end-to-end example
tests/test_pdflib.c     backend-independent unit tests
compat/                 libharu-compatible HPDF_* facade
  include/hpdf.h        libharu 2.4.4 public API (source compatible)
  hpdf_compat.c         HPDF_* -> pdf_* implementation
  tests/                self-check + unmodified upstream libharu demos
standalone/smoke.c      self-checking ABI + feature test for the DLL
standalone/apitest.c    calls every exported entry point (both engines)
standalone/repro-save.c minimal regression repro for the save-path crash
tools/stress_streamed.c streamed-mode batch stress + memory tool
driver/pdflibdrv.*      WDK-facing C ABI facade (opt-in)
driver/drv_harness.c    ABI contract harness (loads the DLL by name)
driver/c_link_check.c   plain-C consumer linked against the import lib
build-driver.cmd        driver facade build (/MD, prebuilt backends)
build-mt.cmd            driver facade build (/MT, static-runtime backends)
build-x86-xp.cmd        driver facade build (x86 + Windows XP, v141_xp)
profiles/                conan profiles (x64 static, x86 + XP static)
tools/check_xp_imports.py  import-table audit for Windows XP
tools/check_api_coverage.py  asserts apitest.c references every export in pdflib.def
tools/run-all-tests.sh   one command: build + export check + ctest, x64 and x86
tools/xp_smoke_test.c   self-checking ABI consumer for a real XP machine
docs/xp-validation.md   how to run that check inside an XP VM
```

## Memory characteristics of streamed mode

Streamed mode keeps memory O(1) per page: content streams are
serialized (and Flate-compressed) as each page completes and are never
buffered as a whole. `tools/stress_streamed.c` verifies this on every
run:

```
build/all-backends/stress_streamed.exe 0 podofo   # verification suite
build/all-backends/stress_streamed.exe 5000 podofo # single big run
```

The suite measures the within-run RSS growth between page checkpoints
for light and heavy per-page content. Content buffering would make the
heavy slope scale with the content volume; a passing run shows the
heavy slope within ~1.3x of the light one:

```
  growth: light=5.63 KB/page  heavy=7.45 KB/page (page 250 -> 1000)
  PASS: memory is O(1) per page and independent of content volume
```

A few KB/page of constant overhead is expected with the PoDoFo engine:
its page-tree cache retains one `PdfPage` wrapper per page
(`PdfPagesTreeCache`), and the page dictionaries themselves stay in
the document until finish. Backends without streamed support are
reported as `SKIP` by the tool (libharu documents are built in memory
and saved once at the end).
