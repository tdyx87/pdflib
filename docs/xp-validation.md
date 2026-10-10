# Validating pdflibdrv on a real Windows XP machine

Everything else in this repository runs on a modern Windows: the build
targets XP, [tools/check_xp_imports.py](../tools/check_xp_imports.py)
audits the import table, and `ctest` exercises the ABI - but under
Windows Server 2022's WOW64. None of that proves the DLL *loads and works
on XP*. [tools/xp_smoke_test.c](../tools/xp_smoke_test.c) is the missing
half: a self-contained program that walks the entire ABI on the XP box
itself and prints a PASS/FAIL report.

## What you need

Two files, in one folder (a shared folder or a copy both work):

| File | Why |
|---|---|
| `pdflibdrv.dll` | the 32-bit XP build (`build/xp-cmake/Release/`) |
| `pdflibdrv_xp_smoke.exe` | the test program, built with the same toolset |

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
pdflibdrv_xp_smoke.exe
```

It finds `pdflibdrv.dll` next to itself. To point it somewhere else, pass
the DLL path or a directory:

```bat
pdflibdrv_xp_smoke.exe D:\driver\pdflibdrv.dll
pdflibdrv_xp_smoke.exe D:\driver
```

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

A good run ends like this:

```
RESULT: 36 check(s), 0 failure(s)
pdflibdrv works on this machine.
```

Exit code 0 means every check passed, 1 means at least one failed. Each
line is self-contained, so the whole output can be pasted into a report.

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
