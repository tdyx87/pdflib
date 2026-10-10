# Validating pdflibdrv on a real Windows XP machine

Everything else in this repository runs on a modern Windows: the build
targets XP, [tools/check_xp_imports.py](../tools/check_xp_imports.py)
audits the import table, and `ctest` exercises the ABI - but under
Windows Server 2022's WOW64. None of that proves the DLL *loads and works
on XP*. [tools/xp_smoke_test.c](../tools/xp_smoke_test.c) is the missing
half: a self-contained program that walks the entire ABI on the XP box
itself and prints a PASS/FAIL report.

## What you need

Three files, in one folder (a shared folder or a copy both work):

| File | Why |
|---|---|
| `pdflibdrv.dll` | the 32-bit XP build (`build/xp-cmake/Release/`) |
| `pdflibdrv_xp_smoke.exe` | the test program; loads the DLL with `LoadLibraryA` |
| `pdflibdrv_xp_smoke_linked.exe` | the same test, linked against the import library |

The two executables run exactly the same checks in the same order. They
differ in one thing: the second one lists `pdflibdrv.dll` in its own
import table, so the loader maps the DLL *before* `main()` runs, while the
first one loads it at run time. On Windows XP those are not equivalent -
see "When it crashes" below - and running both is what tells the two
failure modes apart.

Both come out of the `x86-xp` CI job as the **`xp-validation-bundle`**
artifact, together with this document. A local
[build-x86-xp.cmd](../build-x86-xp.cmd) or
`ci/build-windows.ps1 -Variant x86xp` produces them in
`build/xp-cmake/Release/` too.

The DLL must be the **x86 / XP** one. `pdflibdrv-x64-md` and
`pdflibdrv-x64-mt` are 64-bit and will not load on a 32-bit XP at all.

## Running it

Copy the folder to the XP machine (or map a share), then in `cmd.exe`:

```bat
cd \where\you\copied\them
pdflibdrv_xp_smoke.exe         > report-a.txt
pdflibdrv_xp_smoke_linked.exe  > report-b.txt
```

Redirecting each report to a file makes them easy to carry back. The
`LoadLibraryA` one finds `pdflibdrv.dll` next to itself; to point it
somewhere else, pass the DLL path or a directory:

```bat
pdflibdrv_xp_smoke.exe D:\driver\pdflibdrv.dll
pdflibdrv_xp_smoke.exe D:\driver
```

If the reports look truncated, the program is fine - output is unbuffered
precisely so a crash cannot swallow it, and every line is written as it
is produced.

No install, no registration, no reboot: the DLL is a plain user-mode
library, not a print driver. The program needs no arguments and writes
one file, `xp_smoke_out.pdf`, next to itself.

## What it checks

The header of the report tells you whether the run even counts:

```
OS            : 5.1 (build 2600) Service Pack 3
                -> this is Windows XP (the real target)
```

(On Windows 8.1 and later, `GetVersionExA` reports `6.2` unless the
binary carries a compatibility manifest, so `6.2` there means "not XP" -
the program says so explicitly.)

Then three independent groups:

1. **The PE image is aimed at XP.** Machine `i386`, subsystem version
   `5.01` - read from the loaded module's own headers, not from a
   build-time tool.
2. **The export surface is exactly right.** The 14 names in
   [driver/pdflibdrv.def](../driver/pdflibdrv.def) are enumerated from the
   export directory of the loaded image: nothing missing, nothing extra,
   nothing decorated as `name@N`. A count check catches a decorated name
   even if a lookup by the bare name would still succeed.
3. **The ABI behaves.** Startup is idempotent; a streamed job produces a
   complete PDF through the sink callback (`%PDF-` ... `%%EOF`, byte
   count matching what the sink received); pages draw and rotate; a sink
   that rejects data poisons the job (`PD_ERR_SINK`) and no further bytes
   are delivered; abort discards and invalidates the handle; NULL and
   forged handles return `PD_ERR_INVALID_HANDLE` instead of crashing the
   process; `Shutdown` releases unfinished jobs and `Startup` works again.

A good run ends like this (`36` checks for `pdflibdrv_xp_smoke.exe`, `37`
for the linked one, which adds an export-surface check):

```
load mode     : LoadLibraryA + GetProcAddress
...
RESULT: 36 check(s), 0 failure(s)
pdflibdrv works on this machine.
```

Exit code 0 means every check passed, 1 means at least one failed. Each
line is self-contained, so the whole output can be pasted into a report.

## When it crashes

The program installs an unhandled-exception filter and prints output
unbuffered, so a fault leaves a report like this instead of a bare dialog:

```
CRASH
-----
exception code : 0xc0000005 at 10024a1b -> inside pdflibdrv.dll +0x24a1b
access violation: WRITING address 0x7ffdf000
last call into the DLL: PdflibDrvPageDrawText
dll base: 10000000 (1761280 bytes), exe base: 00400000 (139264 bytes)
```

That block names the faulting module, the offset inside it, whether it was
a read or a write, the bad address, and the entry point that was executing.
The `--crash-test` flag deliberately faults in the test program itself, so
the report format can be checked: `pdflibdrv_xp_smoke.exe --crash-test`.

Compare the two executables. **If `pdflibdrv_xp_smoke.exe` crashes but
`pdflibdrv_xp_smoke_linked.exe` passes, the DLL is not broken on XP - it
cannot be `LoadLibrary`ed there**, because it contains static thread-local
storage and this is a documented pre-Vista limitation:

> On Windows operating systems before Windows Vista, `__declspec(thread)`
> has some limitations. If a DLL declares any data or object as
> `__declspec(thread)`, it can cause a protection fault if dynamically
> loaded. After the DLL is loaded with `LoadLibrary`, it causes system
> failure whenever the code references the `__declspec(thread)` data. [...]
> Use the TLS APIs, such as `TlsAlloc`, in your DLL to allocate TLS if the
> DLL might be loaded with `LoadLibrary`.
> - [Thread Local Storage (TLS), Microsoft Learn](https://learn.microsoft.com/en-us/cpp/parallel/thread-local-storage-tls)

`pdflibdrv.dll` does contain such variables: PoDoFo keeps its FreeType
initializer and a recursion counter in `thread_local` storage. The fault
lands on the first call that touches them, which is drawing text. Keep the
linked executable (a print filter links the import library too, so it maps
the DLL the same way and is unaffected), or eliminate the static TLS in
the DLL - that is a change to make here, not something to work around in
the VM.

If **both** crash, the DLL has a genuine XP problem and the crash report is
the place to start.

## Reading the result

* **open `xp_smoke_out.pdf`** in whatever the machine has (Acrobat 5/7,
  Foxit, SumatraPDF). That is the end-to-end proof: the file is real,
  generated on XP, and any viewer can open it.
* A failure is worth keeping: it names the check, and the same program
  runs on a modern machine (`ctest -R pdflibdrv_xp_smoke`) to tell a
  target-specific problem apart from a plain bug.
* Known limits, so a pass is not over-read: the ABI is exercised, not the
  print pipeline around it; a 32-bit XP is assumed (on 64-bit XP the DLL
  runs under WOW64); and according to Microsoft's documentation the
  x86 build requires **XP with SP3** - earlier service packs are untested.
