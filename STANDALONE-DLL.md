# pdflib.dll — a single-DLL PDF generator for any language

`pdflib.dll` packages **two independent PDF engines — libharu and PoDoFo — behind one
flat C API**, and exposes that API with the style and completeness of libharu's own
`HPDF_*` interface. Any language that can call a C DLL can generate PDFs with it:
Python, .NET, Java, Go, Rust, Node.js, Delphi, VB6/VBA, PowerBuilder, C/C++ itself.

```
+---------------------- your language -----------------------+
|  ctypes / P-Invoke / JNA / cgo / FFI / Declare Function    |
+---------------------------|--------------------------------+
                            |  pdf_*  (plain __cdecl C ABI,
                            |           undecorated names)
+---------------------------v--------------------------------+
|                       pdflib.dll                           |
|  +---------------------- core (pure C) ------------------+ |
|  |  document / page / font / image / annot / outline     | |
|  |  graphics state, paths, text, streaming, security     | |
|  +-----+------------------------------------------+------+ |
|        | vtable                                    |        |
|  +-----v----------+                     +-----------v-----+  |
|  | libharu (C)    |                     | PoDoFo (C++)    |  |
|  | static .lib    |                     | static .lib     |  |
|  +----------------+                     +-----------------+  |
+-------------------------------------------------------------+
```

## Why a DLL and not the source

- **One file to ship.** Both engines are linked *statically* into `pdflib.dll`, built
  with the **static CRT (/MT)**. The DLL has **no runtime dependency** — no VC++
  redistributable, no `hpdf.dll`, no `podofo.dll`. Copy `pdflib.dll` next to your app
  and it runs (Windows 7 and later, x64 or x86).
- **One calling convention, no name mangling.** Exports are plain `__cdecl` with
  **undecorated names on both architectures** (guaranteed by `src/pdflib.def`, not by
  `__declspec`). `GetProcAddress("pdf_doc_create")` works identically on 32- and
  64-bit, so FFI bindings are trivial and portable.

## Complete feature set

| Area | What you get |
|------|--------------|
| Documents | create in memory, save to file / memory buffer / **stream callback** / **OS handle** |
| **Streaming** | **unknown page count**, memory bounded to ~one page (`pdf_doc_create_streamed`) |
| Pages | 7 standard sizes + custom sizes, rotation, page count |
| Fonts | 14 standard fonts, TrueType from **file / memory / system**, **`.ttc` collections unwrapped automatically**, subset embedding, text metrics (width / ascent / descent) |
| Text | single line, aligned, and word-wrapped paragraphs |
| Graphics state | save/restore, transforms, line width/cap/join/miter, dashes, opacity, blend modes, text render modes |
| Colours | gray, RGB, CMYK, per-colour alpha |
| Paths | move/line/cubic bezier/close/rect/circle/ellipse/arc/polygon + 5 paint modes (fill, stroke, both, even-odd variants) |
| Images | PNG and JPEG, from file or memory, scaled drawing |
| Annotations | text, highlight, underline, squiggly, strikeout, square, circle, URI links, internal (Goto) links, colour/icon/opacity/author/subject |
| Outlines | bookmark tree with hierarchy and destinations |
| Metadata | title, author, subject, keywords, creator, producer |
| Security | RC4-40/128, AES-128/256 (engine permitting) with fine-grained permissions |
| Compression | content / image / metadata stream compression |
Where an engine cannot do something, the call returns
`PDF_ERR_BACKEND_UNSUPPORTED` (4) instead of failing silently.

## Fault containment

This DLL is meant to be called from Python, Java, Delphi, VBA, .NET — none of which
have a way to catch an exception that unwinds into them. So the boundary is sealed:

- Every PoDoFo entry point catches **std::exception** *and* has a `catch (...)`
  safety net, and the adapter is compiled with **`/EHa`** so hardware faults
  (access violations) inside the engine are caught too. Anything thrown is converted
  into `PDF_ERR_BACKEND_FAILED`; the host process survives and never dies with a
  crash dialog.
- Handles are validated (`PDF_ERR_INVALID_HANDLE`) and out-parameters checked
  (`PDF_ERR_INVALID_PARAM`) before any pointer is dereferenced.
- The smoke test drives deliberately bad inputs (NULL handles, zero-length or
  garbage font buffers, negative page sizes, unknown backends) and asserts that
  every one returns a status code. `standalone/apitest.c` does the same for all
  84 exports, plus ~85 NULL/bad-argument cases, **and then reads every artefact
  it produced back to assert on its content** — see
  [Testing what was actually written](#testing-what-was-actually-written). It is
  the reason three adapter faults were found and fixed — libharu faulting on
  `/Name` and `/Open` set on a non-text annotation, a deferred GOTO link reaching
  libharu's unguarded markup helpers with a NULL annotation, and a refused font
  load latching libharu's error and silently disabling the whole document.

### Why the API has no `FILE*`

Every "already open target" call takes an **OS handle** — a Win32 `HANDLE`, or a
file descriptor on POSIX — rather than a `FILE*`:

```c
pdf_doc_save_to_handle(doc, (void *)GetStdHandle(STD_OUTPUT_HANDLE));
pdf_doc_create_streamed_to_handle((void *)hFile, &doc);   /* caller keeps ownership */
```

The DLL links the CRT **statically**, so it owns a private `FILE` table. A
`FILE*` the caller opened belongs to the *caller's* CRT, and the DLL's own
`fwrite`/`fflush` fail that stream's validation. In the static CRT that failure
does not return an error — it routes to `_invoke_watson` and then
`__fastfail(FAST_FAIL_INVALID_ARG)`, which kills the process immediately and
cannot be caught: not by `__try/__except`, not by `catch (...)`, not by the
`/EHa` net above. It is a fail-fast by design.

An OS handle has no such problem. `HANDLE`s and file descriptors are
process-wide kernel objects, and `WriteFile`/`write` are plain kernel calls that
do not care which CRT opened them. The caller keeps ownership in every case:
pdflib writes to the handle and never closes it. `pdf_doc_create_streamed_to_file`
is the one exception — there the DLL opened the file itself, so it closes it.

Writing to a path (`pdf_doc_save_to_file`, `pdf_doc_create_streamed_to_file`) and
writing through your own callback (`pdf_doc_save_to_stream`) are unaffected:
in the first case the DLL opens the file, in the second the callback runs inside
*your* module, where your CRT owns the `FILE*`.

## Testing what was actually written

A status code only says the library **accepted** a call. A function that returns
`PDF_OK` and draws nothing passes every status-code test ever written, so
`standalone/apitest.c` also opens each artefact it produces and asserts on its
**content**:

```
pdflib_apitest all                  # full suite, both engines, content verified
pdflib_apitest --inspect file.pdf   # just report what is really inside a file
```

The reader is `standalone/pdf_inspect.h` (header-only, no dependency beyond
zlib): an xref-independent object scan, `/Length` handling that copes with the
indirect form PoDoFo's streamed writer emits, FlateDecode with a raw-deflate
fallback and PNG-predictor undo, dictionary access that walks key/value **pairs**,
and a content-stream lexer that skips strings/dicts/arrays so an operator name
inside a text string is not counted. `--inspect` prints the header, page tree
with real-valued `/MediaBox` and `/Rotate`, an operator histogram per page, font
objects with their embedding markers, image XObjects with encoded/decoded sizes,
annotation subtypes, the outline tree and the info dictionary.

The assertions it enables are on the artefact, not the API: page count and exact
page sizes, `k`/`rg`/`RG`/`g` for the colour spaces, every paint operator
(`f`, `B`, `f*`, `B*`, `S`), `cm`/`d`/`Tr`, `q`/`Q` **balance**, the drawn text
present in the decoded stream, a real image XObject with a matching `Do`, the
link's `/A` with its `/URI` (decoding libharu's octal escapes and UTF-16BE
strings), the outline tree, `/Info` round-tripped from the bytes, an embedded
font program for a TTF and *no* `/FontDescriptor` for a standard font, and
`/Encrypt` present or absent as requested. Every stream in every artefact must
inflate — that is the one check that catches a mis-sized `/Length`, because the
xref and the page tree still look perfect when a stream is truncated.

It also counts how many pages carry non-empty content and requires exactly that
many to. This is the direct test for the failure a status-code suite cannot see:
a call that returns `PDF_OK` and draws nothing still leaves a valid page tree, a
valid `/MediaBox` and a decodable empty stream, so nothing else would notice.
The expectation is a *count* rather than "every page", because a page carrying
only annotations is legitimately blank and the two engines write that page
differently (see the table below).

Writing these found seven reader bugs and three real library defects: a libharu
document poisoned by a single refused font load, a filter written as
`/Filter [ /FlateDecode ]` that a name-only reader treats as "uncompressed", a
zero-length stream that a naive reader reports as corrupt instead of empty, a
wrapped paragraph that was painted two or three times over itself — found only
by counting, not by looking — and a PoDoFo text-rendering mode that could not be
restored, which made the rest of a page vanish (see below).

### A wrapped paragraph painted more than once

`pdf_page_draw_text_wrapped` lays out a paragraph one line at a time and stops
when the next baseline would fall past the bottom of the box. The wrap loop
advanced its own cursor (`seg`) and counted the segment length (`seg_len`) down
to zero as it consumed the text — but the line that resumed the outer loop
recomputed its position as `p + seg_len`, and `seg_len` was zero by then. `p`
never moved, so the same paragraph was re-wrapped from the top and re-drawn
until `cursor_y` finally ran past the box: a three-line subtitle in a 140pt box
was painted three times over itself, and a two-line run in a 120pt box twice.

It is worth being precise about why nothing caught this earlier:

* Every call returned `PDF_OK` and the file was structurally perfect — correct
  page tree, correct fonts, streams that inflate.
* A "does the text appear?" check is **blind to duplication**: the answer is
  yes whether the run was drawn once or five times. Both the API test and the
  showcase asserted exactly that, and both passed.
* The independent render check ("does the page have more than a handful of
  distinct colours?") also passed — the page was not empty, just overprinted.

What catches it is a **count**. `pdfi_count_text()` in
`standalone/pdf_inspect.h` counts occurrences of a run in the decoded content
stream (literal or hex-encoded, the larger of the two, since an engine writes a
given run in exactly one form). `rb_check_text_count()` / `rb_text_count()`
then assert *exactly one*. Both engines report the correct count now, and with
the fix reverted the check fails 3 times per engine — so the assertion is not
vacuous.

The lesson generalises: for a generator, "the right thing is in the output" is
strictly weaker than "the right thing is in the output *once*". Where a run of
drawing is supposed to happen a known number of times, assert the number.

### PoDoFo cannot restore the text rendering mode

`/Tr` is text state: it is part of the graphics state, it survives from one
text object to the next, and only `Q` (or a new operator) takes it back. That
makes "set it to *invisible*, then set it back to *fill*" a correctness-critical
operation — and on PoDoFo it did not work.

`PdfPainter` gets it wrong twice over:

* `PdfPainter::SetTextRenderingMode()` records the mode but writes the operator
  only `if (m_isTextOpen)` — and even then it merely stages it into an internal
  buffer (`SetCurrentTextRenderingMode()` appends to `m_oss`) that nothing ever
  flushes. Called between text objects, which is how a page-level API is used,
  it is a complete no-op.
* `PdfPainter::DrawText()` emits the operator only when the mode is **not**
  fill, on the theory that fill is "the PDF default, so nothing to emit". That
  is true for the first text object of a content stream and false for every
  one after it.

Together: `pdf_set_text_render_mode(p, PDF_TEXT_RENDER_STROKE)` followed by
`PDF_TEXT_RENDER_FILL` leaves the rest of the page stroked; the same with
`PDF_TEXT_RENDER_INVISIBLE` makes every following word disappear. A stroked
heading or an invisible watermark silently destroys the page after it, with
every call still returning `PDF_OK`. libharu writes the operator correctly, so
the two engines disagreed and only the PoDoFo output was wrong.

The adapter no longer uses the painter's bookkeeping at all. It tracks the mode
itself and appends `"<n> Tr"` **directly to the page's content stream**
(`PdfPage::GetContentsForAppending()->GetStream()->Append(...)`) immediately
before each text object, leaving the painter's own mode at fill for the whole
document so it never emits a conflicting operator. The result matches libharu's
operator sequence exactly.

The regression test asserts the **operand of the last `/Tr` on the page**:
`pdfi_last_op_operand(c, n, "Tr")` must be `0`. Counting `Tr` operators cannot
see the bug — the operator is present either way — but the last operand is
exactly the question "did this page finish in fill mode?", and everything drawn
after a stray `3 Tr` is invisible. Before the fix the check reports `Tr 3` on
PoDoFo and `Tr 0` on libharu; after it, both report `Tr 0`.

### libharu latches its first error

libharu records the first error on the document, and while it is set
`HPDF_CheckError()` turns every later call into a **silent no-op that still
reports success**. A function that returns NULL without clearing it therefore
does not report one failure — it disables the whole document. Loading a `.ttc`
(which libharu cannot parse) used to do exactly that: every later `pdf_page_add`
and draw did nothing while returning `PDF_OK`, so the document came out empty.
PoDoFo has no such latch and recovered normally.

The adapter now routes every libharu failure through one helper that clears the
latch, so a failed call reports a status and leaves the document usable — which
is the contract the rest of pdflib assumes. `test_readback` covers it: it loads
a `.ttc`, accepts a clean refusal, and then adds a page to prove the document
still works.

## Known engine limits

Both engines work, but they do not agree on everything. These are the edges worth
knowing before you pick a backend:

| Situation | PoDoFo | libharu |
|-------|--------|---------|
| `pdf_doc_create_streamed` | native, O(1) per page | `PDF_ERR_BACKEND_UNSUPPORTED` |
| `.ttc` collection, embedded | loads (the core unwraps face 0); drawing a subsetted collection face can fail with `PDF_ERR_BACKEND_FAILED` | rejects with `PDF_ERR_BACKEND_FAILED` |
| `.ttc` collection, not embedded | works | rejects |
| a page with nothing drawn on it | omits `/Contents` altogether | writes `/Length 0` + `/Filter [ /FlateDecode ]` |

That last row is not a defect in either engine — both files are valid — but it
trips up anything that reads a PDF back. A page whose only content is an
annotation is *legitimately* blank, and it is represented two different ways.
Any reader must treat a zero-length stream as decoding to zero bytes rather than
as a decode failure: `inflate()` refuses zero input bytes, so a naive reader
reports a blank page as corrupt. `standalone/pdf_inspect.h` distinguishes the
three cases — `/Contents` absent, `/Contents` present but empty, `/Contents`
present but un-inflatable — and `apitest` asserts on the distinction.

`.ttc` is what Windows ships for `simsun.ttc`, `msyh.ttc`, `mingliu.ttc` — several
faces in one file. PoDoFo can load such a file but subset-embedding a face lifted
out of a collection is broken upstream; calling it **does not crash** (that is what
the `catch (...)` + `/EHa` net is for), it returns
`PDF_ERR_BACKEND_FAILED` and `pdflib_last_error()` explains it. Prefer a
single-face `.ttf` copy when you need embedding on PoDoFo.

`pdf_annot_create(PDF_ANNOT_LINK_URI, ..., uri, ...)` materialises the link
immediately on **both** engines, so `pdf_annot_set_color` and friends work right
away. `PDF_ANNOT_LINK_GOTO` stays deferred until `pdf_annot_link_to_page` is
called, because the destination page is not known yet.

## Backends

Both engines are always present; pick one at runtime:

```c
pdflib_set_backend(PDF_BACKEND_PODOFO);   /* 3 */
pdflib_set_backend(PDF_BACKEND_LIBHARU);  /* 2 */
```

- **`PDF_BACKEND_AUTO` (0)** — prefers PoDoFo, then libharu.
- **PoDoFo** implements **native streaming** — the recommended engine for
  print-driver style work with unbounded page counts.
- **libharu** always embeds TrueType fonts and has no streamed writer (it reports
  `PDF_ERR_BACKEND_UNSUPPORTED` for `pdf_doc_create_streamed`), but it is the more
  battle-tested renderer for ordinary text/graphics.

Note the five names that are prefixed `pdflib_` rather than `pdf_` — they sit beside
the backend table itself and were named this way upstream:
`pdflib_version`, `pdflib_backend_version`, `pdflib_set_backend`,
`pdflib_get_backend`, `pdflib_has_backend`, `pdflib_last_error`.

## Building

```
build-deps.cmd x64        :: build static libharu + PoDoFo into build\x64-deps
build-pdflib-dll.cmd x64  :: -> dist\x64\pdflib.dll (+ pdflib.lib, include\pdflib.h)
run-smoke.cmd             :: build + run the self-checking consumer test

build-deps.cmd x86
build-pdflib-dll.cmd x86  :: -> dist\x86\pdflib.dll
```

From a bash shell, one command builds and verifies everything — both
architectures, the DLL, the export table, the API test and the examples:

```
tools/run-all-tests.sh              # x64 and x86
tools/run-all-tests.sh x64          # one architecture
tools/run-all-tests.sh x86 --clean  # wipe build/<arch> first
```

It runs, per architecture: configure → build every target → install into
`dist/<arch>` → `check_exports.py` against `src/pdflib.def` →
`check_api_coverage.py` against `standalone/apitest.c` → `ctest`. It exits
non-zero if any step fails, and writes each run to `build/logs/run-<arch>.log`.

The equivalent CMD entry points are `verify-exports.cmd build/x64/pdflib.dll`
and `run-examples.cmd`; `tools/run-all-tests.sh` simply chains all of them and
adds the coverage check.

`verify-exports.cmd build/x64/pdflib.dll` asserts that the export table matches
`src/pdflib.def` exactly (84 names) and that no name is decorated — a decorated name
like `_pdf_doc_create@4` would break every `GetProcAddress` caller. Run it for **both**
architectures.

The build uses Conan only to produce the two engine libraries; the DLL itself is a
plain CMake/Ninja build (`standalone/CMakeLists.txt`). `pdflib-build-env.cmd` sets up
the MSVC environment by hand because `vcvarsall.bat` depends on `reg.exe`, which may
be unavailable in restricted environments.

> **Do not copy `rc.exe` / `mt.exe` into `.tools`.** The SDK's copies are launchers
> that stop working the moment they leave their own directory — they exit with
> `0xC0000135` (`STATUS_DLL_NOT_FOUND`), which surfaces as `LNK1327: rc.exe 运行失败`
> on any link that needs a manifest. `.tools` must hold only `cl.exe` and `link.exe`;
> the resource tools are resolved through `%SDKBIN%` on `PATH`.

## Using it

### Python (ctypes)

```python
import ctypes
pdf = ctypes.CDLL("pdflib.dll")

doc = ctypes.c_void_p()
pdf.pdf_doc_create(ctypes.byref(doc))
page = ctypes.c_void_p()
pdf.pdf_page_add(doc, 0, 0, ctypes.byref(page))       # A4 portrait
font = ctypes.c_void_p()
pdf.pdf_font_get_std(doc, 4, ctypes.byref(font))       # Helvetica
pdf.pdf_page_draw_text(page, font, b"Hello", 72, 700, 18)
pdf.pdf_doc_save_to_file(doc, b"out.pdf")
pdf.pdf_doc_free(doc)
```

See `examples/python/pdflib_demo.py` for the full version **including a real
streaming example** that pages a 150-page document through a Python callback.

### C#

```csharp
[DllImport("pdflib.dll", CallingConvention = CallingConvention.Cdecl)]
static extern int pdf_doc_create(out IntPtr doc);
```

### Go

```go
// #cgo LDFLAGS: -L. -lpdflib
// int pdf_doc_create(void **out);
import "C"
```

### Delphi / VBA

```pascal
function pdf_doc_create(out doc: Pointer): Integer; cdecl;
  external 'pdflib.dll';
```

```vb
Declare Function pdf_doc_create Lib "pdflib.dll" (ByRef doc As Long) As Long
```

## Streaming in detail

The reason to use this DLL for driver-style work is that the page count is not known
until the source commands stop arriving.

```c
static pdf_status sink(const unsigned char *data, size_t len, void *ctx)
{
    /* append `len` bytes to your file / pipe / socket */
    return PDF_OK;            /* non-zero aborts and poisons the doc */
}

pdf_doc doc;
pdf_doc_create_streamed(sink, my_ctx, &doc);   /* sink bound up front */

for (;;) {                                     /* however many sheets arrive */
    pdf_page page;
    pdf_page_add_custom(doc, width, height, &page);
    ... draw this sheet ...
}                                              /* page bytes already sent */

pdf_doc_finish(doc);    /* embed font subsets + write xref/trailer */
pdf_doc_free(doc);
```

Contract:

- Peak memory stays **O(1) per page** — content is flushed as each page completes, not
  buffered for the whole document.
- If the sink fails (disk full, peer gone), its error propagates verbatim and the
  document is **poisoned**: every later call fails fast with the same code and no
  further bytes are delivered. A half-written target file is removed.
- On a streamed document the `pdf_doc_save_to_*` calls return
  `PDF_ERR_INVALID_STATE` — output is already flowing.

If the engine cannot stream (libharu), `pdf_doc_create_streamed` returns
`PDF_ERR_BACKEND_UNSUPPORTED` so the caller can fall back to
`pdf_doc_create` + `pdf_doc_save_to_stream`.

## Backends & status codes

| Code | Value | Meaning |
|------|-------|---------|
| `PDF_OK` | 0 | success |
| `PDF_ERR_UNKNOWN` | 1 | unspecified |
| `PDF_ERR_INVALID_HANDLE` | 2 | NULL or foreign handle |
| `PDF_ERR_INVALID_PARAM` | 3 | bad argument |
| `PDF_ERR_BACKEND_UNSUPPORTED` | 4 | engine lacks the feature |
| `PDF_ERR_BACKEND_FAILED` | 5 | engine error |
| `PDF_ERR_BACKEND_NOT_FOUND` | 6 | engine not compiled in |
| `PDF_ERR_OUT_OF_MEMORY` | 7 | allocation failed |
| `PDF_ERR_IO` | 8 | file read/write error |

`pdf_dll_status_string(code)` returns a human-readable string for any of these.

## Layout

```
pdflib/
  pdf_api.h              engine API (the exported surface)
  pdf_core.c             backend-agnostic core
  pdf_init.c             lazy backend registration
  backends/
    pdf_backend_libharu.c   libharu adapter
    pdf_backend_podofo.cpp  PoDoFo adapter
    pdf_backend_dummy.c     validation backend
include/pdflib.h         the SINGLE header a consumer includes
src/
  pdflib_export.c        pdf_dll_* helpers
  pdflib.def             authoritative export list (undecorated names)
standalone/
  CMakeLists.txt         builds the single DLL + its verification programs
  smoke.c                self-checking ABI + feature test
  apitest.c              calls every exported entry point (both engines)
  repro-save.c           minimal regression repro for the save-path crash
examples/python/         ctypes binding + streaming demo
tools/check_exports.py    export-table verifier (DLL vs src/pdflib.def)
tools/check_api_coverage.py  verifier (apitest.c vs src/pdflib.def)
tools/run-all-tests.sh    one command: build + verify + ctest, x64 and x86
pdflib-build-env.cmd     self-contained MSVC environment
build-deps.cmd           build static engines (conan)
build-pdflib-dll.cmd     build the DLL
verify-exports.cmd       assert the export table
```
