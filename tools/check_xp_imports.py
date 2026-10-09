"""Audit a DLL's import table for Windows XP compatibility.

    python tools/check_xp_imports.py <dumpbin> <dll>

It reports two things:

1. every dependent DLL. An XP machine has no ``api-ms-win-crt-*``, no
   ``ucrtbase.dll`` and no VC++ redistributable for VS2015+ builds, so
   any of those means the DLL cannot load on XP at all.
2. every imported *function* whose name is on a curated list of APIs
   introduced after Windows XP.

The denylist is a heuristic, not a proof: it cannot catch an API that
exists on XP but behaves differently there. What it does catch is the
realistic failure mode - the VS2015+ CRT or STL quietly binding to a
Vista+ API because some object in the link was not compiled with the XP
toolset (``_USING_V110_SDK71_``) after all.
"""
import collections
import re
import subprocess
import sys

# DLLs that simply do not exist on a Windows XP machine.
BAD_DEPENDENCIES = (
    "api-ms-win-crt",
    "ucrtbase",
    "msvcp",
    "msvcr1",
    "vcruntime",
    "concrt",
    "vccorlib",
)

# Functions introduced after Windows XP, grouped by where they live.
POST_XP = {
    # --- thread / process / TLS -------------------------------------------
    "GetCurrentProcessorNumber", "GetCurrentProcessorNumberEx",
    "GetLogicalProcessorInformationEx",
    "GetActiveProcessorCount", "GetActiveProcessorGroupCount",
    "GetThreadId", "GetProcessIdOfThread",
    "SetThreadStackGuarantee", "GetCurrentThreadStackLimits",
    "QueryThreadCycleTime", "QueryProcessCycleTime",
    "SetThreadDescription", "SetThreadErrorMode", "GetThreadErrorMode",
    # --- fibre-local storage ------------------------------------------------
    "FlsAlloc", "FlsFree", "FlsGetValue", "FlsSetValue",
    # --- one-time init / locks / condition variables ------------------------
    "InitOnceExecuteOnce", "InitOnceBeginInitialize", "InitOnceComplete",
    "InitializeCriticalSectionEx", "InitializeSRWLock",
    "AcquireSRWLockExclusive", "ReleaseSRWLockExclusive",
    "AcquireSRWLockShared", "ReleaseSRWLockShared",
    "TryAcquireSRWLockExclusive", "TryAcquireSRWLockShared",
    "SleepConditionVariableCS", "SleepConditionVariableSRW",
    "WakeConditionVariable", "WakeAllConditionVariable",
    "WaitOnAddress", "WakeByAddressAll", "WakeByAddressSingle",
    "CreateSemaphoreExW", "CreateEventExW", "CreateMutexExW",
    # --- thread pools -------------------------------------------------------
    "CreateThreadpoolWork", "CloseThreadpoolWork", "SubmitThreadpoolWork",
    "WaitForThreadpoolWorkCallbacks", "CreateThreadpoolWait",
    "SetThreadpoolWait", "CloseThreadpoolWait", "SetThreadpoolTimer",
    "TrySubmitThreadpoolCallback",
    # --- files / handles ----------------------------------------------------
    "GetFileInformationByHandleEx", "SetFileInformationByHandle",
    "GetFinalPathNameByHandleW", "GetVolumeInformationByHandleW",
    "OpenFileById", "CancelIoEx", "CancelSynchronousIo",
    "SetFileCompletionNotificationModes", "GetOverlappedResultEx",
    "CreateFile2", "GetFileVersionInfoExW", "GetFileVersionInfoSizeExW",
    # --- time / locale ------------------------------------------------------
    "GetSystemTimePreciseAsFileTime", "GetTickCount64",
    "GetLocaleInfoEx", "GetUserDefaultLocaleName",
    "GetSystemDefaultLocaleName", "EnumSystemLocalesEx",
    "CompareStringEx", "CompareStringOrdinal", "LCMapStringEx",
    "GetDateFormatEx", "GetTimeFormatEx", "GetNumberFormatEx",
    "FindNLSStringEx", "IsValidLocaleName",
    # --- dll search / loader ------------------------------------------------
    "AddDllDirectory", "RemoveDllDirectory", "SetDefaultDllDirectories",
    "LoadPackagedLibrary", "GetPackageFamilyName",
    # --- error / crash ------------------------------------------------------
    "RaiseFailFastException", "SetProcessDEPPolicy", "GetProcessDEPPolicy",
    "GetProcessInformation",
    # --- registry -----------------------------------------------------------
    "RegDeleteTreeW", "RegCopyTreeW", "RegGetValueW", "RegSetKeyValueW",
    # --- advapi32 ETW surface ----------------------------------------------
    "EventRegister", "EventUnregister", "EventWrite", "EventWriteString",
    "EventSetInformation", "EventActivityIdControl",
    # --- numa ---------------------------------------------------------------
    "GetNumaHighestNodeNumber",
    # --- user32 / ws2_32 ----------------------------------------------------
    "SetProcessDPIAware", "GetPointerType",
    "SetThreadDpiAwarenessContext", "GetDpiForWindow",
    "WSAPoll",
}

NAME_RE = re.compile(r"^\s+([0-9A-Fa-f]+)\s+(\S+)\s*$")
DLL_RE = re.compile(r"^    (\S+\.dll)\s*$")


def run(dumpbin, args):
    proc = subprocess.run([dumpbin, "/nologo"] + args,
                          stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if proc.returncode != 0:
        raise SystemExit("dumpbin failed (%d): %s"
                         % (proc.returncode, proc.stdout.decode(errors="replace")))
    return proc.stdout.decode(errors="replace")


def parse_headers(text):
    """Return (machine, os_version, subsystem_version) from /headers."""
    machine = os_ver = subsys_ver = None
    for line in text.splitlines():
        m = re.search(r"\b([0-9A-Fa-f]+) machine \((\w+)\)", line)
        if m:
            machine = m.group(2)
            continue
        m = re.search(r"([0-9.]+) operating system version", line)
        if m:
            os_ver = m.group(1)
            continue
        m = re.search(r"([0-9.]+) subsystem version", line)
        if m:
            subsys_ver = m.group(1)
    return machine, os_ver, subsys_ver


def parse_imports(text):
    imports = collections.OrderedDict()
    current = None
    for line in text.splitlines():
        m = DLL_RE.match(line)
        if m:
            current = m.group(1)
            imports.setdefault(current, [])
            continue
        if current is None:
            continue
        m = NAME_RE.match(line)
        # "Ordinal 8" lines have a non-hex first token, so they never match.
        if m and m.group(2) != "Ordinal":
            imports[current].append(m.group(2))
    return imports


def main(argv):
    if len(argv) != 2:
        raise SystemExit(__doc__)
    dumpbin, dll = argv

    headers = run(dumpbin, ["/headers", dll])
    machine, os_ver, subsys_ver = parse_headers(headers)

    dump = run(dumpbin, ["/imports", dll])
    imports = parse_imports(dump)

    print("file   : %s" % dll)
    print("machine: %s" % machine)
    print("version: os %s / subsystem %s" % (os_ver, subsys_ver))
    print("checks : imports from %d module(s)" % len(imports))

    problems = 0

    # XP is 5.01; anything higher means the linker was not told to allow XP.
    if subsys_ver != "5.01":
        print("\nsubsystem version is %s, not 5.01 -> will not load on XP"
              % subsys_ver)
        problems += 1
    if machine and machine != "x86":
        print("\nmachine is %s; 64-bit Windows XP is a different target"
              % machine)

    print("\ndependencies")
    for name in imports:
        bad = any(name.lower().startswith(p) for p in BAD_DEPENDENCIES)
        print("  %-24s %s" % (name, "NOT ON XP" if bad else "ok"))
        if bad:
            problems += 1

    print("\npost-XP API usage")
    hits = []
    for name, funcs in imports.items():
        for func in funcs:
            if func in POST_XP:
                hits.append((name, func))
    if hits:
        for name, func in hits:
            print("  !! %s!%s" % (name, func))
        problems += len(hits)
    else:
        print("  none of the %d known post-XP functions are imported"
              % len(POST_XP))

    total = sum(len(v) for v in imports.values())
    print("\n%d imported function(s) total" % total)
    if problems:
        print("RESULT: %d problem(s) - this build is not XP-safe" % problems)
        return 1
    print("RESULT: no post-XP dependency or API found")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
