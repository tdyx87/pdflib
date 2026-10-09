# check_exports.cmake - post-build export-table guard for pdflibdrv.dll.
#
# Invoked with -DDUMPBIN=... -DDLL=... -DDEFFILE=...
# Fails the build if the DLL's export table is not exactly the names
# listed in the .def (catches LNK4070 name mismatches and accidental
# __declspec-driven name decoration).

if(NOT EXISTS "${DLL}")
    message(FATAL_ERROR "check_exports: DLL not found: ${DLL}")
endif()
if(NOT EXISTS "${DEFFILE}")
    message(FATAL_ERROR "check_exports: .def not found: ${DEFFILE}")
endif()

# ---- expected names from the .def -----------------------------------
file(STRINGS "${DEFFILE}" _def_lines)
set(_expected "")
set(_in_exports FALSE)
foreach(_line IN LISTS _def_lines)
    string(REGEX REPLACE ";.*$" "" _line "${_line}")   # strip comments
    string(STRIP "${_line}" _line)
    if(_line STREQUAL "")
        continue()
    endif()
    if(_line MATCHES "^EXPORTS$")
        set(_in_exports TRUE)
        continue()
    endif()
    if(_in_exports)
        # take the exported symbol only (ignore aliases / ordinals)
        string(REGEX MATCH "^[A-Za-z_][A-Za-z0-9_]*" _sym "${_line}")
        if(_sym)
            list(APPEND _expected "${_sym}")
        endif()
    endif()
endforeach()

# ---- actual names from dumpbin --------------------------------------
execute_process(
    COMMAND "${DUMPBIN}" /nologo /exports "${DLL}"
    OUTPUT_VARIABLE _dump
    ERROR_VARIABLE _dump_err
    RESULT_VARIABLE _rc)
if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "check_exports: dumpbin failed (${_rc}): ${_dump_err}")
endif()

string(REPLACE "\r" "" _dump "${_dump}")
string(REPLACE "\n" ";" _dump_lines "${_dump}")
set(_actual "")
foreach(_line IN LISTS _dump_lines)
    # "     1    0 00001234 PdflibDrvStartup"
    if(_line MATCHES "^ *[0-9]+ +[0-9A-Fa-f]+ +[0-9A-Fa-f]+ +([^ ]+)$")
        list(APPEND _actual "${CMAKE_MATCH_1}")
    endif()
endforeach()

list(SORT _expected)
list(REMOVE_DUPLICATES _expected)
list(SORT _actual)
list(REMOVE_DUPLICATES _actual)

if(NOT _actual)
    message(FATAL_ERROR
        "check_exports: no exports found in ${DLL}. "
        "A .def whose LIBRARY name differs from the DLL output name "
        "produces exactly this (link.exe only warns: LNK4070).")
endif()

# list(FIND) rather than IN_LIST: the latter needs CMP0057 and this
# script is run with `cmake -P`, which does not inherit the project's
# policy settings.
set(_missing "")
foreach(_n IN LISTS _expected)
    list(FIND _actual "${_n}" _idx)
    if(_idx EQUAL -1)
        list(APPEND _missing "${_n}")
    endif()
endforeach()
set(_extra "")
foreach(_n IN LISTS _actual)
    list(FIND _expected "${_n}" _idx)
    if(_idx EQUAL -1)
        list(APPEND _extra "${_n}")
    endif()
endforeach()

if(_missing OR _extra)
    message(FATAL_ERROR
        "check_exports: export table does not match pdflibdrv.def.\n"
        "  missing: ${_missing}\n"
        "  extra:   ${_extra}\n"
        "  actual:  ${_actual}")
endif()

list(LENGTH _actual _count)
message(STATUS "check_exports: ${_count} exported name(s) verified against pdflibdrv.def")

# ---- optional: no dynamic-CRT imports (i.e. really /MT) -------------
#
# A /MT DLL must not import the VC++ runtime DLLs. If it still does,
# the driver package would need the VC++ redistributable present in the
# print isolation host, which is not something to discover at install
# time on a customer machine.
if(REQUIRE_STATIC_CRT)
    execute_process(
        COMMAND "${DUMPBIN}" /nologo /dependents "${DLL}"
        OUTPUT_VARIABLE _deps
        ERROR_VARIABLE _deps_err
        RESULT_VARIABLE _deps_rc)
    if(NOT _deps_rc EQUAL 0)
        message(FATAL_ERROR
            "check_exports: dumpbin /dependents failed (${_deps_rc}): ${_deps_err}")
    endif()

    set(_bad "")
    foreach(_pat "VCRUNTIME" "MSVCP" "MSVCR" "UCRTBASE" "api-ms-win-crt")
        if(_deps MATCHES "${_pat}")
            list(APPEND _bad "${_pat}")
        endif()
    endforeach()

    if(_bad)
        message(FATAL_ERROR
            "check_exports: ${DLL} still imports the dynamic CRT "
            "(${_bad}) - it is not /MT. Rebuild pdflib and every backend "
            "with compiler.runtime=static and reconfigure.")
    endif()
    message(STATUS
        "check_exports: no dynamic-CRT imports; static runtime verified")
endif()
