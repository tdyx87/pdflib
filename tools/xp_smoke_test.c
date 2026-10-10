/*
 * xp_smoke_test.c - run pdflibdrv.dll through its whole ABI on a real
 * Windows XP box.
 *
 * The build, the import-table audit and the ctest suite all run on a
 * modern Windows; none of them is the same thing as the DLL having been
 * loaded by XP. This program is what closes that gap:
 *
 *   cd <folder>
 *   pdflibdrv_xp_smoke.exe            (finds pdflibdrv.dll next to itself)
 *   pdflibdrv_xp_smoke.exe C:\path\pdflibdrv.dll
 *   pdflibdrv_xp_smoke.exe C:\folder  (folder holding pdflibdrv.dll)
 *
 * It checks, on the XP machine itself, three independent things:
 *
 *   1. the PE image is aimed at XP - machine i386, subsystem version
 *      5.01 - read from the loaded module's own headers, not from a
 *      build-time tool;
 *   2. the export surface is exactly the 14 names of pdflibdrv.def, with
 *      no decoration and nothing extra - read from the export directory
 *      of the loaded image (a name like _PdflibDrvPageAdd@16 or a
 *      missing entry fails here);
 *   3. the ABI behaves - startup is idempotent, a streamed job produces a
 *      complete PDF through the sink, pages draw, a failing sink poisons
 *      the job and stops delivery, abort discards, forged and NULL
 *      handles are refused rather than dereferenced, and shutdown
 *      releases unfinished jobs.
 *
 * Exit code: 0 when every check passed, 1 otherwise. Every check prints
 * a line, so the output can be pasted into a bug report as-is.
 *
 * Only kernel32 entry points that XP has are used, the CRT is linked
 * statically (the XP build is /MT), and the DLL is resolved with
 * LoadLibraryA + GetProcAddress exactly like a print filter would - so
 * this executable itself loads on XP. tools/check_xp_imports.py audits
 * it the same way it audits the DLL.
 */

/* strcpy/strcat/fopen are flagged as "unsafe" by MSVC's CRT. They are
   the right tools here: every buffer involved is a fixed MAX_PATH-sized
   array and no size ever comes from outside the program, so the _s
   variants would only add noise. Define it before the CRT headers so a
   genuine warning in this file stands out. */
#define _CRT_SECURE_NO_WARNINGS

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <stdio.h>
#include <string.h>

#include "pdflibdrv.h"

/* ------------------------------------------------------------------ */
/* tiny check book-keeping                                            */
/* ------------------------------------------------------------------ */

static int g_checks = 0;
static int g_failures = 0;

/* status text comes from the DLL itself; set once resolved */
static const char *(PDCALL *g_status_string)(pd_status);

static const char *status_text(pd_status s)
{
    if (g_status_string) {
        const char *t = g_status_string(s);
        if (t && t[0])
            return t;
    }
    return "?";
}

static void check(int ok, const char *what)
{
    g_checks++;
    if (!ok)
        g_failures++;
    printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
}

/* GetVersionExA is deprecated in favour of the version helpers, but those
   answer according to the binary's manifest while this program wants the
   raw reported version - on XP, the only machine where the answer matters
   here, the two agree anyway. The warning is disabled around this one
   call so that a deprecation notice cannot hide a real one. */
static int query_os_version(OSVERSIONINFOA *osv)
{
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable: 4996)
#endif
    memset(osv, 0, sizeof(*osv));
    osv->dwOSVersionInfoSize = sizeof(*osv);
    return GetVersionExA(osv) ? 1 : 0;
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
}

/* ------------------------------------------------------------------ */
/* crash reporting                                                    */
/*                                                                    */
/* A crash on the machine under test is a result like any other: the    */
/* report has to say where it happened without a debugger. stdout is    */
/* unbuffered, every call into the DLL is trailed, and an unhandled-    */
/* exception filter names the module that faulted.                     */
/* ------------------------------------------------------------------ */

static const unsigned char *g_dll_base;
static unsigned long        g_dll_size;
static const unsigned char *g_exe_base;
static unsigned long        g_exe_size;
static const char          *g_last_call = "(nothing yet)";

static void describe_address(const void *addr)
{
    const unsigned char *p = (const unsigned char *)addr;

    if (g_dll_base && p >= g_dll_base && p < g_dll_base + g_dll_size) {
        printf("inside pdflibdrv.dll +0x%lx", (unsigned long)(p - g_dll_base));
        return;
    }
    if (g_exe_base && p >= g_exe_base && p < g_exe_base + g_exe_size) {
        printf("inside the test program +0x%lx", (unsigned long)(p - g_exe_base));
        return;
    }
    printf("outside both (a system DLL, or a wild pointer)");
}

static LONG WINAPI crash_report(EXCEPTION_POINTERS *ep)
{
    EXCEPTION_RECORD *rec = ep->ExceptionRecord;

    printf("\n");
    printf("CRASH\n");
    printf("-----\n");
    printf("exception code : 0x%08lx at %p -> ",
           (unsigned long)rec->ExceptionCode, rec->ExceptionAddress);
    describe_address(rec->ExceptionAddress);
    printf("\n");
    if (rec->ExceptionCode == 0xC0000005u && rec->NumberParameters >= 2) {
        const char *kind = rec->ExceptionInformation[0] == 1 ? "WRITING" :
                           rec->ExceptionInformation[0] == 8 ? "EXECUTING" :
                           "READING";
        printf("access violation: %s address 0x%08lx%s\n", kind,
               (unsigned long)rec->ExceptionInformation[1],
               rec->ExceptionInformation[1] == 0 ? "  (null pointer)" : "");
    }
    printf("last call into the DLL: %s\n", g_last_call);
    printf("dll base: %p (%lu bytes), exe base: %p (%lu bytes)\n",
           (const void *)g_dll_base, g_dll_size,
           (const void *)g_exe_base, g_exe_size);
    printf("Paste this whole block; it names the faulting call and module.\n");
    printf("\n");
    return EXCEPTION_EXECUTE_HANDLER;
}

/* The same header walk, for the executable, so a fault address can be
   attributed to the test program rather than to the DLL. */
static void record_exe_range(void)
{
    const unsigned char *base = (const unsigned char *)GetModuleHandleA(NULL);
    const IMAGE_DOS_HEADER *dos;
    const IMAGE_NT_HEADERS *nt;

    if (!base)
        return;
    dos = (const IMAGE_DOS_HEADER *)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE)
        return;
    nt = (const IMAGE_NT_HEADERS *)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE)
        return;
    g_exe_base = base;
    g_exe_size = nt->OptionalHeader.SizeOfImage;
}

/* Records the entry point that is about to be called, so a crash report
   can name it even when the failure is inside the DLL. */
static void trace_call(const char *what)
{
    g_last_call = what;
    printf("  ..  calling %s\n", what);
}

static void check_status(pd_status got, pd_status want, const char *what)
{
    g_checks++;
    if (got != want) {
        g_failures++;
        printf("  [FAIL] %s (got %u \"%s\", want %u \"%s\")\n",
               what, got, status_text(got), want, status_text(want));
    }
    else {
        printf("  [PASS] %s\n", what);
    }
}

/* ------------------------------------------------------------------ */
/* sink                                                               */
/* ------------------------------------------------------------------ */

typedef struct
{
    FILE         *fp;              /* NULL: count bytes, write nothing */
    unsigned long calls;
    unsigned long bytes;
    unsigned long zero_len_calls;
    int           fail_on_call;    /* >0: this call returns non-zero */
    int           failed;
    unsigned long bytes_after_failure;
} sink_t;

static int PDCALL sink_write(void *ctx, const unsigned char *data, size_t len)
{
    sink_t *s = (sink_t *)ctx;

    s->calls++;

    if (len == 0) {
        s->zero_len_calls++;
        return 0;
    }
    if (s->failed) {
        /* the DLL is required to stop delivering after a failure */
        s->bytes_after_failure += (unsigned long)len;
        return 0;
    }
    if (s->fail_on_call > 0 && s->calls == (unsigned long)s->fail_on_call) {
        s->failed = 1;
        return 1;
    }
    s->bytes += (unsigned long)len;
    if (s->fp) {
        if (fwrite(data, 1, len, s->fp) != len) {
            s->failed = 1;
            return 1;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* resolving the ABI through the DLL's export table                    */
/* ------------------------------------------------------------------ */

static const char *g_expected_exports[] = {
    "PdflibDrvGetAbiVersion",
    "PdflibDrvJobAbort",
    "PdflibDrvJobCreate",
    "PdflibDrvJobFinish",
    "PdflibDrvJobLastError",
    "PdflibDrvPageAdd",
    "PdflibDrvPageDrawLine",
    "PdflibDrvPageDrawRect",
    "PdflibDrvPageDrawText",
    "PdflibDrvPageSetRotation",
    "PdflibDrvShutdown",
    "PdflibDrvStartup",
    "PdflibDrvStatusString",
    "PdflibDrvVersionString"
};

#define EXPECTED_EXPORT_COUNT 14

/*
 * Two ways to reach the ABI, chosen at build time.
 *
 * Default: LoadLibraryA + GetProcAddress - what a filter that loads the
 * facade by name does, and what the ABI header explicitly allows.
 *
 * PDFLIBDRV_SMOKE_LINK_DIRECT: call the entry points the .def-based
 * import library declares, so the DLL sits in this program's import table
 * and is loaded by the loader before main() runs. On Windows XP those two
 * are NOT equivalent: a DLL loaded with LoadLibrary cannot safely use
 * static TLS (__declspec(thread)) there, and this DLL contains such
 * variables - PoDoFo's FreeType init and its recursion counter. Building
 * both executables separates "the DLL is broken here" from "the DLL
 * cannot be LoadLibrary'd here".
 */
#ifdef PDFLIBDRV_SMOKE_LINK_DIRECT
#define RESOLVE(name) ((FARPROC)(&name))
#define SMOKE_LOAD_MODE "linked import library - DLL loaded with the process"
#else
#define RESOLVE(name) (GetProcAddress(dll, #name))
#define SMOKE_LOAD_MODE "LoadLibraryA + GetProcAddress"
#endif

static int name_is_expected(const char *name)
{
    int i;
    for (i = 0; i < EXPECTED_EXPORT_COUNT; i++) {
        if (strcmp(name, g_expected_exports[i]) == 0)
            return 1;
    }
    return 0;
}

/* Walk the loaded image's headers and export directory. */
static void check_image_and_exports(HMODULE mod, int *out_export_summary_ok)
{
    BYTE *base = (BYTE *)mod;
    IMAGE_DOS_HEADER *dos;
    IMAGE_NT_HEADERS *nt;
    IMAGE_EXPORT_DIRECTORY *exp;
    DWORD *names;
    DWORD i;
    int missing = 0;
    int extra = 0;
    int decorated = 0;

    *out_export_summary_ok = 0;

    dos = (IMAGE_DOS_HEADER *)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
        check(0, "loaded module looks like a PE image");
        return;
    }
    nt = (IMAGE_NT_HEADERS *)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) {
        check(0, "loaded module has a PE header");
        return;
    }
    /* remember the range so a crash can be attributed to this module */
    g_dll_base = base;
    g_dll_size = nt->OptionalHeader.SizeOfImage;

    printf("  machine          : %s\n",
           nt->FileHeader.Machine == IMAGE_FILE_MACHINE_I386 ? "i386 (x86)" :
           nt->FileHeader.Machine == IMAGE_FILE_MACHINE_AMD64 ? "AMD64" : "other");
    printf("  subsystem version: %u.%u\n",
           (unsigned)nt->OptionalHeader.MajorSubsystemVersion,
           (unsigned)nt->OptionalHeader.MinorSubsystemVersion);
    printf("  subsystem        : %u (%s)\n",
           (unsigned)nt->OptionalHeader.Subsystem,
           nt->OptionalHeader.Subsystem == IMAGE_SUBSYSTEM_WINDOWS_GUI ? "GUI" :
           nt->OptionalHeader.Subsystem == IMAGE_SUBSYSTEM_WINDOWS_CUI ? "console" : "other");

#if defined(_M_IX86)
    /* 32-bit build of the test: it exists to validate the XP target, so
       the PE targeting is a hard requirement, not a remark. */
    check(nt->FileHeader.Machine == IMAGE_FILE_MACHINE_I386,
          "the DLL is a 32-bit x86 image (what XP needs)");
    check(nt->OptionalHeader.MajorSubsystemVersion == 5 &&
          nt->OptionalHeader.MinorSubsystemVersion == 1,
          "subsystem version is 5.01, i.e. built to run on XP");
#else
    /* The x64 configurations build this same program against the x64 DLL
       to keep the ABI checks covered there, and on a 64-bit image neither
       statement is true or wanted - say so instead of failing. */
    printf("  (64-bit build of this program: XP targeting is not asserted)\n");
    check(nt->FileHeader.Machine == IMAGE_FILE_MACHINE_AMD64,
          "the DLL matches this 64-bit test program");
#endif

    exp = (IMAGE_EXPORT_DIRECTORY *)(base +
            nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].VirtualAddress);
    if (nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].VirtualAddress == 0) {
        check(0, "the DLL has an export directory");
        return;
    }

    printf("  export names     : %lu\n", (unsigned long)exp->NumberOfNames);
    check(exp->NumberOfNames == (unsigned long)EXPECTED_EXPORT_COUNT,
          "the export directory holds exactly 14 names");

    names = (DWORD *)(base + exp->AddressOfNames);

    for (i = 0; i < exp->NumberOfNames; i++) {
        const char *name = (const char *)(base + names[i]);
        if (!name_is_expected(name)) {
            extra++;
            printf("  unexpected export: %s\n", name);
        }
        if (strchr(name, '@') != NULL)
            decorated++;
    }
    if (decorated) {
        printf("  decorated exports: %d (e.g. __stdcall name@N)\n", decorated);
    }
    check(decorated == 0, "no export carries a __stdcall decoration (@N)");

    /* build the name list once more, this time looking for the expected
       entries instead of unexpected ones */
    for (i = 0; i < EXPECTED_EXPORT_COUNT; i++) {
        int found = 0;
        DWORD j;
        for (j = 0; j < exp->NumberOfNames; j++) {
            const char *name = (const char *)(base + names[j]);
            if (strcmp(name, g_expected_exports[i]) == 0) {
                found = 1;
                break;
            }
        }
        if (!found) {
            missing++;
            printf("  missing export: %s\n", g_expected_exports[i]);
        }
    }
    check(missing == 0 && extra == 0,
          "the export set matches driver/pdflibdrv.def exactly");

    *out_export_summary_ok = (missing == 0 && extra == 0 && decorated == 0);
}

/* ------------------------------------------------------------------ */
/* a tiny x86-specific step: make sure the DLL really is 32-bit       */
/* ------------------------------------------------------------------ */

static int load_the_dll(const char *exe_dir, const char *arg,
                        char *out_path, size_t out_len, HMODULE *out_mod)
{
    char candidate[MAX_PATH];
    HMODULE mod;

#ifdef PDFLIBDRV_SMOKE_LINK_DIRECT
    /* The import table already pulled it in before main() ran; loading it
       again here would defeat the point of this build. */
    (void)exe_dir;
    (void)arg;
    strcpy(candidate, "pdflibdrv.dll");
    mod = GetModuleHandleA(candidate);
    if (!mod) {
        printf("  GetModuleHandleA(\"%s\") failed, GetLastError=%lu\n",
               candidate, (unsigned long)GetLastError());
        return 0;
    }
    printf("  already loaded by the loader before main()\n");
    strncpy(out_path, candidate, out_len - 1);
    out_path[out_len - 1] = 0;
    *out_mod = mod;
    return 1;
#else
    candidate[0] = 0;
    if (arg && arg[0]) {
        size_t n = strlen(arg);
        if (n > 4 && _stricmp(arg + (n - 4), ".dll") == 0) {
            strcpy(candidate, arg);                 /* a .dll path */
        }
        else {
            strcpy(candidate, arg);                 /* a directory */
            if (candidate[strlen(candidate) - 1] != '\\')
                strcat(candidate, "\\");
            strcat(candidate, "pdflibdrv.dll");
        }
    }
    else {
        strcpy(candidate, exe_dir);
        if (candidate[strlen(candidate) - 1] != '\\')
            strcat(candidate, "\\");
        strcat(candidate, "pdflibdrv.dll");
    }

    mod = LoadLibraryA(candidate);
    if (!mod) {
        printf("  LoadLibraryA(\"%s\") failed, GetLastError=%lu\n",
               candidate, (unsigned long)GetLastError());
        mod = LoadLibraryA("pdflibdrv.dll");        /* PATH / cwd */
        if (mod)
            strcpy(candidate, "pdflibdrv.dll (from PATH)");
    }
    if (!mod)
        return 0;

    strncpy(out_path, candidate, out_len - 1);
    out_path[out_len - 1] = 0;
    *out_mod = mod;
    return 1;
#endif
}

int main(int argc, char **argv)
{
    char exe_path[MAX_PATH];
    char exe_dir[MAX_PATH];
    char dll_path[MAX_PATH];
    char *slash;
    const char *arg = NULL;
    HMODULE dll = NULL;
    int export_ok = 0;

    /* the ABI functions, resolved by name - exactly like a print filter */
    void (PDCALL *p_get_abi)(unsigned int *, unsigned int *) = NULL;
    const char *(PDCALL *p_version_string)(void) = NULL;
    pd_status (PDCALL *p_startup)(void) = NULL;
    void (PDCALL *p_shutdown)(void) = NULL;
    pd_status (PDCALL *p_job_create)(const char *, pd_write_fn, void *, pd_doc *) = NULL;
    pd_status (PDCALL *p_job_finish)(pd_doc) = NULL;
    pd_status (PDCALL *p_job_abort)(pd_doc) = NULL;
    const char *(PDCALL *p_job_last_error)(pd_doc) = NULL;
    pd_status (PDCALL *p_page_add)(pd_doc, double, double, pd_page *) = NULL;
    pd_status (PDCALL *p_page_rotation)(pd_page, int) = NULL;
    pd_status (PDCALL *p_draw_text)(pd_page, unsigned int, const char *, double, double, double) = NULL;
    pd_status (PDCALL *p_draw_rect)(pd_page, double, double, double, double, unsigned int) = NULL;
    pd_status (PDCALL *p_draw_line)(pd_page, double, double, double, double) = NULL;

    unsigned int abi_major = 0, abi_minor = 0;
    OSVERSIONINFOA osv;
    pd_status st;
    pd_doc job = NULL;
    pd_page page = NULL;
    sink_t sink;
    FILE *fp;
    long size;
    char head[8];
    char tail[16];
    unsigned long i;
    int crash_test = 0;
    char cwd[MAX_PATH];

    i = 0; /* used by the sink-failure loop below */

    /* Unbuffered: if this process dies mid-check, everything printed so
       far has already left the building, and the report is the evidence. */
    setvbuf(stdout, NULL, _IONBF, 0);
    SetUnhandledExceptionFilter(crash_report);
    record_exe_range();

    if (argc > 1 && strcmp(argv[1], "--crash-test") == 0) {
        crash_test = 1;
        arg = argc > 2 ? argv[2] : NULL;
    }
    else if (argc > 1) {
        arg = argv[1];
    }

    printf("pdflibdrv XP smoke test\n");
    printf("=======================\n");
    printf("load mode     : %s\n\n", SMOKE_LOAD_MODE);

    cwd[0] = 0;
    GetCurrentDirectoryA(sizeof(cwd), cwd);
    printf("working dir   : %s\n", cwd);

    /* --- where am I running? --------------------------------------- */
    query_os_version(&osv);
    printf("OS            : %lu.%lu (build %lu) %s\n",
           (unsigned long)osv.dwMajorVersion,
           (unsigned long)osv.dwMinorVersion,
           (unsigned long)osv.dwBuildNumber,
           osv.szCSDVersion);
    if (osv.dwMajorVersion == 5 && osv.dwMinorVersion == 1)
        printf("                -> this is Windows XP (the real target)\n");
    else
        printf("                -> NOT XP; a pass here is weaker evidence\n");
    printf("\n");

    if (crash_test) {
        volatile unsigned char *nowhere = (volatile unsigned char *)0;
        printf("--crash-test: faulting on purpose, so this report can be\n");
        printf("   compared with the one a real crash produces.\n");
        *nowhere = 1;
        printf("the null write did not fault - the handler is not working\n");
    }

    GetModuleFileNameA(NULL, exe_path, sizeof(exe_path));
    strcpy(exe_dir, exe_path);
    slash = strrchr(exe_dir, '\\');
    if (slash)
        *slash = 0;

    printf("checks\n");
    printf("------\n");

    /* --- 1. load it ------------------------------------------------- */
    if (!load_the_dll(exe_dir, arg, dll_path, sizeof(dll_path), &dll)) {
        check(0, "pdflibdrv.dll is reachable");
        printf("\nPlace pdflibdrv.dll next to this exe, or pass its path.\n");
        printf("RESULT: %d check(s), %d failure(s)\n", g_checks, g_failures);
        return 1;
    }
    check(1, "pdflibdrv.dll is reachable (" SMOKE_LOAD_MODE ")");
    printf("  path           : %s\n", dll_path);
    printf("  module base    : %p\n", (void *)dll);

    /* --- 2. PE headers + export table ------------------------------- */
    check_image_and_exports(dll, &export_ok);

    /* --- 3. the 14 names resolve by name --------------------------- */
    p_get_abi = (void (PDCALL *)(unsigned int *, unsigned int *))
        (void *)RESOLVE(PdflibDrvGetAbiVersion);
    p_version_string = (const char *(PDCALL *)(void))
        (void *)RESOLVE(PdflibDrvVersionString);
    g_status_string = (const char *(PDCALL *)(pd_status))
        (void *)RESOLVE(PdflibDrvStatusString);
    p_startup = (pd_status (PDCALL *)(void))
        (void *)RESOLVE(PdflibDrvStartup);
    p_shutdown = (void (PDCALL *)(void))
        (void *)RESOLVE(PdflibDrvShutdown);
    p_job_create = (pd_status (PDCALL *)(const char *, pd_write_fn, void *, pd_doc *))
        (void *)RESOLVE(PdflibDrvJobCreate);
    p_job_finish = (pd_status (PDCALL *)(pd_doc))
        (void *)RESOLVE(PdflibDrvJobFinish);
    p_job_abort = (pd_status (PDCALL *)(pd_doc))
        (void *)RESOLVE(PdflibDrvJobAbort);
    p_job_last_error = (const char *(PDCALL *)(pd_doc))
        (void *)RESOLVE(PdflibDrvJobLastError);
    p_page_add = (pd_status (PDCALL *)(pd_doc, double, double, pd_page *))
        (void *)RESOLVE(PdflibDrvPageAdd);
    p_page_rotation = (pd_status (PDCALL *)(pd_page, int))
        (void *)RESOLVE(PdflibDrvPageSetRotation);
    p_draw_text = (pd_status (PDCALL *)(pd_page, unsigned int, const char *, double, double, double))
        (void *)RESOLVE(PdflibDrvPageDrawText);
    p_draw_rect = (pd_status (PDCALL *)(pd_page, double, double, double, double, unsigned int))
        (void *)RESOLVE(PdflibDrvPageDrawRect);
    p_draw_line = (pd_status (PDCALL *)(pd_page, double, double, double, double))
        (void *)RESOLVE(PdflibDrvPageDrawLine);

    check(p_get_abi && p_version_string && g_status_string && p_startup &&
          p_shutdown && p_job_create && p_job_finish && p_job_abort &&
          p_job_last_error && p_page_add && p_page_rotation && p_draw_text &&
          p_draw_rect && p_draw_line,
          "all 14 entry points are callable (" SMOKE_LOAD_MODE ")");
#ifdef PDFLIBDRV_SMOKE_LINK_DIRECT
    {
        int missing = 0;
        int k;
        for (k = 0; k < EXPECTED_EXPORT_COUNT; k++) {
            if (!GetProcAddress(dll, g_expected_exports[k]))
                missing++;
        }
        check(missing == 0,
              "all 14 names are still exported under their literal name");
    }
#endif

    if (!p_startup || !p_job_create || !p_job_finish) {
        printf("\nRESULT: %d check(s), %d failure(s) - cannot continue\n",
               g_checks, g_failures);
#ifndef PDFLIBDRV_SMOKE_LINK_DIRECT
        FreeLibrary(dll);
#endif
        return 1;
    }

    /* --- 4. the ABI it was built against --------------------------- */
    if (p_get_abi) {
        p_get_abi(&abi_major, &abi_minor);
        printf("  DLL reports ABI %u.%u, pdflib \"%s\"\n",
               abi_major, abi_minor,
               p_version_string ? p_version_string() : "?");
        check(abi_major == PDFLIBDRV_ABI_MAJOR && abi_minor == PDFLIBDRV_ABI_MINOR,
              "the DLL's ABI version matches the header this test was built from");
    }

    /* --- 5. startup is idempotent ---------------------------------- */
    check_status(p_startup(), PD_OK, "PdflibDrvStartup");
    check_status(p_startup(), PD_OK, "PdflibDrvStartup again (idempotent)");

    /* --- 6. a healthy streamed job --------------------------------- */
    memset(&sink, 0, sizeof(sink));
    fp = fopen("xp_smoke_out.pdf", "wb");
    check(fp != NULL, "opened xp_smoke_out.pdf for the sink");
    sink.fp = fp;

    st = p_job_create("pdflibdrv on Windows XP", sink_write, &sink, &job);
    check_status(st, PD_OK, "PdflibDrvJobCreate");
    check(job != NULL, "the job handle is not NULL");

    if (st == PD_OK && job) {
        trace_call("PdflibDrvPageAdd");
        st = p_page_add(job, 595.0, 842.0, &page);
        check_status(st, PD_OK, "PdflibDrvPageAdd (A4)");
        check(page != NULL, "the page handle is not NULL");

        trace_call("PdflibDrvPageDrawText");
        st = p_draw_text(page, PD_FONT_HELVETICA,
                         "Hello from Windows XP", 72.0, 720.0, 18.0);
        check_status(st, PD_OK, "PdflibDrvPageDrawText");

        trace_call("PdflibDrvPageDrawRect");
        st = p_draw_rect(page, 72.0, 600.0, 200.0, 60.0, PD_PAINT_FILL_STROKE);
        check_status(st, PD_OK, "PdflibDrvPageDrawRect");

        trace_call("PdflibDrvPageDrawLine");
        st = p_draw_line(page, 72.0, 560.0, 400.0, 560.0);
        check_status(st, PD_OK, "PdflibDrvPageDrawLine");

        trace_call("PdflibDrvPageSetRotation");
        st = p_page_rotation(page, 90);
        check_status(st, PD_OK, "PdflibDrvPageSetRotation");

        trace_call("PdflibDrvJobFinish");
        st = p_job_finish(job);
        check_status(st, PD_OK, "PdflibDrvJobFinish");
        job = NULL;
    }

    if (fp) {
        fclose(fp);
        fp = NULL;
    }
    printf("  sink           : %lu call(s), %lu byte(s), %lu zero-length\n",
           sink.calls, sink.bytes, sink.zero_len_calls);
    check(sink.bytes > 1000, "the sink received a plausible PDF");
    /* A len == 0 call is legal; that the job finished with PD_OK is what
       proves any such call was accepted, so this is reported, not asserted. */
    printf("  zero-length calls: %lu (accepted)\n", sink.zero_len_calls);

    if (sink.bytes > 0) {
        size = 0;
        fp = fopen("xp_smoke_out.pdf", "rb");
        if (fp) {
            fseek(fp, 0, SEEK_END);
            size = ftell(fp);
            fseek(fp, 0, SEEK_SET);
            memset(head, 0, sizeof(head));
            fread(head, 1, 5, fp);
            check(memcmp(head, "%PDF-", 5) == 0, "the file on disk starts with %PDF-");
            fseek(fp, -6, SEEK_END);
            memset(tail, 0, sizeof(tail));
            fread(tail, 1, 6, fp);
            check(memcmp(tail, "%%EOF\n", 6) == 0 || memcmp(tail, "%%EOF\r", 6) == 0,
                  "the file on disk ends with %%EOF");
            check(size == (long)sink.bytes,
                  "the file on disk is exactly the byte count the sink delivered");
            fclose(fp);
        }
        else {
            check(0, "re-opened xp_smoke_out.pdf for verification");
        }
    }

    /* --- 7. a failing sink poisons the job and stops delivery ------ */
    memset(&sink, 0, sizeof(sink));
    sink.fail_on_call = 3;
    job = NULL;
    st = p_job_create(NULL, sink_write, &sink, &job);
    if (st == PD_OK && job) {
        pd_page p2 = NULL;
        p_page_add(job, 595.0, 842.0, &p2);
        p_draw_text(p2, PD_FONT_TIMES_ROMAN, "torn", 72.0, 700.0, 12.0);
        for (i = 0; i < 5; i++) {
            p_draw_line(p2, 72.0, 100.0 + (double)i, 400.0, 100.0 + (double)i);
        }
        st = p_job_finish(job);
        check_status(st, PD_ERR_SINK, "JobFinish reports PD_ERR_SINK after the sink failed");
        check(sink.failed, "the sink really did fail");
        check(sink.bytes_after_failure == 0,
              "the DLL stopped delivering bytes after the failure");
        job = NULL;
    }
    else {
        check(0, "created the sink-failure job");
    }

    /* --- 8. abort discards the job --------------------------------- */
    memset(&sink, 0, sizeof(sink));
    job = NULL;
    st = p_job_create(NULL, sink_write, &sink, &job);
    if (st == PD_OK && job) {
        pd_page p3 = NULL;
        p_page_add(job, 595.0, 842.0, &p3);
        p_draw_text(p3, PD_FONT_COURIER, "discarded", 72.0, 700.0, 12.0);
        check_status(p_job_abort(job), PD_OK, "PdflibDrvJobAbort");
        check_status(p_job_finish(job), PD_ERR_INVALID_HANDLE,
                     "JobFinish on the aborted handle is rejected");
        job = NULL;
    }
    else {
        check(0, "created the abort job");
    }

    /* --- 9. forged and NULL handles are errors, not crashes --------- */
    check_status(p_job_finish(NULL), PD_ERR_INVALID_HANDLE,
                 "JobFinish(NULL) returns PD_ERR_INVALID_HANDLE");
    check_status(p_job_finish((pd_doc)(size_t)0xDEADBEEF), PD_ERR_INVALID_HANDLE,
                 "JobFinish(forged handle) returns PD_ERR_INVALID_HANDLE");
    check_status(p_job_abort((pd_doc)(size_t)0x1234), PD_ERR_INVALID_HANDLE,
                 "JobAbort(forged handle) returns PD_ERR_INVALID_HANDLE");
    /* reaching this line at all is the evidence: a dereferenced fake
       handle would have taken the process down before it printed. */
    check(query_os_version(&osv) != 0,
          "the process survived the forged handles (no access violation)");

    /* --- 10. shutdown releases unfinished jobs --------------------- */
    memset(&sink, 0, sizeof(sink));
    job = NULL;
    if (p_job_create(NULL, sink_write, &sink, &job) == PD_OK && job) {
        pd_page p4 = NULL;
        p_page_add(job, 595.0, 842.0, &p4);
        p_shutdown();
        check_status(p_job_finish(job), PD_ERR_INVALID_HANDLE,
                     "Shutdown released the unfinished job");
        check_status(p_startup(), PD_OK, "Startup works again after Shutdown");
        job = NULL;
        {
            pd_doc again = NULL;
            check_status(p_job_create(NULL, sink_write, &sink, &again), PD_OK,
                         "a new job can be created after Shutdown/Startup");
            if (again)
                p_job_abort(again);
        }
    }
    else {
        check(0, "created the shutdown job");
    }

    p_shutdown();
#ifndef PDFLIBDRV_SMOKE_LINK_DIRECT
    FreeLibrary(dll);
#endif

    printf("\nRESULT: %d check(s), %d failure(s)\n", g_checks, g_failures);
    if (g_failures == 0) {
        printf("pdflibdrv works on this machine.\n");
        return 0;
    }
    printf("pdflibdrv FAILED %d check(s) here.\n", g_failures);
    return 1;
}
